#include "app.h"

#include "Music_Rhythm/music_beat_tracker.h"
#include "Music_Rhythm/music_dance.h"
#include "Music_Rhythm/music_onset.h"
#include "Printf/printf.h"
#include "Screen/drv_gpt_timer.h"
#include "ServoLib/ServoDriver.h"
#include "Voice/voice.h"
#include "arm_math.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MUSIC_ONSET_SAMPLE_RATE_HZ              (16000U)
#define MUSIC_ONSET_FFT_SIZE                    (512U)
#define MUSIC_ONSET_FFT_HALF_SIZE               (MUSIC_ONSET_FFT_SIZE / 2U)
#define MUSIC_ONSET_MIC_FRAME_CAPACITY          (128U)
#define MUSIC_ONSET_BLOCK_DURATION_MS           \
    ((MUSIC_ONSET_FFT_SIZE * 1000U) / MUSIC_ONSET_SAMPLE_RATE_HZ)
#define MUSIC_BEAT_FRAME_RATE_HZ                 \
    (1000.0f / (float32_t) MUSIC_ONSET_BLOCK_DURATION_MS)

/*
 * Use a high sensitivity so weaker alternating beats are not
 * discarded. The separate calibration noise gate still rejects fan and room
 * noise before the onset detector sees the spectrum.
 */
#define MUSIC_ONSET_SENSITIVITY                 (82U)
#define MUSIC_ONSET_MINIMUM_INTERVAL_MS         (200U)

/* 160 FFT blocks x 32 ms is approximately 5.12 seconds of quiet calibration. */
#define MUSIC_ONSET_CALIBRATION_BLOCKS          (160U)
#define MUSIC_ONSET_CALIBRATION_REPORT_STEP     (40U)

/* Voice_FrameRead() in this project returns signed PCM16 samples. */
#define MUSIC_ONSET_SAMPLE_SCALE                (1.0f / 32768.0f)
#define MUSIC_ONSET_MAGNITUDE_SCALE             (4.0f / (float32_t) MUSIC_ONSET_FFT_SIZE)

/*
 * A signal must rise clearly above the mean and normal variation measured
 * during calibration. The relative margin handles steady fan noise, while the
 * deviation margin handles small irregular fluctuations around that noise.
 */
#define MUSIC_ONSET_NOISE_MEAN_MULTIPLIER       (1.50f)
#define MUSIC_ONSET_NOISE_DEVIATION_MULTIPLIER  (4.00f)
#define MUSIC_ONSET_MINIMUM_ACTIVE_BANDS        (2U)

/** Inclusive first and last FFT-bin indexes belonging to one analysis band. */
typedef struct st_music_onset_band_range
{
    uint16_t first_bin;
    uint16_t last_bin;
} music_onset_band_range_t;

/*
 * One FFT bin represents 31.25 Hz.  The ranges gradually become wider toward
 * high frequencies, producing 16 approximately logarithmic bands from
 * 31.25 Hz to 6 kHz.  Averaging each range prevents a wider band from winning
 * merely because it contains more FFT bins.
 */
static music_onset_band_range_t const s_music_onset_band_ranges[MUSIC_ONSET_BAND_COUNT] =
{
    {  1U,   2U}, /*   31.25 -   62.50 Hz */
    {  3U,   4U}, /*   93.75 -  125.00 Hz */
    {  5U,   6U}, /*  156.25 -  187.50 Hz */
    {  7U,   9U}, /*  218.75 -  281.25 Hz */
    { 10U,  13U}, /*  312.50 -  406.25 Hz */
    { 14U,  18U}, /*  437.50 -  562.50 Hz */
    { 19U,  24U}, /*  593.75 -  750.00 Hz */
    { 25U,  31U}, /*  781.25 -  968.75 Hz */
    { 32U,  40U}, /* 1000.00 - 1250.00 Hz */
    { 41U,  51U}, /* 1281.25 - 1593.75 Hz */
    { 52U,  64U}, /* 1625.00 - 2000.00 Hz */
    { 65U,  80U}, /* 2031.25 - 2500.00 Hz */
    { 81U, 101U}, /* 2531.25 - 3156.25 Hz */
    {102U, 127U}, /* 3187.50 - 3968.75 Hz */
    {128U, 159U}, /* 4000.00 - 4968.75 Hz */
    {160U, 192U}, /* 5000.00 - 6000.00 Hz */
};

static arm_rfft_fast_instance_f32 s_music_onset_fft_instance;
static voice_sample_t s_music_onset_mic_frame[MUSIC_ONSET_MIC_FRAME_CAPACITY];
static float32_t s_music_onset_fft_input[MUSIC_ONSET_FFT_SIZE];
static float32_t s_music_onset_fft_output[MUSIC_ONSET_FFT_SIZE];
static float32_t s_music_onset_fft_window[MUSIC_ONSET_FFT_SIZE];
static float32_t s_music_onset_magnitude[MUSIC_ONSET_FFT_HALF_SIZE];

static float32_t music_onset_band_average(uint32_t first_bin, uint32_t last_bin)
{
    float32_t sum = 0.0f;

    for (uint32_t bin = first_bin; bin <= last_bin; bin++)
    {
        sum += s_music_onset_magnitude[bin];
    }

    return sum / (float32_t) ((last_bin - first_bin) + 1U);
}

static float32_t music_onset_noise_gate_calculate(float32_t mean,
                                                  float32_t square_difference_sum,
                                                  uint32_t sample_count)
{
    float32_t standard_deviation = 0.0f;
    float32_t deviation_gate;
    float32_t relative_gate = mean * MUSIC_ONSET_NOISE_MEAN_MULTIPLIER;

    if (sample_count > 1U)
    {
        float32_t const variance =
            square_difference_sum / (float32_t) (sample_count - 1U);

        if (ARM_MATH_SUCCESS != arm_sqrt_f32(variance, &standard_deviation))
        {
            standard_deviation = 0.0f;
        }
    }

    deviation_gate = mean +
                     (standard_deviation * MUSIC_ONSET_NOISE_DEVIATION_MULTIPLIER);

    return (deviation_gate > relative_gate) ? deviation_gate : relative_gate;
}

static void music_onset_fft_analyze(float32_t bands[MUSIC_ONSET_BAND_COUNT])
{
    float32_t mean = 0.0f;

    /* Remove the microphone DC offset before applying the FFT window. */
    for (uint32_t i = 0U; i < MUSIC_ONSET_FFT_SIZE; i++)
    {
        mean += s_music_onset_fft_input[i];
    }
    mean /= (float32_t) MUSIC_ONSET_FFT_SIZE;

    /* The Hann window reduces energy leaking into neighboring FFT bins. */
    for (uint32_t i = 0U; i < MUSIC_ONSET_FFT_SIZE; i++)
    {
        s_music_onset_fft_input[i] =
            (s_music_onset_fft_input[i] - mean) * s_music_onset_fft_window[i];
    }

    arm_rfft_fast_f32(&s_music_onset_fft_instance,
                      s_music_onset_fft_input,
                      s_music_onset_fft_output,
                      0U);

    /* DC is unused; real/imaginary pairs for bins 1..255 start at index 2. */
    s_music_onset_magnitude[0] = 0.0f;
    arm_cmplx_mag_f32(&s_music_onset_fft_output[2],
                      &s_music_onset_magnitude[1],
                      MUSIC_ONSET_FFT_HALF_SIZE - 1U);

    for (uint32_t bin = 1U; bin < MUSIC_ONSET_FFT_HALF_SIZE; bin++)
    {
        s_music_onset_magnitude[bin] *= MUSIC_ONSET_MAGNITUDE_SCALE;
    }

    for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
    {
        bands[band] = music_onset_band_average(
            s_music_onset_band_ranges[band].first_bin,
            s_music_onset_band_ranges[band].last_bin);
    }
}

void app_music_onset_test(void)
{
    music_onset_detector_t detector;
    music_beat_tracker_t beat_tracker;
    float32_t noise_mean[MUSIC_ONSET_BAND_COUNT] = {0.0f};
    float32_t noise_square_difference_sum[MUSIC_ONSET_BAND_COUNT] = {0.0f};
    float32_t noise_gate[MUSIC_ONSET_BAND_COUNT] = {0.0f};
    float32_t total_noise_mean = 0.0f;
    float32_t total_noise_square_difference_sum = 0.0f;
    float32_t total_noise_gate = 0.0f;
    uint32_t fft_sample_count = 0U;
    uint32_t calibration_count = 0U;
    uint32_t audio_timestamp_ms = 0U;
    uint32_t last_frame_counter = 0U;
    uint32_t onset_count = 0U;
    uint32_t beat_count = 0U;
    uint32_t last_reported_bpm = 0U;
    uint32_t servo_noise_ignore_until_ms = 0U;
    bool last_frame_valid = false;
    bool warmup_message_printed = false;
    bool tempo_lock_reported = false;
    bool previous_prediction_active = false;
    fsp_err_t err;

    if (ARM_MATH_SUCCESS !=
        arm_rfft_fast_init_f32(&s_music_onset_fft_instance, MUSIC_ONSET_FFT_SIZE))
    {
        app_fatal_error("Music onset FFT initialization", FSP_ERR_INTERNAL);
    }

    arm_hanning_f32(s_music_onset_fft_window, MUSIC_ONSET_FFT_SIZE);
    music_onset_init(&detector,
                     MUSIC_ONSET_SENSITIVITY,
                     MUSIC_ONSET_MINIMUM_INTERVAL_MS);
    music_beat_tracker_init(&beat_tracker, MUSIC_BEAT_FRAME_RATE_HZ);

    /*
     * 拍内抬头使用真实墙钟调度，不能把可能因FIFO积压而滞后的音频时间轴
     * 当作墙钟。各测试模式在hal_entry.c中互斥，此处不会与主程序中由
     * app_lvgl_init()启动的同一个GPT实例发生重复初始化。
     */
    err = drv_gpt_timer_init();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Music dance timer initialization", err);
    }
    music_dance_init();

    Voice_ChannelSet(VOICE_CHANNEL_RIGHT);
    err = Voice_Start();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Music onset microphone start", err);
    }

    printf("Music onset test started: 16 kHz, 512-point FFT, 16 bands.\r\n");
    printf("Keep quiet for about 5 seconds while background noise is measured.\r\n");

    while (1)
    {
        uint32_t frame_counter;
        uint32_t frame_count;
        uint32_t pending_audio_guard_ms = 0U;
        bool pending_command_sent = false;

        /*
         * 每次麦克风循环都检查一次到期的抬头半程。GPT中断会持续提供真实
         * 毫秒时间，即使音频处理短暂积压，上下点头也不会在同一时刻挤到一起。
         */
        if (!music_dance_pending_update(drv_gpt_timer_get_ms(),
                                        &pending_command_sent,
                                        &pending_audio_guard_ms))
        {
            printf("Dance nod return command failed: error=%d\r\n",
                   Servo_GetLastError());
        }
        else if (pending_command_sent)
        {
            uint32_t const pending_guard_until_ms =
                audio_timestamp_ms + pending_audio_guard_ms;

            /*
             * 回抬只能延长现有的舵机噪声保护，不能覆盖并缩短身体动作尚未
             * 结束的保护时间，否则齿轮声可能被重新识别成音乐起音。
             */
            if ((int32_t) (pending_guard_until_ms -
                           servo_noise_ignore_until_ms) > 0)
            {
                servo_noise_ignore_until_ms = pending_guard_until_ms;
            }
        }

        if (!Voice_FrameReady())
        {
            /* Sleep until an interrupt occurs instead of using an empty busy loop. */
            __WFI();
            continue;
        }

        frame_counter = Voice_FrameCounterGet();
        frame_count = Voice_FrameRead(s_music_onset_mic_frame,
                                      MUSIC_ONSET_MIC_FRAME_CAPACITY);
        if (0U == frame_count)
        {
            continue;
        }

        if (last_frame_valid)
        {
            uint32_t const frame_delta = frame_counter - last_frame_counter;

            if (frame_delta > 1U)
            {
                /*
                 * Do not form an FFT or spectral difference from discontinuous
                 * microphone samples. The detector will warm up again safely.
                 */
                fft_sample_count = 0U;
                music_onset_reset(&detector);
                music_beat_tracker_reset(&beat_tracker);
                warmup_message_printed = false;
                tempo_lock_reported = false;
                previous_prediction_active = false;
                last_reported_bpm = 0U;
                {
                    uint32_t audio_guard_ms = 0U;

                    if (music_dance_return_center(&audio_guard_ms))
                    {
                        servo_noise_ignore_until_ms =
                            audio_timestamp_ms + audio_guard_ms;
                    }
                }
            }
        }

        last_frame_counter = frame_counter;
        last_frame_valid = true;

        for (uint32_t i = 0U; i < frame_count; i++)
        {
            s_music_onset_fft_input[fft_sample_count++] =
                (float32_t) s_music_onset_mic_frame[i] * MUSIC_ONSET_SAMPLE_SCALE;

            if (fft_sample_count >= MUSIC_ONSET_FFT_SIZE)
            {
                float32_t bands[MUSIC_ONSET_BAND_COUNT];

                music_onset_fft_analyze(bands);
                fft_sample_count = 0U;
                audio_timestamp_ms += MUSIC_ONSET_BLOCK_DURATION_MS;

                if (calibration_count < MUSIC_ONSET_CALIBRATION_BLOCKS)
                {
                    uint32_t const next_calibration_count = calibration_count + 1U;
                    float32_t frame_total = 0.0f;

                    /*
                     * Welford's algorithm calculates both mean and variance
                     * without storing all 160 calibration frames.
                     */
                    for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
                    {
                        float32_t const difference = bands[band] - noise_mean[band];

                        noise_mean[band] +=
                            difference / (float32_t) next_calibration_count;
                        noise_square_difference_sum[band] +=
                            difference * (bands[band] - noise_mean[band]);
                        frame_total += bands[band];
                    }

                    {
                        float32_t const total_difference = frame_total - total_noise_mean;

                        total_noise_mean +=
                            total_difference / (float32_t) next_calibration_count;
                        total_noise_square_difference_sum +=
                            total_difference * (frame_total - total_noise_mean);
                    }

                    calibration_count = next_calibration_count;
                    if ((0U == (calibration_count % MUSIC_ONSET_CALIBRATION_REPORT_STEP)) ||
                        (calibration_count == MUSIC_ONSET_CALIBRATION_BLOCKS))
                    {
                        printf("Onset calibration: %lu/%lu\r\n",
                               (unsigned long) calibration_count,
                               (unsigned long) MUSIC_ONSET_CALIBRATION_BLOCKS);
                    }

                    if (calibration_count == MUSIC_ONSET_CALIBRATION_BLOCKS)
                    {
                        for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
                        {
                            noise_gate[band] = music_onset_noise_gate_calculate(
                                noise_mean[band],
                                noise_square_difference_sum[band],
                                calibration_count);
                        }

                        total_noise_gate = music_onset_noise_gate_calculate(
                            total_noise_mean,
                            total_noise_square_difference_sum,
                            calibration_count);

                        music_onset_reset(&detector);
                        music_beat_tracker_reset(&beat_tracker);
                        printf("Calibration complete. Start playing music.\r\n");
                        printf("Tempo tracker needs about 4 to 5 seconds of repeated beats.\r\n");
                    }
                }
                else
                {
                    music_onset_result_t result;
                    music_beat_result_t beat_result;
                    float32_t frame_total = 0.0f;
                    uint32_t active_band_count = 0U;
                    bool const onset_detection_allowed =
                        ((int32_t) (audio_timestamp_ms -
                                    servo_noise_ignore_until_ms) >= 0);

                    /* First decide whether this frame is genuinely above room noise. */
                    for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
                    {
                        float32_t const raw_band = bands[band];

                        frame_total += raw_band;
                        if (raw_band > noise_gate[band])
                        {
                            bands[band] = raw_band - noise_gate[band];
                            active_band_count++;
                        }
                        else
                        {
                            bands[band] = 0.0f;
                        }
                    }

                    /*
                     * A tiny fluctuation in only one band is not music activity.
                     * Feed a zero spectrum so the detector can return cleanly to idle.
                     */
                    if ((frame_total <= total_noise_gate) ||
                        (active_band_count < MUSIC_ONSET_MINIMUM_ACTIVE_BANDS))
                    {
                        for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
                        {
                            bands[band] = 0.0f;
                        }
                    }

                    /*
                     * Motor-noise frames still update spectral history, but
                     * their delayed local peaks are marked as ineligible.
                     */
                    result = music_onset_update(&detector,
                                                bands,
                                                audio_timestamp_ms,
                                                onset_detection_allowed);
                    beat_result = music_beat_tracker_update(
                        &beat_tracker,
                        result.detected,
                        result.detected ? result.strength : 0.0f,
                        result.timestamp_ms,
                        audio_timestamp_ms);

                    if (!result.ready && !warmup_message_printed)
                    {
                        printf("Onset detector warming up for about 1 second.\r\n");
                        warmup_message_printed = true;
                    }

                    if (result.detected)
                    {
                        uint32_t const strength_percent =
                            (uint32_t) ((result.strength * 100.0f) + 0.5f);

                        onset_count++;
                        printf("ONSET #%lu: strength=%lu%%, interval=%lu ms\r\n",
                               (unsigned long) onset_count,
                               (unsigned long) strength_percent,
                               (unsigned long) result.interval_ms);
                    }

                    if (beat_result.tempo_locked)
                    {
                        uint32_t const bpm =
                            (uint32_t) (beat_result.bpm + 0.5f);
                        uint32_t const confidence_percent =
                            (uint32_t) ((beat_result.confidence * 100.0f) + 0.5f);
                        uint32_t const bpm_difference = (bpm >= last_reported_bpm) ?
                                                        (bpm - last_reported_bpm) :
                                                        (last_reported_bpm - bpm);

                        if (!tempo_lock_reported)
                        {
                            printf("TEMPO LOCK: bpm=%lu, confidence=%lu%%\r\n",
                                   (unsigned long) bpm,
                                   (unsigned long) confidence_percent);
                            tempo_lock_reported = true;
                            last_reported_bpm = bpm;
                        }
                        else if (beat_result.tempo_updated && (bpm_difference >= 3U))
                        {
                            printf("TEMPO UPDATE: bpm=%lu, confidence=%lu%%\r\n",
                                   (unsigned long) bpm,
                                   (unsigned long) confidence_percent);
                            last_reported_bpm = bpm;
                        }
                    }

                    if (previous_prediction_active && !beat_result.prediction_active)
                    {
                        uint32_t audio_guard_ms = 0U;

                        printf("BEAT prediction paused: no onset for 3.5 seconds.\r\n");
                        /* The next song must report and establish a fresh lock. */
                        tempo_lock_reported = false;
                        last_reported_bpm = 0U;
                        if (music_dance_return_center(&audio_guard_ms))
                        {
                            servo_noise_ignore_until_ms =
                                audio_timestamp_ms + audio_guard_ms;
                        }
                    }
                    previous_prediction_active = beat_result.prediction_active;

                    if (beat_result.beat_event)
                    {
                        uint32_t const bpm =
                            (uint32_t) (beat_result.bpm + 0.5f);
                        uint32_t const confidence_percent =
                            (uint32_t) ((beat_result.confidence * 100.0f) + 0.5f);
                        uint32_t audio_guard_ms = 0U;

                        beat_count++;
                        if (music_dance_apply_beat(beat_result.bpm, &audio_guard_ms))
                        {
                            servo_noise_ignore_until_ms =
                                audio_timestamp_ms + audio_guard_ms;
                        }
                        else
                        {
                            printf("Dance servo command failed: error=%d\r\n",
                                   Servo_GetLastError());
                        }

                        printf("BEAT #%lu: bpm=%lu, confidence=%lu%%, predicted=1\r\n",
                               (unsigned long) beat_count,
                               (unsigned long) bpm,
                               (unsigned long) confidence_percent);
                    }
                }
            }
        }
    }
}
