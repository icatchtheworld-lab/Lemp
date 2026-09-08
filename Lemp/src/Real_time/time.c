/*
 * time.c
 *
 *  Created on: 2026年6月25日
 *      Author: 36315
 */
// 时间初始化
#include "time.h"
#include "hal_data.h"

static bool time_set_default(void)
{
    rtc_time_t default_time = {0};

    /* RTC首次启动时使用的默认日期和时间。 */
    default_time.tm_sec  = 0;
    default_time.tm_min  = 47;
    default_time.tm_hour = 14;
    default_time.tm_mday = 18;
    default_time.tm_mon  = 6;       /* 月份范围为0-11，6表示7月。 */
    default_time.tm_year = 126;     /* RTC年份以1900年为起点。 */
    default_time.tm_wday = 6;       /* 星期范围为0-6，6表示星期六。 */

    return time_set(&default_time);
}

bool time_start(void)
{
    if (!time_init())
    {
        return false;
    }

    /* VBATT保持RTC运行时，不覆盖已经保存的时间。 */
    if (time_is_running())
    {
        return true;
    }

    return time_set_default();
}

bool time_init(void)
{
    fsp_err_t status;
    status = R_RTC_Open(&g_rtc0_ctrl, &g_rtc0_cfg);
    if(status != FSP_SUCCESS)
    {
        return false;
    }

    if (!time_is_running())
    {
        status = R_RTC_ClockSourceSet(&g_rtc0_ctrl);
        if(status != FSP_SUCCESS)
        {
            return false;
        }
    }

    return true;
}

bool time_set(rtc_time_t *p_time)
{
    fsp_err_t status;
    status = R_RTC_CalendarTimeSet(&g_rtc0_ctrl, p_time);
    if(status != FSP_SUCCESS)
    {
        return false;
    }
    return true;
}

bool time_get(rtc_time_t * p_time)
{
    fsp_err_t status;
    status = R_RTC_CalendarTimeGet(&g_rtc0_ctrl, p_time);
    return (status == FSP_SUCCESS);
}

bool time_is_running(void)
{
    rtc_info_t rtc_info;
    fsp_err_t status = R_RTC_InfoGet(&g_rtc0_ctrl, &rtc_info);
    if (status != FSP_SUCCESS)
    {
        return false;
    }
    return (rtc_info.status == RTC_STATUS_RUNNING);
}
