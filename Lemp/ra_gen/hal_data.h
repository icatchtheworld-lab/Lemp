/* generated HAL header file - do not edit */
#ifndef HAL_DATA_H_
#define HAL_DATA_H_
#include <stdint.h>
#include "bsp_api.h"
#include "common_data.h"
#include "r_i2s_api.h"
#include "r_ssi.h"
#include "r_sci_b_uart.h"
#include "r_uart_api.h"
#include "r_gpt.h"
#include "r_timer_api.h"
#include "r_adc_b.h"
#include "r_adc_api.h"
#include "r_ospi_b.h"
#include "r_spi_flash_api.h"
#include "r_dmac.h"
#include "r_transfer_api.h"
#include "r_spi_b.h"
#include "r_rtc.h"
#include "r_rtc_api.h"
#include "r_capture_api.h"
#include "r_ceu.h"
#include "r_iic_master.h"
#include "r_i2c_master_api.h"
FSP_HEADER
/** SSI Instance. */
extern const i2s_instance_t g_i2s0;

/** Access the SSI instance using these structures when calling API functions directly (::p_api is not used). */
extern ssi_instance_ctrl_t g_i2s0_ctrl;
extern const i2s_cfg_t g_i2s0_cfg;

#ifndef alarm_tone_i2s_callback
void alarm_tone_i2s_callback(i2s_callback_args_t *p_args);
#endif
/** UART on SCI Instance. */
extern const uart_instance_t g_uart1;

/** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
extern sci_b_uart_instance_ctrl_t g_uart1_ctrl;
extern const uart_cfg_t g_uart1_cfg;
extern const sci_b_uart_extended_cfg_t g_uart1_cfg_extend;

#ifndef esp_link_uart_callback
void esp_link_uart_callback(uart_callback_args_t *p_args);
#endif
/** Timer on GPT Instance. */
extern const timer_instance_t g_light_bulb_pwm;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_light_bulb_pwm_ctrl;
extern const timer_cfg_t g_light_bulb_pwm_cfg;

#ifndef NULL
void NULL(timer_callback_args_t *p_args);
#endif
/** ADC on ADC_B instance. */
extern const adc_instance_t g_adc0;

/** Access the ADC_B instance using these structures when calling API functions directly (::p_api is not used). */
extern adc_b_instance_ctrl_t g_adc0_ctrl;
extern const adc_cfg_t g_adc0_cfg;
extern const adc_b_scan_cfg_t g_adc0_scan_cfg;

#ifndef NULL
void NULL(adc_callback_args_t *p_args);
#endif
/** Timer on GPT Instance. */
extern const timer_instance_t g_backlight_pwm;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_backlight_pwm_ctrl;
extern const timer_cfg_t g_backlight_pwm_cfg;

#ifndef NULL
void NULL(timer_callback_args_t *p_args);
#endif
#if OSPI_B_CFG_DMAC_SUPPORT_ENABLE
    #include "r_dmac.h"
#endif
#if OSPI_CFG_DOTF_SUPPORT_ENABLE
    #include "r_sce_if.h"
#endif

extern const spi_flash_instance_t g_ospi0;
extern ospi_b_instance_ctrl_t g_ospi0_ctrl;
extern const spi_flash_cfg_t g_ospi0_cfg;
/** Timer on GPT Instance. */
extern const timer_instance_t g_timer0;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_timer0_ctrl;
extern const timer_cfg_t g_timer0_cfg;

#ifndef periodic_timer0_cb
void periodic_timer0_cb(timer_callback_args_t *p_args);
#endif
/* Transfer on DMAC Instance. */
extern const transfer_instance_t g_transfer1;

/** Access the DMAC instance using these structures when calling API functions directly (::p_api is not used). */
extern dmac_instance_ctrl_t g_transfer1_ctrl;
extern const transfer_cfg_t g_transfer1_cfg;

#ifndef g_spi1_tx_transfer_callback
void g_spi1_tx_transfer_callback(transfer_callback_args_t *p_args);
#endif
/** SPI on SPI Instance. */
extern const spi_instance_t g_spi1;

/** Access the SPI instance using these structures when calling API functions directly (::p_api is not used). */
extern spi_b_instance_ctrl_t g_spi1_ctrl;
extern const spi_cfg_t g_spi1_cfg;

/** Callback used by SPI Instance. */
#ifndef spi1_callback
void spi1_callback(spi_callback_args_t *p_args);
#endif

#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == g_transfer1)
    #define g_spi1_P_TRANSFER_TX (NULL)
#else
#define g_spi1_P_TRANSFER_TX (&g_transfer1)
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
#define g_spi1_P_TRANSFER_RX (NULL)
#else
    #define g_spi1_P_TRANSFER_RX (&RA_NOT_DEFINED)
#endif
#undef RA_NOT_DEFINED
/* RTC Instance. */
extern const rtc_instance_t g_rtc0;

/** Access the RTC instance using these structures when calling API functions directly (::p_api is not used). */
extern rtc_instance_ctrl_t g_rtc0_ctrl;
extern const rtc_cfg_t g_rtc0_cfg;

#ifndef NULL
void NULL(rtc_callback_args_t *p_args);
#endif
/** UART on SCI Instance. */
extern const uart_instance_t g_uart9;

/** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
extern sci_b_uart_instance_ctrl_t g_uart9_ctrl;
extern const uart_cfg_t g_uart9_cfg;
extern const sci_b_uart_extended_cfg_t g_uart9_cfg_extend;

#ifndef uart9_callback
void uart9_callback(uart_callback_args_t *p_args);
#endif
/* CEU on CAPTURE instance */
extern const capture_instance_t g_ceu0;
/* Access the CEU instance using these structures when calling API functions directly (::p_api is not used). */
extern ceu_instance_ctrl_t g_ceu0_ctrl;
extern const capture_cfg_t g_ceu0_cfg;
#ifndef g_ceu0_user_callback
void g_ceu0_user_callback(capture_callback_args_t *p_args);
#endif
/* I2C Master on IIC Instance. */
extern const i2c_master_instance_t g_i2c_master1;

/** Access the I2C Master instance using these structures when calling API functions directly (::p_api is not used). */
extern iic_master_instance_ctrl_t g_i2c_master1_ctrl;
extern const i2c_master_cfg_t g_i2c_master1_cfg;

#ifndef i2c_master2_callback
void i2c_master2_callback(i2c_master_callback_args_t *p_args);
#endif
/** Timer on GPT Instance. */
extern const timer_instance_t g_audio_clock_timer;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_audio_clock_timer_ctrl;
extern const timer_cfg_t g_audio_clock_timer_cfg;

#ifndef NULL
void NULL(timer_callback_args_t *p_args);
#endif
/** Timer on GPT Instance. */
extern const timer_instance_t g_timer1;

/** Access the GPT instance using these structures when calling API functions directly (::p_api is not used). */
extern gpt_instance_ctrl_t g_timer1_ctrl;
extern const timer_cfg_t g_timer1_cfg;

#ifndef NULL
void NULL(timer_callback_args_t *p_args);
#endif
/** SSI Instance. */
extern const i2s_instance_t g_i2s1;

/** Access the SSI instance using these structures when calling API functions directly (::p_api is not used). */
extern ssi_instance_ctrl_t g_i2s1_ctrl;
extern const i2s_cfg_t g_i2s1_cfg;

#ifndef voice_i2s_callback
void voice_i2s_callback(i2s_callback_args_t *p_args);
#endif
/** UART on SCI Instance. */
extern const uart_instance_t g_uart0;

/** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
extern sci_b_uart_instance_ctrl_t g_uart0_ctrl;
extern const uart_cfg_t g_uart0_cfg;
extern const sci_b_uart_extended_cfg_t g_uart0_cfg_extend;

#ifndef g_uart0_callback
void g_uart0_callback(uart_callback_args_t *p_args);
#endif
void hal_entry(void);
void g_hal_init(void);
FSP_FOOTER
#endif /* HAL_DATA_H_ */
