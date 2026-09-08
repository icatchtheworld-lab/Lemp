#include "Esp/esp_time_sync.h"

#include "Esp/esp_transport.h"
#include "Real_time/time.h"

#include <stddef.h>
#include <string.h>

#define ESP_TIME_DATETIME_LENGTH (19U)
#define ESP_TIME_RETRY_PERIOD_MS  (3000U)
#define ESP_TIME_REQUEST_WINDOW_MS (30000U)
#define ESP_TIME_SYNC_INTERVAL_MS  (12UL * 60UL * 60UL * 1000UL)
#define ESP_TIME_FAILED_RETRY_MS   (5UL * 60UL * 1000UL)

static bool s_time_synchronized;
static bool s_request_active;
static uint32_t s_request_started_ms;
static uint32_t s_last_request_ms;
static uint32_t s_last_sync_ms;
static uint32_t s_last_cycle_started_ms;

static bool interval_elapsed(uint32_t now_ms, uint32_t previous_ms, uint32_t interval_ms)
{
    return ((uint32_t) (now_ms - previous_ms) >= interval_ms);
}

static bool time_request_send(uint32_t now_ms)
{
    s_last_request_ms = now_ms;
    return esp_transport_send(ESP_CHANNEL_WEATHER,
                              ESP_MSG_TIME_REQUEST,
                              0U,
                              NULL,
                              0U);
}

static bool parse_decimal(uint8_t const * text, uint32_t digits, uint32_t * value)
{
    uint32_t parsed = 0U;

    if ((NULL == text) || (NULL == value) || (0U == digits))
    {
        return false;
    }

    for (uint32_t index = 0U; index < digits; index++)
    {
        if ((text[index] < (uint8_t) '0') || (text[index] > (uint8_t) '9'))
        {
            return false;
        }
        parsed = (parsed * 10U) + (uint32_t) (text[index] - (uint8_t) '0');
    }

    *value = parsed;
    return true;
}

static bool year_is_leap(uint32_t year)
{
    return ((0U == (year % 4U)) && (0U != (year % 100U))) ||
           (0U == (year % 400U));
}

static uint32_t month_day_count(uint32_t year, uint32_t month)
{
    static uint8_t const days_per_month[12] =
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    if ((0U == month) || (month > 12U))
    {
        return 0U;
    }
    if ((2U == month) && year_is_leap(year))
    {
        return 29U;
    }
    return days_per_month[month - 1U];
}

static uint32_t weekday_calculate(uint32_t year, uint32_t month, uint32_t day)
{
    /* Gregorian calendar: result is 0=Sunday ... 6=Saturday, matching rtc_time_t. */
    static uint8_t const month_offsets[12] =
    {
        0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U
    };

    if (month < 3U)
    {
        year--;
    }
    return (year + (year / 4U) - (year / 100U) + (year / 400U) +
            month_offsets[month - 1U] + day) % 7U;
}

static uint8_t const * datetime_find(uint8_t const * payload, uint32_t payload_length)
{
    static char const marker[] = "\"datetime\":\"";
    uint32_t const marker_length = (uint32_t) (sizeof(marker) - 1U);

    if ((NULL == payload) ||
        (payload_length < (marker_length + ESP_TIME_DATETIME_LENGTH)))
    {
        return NULL;
    }

    for (uint32_t offset = 0U;
         (offset + marker_length + ESP_TIME_DATETIME_LENGTH) <= payload_length;
         offset++)
    {
        if (0 == memcmp(&payload[offset], marker, marker_length))
        {
            return &payload[offset + marker_length];
        }
    }
    return NULL;
}

void esp_time_sync_init(void)
{
    s_time_synchronized = false;
    s_request_active = false;
    s_request_started_ms = 0U;
    s_last_request_ms = 0U;
    s_last_sync_ms = 0U;
    s_last_cycle_started_ms = 0U;
}

void esp_time_sync_request(uint32_t now_ms)
{
    if (s_request_active)
    {
        return;
    }
    s_request_active = true;
    s_request_started_ms = now_ms;
    s_last_cycle_started_ms = now_ms;
    (void) time_request_send(now_ms);
}

void esp_time_sync_poll(uint32_t now_ms)
{
    if (!s_request_active)
    {
        uint32_t const interval = s_time_synchronized ?
                                  ESP_TIME_SYNC_INTERVAL_MS :
                                  ESP_TIME_FAILED_RETRY_MS;
        uint32_t const reference_ms = s_time_synchronized ?
                                      s_last_sync_ms :
                                      s_last_cycle_started_ms;
        if (interval_elapsed(now_ms, reference_ms, interval) &&
            interval_elapsed(now_ms,
                             s_last_cycle_started_ms,
                             ESP_TIME_FAILED_RETRY_MS))
        {
            esp_time_sync_request(now_ms);
        }
        return;
    }
    if (interval_elapsed(now_ms, s_request_started_ms, ESP_TIME_REQUEST_WINDOW_MS))
    {
        s_request_active = false;
        return;
    }
    if (interval_elapsed(now_ms, s_last_request_ms, ESP_TIME_RETRY_PERIOD_MS))
    {
        (void) time_request_send(now_ms);
    }
}

bool esp_time_sync_handle(uint8_t const * payload,
                          uint32_t payload_length,
                          uint32_t now_ms)
{
    uint8_t const * datetime = datetime_find(payload, payload_length);
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    rtc_time_t rtc_time = {0};

    if ((NULL == datetime) ||
        ((uint8_t) '-' != datetime[4]) ||
        ((uint8_t) '-' != datetime[7]) ||
        ((uint8_t) 'T' != datetime[10]) ||
        ((uint8_t) ':' != datetime[13]) ||
        ((uint8_t) ':' != datetime[16]) ||
        !parse_decimal(&datetime[0], 4U, &year) ||
        !parse_decimal(&datetime[5], 2U, &month) ||
        !parse_decimal(&datetime[8], 2U, &day) ||
        !parse_decimal(&datetime[11], 2U, &hour) ||
        !parse_decimal(&datetime[14], 2U, &minute) ||
        !parse_decimal(&datetime[17], 2U, &second))
    {
        return false;
    }

    /* RA8P1 calendar RTC stores years 2000-2099. */
    if ((year < 2000U) || (year > 2099U) ||
        (0U == month) || (month > 12U) ||
        (0U == day) || (day > month_day_count(year, month)) ||
        (hour > 23U) || (minute > 59U) || (second > 59U))
    {
        return false;
    }

    rtc_time.tm_year = (int) year - 1900;
    rtc_time.tm_mon  = (int) month - 1;
    rtc_time.tm_mday = (int) day;
    rtc_time.tm_hour = (int) hour;
    rtc_time.tm_min  = (int) minute;
    rtc_time.tm_sec  = (int) second;
    rtc_time.tm_wday = (int) weekday_calculate(year, month, day);

    if (!time_set(&rtc_time))
    {
        return false;
    }

    s_time_synchronized = true;
    s_request_active = false;
    s_last_sync_ms = now_ms;
    s_last_cycle_started_ms = now_ms;
    return true;
}

bool esp_time_sync_handle_error(uint8_t const * payload, uint32_t payload_length)
{
    static char const not_ready[] = "TIME_NOT_READY";
    uint32_t const expected_length = (uint32_t) (sizeof(not_ready) - 1U);

    if (!s_request_active || (NULL == payload) ||
        (payload_length != expected_length) ||
        (0 != memcmp(payload, not_ready, expected_length)))
    {
        return false;
    }

    /* 保持本次校准请求有效，由poll按周期重试，直到NTP就绪或窗口超时。 */
    return true;
}

bool esp_time_sync_is_synchronized(void)
{
    return s_time_synchronized;
}
