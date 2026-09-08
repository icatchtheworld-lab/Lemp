#include "app.h"

#include "Printf/printf.h"
#include "Voice/voice.h"

#include <stdint.h>
#include <stdio.h>

#define MIC_UART_EXPORT_SAMPLE_RATE      (16000U)
#define MIC_UART_EXPORT_SECONDS          (10U)
#define MIC_UART_EXPORT_SAMPLE_COUNT     (MIC_UART_EXPORT_SAMPLE_RATE * MIC_UART_EXPORT_SECONDS)
#define MIC_UART_EXPORT_FRAME_CAPACITY   (128U)

static int16_t s_mic_uart_export_pcm[MIC_UART_EXPORT_SAMPLE_COUNT];
static voice_sample_t s_voice_frame[MIC_UART_EXPORT_FRAME_CAPACITY];

void app_mic_uart_export_test(void)
{
    uint32_t captured = 0U;
    fsp_err_t err;

    Voice_ChannelSet(VOICE_CHANNEL_RIGHT);
    err = Voice_Start();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Microphone start", err);
    }

    printf("MIC capture start\r\n");

    while (captured < MIC_UART_EXPORT_SAMPLE_COUNT)
    {
        uint32_t frame_count;

        if (!Voice_FrameReady())
        {
            __WFI();
            continue;
        }

        frame_count = Voice_FrameRead(s_voice_frame, MIC_UART_EXPORT_FRAME_CAPACITY);
        for (uint32_t i = 0U; (i < frame_count) && (captured < MIC_UART_EXPORT_SAMPLE_COUNT); i++)
        {
            s_mic_uart_export_pcm[captured++] = s_voice_frame[i];
        }
    }

    printf("AUDIO_BEGIN %lu\r\n", (unsigned long) (captured * sizeof(s_mic_uart_export_pcm[0])));
    Uart_WriteRaw((uint8_t const *) s_mic_uart_export_pcm, captured * sizeof(s_mic_uart_export_pcm[0]));
    printf("\r\nAUDIO_END\r\n");
    printf("MIC export done\r\n");

    while (1)
    {
        __WFI();
    }
}
