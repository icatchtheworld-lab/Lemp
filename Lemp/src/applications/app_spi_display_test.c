/*
 * app_spi_display_test.c
 *
 *  Created on: 2026年7月16日
 *      Author: 36315
 */


/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "app.h"
#include "Printf/printf.h"
#include "Screen/drv_spi_display.h"
#include <stdio.h>


/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static void spi_display_show_color(uint16_t color_le);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/
void app_spi_display_test(void)
{
    fsp_err_t err = drv_spi_display_init();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("SPI display test initialization", err);
    }

    while(1)
    {
        spi_display_show_color((uint16_t)LCD_COLOR_RED);
        printf ("Full screen display in red\r\n");
        R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); //延时500ms

        spi_display_show_color((uint16_t)LCD_COLOR_GREEN);
        printf ("Full screen display in green\r\n");
        R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); //延时500ms

        spi_display_show_color((uint16_t)LCD_COLOR_BLUE);
        printf ("Full screen display in blue\r\n");
        R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS); //延时500ms
    }
}


static void spi_display_show_color(uint16_t color_le)
{
    uint8_t color_be[2];
    color_be [0] = (uint8_t)((color_le & 0xff00) >> 8);
    color_be [1] = (uint8_t)(color_le & 0xff);

    spi_display_set_window(0, 0, LCD_SCREEN_WIDTH-1, LCD_SCREEN_HEIGHT-1);

    for(uint16_t x = 0; x < LCD_SCREEN_WIDTH; x++)
        for(uint16_t y = 0; y < LCD_SCREEN_HEIGHT; y++)
            drv_spi_display_flush_data(color_be, 2);
}
/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
