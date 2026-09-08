#include "Alarm/alarm_tone.h"

#include "Audio/cloud_speaker.h"
#include "Screen/drv_gpt_timer.h"

#include <stdint.h>
#include <stdio.h>

#define ALARM_TONE_SAMPLE_RATE_HZ       (16000U)
#define ALARM_TONE_BLOCK_SAMPLES        (1600U) /* 100 ms */
#define ALARM_TONE_PCM_BYTES            (ALARM_TONE_BLOCK_SAMPLES * 2U)
#define ALARM_TONE_PERIOD_SAMPLES       (20U)   /* 800 Hz */
#define ALARM_TONE_AMPLITUDE            (20000)
#define ALARM_TONE_PATTERN_BLOCKS       (15U)   /* 1.5 seconds */
#define ALARM_TONE_PROMPT_BLOCKS        (7U)
#define ALARM_TONE_TARGET_BUFFER_SAMPLES \
    (ALARM_TONE_PATTERN_BLOCKS * ALARM_TONE_BLOCK_SAMPLES)
#define ALARM_TONE_REFILL_BLOCK_LIMIT   (4U)

#if ALARM_TONE_SAMPLE_RATE_HZ != CLOUD_SPEAKER_SAMPLE_RATE_HZ
#error "Alarm tone and cloud speaker sample rates must match"
#endif

/*
 * The cloud speaker path is already proven by realtime-chat playback.  The
 * alarm therefore feeds PCM16 into the same buffered output path instead of
 * maintaining a second, timing-sensitive SSI state machine.
 */
static uint8_t s_tone_pcm[ALARM_TONE_PCM_BYTES];
static uint8_t s_silence_pcm[ALARM_TONE_PCM_BYTES];
static bool s_initialized;
static bool s_active;
static bool s_one_shot;
static bool s_queue_error_reported;
static uint32_t s_pattern_block;
static uint32_t s_last_diagnostic_ms;

static void alarm_tone_pcm_build(void)
{
    for (uint32_t sample_index = 0U;
         sample_index < ALARM_TONE_BLOCK_SAMPLES;
         sample_index++)
    {
        uint32_t const phase = sample_index % ALARM_TONE_PERIOD_SAMPLES;
        int32_t sample;
        uint16_t raw;

        if (phase < (ALARM_TONE_PERIOD_SAMPLES / 2U))
        {
            sample = -ALARM_TONE_AMPLITUDE +
                     ((int32_t) phase * (2 * ALARM_TONE_AMPLITUDE) /
                      (int32_t) (ALARM_TONE_PERIOD_SAMPLES / 2U));
        }
        else
        {
            sample = ALARM_TONE_AMPLITUDE -
                     ((int32_t) (phase - (ALARM_TONE_PERIOD_SAMPLES / 2U)) *
                      (2 * ALARM_TONE_AMPLITUDE) /
                      (int32_t) (ALARM_TONE_PERIOD_SAMPLES / 2U));
        }

        raw = (uint16_t) (int16_t) sample;
        s_tone_pcm[sample_index * 2U] = (uint8_t) raw;
        s_tone_pcm[(sample_index * 2U) + 1U] = (uint8_t) (raw >> 8U);
        s_silence_pcm[sample_index * 2U] = 0U;
        s_silence_pcm[(sample_index * 2U) + 1U] = 0U;
    }
}

static uint8_t const * alarm_tone_pattern_select(uint32_t pattern_block)
{
    uint32_t const phase = pattern_block % ALARM_TONE_PATTERN_BLOCKS;

    /*
     * 0.1 s amplifier wake-up, 0.2 s chime, 0.1 s pause, 0.3 s chime,
     * then 0.8 s rest before repeating.
     */
    if (((phase >= 1U) && (phase < 3U)) ||
        ((phase >= 4U) && (phase < 7U)))
    {
        return s_tone_pcm;
    }
    return s_silence_pcm;
}

static bool alarm_tone_queue_next(void)
{
    uint8_t const * pcm = alarm_tone_pattern_select(s_pattern_block++);

    if (CloudSpeaker_EnqueuePcm16Le(pcm, ALARM_TONE_PCM_BYTES))
    {
        s_queue_error_reported = false;
        return true;
    }
    if (!s_queue_error_reported)
    {
        printf("[ALARM][AUDIO] PCM enqueue failed: fsp=%d buffered=%lu\r\n",
               (int) CloudSpeaker_LastError(),
               (unsigned long) CloudSpeaker_BufferedSamples());
        s_queue_error_reported = true;
    }
    return false;
}

bool alarm_tone_init(void)
{
    fsp_err_t err;

    if (s_initialized)
    {
        return true;
    }

    alarm_tone_pcm_build();
    err = CloudSpeaker_Init();
    if (FSP_SUCCESS != err)
    {
        printf("[ALARM][AUDIO] speaker init failed: fsp=%d\r\n", (int) err);
        return false;
    }

    s_active = false;
    s_one_shot = false;
    s_queue_error_reported = false;
    s_pattern_block = 0U;
    s_last_diagnostic_ms = 0U;
    s_initialized = true;
    printf("[ALARM][AUDIO] shared speaker path ready\r\n");
    return true;
}

static bool alarm_tone_start_internal(bool one_shot)
{
    uint32_t blocks_to_queue;

    if (!s_initialized && !alarm_tone_init())
    {
        return false;
    }
    if (s_active)
    {
        return true;
    }

    CloudSpeaker_StopAndClear();
    s_pattern_block = 0U;
    s_one_shot = one_shot;
    s_queue_error_reported = false;
    blocks_to_queue = one_shot ? ALARM_TONE_PROMPT_BLOCKS :
                      ALARM_TONE_PATTERN_BLOCKS;

    for (uint32_t block = 0U; block < blocks_to_queue; block++)
    {
        if (!alarm_tone_queue_next())
        {
            CloudSpeaker_StopAndClear();
            s_one_shot = false;
            return false;
        }
    }

    if (one_shot)
    {
        CloudSpeaker_Flush();
    }
    s_active = true;
    s_last_diagnostic_ms = drv_gpt_timer_get_ms();
    printf("[ALARM][AUDIO] started: mode=%s buffered=%lums\r\n",
           one_shot ? "prompt" : "ring",
           (unsigned long) (CloudSpeaker_BufferedSamples() * 1000U /
                            CLOUD_SPEAKER_SAMPLE_RATE_HZ));
    return true;
}

bool alarm_tone_start(void)
{
    return alarm_tone_start_internal(false);
}

bool alarm_tone_start_prompt(void)
{
    return alarm_tone_start_internal(true);
}

void alarm_tone_stop(void)
{
    if (!s_initialized)
    {
        return;
    }

    printf("[ALARM][AUDIO] final: played=%lu buffered=%lums peak=%lu "
           "gaps=%lu idle=%lu write_err=%lu last_fsp=%d\r\n",
           (unsigned long) CloudSpeaker_BlocksPlayed(),
           (unsigned long) (CloudSpeaker_BufferedSamples() * 1000U /
                            CLOUD_SPEAKER_SAMPLE_RATE_HZ),
           (unsigned long) CloudSpeaker_InputPeak(),
           (unsigned long) CloudSpeaker_GapBlockCount(),
           (unsigned long) CloudSpeaker_UnexpectedIdleCount(),
           (unsigned long) CloudSpeaker_WriteErrorCount(),
           (int) CloudSpeaker_LastError());
    s_active = false;
    s_one_shot = false;
    s_pattern_block = 0U;
    CloudSpeaker_StopAndClear();
    printf("[ALARM][AUDIO] stopped\r\n");
}

void alarm_tone_poll(void)
{
    uint8_t refill_count = 0U;
    uint32_t now_ms;

    if (!s_initialized)
    {
        return;
    }

    CloudSpeaker_Poll();
    if (!s_active)
    {
        return;
    }
    if (s_one_shot)
    {
        if (!CloudSpeaker_IsBusy())
        {
            s_active = false;
            s_one_shot = false;
        }
        return;
    }

    now_ms = drv_gpt_timer_get_ms();
    if ((uint32_t) (now_ms - s_last_diagnostic_ms) >= 1000U)
    {
        s_last_diagnostic_ms = now_ms;
        printf("[ALARM][AUDIO] stat: played=%lu buffered=%lums peak=%lu "
               "gaps=%lu idle=%lu write_err=%lu last_fsp=%d\r\n",
               (unsigned long) CloudSpeaker_BlocksPlayed(),
               (unsigned long) (CloudSpeaker_BufferedSamples() * 1000U /
                                CLOUD_SPEAKER_SAMPLE_RATE_HZ),
               (unsigned long) CloudSpeaker_InputPeak(),
               (unsigned long) CloudSpeaker_GapBlockCount(),
               (unsigned long) CloudSpeaker_UnexpectedIdleCount(),
               (unsigned long) CloudSpeaker_WriteErrorCount(),
               (int) CloudSpeaker_LastError());
    }

    while ((CloudSpeaker_BufferedSamples() < ALARM_TONE_TARGET_BUFFER_SAMPLES) &&
           (refill_count < ALARM_TONE_REFILL_BLOCK_LIMIT))
    {
        if (!alarm_tone_queue_next())
        {
            break;
        }
        refill_count++;
    }
}

void alarm_tone_i2s_callback(i2s_callback_args_t * p_args)
{
    /*
     * The generated FSP configuration still references this symbol. Runtime
     * alarm playback opens SSI through CloudSpeaker_Init(), which installs the
     * cloud speaker callback in its copied configuration.
     */
    (void) p_args;
}
