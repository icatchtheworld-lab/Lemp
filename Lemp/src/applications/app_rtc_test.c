#include "app.h"

#include "Real_time/time.h"

#include <stdio.h>

void app_rtc_test(void)
{
    rtc_time_t current_time;

    if (!time_start())
    {
        app_fatal_error("RTC test initialization", FSP_ERR_INTERNAL);
    }

    while (1)
    {
        if (time_get(&current_time))
        {
            printf("20%02d-%02d-%02d %02d:%02d:%02d\r\n",
                   current_time.tm_year % 100,
                   current_time.tm_mon + 1,
                   current_time.tm_mday,
                   current_time.tm_hour,
                   current_time.tm_min,
                   current_time.tm_sec);
        }
        else
        {
            printf("RTC read failed.\r\n");
        }

        R_BSP_SoftwareDelay(1000U, BSP_DELAY_UNITS_MILLISECONDS);
    }
}
