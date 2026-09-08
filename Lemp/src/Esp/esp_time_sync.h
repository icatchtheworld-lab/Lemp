#ifndef ESP_ESP_TIME_SYNC_H_
#define ESP_ESP_TIME_SYNC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_time_sync_init(void);
void esp_time_sync_request(uint32_t now_ms);
void esp_time_sync_poll(uint32_t now_ms);
bool esp_time_sync_handle(uint8_t const * payload,
                          uint32_t payload_length,
                          uint32_t now_ms);
bool esp_time_sync_handle_error(uint8_t const * payload, uint32_t payload_length);
bool esp_time_sync_is_synchronized(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_ESP_TIME_SYNC_H_ */
