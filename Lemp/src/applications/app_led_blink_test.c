/*
 * app_led_blink_test.c
 *
 * P1_10 GPIO输出翻转测试。
 */

#include "app.h"

#define LED_BLINK_PIN                 (BSP_IO_PORT_01_PIN_10)
#define LED_BLINK_INTERVAL_MS         (500U)

void app_led_blink_test(void)
{
    fsp_err_t err;
    bsp_io_level_t read_level;
    bsp_io_level_t write_level;

    /*
     * P1_10当前没有配置在configuration.xml中，
     * 因此测试开始时将它配置为初始低电平的GPIO输出。
     */
    err = g_ioport.p_api->pinCfg(&g_ioport_ctrl,
                                 LED_BLINK_PIN,
                                 IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                                 IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("LED test pin configuration", err);
    }

    printf("LED blink test started: P1_10, interval=500 ms.\r\n");

    while (1)
    {
        /* 读取当前输出电平。 */
        err = g_ioport.p_api->pinRead(&g_ioport_ctrl,
                                      LED_BLINK_PIN,
                                      &read_level);
        if (FSP_SUCCESS != err)
        {
            app_fatal_error("LED test pin read", err);
        }

        /* 使用明确的高低电平判断，避免依赖枚举值正好为0和1。 */
        write_level = (BSP_IO_LEVEL_LOW == read_level) ?
                      BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW;

        err = g_ioport.p_api->pinWrite(&g_ioport_ctrl,
                                       LED_BLINK_PIN,
                                       write_level);
        if (FSP_SUCCESS != err)
        {
            app_fatal_error("LED test pin write", err);
        }

        printf("P1_10 level: %s\r\n",
               (BSP_IO_LEVEL_HIGH == write_level) ? "HIGH" : "LOW");

        R_BSP_SoftwareDelay(LED_BLINK_INTERVAL_MS,
                            BSP_DELAY_UNITS_MILLISECONDS);
    }
}
