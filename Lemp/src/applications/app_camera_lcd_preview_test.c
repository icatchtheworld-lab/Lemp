#include "app.h"

#include "Ai/ai_book_face.h"
#include "Screen/drv_spi_display.h"
#include "ServoLib/ServoDriver.h"
#include "zf_device/zf_device_scc8660.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAMERA_PREVIEW_X             ((LCD_SCREEN_WIDTH - SCC8660_W) / 2U)
#define CAMERA_PREVIEW_Y             ((LCD_SCREEN_HEIGHT - SCC8660_H) / 2U)
#define CAMERA_PREVIEW_CLEAR_ROWS    (8U)
#define CAMERA_BOX_THICKNESS          (2)//框起来的厚度
#define CAMERA_BOX_COLOR              ((uint16_t) 0xE007U) /* 绿色，字节交换后的RGB565。 */
#define CAMERA_TARGET_CENTER_COLOR    ((uint16_t) 0xE0FFU) /* 黄色，表示目标中心。 */
#define CAMERA_IMAGE_CENTER_COLOR     ((uint16_t) 0xFFFFU) /* 白色，表示画面中心。 */
#define CAMERA_TRACK_DEAD_ZONE_COLOR  ((uint16_t) 0x00F8U) /* 红色，表示停止追踪区域。 */
#define CAMERA_TRACK_DEAD_ZONE_PIXELS (8)
#define CAMERA_TRACK_SERVO1_MIN        (1500)
#define CAMERA_TRACK_SERVO1_MAX        (2700)
#define CAMERA_TRACK_SERVO2_MIN        (1500)
#define CAMERA_TRACK_SERVO2_MAX        (2050)
#define CAMERA_TRACK_SERVO1_COUNTS_PER_PIXEL (20)
#define CAMERA_TRACK_SERVO2_COUNTS_PER_PIXEL (20)
#define CAMERA_TRACK_COMMAND_CHANGE_MIN (6)
#define CAMERA_TRACK_SERVO_SPEED       (300U)
#define CAMERA_TRACK_SERVO_ACC         (10U)
#define CAMERA_TRACK_SERVO_STARTUP_MS  (1000U)
#define CAMERA_TRACK_SERVO_MODE_WAIT_MS (50U)
#define CAMERA_TRACK_POSITION_RETRIES  (5U)
#define CAMERA_TRACK_RETRY_WAIT_MS     (50U)

typedef enum e_camera_track_action
{
    CAMERA_TRACK_ACTION_UNKNOWN = 0,
    CAMERA_TRACK_ACTION_STOP,
    CAMERA_TRACK_ACTION_NEGATIVE,
    CAMERA_TRACK_ACTION_POSITIVE,
} camera_track_action_t;

static bool s_camera_preview_initialized;
static uint32_t s_camera_preview_frame_id;
static camera_track_action_t s_servo1_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
static camera_track_action_t s_servo2_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
static int16_t s_servo1_track_position;
static int16_t s_servo2_track_position;
static bool s_camera_tracking_servo_ready;
static bool s_servo1_live_read_error_reported;
static bool s_servo2_live_read_error_reported;
static uint16_t s_camera_preview_clear_buffer[LCD_SCREEN_WIDTH * CAMERA_PREVIEW_CLEAR_ROWS]//清屏缓存区，320*8  初始化的时候陈旭反复调用 这样只需要约5KiB缓冲区，不需要准备完整的屏幕缓冲区
    BSP_PLACE_IN_SECTION(".ram_nocache") BSP_ALIGN_VARIABLE(32);

/*
 * NPU使用的图像属于AI模块，因此不直接修改。
 * 先复制到显示缓冲，再在显示缓冲上绘制检测框。
 */
static uint16_t s_camera_preview_overlay[SCC8660_H][SCC8660_W]//带框的摄像头显示缓存区
    BSP_PLACE_IN_SECTION(".ram_nocache") BSP_ALIGN_VARIABLE(32);

/* 复用检测框绘制函数显示中心停止区域，不增加独立的绘制代码路径。 */
static ai_book_face_detection_t const s_camera_tracking_dead_zone =
{
    .x_min = (SCC8660_W / 2) - CAMERA_TRACK_DEAD_ZONE_PIXELS,
    .y_min = (SCC8660_H / 2) - CAMERA_TRACK_DEAD_ZONE_PIXELS,
    .x_max = (SCC8660_W / 2) + CAMERA_TRACK_DEAD_ZONE_PIXELS,
    .y_max = (SCC8660_H / 2) + CAMERA_TRACK_DEAD_ZONE_PIXELS,
    .center_x = SCC8660_W / 2,
    .center_y = SCC8660_H / 2,
};

//添加绘制函数
static void camera_preview_set_pixel(int x,int y,uint16_t color)
{
    if ((x < 0) || (x >= SCC8660_W) ||
            (y < 0) || (y >= SCC8660_H))
        {
            return;
        }
    s_camera_preview_overlay[(uint32_t) y][(uint32_t) x] = color;
}
/* 绘制十字，用来表示目标中心或画面中心。 */
static void camera_preview_draw_cross(int center_x,
        int center_y,
        int radius,
        uint16_t color)
{
    for (int offset = -radius; offset <= radius; offset++)
       {
           camera_preview_set_pixel(center_x + offset, center_y, color);
           camera_preview_set_pixel(center_x, center_y + offset, color);
       }
}
/* 根据NPU检测框坐标绘制矩形边框。 */

static void camera_preview_draw_box(ai_book_face_detection_t const * detection,
                                    uint16_t color)
{
    for (int layer = 0; layer < CAMERA_BOX_THICKNESS; layer++)
        {
            int const left   = detection->x_min + layer;
            int const right  = detection->x_max - layer;
            int const top    = detection->y_min + layer;
            int const bottom = detection->y_max - layer;

            if ((left > right) || (top > bottom))
            {
                break;
            }

            /* 绘制矩形的上边和下边。 */
            for (int x = left; x <= right; x++)
            {
                camera_preview_set_pixel(x, top, color);
                camera_preview_set_pixel(x, bottom, color);
            }

            /* 绘制矩形的左边和右边。 */
            for (int y = top; y <= bottom; y++)
            {
                camera_preview_set_pixel(left, y, color);
                camera_preview_set_pixel(right, y, color);
            }
        }
}

static camera_track_action_t camera_tracking_axis_action(int target_center,
                                                         int image_center)
{
    if (target_center < (image_center - CAMERA_TRACK_DEAD_ZONE_PIXELS))
    {
        return CAMERA_TRACK_ACTION_NEGATIVE;
    }

    if (target_center > (image_center + CAMERA_TRACK_DEAD_ZONE_PIXELS))
    {
        return CAMERA_TRACK_ACTION_POSITIVE;
    }

    return CAMERA_TRACK_ACTION_STOP;
}

/* 返回串口日志使用的舵机动作名称。 */
static char const * camera_tracking_action_name(uint8_t servo_id,
                                                camera_track_action_t action)
{
    if (CAMERA_TRACK_ACTION_STOP == action)
    {
        return "STOP";
    }

    if (1U == servo_id)
    {
        return (CAMERA_TRACK_ACTION_NEGATIVE == action) ? "LEFT" : "RIGHT";
    }

    return (CAMERA_TRACK_ACTION_NEGATIVE == action) ? "UP" : "DOWN";
}

static int16_t camera_tracking_limit_position(int position, int minimum, int maximum)
{
    if (position < minimum)
    {
        return (int16_t) minimum;
    }

    if (position > maximum)
    {
        return (int16_t) maximum;
    }

    return (int16_t) position;
}

static int camera_tracking_read_position(uint8_t servo_id)
{
    for (uint32_t attempt = 1U; attempt <= CAMERA_TRACK_POSITION_RETRIES; attempt++)
    {
        int const position = Servo_ReadPosition(servo_id);

        if ((position >= SERVO_STS_POSITION_MIN) &&
            (position <= SERVO_STS_POSITION_MAX))
        {
            return position;
        }

        printf("[TRACK] SERVO%u position read retry %lu/%u\r\n",
               (unsigned int) servo_id,
               (unsigned long) attempt,
               (unsigned int) CAMERA_TRACK_POSITION_RETRIES);
        R_BSP_SoftwareDelay(CAMERA_TRACK_RETRY_WAIT_MS,
                            BSP_DELAY_UNITS_MILLISECONDS);
    }

    return -1;
}

static bool camera_tracking_read_live_position(uint8_t servo_id,
                                               int * position,
                                               bool * error_reported)
{
    int const current_position = Servo_ReadPosition(servo_id);

    if ((current_position < SERVO_STS_POSITION_MIN) ||
        (current_position > SERVO_STS_POSITION_MAX))
    {
        if (!*error_reported)
        {
            printf("[TRACK] SERVO%u live position read failed\r\n",
                   (unsigned int) servo_id);
            *error_reported = true;
        }
        return false;
    }

    *error_reported = false;
    *position = current_position;
    return true;
}

static int camera_tracking_abs(int value)
{
    return (value < 0) ? -value : value;
}

static bool camera_tracking_servo_init(void)
{
    int servo1_position;
    int servo2_position;

    Servo_Init();
    if (!Servo_IsOpened())
    {
        return false;
    }

    /* 与主程序的舵机初始化时序一致，等待总线和舵机上电稳定。 */
    R_BSP_SoftwareDelay(CAMERA_TRACK_SERVO_STARTUP_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);

    /* 只配置1、2号舵机，3～5号不发送模式、扭矩或位置命令。 */
    if (!Servo_PositionMode(1U) || !Servo_PositionMode(2U))
    {
        return false;
    }

    R_BSP_SoftwareDelay(CAMERA_TRACK_SERVO_MODE_WAIT_MS,
                        BSP_DELAY_UNITS_MILLISECONDS);

    servo1_position = camera_tracking_read_position(1U);
    servo2_position = camera_tracking_read_position(2U);
    if ((servo1_position < CAMERA_TRACK_SERVO1_MIN) ||
        (servo1_position > CAMERA_TRACK_SERVO1_MAX) ||
        (servo2_position < CAMERA_TRACK_SERVO2_MIN) ||
        (servo2_position > CAMERA_TRACK_SERVO2_MAX))
    {
        printf("[TRACK] servo position invalid: S1=%d S2=%d\r\n",
               servo1_position,
               servo2_position);
        return false;
    }

    if (!Servo_EnableTorque(1U, 1U) || !Servo_EnableTorque(2U, 1U))
    {
        return false;
    }

    s_servo1_track_position = (int16_t) servo1_position;
    s_servo2_track_position = (int16_t) servo2_position;
    s_servo1_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
    s_servo2_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
    s_servo1_live_read_error_reported = false;
    s_servo2_live_read_error_reported = false;
    s_camera_tracking_servo_ready = true;

    printf("[TRACK] servos ready: S1=%d S2=%d\r\n",
           servo1_position,
           servo2_position);
    return true;
}

static void camera_tracking_update(ai_book_face_detection_t const * target)
{
    uint8_t servo_ids[2];
    int16_t servo_positions[2];
    uint16_t servo_speeds[2];
    uint8_t servo_accelerations[2];
    camera_track_action_t actions[2];
    uint8_t command_count = 0U;
    camera_track_action_t servo1_action = CAMERA_TRACK_ACTION_STOP;
    camera_track_action_t servo2_action = CAMERA_TRACK_ACTION_STOP;
    int box_center_x = SCC8660_W / 2;
    int box_center_y = SCC8660_H / 2;

    if (!s_camera_tracking_servo_ready)
    {
        return;
    }

    if (NULL != target)
    {
        /* 停止判断严格使用当前绿框四条边计算出的中心点。 */
        box_center_x = (target->x_min + target->x_max) / 2;
        box_center_y = (target->y_min + target->y_max) / 2;

        servo1_action = camera_tracking_axis_action(box_center_x, SCC8660_W / 2);
        servo2_action = camera_tracking_axis_action(box_center_y, SCC8660_H / 2);
    }

    if (CAMERA_TRACK_ACTION_STOP == servo1_action)
    {
        if (CAMERA_TRACK_ACTION_STOP != s_servo1_track_action)
        {
            int current_position;

            /* 进入死区时用当前位置覆盖旧目标，立即终止尚未完成的大步运动。 */
            if (camera_tracking_read_live_position(1U,
                                                   &current_position,
                                                   &s_servo1_live_read_error_reported) &&
                (current_position != s_servo1_track_position))
            {
                servo_ids[command_count] = 1U;
                servo_positions[command_count] = (int16_t) current_position;
                servo_speeds[command_count] = CAMERA_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = CAMERA_TRACK_SERVO_ACC;
                actions[command_count] = CAMERA_TRACK_ACTION_STOP;
                command_count++;
            }
            else
            {
                printf("[TRACK] SERVO1 STOP\r\n");
            }
        }
    }
    else
    {
        int current_position;

        if (camera_tracking_read_live_position(1U,
                                               &current_position,
                                               &s_servo1_live_read_error_reported))
        {
            int const pixel_error = box_center_x - (SCC8660_W / 2);
            int16_t const next_position = camera_tracking_limit_position(
                current_position + pixel_error * CAMERA_TRACK_SERVO1_COUNTS_PER_PIXEL,
                CAMERA_TRACK_SERVO1_MIN,
                CAMERA_TRACK_SERVO1_MAX);

            if (camera_tracking_abs((int) next_position -
                                    (int) s_servo1_track_position) >=
                CAMERA_TRACK_COMMAND_CHANGE_MIN)
            {
                servo_ids[command_count] = 1U;
                servo_positions[command_count] = next_position;
                servo_speeds[command_count] = CAMERA_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = CAMERA_TRACK_SERVO_ACC;
                actions[command_count] = servo1_action;
                command_count++;
            }
        }
    }

    if (CAMERA_TRACK_ACTION_STOP == servo2_action)
    {
        if (CAMERA_TRACK_ACTION_STOP != s_servo2_track_action)
        {
            int current_position;

            if (camera_tracking_read_live_position(2U,
                                                   &current_position,
                                                   &s_servo2_live_read_error_reported) &&
                (current_position != s_servo2_track_position))
            {
                servo_ids[command_count] = 2U;
                servo_positions[command_count] = (int16_t) current_position;
                servo_speeds[command_count] = CAMERA_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = CAMERA_TRACK_SERVO_ACC;
                actions[command_count] = CAMERA_TRACK_ACTION_STOP;
                command_count++;
            }
            else
            {
                printf("[TRACK] SERVO2 STOP\r\n");
            }
        }
    }
    else
    {
        int current_position;

        if (camera_tracking_read_live_position(2U,
                                               &current_position,
                                               &s_servo2_live_read_error_reported))
        {
            /* 实机方向与画面Y坐标一致：上方误差为负，下方误差为正。 */
            int const pixel_error = box_center_y - (SCC8660_H / 2);
            int16_t const next_position = camera_tracking_limit_position(
                current_position + pixel_error * CAMERA_TRACK_SERVO2_COUNTS_PER_PIXEL,
                CAMERA_TRACK_SERVO2_MIN,
                CAMERA_TRACK_SERVO2_MAX);

            if (camera_tracking_abs((int) next_position -
                                    (int) s_servo2_track_position) >=
                CAMERA_TRACK_COMMAND_CHANGE_MIN)
            {
                servo_ids[command_count] = 2U;
                servo_positions[command_count] = next_position;
                servo_speeds[command_count] = CAMERA_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = CAMERA_TRACK_SERVO_ACC;
                actions[command_count] = servo2_action;
                command_count++;
            }
        }
    }

    s_servo1_track_action = servo1_action;
    s_servo2_track_action = servo2_action;

    if (0U == command_count)
    {
        return;
    }

    if (!Servo_SyncWritePos(servo_ids,
                            command_count,
                            servo_positions,
                            servo_speeds,
                            servo_accelerations))
    {
        printf("[TRACK] servo write failed\r\n");
        return;
    }

    for (uint8_t i = 0U; i < command_count; i++)
    {
        if (1U == servo_ids[i])
        {
            s_servo1_track_position = servo_positions[i];
        }
        else
        {
            s_servo2_track_position = servo_positions[i];
        }

        printf("[TRACK] SERVO%u %s position=%d\r\n",
               (unsigned int) servo_ids[i],
               camera_tracking_action_name(servo_ids[i], actions[i]),
               servo_positions[i]);
    }
}

static fsp_err_t camera_preview_display_init(void)
{
    fsp_err_t err = drv_spi_display_init();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    for (uint16_t y = 0U; y < LCD_SCREEN_HEIGHT; y += CAMERA_PREVIEW_CLEAR_ROWS)
    {
        err = spi_display_set_window(0U,
                                     y,
                                     LCD_SCREEN_WIDTH - 1U,
                                     y + CAMERA_PREVIEW_CLEAR_ROWS - 1U);
        if (FSP_SUCCESS != err)
        {
            return err;
        }

        err = drv_spi_display_flush_data((uint8_t *) s_camera_preview_clear_buffer,
                                         sizeof(s_camera_preview_clear_buffer));
        if (FSP_SUCCESS != err)
        {
            return err;
        }
    }

    s_camera_preview_frame_id = 0U;
    s_servo1_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
    s_servo2_track_action = CAMERA_TRACK_ACTION_UNKNOWN;
    s_camera_preview_initialized = true;
    return FSP_SUCCESS;
}

static fsp_err_t camera_preview_display_process(void)
{
    uint16_t const * frame;
    uint32_t frame_id;

    if (!s_camera_preview_initialized ||
        !ai_book_face_get_latest_frame(&frame, &frame_id) ||
        (frame_id == s_camera_preview_frame_id))
    {
        return FSP_SUCCESS;
    }

    ai_book_face_detection_t detections[AI_BOOK_FACE_MAX_DETECTIONS];
    ai_book_face_detection_t const * tracking_target = NULL;
    uint32_t tracking_target_area = 0U;
    uint32_t detection_count;

    /* 复制推理使用的原始画面，避免直接修改AI模块的帧缓存。 */
    memcpy(s_camera_preview_overlay,
           frame,
           sizeof(s_camera_preview_overlay));

    /* 获取当前推理产生的全部检测结果。 */
    detection_count = ai_book_face_get_detections(
        detections,
        AI_BOOK_FACE_MAX_DETECTIONS);

    if (detection_count > AI_BOOK_FACE_MAX_DETECTIONS)
    {
        detection_count = AI_BOOK_FACE_MAX_DETECTIONS;
    }

    for (uint32_t i = 0U; i < detection_count; i++)
    {
        /*
         * 只绘制与当前画面属于同一帧的检测结果，
         * 避免把上一帧检测框画到新的摄像头画面上。
         */
        if (detections[i].frame_id != frame_id)
        {
            continue;
        }

        /* 多目标时跟踪绿框面积最大的目标。 */
        int const box_width = detections[i].x_max - detections[i].x_min + 1;
        int const box_height = detections[i].y_max - detections[i].y_min + 1;
        uint32_t const box_area = ((box_width > 0) && (box_height > 0)) ?
                                  (uint32_t) box_width * (uint32_t) box_height : 0U;

        if ((NULL == tracking_target) || (box_area > tracking_target_area))
        {
            tracking_target = &detections[i];
            tracking_target_area = box_area;
        }

        camera_preview_draw_box(&detections[i], CAMERA_BOX_COLOR);
    }

    if (NULL != tracking_target)
    {
        int const tracking_center_x =
            (tracking_target->x_min + tracking_target->x_max) / 2;
        int const tracking_center_y =
            (tracking_target->y_min + tracking_target->y_max) / 2;

        /* 黄色十字只标记当前真正用于舵机跟踪的最大目标。 */
        camera_preview_draw_cross(tracking_center_x,
                                  tracking_center_y,
                                  3,
                                  CAMERA_TARGET_CENTER_COLOR);
    }

    camera_tracking_update(tracking_target);

    /* 红框与舵机停止判断共用同一个中心死区参数。 */
    camera_preview_draw_box(&s_camera_tracking_dead_zone,
                            CAMERA_TRACK_DEAD_ZONE_COLOR);

    /* 在摄像头画面正中央绘制白色十字，即坐标(80,60)。 */
    camera_preview_draw_cross(SCC8660_W / 2,
                              SCC8660_H / 2,
                              4,
                              CAMERA_IMAGE_CENTER_COLOR);




    fsp_err_t err = spi_display_set_window(CAMERA_PREVIEW_X,
                                           CAMERA_PREVIEW_Y,
                                           CAMERA_PREVIEW_X + SCC8660_W - 1U,
                                           CAMERA_PREVIEW_Y + SCC8660_H - 1U);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = drv_spi_display_flush_data(
        (uint8_t *) s_camera_preview_overlay,
        (uint32_t) sizeof(s_camera_preview_overlay));
    if (FSP_SUCCESS == err)
    {
        s_camera_preview_frame_id = frame_id;
    }

    return err;
}

void app_camera_lcd_preview_test(void)
{
    fsp_err_t err;

    /* The camera/SCCB startup remains ahead of all LCD timer activity. */
    if (!ai_book_face_init())
    {
        app_fatal_error("Camera and AI preview initialization", FSP_ERR_INTERNAL);
    }

    /* 跟踪模式只保留舵机操作日志，不输出检测框明细。 */
    ai_book_face_set_reporting_enabled(false);

    if (!camera_tracking_servo_init())
    {
        app_fatal_error("Camera tracking servo initialization", FSP_ERR_INTERNAL);
    }

    err = camera_preview_display_init();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Camera preview display initialization", err);
    }

    printf("Camera preview started: 160x120 centered on LCD.\r\n");

    while (1)
    {
        bool const inference_complete = ai_book_face_poll();

        if (inference_complete)
        {
            /*
             * 只在NPU产生新结果时刷新画面，
             * 保证显示图像和检测框来自同一个frame_id。
             */
            err = camera_preview_display_process();
            if (FSP_SUCCESS != err)
            {
                app_fatal_error("Camera preview refresh", err);
            }
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
