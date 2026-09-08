#include "Keyword/keyword_detector.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Voice/voice.h"
#include "bsp_api.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

#define KEYWORD_SLICE_SAMPLE_COUNT       (EI_CLASSIFIER_SLICE_SIZE)
#define KEYWORD_WARMUP_WINDOWS           (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)

/* ── Per-class confidence thresholds (permille) ───────────────────────
 * on/off 是短词且易混淆，需要更高阈值；time/weather 是多音节词，模型
 * 区分度高，可适当放宽。                                               */
#define KEYWORD_THRESHOLD_NOISE          (600U)  /* 60% */
#define KEYWORD_THRESHOLD_OFF            (700U)  /* 70% — 短词，易与 on 混淆 */
#define KEYWORD_THRESHOLD_ON             (700U)  /* 70% */
#define KEYWORD_THRESHOLD_TIME           (550U)  /* 55% — 多音节词，区分度高 */
#define KEYWORD_THRESHOLD_WEATHER        (550U)  /* 55% */

/* ── Per-class confirmation windows ──────────────────────────────────
 * on/off 需要 3 个连续窗口确认，降低单次误检导致的误触发。            */
#define KEYWORD_CONFIRM_WINDOWS_ON_OFF   (3U)
#define KEYWORD_CONFIRM_WINDOWS_OTHER    (2U)

/* ── Cooldown ─────────────────────────────────────────────────────── */
#define KEYWORD_COOLDOWN_WINDOWS         (4U)
#define KEYWORD_ANTI_FLIP_COOLDOWN       (8U)   /* on↔off 互锁冷却 */

/* ── Top-2 margin for on/off (permille) ──────────────────────────────
 * 当 top-1 是 on 或 off 时，需要领先第二名至少 150‰ (15%)，
 * 防止 on 和 off 分数接近时的随机翻转。                               */
#define KEYWORD_ON_OFF_MIN_MARGIN        (150U)

static float s_keyword_audio_slice[KEYWORD_SLICE_SAMPLE_COUNT];
static voice_sample_t s_keyword_voice_frame[128];
static uint32_t s_keyword_slice_count;
static uint32_t s_keyword_inference_count;
static uint32_t s_keyword_event_sequence;
static uint32_t s_keyword_consumed_event_sequence;
static uint32_t s_keyword_last_dropped_frames;
static uint8_t s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
static uint8_t s_keyword_candidate_windows;
static uint8_t s_keyword_cooldown_windows;
static uint8_t s_keyword_anti_flip_on_cooldown;   /* on 确认后禁止 off 的窗口数 */
static uint8_t s_keyword_anti_flip_off_cooldown;  /* off 确认后禁止 on 的窗口数 */
static bool s_keyword_initialized;
static bool s_keyword_latest_valid;
static keyword_detector_result_t s_keyword_latest;
static keyword_detector_result_t s_keyword_event;

static bool keyword_detector_recover_audio_overflow(void)
{
    uint32_t const dropped_frames = Voice_DroppedFrameCountGet();

    if (dropped_frames == s_keyword_last_dropped_frames)
    {
        return false;
    }

    printf("[KW] Audio queue overflow: dropped=%lu\r\n",
           (unsigned long) (dropped_frames - s_keyword_last_dropped_frames));
    s_keyword_last_dropped_frames = dropped_frames;

    /* A dropped frame breaks the continuous audio stream. Discard both the
     * classifier history and any partial 250 ms slice before resuming. */
    run_classifier_init();
    s_keyword_slice_count = 0U;
    s_keyword_inference_count = 0U;
    s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
    s_keyword_candidate_windows = 0U;
    s_keyword_cooldown_windows = 0U;
    s_keyword_anti_flip_on_cooldown = 0U;
    s_keyword_anti_flip_off_cooldown = 0U;
    return true;
}

static int keyword_audio_get_data(size_t offset, size_t length, float * output)
{
    if ((NULL == output) || ((offset + length) > KEYWORD_SLICE_SAMPLE_COUNT))
    {
        return EIDSP_PARAMETER_INVALID;
    }

    memcpy(output, &s_keyword_audio_slice[offset], length * sizeof(output[0]));
    return EIDSP_OK;
}

char const * keyword_detector_class_name(keyword_detector_class_t class_id)
{
    static char const * const labels[KEYWORD_DETECTOR_CLASS_COUNT] =
    {
        "noise",
        "off",
        "on",
        "time",
        "weather",
    };

    if ((uint32_t) class_id >= KEYWORD_DETECTOR_CLASS_COUNT)
    {
        return "unknown";
    }

    return labels[(uint32_t) class_id];
}

static void keyword_detector_update_event(keyword_detector_class_t class_id, uint16_t confidence_permille)
{
    uint16_t threshold;
    uint8_t  required_windows;

    /* 递减所有冷却计数器 */
    if (s_keyword_cooldown_windows > 0U)
    {
        s_keyword_cooldown_windows--;
    }
    if (s_keyword_anti_flip_on_cooldown > 0U)
    {
        s_keyword_anti_flip_on_cooldown--;
    }
    if (s_keyword_anti_flip_off_cooldown > 0U)
    {
        s_keyword_anti_flip_off_cooldown--;
    }

    /* ── Per-class threshold lookup ─────────────────────────────── */
    switch (class_id)
    {
        case KEYWORD_DETECTOR_CLASS_OFF:     threshold = KEYWORD_THRESHOLD_OFF;     break;
        case KEYWORD_DETECTOR_CLASS_ON:      threshold = KEYWORD_THRESHOLD_ON;      break;
        case KEYWORD_DETECTOR_CLASS_TIME:    threshold = KEYWORD_THRESHOLD_TIME;    break;
        case KEYWORD_DETECTOR_CLASS_WEATHER: threshold = KEYWORD_THRESHOLD_WEATHER; break;
        default:                             threshold = KEYWORD_THRESHOLD_NOISE;   break;
    }

    /* ── Anti-flip check for on↔off ─────────────────────────────── */
    if ((KEYWORD_DETECTOR_CLASS_OFF == class_id) && (s_keyword_anti_flip_on_cooldown > 0U))
    {
        /* 刚确认了 on，短时间内拒绝 off */
        s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
        s_keyword_candidate_windows = 0U;
        return;
    }
    if ((KEYWORD_DETECTOR_CLASS_ON == class_id) && (s_keyword_anti_flip_off_cooldown > 0U))
    {
        /* 刚确认了 off，短时间内拒绝 on */
        s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
        s_keyword_candidate_windows = 0U;
        return;
    }

    /* ── Below threshold or noise → reset candidate ─────────────── */
    if ((KEYWORD_DETECTOR_CLASS_NOISE == class_id) ||
        (confidence_permille <= threshold))
    {
        s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
        s_keyword_candidate_windows = 0U;
        return;
    }

    /* ── Consecutive window tracking ────────────────────────────── */
    if ((uint8_t) class_id == s_keyword_candidate_class)
    {
        if (s_keyword_candidate_windows < UINT8_MAX)
        {
            s_keyword_candidate_windows++;
        }
    }
    else
    {
        s_keyword_candidate_class = (uint8_t) class_id;
        s_keyword_candidate_windows = 1U;
    }

    /* ── Per-class required windows lookup ──────────────────────── */
    if ((KEYWORD_DETECTOR_CLASS_OFF == class_id) ||
        (KEYWORD_DETECTOR_CLASS_ON  == class_id))
    {
        required_windows = KEYWORD_CONFIRM_WINDOWS_ON_OFF;
    }
    else
    {
        required_windows = KEYWORD_CONFIRM_WINDOWS_OTHER;
    }

    /* ── Confirmation gate ──────────────────────────────────────── */
    if ((s_keyword_inference_count < KEYWORD_WARMUP_WINDOWS) ||
        (s_keyword_candidate_windows < required_windows) ||
        (s_keyword_cooldown_windows > 0U))
    {
        return;
    }

    /* ── Confirm ────────────────────────────────────────────────── */
    s_keyword_event_sequence++;
    s_keyword_event.class_id = class_id;
    s_keyword_event.confidence_permille = confidence_permille;
    s_keyword_event.inference_count = s_keyword_inference_count;
    s_keyword_event.event_sequence = s_keyword_event_sequence;
    s_keyword_cooldown_windows = KEYWORD_COOLDOWN_WINDOWS;
    s_keyword_candidate_windows = 0U;

    /* 设置防翻转冷却：on 和 off 互锁 */
    if (KEYWORD_DETECTOR_CLASS_ON == class_id)
    {
        s_keyword_anti_flip_on_cooldown = KEYWORD_ANTI_FLIP_COOLDOWN;
    }
    else if (KEYWORD_DETECTOR_CLASS_OFF == class_id)
    {
        s_keyword_anti_flip_off_cooldown = KEYWORD_ANTI_FLIP_COOLDOWN;
    }

    printf("[KW] CONFIRMED: %s, confidence=%u.%03u, inference=%lu\r\n",
           keyword_detector_class_name(class_id),
           (unsigned int) (confidence_permille / 1000U),
           (unsigned int) (confidence_permille % 1000U),
           (unsigned long) s_keyword_inference_count);
}

static bool keyword_detector_run_slice(void)
{
    ei_impulse_result_t result = {};
    signal_t signal;
    EI_IMPULSE_ERROR status;
    uint32_t best_index = 0U;
    uint32_t second_index = 0U;
    float best_score = -1.0F;
    float second_score = -1.0F;

    signal.total_length = KEYWORD_SLICE_SAMPLE_COUNT;
    signal.get_data = keyword_audio_get_data;

    status = run_classifier_continuous(&signal, &result, false, true);
    if (EI_IMPULSE_OK != status)
    {
        printf("[KW] Inference failed: %d\r\n", (int) status);
        return false;
    }

    s_keyword_inference_count++;

    /* ── Find top-2 classes ─────────────────────────────────────── */
    for (uint32_t i = 0U; i < KEYWORD_DETECTOR_CLASS_COUNT; i++)
    {
        float const score = result.classification[i].value;

        if (score > best_score)
        {
            second_score = best_score;
            second_index = best_index;
            best_score = score;
            best_index = i;
        }
        else if (score > second_score)
        {
            second_score = score;
            second_index = i;
        }
    }

    /* Clamp */
    if (best_score < 0.0F)   { best_score = 0.0F; }
    if (best_score > 1.0F)   { best_score = 1.0F; }
    if (second_score < 0.0F) { second_score = 0.0F; }

    uint16_t const best_permille   = (uint16_t) ((best_score * 1000.0F) + 0.5F);
    uint16_t const second_permille = (uint16_t) ((second_score * 1000.0F) + 0.5F);
    uint16_t const margin          = (uint16_t) (best_permille - second_permille);

    /* ── Top-2 margin check for on/off ─────────────────────────────
     * 当 top-1 是 on 或 off 时，要求领先第二名至少 15%。
     * 如果 on 和 off 分数接近，说明模型不确定，此时回退为 noise。    */
    if ((KEYWORD_DETECTOR_CLASS_ON  == (keyword_detector_class_t) best_index) ||
        (KEYWORD_DETECTOR_CLASS_OFF == (keyword_detector_class_t) best_index))
    {
        if (margin < KEYWORD_ON_OFF_MIN_MARGIN)
        {
            /* 分数太接近，不可靠 → 视为 noise */
            printf("[KW] Margin reject: top=%s(%.3f) 2nd=%s(%.3f) margin=%.3f\r\n",
                   keyword_detector_class_name((keyword_detector_class_t) best_index),
                   (double) best_score,
                   keyword_detector_class_name((keyword_detector_class_t) second_index),
                   (double) second_score,
                   (double) margin / 1000.0);
            best_index = (uint32_t) KEYWORD_DETECTOR_CLASS_NOISE;
            best_score = 1.0F - (float) margin / 1000.0F;  /* 用 margin 反推一个低置信度 */
        }
    }

    s_keyword_latest.class_id = (keyword_detector_class_t) best_index;
    s_keyword_latest.confidence_permille = (uint16_t) ((best_score * 1000.0F) + 0.5F);
    s_keyword_latest.inference_count = s_keyword_inference_count;
    s_keyword_latest.event_sequence = s_keyword_event_sequence;
    s_keyword_latest_valid = true;

    /* 每 50 次推理输出 top-2 信息，便于诊断 on/off 混淆 */
    if (0U == (s_keyword_inference_count % 50U))
    {
        printf("[KW] #%lu top1=%s(%.3f) top2=%s(%.3f) margin=%.3f\r\n",
               (unsigned long) s_keyword_inference_count,
               keyword_detector_class_name((keyword_detector_class_t) best_index),
               (double) best_score,
               keyword_detector_class_name((keyword_detector_class_t) second_index),
               (double) second_score,
               (double) margin / 1000.0);
    }

    keyword_detector_update_event(s_keyword_latest.class_id, s_keyword_latest.confidence_permille);
    return true;
}

bool keyword_detector_init(void)
{
    if (s_keyword_initialized)
    {
        return true;
    }

    if ((KEYWORD_DETECTOR_SAMPLE_RATE_HZ != EI_CLASSIFIER_FREQUENCY) ||
        (KEYWORD_DETECTOR_CLASS_COUNT != EI_CLASSIFIER_LABEL_COUNT))
    {
        printf("[KW] Model metadata mismatch: rate=%lu/%lu, labels=%u/%lu\r\n",
               (unsigned long) KEYWORD_DETECTOR_SAMPLE_RATE_HZ,
               (unsigned long) EI_CLASSIFIER_FREQUENCY,
               (unsigned int) KEYWORD_DETECTOR_CLASS_COUNT,
               (unsigned long) EI_CLASSIFIER_LABEL_COUNT);
        return false;
    }

    s_keyword_initialized = true;
    keyword_detector_reset();

    printf("[KW] Model ready: deploy=v%u, 16 kHz, labels=noise/off/on/time/weather, arena=%lu\r\n",
           (unsigned int) EI_CLASSIFIER_PROJECT_DEPLOY_VERSION,
           (unsigned long) EI_CLASSIFIER_TFLITE_LARGEST_ARENA_SIZE);
    return true;
}

void keyword_detector_deinit(void)
{
    if (!s_keyword_initialized)
    {
        return;
    }

    s_keyword_initialized = false;
    s_keyword_latest_valid = false;
    printf("[KW] Detector deinitialized.\r\n");
}

void keyword_detector_reset(void)
{
    if (!s_keyword_initialized)
    {
        return;
    }

    s_keyword_slice_count = 0U;
    s_keyword_inference_count = 0U;
    s_keyword_event_sequence = 0U;
    s_keyword_consumed_event_sequence = 0U;
    s_keyword_last_dropped_frames = Voice_DroppedFrameCountGet();
    s_keyword_candidate_class = KEYWORD_DETECTOR_CLASS_NOISE;
    s_keyword_candidate_windows = 0U;
    s_keyword_cooldown_windows = 0U;
    s_keyword_anti_flip_on_cooldown = 0U;
    s_keyword_anti_flip_off_cooldown = 0U;
    s_keyword_latest_valid = false;
    memset(&s_keyword_latest, 0, sizeof(s_keyword_latest));
    memset(&s_keyword_event, 0, sizeof(s_keyword_event));
    run_classifier_init();
}

bool keyword_detector_poll(void)
{
    bool inference_ran = false;

    if (!s_keyword_initialized)
    {
        return false;
    }

    (void) keyword_detector_recover_audio_overflow();

    while (Voice_FrameReady() && !inference_ran)
    {
        uint32_t const frame_count = Voice_FrameRead(s_keyword_voice_frame,
                                                     sizeof(s_keyword_voice_frame) /
                                                     sizeof(s_keyword_voice_frame[0]));
        for (uint32_t i = 0U; i < frame_count; i++)
        {
            s_keyword_audio_slice[s_keyword_slice_count++] =
                (float) s_keyword_voice_frame[i];

            if (KEYWORD_SLICE_SAMPLE_COUNT == s_keyword_slice_count)
            {
                s_keyword_slice_count = 0U;
                inference_ran = keyword_detector_run_slice();
            }
        }
    }

    /* Inference itself can take long enough for the ISR queue to overflow. */
    (void) keyword_detector_recover_audio_overflow();

    return inference_ran;
}

bool keyword_detector_get_latest(keyword_detector_result_t * result)
{
    if ((NULL == result) || !s_keyword_latest_valid)
    {
        return false;
    }

    *result = s_keyword_latest;
    return true;
}

bool keyword_detector_take_event(keyword_detector_result_t * result)
{
    if ((NULL == result) ||
        (0U == s_keyword_event_sequence) ||
        (s_keyword_consumed_event_sequence == s_keyword_event_sequence))
    {
        return false;
    }

    *result = s_keyword_event;
    s_keyword_consumed_event_sequence = s_keyword_event_sequence;
    return true;
}

EI_IMPULSE_ERROR ei_run_impulse_check_canceled()
{
    return EI_IMPULSE_OK;
}

EI_IMPULSE_ERROR ei_sleep(int32_t time_ms)
{
    if (time_ms > 0)
    {
        R_BSP_SoftwareDelay((uint32_t) time_ms, BSP_DELAY_UNITS_MILLISECONDS);
    }
    return EI_IMPULSE_OK;
}

uint64_t ei_read_timer_ms()
{
    return 0U;
}

uint64_t ei_read_timer_us()
{
    return 0U;
}

void ei_serial_set_baudrate(int baudrate)
{
    (void) baudrate;
}

void ei_putchar(char character)
{
    putchar((int) character);
}

char ei_getchar(void)
{
    return 0;
}

void ei_printf(char const * format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
}

void ei_printf_float(float value)
{
    printf("%f", (double) value);
}

void * ei_malloc(size_t size)
{
    return malloc(size);
}

void * ei_calloc(size_t item_count, size_t item_size)
{
    return calloc(item_count, item_size);
}

void ei_free(void * pointer)
{
    free(pointer);
}

void DebugLog(char const * message)
{
    ei_printf("%s", message);
}
