#include "Esp/esp_link.h"

#include "hal_data.h"

#include <stddef.h>

#define ESP_LINK_RX_RING_SIZE (20480U)

static volatile uint8_t  s_rx_ring[ESP_LINK_RX_RING_SIZE];
static volatile uint32_t s_rx_write_count;
static volatile uint32_t s_rx_read_count;
static volatile bool     s_rx_overflow;
static volatile uint32_t s_uart_errors;
static volatile bool     s_tx_complete = true;
static bool              s_initialized;
static uart_cfg_t        s_uart_cfg;
static sci_b_uart_extended_cfg_t s_uart_extended_cfg;

bool esp_link_init(void)
{
    fsp_err_t err;

    if (s_initialized)
    {
        return true;
    }

    s_rx_write_count = 0U;
    s_rx_read_count = 0U;
    s_rx_overflow = false;
    s_uart_errors = 0U;
    s_tx_complete = true;

    s_uart_cfg = *g_uart1.p_cfg;
    s_uart_extended_cfg = *((sci_b_uart_extended_cfg_t const *) g_uart1.p_cfg->p_extend);
    s_uart_extended_cfg.rx_fifo_trigger = SCI_B_UART_RX_FIFO_TRIGGER_MAX;
    s_uart_cfg.p_extend = &s_uart_extended_cfg;
    s_uart_cfg.p_callback = esp_link_uart_callback;
    s_uart_cfg.p_context = NULL;

    err = g_uart1.p_api->open(g_uart1.p_ctrl, &s_uart_cfg);
    if (FSP_SUCCESS != err)
    {
        return false;
    }

    s_initialized = true;
    return true;
}

bool esp_link_write(uint8_t const * data, uint32_t length)
{
    fsp_err_t err;

    if (!s_initialized || (NULL == data) || (0U == length))
    {
        return false;
    }

    s_tx_complete = false;
    err = g_uart1.p_api->write(g_uart1.p_ctrl, data, length);
    if (FSP_SUCCESS != err)
    {
        s_tx_complete = true;
        return false;
    }
    while (!s_tx_complete)
    {
        __WFI();
    }
    return true;
}

bool esp_link_read_byte(uint8_t * byte)
{
    uint32_t interrupt_state;

    if (NULL == byte)
    {
        return false;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (s_rx_read_count == s_rx_write_count)
    {
        if (0U == interrupt_state)
        {
            __enable_irq();
        }
        return false;
    }
    *byte = s_rx_ring[s_rx_read_count % ESP_LINK_RX_RING_SIZE];
    s_rx_read_count++;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    return true;
}

bool esp_link_take_rx_overflow(void)
{
    bool overflow;
    uint32_t const interrupt_state = __get_PRIMASK();
    __disable_irq();
    overflow = s_rx_overflow;
    s_rx_overflow = false;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    return overflow;
}

uint32_t esp_link_take_uart_errors(void)
{
    uint32_t errors;
    uint32_t const interrupt_state = __get_PRIMASK();
    __disable_irq();
    errors = s_uart_errors;
    s_uart_errors = 0U;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    return errors;
}

void esp_link_discard_received(void)
{
    uint32_t const interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_rx_read_count = s_rx_write_count;
    s_rx_overflow = false;
    s_uart_errors = 0U;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
}

void esp_link_uart_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        uint32_t const write_count = s_rx_write_count;
        if ((write_count - s_rx_read_count) < ESP_LINK_RX_RING_SIZE)
        {
            s_rx_ring[write_count % ESP_LINK_RX_RING_SIZE] = (uint8_t) p_args->data;
            __DMB();
            s_rx_write_count = write_count + 1U;
        }
        else
        {
            s_rx_overflow = true;
        }
    }
    else if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        s_tx_complete = true;
    }
    else if (0U != (p_args->event & (UART_EVENT_ERR_PARITY |
                                     UART_EVENT_ERR_FRAMING |
                                     UART_EVENT_ERR_OVERFLOW |
                                     UART_EVENT_BREAK_DETECT)))
    {
        s_uart_errors |= (uint32_t) p_args->event;
    }
}
