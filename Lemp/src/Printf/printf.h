/*
 * printf.h
 *
 *  Created on: 2026年5月11日
 *      Author: 36315
 */

#ifndef PRINTF_PRINTF_H_
#define PRINTF_PRINTF_H_

#include <stdint.h>
#include "r_uart_api.h"

void Uart_Init(void);
extern volatile uint8_t g_uart9_tx_complete;
/** UART9发送完成中断超时的累计次数，可在调试器中观察。 */
extern volatile uint32_t g_uart9_tx_timeout_count;
void Uart_WriteRaw(const uint8_t *p_data, uint32_t size);
int _write(int fd,char * pBuffer,int size);
void uart9_callback(uart_callback_args_t * p_args);
void debug_assert_handler(unsigned char pass, char * file, int line);
void debug_log_handler(unsigned char pass, char * str, char * file, int line);
void debug_init(void);
#endif /* PRINTF_PRINTF_H_ */
