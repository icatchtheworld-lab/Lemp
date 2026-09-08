/*
 * drv_spi_display.c
 *
 *  Created on: 2026年7月16日
 *      Author: 36315
 */


/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "drv_spi_display.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define LCD_DC_PIN              BSP_IO_PORT_01_PIN_04
#define LCD_RESET_PIN           BSP_IO_PORT_01_PIN_06

#define SPI_SEND_DATA           BSP_IO_LEVEL_HIGH
#define SPI_SEND_CMD            BSP_IO_LEVEL_LOW
#define SPI_TX_TIMEOUT_US       (100000U)
#define SPI_TX_POLL_INTERVAL_US     (10U)

#define LCD_BACKLIGHT_DEFAULT_PERCENT    (60U)
#define LCD_BACKLIGHT_MAX_PERCENT        (100U)

/* ST7796S部分寄存器定义 */
#define LCD_DISPLAY_CMD_RAMCTRL           0xb0 // RAM Control
#define LCD_DISPLAY_CMD_CASET             0x2a // Column address set
#define LCD_DISPLAY_CMD_RASET             0x2b // Row address set
#define LCD_DISPLAY_CMD_RAMWR             0x2c // Memory write

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t spi1_wait_for_tx(void);
static fsp_err_t spi_display_init(void);

static fsp_err_t spi_send_data_cmd(uint8_t * uc_data, bsp_io_level_t uc_cmd, uint32_t len);
static fsp_err_t spi_display_backlight_pwm_init(uint8_t brightness_percent);
static fsp_err_t spi_display_reset(void);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
/* Event flags for master */
static volatile spi_event_t g_master_event_flag;    // Master Transfer Event completion flag
static bool s_backlight_pwm_open;
static uint8_t s_backlight_brightness_percent = LCD_BACKLIGHT_DEFAULT_PERCENT;

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

fsp_err_t drv_spi_display_init(void)
{
    fsp_err_t err;

    /*
     * 先以 0% 占空比启动背光 PWM，避免屏幕控制器初始化期间出现白屏闪烁。
     * P7_13 已在 FSP 中配置为 GPT2_GTIOC2A，不能再使用 GPIO pinWrite 控制。
     */
    err = spi_display_backlight_pwm_init(0U);
    if (FSP_SUCCESS != err)
    {
        printf("Backlight PWM initialization failed: %d\r\n", (int) err);
        return err;
    }

    /* Initialize the SPI display driver. */
    err = g_spi1.p_api->open(&g_spi1_ctrl, &g_spi1_cfg);
    if (FSP_SUCCESS != err)
    {
        printf ("%s %d\r\n", __FUNCTION__, __LINE__);
        return err;
    }

    err = spi_display_init();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 屏幕控制器初始化完成后，将背光设置为界面默认的 60%。 */
    return drv_spi_display_set_brightness(LCD_BACKLIGHT_DEFAULT_PERCENT);
}

fsp_err_t spi_display_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    fsp_err_t err;
    uint8_t caset[4];
    uint8_t raset[4];

    caset[0] = (uint8_t)(x1 >> 8) & 0xFF;
    caset[1] = (uint8_t)(x1 & 0xff);
    caset[2] = (uint8_t)(x2 >> 8) & 0xFF;
    caset[3] = (uint8_t)(x2 & 0xff) ;

    raset[0] = (uint8_t)(y1 >> 8) & 0xFF;
    raset[1] = (uint8_t)(y1 & 0xff);
    raset[2] = (uint8_t)(y2 >> 8) & 0xFF;
    raset[3] = (uint8_t)(y2 & 0xff);

    err = spi_send_data_cmd((uint8_t []){LCD_DISPLAY_CMD_CASET}, SPI_SEND_CMD, 1); // Horiz
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd(caset, SPI_SEND_DATA, 4);
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){LCD_DISPLAY_CMD_RASET}, SPI_SEND_CMD, 1); // Vert
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd(raset, SPI_SEND_DATA, 4);
    if (FSP_SUCCESS != err) return err;

    return spi_send_data_cmd((uint8_t []){LCD_DISPLAY_CMD_RAMWR}, SPI_SEND_CMD, 1); // Memory write
}

fsp_err_t drv_spi_display_flush_data(uint8_t * data, uint32_t len)
{
    fsp_err_t err;

    err = spi_send_data_cmd(data, SPI_SEND_DATA, len);
    if (FSP_SUCCESS != err)
    {
        printf ("%s %d\r\n", __FUNCTION__, __LINE__);
        return err;
    }

    return err;
}


void spi1_callback(spi_callback_args_t *p_args)
{
    /* 判断是否是发送完成触发的中断 */
    /* 如果是的话就将发送完成标志位置1 */
    if (SPI_EVENT_TRANSFER_COMPLETE == p_args->event)
    {
        g_master_event_flag = SPI_EVENT_TRANSFER_COMPLETE;
    }
    else
    {
        g_master_event_flag = SPI_EVENT_TRANSFER_ABORTED;
    }
}

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
static fsp_err_t spi1_wait_for_tx(void)
{
    /*
     * 窗口命令通常只需要几十微秒。原来的1 ms轮询会让LVGL每发送一个
     * 小命令都至少停顿1 ms，一次局部刷新被拆成多块后会明显阻塞舵机。
     */
    for (uint32_t elapsed_us = 0U;
         elapsed_us < SPI_TX_TIMEOUT_US;
         elapsed_us += SPI_TX_POLL_INTERVAL_US)
    {
        spi_event_t event = g_master_event_flag;
        if ((spi_event_t) 0 != event)
        {
            g_master_event_flag = (spi_event_t) 0;
            return (SPI_EVENT_TRANSFER_COMPLETE == event) ? FSP_SUCCESS : FSP_ERR_ABORTED;
        }

        R_BSP_SoftwareDelay(SPI_TX_POLL_INTERVAL_US, BSP_DELAY_UNITS_MICROSECONDS);
    }

    return FSP_ERR_TIMEOUT;
}


static fsp_err_t spi_display_init(void)
{
    fsp_err_t err = FSP_SUCCESS;

    err = spi_display_reset();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

#if 1
    err = spi_send_data_cmd((uint8_t []){0x11}, SPI_SEND_CMD, 1);     // Sleep out
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){0x20}, SPI_SEND_CMD, 1);     // 关闭显示反转
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){0x36}, SPI_SEND_CMD, 1);     // 内存数据访问控制设置
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){0x48}, SPI_SEND_DATA, 1);    // 显示方向：左->右，上->下(不旋转); BGR
    if (FSP_SUCCESS != err) return err;

    err = spi_send_data_cmd((uint8_t []){0x3a}, SPI_SEND_CMD, 1);     // 设置像素格式(bpp)
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){0x55}, SPI_SEND_DATA, 1);    // RGB接口颜色格式：16bit/pixel；控制接口的颜色格式：16bit/pixel
    if (FSP_SUCCESS != err) return err;

    err = spi_send_data_cmd((uint8_t []){0x13}, SPI_SEND_CMD, 1);     // 普通显示模式
    if (FSP_SUCCESS != err) return err;
    err = spi_send_data_cmd((uint8_t []){0x29}, SPI_SEND_CMD, 1);     // 开启显示
    if (FSP_SUCCESS != err) return err;
#else
    spi_send_data_cmd((uint8_t []){0x11}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x00}, SPI_SEND_DATA, 1);
    R_BSP_SoftwareDelay(120, BSP_DELAY_UNITS_MILLISECONDS);     //延时120ms

    spi_send_data_cmd((uint8_t []){0xf0}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0xc3}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0xf0}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x96}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0x36}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x48}, SPI_SEND_DATA, 1);    // RGB
    spi_send_data_cmd((uint8_t []){0xb4}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x01}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0xb7}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0xc6}, SPI_SEND_DATA, 1);

    spi_send_data_cmd((uint8_t []){0xe8}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, SPI_SEND_DATA, 8);

    spi_send_data_cmd((uint8_t []){0xc1}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x06}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0xc2}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0xa7}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0xc5}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x18}, SPI_SEND_DATA, 1);

    spi_send_data_cmd((uint8_t []){0xe0}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, SPI_SEND_DATA, 14);

    spi_send_data_cmd((uint8_t []){0xe1}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, SPI_SEND_DATA, 14);

    spi_send_data_cmd((uint8_t []){0xf0}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x3c}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0xf0}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x69}, SPI_SEND_DATA, 1);
    spi_send_data_cmd((uint8_t []){0x3a}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x55}, SPI_SEND_DATA, 1);
    R_BSP_SoftwareDelay(120, BSP_DELAY_UNITS_MILLISECONDS);     //延时120ms

    spi_send_data_cmd((uint8_t []){0x29}, SPI_SEND_CMD, 1);

    /*rotation*/
    spi_send_data_cmd((uint8_t []){0x36}, SPI_SEND_CMD, 1);
    spi_send_data_cmd((uint8_t []){0x48}, SPI_SEND_DATA, 1);    // 0
#endif

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    return err;
}

fsp_err_t drv_spi_display_recover(void)
{
    fsp_err_t close_err;
    fsp_err_t open_err;

    /*
     * SPI_B没有单独的Abort接口。关闭实例时FSP会同时关闭关联的DMAC，
     * 随后重新open可清除偶发超时后残留的busy/transfer状态。
     */
    close_err = g_spi1.p_api->close(g_spi1.p_ctrl);
    if ((FSP_SUCCESS != close_err) && (FSP_ERR_NOT_OPEN != close_err))
    {
        return close_err;
    }

    g_master_event_flag = (spi_event_t) 0;
    open_err = g_spi1.p_api->open(g_spi1.p_ctrl, g_spi1.p_cfg);
    return open_err;
}

static fsp_err_t spi_send_data_cmd(uint8_t * uc_data, bsp_io_level_t uc_cmd, uint32_t len)
{
    fsp_err_t err = FSP_SUCCESS;     // Error status

    /* Master send data to device */
    err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, LCD_DC_PIN, uc_cmd);
    if(FSP_SUCCESS != err)
    {
        printf ("%s %d\r\n", __FUNCTION__, __LINE__);
        return err;
    }

    g_master_event_flag = (spi_event_t) 0;
    err = g_spi1.p_api->write(g_spi1.p_ctrl, uc_data, len, SPI_BIT_WIDTH_8_BITS);
    if(FSP_SUCCESS != err)
    {
        printf ("%s %d\r\n", __FUNCTION__, __LINE__);
        return err;
    }

    err = spi1_wait_for_tx();
    if (FSP_SUCCESS != err)
    {
        printf ("%s %d, SPI event=%d\r\n", __FUNCTION__, __LINE__, (int) err);
        return err;
    }

    return err;
}

fsp_err_t drv_spi_display_set_brightness(uint8_t brightness_percent)
{
    uint32_t duty_counts;
    fsp_err_t err;

    if (brightness_percent > LCD_BACKLIGHT_MAX_PERCENT)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (!s_backlight_pwm_open)
    {
        return FSP_ERR_NOT_OPEN;
    }

    /*
     * LVGL 滑条使用 0～100 的百分比，而 FSP 需要一个周期内的高电平计数值。
     * 使用 64 位中间值，避免周期计数与百分比相乘时发生溢出。
     */
    duty_counts = (uint32_t) (((uint64_t) g_backlight_pwm_cfg.period_counts * brightness_percent) /
                              LCD_BACKLIGHT_MAX_PERCENT);

    err = g_backlight_pwm.p_api->dutyCycleSet(g_backlight_pwm.p_ctrl,
                                              duty_counts,
                                              GPT_IO_PIN_GTIOCA);
    if (FSP_SUCCESS == err)
    {
        s_backlight_brightness_percent = brightness_percent;
    }
    return err;
}

static fsp_err_t spi_display_backlight_pwm_init(uint8_t brightness_percent)
{
    fsp_err_t err;

    if (brightness_percent > LCD_BACKLIGHT_MAX_PERCENT)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (s_backlight_pwm_open)
    {
        return drv_spi_display_set_brightness(brightness_percent);
    }

    /* 打开 FSP 生成的 GPT2 背光 PWM 实例。 */
    err = g_backlight_pwm.p_api->open(g_backlight_pwm.p_ctrl, g_backlight_pwm.p_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_backlight_pwm_open = true;

    /* 在启动计数器前设置初始占空比，确保背光从期望亮度开始输出。 */
    err = drv_spi_display_set_brightness(brightness_percent);
    if (FSP_SUCCESS != err)
    {
        (void) g_backlight_pwm.p_api->close(g_backlight_pwm.p_ctrl);
        s_backlight_pwm_open = false;
        return err;
    }

    err = g_backlight_pwm.p_api->start(g_backlight_pwm.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) g_backlight_pwm.p_api->close(g_backlight_pwm.p_ctrl);
        s_backlight_pwm_open = false;
        return err;
    }

    return FSP_SUCCESS;
}


static fsp_err_t spi_display_reset(void)
{
    fsp_err_t err = FSP_SUCCESS;     // Error status

    err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, LCD_RESET_PIN, BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    R_BSP_SoftwareDelay(120, BSP_DELAY_UNITS_MILLISECONDS); //延时120ms
    err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, LCD_RESET_PIN, BSP_IO_LEVEL_HIGH);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    R_BSP_SoftwareDelay(120, BSP_DELAY_UNITS_MILLISECONDS); //延时120ms

    return err;
}
