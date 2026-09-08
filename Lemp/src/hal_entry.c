#include "hal_data.h"

#include "Printf/printf.h"
#include "applications/app.h"

#include <stdio.h>

/* Select exactly one application mode. */
#define MIC_UART_EXPORT_TEST_ENABLE       (0U)
#define CAMERA_LCD_PREVIEW_TEST_ENABLE    (0U)
#define MAIN_APP_ENABLE                   (1U)
#define RTC_TEST_ENABLE                   (0U)
#define Flash_TEST_ENABLE                  (0U)
#define LED_BLINK_TEST_ENABLE              (0U)
#define MUSIC_TEST_ENABLE                 (0U)
#define BUZZER_TEST_ENABLE                 (0U)

#if ((MIC_UART_EXPORT_TEST_ENABLE + CAMERA_LCD_PREVIEW_TEST_ENABLE + MAIN_APP_ENABLE + RTC_TEST_ENABLE + \
      Flash_TEST_ENABLE + LED_BLINK_TEST_ENABLE + MUSIC_TEST_ENABLE+BUZZER_TEST_ENABLE) != 1U)
#error "Enable exactly one application/test mode."
#endif

void hal_entry(void)
{
    Uart_Init();
    printf("UART9 started\r\n");

#if MIC_UART_EXPORT_TEST_ENABLE
    app_mic_uart_export_test();
#elif CAMERA_LCD_PREVIEW_TEST_ENABLE
    app_camera_lcd_preview_test();
#elif MAIN_APP_ENABLE
    app_main_run();
#elif RTC_TEST_ENABLE
    app_rtc_test();
#elif Flash_TEST_ENABLE
    app_test_flash();
#elif LED_BLINK_TEST_ENABLE
    app_led_blink_test();
#elif MUSIC_TEST_ENABLE
    /* Keep this as a selectable standalone test while MAIN_APP_ENABLE stays default. */
    app_music_onset_test();
#elif BUZZER_TEST_ENABLE

#endif

    app_fatal_error("Selected application returned unexpectedly", FSP_ERR_INTERNAL);
}

#if BSP_TZ_SECURE_BUILD

FSP_CPP_HEADER
BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ();

BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ()
{
}
FSP_CPP_FOOTER

#endif
