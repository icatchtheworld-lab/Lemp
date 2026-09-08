/*
 * drv_spi_display.h
 *
 *  Created on: 2026年7月16日
 *      Author: 36315
 */

#ifndef DRV_SPI_DISPLAY_H
#define DRV_SPI_DISPLAY_H

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "hal_data.h"

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define LCD_SCREEN_WIDTH        (320)
#define LCD_SCREEN_HEIGHT       (480)

#define LCD_COLOR_RED           (0xF800)
#define LCD_COLOR_GREEN         (0x07E0)
#define LCD_COLOR_BLUE          (0x001F)

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Exported global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Exported global functions (to be accessed by other files)
 **********************************************************************************************************************/

fsp_err_t drv_spi_display_init(void);

fsp_err_t spi_display_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

fsp_err_t drv_spi_display_flush_data(uint8_t * data, uint32_t len);

/**
 * @brief SPI/DMAC传输异常后重新打开屏幕通信外设。
 *
 * 该函数只恢复SPI通信，不重新复位LCD控制器，也不会改变当前屏幕内容。
 */
fsp_err_t drv_spi_display_recover(void);

/**
 * @brief 设置屏幕背光亮度。
 *
 * @param[in] brightness_percent 背光占空比，范围为 0～100。
 *
 * @return FSP_SUCCESS 设置成功；其他返回值表示 GPT 操作失败或参数无效。
 */
fsp_err_t drv_spi_display_set_brightness(uint8_t brightness_percent);

/* Audio playback temporarily reuses GPT2, then restores the PWM backlight. */

#endif /*DRV_SPI_DISPLAY_H*/
