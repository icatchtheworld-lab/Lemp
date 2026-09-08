#ifndef ESP_ESP_LINK_H_
#define ESP_ESP_LINK_H_

#include "r_uart_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_link_init(void);
bool esp_link_write(uint8_t const * data, uint32_t length);
bool esp_link_read_byte(uint8_t * byte);
bool esp_link_take_rx_overflow(void);
uint32_t esp_link_take_uart_errors(void);
void esp_link_discard_received(void);
void esp_link_uart_callback(uart_callback_args_t * p_args);

#ifdef __cplusplus
}
#endif

#endif /* ESP_ESP_LINK_H_ */
