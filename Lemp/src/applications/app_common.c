#include "app.h"

#include "Ai/ai_book_face.h"
#include "Chat/chat_service.h"
#include "Weather/weather_client.h"
#include "Middlewares/lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static bool s_ai_book_face_enabled;
static bool s_network_transport_ready;

void app_background_set_ai_enabled(bool enabled)
{
    s_ai_book_face_enabled = enabled;
}

void app_background_process(void)
{
    uint32_t now_ms = lv_tick_get();

    /*
     * Keep the shared ESP UART, Doubao protocol, microphone upload and speaker
     * queue serviced from the foreground loop.  Running these only from an
     * LVGL timer lets a display flush delay realtime audio long enough to lose
     * UART data or starve playback.
     */
    /* Retry the shared UART transport if its first boot-time open failed. */
    if (!s_network_transport_ready)
    {
        s_network_transport_ready = weather_client_init();
    }
    if (s_network_transport_ready)
    {
        weather_client_poll(now_ms);
    }
    chat_service_poll(now_ms);

    /* AI and LVGL must keep running while the application is in a long loop. */
    if (s_ai_book_face_enabled)
    {
        (void) ai_book_face_poll();
    }

    /* After button release, keep the already-rendered "发送中" page static
     * while the last partial PCM packet and EndASR event are transmitted. */
    if (!DoubaoRealtime_UploadingRecording())
    {
        app_lvgl_process();
    }

    /* A camera inference or LCD flush may take longer than one 20 ms cloud
     * audio period.  Service the link again immediately with a fresh clock so
     * overdue upload/playback work is not delayed until the next main loop. */
    now_ms = lv_tick_get();
    if (s_network_transport_ready)
    {
        weather_client_poll(now_ms);
    }
    chat_service_poll(now_ms);
}

void app_background_delay(uint32_t milliseconds)
{
    while (milliseconds > 0U)
    {
        uint32_t const slice_ms = (milliseconds > 5U) ? 5U : milliseconds;

        app_background_process();
        R_BSP_SoftwareDelay(slice_ms, BSP_DELAY_UNITS_MILLISECONDS);
        milliseconds -= slice_ms;
    }
}

void app_fatal_error(char const * operation, fsp_err_t err)
{
    char const * name = (NULL != operation) ? operation : "Unknown operation";

    printf("[FATAL] %s failed: %d\r\n", name, (int) err);

    /*
     * A fatal application error has no automatic recovery path.  WFI keeps
     * the core available to the debugger without running a full-speed busy
     * loop.  Any enabled interrupt may wake the core, so the loop is still
     * required.
     */
    while (1)
    {
        __WFI();
    }
}
