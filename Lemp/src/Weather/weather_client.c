#include "Weather/weather_client.h"

#include "Doubao/doubao_realtime.h"
#include "Esp/esp_link.h"
#include "Esp/esp_time_sync.h"
#include "Esp/esp_transport.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEATHER_JSON_BUFFER_SIZE    (ESP_TRANSPORT_MAX_PAYLOAD + 1U)
#define WEATHER_JSON_TOKEN_COUNT    (1024U)
#define WEATHER_REQUEST_TIMEOUT_MS  (15000U)
#define WEATHER_REQUEST_COOLDOWN_MS (3000U)
#define WEATHER_BUSY_RETRY_MS       (500U)
#define WEATHER_REQUEST_CURRENT     (1U)

typedef enum e_json_token_type
{
    JSON_TOKEN_UNDEFINED = 0,
    JSON_TOKEN_OBJECT,
    JSON_TOKEN_ARRAY,
    JSON_TOKEN_STRING,
    JSON_TOKEN_PRIMITIVE,
} json_token_type_t;

typedef struct st_json_token
{
    json_token_type_t type;
    int32_t start;
    int32_t end;
    int32_t parent;
} json_token_t;

static char s_json_buffer[WEATHER_JSON_BUFFER_SIZE];
static json_token_t s_json_tokens[WEATHER_JSON_TOKEN_COUNT];
static weather_data_t s_result;
static bool s_initialized;
static bool s_busy;
static bool s_retry_pending;
static bool s_result_ready;
static uint32_t s_request_started_ms;
static uint32_t s_retry_due_ms;
static uint32_t s_next_request_allowed_ms;
static char s_last_error[48];

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return ((int32_t) (now_ms - deadline_ms) >= 0);
}

static void set_last_error(char const * error)
{
    if (NULL == error)
    {
        s_last_error[0] = '\0';
    }
    else
    {
        (void) snprintf(s_last_error, sizeof(s_last_error), "%s", error);
    }
}

static bool payload_equals(uint8_t const * payload,
                           uint32_t payload_length,
                           char const * text)
{
    size_t const text_length = (NULL != text) ? strlen(text) : 0U;
    return (NULL != payload) && (payload_length == text_length) &&
           (0 == memcmp(payload, text, text_length));
}

static int json_allocate_token(json_token_t * tokens, size_t capacity, size_t * count)
{
    int index;
    if (*count >= capacity)
    {
        return -1;
    }
    index = (int) *count;
    tokens[*count].type = JSON_TOKEN_UNDEFINED;
    tokens[*count].start = -1;
    tokens[*count].end = -1;
    tokens[*count].parent = -1;
    (*count)++;
    return index;
}

static int json_parse(char const * json, size_t length,
                      json_token_t * tokens, size_t capacity)
{
    size_t count = 0U;
    int32_t parent = -1;

    for (size_t position = 0U; position < length; position++)
    {
        char const ch = json[position];
        if (('{' == ch) || ('[' == ch))
        {
            int const index = json_allocate_token(tokens, capacity, &count);
            if (index < 0)
            {
                return -1;
            }
            tokens[index].type = ('{' == ch) ? JSON_TOKEN_OBJECT : JSON_TOKEN_ARRAY;
            tokens[index].start = (int32_t) position;
            tokens[index].parent = parent;
            parent = index;
        }
        else if (('}' == ch) || (']' == ch))
        {
            json_token_type_t const expected =
                ('}' == ch) ? JSON_TOKEN_OBJECT : JSON_TOKEN_ARRAY;
            if ((parent < 0) || (tokens[parent].type != expected))
            {
                return -1;
            }
            tokens[parent].end = (int32_t) position + 1;
            parent = tokens[parent].parent;
        }
        else if ('"' == ch)
        {
            size_t end = position + 1U;
            bool escaped = false;
            for (; end < length; end++)
            {
                char const string_ch = json[end];
                if (escaped)
                {
                    escaped = false;
                }
                else if ('\\' == string_ch)
                {
                    escaped = true;
                }
                else if ('"' == string_ch)
                {
                    break;
                }
            }
            if (end >= length)
            {
                return -1;
            }
            int const index = json_allocate_token(tokens, capacity, &count);
            if (index < 0)
            {
                return -1;
            }
            tokens[index].type = JSON_TOKEN_STRING;
            tokens[index].start = (int32_t) position + 1;
            tokens[index].end = (int32_t) end;
            tokens[index].parent = parent;
            position = end;
        }
        else if ((' ' == ch) || ('\t' == ch) || ('\r' == ch) || ('\n' == ch) ||
                 (':' == ch) || (',' == ch))
        {
        }
        else
        {
            size_t end = position;
            while (end < length)
            {
                char const primitive_ch = json[end];
                if ((',' == primitive_ch) || ('}' == primitive_ch) ||
                    (']' == primitive_ch) || (' ' == primitive_ch) ||
                    ('\t' == primitive_ch) || ('\r' == primitive_ch) ||
                    ('\n' == primitive_ch))
                {
                    break;
                }
                end++;
            }
            if (end == position)
            {
                return -1;
            }
            int const index = json_allocate_token(tokens, capacity, &count);
            if (index < 0)
            {
                return -1;
            }
            tokens[index].type = JSON_TOKEN_PRIMITIVE;
            tokens[index].start = (int32_t) position;
            tokens[index].end = (int32_t) end;
            tokens[index].parent = parent;
            position = end - 1U;
        }
    }

    if ((parent >= 0) || (0U == count))
    {
        return -1;
    }
    return (int) count;
}

static bool json_token_equals(char const * json,
                              json_token_t const * token,
                              char const * text)
{
    size_t const token_length = (size_t) (token->end - token->start);
    size_t const text_length = strlen(text);
    return (token->type == JSON_TOKEN_STRING) &&
           (token_length == text_length) &&
           (0 == memcmp(&json[token->start], text, text_length));
}

static int json_object_get(char const * json,
                           json_token_t const * tokens,
                           int token_count,
                           int object_index,
                           char const * key)
{
    bool expect_key = true;
    bool key_matches = false;

    if ((object_index < 0) || (object_index >= token_count) ||
        (tokens[object_index].type != JSON_TOKEN_OBJECT))
    {
        return -1;
    }

    for (int index = object_index + 1; index < token_count; index++)
    {
        if (tokens[index].start >= tokens[object_index].end)
        {
            break;
        }
        if (tokens[index].parent != object_index)
        {
            continue;
        }
        if (expect_key)
        {
            if (tokens[index].type != JSON_TOKEN_STRING)
            {
                return -1;
            }
            key_matches = json_token_equals(json, &tokens[index], key);
            expect_key = false;
        }
        else
        {
            if (key_matches)
            {
                return index;
            }
            expect_key = true;
        }
    }
    return -1;
}

static int json_array_get(json_token_t const * tokens,
                          int token_count,
                          int array_index,
                          uint32_t element_index)
{
    uint32_t current = 0U;
    if ((array_index < 0) || (array_index >= token_count) ||
        (tokens[array_index].type != JSON_TOKEN_ARRAY))
    {
        return -1;
    }
    for (int index = array_index + 1; index < token_count; index++)
    {
        if (tokens[index].start >= tokens[array_index].end)
        {
            break;
        }
        if (tokens[index].parent != array_index)
        {
            continue;
        }
        if (current == element_index)
        {
            return index;
        }
        current++;
    }
    return -1;
}

static int json_hex_value(char ch)
{
    if ((ch >= '0') && (ch <= '9'))
    {
        return ch - '0';
    }
    if ((ch >= 'a') && (ch <= 'f'))
    {
        return ch - 'a' + 10;
    }
    if ((ch >= 'A') && (ch <= 'F'))
    {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool append_utf8(char * destination, size_t capacity,
                        size_t * length, uint32_t codepoint)
{
    uint8_t bytes[3];
    size_t byte_count;
    if (codepoint <= 0x7FU)
    {
        bytes[0] = (uint8_t) codepoint;
        byte_count = 1U;
    }
    else if (codepoint <= 0x7FFU)
    {
        bytes[0] = (uint8_t) (0xC0U | (codepoint >> 6));
        bytes[1] = (uint8_t) (0x80U | (codepoint & 0x3FU));
        byte_count = 2U;
    }
    else
    {
        bytes[0] = (uint8_t) (0xE0U | (codepoint >> 12));
        bytes[1] = (uint8_t) (0x80U | ((codepoint >> 6) & 0x3FU));
        bytes[2] = (uint8_t) (0x80U | (codepoint & 0x3FU));
        byte_count = 3U;
    }
    if ((*length + byte_count + 1U) > capacity)
    {
        return false;
    }
    for (size_t index = 0U; index < byte_count; index++)
    {
        destination[(*length)++] = (char) bytes[index];
    }
    return true;
}

static bool json_copy_string(char const * json,
                             json_token_t const * token,
                             char * destination,
                             size_t capacity)
{
    size_t output_length = 0U;
    if ((NULL == destination) || (0U == capacity) ||
        (token->type != JSON_TOKEN_STRING))
    {
        return false;
    }

    for (int32_t index = token->start; index < token->end; index++)
    {
        uint32_t codepoint;
        char ch = json[index];
        if ('\\' != ch)
        {
            if ((output_length + 2U) > capacity)
            {
                return false;
            }
            destination[output_length++] = ch;
            continue;
        }

        index++;
        if (index >= token->end)
        {
            return false;
        }
        ch = json[index];
        if ('u' == ch)
        {
            if ((index + 4) >= token->end)
            {
                return false;
            }
            codepoint = 0U;
            for (uint32_t digit = 0U; digit < 4U; digit++)
            {
                int const value = json_hex_value(json[++index]);
                if (value < 0)
                {
                    return false;
                }
                codepoint = (codepoint << 4U) | (uint32_t) value;
            }
            if (!append_utf8(destination, capacity, &output_length, codepoint))
            {
                return false;
            }
        }
        else
        {
            switch (ch)
            {
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '"':
                case '\\':
                case '/':
                    break;
                default:
                    return false;
            }
            if ((output_length + 2U) > capacity)
            {
                return false;
            }
            destination[output_length++] = ch;
        }
    }
    destination[output_length] = '\0';
    return true;
}

static bool json_token_to_int(char const * json,
                              json_token_t const * token,
                              int * value)
{
    char number[16];
    size_t const length = (size_t) (token->end - token->start);
    char * end = NULL;
    long parsed;
    if ((token->type != JSON_TOKEN_PRIMITIVE) || (length >= sizeof(number)))
    {
        return false;
    }
    memcpy(number, &json[token->start], length);
    number[length] = '\0';
    parsed = strtol(number, &end, 10);
    if ((end == number) || ('\0' != *end))
    {
        return false;
    }
    *value = (int) parsed;
    return true;
}

static bool extract_temperature(char const * source,
                                char * destination,
                                size_t capacity)
{
    size_t length = 0U;
    bool started = false;
    for (size_t index = 0U; source[index] != '\0'; index++)
    {
        char const ch = source[index];
        if (((ch >= '0') && (ch <= '9')) || (!started && ('-' == ch)))
        {
            if ((length + 2U) > capacity)
            {
                return false;
            }
            destination[length++] = ch;
            started = true;
        }
        else if (started)
        {
            break;
        }
    }
    destination[length] = '\0';
    return (length > 0U);
}

static bool parse_weather_json(size_t json_length, weather_data_t * result)
{
    int const token_count = json_parse(s_json_buffer, json_length,
                                       s_json_tokens, WEATHER_JSON_TOKEN_COUNT);
    int status_index;
    int city_info_index;
    int city_index;
    int data_index;
    int temperature_index;
    int forecast_index;
    int status;

    if ((token_count <= 0) || (s_json_tokens[0].type != JSON_TOKEN_OBJECT))
    {
        return false;
    }
    status_index = json_object_get(s_json_buffer, s_json_tokens, token_count, 0, "status");
    city_info_index = json_object_get(s_json_buffer, s_json_tokens, token_count, 0, "cityInfo");
    data_index = json_object_get(s_json_buffer, s_json_tokens, token_count, 0, "data");
    if ((status_index < 0) || (city_info_index < 0) || (data_index < 0) ||
        !json_token_to_int(s_json_buffer, &s_json_tokens[status_index], &status) ||
        (200 != status))
    {
        return false;
    }

    city_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                 city_info_index, "city");
    temperature_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                        data_index, "wendu");
    forecast_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                     data_index, "forecast");
    if ((city_index < 0) || (temperature_index < 0) || (forecast_index < 0) ||
        (s_json_tokens[forecast_index].type != JSON_TOKEN_ARRAY))
    {
        return false;
    }

    memset(result, 0, sizeof(*result));
    if (!json_copy_string(s_json_buffer, &s_json_tokens[city_index],
                          result->city, sizeof(result->city)) ||
        !json_copy_string(s_json_buffer, &s_json_tokens[temperature_index],
                          result->current_temperature,
                          sizeof(result->current_temperature)))
    {
        return false;
    }

    for (uint32_t day = 0U; day < WEATHER_FORECAST_DAY_COUNT; day++)
    {
        char high_text[32];
        char low_text[32];
        int const day_index = json_array_get(s_json_tokens, token_count,
                                             forecast_index, day);
        int condition_index;
        int high_index;
        int low_index;
        if ((day_index < 0) || (s_json_tokens[day_index].type != JSON_TOKEN_OBJECT))
        {
            return false;
        }
        condition_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                          day_index, "type");
        high_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                     day_index, "high");
        low_index = json_object_get(s_json_buffer, s_json_tokens, token_count,
                                    day_index, "low");
        if ((condition_index < 0) || (high_index < 0) || (low_index < 0) ||
            !json_copy_string(s_json_buffer, &s_json_tokens[condition_index],
                              result->forecast[day].condition,
                              sizeof(result->forecast[day].condition)) ||
            !json_copy_string(s_json_buffer, &s_json_tokens[high_index],
                              high_text, sizeof(high_text)) ||
            !json_copy_string(s_json_buffer, &s_json_tokens[low_index],
                              low_text, sizeof(low_text)) ||
            !extract_temperature(high_text,
                                 result->forecast[day].high_temperature,
                                 sizeof(result->forecast[day].high_temperature)) ||
            !extract_temperature(low_text,
                                 result->forecast[day].low_temperature,
                                 sizeof(result->forecast[day].low_temperature)))
        {
            return false;
        }

        if (0U == day)
        {
            int const direction_index = json_object_get(s_json_buffer, s_json_tokens,
                                                        token_count, day_index, "fx");
            int const force_index = json_object_get(s_json_buffer, s_json_tokens,
                                                    token_count, day_index, "fl");
            if (direction_index >= 0)
            {
                (void) json_copy_string(s_json_buffer, &s_json_tokens[direction_index],
                                        result->wind_direction,
                                        sizeof(result->wind_direction));
            }
            if (force_index >= 0)
            {
                (void) json_copy_string(s_json_buffer, &s_json_tokens[force_index],
                                        result->wind_force,
                                        sizeof(result->wind_force));
            }
        }
    }
    return true;
}

bool weather_client_init(void)
{
    if (s_initialized)
    {
        return true;
    }
    set_last_error(NULL);
    s_busy = false;
    s_retry_pending = false;
    s_result_ready = false;
    s_next_request_allowed_ms = 0U;
    esp_time_sync_init();
    s_initialized = esp_transport_init();
    if (!s_initialized)
    {
        set_last_error("UART_INIT_FAILED");
    }
    return s_initialized;
}

void weather_client_poll(uint32_t now_ms)
{
    esp_transport_frame_t frame;

    if (!s_initialized)
    {
        return;
    }

    while (esp_transport_receive(&frame))
    {
        if ((ESP_CHANNEL_WEATHER == frame.channel) &&
            (ESP_MSG_WEATHER_DATA == frame.message))
        {
            if ((0U == frame.payload_length) ||
                (frame.payload_length >= WEATHER_JSON_BUFFER_SIZE))
            {
                set_last_error("WEATHER_JSON_SIZE");
            }
            else
            {
                memcpy(s_json_buffer, frame.payload, frame.payload_length);
                s_json_buffer[frame.payload_length] = '\0';
                if (parse_weather_json(frame.payload_length, &s_result))
                {
                    s_result_ready = true;
                    set_last_error(NULL);
                }
                else
                {
                    set_last_error("WEATHER_JSON_PARSE");
                }
            }
            s_busy = false;
            s_retry_pending = false;
            s_next_request_allowed_ms = now_ms + WEATHER_REQUEST_COOLDOWN_MS;
        }
        else if ((ESP_CHANNEL_WEATHER == frame.channel) &&
                 (ESP_MSG_TIME_DATA == frame.message))
        {
            /* ESP32-C3 NTP结果在RA8P1中解析并写入本地RTC。时间帧与天气帧
             * 共用同一个串口入口，避免两个模块同时读取传输层而丢帧。 */
            (void) esp_time_sync_handle(frame.payload,
                                        frame.payload_length,
                                        now_ms);
        }
        else if ((ESP_MSG_ERROR == frame.message) &&
                 ((ESP_CHANNEL_WEATHER == frame.channel) ||
                  (ESP_CHANNEL_CONTROL == frame.channel)))
        {
            if (!esp_time_sync_handle_error(frame.payload, frame.payload_length))
            {
                if (s_busy && payload_equals(frame.payload,
                                             frame.payload_length,
                                             "BUSY"))
                {
                    /* WebSocket shutdown on the ESP32-C3 is asynchronous.  A
                     * weather request made just after leaving chat can briefly
                     * receive BUSY; retry it inside the original timeout. */
                    s_retry_pending = true;
                    s_retry_due_ms = now_ms + WEATHER_BUSY_RETRY_MS;
                }
                else if (s_busy || (ESP_CHANNEL_WEATHER == frame.channel))
                {
                    size_t const length = (frame.payload_length < (sizeof(s_last_error) - 1U)) ?
                                          frame.payload_length : (sizeof(s_last_error) - 1U);
                    memcpy(s_last_error, frame.payload, length);
                    s_last_error[length] = '\0';
                    s_busy = false;
                    s_retry_pending = false;
                    s_next_request_allowed_ms = now_ms + WEATHER_REQUEST_COOLDOWN_MS;
                }
                else
                {
                    DoubaoRealtime_HandleTransportFrame(&frame, now_ms);
                }
            }
        }
        else
        {
            /* 串口接收只在这里进行，聊天帧同步分发给豆包协议引擎。 */
            DoubaoRealtime_HandleTransportFrame(&frame, now_ms);
        }
    }

    esp_time_sync_poll(now_ms);

    if (s_busy && s_retry_pending && deadline_reached(now_ms, s_retry_due_ms))
    {
        uint8_t const request_type = WEATHER_REQUEST_CURRENT;
        s_retry_pending = false;
        if (!esp_transport_send(ESP_CHANNEL_WEATHER,
                                ESP_MSG_WEATHER_REQUEST,
                                0U,
                                &request_type,
                                sizeof(request_type)))
        {
            set_last_error("UART_REQUEST_FAILED");
            s_busy = false;
        }
    }

    if (esp_link_take_rx_overflow())
    {
        set_last_error("UART_RX_OVERFLOW");
        s_busy = false;
        s_retry_pending = false;
        DoubaoRealtime_NotifyTransportError("UART_RX_OVERFLOW");
    }
    if ((0U != esp_link_take_uart_errors()) ||
        esp_transport_take_crc_error() ||
        esp_transport_take_header_error())
    {
        set_last_error("UART_FRAME_ERROR");
        s_busy = false;
        s_retry_pending = false;
        DoubaoRealtime_NotifyTransportError("UART_FRAME_ERROR");
    }
    if (s_busy &&
        ((uint32_t) (now_ms - s_request_started_ms) >= WEATHER_REQUEST_TIMEOUT_MS))
    {
        set_last_error("REQUEST_TIMEOUT");
        s_busy = false;
        s_retry_pending = false;
        s_next_request_allowed_ms = now_ms + WEATHER_REQUEST_COOLDOWN_MS;
    }
}

bool weather_client_request(uint32_t now_ms)
{
    uint8_t const request_type = WEATHER_REQUEST_CURRENT;

    if (!s_initialized || s_busy ||
        !deadline_reached(now_ms, s_next_request_allowed_ms))
    {
        return false;
    }
    set_last_error(NULL);
    s_result_ready = false;
    s_busy = true;
    s_retry_pending = false;
    s_request_started_ms = now_ms;
    if (!esp_transport_send(ESP_CHANNEL_WEATHER,
                            ESP_MSG_WEATHER_REQUEST,
                            0U,
                            &request_type,
                            sizeof(request_type)))
    {
        set_last_error("UART_REQUEST_FAILED");
        s_busy = false;
        s_retry_pending = false;
        return false;
    }
    return true;
}

bool weather_client_is_busy(void)
{
    return s_busy;
}

bool weather_client_take_data(weather_data_t * data)
{
    if ((NULL == data) || !s_result_ready)
    {
        return false;
    }
    *data = s_result;
    s_result_ready = false;
    return true;
}

char const * weather_client_last_error(void)
{
    return s_last_error;
}
