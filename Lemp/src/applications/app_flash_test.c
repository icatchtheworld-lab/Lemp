/*
 * app_flash_test.c
 *
 *  Created on: 2026年7月23日
 *      Author: 36315
 */


#include "Flash/qspi_flash.h"
#include "app.h"
#include "Printf/printf.h"

void app_test_flash(void)
{
    fsp_err_t err;
    /*
     * 初始化 QSPI Flash。
     * 此阶段只检查通信是否正常，失败时继续运行主程序，
     * 方便通过 UART 日志定位问题。
     */
    err = qspi_flash_init();
    if (FSP_SUCCESS != err)
    {
        printf("QSPI Flash init failed: %d\r\n", (int) err);
    }
    else
    {
        alarm_setting_t alarm;
        if (alarm_setting_load(&alarm))
        {
            printf("Alarm loaded: %02u:%02u, enable=%u, repeat=0x%02X\r\n",
                   alarm.hour, alarm.minute, alarm.enable, alarm.repeat_mask);
        }
        else
        {
            /* 构造一条用于首次写入测试的闹钟设置。 */
            alarm.hour        = 7U;
            alarm.minute      = 30U;
            alarm.enable      = 1U;
            alarm.repeat_mask = 0x3EU;

            err = alarm_setting_save(&alarm);
            if (FSP_SUCCESS == err)
            {
                printf("Alarm saved: 07:30, enable=1, repeat=0x3E\r\n");
            }
            else
            {
                printf("Alarm save failed: %d\r\n", (int) err);
            }
        }
    }
    while (1)
    {
        R_BSP_SoftwareDelay(1000U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
