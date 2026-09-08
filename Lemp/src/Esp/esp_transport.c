#include "Esp/esp_transport.h"

#include "Esp/esp_link.h"

#include <stddef.h>
#include <string.h>

#define TRANSPORT_MAGIC_0     (0xA5U)
#define TRANSPORT_MAGIC_1     (0x5AU)
#define TRANSPORT_VERSION     (1U)
#define TRANSPORT_HEADER_SIZE (12U)
#define TRANSPORT_CRC_SIZE    (4U)
#define TRANSPORT_CHANNELS    (3U)
#define WS_OPEN_BUFFER_SIZE   (1800U)

typedef enum e_transport_rx_state
{
    TRANSPORT_RX_MAGIC_0 = 0,
    TRANSPORT_RX_MAGIC_1,
    TRANSPORT_RX_HEADER,
    TRANSPORT_RX_PAYLOAD,
    TRANSPORT_RX_CRC,
} transport_rx_state_t;

static transport_rx_state_t s_rx_state;
static uint8_t  s_header[TRANSPORT_HEADER_SIZE];
static uint32_t s_header_received;
static uint8_t  s_payload[ESP_TRANSPORT_MAX_PAYLOAD];
static uint32_t s_payload_length;
static uint32_t s_payload_received;
static uint8_t  s_crc_bytes[TRANSPORT_CRC_SIZE];
static uint32_t s_crc_received;
static uint32_t s_frame_crc;
static uint16_t s_tx_sequence[TRANSPORT_CHANNELS];
static bool s_crc_error;
static bool s_header_error;
static bool s_initialized;

static void write_be16(uint8_t * destination, uint16_t value)
{
    destination[0] = (uint8_t) (value >> 8U);
    destination[1] = (uint8_t) value;
}

static void write_be32(uint8_t * destination, uint32_t value)
{
    destination[0] = (uint8_t) (value >> 24U);
    destination[1] = (uint8_t) (value >> 16U);
    destination[2] = (uint8_t) (value >> 8U);
    destination[3] = (uint8_t) value;
}

static uint16_t read_be16(uint8_t const * source)
{
    return (uint16_t) (((uint16_t) source[0] << 8U) | source[1]);
}

static uint32_t read_be32(uint8_t const * source)
{
    return ((uint32_t) source[0] << 24U) |
           ((uint32_t) source[1] << 16U) |
           ((uint32_t) source[2] << 8U) |
           (uint32_t) source[3];
}

static uint32_t crc32_update(uint32_t crc, uint8_t const * data, uint32_t length)
{
    for (uint32_t index = 0U; index < length; index++)
    {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static void parser_reset(void)
{
    s_rx_state = TRANSPORT_RX_MAGIC_0;
    s_header_received = 0U;
    s_payload_length = 0U;
    s_payload_received = 0U;
    s_crc_received = 0U;
    s_frame_crc = 0xFFFFFFFFU;
}

bool esp_transport_init(void)
{
    if (s_initialized)
    {
        return true;
    }
    if (!esp_link_init())
    {
        return false;
    }
    memset(s_tx_sequence, 0, sizeof(s_tx_sequence));
    s_crc_error = false;
    s_header_error = false;
    esp_link_discard_received();
    parser_reset();
    s_initialized = true;
    return true;
}

bool esp_transport_send(uint8_t channel,
                        uint8_t message,
                        uint8_t flags,
                        uint8_t const * payload,
                        uint32_t payload_length)
{
    uint8_t header[TRANSPORT_HEADER_SIZE];
    uint8_t crc_bytes[TRANSPORT_CRC_SIZE];
    uint32_t crc;

    if ((channel >= TRANSPORT_CHANNELS) ||
        (payload_length > ESP_TRANSPORT_MAX_PAYLOAD) ||
        ((payload_length > 0U) && (NULL == payload)))
    {
        return false;
    }

    header[0] = TRANSPORT_MAGIC_0;
    header[1] = TRANSPORT_MAGIC_1;
    header[2] = TRANSPORT_VERSION;
    header[3] = message;
    header[4] = channel;
    header[5] = flags;
    write_be16(&header[6], ++s_tx_sequence[channel]);
    write_be32(&header[8], payload_length);

    crc = crc32_update(0xFFFFFFFFU, &header[2], TRANSPORT_HEADER_SIZE - 2U);
    if (payload_length > 0U)
    {
        crc = crc32_update(crc, payload, payload_length);
    }
    write_be32(crc_bytes, crc ^ 0xFFFFFFFFU);

    return esp_link_write(header, sizeof(header)) &&
           ((0U == payload_length) || esp_link_write(payload, payload_length)) &&
           esp_link_write(crc_bytes, sizeof(crc_bytes));
}

bool esp_transport_ws_open(char const * host,
                           uint16_t port,
                           char const * path,
                           char const * headers)
{
    static uint8_t metadata[WS_OPEN_BUFFER_SIZE];
    size_t host_length;
    size_t path_length;
    size_t headers_length;
    size_t total_length;
    size_t offset = 8U;

    if ((NULL == host) || (NULL == path) || (NULL == headers) || (0U == port))
    {
        return false;
    }

    host_length = strlen(host);
    path_length = strlen(path);
    headers_length = strlen(headers);
    total_length = 8U + host_length + path_length;
    if ((0U == host_length) || (0U == path_length) ||
        (host_length > UINT16_MAX) || (path_length > UINT16_MAX) ||
        (headers_length > ESP_TRANSPORT_MAX_PAYLOAD) ||
        (total_length > sizeof(metadata)))
    {
        return false;
    }

    if (!esp_transport_send(ESP_CHANNEL_DOUBAO,
                            ESP_MSG_WS_CONFIG,
                            0U,
                            (uint8_t const *) headers,
                            (uint32_t) headers_length))
    {
        return false;
    }

    write_be16(&metadata[0], port);
    write_be16(&metadata[2], (uint16_t) host_length);
    write_be16(&metadata[4], (uint16_t) path_length);
    write_be16(&metadata[6], 0U);
    memcpy(&metadata[offset], host, host_length);
    offset += host_length;
    memcpy(&metadata[offset], path, path_length);

    return esp_transport_send(ESP_CHANNEL_DOUBAO,
                              ESP_MSG_WS_OPEN,
                              0U,
                              metadata,
                              (uint32_t) total_length);
}

static bool parser_accept_byte(uint8_t byte, esp_transport_frame_t * frame)
{
    switch (s_rx_state)
    {
        case TRANSPORT_RX_MAGIC_0:
            if (TRANSPORT_MAGIC_0 == byte)
            {
                s_header[0] = byte;
                s_rx_state = TRANSPORT_RX_MAGIC_1;
            }
            break;

        case TRANSPORT_RX_MAGIC_1:
            if (TRANSPORT_MAGIC_1 == byte)
            {
                s_header[1] = byte;
                s_header_received = 2U;
                s_rx_state = TRANSPORT_RX_HEADER;
            }
            else if (TRANSPORT_MAGIC_0 != byte)
            {
                s_rx_state = TRANSPORT_RX_MAGIC_0;
            }
            break;

        case TRANSPORT_RX_HEADER:
            s_header[s_header_received++] = byte;
            if (TRANSPORT_HEADER_SIZE == s_header_received)
            {
                s_payload_length = read_be32(&s_header[8]);
                if ((TRANSPORT_VERSION != s_header[2]) ||
                    (s_header[4] >= TRANSPORT_CHANNELS) ||
                    (s_payload_length > ESP_TRANSPORT_MAX_PAYLOAD))
                {
                    s_header_error = true;
                    parser_reset();
                    break;
                }
                s_frame_crc = crc32_update(0xFFFFFFFFU,
                                           &s_header[2],
                                           TRANSPORT_HEADER_SIZE - 2U);
                s_payload_received = 0U;
                s_crc_received = 0U;
                s_rx_state = (0U == s_payload_length) ?
                             TRANSPORT_RX_CRC : TRANSPORT_RX_PAYLOAD;
            }
            break;

        case TRANSPORT_RX_PAYLOAD:
            s_payload[s_payload_received++] = byte;
            s_frame_crc = crc32_update(s_frame_crc, &byte, 1U);
            if (s_payload_received == s_payload_length)
            {
                s_rx_state = TRANSPORT_RX_CRC;
            }
            break;

        case TRANSPORT_RX_CRC:
            s_crc_bytes[s_crc_received++] = byte;
            if (TRANSPORT_CRC_SIZE == s_crc_received)
            {
                uint32_t const expected_crc = read_be32(s_crc_bytes);
                uint32_t const actual_crc = s_frame_crc ^ 0xFFFFFFFFU;
                if (expected_crc == actual_crc)
                {
                    frame->message = s_header[3];
                    frame->channel = s_header[4];
                    frame->flags = s_header[5];
                    frame->sequence = read_be16(&s_header[6]);
                    frame->payload_length = s_payload_length;
                    frame->payload = s_payload;
                    parser_reset();
                    return true;
                }
                s_crc_error = true;
                parser_reset();
            }
            break;

        default:
            parser_reset();
            break;
    }
    return false;
}

bool esp_transport_receive(esp_transport_frame_t * frame)
{
    uint8_t byte;
    if (NULL == frame)
    {
        return false;
    }
    while (esp_link_read_byte(&byte))
    {
        if (parser_accept_byte(byte, frame))
        {
            return true;
        }
    }
    return false;
}

bool esp_transport_take_crc_error(void)
{
    bool const error = s_crc_error;
    s_crc_error = false;
    return error;
}

bool esp_transport_take_header_error(void)
{
    bool const error = s_header_error;
    s_header_error = false;
    return error;
}
