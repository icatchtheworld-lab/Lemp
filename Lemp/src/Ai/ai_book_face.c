#include "Ai/ai_book_face.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal_data.h"
#include "model.h"
#include "zf_device/zf_device_scc8660.h"

#define AI_INPUT_W                         ((int) BOOK_FACE_V5_INPUT_WIDTH)
#define AI_INPUT_H                         ((int) BOOK_FACE_V5_INPUT_HEIGHT)
#define AI_HEAD_ANCHORS                    (3U)
#define AI_HEAD_VALUES                     (8U)
#define AI_MAX_CANDIDATES                  (32U)
/* 桌宠跟踪需要及时获得最新坐标，因此每采集一帧就执行一次正式推理。 */
#define AI_FRAME_INTERVAL                  (1U)
#define AI_NO_DETECTION_PRINT_INTERVAL     (10U)
#define AI_CAMERA_INIT_RETRY_COUNT         (5U)
#define AI_CAMERA_POWER_STABLE_DELAY_MS    (1000U)
#define AI_LETTERBOX_QUANTIZED             (56)
#define AI_NMS_IOU_THRESHOLD               (0.45F)
#define AI_BOX_X_OFFSET_PIXELS              (16.0F)
#define AI_BOX_Y_OFFSET_PIXELS              (10.0F)

/*
 * 测试程序默认保留识别明细输出；主程序进入观察者模式后会将其关闭，
 * 避免同步UART打印长时间占用CPU，打断舵机的周期更新。
 */
static bool g_ai_reporting_enabled = true;

typedef struct st_ai_candidate
{
    uint8_t class_id;
    int score_permille;
    float x_min;
    float y_min;
    float x_max;
    float y_max;
} ai_candidate_t;

static uint16_t g_ai_frame[SCC8660_H][SCC8660_W]
    BSP_PLACE_IN_SECTION(".ram_nocache") BSP_ALIGN_VARIABLE(32);
static ai_candidate_t g_ai_candidates[AI_MAX_CANDIDATES];
static ai_book_face_detection_t g_ai_detections[AI_BOOK_FACE_MAX_DETECTIONS];
static uint32_t g_ai_detection_count;
static uint32_t g_ai_frame_id;
static uint32_t g_ai_no_detection_count;

static char const * const g_ai_class_names[AI_BOOK_FACE_CLASS_COUNT] =
{
    "closed_book",
    "open_book",
    "face",
};

static uint16_t const g_ai_anchors_16[AI_HEAD_ANCHORS][2] =
{
    {12U, 18U},
    {37U, 49U},
    {52U, 132U},
};

static uint16_t const g_ai_anchors_8[AI_HEAD_ANCHORS][2] =
{
    {115U, 73U},
    {119U, 199U},
    {242U, 238U},
};

static int const g_ai_class_threshold_permille[AI_BOOK_FACE_CLASS_COUNT] =
{
    650, /* closed_book */
    500, /* open_book */
    200, /* face：降低人脸进入跟踪的置信度门槛。 */
};

static uint16_t camera_rgb565_to_normal(uint16_t pixel)
{
    return (uint16_t) ((pixel >> 8) | (pixel << 8));
}

static int clamp_int(int value, int low, int high)
{
    if (value < low)
    {
        return low;
    }

    if (value > high)
    {
        return high;
    }

    return value;
}

static float clamp_float(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }

    if (value > high)
    {
        return high;
    }

    return value;
}

/* Accurate enough for YOLO decoding without pulling the full libm expf code. */
static float exp_negative_approx(float magnitude)
{
    static float const powers_of_two_negative[] =
    {
        1.0F,
        0.5F,
        0.25F,
        0.125F,
        0.0625F,
        0.03125F,
        0.015625F,
        0.0078125F,
        0.00390625F,
        0.001953125F,
        0.0009765625F,
        0.00048828125F,
        0.000244140625F,
        0.0001220703125F,
        0.00006103515625F,
        0.000030517578125F,
        0.0000152587890625F,
    };
    int exponent;
    float remainder;
    float remainder_squared;
    float polynomial;

    if (magnitude >= 12.0F)
    {
        return 0.0F;
    }

    exponent = (int) (magnitude * 1.4426950408889634F);
    exponent = clamp_int(exponent, 0, 16);
    remainder = magnitude - ((float) exponent * 0.6931471805599453F);
    remainder_squared = remainder * remainder;
    polynomial = 1.0F - remainder + (remainder_squared * 0.5F) -
                 (remainder_squared * remainder * 0.1666666667F) +
                 (remainder_squared * remainder_squared * 0.0416666667F) -
                 (remainder_squared * remainder_squared * remainder * 0.0083333333F);

    return powers_of_two_negative[exponent] * polynomial;
}

static float sigmoid(float value)
{
    float exponential;

    if (value >= 0.0F)
    {
        exponential = exp_negative_approx(value);
        return 1.0F / (1.0F + exponential);
    }

    exponential = exp_negative_approx(-value);
    return exponential / (1.0F + exponential);
}

static int8_t quantize_rgb5(uint16_t value)
{
    int const quantized = (int) (((uint32_t) value * 255U + 31U) / 62U) - 1;
    return (int8_t) ((quantized > 126) ? 126 : quantized);
}

static int8_t quantize_rgb6(uint16_t value)
{
    int const quantized = (int) (((uint32_t) value * 255U + 63U) / 126U) - 1;
    return (int8_t) ((quantized > 126) ? 126 : quantized);
}

static void prepare_model_input(uint16_t const frame[SCC8660_H][SCC8660_W])
{
    int8_t * input = GetModelInputPtr_book_face_v5_images();
    int const scaled_h = (SCC8660_H * AI_INPUT_W) / SCC8660_W;
    int const y_pad = (AI_INPUT_H - scaled_h) / 2;

    /* The NPU input tensor is NHWC INT8 and lives inside the model arena. */
    memset(input,
           AI_LETTERBOX_QUANTIZED,
           (size_t) AI_INPUT_W * (size_t) AI_INPUT_H * BOOK_FACE_V5_INPUT_CHANNELS);

    for (int y = 0; y < scaled_h; y++)
    {
        int const source_y = (y * SCC8660_H) / scaled_h;
        int const destination_y = y + y_pad;

        for (int x = 0; x < AI_INPUT_W; x++)
        {
            int const source_x = (x * SCC8660_W) / AI_INPUT_W;
            uint16_t const rgb565 = camera_rgb565_to_normal(frame[source_y][source_x]);
            uint32_t const index =
                ((uint32_t) destination_y * BOOK_FACE_V5_INPUT_WIDTH + (uint32_t) x) *
                BOOK_FACE_V5_INPUT_CHANNELS;

            input[index] = quantize_rgb5((uint16_t) ((rgb565 >> 11) & 0x1FU));
            input[index + 1U] = quantize_rgb6((uint16_t) ((rgb565 >> 5) & 0x3FU));
            input[index + 2U] = quantize_rgb5((uint16_t) (rgb565 & 0x1FU));
        }
    }
}

static uint32_t output_index(uint8_t channel, uint8_t grid, uint8_t cell_y, uint8_t cell_x)
{
    return (((uint32_t) channel * (uint32_t) grid + (uint32_t) cell_y) * (uint32_t) grid) +
           (uint32_t) cell_x;
}

static float dequantize(int8_t value, int zero_point, float scale)
{
    return (float) ((int) value - zero_point) * scale;
}

static void store_candidate(ai_candidate_t const * candidate, uint32_t * candidate_count)
{
    if (*candidate_count < AI_MAX_CANDIDATES)
    {
        g_ai_candidates[*candidate_count] = *candidate;
        (*candidate_count)++;
        return;
    }

    uint32_t lowest_index = 0U;
    for (uint32_t i = 1U; i < AI_MAX_CANDIDATES; i++)
    {
        if (g_ai_candidates[i].score_permille < g_ai_candidates[lowest_index].score_permille)
        {
            lowest_index = i;
        }
    }

    if (candidate->score_permille > g_ai_candidates[lowest_index].score_permille)
    {
        g_ai_candidates[lowest_index] = *candidate;
    }
}

static void decode_head(int8_t const * output,
                        uint8_t grid,
                        uint8_t stride,
                        int zero_point,
                        float scale,
                        uint16_t const anchors[AI_HEAD_ANCHORS][2],
                        uint32_t * candidate_count)
{
    int const scaled_h = (SCC8660_H * AI_INPUT_W) / SCC8660_W;
    int const y_pad = (AI_INPUT_H - scaled_h) / 2;

    for (uint8_t anchor = 0U; anchor < AI_HEAD_ANCHORS; anchor++)
    {
        uint8_t const channel_base = (uint8_t) (anchor * AI_HEAD_VALUES);

        for (uint8_t cell_y = 0U; cell_y < grid; cell_y++)
        {
            for (uint8_t cell_x = 0U; cell_x < grid; cell_x++)
            {
                float const objectness = sigmoid(dequantize(
                    output[output_index((uint8_t) (channel_base + 4U), grid, cell_y, cell_x)],
                    zero_point,
                    scale));
                float best_class_probability = 0.0F;
                uint8_t best_class_id = 0U;

                for (uint8_t class_id = 0U; class_id < AI_BOOK_FACE_CLASS_COUNT; class_id++)
                {
                    float const probability = sigmoid(dequantize(
                        output[output_index((uint8_t) (channel_base + 5U + class_id), grid, cell_y, cell_x)],
                        zero_point,
                        scale));
                    if (probability > best_class_probability)
                    {
                        best_class_probability = probability;
                        best_class_id = class_id;
                    }
                }

                /*
                 * 当前项目不使用闭合书本类别。
                 * 在候选框进入排序和NMS之前将其丢弃，避免误检结果干扰打开书本和人脸。
                 */
                if (AI_BOOK_FACE_CLASS_CLOSED_BOOK == best_class_id)
                {
                    continue;
                }

                float const score = objectness * best_class_probability;
                int const score_permille = (int) (score * 1000.0F + 0.5F);
                if (score_permille < g_ai_class_threshold_permille[best_class_id])
                {
                    continue;
                }

                float const tx = sigmoid(dequantize(
                    output[output_index(channel_base, grid, cell_y, cell_x)], zero_point, scale));
                float const ty = sigmoid(dequantize(
                    output[output_index((uint8_t) (channel_base + 1U), grid, cell_y, cell_x)],
                    zero_point,
                    scale));
                float const tw_sigmoid = sigmoid(dequantize(
                    output[output_index((uint8_t) (channel_base + 2U), grid, cell_y, cell_x)],
                    zero_point,
                    scale));
                float const th_sigmoid = sigmoid(dequantize(
                    output[output_index((uint8_t) (channel_base + 3U), grid, cell_y, cell_x)],
                    zero_point,
                    scale));
                float const center_x = ((tx * 2.0F - 0.5F) + (float) cell_x) * (float) stride;
                float const center_y = ((ty * 2.0F - 0.5F) + (float) cell_y) * (float) stride;
                float const width_factor = tw_sigmoid * 2.0F;
                float const height_factor = th_sigmoid * 2.0F;
                float const width = width_factor * width_factor * (float) anchors[anchor][0];
                float const height = height_factor * height_factor * (float) anchors[anchor][1];
                ai_candidate_t candidate;

                candidate.class_id = best_class_id;
                candidate.score_permille = score_permille;
                /*
                 * The camera/model combination has a small, repeatable horizontal
                 * calibration error: decoded boxes appear to the left of the target.
                 * Apply the correction in camera coordinates so the box and center
                 * point remain consistent for every consumer of the detection result.
                 */
                candidate.x_min = clamp_float((center_x - width * 0.5F) * (float) SCC8660_W /
                                              (float) AI_INPUT_W + AI_BOX_X_OFFSET_PIXELS,
                                              0.0F,
                                              (float) (SCC8660_W - 1));
                candidate.x_max = clamp_float((center_x + width * 0.5F) * (float) SCC8660_W /
                                              (float) AI_INPUT_W + AI_BOX_X_OFFSET_PIXELS,
                                              0.0F,
                                              (float) (SCC8660_W - 1));
                candidate.y_min = clamp_float(((center_y - height * 0.5F) - (float) y_pad) *
                                              (float) SCC8660_H / (float) scaled_h +
                                              AI_BOX_Y_OFFSET_PIXELS,
                                              0.0F,
                                              (float) (SCC8660_H - 1));
                candidate.y_max = clamp_float(((center_y + height * 0.5F) - (float) y_pad) *
                                              (float) SCC8660_H / (float) scaled_h +
                                              AI_BOX_Y_OFFSET_PIXELS,
                                              0.0F,
                                              (float) (SCC8660_H - 1));

                if ((candidate.x_max > candidate.x_min) && (candidate.y_max > candidate.y_min))
                {
                    store_candidate(&candidate, candidate_count);
                }
            }
        }
    }
}

static void sort_candidates(uint32_t candidate_count)
{
    for (uint32_t i = 1U; i < candidate_count; i++)
    {
        ai_candidate_t const value = g_ai_candidates[i];
        uint32_t position = i;

        while ((position > 0U) &&
               (g_ai_candidates[position - 1U].score_permille < value.score_permille))
        {
            g_ai_candidates[position] = g_ai_candidates[position - 1U];
            position--;
        }

        g_ai_candidates[position] = value;
    }
}

static float candidate_iou(ai_candidate_t const * left, ai_candidate_t const * right)
{
    float const intersection_x_min = (left->x_min > right->x_min) ? left->x_min : right->x_min;
    float const intersection_y_min = (left->y_min > right->y_min) ? left->y_min : right->y_min;
    float const intersection_x_max = (left->x_max < right->x_max) ? left->x_max : right->x_max;
    float const intersection_y_max = (left->y_max < right->y_max) ? left->y_max : right->y_max;
    float const intersection_width = intersection_x_max - intersection_x_min;
    float const intersection_height = intersection_y_max - intersection_y_min;

    if ((intersection_width <= 0.0F) || (intersection_height <= 0.0F))
    {
        return 0.0F;
    }

    float const intersection_area = intersection_width * intersection_height;
    float const left_area = (left->x_max - left->x_min) * (left->y_max - left->y_min);
    float const right_area = (right->x_max - right->x_min) * (right->y_max - right->y_min);
    float const union_area = left_area + right_area - intersection_area;

    return (union_area > 0.0F) ? (intersection_area / union_area) : 0.0F;
}

static void parse_model_output(void)
{
    uint8_t kept_candidate_indices[AI_BOOK_FACE_MAX_DETECTIONS];
    uint32_t candidate_count = 0U;

    decode_head(GetModelOutputPtr_book_face_v5_output0_70440_70594(),
                BOOK_FACE_V5_OUTPUT16_GRID,
                16U,
                BOOK_FACE_V5_OUTPUT16_ZERO_POINT,
                BOOK_FACE_V5_OUTPUT16_SCALE,
                g_ai_anchors_16,
                &candidate_count);
    decode_head(GetModelOutputPtr_book_face_v5__837_70448_70593(),
                BOOK_FACE_V5_OUTPUT8_GRID,
                32U,
                BOOK_FACE_V5_OUTPUT8_ZERO_POINT,
                BOOK_FACE_V5_OUTPUT8_SCALE,
                g_ai_anchors_8,
                &candidate_count);

    sort_candidates(candidate_count);
    g_ai_detection_count = 0U;

    for (uint32_t candidate_index = 0U;
         (candidate_index < candidate_count) &&
         (g_ai_detection_count < AI_BOOK_FACE_MAX_DETECTIONS);
         candidate_index++)
    {
        ai_candidate_t const * candidate = &g_ai_candidates[candidate_index];
        bool suppressed = false;

        for (uint32_t kept_index = 0U; kept_index < g_ai_detection_count; kept_index++)
        {
            if (candidate_iou(candidate, &g_ai_candidates[kept_candidate_indices[kept_index]]) >
                AI_NMS_IOU_THRESHOLD)
            {
                suppressed = true;
                break;
            }
        }

        if (suppressed)
        {
            continue;
        }

        ai_book_face_detection_t * detection = &g_ai_detections[g_ai_detection_count];
        detection->class_id = candidate->class_id;
        detection->frame_id = g_ai_frame_id;
        detection->score_permille = candidate->score_permille;
        detection->x_min = (int) (candidate->x_min + 0.5F);
        detection->y_min = (int) (candidate->y_min + 0.5F);
        detection->x_max = (int) (candidate->x_max + 0.5F);
        detection->y_max = (int) (candidate->y_max + 0.5F);
        detection->center_x = (detection->x_min + detection->x_max) / 2;
        detection->center_y = (detection->y_min + detection->y_max) / 2;
        kept_candidate_indices[g_ai_detection_count] = (uint8_t) candidate_index;
        g_ai_detection_count++;
    }
}

char const * ai_book_face_class_name(uint8_t class_id)
{
    if (class_id >= AI_BOOK_FACE_CLASS_COUNT)
    {
        return "unknown";
    }

    return g_ai_class_names[class_id];
}

uint32_t ai_book_face_get_detections(ai_book_face_detection_t * detections, uint32_t capacity)
{
    uint32_t const copy_count = (capacity < g_ai_detection_count) ? capacity : g_ai_detection_count;

    if ((NULL != detections) && (copy_count > 0U))
    {
        memcpy(detections, g_ai_detections, copy_count * sizeof(g_ai_detections[0]));
    }

    return g_ai_detection_count;
}

bool ai_book_face_get_best_detection(uint8_t class_id, ai_book_face_detection_t * detection)
{
    if ((NULL == detection) || (class_id >= AI_BOOK_FACE_CLASS_COUNT))
    {
        return false;
    }

    for (uint32_t i = 0U; i < g_ai_detection_count; i++)
    {
        if (class_id == g_ai_detections[i].class_id)
        {
            *detection = g_ai_detections[i];
            return true;
        }
    }

    return false;
}

bool ai_book_face_get_latest_frame(uint16_t const ** frame, uint32_t * frame_id)
{
    if ((NULL == frame) || (NULL == frame_id) || (0U == g_ai_frame_id))
    {
        return false;
    }

    *frame = &g_ai_frame[0][0];
    *frame_id = g_ai_frame_id;
    return true;
}

bool ai_book_face_init(void)
{
    /* 首次访问SCCB前等待摄像头电源和内部命令处理器稳定。 */
    R_BSP_SoftwareDelay(AI_CAMERA_POWER_STABLE_DELAY_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);

    for (uint32_t attempt = 1U; attempt <= AI_CAMERA_INIT_RETRY_COUNT; attempt++)
    {
        uint8_t const init_state = scc8660_init();

        if (0U == init_state)
        {
            printf("SCC8660 init success.\r\n");
            break;
        }

        printf("SCC8660 init retry %lu/%u, stage=%u...\r\n",
               (unsigned long) attempt,
               (unsigned int) AI_CAMERA_INIT_RETRY_COUNT,
               (unsigned int) init_state);

        if (AI_CAMERA_INIT_RETRY_COUNT == attempt)
        {
            /* 即使失败发生在CEU已打开之后，也要清理可能残留的采集状态。 */
            (void) scc8660_capture_stop();
            printf("SCC8660 init failed; book/face input disabled.\r\n");
            return false;
        }

        R_BSP_SoftwareDelay(500U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    fsp_err_t const err = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl, &g_rm_ethosu0_cfg);
    if (FSP_SUCCESS != err)
    {
        /* NPU不可用时主程序不会消费图像，因此同时停止已经启动的CEU。 */
        (void) scc8660_capture_stop();
        printf("AI NPU init failed: %d\r\n", (int) err);
        return false;
    }

    g_ai_detection_count = 0U;
    g_ai_frame_id = 0U;
    printf("Book/face V5 NPU model ready: 256x256, direct INT8 arena input.\r\n");
    return true;
}

void ai_book_face_set_reporting_enabled(bool enabled)
{
    g_ai_reporting_enabled = enabled;
}

bool ai_book_face_set_camera_enabled(bool enabled)
{
    fsp_err_t const err = enabled ?
        scc8660_capture_resume() : scc8660_capture_stop();

    if (FSP_SUCCESS != err)
    {
        printf("SCC8660 capture %s failed: %d\r\n",
               enabled ? "resume" : "stop",
               (int) err);
        return false;
    }

    if (!enabled)
    {
        /* 暂停期间清除旧结果，恢复后必须等待一帧新图像再提交坐标。 */
        g_ai_detection_count = 0U;
        scc8660_finish_flag = false;
    }
    return true;
}

bool ai_book_face_poll(void)
{
    if (!scc8660_finish_flag)
    {
        return false;
    }

    scc8660_finish_flag = false;
    g_ai_frame_id++;
    memcpy(g_ai_frame, scc8660_image, sizeof(g_ai_frame));

    if ((g_ai_frame_id % AI_FRAME_INTERVAL) != 0U)
    {
        return false;
    }

    prepare_model_input(g_ai_frame);
    if (0 != RunModel_book_face_v5(false))
    {
        g_ai_detection_count = 0U;
        printf("Book/face NPU inference failed at frame=%lu.\r\n", (unsigned long) g_ai_frame_id);
        return false;
    }

    parse_model_output();

    if (g_ai_detection_count > 0U)
    {
        g_ai_no_detection_count = 0U;
        if (g_ai_reporting_enabled)
        {
            for (uint32_t i = 0U; i < g_ai_detection_count; i++)
            {
                ai_book_face_detection_t const * detection = &g_ai_detections[i];

                /* 输出检测框中心，便于单独调试AI坐标时观察结果。 */
                printf("AI frame=%lu class=%s id=%u score=%d.%03d box=(%d,%d)-(%d,%d) center=(%d,%d)\r\n",
                       (unsigned long) detection->frame_id,
                       ai_book_face_class_name(detection->class_id),
                       (unsigned int) detection->class_id,
                       detection->score_permille / 1000,
                       detection->score_permille % 1000,
                       detection->x_min,
                       detection->y_min,
                       detection->x_max,
                       detection->y_max,
                       detection->center_x,
                       detection->center_y);
            }
        }
    }
    else
    {
        g_ai_no_detection_count++;
        if (g_ai_no_detection_count >= AI_NO_DETECTION_PRINT_INTERVAL)
        {
            g_ai_no_detection_count = 0U;
            if (g_ai_reporting_enabled)
            {
                printf("AI frame=%lu no book/face detection.\r\n", (unsigned long) g_ai_frame_id);
            }
        }
    }

    return true;
}
