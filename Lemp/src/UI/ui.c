#include "UI/ui.h"

#include "Alarm/alarm_clock.h"
#include "Chat/chat_service.h"
#include "Doubao/doubao_realtime.h"
#include "Esp/esp_time_sync.h"
#include "Middlewares/lvgl/lvgl.h"
#include "Real_time/time.h"
#include "Screen/drv_spi_display.h"
#include "Servo/observer_mode.h"
#include "Weather/weather_client.h"
#include "applications/app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

LV_FONT_DECLARE(ui_font_chinese_16);

#define UI_SCREEN_WIDTH                     (320)
#define UI_SCREEN_HEIGHT                    (480)
#define UI_TRANSITION_TIME_MS               (280U)
#define UI_STATE_TIMER_PERIOD_MS            (100U)
#define UI_NETWORK_TIMER_PERIOD_MS           (10U)
#define UI_WORK_REMINDER_INTERVAL_MS      (10000U)
#define UI_FACE_BLINK_INTERVAL_MS          (2600U)
#define UI_FACE_BLINK_TIME_MS               (120U)
/* 观察者模式优先保证AI和机械动作流畅，暂时关闭会周期性触发刷屏的自动眨眼。 */
#define UI_OBSERVER_BLINK_ENABLE               (0U)

typedef enum e_ui_page
{
    UI_PAGE_HOME = 0,
    UI_PAGE_TABLE_LAMP,
    UI_PAGE_OBSERVER,
    UI_PAGE_ALARM,
    UI_PAGE_WEATHER,
    UI_PAGE_CHAT
} ui_page_t;

/* ---- 页面对象 ---- */
static lv_obj_t * s_home_screen;
static lv_obj_t * s_table_lamp_screen;
static lv_obj_t * s_observer_screen;
static lv_obj_t * s_alarm_screen;
static lv_obj_t * s_weather_screen;
static lv_obj_t * s_chat_screen;

/* ---- 待机页面对象 ---- */
static lv_obj_t * s_time_label;
static lv_obj_t * s_date_label;
static lv_obj_t * s_weekday_label;
static lv_obj_t * s_screen_brightness_value_label;
static lv_obj_t * s_home_weather_value_label;

/* ---- 台灯工作页面对象 ---- */
static lv_obj_t * s_work_timer_arc;
static lv_obj_t * s_work_time_label;
static lv_obj_t * s_work_toggle_label;
static lv_obj_t * s_work_status_label;
static lv_obj_t * s_auto_brightness_switch;
static lv_obj_t * s_auto_brightness_hint;
static lv_obj_t * s_manual_brightness_panel;
static lv_obj_t * s_manual_brightness_slider;
static lv_obj_t * s_manual_brightness_value_label;
static lv_obj_t * s_lamp_output_label;
static lv_obj_t * s_reminder_overlay;

/* ---- 观察者表情页面对象 ---- */
static lv_obj_t * s_face_plate;
static lv_obj_t * s_face_left_eye;
static lv_obj_t * s_face_right_eye;
static lv_obj_t * s_face_left_pupil;
static lv_obj_t * s_face_right_pupil;
static lv_obj_t * s_face_left_brow;
static lv_obj_t * s_face_right_brow;
static lv_obj_t * s_face_smile;
static lv_obj_t * s_face_flat_mouth;
static lv_obj_t * s_face_round_mouth;
static lv_obj_t * s_face_expression_label;
static lv_obj_t * s_face_action_label;
static lv_obj_t * s_face_bpm_label;
static lv_obj_t * s_observer_music_button;
static lv_obj_t * s_observer_music_button_label;
static lv_obj_t * s_observer_lamp_button;
static lv_obj_t * s_observer_lamp_button_label;

/* ---- 天气页面对象 ---- */
static lv_obj_t * s_weather_city_label;
static lv_obj_t * s_weather_temperature_label;
static lv_obj_t * s_weather_wind_label;
static lv_obj_t * s_weather_status_label;
static lv_obj_t * s_weather_condition_labels[WEATHER_FORECAST_DAY_COUNT];
static lv_obj_t * s_weather_temperature_labels[WEATHER_FORECAST_DAY_COUNT];

/* ---- 实时聊天页面对象 ---- */
static lv_obj_t * s_chat_state_label;
static lv_obj_t * s_chat_mic_label;
static lv_obj_t * s_chat_hint_label;
static lv_obj_t * s_chat_stats_label;
static lv_obj_t * s_chat_indicator;

/* ---- 页面运行状态 ---- */
static ui_page_t s_current_page = UI_PAGE_HOME;
static app_mode_t s_last_app_mode = APP_MODE_IDLE;

static bool     s_work_timer_running;
static bool     s_reminder_visible;
static uint32_t s_work_elapsed_ms;
static uint32_t s_work_last_tick_ms;
static uint32_t s_work_next_reminder_ms;

static bool                  s_face_blinking;
static uint32_t              s_face_blink_end_ms;
static uint32_t              s_face_next_blink_ms;
static observer_expression_t s_last_face_expression = (observer_expression_t) 0xFF;
static uint8_t               s_last_face_action = UINT8_MAX;
static uint16_t              s_last_face_bpm = UINT16_MAX;
static bool                  s_observer_controls_valid;
static bool                  s_last_observer_music_enabled;
static bool                  s_last_observer_lamp_enabled;
static bool                  s_weather_client_ready;
static bool                  s_weather_request_waiting;

static void home_screen_create(void);
static void table_lamp_screen_create(void);
static void observer_screen_create(void);
static void alarm_screen_create(void);
static void weather_screen_create(void);
static void chat_screen_create(void);

static lv_obj_t * panel_create(lv_obj_t * parent, lv_coord_t width, lv_coord_t height,
                               lv_color_t color, lv_opa_t opacity, lv_coord_t radius);
static lv_obj_t * chinese_label_create(lv_obj_t * parent, const char * text);
static lv_obj_t * text_button_create(lv_obj_t * parent, const char * text,
                                     lv_coord_t x, lv_coord_t y,
                                     lv_coord_t width, lv_coord_t height,
                                     lv_color_t color, lv_event_cb_t event_cb);
static lv_obj_t * back_button_create(lv_obj_t * parent, lv_event_cb_t event_cb);
static lv_obj_t * cloud_create(lv_obj_t * parent, lv_coord_t y, lv_opa_t opacity);

static void home_background_create(lv_obj_t * screen);
static void ui_load_page(ui_page_t page, lv_scr_load_anim_t animation);
static void work_session_start(void);
static void work_timer_update(void);
static void work_running_set(bool running);
static void work_brightness_visibility_update(void);
static void work_reminder_show(void);
static void work_reminder_hide(void);
static void observer_face_update(void);
static void observer_face_apply(observer_expression_t expression, uint8_t action_index);
static void observer_face_blink_start(uint32_t now_ms);
static void observer_face_blink_finish(uint32_t now_ms);
static void observer_controls_update(void);

static void clock_timer_cb(lv_timer_t * timer);
static void ui_state_timer_cb(lv_timer_t * timer);
static void network_timer_cb(lv_timer_t * timer);
static void cloud_x_anim_cb(void * object, int32_t value);
static void face_y_anim_cb(void * object, int32_t value);

static void home_table_lamp_event_cb(lv_event_t * event);
static void home_observer_event_cb(lv_event_t * event);
static void home_alarm_event_cb(lv_event_t * event);
static void home_weather_event_cb(lv_event_t * event);
static void home_chat_event_cb(lv_event_t * event);
static void chat_mic_event_cb(lv_event_t * event);
static void common_home_event_cb(lv_event_t * event);
static void table_lamp_back_event_cb(lv_event_t * event);
static void observer_back_event_cb(lv_event_t * event);
static void observer_music_event_cb(lv_event_t * event);
static void observer_lamp_event_cb(lv_event_t * event);
static void screen_brightness_event_cb(lv_event_t * event);
static void work_toggle_event_cb(lv_event_t * event);
static void auto_brightness_event_cb(lv_event_t * event);
static void manual_brightness_event_cb(lv_event_t * event);
static void reminder_rest_event_cb(lv_event_t * event);
static void reminder_later_event_cb(lv_event_t * event);
static void weather_refresh_event_cb(lv_event_t * event);
static void weather_request_start(void);
static void weather_poll(void);
static void weather_display_update(weather_data_t const * weather);
static void chat_status_update(void);

static lv_obj_t * panel_create(lv_obj_t * parent, lv_coord_t width, lv_coord_t height,
                               lv_color_t color, lv_opa_t opacity, lv_coord_t radius)
{
    lv_obj_t * panel = lv_obj_create(parent);

    lv_obj_set_size(panel, width, height);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_bg_color(panel, color, 0);
    lv_obj_set_style_bg_opa(panel, opacity, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    return panel;
}

static lv_obj_t * chinese_label_create(lv_obj_t * parent, const char * text)
{
    lv_obj_t * label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &ui_font_chinese_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);

    return label;
}

static lv_obj_t * text_button_create(lv_obj_t * parent, const char * text,
                                     lv_coord_t x, lv_coord_t y,
                                     lv_coord_t width, lv_coord_t height,
                                     lv_color_t color, lv_event_cb_t event_cb)
{
    lv_obj_t * button = lv_btn_create(parent);
    lv_obj_t * label;

    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_shadow_width(button, 18, 0);
    lv_obj_set_style_shadow_color(button, color, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
    lv_obj_set_style_transform_zoom(button, 248, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, NULL);

    label = chinese_label_create(button, text);
    lv_obj_center(label);

    return button;
}

static lv_obj_t * back_button_create(lv_obj_t * parent, lv_event_cb_t event_cb)
{
    lv_obj_t * button = lv_btn_create(parent);
    lv_obj_t * label;

    lv_obj_set_size(button, 76, 38);
    lv_obj_set_pos(button, 12, 12);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), 0);
    /* LVGL 8.3 仅提供以 10% 为步进的透明度枚举，取最接近的 20%。 */
    lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_transform_zoom(button, 246, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, NULL);

    label = chinese_label_create(button, LV_SYMBOL_LEFT " 返回");
    lv_obj_center(label);

    return button;
}

static lv_obj_t * cloud_create(lv_obj_t * parent, lv_coord_t y, lv_opa_t opacity)
{
    lv_obj_t * cloud = panel_create(parent, 105, 42, lv_color_white(), LV_OPA_TRANSP, 0);
    lv_obj_t * base;
    lv_obj_t * left;
    lv_obj_t * middle;
    lv_obj_t * right;

    lv_obj_set_pos(cloud, -110, y);

    base = panel_create(cloud, 72, 18, lv_color_white(), opacity, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(base, 16, 20);
    left = panel_create(cloud, 32, 32, lv_color_white(), opacity, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(left, 20, 8);
    middle = panel_create(cloud, 42, 42, lv_color_white(), opacity, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(middle, 41, 0);
    right = panel_create(cloud, 28, 28, lv_color_white(), opacity, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(right, 68, 11);

    return cloud;
}

static void cloud_x_anim_cb(void * object, int32_t value)
{
    lv_obj_set_x((lv_obj_t *) object, (lv_coord_t) value);
}

static void face_y_anim_cb(void * object, int32_t value)
{
    lv_obj_set_y((lv_obj_t *) object, (lv_coord_t) value);
}

static void home_background_create(lv_obj_t * screen)
{
    lv_obj_t * sun;
    lv_obj_t * cloud_1;
    lv_obj_t * cloud_2;
    lv_obj_t * far_hill;
    lv_obj_t * near_hill;
    lv_anim_t animation;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x315B8A), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0xE8A35A), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    sun = panel_create(screen, 58, 58, lv_color_hex(0xFFD66B), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(sun, 238, 54);
    lv_obj_set_style_shadow_width(sun, 28, 0);
    lv_obj_set_style_shadow_color(sun, lv_color_hex(0xFFD66B), 0);
    lv_obj_set_style_shadow_opa(sun, LV_OPA_40, 0);

    cloud_1 = cloud_create(screen, 76, LV_OPA_60);
    cloud_2 = cloud_create(screen, 148, LV_OPA_30);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, cloud_1);
    lv_anim_set_exec_cb(&animation, cloud_x_anim_cb);
    lv_anim_set_values(&animation, -110, 325);
    lv_anim_set_time(&animation, 18000);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, cloud_2);
    lv_anim_set_exec_cb(&animation, cloud_x_anim_cb);
    lv_anim_set_values(&animation, -150, 325);
    lv_anim_set_time(&animation, 26000);
    lv_anim_set_delay(&animation, 3500);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);

    far_hill = panel_create(screen, 390, 170, lv_color_hex(0x5E8A66), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(far_hill, -130, 332);
    near_hill = panel_create(screen, 390, 165, lv_color_hex(0x315D4A), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(near_hill, 75, 350);
}

static void home_screen_create(void)
{
    lv_obj_t * weather_text;
    lv_obj_t * clock_card;
    lv_obj_t * mode_card;
    lv_obj_t * mode_title;
    lv_obj_t * brightness_card;
    lv_obj_t * brightness_title;
    lv_obj_t * brightness_slider;

    s_home_screen = lv_obj_create(NULL);
    home_background_create(s_home_screen);

    s_date_label = lv_label_create(s_home_screen);
    lv_label_set_text(s_date_label, "---- -- --");
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_white(), 0);
    lv_obj_set_pos(s_date_label, 16, 16);

    weather_text = chinese_label_create(s_home_screen, "晴");
    lv_obj_align(weather_text, LV_ALIGN_TOP_RIGHT, -62, 15);

    s_home_weather_value_label = lv_label_create(s_home_screen);
    lv_label_set_text(s_home_weather_value_label, "-- C");
    lv_obj_set_style_text_font(s_home_weather_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_home_weather_value_label, lv_color_white(), 0);
    lv_obj_align(s_home_weather_value_label, LV_ALIGN_TOP_RIGHT, -14, 15);

    clock_card = panel_create(s_home_screen, 282, 118, lv_color_hex(0x14283B), LV_OPA_40, 24);
    lv_obj_align(clock_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_border_width(clock_card, 1, 0);
    lv_obj_set_style_border_color(clock_card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(clock_card, LV_OPA_20, 0);

    s_time_label = lv_label_create(clock_card);
    lv_label_set_text(s_time_label, "--:--:--");
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -10);

    s_weekday_label = chinese_label_create(clock_card, "星期--");
    lv_obj_set_style_text_color(s_weekday_label, lv_color_hex(0xD6E8F6), 0);
    lv_obj_align(s_weekday_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    mode_card = panel_create(s_home_screen, 292, 202, lv_color_hex(0x10251F), LV_OPA_80, 22);
    lv_obj_align(mode_card, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_style_border_width(mode_card, 1, 0);
    lv_obj_set_style_border_color(mode_card, lv_color_hex(0xA5D4C5), 0);
    lv_obj_set_style_border_opa(mode_card, LV_OPA_20, 0);

    mode_title = chinese_label_create(mode_card, "选择功能");
    lv_obj_set_style_text_color(mode_title, lv_color_hex(0xDCEBE5), 0);
    lv_obj_align(mode_title, LV_ALIGN_TOP_MID, 0, 11);

    (void) text_button_create(mode_card, "台灯模式", 12, 34, 128, 44,
                              lv_color_hex(0xD99B35), home_table_lamp_event_cb);
    (void) text_button_create(mode_card, "桌宠模式", 152, 34, 128, 44,
                              lv_color_hex(0x397C91), home_observer_event_cb);
    (void) text_button_create(mode_card, "闹钟", 12, 86, 128, 44,
                              lv_color_hex(0x5A547D), home_alarm_event_cb);
    (void) text_button_create(mode_card, "天气", 152, 86, 128, 44,
                              lv_color_hex(0x4C7668), home_weather_event_cb);
    (void) text_button_create(mode_card, "实时聊天", 12, 138, 268, 44,
                              lv_color_hex(0x76518E), home_chat_event_cb);

    brightness_card = panel_create(s_home_screen, 292, 70, lv_color_hex(0x142B2A), LV_OPA_90, 20);
    lv_obj_align(brightness_card, LV_ALIGN_BOTTOM_MID, 0, -7);

    brightness_title = chinese_label_create(brightness_card, "屏幕亮度");
    lv_obj_set_pos(brightness_title, 16, 13);

    brightness_slider = lv_slider_create(brightness_card);
    lv_obj_set_size(brightness_slider, 198, 12);
    lv_obj_set_pos(brightness_slider, 16, 47);
    lv_slider_set_range(brightness_slider, 15, 100);
    lv_slider_set_value(brightness_slider, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0x58736D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_hex(0xFFD36A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(brightness_slider, screen_brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_screen_brightness_value_label = lv_label_create(brightness_card);
    lv_label_set_text(s_screen_brightness_value_label, "60%");
    lv_obj_set_style_text_font(s_screen_brightness_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_screen_brightness_value_label, lv_color_white(), 0);
    lv_obj_align(s_screen_brightness_value_label, LV_ALIGN_RIGHT_MID, -16, 13);
}

static void table_lamp_screen_create(void)
{
    lv_obj_t * title;
    lv_obj_t * timer_glow;
    lv_obj_t * toggle_button;
    lv_obj_t * control_card;
    lv_obj_t * auto_title;
    lv_obj_t * manual_title;
    lv_obj_t * reminder_card;
    lv_obj_t * reminder_title;
    lv_obj_t * reminder_text;

    s_table_lamp_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_table_lamp_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_table_lamp_screen, lv_color_hex(0x0D1A20), 0);
    lv_obj_set_style_bg_grad_color(s_table_lamp_screen, lv_color_hex(0x24483D), 0);
    lv_obj_set_style_bg_grad_dir(s_table_lamp_screen, LV_GRAD_DIR_VER, 0);

    (void) back_button_create(s_table_lamp_screen, table_lamp_back_event_cb);
    title = chinese_label_create(s_table_lamp_screen, "专注工作");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 23);

    timer_glow = panel_create(s_table_lamp_screen, 246, 246,
                              lv_color_hex(0xF4B85A), LV_OPA_10, LV_RADIUS_CIRCLE);
    lv_obj_align(timer_glow, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_shadow_width(timer_glow, 35, 0);
    lv_obj_set_style_shadow_color(timer_glow, lv_color_hex(0xF4B85A), 0);
    lv_obj_set_style_shadow_opa(timer_glow, LV_OPA_20, 0);

    s_work_timer_arc = lv_arc_create(s_table_lamp_screen);
    lv_obj_set_size(s_work_timer_arc, 226, 226);
    lv_obj_align(s_work_timer_arc, LV_ALIGN_TOP_MID, 0, 68);
    lv_arc_set_rotation(s_work_timer_arc, 270);
    lv_arc_set_bg_angles(s_work_timer_arc, 0, 360);
    lv_arc_set_range(s_work_timer_arc, 0, 60);
    lv_arc_set_value(s_work_timer_arc, 0);
    lv_obj_set_style_arc_width(s_work_timer_arc, 13, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_work_timer_arc, lv_color_hex(0x34544D), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_work_timer_arc, 13, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_work_timer_arc, lv_color_hex(0xFFD36A), LV_PART_INDICATOR);
    lv_obj_remove_style(s_work_timer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_work_timer_arc, LV_OBJ_FLAG_CLICKABLE);

    s_work_time_label = lv_label_create(s_table_lamp_screen);
    lv_label_set_text(s_work_time_label, "00:00");
    lv_obj_set_style_text_font(s_work_time_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_work_time_label, lv_color_white(), 0);
    lv_obj_align(s_work_time_label, LV_ALIGN_TOP_MID, 0, 125);

    toggle_button = lv_btn_create(s_table_lamp_screen);
    lv_obj_set_size(toggle_button, 58, 58);
    lv_obj_align(toggle_button, LV_ALIGN_TOP_MID, 0, 205);
    lv_obj_set_style_radius(toggle_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(toggle_button, lv_color_hex(0xF0B64D), 0);
    lv_obj_set_style_shadow_width(toggle_button, 20, 0);
    lv_obj_set_style_shadow_color(toggle_button, lv_color_hex(0xF0B64D), 0);
    lv_obj_set_style_shadow_opa(toggle_button, LV_OPA_30, 0);
    lv_obj_set_style_transform_zoom(toggle_button, 244, LV_STATE_PRESSED);
    lv_obj_add_event_cb(toggle_button, work_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    s_work_toggle_label = lv_label_create(toggle_button);
    lv_label_set_text(s_work_toggle_label, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(s_work_toggle_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_work_toggle_label, lv_color_hex(0x24301E), 0);
    lv_obj_center(s_work_toggle_label);

    s_work_status_label = chinese_label_create(s_table_lamp_screen, "工作中");
    lv_obj_set_style_text_color(s_work_status_label, lv_color_hex(0xC7DED5), 0);
    lv_obj_align(s_work_status_label, LV_ALIGN_TOP_MID, 0, 286);

    control_card = panel_create(s_table_lamp_screen, 292, 146,
                                lv_color_hex(0x142A29), LV_OPA_COVER, 22);
    lv_obj_align(control_card, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_border_width(control_card, 1, 0);
    lv_obj_set_style_border_color(control_card, lv_color_hex(0x5D8076), 0);
    lv_obj_set_style_border_opa(control_card, LV_OPA_30, 0);

    auto_title = chinese_label_create(control_card, "自动调光");
    lv_obj_set_pos(auto_title, 16, 14);

    s_auto_brightness_switch = lv_switch_create(control_card);
    lv_obj_set_size(s_auto_brightness_switch, 50, 27);
    lv_obj_align(s_auto_brightness_switch, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_add_event_cb(s_auto_brightness_switch, auto_brightness_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_lamp_output_label = lv_label_create(control_card);
    lv_label_set_text(s_lamp_output_label, "0%");
    lv_obj_set_style_text_font(s_lamp_output_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lamp_output_label, lv_color_hex(0xFFD36A), 0);
    lv_obj_align(s_lamp_output_label, LV_ALIGN_TOP_RIGHT, -78, 16);

    s_auto_brightness_hint = chinese_label_create(control_card, "光敏电阻正在调节灯光");
    lv_obj_set_style_text_color(s_auto_brightness_hint, lv_color_hex(0x9FC2B7), 0);
    lv_obj_align(s_auto_brightness_hint, LV_ALIGN_CENTER, 0, 25);

    s_manual_brightness_panel = panel_create(control_card, 264, 76,
                                             lv_color_hex(0x203C37), LV_OPA_COVER, 16);
    lv_obj_align(s_manual_brightness_panel, LV_ALIGN_BOTTOM_MID, 0, -8);

    manual_title = chinese_label_create(s_manual_brightness_panel, "手动亮度");
    lv_obj_set_pos(manual_title, 12, 10);

    s_manual_brightness_slider = lv_slider_create(s_manual_brightness_panel);
    lv_obj_set_size(s_manual_brightness_slider, 184, 12);
    lv_obj_set_pos(s_manual_brightness_slider, 12, 49);
    lv_slider_set_range(s_manual_brightness_slider, 0, 100);
    lv_slider_set_value(s_manual_brightness_slider, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_manual_brightness_slider, lv_color_hex(0x607B74), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_manual_brightness_slider, lv_color_hex(0xFFD36A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_manual_brightness_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(s_manual_brightness_slider, manual_brightness_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_manual_brightness_value_label = lv_label_create(s_manual_brightness_panel);
    lv_label_set_text(s_manual_brightness_value_label, "60%");
    lv_obj_set_style_text_font(s_manual_brightness_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_manual_brightness_value_label, lv_color_white(), 0);
    lv_obj_align(s_manual_brightness_value_label, LV_ALIGN_RIGHT_MID, -13, 15);

    /* 提醒层最后创建，因此显示时会遮住工作页面上的其他控件。 */
    s_reminder_overlay = panel_create(s_table_lamp_screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT,
                                      lv_color_black(), LV_OPA_70, 0);
    lv_obj_set_pos(s_reminder_overlay, 0, 0);
    lv_obj_add_flag(s_reminder_overlay, LV_OBJ_FLAG_HIDDEN);

    reminder_card = panel_create(s_reminder_overlay, 282, 252,
                                 lv_color_hex(0xF5F0E5), LV_OPA_COVER, 28);
    lv_obj_center(reminder_card);
    lv_obj_set_style_shadow_width(reminder_card, 38, 0);
    lv_obj_set_style_shadow_color(reminder_card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(reminder_card, LV_OPA_30, 0);

    reminder_title = chinese_label_create(reminder_card, "休息一下吧");
    lv_obj_set_style_text_color(reminder_title, lv_color_hex(0x334640), 0);
    lv_obj_align(reminder_title, LV_ALIGN_TOP_MID, 0, 28);

    reminder_text = chinese_label_create(reminder_card, "喝杯水，活动一下身体");
    lv_obj_set_style_text_color(reminder_text, lv_color_hex(0x60736C), 0);
    lv_obj_align(reminder_text, LV_ALIGN_TOP_MID, 0, 74);

    (void) text_button_create(reminder_card, "开始休息", 19, 142, 116, 58,
                              lv_color_hex(0x4A8B72), reminder_rest_event_cb);
    (void) text_button_create(reminder_card, "稍后提醒", 147, 142, 116, 58,
                              lv_color_hex(0xBA8B44), reminder_later_event_cb);
}

static void observer_screen_create(void)
{
    lv_obj_t * title;
    lv_obj_t * expression_card;
    lv_obj_t * footer;
    lv_anim_t animation;

    s_observer_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_observer_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_observer_screen, lv_color_hex(0x101522), 0);
    lv_obj_set_style_bg_grad_color(s_observer_screen, lv_color_hex(0x263857), 0);
    lv_obj_set_style_bg_grad_dir(s_observer_screen, LV_GRAD_DIR_VER, 0);

    (void) back_button_create(s_observer_screen, observer_back_event_cb);
    title = chinese_label_create(s_observer_screen, "桌宠模式");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 23);

    /*
     * 桌宠默认只启用摄像头。用户主动打开“音频”后，主循环才切换到
     * 音乐识别；灯光按钮只提交外接灯泡的90%/0%开关请求。
     */
    s_observer_music_button = text_button_create(s_observer_screen, "音频：关",
                                                  96, 58, 100, 34,
                                                  lv_color_hex(0x34435D),
                                                  observer_music_event_cb);
    s_observer_music_button_label = lv_obj_get_child(s_observer_music_button, 0);

    s_observer_lamp_button = text_button_create(s_observer_screen, "灯光：关",
                                                 204, 58, 104, 34,
                                                 lv_color_hex(0x34435D),
                                                 observer_lamp_event_cb);
    s_observer_lamp_button_label = lv_obj_get_child(s_observer_lamp_button, 0);

    s_face_plate = panel_create(s_observer_screen, 240, 240,
                                lv_color_hex(0xF2C45E), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_plate, 40, 98);
    lv_obj_set_style_shadow_width(s_face_plate, 38, 0);
    lv_obj_set_style_shadow_color(s_face_plate, lv_color_hex(0x91B9FF), 0);
    lv_obj_set_style_shadow_opa(s_face_plate, LV_OPA_20, 0);

    s_face_left_brow = panel_create(s_face_plate, 54, 7,
                                    lv_color_hex(0x29313B), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_left_brow, 40, 50);
    s_face_right_brow = panel_create(s_face_plate, 54, 7,
                                     lv_color_hex(0x29313B), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_right_brow, 146, 50);

    s_face_left_eye = panel_create(s_face_plate, 56, 72,
                                   lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_left_eye, 42, 70);
    s_face_right_eye = panel_create(s_face_plate, 56, 72,
                                    lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_right_eye, 142, 70);

    s_face_left_pupil = panel_create(s_face_left_eye, 20, 25,
                                     lv_color_hex(0x273442), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_left_pupil, 18, 24);
    s_face_right_pupil = panel_create(s_face_right_eye, 20, 25,
                                      lv_color_hex(0x273442), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_right_pupil, 18, 24);

    s_face_smile = lv_arc_create(s_face_plate);
    lv_obj_set_size(s_face_smile, 78, 48);
    lv_obj_set_pos(s_face_smile, 81, 150);
    lv_arc_set_angles(s_face_smile, 20, 160);
    lv_obj_set_style_arc_opa(s_face_smile, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_face_smile, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_face_smile, lv_color_hex(0x50392A), LV_PART_INDICATOR);
    lv_obj_remove_style(s_face_smile, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_face_smile, LV_OBJ_FLAG_CLICKABLE);

    s_face_flat_mouth = panel_create(s_face_plate, 52, 7,
                                     lv_color_hex(0x50392A), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_flat_mouth, 94, 185);

    s_face_round_mouth = panel_create(s_face_plate, 30, 38,
                                      lv_color_hex(0x50392A), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(s_face_round_mouth, 105, 166);

    expression_card = panel_create(s_observer_screen, 286, 82,
                                   lv_color_hex(0x17243A), LV_OPA_90, 20);
    lv_obj_align(expression_card, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_border_width(expression_card, 1, 0);
    lv_obj_set_style_border_color(expression_card, lv_color_hex(0x607A9F), 0);
    lv_obj_set_style_border_opa(expression_card, LV_OPA_30, 0);

    s_face_expression_label = chinese_label_create(expression_card, "平静观察");
    lv_obj_set_style_text_color(s_face_expression_label, lv_color_hex(0xE3ECFF), 0);
    lv_obj_align(s_face_expression_label, LV_ALIGN_TOP_MID, 0, 15);

    s_face_action_label = lv_label_create(expression_card);
    lv_label_set_text(s_face_action_label, "ACTION 01 / 43");
    lv_obj_set_style_text_font(s_face_action_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_face_action_label, lv_color_hex(0x91A7C7), 0);
    lv_obj_align(s_face_action_label, LV_ALIGN_BOTTOM_MID, 0, -13);

    footer = chinese_label_create(s_observer_screen, "正在观察周围环境");
    lv_obj_set_style_text_color(footer, lv_color_hex(0x8399BA), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -15);

    /* 音乐锁拍后在桌宠页面右下角显示过滤后的稳定BPM。 */
    s_face_bpm_label = lv_label_create(s_observer_screen);
    lv_label_set_text(s_face_bpm_label, "0 BPM");
    lv_obj_set_style_text_font(s_face_bpm_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_face_bpm_label, lv_color_hex(0xFFD36A), 0);
    lv_obj_align(s_face_bpm_label, LV_ALIGN_BOTTOM_RIGHT, -12, -14);
    lv_obj_add_flag(s_face_bpm_label, LV_OBJ_FLAG_HIDDEN);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_face_plate);
    lv_anim_set_exec_cb(&animation, face_y_anim_cb);
    lv_anim_set_values(&animation, 98, 105);
    lv_anim_set_time(&animation, 1500);
    lv_anim_set_playback_time(&animation, 1500);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);

    observer_face_apply(OBSERVER_EXPRESSION_NEUTRAL, 0U);
    observer_controls_update();
}

static void alarm_screen_create(void)
{
    s_alarm_screen = alarm_clock_screen_create(common_home_event_cb);
}

static void weather_day_card_create(lv_obj_t * parent, lv_coord_t y,
                                    uint32_t day_index, const char * day)
{
    lv_obj_t * card = panel_create(parent, 272, 66, lv_color_hex(0xFFFFFF), LV_OPA_10, 16);
    lv_obj_t * day_label;
    lv_obj_t * condition_label;
    lv_obj_t * temp_label;

    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);

    day_label = chinese_label_create(card, day);
    lv_obj_set_pos(day_label, 18, 22);
    condition_label = chinese_label_create(card, "--");
    lv_obj_set_style_text_color(condition_label, lv_color_hex(0xDCEBFF), 0);
    lv_obj_align(condition_label, LV_ALIGN_CENTER, 0, 0);

    temp_label = lv_label_create(card);
    lv_label_set_text(temp_label, "-- / -- C");
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(temp_label, lv_color_white(), 0);
    lv_obj_align(temp_label, LV_ALIGN_RIGHT_MID, -18, 0);

    if (day_index < WEATHER_FORECAST_DAY_COUNT)
    {
        s_weather_condition_labels[day_index] = condition_label;
        s_weather_temperature_labels[day_index] = temp_label;
    }
}

static void weather_screen_create(void)
{
    lv_obj_t * title;
    lv_obj_t * refresh_button;

    s_weather_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_weather_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_weather_screen, lv_color_hex(0x183B58), 0);
    lv_obj_set_style_bg_grad_color(s_weather_screen, lv_color_hex(0x4D7FA3), 0);
    lv_obj_set_style_bg_grad_dir(s_weather_screen, LV_GRAD_DIR_VER, 0);

    (void) back_button_create(s_weather_screen, common_home_event_cb);
    title = chinese_label_create(s_weather_screen, "近期天气");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    refresh_button = text_button_create(s_weather_screen, "刷新", 244, 12, 64, 38,
                                        lv_color_hex(0x3C718F), weather_refresh_event_cb);
    lv_obj_set_style_radius(refresh_button, 14, 0);

    s_weather_city_label = chinese_label_create(s_weather_screen, "等待请求");
    lv_obj_set_style_text_color(s_weather_city_label, lv_color_hex(0xD7EAF7), 0);
    lv_obj_align(s_weather_city_label, LV_ALIGN_TOP_MID, 0, 55);

    s_weather_temperature_label = lv_label_create(s_weather_screen);
    lv_label_set_text(s_weather_temperature_label, "-- C");
    lv_obj_set_style_text_font(s_weather_temperature_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_weather_temperature_label, lv_color_white(), 0);
    lv_obj_align(s_weather_temperature_label, LV_ALIGN_TOP_MID, 0, 76);

    s_weather_wind_label = chinese_label_create(s_weather_screen, "风力 --");
    lv_obj_set_style_text_color(s_weather_wind_label, lv_color_hex(0xD7EAF7), 0);
    lv_obj_align(s_weather_wind_label, LV_ALIGN_TOP_MID, 0, 135);

    weather_day_card_create(s_weather_screen, 165, 0U, "今天");
    weather_day_card_create(s_weather_screen, 241, 1U, "明天");
    weather_day_card_create(s_weather_screen, 317, 2U, "后天");

    s_weather_status_label = chinese_label_create(s_weather_screen, "点击刷新获取天气");
    lv_obj_set_style_text_color(s_weather_status_label, lv_color_hex(0xC4D8E8), 0);
    lv_obj_align(s_weather_status_label, LV_ALIGN_BOTTOM_MID, 0, -21);
}

static void chat_screen_create(void)
{
    lv_obj_t * title;
    lv_obj_t * state_card;
    lv_obj_t * mic_card;
    lv_obj_t * button_label;

    s_chat_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_chat_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_chat_screen, lv_color_hex(0x130F24), 0);
    lv_obj_set_style_bg_grad_color(s_chat_screen, lv_color_hex(0x392D57), 0);
    lv_obj_set_style_bg_grad_dir(s_chat_screen, LV_GRAD_DIR_VER, 0);

    (void) back_button_create(s_chat_screen, common_home_event_cb);
    title = chinese_label_create(s_chat_screen, "实时聊天");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE9D8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    state_card = panel_create(s_chat_screen, 292, 92,
                              lv_color_hex(0x241B3A), LV_OPA_90, 20);
    lv_obj_set_pos(state_card, 14, 62);
    s_chat_state_label = chinese_label_create(state_card, "正在初始化...");
    lv_obj_set_style_text_color(s_chat_state_label, lv_color_hex(0xCDB7EA), 0);
    lv_obj_align(s_chat_state_label, LV_ALIGN_TOP_MID, 0, 17);
    s_chat_stats_label = lv_label_create(state_card);
    lv_label_set_text(s_chat_stats_label, "REC 0.0s  TX 0.0s");
    lv_obj_set_style_text_font(s_chat_stats_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_chat_stats_label, lv_color_hex(0x9F92B6), 0);
    lv_obj_align(s_chat_stats_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    mic_card = panel_create(s_chat_screen, 292, 236,
                            lv_color_hex(0x1C263B), LV_OPA_90, 24);
    lv_obj_set_pos(mic_card, 14, 168);

    /* Tap once to start streaming; tap again to stop and send EndASR. */
    s_chat_indicator = lv_obj_create(mic_card);
    lv_obj_set_size(s_chat_indicator, 116, 116);
    lv_obj_align(s_chat_indicator, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_radius(s_chat_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0x66527C), 0);
    lv_obj_set_style_bg_opa(s_chat_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chat_indicator, 0, 0);
    lv_obj_set_style_pad_all(s_chat_indicator, 0, 0);
    lv_obj_set_style_shadow_width(s_chat_indicator, 24, 0);
    lv_obj_set_style_shadow_color(s_chat_indicator, lv_color_hex(0x8C6BB1), 0);
    lv_obj_set_style_shadow_opa(s_chat_indicator, LV_OPA_30, 0);
    lv_obj_add_flag(s_chat_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_chat_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chat_indicator, chat_mic_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0x59466E), LV_STATE_PRESSED);

    s_chat_mic_label = chinese_label_create(s_chat_indicator, "等待");
    lv_obj_center(s_chat_mic_label);

    button_label = chinese_label_create(mic_card, "点击上方圆形按钮");
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xDCCBF0), 0);
    lv_obj_align(button_label, LV_ALIGN_BOTTOM_MID, 0, -58);

    s_chat_hint_label = chinese_label_create(mic_card,
                                             "点击一次开始录音，再点击一次停止并发送");
    lv_obj_set_width(s_chat_hint_label, 264);
    lv_label_set_long_mode(s_chat_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_chat_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_chat_hint_label, lv_color_hex(0xAFA4C2), 0);
    lv_obj_align(s_chat_hint_label, LV_ALIGN_BOTTOM_MID, 0, -18);

}

static lv_obj_t * ui_page_object_get(ui_page_t page)
{
    switch (page)
    {
        case UI_PAGE_TABLE_LAMP:
            return s_table_lamp_screen;
        case UI_PAGE_OBSERVER:
            return s_observer_screen;
        case UI_PAGE_ALARM:
            return s_alarm_screen;
        case UI_PAGE_WEATHER:
            return s_weather_screen;
        case UI_PAGE_CHAT:
            return s_chat_screen;
        case UI_PAGE_HOME:
        default:
            return s_home_screen;
    }
}

static void ui_load_page(ui_page_t page, lv_scr_load_anim_t animation)
{
    lv_obj_t * target_screen;

    if (page == s_current_page)
    {
        return;
    }

    target_screen = ui_page_object_get(page);
    if (NULL == target_screen)
    {
        return;
    }

    if ((UI_PAGE_CHAT == s_current_page) && (UI_PAGE_CHAT != page))
    {
        chat_service_leave(lv_tick_get());
    }

    lv_scr_load_anim(target_screen, animation, UI_TRANSITION_TIME_MS, 0U, false);
    s_current_page = page;
}

static void work_session_start(void)
{
    bool auto_enabled = app_main_get_auto_brightness();

    s_work_elapsed_ms = 0U;
    s_work_last_tick_ms = lv_tick_get();
    s_work_next_reminder_ms = UI_WORK_REMINDER_INTERVAL_MS;
    work_reminder_hide();
    work_running_set(true);

    if (auto_enabled)
    {
        lv_obj_add_state(s_auto_brightness_switch, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_clear_state(s_auto_brightness_switch, LV_STATE_CHECKED);
    }

    if (!app_main_is_auto_brightness_available())
    {
        lv_obj_add_state(s_auto_brightness_switch, LV_STATE_DISABLED);
        lv_obj_clear_state(s_auto_brightness_switch, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_clear_state(s_auto_brightness_switch, LV_STATE_DISABLED);
    }

    work_brightness_visibility_update();
    work_timer_update();

    /* 台灯模式开始即后台静默预热 WebSocket，使首次休息提醒可即时播报。 */
    (void) chat_service_prewarm(lv_tick_get());
}

static void work_running_set(bool running)
{
    s_work_timer_running = running;
    s_work_last_tick_ms = lv_tick_get();

    lv_label_set_text(s_work_toggle_label, running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(s_work_status_label, running ? "工作中" : "已暂停");
    lv_obj_set_style_text_color(s_work_status_label,
                                running ? lv_color_hex(0xC7DED5) : lv_color_hex(0xFFD36A),
                                0);
}

static void work_timer_update(void)
{
    uint32_t now_ms = lv_tick_get();
    uint32_t total_seconds;
    uint32_t minutes;
    uint32_t seconds;

    if (s_work_timer_running)
    {
        s_work_elapsed_ms += now_ms - s_work_last_tick_ms;
    }
    s_work_last_tick_ms = now_ms;

    total_seconds = s_work_elapsed_ms / 1000U;
    minutes = total_seconds / 60U;
    seconds = total_seconds % 60U;

    lv_label_set_text_fmt(s_work_time_label, "%02lu:%02lu",
                          (unsigned long) minutes,
                          (unsigned long) seconds);
    /* lv_arc_set_value() 的参数类型是 int16_t，且秒数已被限制在 0～60。 */
    lv_arc_set_value(s_work_timer_arc, (int16_t) seconds);

    if (s_work_timer_running && !s_reminder_visible &&
        (s_work_elapsed_ms >= s_work_next_reminder_ms))
    {
        work_reminder_show();
    }
}

static void work_brightness_visibility_update(void)
{
    bool auto_enabled = lv_obj_has_state(s_auto_brightness_switch, LV_STATE_CHECKED);

    if (auto_enabled)
    {
        lv_obj_clear_flag(s_auto_brightness_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_manual_brightness_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_auto_brightness_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_manual_brightness_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void work_reminder_show(void)
{
    if (s_reminder_visible)
    {
        return;
    }

    s_reminder_visible = true;
    lv_obj_clear_flag(s_reminder_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(s_reminder_overlay, 240U, 0U);

    /* 通过豆包云TTS播报语音休息提醒。 */
    if (!chat_service_play_rest_reminder(lv_tick_get()))
    {
        printf("[UI][LAMP] TTS rest reminder failed, screen-only fallback\r\n");
    }
}

static void work_reminder_hide(void)
{
    s_reminder_visible = false;
    if (NULL != s_reminder_overlay)
    {
        lv_obj_add_flag(s_reminder_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void observer_face_mouth_select(bool smile, bool flat, bool round)
{
    if (smile)
    {
        lv_obj_clear_flag(s_face_smile, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_face_smile, LV_OBJ_FLAG_HIDDEN);
    }

    if (flat)
    {
        lv_obj_clear_flag(s_face_flat_mouth, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_face_flat_mouth, LV_OBJ_FLAG_HIDDEN);
    }

    if (round)
    {
        lv_obj_clear_flag(s_face_round_mouth, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_face_round_mouth, LV_OBJ_FLAG_HIDDEN);
    }
}

static void observer_face_eye_restore(void)
{
    lv_obj_set_size(s_face_left_eye, 56, 72);
    lv_obj_set_pos(s_face_left_eye, 42, 70);
    lv_obj_set_size(s_face_right_eye, 56, 72);
    lv_obj_set_pos(s_face_right_eye, 142, 70);
    lv_obj_clear_flag(s_face_left_pupil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_face_right_pupil, LV_OBJ_FLAG_HIDDEN);
}

static void observer_face_apply(observer_expression_t expression, uint8_t action_index)
{
    const char * expression_text = "平静观察";
    lv_color_t face_color = lv_color_hex(0xF2C45E);
    lv_coord_t pupil_x = 18;
    lv_coord_t pupil_y = 24;
    int16_t left_brow_angle = -30;
    int16_t right_brow_angle = 30;
    bool smile = false;
    bool flat = true;
    bool round = false;

    observer_face_eye_restore();

    switch (expression)
    {
        case OBSERVER_EXPRESSION_HAPPY:
            expression_text = "开心放松";
            face_color = lv_color_hex(0xFFD46B);
            left_brow_angle = -60;
            right_brow_angle = 60;
            smile = true;
            flat = false;
            break;

        case OBSERVER_EXPRESSION_CURIOUS_LEFT:
            expression_text = "好奇地看向左边";
            face_color = lv_color_hex(0x8ED6C7);
            pupil_x = 7;
            left_brow_angle = -130;
            right_brow_angle = -20;
            break;

        case OBSERVER_EXPRESSION_CURIOUS_RIGHT:
            expression_text = "好奇地看向右边";
            face_color = lv_color_hex(0x8ED6C7);
            pupil_x = 29;
            left_brow_angle = 20;
            right_brow_angle = 130;
            break;

        case OBSERVER_EXPRESSION_FOCUSED:
            expression_text = "认真观察";
            face_color = lv_color_hex(0x87B7DF);
            pupil_y = 35;
            left_brow_angle = 90;
            right_brow_angle = -90;
            break;

        case OBSERVER_EXPRESSION_ALERT:
            expression_text = "发现情况，保持警惕";
            face_color = lv_color_hex(0xF19A74);
            pupil_y = 16;
            left_brow_angle = 150;
            right_brow_angle = -150;
            flat = false;
            round = true;
            break;

        case OBSERVER_EXPRESSION_LISTENING:
            expression_text = "正在仔细聆听";
            face_color = lv_color_hex(0xB7A0E3);
            pupil_x = (action_index < 32U) ? 7 : 29;
            left_brow_angle = -100;
            right_brow_angle = 100;
            flat = false;
            round = true;
            break;

        case OBSERVER_EXPRESSION_SLEEPING:
            expression_text = "正在安静休息";
            face_color = lv_color_hex(0x66758E);
            left_brow_angle = 0;
            right_brow_angle = 0;
            flat = false;
            smile = true;
            /* Collapse both eyes and hide their pupils to show sleep. */
            lv_obj_set_size(s_face_left_eye, 56, 7);
            lv_obj_set_pos(s_face_left_eye, 42, 103);
            lv_obj_set_size(s_face_right_eye, 56, 7);
            lv_obj_set_pos(s_face_right_eye, 142, 103);
            lv_obj_add_flag(s_face_left_pupil, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_face_right_pupil, LV_OBJ_FLAG_HIDDEN);
            break;

        case OBSERVER_EXPRESSION_RESPONDING:
            expression_text = "发现目标，正在回应";
            face_color = lv_color_hex(0xFFD46B);
            left_brow_angle = -60;
            right_brow_angle = 60;
            smile = true;
            flat = false;
            break;

        case OBSERVER_EXPRESSION_NEUTRAL:
        default:
            break;
    }

    lv_obj_set_style_bg_color(s_face_plate, face_color, 0);
    lv_obj_set_style_transform_angle(s_face_left_brow, left_brow_angle, 0);
    lv_obj_set_style_transform_angle(s_face_right_brow, right_brow_angle, 0);
    lv_obj_set_pos(s_face_left_pupil, pupil_x, pupil_y);
    lv_obj_set_pos(s_face_right_pupil, pupil_x, pupil_y);
    observer_face_mouth_select(smile, flat, round);

    lv_label_set_text(s_face_expression_label, expression_text);
    switch (ObserverMode_GetBehavior())
    {
        case OBSERVER_BEHAVIOR_CALIBRATING:
            /* 环境底噪校准期间舵机保持静止，提示用户当前启动阶段。 */
            lv_label_set_text(s_face_action_label, "AUDIO CALIBRATION");
            break;

        case OBSERVER_BEHAVIOR_TRACKING:
            lv_label_set_text(s_face_action_label, "FACE TRACKING");
            break;

        case OBSERVER_BEHAVIOR_NODDING:
            lv_label_set_text(s_face_action_label, "TARGET RESPONSE");
            break;

        case OBSERVER_BEHAVIOR_DANCING:
            /* 入场点头结束后直接使用完整幅度，不再显示渐增百分比。 */
            lv_label_set_text(s_face_action_label, "DANCING");
            break;

        case OBSERVER_BEHAVIOR_MUSIC_LISTENING:
            /* 音乐按钮已开启，但尚未锁定稳定节拍。 */
            lv_label_set_text(s_face_action_label, "MUSIC LISTENING");
            break;

        case OBSERVER_BEHAVIOR_GOING_TO_SLEEP:
            lv_label_set_text(s_face_action_label, "GOING TO SLEEP");
            break;

        case OBSERVER_BEHAVIOR_SLEEPING:
            lv_label_set_text(s_face_action_label, "HEARING ACTIVE");
            break;

        case OBSERVER_BEHAVIOR_WAKING:
            lv_label_set_text(s_face_action_label, "WAKING UP");
            break;

        case OBSERVER_BEHAVIOR_OBSERVING:
        default:
            lv_label_set_text_fmt(s_face_action_label, "ACTION %02u / %02u",
                                  (unsigned int) (action_index + 1U),
                                  (unsigned int) ObserverMode_GetActionCount());
            break;
    }
}

static void observer_controls_update(void)
{
    bool const music_enabled = app_main_get_observer_music_enabled();
    bool const lamp_enabled = app_main_get_observer_lamp_enabled();

    if ((NULL == s_observer_music_button) ||
        (NULL == s_observer_music_button_label) ||
        (NULL == s_observer_lamp_button) ||
        (NULL == s_observer_lamp_button_label))
    {
        return;
    }

    /* 只有开关状态实际变化时才刷新控件，避免桌宠页面周期性产生额外SPI刷新。 */
    if ((!s_observer_controls_valid) ||
        (music_enabled != s_last_observer_music_enabled))
    {
        lv_label_set_text(s_observer_music_button_label,
                          music_enabled ? "音频：开" : "音频：关");
        lv_obj_set_style_bg_color(s_observer_music_button,
                                  music_enabled ? lv_color_hex(0x76518E) :
                                                  lv_color_hex(0x34435D),
                                  0);
        lv_obj_set_style_shadow_color(s_observer_music_button,
                                      music_enabled ? lv_color_hex(0xB784CF) :
                                                      lv_color_hex(0x34435D),
                                      0);
        s_last_observer_music_enabled = music_enabled;
    }

    if ((!s_observer_controls_valid) ||
        (lamp_enabled != s_last_observer_lamp_enabled))
    {
        lv_label_set_text(s_observer_lamp_button_label,
                          lamp_enabled ? "灯光：开" : "灯光：关");
        lv_obj_set_style_bg_color(s_observer_lamp_button,
                                  lamp_enabled ? lv_color_hex(0xD99B35) :
                                                 lv_color_hex(0x34435D),
                                  0);
        lv_obj_set_style_shadow_color(s_observer_lamp_button,
                                      lamp_enabled ? lv_color_hex(0xFFD36A) :
                                                     lv_color_hex(0x34435D),
                                      0);
        s_last_observer_lamp_enabled = lamp_enabled;
    }

    s_observer_controls_valid = true;
}

static void observer_face_blink_start(uint32_t now_ms)
{
    s_face_blinking = true;
    s_face_blink_end_ms = now_ms + UI_FACE_BLINK_TIME_MS;

    lv_obj_set_size(s_face_left_eye, 56, 7);
    lv_obj_set_pos(s_face_left_eye, 42, 103);
    lv_obj_set_size(s_face_right_eye, 56, 7);
    lv_obj_set_pos(s_face_right_eye, 142, 103);
    lv_obj_add_flag(s_face_left_pupil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_face_right_pupil, LV_OBJ_FLAG_HIDDEN);
}

static void observer_face_blink_finish(uint32_t now_ms)
{
    s_face_blinking = false;
    s_face_next_blink_ms = now_ms + UI_FACE_BLINK_INTERVAL_MS +
                           ((uint32_t) (s_last_face_action % 4U) * 180U);
    observer_face_apply(s_last_face_expression, s_last_face_action);
}

static void observer_face_update(void)
{
    observer_expression_t expression = ObserverMode_GetExpression();
    uint8_t action_index = ObserverMode_GetActionIndex();
    uint16_t const bpm = ObserverMode_GetMusicBpm();
    uint32_t now_ms = lv_tick_get();

    if (bpm != s_last_face_bpm)
    {
        s_last_face_bpm = bpm;
        if (bpm > 0U)
        {
            lv_label_set_text_fmt(s_face_bpm_label, "%u BPM", (unsigned int) bpm);
            lv_obj_clear_flag(s_face_bpm_label, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_face_bpm_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if ((expression != s_last_face_expression) || (action_index != s_last_face_action))
    {
        s_last_face_expression = expression;
        s_last_face_action = action_index;
        if (!s_face_blinking)
        {
            observer_face_apply(expression, action_index);
        }
    }

    if (0U != UI_OBSERVER_BLINK_ENABLE)
    {
        if (s_face_blinking)
        {
            if ((int32_t) (now_ms - s_face_blink_end_ms) >= 0)
            {
                observer_face_blink_finish(now_ms);
            }
        }
        else if ((int32_t) (now_ms - s_face_next_blink_ms) >= 0)
        {
            observer_face_blink_start(now_ms);
        }
    }
}

static void clock_timer_cb(lv_timer_t * timer)
{
    static const char * weekdays[] =
    {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    rtc_time_t current_time;

    LV_UNUSED(timer);

    if (!time_get(&current_time))
    {
        lv_label_set_text(s_time_label, "--:--:--");
        lv_label_set_text(s_date_label, "---- -- --");
        lv_label_set_text(s_weekday_label, "RTC ERROR");
        return;
    }

    lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d",
                          current_time.tm_hour,
                          current_time.tm_min,
                          current_time.tm_sec);
    lv_label_set_text_fmt(s_date_label, "%04d-%02d-%02d",
                          current_time.tm_year + 1900,
                          current_time.tm_mon + 1,
                          current_time.tm_mday);

    if ((current_time.tm_wday >= 0) && (current_time.tm_wday < 7))
    {
        lv_label_set_text(s_weekday_label, weekdays[current_time.tm_wday]);
    }
    else
    {
        lv_label_set_text(s_weekday_label, "星期--");
    }
}

static void ui_state_timer_cb(lv_timer_t * timer)
{
    app_mode_t current_mode = app_main_get_mode();
    bool const mode_changed = (current_mode != s_last_app_mode);

    LV_UNUSED(timer);

    alarm_clock_poll();
    chat_status_update();

    /*
     * 页面不能只依赖“模式发生变化”来切换。模式请求可能在页面初始化前
     * 已经生效，或者快速来回切换后出现“后台仍是台灯模式、页面却在首页”
     * 的短暂不同步。此时再次请求同一模式不会改变模式值，旧逻辑将永远
     * 不再跳页。因此每次都核对稳定模式和当前页面，并在不一致时纠正。
     */
    if (APP_MODE_TABLE_LAMP == current_mode)
    {
        if (mode_changed || (UI_PAGE_TABLE_LAMP != s_current_page))
        {
            work_session_start();
            /*
             * 台灯页包含圆弧、阴影和半透明面板，淡入动画会连续触发多次
             * 全屏混合刷新。模式切换时直接加载，减少SPI/DMAC瞬时压力。
             */
            ui_load_page(UI_PAGE_TABLE_LAMP, LV_SCR_LOAD_ANIM_NONE);
        }
    }
    else if (APP_MODE_OBSERVER == current_mode)
    {
        if (mode_changed || (UI_PAGE_OBSERVER != s_current_page))
        {
            s_last_face_expression = (observer_expression_t) 0xFF;
            s_last_face_action = UINT8_MAX;
            s_last_face_bpm = UINT16_MAX;
            s_observer_controls_valid = false;
            s_face_blinking = false;
            s_face_next_blink_ms = lv_tick_get() + 900U;
            /* 全屏淡入会连续刷新大量像素；观察者模式直接切换以优先保证舵机流畅。 */
            ui_load_page(UI_PAGE_OBSERVER, LV_SCR_LOAD_ANIM_NONE);
        }
    }
    else if ((UI_PAGE_TABLE_LAMP == s_current_page) ||
             (UI_PAGE_OBSERVER == s_current_page))
    {
        s_work_timer_running = false;
        work_reminder_hide();
        chat_service_stop_prompt();
        chat_service_teardown_prewarm();
        /* 模式页面之间不叠加切屏动画，避免快速切换时出现半幅旧页面。 */
        ui_load_page(UI_PAGE_HOME, LV_SCR_LOAD_ANIM_NONE);
    }

    s_last_app_mode = current_mode;

    if (APP_MODE_TABLE_LAMP == current_mode)
    {
        work_timer_update();
        chat_service_prewarm_poll(lv_tick_get());
        lv_label_set_text_fmt(s_lamp_output_label, "%u%%",
                              (unsigned int) app_main_get_lamp_brightness());
    }
    else if (APP_MODE_OBSERVER == current_mode)
    {
        observer_controls_update();
        observer_face_update();
    }
}

static void network_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    weather_poll();
}

static void home_table_lamp_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    app_main_request_mode(APP_MODE_TABLE_LAMP);
}

static void home_observer_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    app_main_request_mode(APP_MODE_OBSERVER);
}

static void home_alarm_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_load_page(UI_PAGE_ALARM, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void home_weather_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_load_page(UI_PAGE_WEATHER, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    weather_request_start();
}

static void home_chat_event_cb(lv_event_t * event)
{
    uint32_t const now_ms = lv_tick_get();

    LV_UNUSED(event);
    /*
     * 进入聊天页前必须 teardown prewarm：
     * - prewarm 的 s_silent_session=true 会让聊天页握手完成后跳过自我介绍
     * - teardown 后 chat_service_enter 重新握手并恢复自动 SayHello 行为
     */
    chat_service_teardown_prewarm();
    ui_load_page(UI_PAGE_CHAT, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    if (!chat_service_enter(now_ms))
    {
        lv_label_set_text_fmt(s_chat_hint_label,
                              "初始化失败: %s",
                              chat_service_last_error());
        lv_obj_set_style_text_color(s_chat_hint_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
    chat_status_update();
}

static void chat_mic_event_cb(lv_event_t * event)
{
    doubao_realtime_state_t const state = DoubaoRealtime_State();
    bool enable;

    LV_UNUSED(event);
    if ((UI_PAGE_CHAT != s_current_page) ||
        (DOUBAO_STATE_READY != state) ||
        DoubaoRealtime_UploadingRecording())
    {
        return;
    }

    enable = !DoubaoRealtime_MicrophoneRequested();

    if (!chat_service_set_microphone(enable, lv_tick_get()))
    {
        printf("[UI][CHAT] screen button failed: %s\r\n",
               chat_service_last_error());
    }
    chat_status_update();
}

static void common_home_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_load_page(UI_PAGE_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void table_lamp_back_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    s_work_timer_running = false;
    work_reminder_hide();
    chat_service_stop_prompt();
    chat_service_teardown_prewarm();
    app_main_request_mode(APP_MODE_IDLE);
}

static void observer_back_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    app_main_request_mode(APP_MODE_IDLE);
}

static void observer_music_event_cb(lv_event_t * event)
{
    bool enable;

    LV_UNUSED(event);
    if ((UI_PAGE_OBSERVER != s_current_page) ||
        (APP_MODE_OBSERVER != app_main_get_mode()))
    {
        return;
    }

    /* LVGL只提交开关意图，摄像头与麦克风的互斥切换由主循环安全执行。 */
    enable = !app_main_get_observer_music_enabled();
    app_main_set_observer_music_enabled(enable);
    observer_controls_update();
}

static void observer_lamp_event_cb(lv_event_t * event)
{
    bool enable;

    LV_UNUSED(event);
    if ((UI_PAGE_OBSERVER != s_current_page) ||
        (APP_MODE_OBSERVER != app_main_get_mode()))
    {
        return;
    }

    /* 开启对应90%亮度，关闭对应0%；PWM操作仍交给主循环完成。 */
    enable = !app_main_get_observer_lamp_enabled();
    app_main_set_observer_lamp_enabled(enable);
    observer_controls_update();
}

static void screen_brightness_event_cb(lv_event_t * event)
{
    lv_obj_t * slider = lv_event_get_target(event);
    int32_t value = lv_slider_get_value(slider);
    fsp_err_t err;

    lv_label_set_text_fmt(s_screen_brightness_value_label, "%ld%%", (long) value);
    err = drv_spi_display_set_brightness((uint8_t) value);
    if (FSP_SUCCESS != err)
    {
        printf("[UI] Backlight PWM set failed: %d\r\n", (int) err);
    }
}

static void work_toggle_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);

    work_running_set(!s_work_timer_running);
    if (s_work_timer_running && (s_work_next_reminder_ms <= s_work_elapsed_ms))
    {
        s_work_next_reminder_ms = s_work_elapsed_ms + UI_WORK_REMINDER_INTERVAL_MS;
    }
}

static void auto_brightness_event_cb(lv_event_t * event)
{
    lv_obj_t * automatic_switch = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(automatic_switch, LV_STATE_CHECKED);

    app_main_set_auto_brightness(enabled);
    work_brightness_visibility_update();
}

static void manual_brightness_event_cb(lv_event_t * event)
{
    lv_obj_t * slider = lv_event_get_target(event);
    int32_t value = lv_slider_get_value(slider);

    lv_label_set_text_fmt(s_manual_brightness_value_label, "%ld%%", (long) value);
    app_main_set_manual_brightness((uint8_t) value);
}

static void reminder_rest_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);

    work_running_set(false);
    s_work_next_reminder_ms = s_work_elapsed_ms + UI_WORK_REMINDER_INTERVAL_MS;
    work_reminder_hide();
    chat_service_stop_prompt();
    lv_label_set_text(s_work_status_label, "休息中");
}

static void reminder_later_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);

    s_work_next_reminder_ms = s_work_elapsed_ms + UI_WORK_REMINDER_INTERVAL_MS;
    work_reminder_hide();
    chat_service_stop_prompt();
}

static void weather_display_update(weather_data_t const * weather)
{
    if (NULL == weather)
    {
        return;
    }

    lv_label_set_text(s_weather_city_label, weather->city);
    lv_label_set_text_fmt(s_weather_temperature_label, "%s C",
                          weather->current_temperature);
    lv_label_set_text_fmt(s_home_weather_value_label, "%s C",
                          weather->current_temperature);
    lv_label_set_text_fmt(s_weather_wind_label, "%s  %s",
                          weather->wind_direction,
                          weather->wind_force);

    for (uint32_t day = 0U; day < WEATHER_FORECAST_DAY_COUNT; day++)
    {
        lv_label_set_text(s_weather_condition_labels[day],
                          weather->forecast[day].condition);
        lv_label_set_text_fmt(s_weather_temperature_labels[day], "%s / %s C",
                              weather->forecast[day].high_temperature,
                              weather->forecast[day].low_temperature);
    }
    lv_label_set_text(s_weather_status_label, "天气更新完成");
    lv_obj_set_style_text_color(s_weather_status_label, lv_color_hex(0xBDECCF), 0);
}

static void weather_request_start(void)
{
    if (!s_weather_client_ready)
    {
        lv_label_set_text(s_weather_status_label, "天气串口不可用");
        lv_obj_set_style_text_color(s_weather_status_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
        return;
    }

    if (weather_client_request(lv_tick_get()))
    {
        s_weather_request_waiting = true;
        lv_label_set_text(s_weather_status_label, "正在请求天气...");
        lv_obj_set_style_text_color(s_weather_status_label, lv_color_hex(0xFFE09A), 0);
    }
    else if (weather_client_is_busy())
    {
        lv_label_set_text(s_weather_status_label, "天气请求进行中");
    }
    else
    {
        lv_label_set_text(s_weather_status_label, "请求过于频繁，请稍后");
    }
}

static void weather_poll(void)
{
    weather_data_t weather;

    if (!s_weather_client_ready)
    {
        return;
    }
    if (weather_client_take_data(&weather))
    {
        s_weather_request_waiting = false;
        weather_display_update(&weather);
    }
    else if (s_weather_request_waiting && !weather_client_is_busy())
    {
        char const * error = weather_client_last_error();
        s_weather_request_waiting = false;
        lv_label_set_text_fmt(s_weather_status_label, "请求失败: %s",
                              ((NULL != error) && ('\0' != error[0])) ? error : "UNKNOWN");
        lv_obj_set_style_text_color(s_weather_status_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
}

static void weather_refresh_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    weather_request_start();
}

static void chat_status_update(void)
{
    doubao_realtime_state_t state;
    char const * state_text;
    char const * error;
    bool recording;
    bool uploading;

    if ((UI_PAGE_CHAT != s_current_page) || (NULL == s_chat_state_label))
    {
        return;
    }

    if (!chat_service_is_initialized())
    {
        error = chat_service_last_error();
        lv_label_set_text(s_chat_state_label, "聊天功能未就绪");
        lv_label_set_text(s_chat_mic_label, "不可用");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0x8A3947), 0);
        if ((NULL != error) && ('\0' != error[0]))
        {
            lv_label_set_text_fmt(s_chat_hint_label, "初始化失败: %s", error);
        }
        return;
    }

    state = DoubaoRealtime_State();
    recording = DoubaoRealtime_MicrophoneRunning();
    uploading = DoubaoRealtime_UploadingRecording();
    switch (state)
    {
        case DOUBAO_STATE_WAITING_FOR_WIFI:
            state_text = "等待 ESP32-C3 联网";
            break;
        case DOUBAO_STATE_OPENING_WEBSOCKET:
            state_text = "正在连接聊天服务";
            break;
        case DOUBAO_STATE_STARTING_CONNECTION:
        case DOUBAO_STATE_STARTING_SESSION:
            state_text = "正在建立实时会话";
            break;
        case DOUBAO_STATE_READY:
            state_text = "实时会话已连接";
            break;
        case DOUBAO_STATE_ERROR:
            state_text = "聊天连接错误";
            break;
        case DOUBAO_STATE_UNINITIALIZED:
        default:
            state_text = "正在初始化聊天";
            break;
    }
    lv_label_set_text(s_chat_state_label, state_text);
    lv_label_set_text_fmt(s_chat_stats_label,
                          "REC %lu.%lus  TX %lu.%lus",
                          (unsigned long) (DoubaoRealtime_RecordedSamples() / 16000U),
                          (unsigned long) ((DoubaoRealtime_RecordedSamples() % 16000U) / 1600U),
                          (unsigned long) (DoubaoRealtime_UploadedSamples() / 16000U),
                          (unsigned long) ((DoubaoRealtime_UploadedSamples() % 16000U) / 1600U));

    if (DOUBAO_STATE_ERROR == state)
    {
        lv_obj_add_state(s_chat_indicator, LV_STATE_DISABLED);
        error = chat_service_last_error();
        lv_label_set_text(s_chat_mic_label, "错误");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0xA33E50), 0);
        lv_label_set_text_fmt(s_chat_hint_label,
                              "错误: %s",
                              ((NULL != error) && ('\0' != error[0])) ? error : "UNKNOWN");
        lv_obj_set_style_text_color(s_chat_hint_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
    else if (recording)
    {
        lv_obj_clear_state(s_chat_indicator, LV_STATE_DISABLED);
        lv_label_set_text(s_chat_mic_label, "发送中");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0xD94C64), 0);
        lv_label_set_text(s_chat_hint_label, "再次点击圆形按钮，停止录音并发送");
        lv_obj_set_style_text_color(s_chat_hint_label, lv_color_hex(0xFFD4DA), 0);
    }
    else if (uploading)
    {
        lv_obj_add_state(s_chat_indicator, LV_STATE_DISABLED);
        lv_label_set_text(s_chat_mic_label, "发送中");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0xC8833C), 0);
        lv_label_set_text(s_chat_hint_label, "正在发送音频并等待回复...");
        lv_obj_set_style_text_color(s_chat_hint_label, lv_color_hex(0xFFE3B5), 0);
    }
    else if (DOUBAO_STATE_READY == state)
    {
        lv_obj_clear_state(s_chat_indicator, LV_STATE_DISABLED);
        lv_label_set_text(s_chat_mic_label, "可录音");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0x3F8B70), 0);
        lv_label_set_text(s_chat_hint_label, "点击一次开始录音，再点击一次停止并发送");
        lv_obj_set_style_text_color(s_chat_hint_label, lv_color_hex(0xC9E8DA), 0);
    }
    else
    {
        lv_obj_add_state(s_chat_indicator, LV_STATE_DISABLED);
        lv_label_set_text(s_chat_mic_label, "连接中");
        lv_obj_set_style_bg_color(s_chat_indicator, lv_color_hex(0x66527C), 0);
        lv_label_set_text(s_chat_hint_label, "连接完成并播放自动介绍后即可开始聊天");
        lv_obj_set_style_text_color(s_chat_hint_label, lv_color_hex(0xAFA4C2), 0);
    }
}

void ui_init(void)
{
    fsp_err_t brightness_err;

    if (NULL != s_home_screen)
    {
        return;
    }

    (void) alarm_clock_init();
    s_weather_client_ready = weather_client_init();

    home_screen_create();
    table_lamp_screen_create();
    observer_screen_create();
    alarm_screen_create();
    weather_screen_create();
    chat_screen_create();

    clock_timer_cb(NULL);
    (void) lv_timer_create(clock_timer_cb, 1000U, NULL);
    (void) lv_timer_create(ui_state_timer_cb, UI_STATE_TIMER_PERIOD_MS, NULL);
    (void) lv_timer_create(network_timer_cb, UI_NETWORK_TIMER_PERIOD_MS, NULL);

    /* 首页滑杆默认显示60%，同时把实际屏幕背光同步到相同值。 */
    brightness_err = drv_spi_display_set_brightness(60U);
    if (FSP_SUCCESS != brightness_err)
    {
        printf("[UI] Initial backlight PWM set failed: %d\r\n", (int) brightness_err);
    }

    s_last_app_mode = app_main_get_mode();
    lv_scr_load(s_home_screen);
    /* 上电进入首页后立即发起第一次时间校准。 */
    esp_time_sync_request(lv_tick_get());
}
