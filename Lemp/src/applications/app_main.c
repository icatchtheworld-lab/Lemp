#include "app.h"

#include "Ai/ai_book_face.h"
#include "Light_Module/light_module.h"
#include "Real_time/time.h"
#include "Screen/drv_gpt_timer.h"
#include "Servo/observer_mode.h"
#include "Servo/servo.h"
#include "ServoLib/ServoDriver.h"
#include "Voice/voice.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define APP_LVGL_NORMAL_PERIOD_MS             (5U)
#define APP_LVGL_OBSERVER_PERIOD_MS         (200U)
/* 麦克风启动失败后限速重试，避免主循环连续访问驱动并刷满串口。 */
#define APP_VOICE_START_RETRY_MS            (1000U)
/* CEU暂停或恢复失败后同样限速重试。 */
#define APP_CAMERA_STATE_RETRY_MS           (1000U)

/* 音乐专项诊断期间关闭NPU，避免推理占用时间影响麦克风队列。 */
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
#define APP_OBSERVER_VISION_ENABLE             (0U)
#else
/* 正常桌宠待机状态中启用NPU；校准、巡查、回应和睡眠期间自动暂停。 */
#define APP_OBSERVER_VISION_ENABLE             (1U)
#endif

/* 自动调光每200 ms采样一次，兼顾响应速度和ADC占用。 */
#define APP_LIGHT_SAMPLE_PERIOD_MS           (200U)
#define APP_LIGHT_ADC_MAX_VALUE             (4095U)
#define APP_LIGHT_MIN_PERCENT                 (15U)
#define APP_LIGHT_MAX_PERCENT                (100U)
#define APP_LIGHT_DEADBAND_PERCENT             (3U)
#define APP_LIGHT_MAX_STEP_PERCENT             (5U)
#define APP_LIGHT_MAX_CONSECUTIVE_FAILURES      (3U)

/* 每100 ms检查一次2、3号承重舵机的运动和负载。 */
#define APP_TABLE_LAMP_SERVO_PERIOD_MS        (100U)
/* 展开命令发出后给舵机留出到位时间，再切换到可手动调节的阻尼状态。 */
#define APP_TABLE_LAMP_POSE_SETTLE_MS         (1200U)
#define APP_CHAT_LIGHT_ON_PERCENT               (60U)
/* 桌宠页面的灯光按钮只使用固定亮度：开启90%，关闭0%。 */
#define APP_OBSERVER_LAMP_ON_PERCENT             (90U)
#define APP_CHAT_SERVO_STEP_MS                  (10U)
#define APP_CHAT_STAND_HEAD_DELAY_MS           (250U)
#define APP_CHAT_SIT_FINAL_DELAY_MS             (500U)
#define APP_CHAT_SHAKE_HALF_SWEEP_MS            (700U)
#define APP_CHAT_SHAKE_FULL_SWEEP_MS           (1400U)
#define APP_CHAT_SHAKE_RETRY_MS                  (50U)
#define APP_CHAT_SHAKE_AMPLITUDE                 (600)
#define APP_CHAT_SHAKE_MIN_POSITION              (500)
#define APP_CHAT_SHAKE_MAX_POSITION             (2500)

typedef enum e_app_chat_servo_action
{
    APP_CHAT_SERVO_ACTION_NONE = 0U,
    APP_CHAT_SERVO_ACTION_STAND,
    APP_CHAT_SERVO_ACTION_SIT_FINAL,
    APP_CHAT_SERVO_ACTION_SHAKE_HEAD
} app_chat_servo_action_t;

/*
 * Cloud-chat motion tuning.
 *
 * Change these position arrays when adjusting the action associated with
 * "stand", "sit", or "shake head".  Values are STS servo position counts
 * (0..4095).  The stand pose is intentionally more open than the original
 * power-on pose, so switching between stand and sit has a clearer amplitude.
 */
static uint8_t const s_chat_sit_step1_ids[] = {1U, 2U, 4U, 5U};
static int16_t const s_chat_sit_step1_positions[] = {2057, 1729, 1276, 2967};
static uint16_t const s_chat_sit_step1_speeds[] = {400U, 1000U, 1000U, 1000U};
static uint8_t const s_chat_sit_step1_accelerations[] = {5U, 5U, 10U, 10U};

static int16_t const s_chat_stand_positions[] = {2084, 1850, 2520, 1266, 2720};
static uint16_t const s_chat_stand_speeds[] = {600U, 600U, 1000U, 600U, 600U};
static uint8_t const s_chat_stand_accelerations[] = {5U, 5U, 10U, 5U, 5U};

static uint8_t const s_chat_sit_ids[] = {1U, 2U, 3U, 4U, 5U};
static int16_t const s_chat_sit_positions[] = {2057, 1639, 3178, 1254, 3179};
static uint16_t const s_chat_sit_speeds[] = {400U, 1000U, 500U, 1000U, 1000U};
static uint8_t const s_chat_sit_accelerations[] = {5U, 5U, 10U, 10U, 10U};

/* Calculated around the position where the chat shake starts. */
static int16_t s_chat_shake_positions[2];

static volatile bool       s_mode_request_pending;
static volatile app_mode_t s_requested_mode = APP_MODE_IDLE;
static volatile bool               s_chat_command_request_pending;
static volatile app_chat_command_t s_requested_chat_command = APP_CHAT_COMMAND_NONE;

static app_mode_t s_app_mode = APP_MODE_IDLE;
static bool       s_voice_running;
/* 仅在观察者状态机的听觉需求发生变化时启停麦克风。 */
static bool       s_observer_hearing_required;
#if APP_OBSERVER_VISION_ENABLE
static bool       s_ai_available;
static bool       s_observer_camera_enabled;
static uint32_t   s_observer_camera_retry_after_ms;
#endif

static bool     s_light_bulb_ready;
static bool     s_auto_light_ready;
static bool     s_auto_brightness_enabled = true;
static bool     s_manual_brightness_pending;
static bool     s_light_filter_valid;
static bool     s_light_failure_reported;
static bool     s_light_output_error_reported;
static uint8_t  s_light_failure_count;
static uint8_t  s_lamp_brightness_percent;
static uint8_t  s_manual_brightness_percent = 60U;
static uint32_t s_filtered_light_raw;
static uint32_t s_last_light_sample_ms;
static uint32_t s_last_table_lamp_servo_ms;
static uint32_t s_table_lamp_pose_start_ms;
static bool     s_table_lamp_pose_ready;
static uint32_t s_observer_voice_retry_after_ms;
/* UI先更新请求状态，实际的模式切换和PWM写入由下一轮主循环执行。 */
static bool     s_observer_music_enabled;
static bool     s_observer_music_request_pending;
static bool     s_observer_lamp_enabled;
static bool     s_observer_lamp_request_pending;
static app_chat_servo_action_t s_chat_servo_action;
static uint8_t  s_chat_servo_step;
static int16_t  s_chat_servo4_restore_position = 1266;
static uint32_t s_chat_servo_due_ms;

static bool app_main_voice_start(void);
static void app_main_voice_stop(void);
static void app_main_light_control_init(void);
static void app_main_process_requests(void);
static void app_main_process_observer_control_requests(void);
static bool app_main_take_mode_request(app_mode_t * p_requested_mode);
static bool app_main_take_chat_command(app_chat_command_t * command);
static void app_main_set_mode(app_mode_t next_mode);
static void app_main_execute_chat_command(app_chat_command_t command);
static void app_main_chat_servo_update(void);
static void app_main_table_lamp_update(void);
static void app_main_table_lamp_servo_update(void);
static void app_main_observer_sensor_update(uint32_t now_ms);
static void app_main_observer_update(void);
static void app_main_process_current_mode(void);
static uint8_t app_main_light_raw_to_percent(uint32_t raw_value);
static bool app_main_set_lamp_brightness(uint8_t brightness_percent);

void app_main_request_mode(app_mode_t mode)
{
    uint32_t interrupt_state;

    if ((APP_MODE_IDLE != mode) &&
        (APP_MODE_OBSERVER != mode) &&
        (APP_MODE_TABLE_LAMP != mode))
    {
        return;
    }

    /* 模式值和“有新请求”标志必须成对更新，避免主循环读到一半状态。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_requested_mode = mode;
    s_mode_request_pending = true;
    __set_PRIMASK(interrupt_state);
}

app_mode_t app_main_get_mode(void)
{
    return s_app_mode;
}

bool app_main_request_chat_command(app_chat_command_t command)
{
    uint32_t interrupt_state;

    if ((command <= APP_CHAT_COMMAND_NONE) ||
        (command > APP_CHAT_COMMAND_SHAKE_HEAD))
    {
        return false;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_requested_chat_command = command;
    s_chat_command_request_pending = true;
    __set_PRIMASK(interrupt_state);
    return true;
}

void app_main_request_mode_toggle(void)
{
    /* 保留旧接口，便于尚未迁移的调用点继续使用。 */
    app_main_request_mode((APP_MODE_TABLE_LAMP == s_app_mode) ?
                          APP_MODE_OBSERVER : APP_MODE_TABLE_LAMP);
}

void app_main_set_auto_brightness(bool enabled)
{
    if (enabled && !s_auto_light_ready)
    {
        enabled = false;
    }

    s_auto_brightness_enabled = enabled;
    s_light_filter_valid = false;
    s_light_failure_count = 0U;
    s_light_failure_reported = false;

    if (enabled)
    {
        /* 重新开启自动调光后，在下一次台灯循环立即读取环境光。 */
        s_last_light_sample_ms = drv_gpt_timer_get_ms() - APP_LIGHT_SAMPLE_PERIOD_MS;
    }
    else
    {
        /* 关闭自动调光后，把UI保存的手动亮度交给主循环输出。 */
        s_manual_brightness_pending = true;
    }
}

bool app_main_get_auto_brightness(void)
{
    return s_auto_brightness_enabled;
}

bool app_main_is_auto_brightness_available(void)
{
    return s_auto_light_ready;
}

void app_main_set_manual_brightness(uint8_t brightness_percent)
{
    if (brightness_percent > APP_LIGHT_MAX_PERCENT)
    {
        brightness_percent = APP_LIGHT_MAX_PERCENT;
    }

    s_manual_brightness_percent = brightness_percent;
    s_manual_brightness_pending = true;
}

uint8_t app_main_get_lamp_brightness(void)
{
    return s_lamp_brightness_percent;
}

void app_main_set_observer_music_enabled(bool enabled)
{
    /* 只有桌宠页面可以开启音乐模式；离开桌宠时仍允许提交关闭请求。 */
    if (enabled && (APP_MODE_OBSERVER != s_app_mode))
    {
        return;
    }

    s_observer_music_enabled = enabled;
    s_observer_music_request_pending = true;
}

bool app_main_get_observer_music_enabled(void)
{
    return s_observer_music_enabled;
}

void app_main_set_observer_lamp_enabled(bool enabled)
{
    /* 桌宠灯光开关不参与台灯模式的光敏电阻自动调光。 */
    if (enabled && (APP_MODE_OBSERVER != s_app_mode))
    {
        return;
    }

    s_observer_lamp_enabled = enabled;
    s_observer_lamp_request_pending = true;
}

bool app_main_get_observer_lamp_enabled(void)
{
    return s_observer_lamp_enabled;
}

static bool app_main_take_mode_request(app_mode_t * p_requested_mode)
{
    bool requested;
    uint32_t interrupt_state;

    if (NULL == p_requested_mode)
    {
        return false;
    }

    /* “读取并清零”作为一个整体完成，防止新请求被清零动作覆盖。 */
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    requested = s_mode_request_pending;
    *p_requested_mode = s_requested_mode;
    s_mode_request_pending = false;
    __set_PRIMASK(interrupt_state);

    return requested;
}

static bool app_main_take_chat_command(app_chat_command_t * command)
{
    bool requested;
    uint32_t interrupt_state;

    if (NULL == command)
    {
        return false;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    requested = s_chat_command_request_pending;
    *command = s_requested_chat_command;
    s_chat_command_request_pending = false;
    __set_PRIMASK(interrupt_state);
    return requested;
}

static bool app_main_voice_start(void)
{
    fsp_err_t err;

    if (s_voice_running)
    {
        return true;
    }

    /*
     * MUSIC_TEST_ENABLE 已在这块硬件上验证右声道有效。这里固定使用相同
     * 声道，避免AUTO把机械噪声更强的空时隙误选为麦克风信号。
     */
    Voice_ChannelSet(VOICE_CHANNEL_RIGHT);
    err = Voice_Start();
    if (FSP_SUCCESS == err)
    {
        s_voice_running = true;
    }
    else
    {
        /* 启动失败不进入致命错误，由桌宠调度器按限速周期继续重试。 */
        printf("Voice start failed; sound reactions disabled: %d\r\n", (int) err);
    }

    return s_voice_running;
}

static void app_main_voice_stop(void)
{
    fsp_err_t err;

    if (!s_voice_running)
    {
        return;
    }

    err = Voice_Stop();
    if (FSP_SUCCESS != err)
    {
        printf("Voice stop failed: %d\r\n", (int) err);
    }
    s_voice_running = false;
}

static void app_main_light_control_init(void)
{
    fsp_err_t bulb_err;
    fsp_err_t sensor_err;

    /* 灯泡驱动内部会先设置0%占空比再启动GPT3，防止上电闪亮。 */
    bulb_err = light_bulb_init();
    if (FSP_SUCCESS == bulb_err)
    {
        s_light_bulb_ready = true;
    }
    else
    {
        printf("Light bulb PWM init failed: %d\r\n", (int) bulb_err);
    }

    sensor_err = light_module_init();
    if (FSP_SUCCESS != sensor_err)
    {
        printf("Light ADC init failed: %d\r\n", (int) sensor_err);
    }

    s_auto_light_ready = s_light_bulb_ready && (FSP_SUCCESS == sensor_err);
    if (s_auto_light_ready)
    {
        printf("Automatic lamp brightness ready.\r\n");
    }
    else
    {
        s_auto_brightness_enabled = false;
        if (s_light_bulb_ready)
        {
            (void) light_bulb_set_brightness(0U);
        }
    }
}

static uint8_t app_main_light_raw_to_percent(uint32_t raw_value)
{
    uint32_t brightness_range;
    uint32_t brightness_percent;

    if (raw_value > APP_LIGHT_ADC_MAX_VALUE)
    {
        raw_value = APP_LIGHT_ADC_MAX_VALUE;
    }

    /* 当前模块在环境越暗时ADC值越大：明亮接近15%，黑暗接近100%。 */
    brightness_range = APP_LIGHT_MAX_PERCENT - APP_LIGHT_MIN_PERCENT;
    brightness_percent = APP_LIGHT_MIN_PERCENT +
                         ((raw_value * brightness_range) + (APP_LIGHT_ADC_MAX_VALUE / 2U)) /
                         APP_LIGHT_ADC_MAX_VALUE;

    return (uint8_t) brightness_percent;
}

static bool app_main_set_lamp_brightness(uint8_t brightness_percent)
{
    fsp_err_t err;

    if (!s_light_bulb_ready)
    {
        return false;
    }

    err = light_bulb_set_brightness(brightness_percent);
    if (FSP_SUCCESS != err)
    {
        if (!s_light_output_error_reported)
        {
            printf("Light bulb brightness set failed: %d\r\n", (int) err);
            s_light_output_error_reported = true;
        }
        return false;
    }

    s_light_output_error_reported = false;
    s_lamp_brightness_percent = brightness_percent;
    return true;
}

static void app_main_chat_servo_prepare(void)
{
    if (APP_MODE_IDLE != s_app_mode)
    {
        app_main_set_mode(APP_MODE_IDLE);
    }
    Servo_DampingMode_ProStop();
    Servo_NormalMode();
}

static void app_main_execute_chat_command(app_chat_command_t command)
{
    switch (command)
    {
        case APP_CHAT_COMMAND_LIGHT_ON:
            /* Chat control is temporary; keep the table-lamp brightness mode. */
            (void) app_main_set_lamp_brightness(APP_CHAT_LIGHT_ON_PERCENT);
            printf("[CHAT_CMD] LIGHT_ON brightness=%u%%\r\n",
                   (unsigned int) APP_CHAT_LIGHT_ON_PERCENT);
            break;

        case APP_CHAT_COMMAND_LIGHT_OFF:
            /* Do not overwrite the brightness restored on the next lamp entry. */
            (void) app_main_set_lamp_brightness(0U);
            printf("[CHAT_CMD] LIGHT_OFF\r\n");
            break;

        case APP_CHAT_COMMAND_STAND:
            app_main_chat_servo_prepare();
            s_chat_servo_action = APP_CHAT_SERVO_ACTION_STAND;
            s_chat_servo_step = 0U;
            s_chat_servo_due_ms = drv_gpt_timer_get_ms();
            printf("[CHAT_CMD] STAND queued\r\n");
            break;

        case APP_CHAT_COMMAND_SIT:
            app_main_chat_servo_prepare();
            (void) Servo_SyncWritePos(s_chat_sit_step1_ids,
                                      (uint8_t) (sizeof(s_chat_sit_step1_ids) /
                                                 sizeof(s_chat_sit_step1_ids[0])),
                                      s_chat_sit_step1_positions,
                                      s_chat_sit_step1_speeds,
                                      s_chat_sit_step1_accelerations);
            s_chat_servo_action = APP_CHAT_SERVO_ACTION_SIT_FINAL;
            s_chat_servo_step = 0U;
            s_chat_servo_due_ms = drv_gpt_timer_get_ms() + APP_CHAT_SIT_FINAL_DELAY_MS;
            printf("[CHAT_CMD] SIT step 1 sent\r\n");
            break;

        case APP_CHAT_COMMAND_SHAKE_HEAD:
        {
            int current_position;
            int counterclockwise_position;
            int clockwise_position;

            app_main_chat_servo_prepare();
            current_position = Servo_ReadPosition(4U);
            if ((current_position >= SERVO_STS_POSITION_MIN) &&
                (current_position <= SERVO_STS_POSITION_MAX))
            {
                s_chat_servo4_restore_position = (int16_t) current_position;
            }
            else
            {
                s_chat_servo4_restore_position = 1266;
            }

            counterclockwise_position =
                s_chat_servo4_restore_position - APP_CHAT_SHAKE_AMPLITUDE;
            clockwise_position =
                s_chat_servo4_restore_position + APP_CHAT_SHAKE_AMPLITUDE;
            if (counterclockwise_position < APP_CHAT_SHAKE_MIN_POSITION)
            {
                counterclockwise_position = APP_CHAT_SHAKE_MIN_POSITION;
            }
            if (clockwise_position > APP_CHAT_SHAKE_MAX_POSITION)
            {
                clockwise_position = APP_CHAT_SHAKE_MAX_POSITION;
            }
            s_chat_shake_positions[0] = (int16_t) counterclockwise_position;
            s_chat_shake_positions[1] = (int16_t) clockwise_position;

            s_chat_servo_action = APP_CHAT_SERVO_ACTION_SHAKE_HEAD;
            s_chat_servo_step = 0U;
            s_chat_servo_due_ms = drv_gpt_timer_get_ms();
            printf("[CHAT_CMD] SHAKE_HEAD queued: center=%d, ccw=%d, cw=%d\r\n",
                   s_chat_servo4_restore_position,
                   s_chat_shake_positions[0],
                   s_chat_shake_positions[1]);
            break;
        }

        case APP_CHAT_COMMAND_NONE:
        default:
            break;
    }
}

static void app_main_chat_servo_update(void)
{
    uint32_t const now_ms = drv_gpt_timer_get_ms();

    if ((APP_CHAT_SERVO_ACTION_NONE == s_chat_servo_action) ||
        ((int32_t) (now_ms - s_chat_servo_due_ms) < 0))
    {
        return;
    }

    switch (s_chat_servo_action)
    {
        case APP_CHAT_SERVO_ACTION_STAND:
            if (s_chat_servo_step < 5U)
            {
                uint8_t const servo_id = s_chat_servo_step + 1U;
                (void) Servo_WritePos(servo_id,
                                      s_chat_stand_positions[s_chat_servo_step],
                                      s_chat_stand_speeds[s_chat_servo_step],
                                      s_chat_stand_accelerations[s_chat_servo_step]);
                s_chat_servo_step++;
                s_chat_servo_due_ms = now_ms +
                    ((4U == s_chat_servo_step) ?
                     APP_CHAT_STAND_HEAD_DELAY_MS : APP_CHAT_SERVO_STEP_MS);
            }
            if (s_chat_servo_step >= 5U)
            {
                s_chat_servo_action = APP_CHAT_SERVO_ACTION_NONE;
                printf("[CHAT_CMD] STAND complete\r\n");
            }
            break;

        case APP_CHAT_SERVO_ACTION_SIT_FINAL:
            (void) Servo_SyncWritePos(s_chat_sit_ids,
                                      (uint8_t) (sizeof(s_chat_sit_ids) /
                                                 sizeof(s_chat_sit_ids[0])),
                                      s_chat_sit_positions,
                                      s_chat_sit_speeds,
                                      s_chat_sit_accelerations);
            s_chat_servo_action = APP_CHAT_SERVO_ACTION_NONE;
            printf("[CHAT_CMD] SIT complete\r\n");
            break;

        case APP_CHAT_SERVO_ACTION_SHAKE_HEAD:
            if (s_chat_servo_step <
                (uint8_t) (sizeof(s_chat_shake_positions) /
                           sizeof(s_chat_shake_positions[0])))
            {
                int const write_result =
                    Servo_WritePos(4U,
                                   s_chat_shake_positions[s_chat_servo_step],
                                   1000U,
                                   18U);

                if (write_result)
                {
                    printf("[CHAT_CMD] SHAKE_HEAD step=%u target=%d\r\n",
                           (unsigned int) s_chat_servo_step,
                           s_chat_shake_positions[s_chat_servo_step]);
                    s_chat_servo_step++;
                    s_chat_servo_due_ms = now_ms +
                        ((1U == s_chat_servo_step) ?
                         APP_CHAT_SHAKE_HALF_SWEEP_MS :
                         APP_CHAT_SHAKE_FULL_SWEEP_MS);
                }
                else
                {
                    s_chat_servo_due_ms = now_ms + APP_CHAT_SHAKE_RETRY_MS;
                    printf("[CHAT_CMD] SHAKE_HEAD step=%u write failed, retry\r\n",
                           (unsigned int) s_chat_servo_step);
                }
            }
            else
            {
                if (Servo_WritePos(4U,
                                   s_chat_servo4_restore_position,
                                   500U,
                                   15U))
                {
                    s_chat_servo_action = APP_CHAT_SERVO_ACTION_NONE;
                    printf("[CHAT_CMD] SHAKE_HEAD complete: restore=%d\r\n",
                           s_chat_servo4_restore_position);
                }
                else
                {
                    s_chat_servo_due_ms = now_ms + APP_CHAT_SHAKE_RETRY_MS;
                    printf("[CHAT_CMD] SHAKE_HEAD restore write failed, retry\r\n");
                }
            }
            break;

        case APP_CHAT_SERVO_ACTION_NONE:
        default:
            s_chat_servo_action = APP_CHAT_SERVO_ACTION_NONE;
            break;
    }
}

static void app_main_table_lamp_update(void)
{
    bool first_valid_sample;
    uint8_t target_percent;
    uint8_t output_percent;
    uint8_t difference;
    uint16_t raw_value;
    uint32_t now_ms;
    fsp_err_t err;

    if (!s_auto_brightness_enabled)
    {
        if (s_manual_brightness_pending)
        {
            if (app_main_set_lamp_brightness(s_manual_brightness_percent))
            {
                s_manual_brightness_pending = false;
            }
        }
        return;
    }

    if (!s_auto_light_ready)
    {
        return;
    }

    now_ms = drv_gpt_timer_get_ms();
    if ((now_ms - s_last_light_sample_ms) < APP_LIGHT_SAMPLE_PERIOD_MS)
    {
        return;
    }
    s_last_light_sample_ms = now_ms;

    err = light_module_read_raw(&raw_value);
    if (FSP_SUCCESS != err)
    {
        if (s_light_failure_count < APP_LIGHT_MAX_CONSECUTIVE_FAILURES)
        {
            s_light_failure_count++;
        }

        if ((s_light_failure_count >= APP_LIGHT_MAX_CONSECUTIVE_FAILURES) &&
            (!s_light_failure_reported))
        {
            (void) app_main_set_lamp_brightness(0U);
            printf("Light ADC read failed repeatedly; lamp turned off: %d\r\n", (int) err);
            s_light_failure_reported = true;
            s_light_filter_valid = false;
        }
        return;
    }

    if (s_light_failure_reported)
    {
        printf("Light ADC sampling recovered.\r\n");
    }
    s_light_failure_count = 0U;
    s_light_failure_reported = false;

    first_valid_sample = !s_light_filter_valid;
    if (first_valid_sample)
    {
        s_filtered_light_raw = raw_value;
        s_light_filter_valid = true;
    }
    else
    {
        /* 一阶指数滤波：7/8旧值加1/8新值，削弱ADC和灯光反馈噪声。 */
        s_filtered_light_raw = ((s_filtered_light_raw * 7U) + raw_value + 4U) / 8U;
    }

    target_percent = app_main_light_raw_to_percent(s_filtered_light_raw);
    output_percent = target_percent;

    if (!first_valid_sample)
    {
        difference = (target_percent >= s_lamp_brightness_percent) ?
                     (uint8_t) (target_percent - s_lamp_brightness_percent) :
                     (uint8_t) (s_lamp_brightness_percent - target_percent);

        if (difference < APP_LIGHT_DEADBAND_PERCENT)
        {
            return;
        }

        if (target_percent > (uint8_t) (s_lamp_brightness_percent + APP_LIGHT_MAX_STEP_PERCENT))
        {
            output_percent = (uint8_t) (s_lamp_brightness_percent + APP_LIGHT_MAX_STEP_PERCENT);
        }
        else if ((target_percent + APP_LIGHT_MAX_STEP_PERCENT) < s_lamp_brightness_percent)
        {
            output_percent = (uint8_t) (s_lamp_brightness_percent - APP_LIGHT_MAX_STEP_PERCENT);
        }
    }

    (void) app_main_set_lamp_brightness(output_percent);
}

static void app_main_table_lamp_servo_update(void)
{
    uint32_t now_ms = drv_gpt_timer_get_ms();

    if (!s_table_lamp_pose_ready)
    {
        if ((now_ms - s_table_lamp_pose_start_ms) < APP_TABLE_LAMP_POSE_SETTLE_MS)
        {
            return;
        }

        /*
         * 必须等展开动作基本结束后再解除保持力矩；如果进入模式时立即切到
         * 阻尼状态，舵机会停在展开途中，表现为用户还需要手动“抬头”。
         */
        /*
         * 进入增强阻尼控制：1、4、5号进入阻尼，2、3号继续提供支撑扭矩。
         * 不能调用Servo_DampingMode()，因为它会把承重的2、3号也直接设为阻尼。
         */
        Servo_DampingMode_Pro();
        s_table_lamp_pose_ready = true;
        s_last_table_lamp_servo_ms = now_ms;
        return;
    }

    if ((now_ms - s_last_table_lamp_servo_ms) >= APP_TABLE_LAMP_SERVO_PERIOD_MS)
    {
        s_last_table_lamp_servo_ms = now_ms;
        Servo_DampingMode_Pro();
    }
}

/**
 * @brief 按桌宠状态机的需求互斥切换CEU摄像头采集和I2S麦克风。
 */
static void app_main_observer_sensor_update(uint32_t now_ms)
{
    bool hearing_required = ObserverMode_IsHearingRequired();
#if APP_OBSERVER_VISION_ENABLE
    bool const vision_required = s_ai_available &&
                                 ObserverMode_IsVisionRequired();

    if (vision_required)
    {
        /* 恢复摄像头前必须先关麦克风，保证两条采集链路不重叠。 */
        app_main_voice_stop();
        s_observer_hearing_required = false;

        if (!s_observer_camera_enabled &&
            ((int32_t) (now_ms - s_observer_camera_retry_after_ms) >= 0))
        {
            if (ai_book_face_set_camera_enabled(true))
            {
                s_observer_camera_enabled = true;
                s_observer_camera_retry_after_ms = 0U;
            }
            else
            {
                s_observer_camera_retry_after_ms =
                    now_ms + APP_CAMERA_STATE_RETRY_MS;
            }
        }
        return;
    }

    if (s_observer_camera_enabled &&
        ((int32_t) (now_ms - s_observer_camera_retry_after_ms) >= 0))
    {
        if (ai_book_face_set_camera_enabled(false))
        {
            s_observer_camera_enabled = false;
            s_observer_camera_retry_after_ms = 0U;
        }
        else
        {
            s_observer_camera_retry_after_ms =
                now_ms + APP_CAMERA_STATE_RETRY_MS;
        }
    }

    /* CEU尚未成功暂停时不能启动麦克风。 */
    if (s_observer_camera_enabled)
    {
        hearing_required = false;
    }
#endif

    if (!hearing_required)
    {
        app_main_voice_stop();
        s_observer_hearing_required = false;
        return;
    }

    if (!s_observer_hearing_required &&
        ((int32_t) (now_ms - s_observer_voice_retry_after_ms) >= 0))
    {
        if (app_main_voice_start())
        {
            /* 只有驱动实际启动成功后，才缓存“听觉已开启”。 */
            s_observer_hearing_required = true;
        }
        else
        {
            s_observer_voice_retry_after_ms =
                now_ms + APP_VOICE_START_RETRY_MS;
        }
    }
}

/**
 * @brief Run one cooperative observer-mode scheduling slice.
 *
 * Servo updates surround the short audio and LVGL tasks so a display refresh
 * cannot postpone the next motion command for a complete main-loop cycle.
 */
static void app_main_observer_update(void)
{
    uint32_t const now_ms = drv_gpt_timer_get_ms();
#if APP_OBSERVER_VISION_ENABLE
    bool inference_complete = false;
#endif

    ObserverMode_Update();

    /* I2S若因运行时错误自行停下，清除软件缓存，让后面的限速重试接管。 */
    if (s_voice_running && !Voice_Running())
    {
        s_voice_running = false;
        s_observer_hearing_required = false;
    }

    /*
     * 进入桌宠模式后，先让舵机展开并静置2秒；确认余振消失后才启动麦克风
     * 完成一次底噪校准。校准结束后普通待机只运行视觉；用户开启音乐模式
     * 时暂停视觉并单独启动麦克风，两条链路不会同时运行。
     */
    app_main_observer_sensor_update(now_ms);
    ObserverMode_AudioUpdate();

    /* 音频处理可能完成校准并切到视觉待机，立即再次互斥切换传感器。 */
    app_main_observer_sensor_update(drv_gpt_timer_get_ms());

#if APP_OBSERVER_VISION_ENABLE
    if (s_ai_available && s_observer_camera_enabled &&
        ObserverMode_IsVisionRequired())
    {
        inference_complete = ai_book_face_poll();
        if (inference_complete)
        {
            observer_behavior_t const behavior = ObserverMode_GetBehavior();
            ai_book_face_detection_t detection;

            if (OBSERVER_BEHAVIOR_TRACKING == behavior)
            {
                /*
                 * 跟踪期间只取人脸框中心；无脸则提交无效帧。状态机从
                 * 首次丢脸开始计时，连续4秒未重新发现人脸才结束跟踪。
                 */
                bool const has_face =
                    ai_book_face_get_best_detection(AI_BOOK_FACE_CLASS_FACE, &detection);
                ObserverMode_SubmitTrackingFrame(
                    has_face,
                    has_face ? (int16_t) ((detection.x_min + detection.x_max) / 2) : 0,
                    has_face ? (int16_t) ((detection.y_min + detection.y_max) / 2) : 0);
            }
            else
            {
                /*
                 * 待机阶段只检测人脸；书本忽略（既不点头也不跟踪）。
                 * 音乐模式开启后IsVisionRequired()会立即关闭这条视觉链路。
                 */
                observer_visual_target_t target = OBSERVER_VISUAL_TARGET_NONE;
                if (ai_book_face_get_best_detection(AI_BOOK_FACE_CLASS_FACE, &detection))
                {
                    target = OBSERVER_VISUAL_TARGET_FACE;
                }
                ObserverMode_SubmitVisionResult(target);
            }
        }
    }
#endif

    /* The observer page only redraws when its expression or status changes. */
    app_lvgl_process();
    ObserverMode_Update();

    if (!s_observer_music_request_pending)
    {
        /* 校准超时等内部状态变化也要同步回按钮，避免UI显示与实际链路不一致。 */
        s_observer_music_enabled = ObserverMode_IsMusicModeEnabled();
    }
}

static void app_main_set_mode(app_mode_t next_mode)
{
    app_mode_t previous_mode;
    uint32_t now_ms;

    if (next_mode == s_app_mode)
    {
        return;
    }

    if (APP_MODE_IDLE != next_mode)
    {
        s_chat_servo_action = APP_CHAT_SERVO_ACTION_NONE;
    }

    previous_mode = s_app_mode;

    /* 先释放旧模式占用的功能。 */
    if (APP_MODE_OBSERVER == previous_mode)
    {
        /* 离开桌宠时同时撤销页面上的音乐和灯光请求。 */
        s_observer_music_enabled = false;
        s_observer_music_request_pending = false;
        s_observer_lamp_enabled = false;
        s_observer_lamp_request_pending = false;
        ObserverMode_SetMusicModeEnabled(false);
        ObserverMode_Stop();
        app_main_voice_stop();
        s_observer_hearing_required = false;
        s_observer_voice_retry_after_ms = 0U;
#if APP_OBSERVER_VISION_ENABLE
        if (s_ai_available && s_observer_camera_enabled &&
            ai_book_face_set_camera_enabled(false))
        {
            s_observer_camera_enabled = false;
        }
        s_observer_camera_retry_after_ms = 0U;
#endif
        (void) app_main_set_lamp_brightness(0U);
    }
    else if (APP_MODE_TABLE_LAMP == previous_mode)
    {
        Servo_DampingMode_ProStop();
        s_table_lamp_pose_ready = false;
        (void) app_main_set_lamp_brightness(0U);
        s_light_filter_valid = false;
    }
    s_app_mode = next_mode;

    switch (s_app_mode)
    {
        case APP_MODE_TABLE_LAMP:
            app_background_set_ai_enabled(false);
            app_main_voice_stop();
            app_lvgl_set_process_period(APP_LVGL_NORMAL_PERIOD_MS);

            /*
             * Always clear the enhanced-damping state before a new table-lamp
             * entry. This prevents an interrupted previous transition from
             * making the first periodic update reuse stale joint state.
             */
            Servo_DampingMode_ProStop();

            /* 先恢复位置模式并展开；到位后由周期函数切换到阻尼模式。 */
            Servo_NormalMode();
            Servo_Power_on();

            now_ms = drv_gpt_timer_get_ms();
            s_table_lamp_pose_start_ms = now_ms;
            s_table_lamp_pose_ready = false;
            s_last_light_sample_ms = now_ms - APP_LIGHT_SAMPLE_PERIOD_MS;
            s_last_table_lamp_servo_ms = now_ms - APP_TABLE_LAMP_SERVO_PERIOD_MS;
            s_light_filter_valid = false;
            s_light_failure_count = 0U;
            s_light_failure_reported = false;
            s_lamp_brightness_percent = 0U;
            s_manual_brightness_pending = true;
            (void) app_main_set_lamp_brightness(0U);

            break;

        case APP_MODE_OBSERVER:
            /* 先展开并静置2秒，再进行一次底噪校准；校准期间不执行舵机动作。 */
            app_background_set_ai_enabled(false);
#if APP_OBSERVER_VISION_ENABLE
            /* 主程序不输出每个检测框，避免同步UART日志阻塞舵机更新。 */
            ai_book_face_set_reporting_enabled(false);
#endif
            s_observer_hearing_required = false;
            s_observer_voice_retry_after_ms = 0U;
#if APP_OBSERVER_VISION_ENABLE
            s_observer_camera_retry_after_ms = 0U;
#endif
            s_observer_music_enabled = false;
            s_observer_music_request_pending = false;
            s_observer_lamp_enabled = false;
            s_observer_lamp_request_pending = false;
            if (!app_main_set_lamp_brightness(0U))
            {
                s_observer_lamp_enabled = (s_lamp_brightness_percent > 0U);
            }
            ObserverMode_Start();
            app_lvgl_set_process_period(APP_LVGL_OBSERVER_PERIOD_MS);
            printf("Observer mode selected.\r\n");
            break;

        case APP_MODE_IDLE:
        default:
            app_background_set_ai_enabled(false);
            app_main_voice_stop();
            app_lvgl_set_process_period(APP_LVGL_NORMAL_PERIOD_MS);

            /* 从工作或桌宠页面返回待机时，由触摸界面请求安全收灯。 */
            if ((APP_MODE_TABLE_LAMP == previous_mode) ||
                (APP_MODE_OBSERVER == previous_mode))
            {
                Servo_Power_off();
            }
            printf("Standby mode selected.\r\n");
            break;
    }
}

static void app_main_process_observer_control_requests(void)
{
    if (s_observer_music_request_pending)
    {
        bool const enabled = s_observer_music_enabled &&
                             (APP_MODE_OBSERVER == s_app_mode);

        s_observer_music_request_pending = false;
        ObserverMode_SetMusicModeEnabled(enabled);
        s_observer_music_enabled = ObserverMode_IsMusicModeEnabled();
    }

    if (s_observer_lamp_request_pending)
    {
        bool const enabled = s_observer_lamp_enabled &&
                             (APP_MODE_OBSERVER == s_app_mode);

        s_observer_lamp_request_pending = false;
        if (!app_main_set_lamp_brightness(enabled ?
                                          APP_OBSERVER_LAMP_ON_PERCENT : 0U))
        {
            /* PWM写入失败时恢复为上一次真正成功输出的灯光状态。 */
            s_observer_lamp_enabled = (s_lamp_brightness_percent > 0U);
        }
    }
}

static void app_main_process_requests(void)
{
    app_mode_t requested_mode;
    app_chat_command_t chat_command;

    if (app_main_take_chat_command(&chat_command))
    {
        app_main_execute_chat_command(chat_command);
    }

    if (app_main_take_mode_request(&requested_mode))
    {
        app_main_set_mode(requested_mode);
    }

    /* 模式请求优先，防止离开桌宠的同一轮又执行旧的页面按钮请求。 */
    app_main_process_observer_control_requests();
}

static void app_main_process_current_mode(void)
{
    app_main_chat_servo_update();

    switch (s_app_mode)
    {
        case APP_MODE_OBSERVER:
            /*
             * AI推理和LVGL刷新可能占用一段连续时间，因此处理前后各检查一次
             * 舵机时间片，使阻塞结束后能够立即补上下一次动作更新。
             */
            app_main_observer_update();
            R_BSP_SoftwareDelay(2U, BSP_DELAY_UNITS_MILLISECONDS);
            break;

        case APP_MODE_TABLE_LAMP:
            /*
             * Check the servo transition before and after potentially lengthy
             * LVGL, network, and display work. Once the 1200 ms settling time
             * expires, enhanced damping is therefore applied without waiting
             * for another complete background cycle.
             */
            app_main_table_lamp_servo_update();
            app_background_process();
            app_main_table_lamp_update();
            app_main_table_lamp_servo_update();
            R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);
            break;

        case APP_MODE_IDLE:
        default:
            app_background_process();
            R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);
            break;
    }
}

void app_main_run(void)
{
    fsp_err_t err;

#if APP_OBSERVER_VISION_ENABLE
    /* 软件SCCB配置摄像头时对中断抖动敏感，因此在LVGL定时器前初始化。 */
    s_ai_available = ai_book_face_init();
    if (!s_ai_available)
    {
        printf("Book/face AI init failed; object detection disabled.\r\n");
    }
    else
    {
        /* 主页面和台灯模式不需要取帧；进入桌宠视觉状态时再恢复CEU。 */
        s_observer_camera_enabled = true;
        if (ai_book_face_set_camera_enabled(false))
        {
            s_observer_camera_enabled = false;
        }
    }
#else
    /* 音乐专项诊断不启动摄像头和NPU，保证音频链路与独立测试接近。 */
    printf("Music diagnostic: camera and NPU initialization skipped.\r\n");
#endif

    if (!time_start())
    {
        printf("Time start failed; the LVGL clock will show RTC ERROR.\r\n");
    }

    Servo_Init();
    R_BSP_SoftwareDelay(1000U, BSP_DELAY_UNITS_MILLISECONDS);
    app_main_light_control_init();

    /* 待机页面不需要持续运行AI和麦克风。 */
    app_background_set_ai_enabled(false);

    err = app_lvgl_init();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("LVGL application initialization", err);
    }

    while (1)
    {
        app_main_process_requests();
        app_main_process_current_mode();
    }
}
