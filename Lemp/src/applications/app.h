#ifndef APPLICATIONS_APP_H
#define APPLICATIONS_APP_H

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/** 主程序运行模式，由LVGL触摸界面提交切换请求。 */
typedef enum e_app_mode
{
    APP_MODE_IDLE       = 0U,
    APP_MODE_OBSERVER   = 3U,
    APP_MODE_TABLE_LAMP = 4U
} app_mode_t;

/** 云端实时聊天识别后提交给主循环的设备控制命令。 */
typedef enum e_app_chat_command
{
    APP_CHAT_COMMAND_NONE = 0U,
    APP_CHAT_COMMAND_LIGHT_ON,
    APP_CHAT_COMMAND_LIGHT_OFF,
    APP_CHAT_COMMAND_STAND,
    APP_CHAT_COMMAND_SIT,
    APP_CHAT_COMMAND_SHAKE_HEAD
} app_chat_command_t;

/***********************************************************************************************************************
 * Exported global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Exported global functions (to be accessed by other files)
 **********************************************************************************************************************/

void app_i2c_touchpad_test(void);
void app_spi_display_test(void);
fsp_err_t app_lvgl_init(void);
void app_lvgl_process(void);
void app_lvgl_set_process_period(uint32_t period_ms);
void app_lvgl_test(void);

void app_main_run(void);
/**
 * @brief 请求主循环切换到指定模式。
 *
 * 该函数只提交请求，可以由LVGL按钮回调安全调用；
 * 实际的ADC、PWM和舵机操作由主循环完成。
 */
void app_main_request_mode(app_mode_t mode);

/** @brief 获取主程序当前已经生效的模式。 */
app_mode_t app_main_get_mode(void);

/** @brief 提交一条云端聊天控制命令，由主循环安全执行。 */
bool app_main_request_chat_command(app_chat_command_t command);

/** @brief 兼容旧入口：在台灯模式和观察者模式之间切换。 */
void app_main_request_mode_toggle(void);

/** @brief 开启或关闭光敏电阻自动调光。 */
void app_main_set_auto_brightness(bool enabled);

/** @brief 查询自动调光开关状态。 */
bool app_main_get_auto_brightness(void);

/** @brief 查询光敏电阻和灯泡PWM是否都已就绪。 */
bool app_main_is_auto_brightness_available(void);

/** @brief 设置手动灯泡亮度，参数范围为0～100。 */
void app_main_set_manual_brightness(uint8_t brightness_percent);

/** @brief 获取灯泡当前已经输出的亮度百分比。 */
uint8_t app_main_get_lamp_brightness(void);

/**
 * @brief 请求开启或关闭桌宠音乐模式。
 *
 * 开启后由主循环停止摄像头并启动麦克风音乐识别；
 * 关闭后由主循环停止音乐识别并恢复摄像头。
 */
void app_main_set_observer_music_enabled(bool enabled);

/** @brief 查询桌宠音乐模式当前的开关状态。 */
bool app_main_get_observer_music_enabled(void);

/**
 * @brief 请求开启或关闭桌宠页面的外接灯泡。
 *
 * 开启时由主循环输出90%亮度，关闭时输出0%亮度。
 */
void app_main_set_observer_lamp_enabled(bool enabled);

/** @brief 查询桌宠页面外接灯泡当前的开关状态。 */
bool app_main_get_observer_lamp_enabled(void);

void app_mic_uart_export_test(void);
void app_music_beat_test(void);
void app_music_fft_test(void);
/** @brief Run the standalone onset, beat-tracking, and servo-dance test. */
void app_music_onset_test(void);
void app_camera_lcd_preview_test(void);
void app_rtc_test(void);
void app_test_flash(void);
void app_adc_light_test(void);
void app_led_blink_test(void);

void app_fatal_error(char const * operation, fsp_err_t err) __attribute__((noreturn));
void app_background_set_ai_enabled(bool enabled);
void app_background_process(void);
void app_background_delay(uint32_t milliseconds);

void Light_bulb_test(void);


#endif /* APPLICATIONS_APP_H */
