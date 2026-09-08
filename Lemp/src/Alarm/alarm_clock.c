#include "Alarm/alarm_clock.h"
#include "Alarm/alarm_tone.h"

#include "Flash/qspi_flash.h"
#include "Real_time/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ALARM_MAX_COUNT           (ALARM_SETTING_MAX_COUNT)
#define ALARM_ROLLER_WIDTH        (78)
#define ALARM_ROLLER_VISIBLE_ROWS (3U)
#define ALARM_ROLLER_LINE_SPACE   (12)

typedef struct st_alarm_item
{
    uint8_t hour;
    uint8_t minute;
    bool    enabled;
} alarm_item_t;

static char const s_hour_options[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";

static char const s_minute_options[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n"
    "24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n"
    "36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
    "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

LV_FONT_DECLARE(ui_font_chinese_16);

static alarm_item_t s_alarms[ALARM_MAX_COUNT];
static uint8_t      s_alarm_count;
static int8_t       s_edit_index = -1;
static bool         s_flash_ready;
static bool         s_tone_ready;
static bool         s_ringing;
static uint32_t     s_last_trigger_key = UINT32_MAX;
static int          s_last_polled_second = -1;

static lv_obj_t * s_hour_roller;
static lv_obj_t * s_minute_roller;
static lv_obj_t * s_action_button_label;
static lv_obj_t * s_status_label;
static lv_obj_t * s_clock_label;
static lv_obj_t * s_list_panel;
static lv_obj_t * s_ring_overlay;

static lv_obj_t * alarm_panel_create(lv_obj_t * parent,
                                     lv_coord_t width,
                                     lv_coord_t height,
                                     lv_color_t color,
                                     lv_coord_t radius)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_bg_color(panel, color, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t * alarm_label_create(lv_obj_t * parent, char const * text)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &ui_font_chinese_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

static lv_obj_t * alarm_button_create(lv_obj_t * parent,
                                      char const * text,
                                      lv_coord_t width,
                                      lv_coord_t height,
                                      lv_color_t color,
                                      lv_event_cb_t callback,
                                      void * user_data)
{
    lv_obj_t * button = lv_btn_create(parent);
    lv_obj_t * label;

    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    label = alarm_label_create(button, text);
    lv_obj_center(label);
    return button;
}

static void alarm_status_set(char const * text, lv_color_t color)
{
    if (NULL != s_status_label)
    {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, color, 0);
    }
}

static fsp_err_t alarm_flash_save(void)
{
    alarm_setting_t settings[ALARM_MAX_COUNT] = {0};

    if (!s_flash_ready)
    {
        return FSP_ERR_NOT_INITIALIZED;
    }

    for (uint8_t index = 0U; index < s_alarm_count; index++)
    {
        settings[index].hour = s_alarms[index].hour;
        settings[index].minute = s_alarms[index].minute;
        settings[index].enable = s_alarms[index].enabled ? 1U : 0U;
        settings[index].repeat_mask = 0x7FU;
    }
    return alarm_settings_save(settings, s_alarm_count);
}

static bool alarm_time_exists(uint8_t hour, uint8_t minute, int8_t ignored_index)
{
    for (uint8_t index = 0U; index < s_alarm_count; index++)
    {
        if (((int8_t) index != ignored_index) &&
            (s_alarms[index].hour == hour) &&
            (s_alarms[index].minute == minute))
        {
            return true;
        }
    }
    return false;
}

static void alarm_list_sort(void)
{
    for (uint8_t index = 1U; index < s_alarm_count; index++)
    {
        alarm_item_t item = s_alarms[index];
        uint8_t insert = index;
        while ((insert > 0U) &&
               (((uint16_t) s_alarms[insert - 1U].hour * 60U + s_alarms[insert - 1U].minute) >
                ((uint16_t) item.hour * 60U + item.minute)))
        {
            s_alarms[insert] = s_alarms[insert - 1U];
            insert--;
        }
        s_alarms[insert] = item;
    }
}

static void alarm_edit_cancel(void)
{
    s_edit_index = -1;
    if (NULL != s_action_button_label)
    {
        lv_label_set_text(s_action_button_label, "添加");
    }
}

static void alarm_action_event_cb(lv_event_t * event);
static void alarm_edit_event_cb(lv_event_t * event);
static void alarm_delete_event_cb(lv_event_t * event);
static void alarm_enable_event_cb(lv_event_t * event);
static void alarm_stop_event_cb(lv_event_t * event);

static void alarm_list_rebuild(void)
{
    if (NULL == s_list_panel)
    {
        return;
    }

    lv_obj_clean(s_list_panel);
    if (0U == s_alarm_count)
    {
        lv_obj_t * empty = alarm_label_create(s_list_panel, "暂无闹钟");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8E9BAF), 0);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_set_height(s_list_panel, 214);
        return;
    }

    for (uint8_t index = 0U; index < s_alarm_count; index++)
    {
        lv_obj_t * row = alarm_panel_create(s_list_panel, 264, 56,
                                             lv_color_hex(0x202C40), 12);
        lv_obj_t * time_label;
        lv_obj_t * enable_switch;
        lv_obj_t * edit_button;
        lv_obj_t * delete_button;

        lv_obj_set_pos(row, 2, (lv_coord_t) index * 64);

        time_label = lv_label_create(row);
        lv_label_set_text_fmt(time_label, "%02u:%02u",
                              (unsigned int) s_alarms[index].hour,
                              (unsigned int) s_alarms[index].minute);
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFD166), 0);
        lv_obj_set_pos(time_label, 10, 20);

        enable_switch = lv_switch_create(row);
        lv_obj_set_size(enable_switch, 42, 24);
        lv_obj_set_pos(enable_switch, 82, 17);
        if (s_alarms[index].enabled)
        {
            lv_obj_add_state(enable_switch, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(enable_switch, alarm_enable_event_cb,
                            LV_EVENT_VALUE_CHANGED, (void *) (uintptr_t) index);

        edit_button = alarm_button_create(row, "编辑", 58, 36,
                                          lv_color_hex(0x397A70),
                                          alarm_edit_event_cb,
                                          (void *) (uintptr_t) index);
        lv_obj_set_pos(edit_button, 136, 11);

        delete_button = alarm_button_create(row, "删除", 58, 36,
                                            lv_color_hex(0xA14452),
                                            alarm_delete_event_cb,
                                            (void *) (uintptr_t) index);
        lv_obj_set_pos(delete_button, 206, 11);
    }

    lv_obj_set_height(s_list_panel,
                      (lv_coord_t) ((s_alarm_count * 64U) > 214U ?
                                    (s_alarm_count * 64U) : 214U));
}

static void alarm_action_event_cb(lv_event_t * event)
{
    uint16_t const hour_selected = lv_roller_get_selected(s_hour_roller);
    uint16_t const minute_selected = lv_roller_get_selected(s_minute_roller);
    uint8_t const hour = (uint8_t) hour_selected;
    uint8_t const minute = (uint8_t) minute_selected;
    alarm_item_t backup[ALARM_MAX_COUNT];
    uint8_t const backup_count = s_alarm_count;

    LV_UNUSED(event);
    if ((hour_selected > 23U) || (minute_selected > 59U))
    {
        alarm_status_set("PICKER ERROR", lv_palette_main(LV_PALETTE_RED));
        return;
    }
    if (alarm_time_exists(hour, minute, s_edit_index))
    {
        alarm_status_set("TIME EXISTS", lv_palette_main(LV_PALETTE_ORANGE));
        return;
    }

    memcpy(backup, s_alarms, sizeof(backup));
    if (s_edit_index >= 0)
    {
        s_alarms[(uint8_t) s_edit_index].hour = hour;
        s_alarms[(uint8_t) s_edit_index].minute = minute;
    }
    else
    {
        if (s_alarm_count >= ALARM_MAX_COUNT)
        {
            alarm_status_set("LIST FULL", lv_palette_main(LV_PALETTE_RED));
            return;
        }
        s_alarms[s_alarm_count].hour = hour;
        s_alarms[s_alarm_count].minute = minute;
        s_alarms[s_alarm_count].enabled = true;
        s_alarm_count++;
    }

    alarm_list_sort();
    if (FSP_SUCCESS != alarm_flash_save())
    {
        memcpy(s_alarms, backup, sizeof(backup));
        s_alarm_count = backup_count;
        alarm_status_set("FLASH ERROR", lv_palette_main(LV_PALETTE_RED));
        return;
    }

    alarm_status_set((s_edit_index >= 0) ? "ALARM UPDATED" : "ALARM ADDED",
                     lv_palette_main(LV_PALETTE_GREEN));
    alarm_edit_cancel();
    alarm_list_rebuild();
}

static void alarm_edit_event_cb(lv_event_t * event)
{
    uint8_t const index = (uint8_t) (uintptr_t) lv_event_get_user_data(event);
    if (index >= s_alarm_count)
    {
        return;
    }
    s_edit_index = (int8_t) index;
    lv_roller_set_selected(s_hour_roller, s_alarms[index].hour, LV_ANIM_ON);
    lv_roller_set_selected(s_minute_roller, s_alarms[index].minute, LV_ANIM_ON);
    lv_label_set_text(s_action_button_label, "保存");
    alarm_status_set("EDIT MODE", lv_color_hex(0x77C7FF));
}

static void alarm_delete_event_cb(lv_event_t * event)
{
    uint8_t const index = (uint8_t) (uintptr_t) lv_event_get_user_data(event);
    alarm_item_t backup[ALARM_MAX_COUNT];
    uint8_t const backup_count = s_alarm_count;

    if (index >= s_alarm_count)
    {
        return;
    }
    memcpy(backup, s_alarms, sizeof(backup));
    for (uint8_t move = index; (move + 1U) < s_alarm_count; move++)
    {
        s_alarms[move] = s_alarms[move + 1U];
    }
    s_alarm_count--;

    if (FSP_SUCCESS != alarm_flash_save())
    {
        memcpy(s_alarms, backup, sizeof(backup));
        s_alarm_count = backup_count;
        alarm_status_set("FLASH ERROR", lv_palette_main(LV_PALETTE_RED));
        return;
    }
    alarm_edit_cancel();
    alarm_status_set("ALARM DELETED", lv_color_hex(0xE5A5AE));
    alarm_list_rebuild();
}

static void alarm_enable_event_cb(lv_event_t * event)
{
    uint8_t const index = (uint8_t) (uintptr_t) lv_event_get_user_data(event);
    lv_obj_t * alarm_switch;
    bool previous_enabled;

    if (index >= s_alarm_count)
    {
        return;
    }
    alarm_switch = lv_event_get_target(event);
    previous_enabled = s_alarms[index].enabled;
    s_alarms[index].enabled = lv_obj_has_state(alarm_switch, LV_STATE_CHECKED);
    if (FSP_SUCCESS != alarm_flash_save())
    {
        s_alarms[index].enabled = previous_enabled;
        if (previous_enabled)
        {
            lv_obj_add_state(alarm_switch, LV_STATE_CHECKED);
        }
        else
        {
            lv_obj_clear_state(alarm_switch, LV_STATE_CHECKED);
        }
        alarm_status_set("FLASH ERROR", lv_palette_main(LV_PALETTE_RED));
        return;
    }
    alarm_status_set(s_alarms[index].enabled ? "ALARM ON" : "ALARM OFF",
                     s_alarms[index].enabled ? lv_palette_main(LV_PALETTE_GREEN) :
                                               lv_color_hex(0xA5ADBD));
}

static uint32_t alarm_trigger_key_build(rtc_time_t const * current_time)
{
    uint32_t key = (uint32_t) current_time->tm_year;
    key = (key * 13U) + (uint32_t) current_time->tm_mon;
    key = (key * 32U) + (uint32_t) current_time->tm_mday;
    key = (key * 24U) + (uint32_t) current_time->tm_hour;
    key = (key * 60U) + (uint32_t) current_time->tm_min;
    return key;
}

static void alarm_ring_overlay_create(alarm_item_t const * alarm)
{
    lv_obj_t * title;
    lv_obj_t * ring_time_label;
    lv_obj_t * hint;
    lv_obj_t * stop_button;

    s_ring_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ring_overlay, 320, 480);
    lv_obj_set_pos(s_ring_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_ring_overlay, lv_color_hex(0x08101D), 0);
    lv_obj_set_style_bg_opa(s_ring_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_ring_overlay, 0, 0);
    lv_obj_clear_flag(s_ring_overlay, LV_OBJ_FLAG_SCROLLABLE);

    title = alarm_label_create(s_ring_overlay, "闹钟时间到");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFD166), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 78);

    ring_time_label = lv_label_create(s_ring_overlay);
    lv_label_set_text_fmt(ring_time_label, "%02u:%02u",
                          (unsigned int) alarm->hour,
                          (unsigned int) alarm->minute);
    lv_obj_set_style_text_font(ring_time_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ring_time_label, lv_color_white(), 0);
    lv_obj_align(ring_time_label, LV_ALIGN_CENTER, 0, -42);

    hint = alarm_label_create(s_ring_overlay, "点击停止闹钟");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xBCC9DA), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);

    stop_button = alarm_button_create(s_ring_overlay, "停止", 180, 64,
                                      lv_color_hex(0xC84C5A),
                                      alarm_stop_event_cb, NULL);
    lv_obj_align(stop_button, LV_ALIGN_CENTER, 0, 92);
}

static void alarm_start(uint8_t alarm_index, uint32_t trigger_key)
{
    if (s_ringing || (alarm_index >= s_alarm_count))
    {
        return;
    }
    s_last_trigger_key = trigger_key;
    s_ringing = true;
    alarm_ring_overlay_create(&s_alarms[alarm_index]);
    if (!s_tone_ready)
    {
        /* Initialization can fail transiently during early board startup. */
        s_tone_ready = alarm_tone_init();
    }
    if (s_tone_ready)
    {
        if (!alarm_tone_start())
        {
            printf("Alarm tone waiting for shared audio output.\r\n");
        }
    }
    alarm_status_set("ALARM RINGING", lv_palette_main(LV_PALETTE_ORANGE));
}

static void alarm_stop_event_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    s_ringing = false;
    alarm_tone_stop();
    if (NULL != s_ring_overlay)
    {
        lv_obj_del(s_ring_overlay);
        s_ring_overlay = NULL;
    }
    alarm_status_set("ALARM STOPPED", lv_color_hex(0xA5ADBD));
}

bool alarm_clock_init(void)
{
    alarm_setting_t settings[ALARM_MAX_COUNT];
    uint8_t count = 0U;

    s_flash_ready = (FSP_SUCCESS == qspi_flash_init());
    s_tone_ready = alarm_tone_init();
    s_alarm_count = 0U;
    if (s_flash_ready && alarm_settings_load(settings, ALARM_MAX_COUNT, &count))
    {
        s_alarm_count = count;
        for (uint8_t index = 0U; index < count; index++)
        {
            s_alarms[index].hour = settings[index].hour;
            s_alarms[index].minute = settings[index].minute;
            s_alarms[index].enabled = (0U != settings[index].enable);
        }
        alarm_list_sort();
    }
    return s_flash_ready;
}

lv_obj_t * alarm_clock_screen_create(lv_event_cb_t back_event_cb)
{
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_t * title;
    lv_obj_t * back_button;
    lv_obj_t * set_panel;
    lv_obj_t * set_title;
    lv_obj_t * colon;
    lv_obj_t * action_button;
    lv_obj_t * list_title;
    lv_obj_t * list_view;

    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x253451), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    back_button = alarm_button_create(screen, LV_SYMBOL_LEFT " 返回", 76, 38,
                                      lv_color_hex(0x46536B), back_event_cb, NULL);
    lv_obj_set_pos(back_button, 12, 10);

    title = alarm_label_create(screen, "闹钟");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFD166), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 21);

    s_clock_label = lv_label_create(screen);
    lv_label_set_text(s_clock_label, "--:--:--");
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xD6E2F0), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -12, 20);

    set_panel = alarm_panel_create(screen, 300, 156, lv_color_hex(0x1E293B), 18);
    lv_obj_set_pos(set_panel, 10, 48);
    set_title = alarm_label_create(set_panel, "时间设置");
    lv_obj_set_style_text_color(set_title, lv_color_hex(0xA9BED5), 0);
    lv_obj_set_pos(set_title, 12, 9);

    s_hour_roller = lv_roller_create(set_panel);
    lv_roller_set_options(s_hour_roller, s_hour_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(s_hour_roller, ALARM_ROLLER_WIDTH);
    lv_obj_set_style_text_font(s_hour_roller, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(s_hour_roller, &lv_font_montserrat_16, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(s_hour_roller, ALARM_ROLLER_LINE_SPACE, 0);
    lv_obj_set_style_text_line_space(s_hour_roller, ALARM_ROLLER_LINE_SPACE, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_hour_roller, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_color(s_hour_roller, lv_color_hex(0x397A70), LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_hour_roller, 0, 0);
    lv_roller_set_visible_row_count(s_hour_roller, ALARM_ROLLER_VISIBLE_ROWS);
    lv_obj_set_pos(s_hour_roller, 12, 42);

    colon = lv_label_create(set_panel);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(colon, lv_color_white(), 0);
    lv_obj_set_pos(colon, 99, 76);

    s_minute_roller = lv_roller_create(set_panel);
    lv_roller_set_options(s_minute_roller, s_minute_options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(s_minute_roller, ALARM_ROLLER_WIDTH);
    lv_obj_set_style_text_font(s_minute_roller, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(s_minute_roller, &lv_font_montserrat_16, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(s_minute_roller, ALARM_ROLLER_LINE_SPACE, 0);
    lv_obj_set_style_text_line_space(s_minute_roller, ALARM_ROLLER_LINE_SPACE, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_minute_roller, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_color(s_minute_roller, lv_color_hex(0x397A70), LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_minute_roller, 0, 0);
    lv_roller_set_visible_row_count(s_minute_roller, ALARM_ROLLER_VISIBLE_ROWS);
    lv_obj_set_pos(s_minute_roller, 116, 42);

    action_button = alarm_button_create(set_panel, "添加", 82, 48,
                                        lv_color_hex(0x2F8F78),
                                        alarm_action_event_cb, NULL);
    lv_obj_set_pos(action_button, 206, 60);
    s_action_button_label = lv_obj_get_child(action_button, 0);

    s_status_label = lv_label_create(set_panel);
    lv_label_set_text(s_status_label, s_flash_ready ? "READY" : "FLASH ERROR");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_label,
                                s_flash_ready ? lv_color_hex(0xA5ADBD) :
                                                lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_RIGHT, -12, -17);

    list_title = alarm_label_create(screen, "已设置闹钟");
    lv_obj_set_style_text_color(list_title, lv_color_hex(0xA9BED5), 0);
    lv_obj_set_pos(list_title, 14, 216);

    list_view = alarm_panel_create(screen, 300, 232, lv_color_hex(0x151F31), 18);
    lv_obj_set_pos(list_view, 10, 242);
    lv_obj_add_flag(list_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_view, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(list_view, 8, 0);

    s_list_panel = lv_obj_create(list_view);
    lv_obj_set_width(s_list_panel, 280);
    lv_obj_set_height(s_list_panel, 214);
    lv_obj_set_pos(s_list_panel, 0, 0);
    lv_obj_set_style_bg_opa(s_list_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_panel, 0, 0);
    lv_obj_set_style_pad_all(s_list_panel, 0, 0);
    lv_obj_clear_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);

    alarm_list_rebuild();
    return screen;
}

void alarm_clock_poll(void)
{
    rtc_time_t current_time;
    uint32_t trigger_key;

    /* I2S callbacks only mark deferred cleanup; release the shared external
     * audio clock here in normal LVGL context. */
    alarm_tone_poll();
    if (s_ringing)
    {
        if (!s_tone_ready)
        {
            s_tone_ready = alarm_tone_init();
        }
        /*
         * alarm_tone_start() is idempotent while active.  Repeating it here
         * recovers automatically from a transient I2S ownership handover.
         */
        if (s_tone_ready)
        {
            (void) alarm_tone_start();
        }
    }

    if (!time_get(&current_time))
    {
        if (NULL != s_clock_label)
        {
            lv_label_set_text(s_clock_label, "RTC ERROR");
        }
        return;
    }

    if (current_time.tm_sec == s_last_polled_second)
    {
        return;
    }
    s_last_polled_second = current_time.tm_sec;
    if (NULL != s_clock_label)
    {
        lv_label_set_text_fmt(s_clock_label, "%02d:%02d:%02d",
                              current_time.tm_hour,
                              current_time.tm_min,
                              current_time.tm_sec);
    }

    trigger_key = alarm_trigger_key_build(&current_time);
    if (trigger_key == s_last_trigger_key)
    {
        return;
    }

    for (uint8_t index = 0U; index < s_alarm_count; index++)
    {
        if (s_alarms[index].enabled &&
            (s_alarms[index].hour == (uint8_t) current_time.tm_hour) &&
            (s_alarms[index].minute == (uint8_t) current_time.tm_min))
        {
            alarm_start(index, trigger_key);
            break;
        }
    }
}
