/*
 * app_lvgl_test.c
 *
 *  Created on: 2026年7月17日
 *      Author: 36315
 */


/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "app.h"
#include "Printf/printf.h"
#include "Screen/drv_gpt_timer.h"
#include "Screen/lv_port_disp.h"
#include "Screen/lv_port_indev.h"
#include "Middlewares/lvgl/lvgl.h"
#include "UI/ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define APP_LVGL_DEFAULT_PROCESS_PERIOD_MS    (5U)
#define APP_LVGL_MIN_PROCESS_PERIOD_MS        (1U)

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static bool s_lvgl_initialized = false;
static uint32_t s_lvgl_process_period_ms = APP_LVGL_DEFAULT_PROCESS_PERIOD_MS;
static uint32_t s_lvgl_last_process_ms;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
fsp_err_t app_lvgl_init(void)
{
    fsp_err_t err;

    if (s_lvgl_initialized)
    {
        return FSP_SUCCESS;
    }

    err = drv_gpt_timer_init();
    if(FSP_SUCCESS != err)
    {
        printf("[LVGL] GPT timer initialization failed: %d\r\n", (int) err);
        return err;
    }

    lv_init();

    err = lv_port_disp_init();
    if (FSP_SUCCESS != err)
    {
        printf("[LVGL] display port initialization failed: %d\r\n", (int) err);
        return err;
    }

    err = lv_port_indev_init();
    if (FSP_SUCCESS != err)
    {
        printf("[LVGL] input port initialization failed: %d\r\n", (int) err);
        return err;
    }

    /* Create the custom smart-lamp UI. */
    ui_init();

    s_lvgl_last_process_ms = lv_tick_get();
    s_lvgl_initialized = true;
    return FSP_SUCCESS;
}

void app_lvgl_set_process_period(uint32_t period_ms)
{
    /* 防止0 ms导致主循环无间隔地反复执行LVGL任务。 */
    if (period_ms < APP_LVGL_MIN_PROCESS_PERIOD_MS)
    {
        period_ms = APP_LVGL_MIN_PROCESS_PERIOD_MS;
    }

    s_lvgl_process_period_ms = period_ms;
    s_lvgl_last_process_ms = lv_tick_get();
}

void app_lvgl_process(void)
{
    if (s_lvgl_initialized)
    {
        uint32_t const now_ms = lv_tick_get();

        /* 只在设定周期到达后运行LVGL，避免空闲轮询长期占用CPU。 */
        if (lv_tick_elaps(s_lvgl_last_process_ms) >= s_lvgl_process_period_ms)
        {
            s_lvgl_last_process_ms = now_ms;
            lv_timer_handler();
        }
    }
}

void app_lvgl_test(void)
{
    fsp_err_t err = app_lvgl_init();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("LVGL test initialization", err);
    }

    while(1)
    {
        app_lvgl_process();
        R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);  // delay 5ms
    }
}

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
