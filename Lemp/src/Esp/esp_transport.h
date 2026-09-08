#ifndef ESP_ESP_TRANSPORT_H_
#define ESP_ESP_TRANSPORT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_TRANSPORT_MAX_PAYLOAD (16384U)

#define ESP_CHANNEL_CONTROL (0U)
#define ESP_CHANNEL_DOUBAO  (1U)
#define ESP_CHANNEL_WEATHER (2U)

#define ESP_MSG_WS_CONFIG       (0x0FU)
#define ESP_MSG_WS_OPEN         (0x10U)
#define ESP_MSG_WS_SEND         (0x11U)
#define ESP_MSG_WS_CLOSE        (0x12U)
#define ESP_MSG_WEATHER_REQUEST (0x20U)
#define ESP_MSG_TIME_REQUEST    (0x21U)
#define ESP_MSG_WS_CONFIGURED   (0x8FU)
#define ESP_MSG_READY           (0x80U)
#define ESP_MSG_WS_OPENED       (0x90U)
#define ESP_MSG_WS_DATA         (0x91U)
#define ESP_MSG_WS_CLOSED       (0x92U)
#define ESP_MSG_WS_SENT         (0x93U)
#define ESP_MSG_WEATHER_DATA    (0xA0U)
#define ESP_MSG_TIME_DATA       (0xA1U)
#define ESP_MSG_ERROR           (0xFFU)

#define ESP_WS_FLAG_TEXT (0x01U)

typedef struct st_esp_transport_frame
{
    uint8_t         message;
    uint8_t         channel;
    uint8_t         flags;
    uint16_t        sequence;
    uint32_t        payload_length;
    uint8_t const * payload;
} esp_transport_frame_t;

bool esp_transport_init(void);
bool esp_transport_send(uint8_t channel,
                        uint8_t message,
                        uint8_t flags,
                        uint8_t const * payload,
                        uint32_t payload_length);
bool esp_transport_ws_open(char const * host,
                           uint16_t port,
                           char const * path,
                           char const * headers);
bool esp_transport_receive(esp_transport_frame_t * frame);
bool esp_transport_take_crc_error(void);
bool esp_transport_take_header_error(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_ESP_TRANSPORT_H_ */
