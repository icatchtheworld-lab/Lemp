#include "Printf/printf.h"
#include "hal_data.h"
#include <stdbool.h>
#include <stddef.h>

volatile uint8_t g_uart9_tx_complete = 1;
volatile uint32_t g_uart9_tx_timeout_count = 0U;

/*
 * 115200波特率下发送一个字节约需要87 us。这里按每字节120 us计算，
 * 再额外保留5 ms中断调度余量。调试串口异常时只丢失本条日志，不能
 * 让主循环、LVGL和舵机控制永久停在等待发送完成的空循环中。
 */
#define UART9_TX_TIMEOUT_BASE_US       (5000U)
#define UART9_TX_TIMEOUT_PER_BYTE_US    (120U)
#define UART9_TX_POLL_INTERVAL_US        (10U)
#define UART9_TX_TIMEOUT_MAX_US      (500000U)

static bool uart9_wait_tx_complete(uint32_t size)
{
    uint64_t calculated_timeout = UART9_TX_TIMEOUT_BASE_US +
                                  ((uint64_t) size * UART9_TX_TIMEOUT_PER_BYTE_US);
    uint32_t timeout_us = (calculated_timeout > UART9_TX_TIMEOUT_MAX_US) ?
                          UART9_TX_TIMEOUT_MAX_US : (uint32_t) calculated_timeout;
    uint32_t waited_us = 0U;

    while ((!g_uart9_tx_complete) && (waited_us < timeout_us))
    {
        R_BSP_SoftwareDelay(UART9_TX_POLL_INTERVAL_US, BSP_DELAY_UNITS_MICROSECONDS);
        waited_us += UART9_TX_POLL_INTERVAL_US;
    }

    if (g_uart9_tx_complete)
    {
        return true;
    }

    /*
     * 发送完成中断偶尔丢失时主动终止本次发送，释放SCI驱动状态。
     * 这里不能再调用printf报告错误，否则会递归进入同一个故障路径。
     */
    (void) g_uart9.p_api->communicationAbort(g_uart9.p_ctrl, UART_DIR_TX);
    g_uart9_tx_complete = 1U;
    g_uart9_tx_timeout_count++;
    return false;
}

void Uart_Init(void)
{
    g_uart9.p_api->open(g_uart9.p_ctrl, g_uart9.p_cfg);
}

int _write(int fd,char * pBuffer,int size)
{
    fsp_err_t err;

    (void) fd;
    if ((NULL == pBuffer) || (size <= 0))
    {
        return 0;
    }

    g_uart9_tx_complete = 0;
    err = g_uart9.p_api->write(g_uart9.p_ctrl, (uint8_t const *) pBuffer, (uint32_t) size);
    if (FSP_SUCCESS != err)
    {
        g_uart9_tx_complete = 1;
        return -1;
    }

    if (!uart9_wait_tx_complete((uint32_t) size))
    {
        return -1;
    }

    return size;
}
//回调函数写在了zf_common/zf_common_debug.c

void Uart_WriteRaw(const uint8_t *p_data, uint32_t size)
{
    uint32_t sent = 0;

    if ((NULL == p_data) || (0U == size))
    {
        return;
    }

    while (sent < size)
    {
        uint32_t chunk = size - sent;
        if (chunk > 64) chunk = 64;

        g_uart9_tx_complete = 0;
        if (FSP_SUCCESS != g_uart9.p_api->write(g_uart9.p_ctrl,
                                                (uint8_t const *) (p_data + sent),
                                                chunk))
        {
            g_uart9_tx_complete = 1U;
            return;
        }

        if (!uart9_wait_tx_complete(chunk))
        {
            return;
        }
        sent += chunk;
    }
}
