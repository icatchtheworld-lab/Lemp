#include "Audio/cloud_speaker.h"

#include "Audio/audio_output_owner.h"
#include "Audio/audio_shared_buffer.h"
#include "hal_data.h"

#include <stddef.h>

#define SPEAKER_RING_SAMPLES       (262144U)
#define SPEAKER_BLOCK_SAMPLES      (160U)
#define SPEAKER_WORDS_PER_SAMPLE   (2U)
#define SPEAKER_BLOCK_WORDS        (SPEAKER_BLOCK_SAMPLES * SPEAKER_WORDS_PER_SAMPLE)
/* Keep one second of cloud audio queued before starting SSI playback.  The
 * previous 300 ms reserve was smaller than normal WebSocket/UART jitter and
 * caused repeated ring-buffer starvation (reported by the gaps counter). */
#define SPEAKER_STARTUP_SAMPLES    (16000U)
#define SPEAKER_GAIN_SHIFT         (1U)
#define SPEAKER_TAIL_PEAK_LIMIT    (2048U)
#define SPEAKER_TAIL_MEAN_LIMIT    (256U)
#define SPEAKER_TAIL_KEEP_SAMPLES  (1600U)
#define SPEAKER_AMP_SD_PIN          (BSP_IO_PORT_05_PIN_15)

#if SPEAKER_RING_SAMPLES > AUDIO_SHARED_BUFFER_SAMPLES
#error "Shared audio buffer is smaller than the cloud speaker ring"
#endif

int16_t g_audio_shared_buffer[AUDIO_SHARED_BUFFER_SAMPLES]
    BSP_ALIGN_VARIABLE(32);

#define s_sample_ring g_audio_shared_buffer
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;
static volatile uint32_t s_ring_count;
static uint32_t s_tx_blocks[2][SPEAKER_BLOCK_WORDS];
static uint8_t s_tx_index;
static volatile bool s_active;
static volatile bool s_stop_requested;
static bool s_initialized;
static bool s_output_open;
static bool s_output_reserved;
static volatile bool s_cleanup_pending;
static volatile bool s_amp_enabled;
static volatile uint32_t s_blocks_played;
static volatile uint32_t s_overflow_count;
static volatile uint32_t s_gap_block_count;
static volatile uint32_t s_partial_wait_count;
static volatile uint32_t s_unexpected_idle_count;
static volatile uint32_t s_write_error_count;
static volatile uint32_t s_input_peak;
static volatile uint32_t s_rail_sample_count;
static volatile fsp_err_t s_last_error = FSP_SUCCESS;
static volatile bool s_end_requested;
static volatile bool s_stop_after_block;
static volatile bool s_drain_pending;
static bool s_ramp_in_pending;
static int16_t s_last_output_sample;
static i2s_cfg_t s_cloud_i2s_cfg;

static void cloud_speaker_store_stereo(uint32_t * block,
                                       uint32_t sample_index,
                                       int32_t pcm16);
static bool cloud_speaker_submit_next(void);
static bool cloud_speaker_submit_silence(bool stop_after_block);
static bool cloud_speaker_output_open(void);
static void cloud_speaker_output_close(void);

static fsp_err_t cloud_speaker_amp_set(bool enabled)
{
    fsp_err_t err;

    if (s_amp_enabled == enabled)
    {
        return FSP_SUCCESS;
    }

    err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl,
                                   SPEAKER_AMP_SD_PIN,
                                   enabled ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS == err)
    {
        s_amp_enabled = enabled;
    }
    return err;
}

static bool cloud_speaker_submit_silence(bool stop_after_block)
{
    uint32_t interrupt_state;
    uint8_t tx_index;
    fsp_err_t err;

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    tx_index = s_tx_index;
    s_tx_index ^= 1U;
    s_active = true;
    s_stop_after_block = stop_after_block;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    for (uint32_t index = 0U; index < SPEAKER_BLOCK_SAMPLES; index++)
    {
        cloud_speaker_store_stereo(s_tx_blocks[tx_index], index, 0);
    }

    err = g_i2s0.p_api->write(g_i2s0.p_ctrl,
                              s_tx_blocks[tx_index],
                              sizeof(s_tx_blocks[tx_index]));
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        s_write_error_count++;
        s_stop_after_block = false;
        s_active = false;
        s_cleanup_pending = true;
        (void) cloud_speaker_amp_set(false);
        return false;
    }
    return true;
}

static uint32_t cloud_speaker_pcm_word(int32_t pcm16)
{
    int32_t const pcm24 = (pcm16 >> SPEAKER_GAIN_SHIFT) * 256;
    return ((uint32_t) pcm24) & 0x00FFFFFFU;
}

static void cloud_speaker_store_stereo(uint32_t * block,
                                       uint32_t sample_index,
                                       int32_t pcm16)
{
    uint32_t const word = cloud_speaker_pcm_word(pcm16);
    block[sample_index * SPEAKER_WORDS_PER_SAMPLE] = word;
    block[(sample_index * SPEAKER_WORDS_PER_SAMPLE) + 1U] = word;
}

static bool cloud_speaker_output_open(void)
{
    fsp_err_t err;

    if (s_output_open)
    {
        return true;
    }
    if (!audio_output_owner_acquire(AUDIO_OUTPUT_OWNER_CHAT))
    {
        s_last_error = FSP_ERR_INTERNAL;
        return false;
    }
    s_output_reserved = true;

    /* GPT1 drives the board's shared external AUDIO_CLK input.  The clock
     * manager keeps it running if the microphone is using it concurrently. */
    err = audio_output_clock_start();
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        audio_output_owner_release(AUDIO_OUTPUT_OWNER_CHAT);
        s_output_reserved = false;
        return false;
    }

    err = g_i2s0.p_api->open(g_i2s0.p_ctrl, &s_cloud_i2s_cfg);
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        audio_output_clock_stop();
        audio_output_owner_release(AUDIO_OUTPUT_OWNER_CHAT);
        s_output_reserved = false;
        return false;
    }

    s_output_open = true;
    s_cleanup_pending = false;
    return true;
}

static void cloud_speaker_output_close(void)
{
    if (!s_output_reserved)
    {
        s_cleanup_pending = false;
        return;
    }
    if (s_output_open)
    {
        (void) g_i2s0.p_api->stop(g_i2s0.p_ctrl);
        (void) g_i2s0.p_api->close(g_i2s0.p_ctrl);
        s_output_open = false;
    }
    (void) cloud_speaker_amp_set(false);
    audio_output_clock_stop();
    audio_output_owner_release(AUDIO_OUTPUT_OWNER_CHAT);
    s_output_reserved = false;
    s_cleanup_pending = false;
}

static void cloud_speaker_i2s_callback(i2s_callback_args_t * p_args)
{
    bool restart_required = false;

    if (NULL == p_args)
    {
        return;
    }

    if (I2S_EVENT_TX_EMPTY == p_args->event)
    {
        s_blocks_played++;
        if (s_stop_after_block)
        {
            fsp_err_t const err = g_i2s0.p_api->stop(g_i2s0.p_ctrl);
            s_stop_after_block = false;
            s_stop_requested = true;
            if (FSP_SUCCESS != err)
            {
                s_last_error = err;
                s_write_error_count++;
                s_stop_requested = false;
                s_active = false;
                s_cleanup_pending = true;
                (void) cloud_speaker_amp_set(false);
            }
            return;
        }
        if (s_drain_pending)
        {
            s_drain_pending = false;
            (void) cloud_speaker_submit_silence(true);
        }
        else
        {
            (void) cloud_speaker_submit_next();
        }
    }
    else if (I2S_EVENT_IDLE == p_args->event)
    {
        if (s_active && !s_stop_requested)
        {
            s_last_error = FSP_ERR_INTERNAL;
            s_unexpected_idle_count++;
        }
        s_stop_requested = false;
        s_active = false;
        s_stop_after_block = false;
        s_drain_pending = false;
        if ((s_ring_count >= SPEAKER_STARTUP_SAMPLES) ||
            (s_end_requested && (s_ring_count > 0U)))
        {
            s_ramp_in_pending = true;
            restart_required = true;
        }
        if (restart_required)
        {
            (void) cloud_speaker_submit_next();
        }
        else
        {
            fsp_err_t const err = cloud_speaker_amp_set(false);
            if (FSP_SUCCESS != err)
            {
                s_last_error = err;
                s_write_error_count++;
            }
            s_cleanup_pending = true;
        }
    }
}

fsp_err_t CloudSpeaker_Init(void)
{
    fsp_err_t err;

    if (s_initialized)
    {
        return FSP_SUCCESS;
    }

    s_ring_head = 0U;
    s_ring_tail = 0U;
    s_ring_count = 0U;
    s_tx_index = 0U;
    s_active = false;
    s_output_open = false;
    s_output_reserved = false;
    s_cleanup_pending = false;
    s_amp_enabled = false;
    s_stop_requested = false;
    s_blocks_played = 0U;
    s_overflow_count = 0U;
    s_gap_block_count = 0U;
    s_partial_wait_count = 0U;
    s_unexpected_idle_count = 0U;
    s_write_error_count = 0U;
    s_input_peak = 0U;
    s_rail_sample_count = 0U;
    s_last_error = FSP_SUCCESS;
    s_end_requested = false;
    s_stop_after_block = false;
    s_drain_pending = false;
    s_ramp_in_pending = false;
    s_last_output_sample = 0;

    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                 SPEAKER_AMP_SD_PIN,
                                 IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                                 IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        return err;
    }

    s_cloud_i2s_cfg = *g_i2s0.p_cfg;
    s_cloud_i2s_cfg.p_callback = cloud_speaker_i2s_callback;
    s_cloud_i2s_cfg.ws_continue = I2S_WS_CONTINUE_ON;

    s_initialized = true;
    return FSP_SUCCESS;
}

void CloudSpeaker_Poll(void)
{
    if (s_cleanup_pending && !s_active)
    {
        cloud_speaker_output_close();
    }
}

static bool cloud_speaker_submit_next(void)
{
    uint32_t sample_count;
    uint32_t ring_tail;
    uint32_t interrupt_state;
    uint8_t tx_index;
    bool send_gap_block = false;
    bool ramp_in;
    bool drain_after_block = false;
    bool const amp_was_enabled = s_amp_enabled;
    fsp_err_t err;

    if (!cloud_speaker_output_open())
    {
        s_write_error_count++;
        s_active = false;
        return false;
    }
    err = cloud_speaker_amp_set(true);
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        s_write_error_count++;
        s_active = false;
        /* Defer hardware teardown to CloudSpeaker_Poll().  This path can also
         * run from the I2S callback, where closing GPT/I2S directly is unsafe. */
        s_cleanup_pending = true;
        return false;
    }
    if (!amp_was_enabled)
    {
        /* Keep speech in the ring while MAX98357 wakes from shutdown. */
        s_ramp_in_pending = true;
        return cloud_speaker_submit_silence(false);
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (s_ring_count < SPEAKER_BLOCK_SAMPLES)
    {
        if (s_end_requested)
        {
            s_end_requested = false;
            drain_after_block = true;
        }
        else
        {
            /* Keep a partial cloud packet for the next complete block.  Playing
             * it immediately and padding the remainder with zero creates a
             * discontinuity at every network packet boundary.  Keep SSI
             * running with short silence blocks until more audio arrives. */
            if (s_ring_count > 0U)
            {
                s_partial_wait_count++;
            }
            s_gap_block_count++;
            send_gap_block = true;
            s_ramp_in_pending = true;
        }
    }

    sample_count = send_gap_block ? 0U :
                   ((s_ring_count > SPEAKER_BLOCK_SAMPLES) ?
                    SPEAKER_BLOCK_SAMPLES : s_ring_count);
    ring_tail = s_ring_tail;
    tx_index = s_tx_index;
    s_tx_index ^= 1U;
    s_active = true;
    ramp_in = s_ramp_in_pending && (sample_count > 0U);
    if (sample_count > 0U)
    {
        s_ramp_in_pending = false;
    }
    if (drain_after_block)
    {
        s_drain_pending = true;
    }
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    for (uint32_t index = 0U; index < sample_count; index++)
    {
        int32_t sample = s_sample_ring[(ring_tail + index) % SPEAKER_RING_SAMPLES];
        if (ramp_in)
        {
            sample = (sample * (int32_t) (index + 1U)) /
                     (int32_t) SPEAKER_BLOCK_SAMPLES;
        }
        cloud_speaker_store_stereo(s_tx_blocks[tx_index], index, sample);
        s_last_output_sample = (int16_t) sample;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (sample_count > 0U)
    {
        s_ring_tail = (s_ring_tail + sample_count) % SPEAKER_RING_SAMPLES;
        s_ring_count -= sample_count;
    }
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    if (sample_count < SPEAKER_BLOCK_SAMPLES)
    {
        uint32_t const fade_samples = SPEAKER_BLOCK_SAMPLES - sample_count;
        int32_t const fade_start = s_last_output_sample;
        for (uint32_t index = 0U; index < fade_samples; index++)
        {
            int32_t const sample =
                (fade_start * (int32_t) (fade_samples - index - 1U)) /
                (int32_t) fade_samples;
            cloud_speaker_store_stereo(s_tx_blocks[tx_index],
                                       sample_count + index,
                                       sample);
        }
        s_last_output_sample = 0;
    }

    err = g_i2s0.p_api->write(g_i2s0.p_ctrl,
                              s_tx_blocks[tx_index],
                              sizeof(s_tx_blocks[tx_index]));
    if (FSP_SUCCESS != err)
    {
        s_last_error = err;
        s_write_error_count++;
        s_stop_after_block = false;
        s_drain_pending = false;
        s_active = false;
        s_cleanup_pending = true;
        (void) cloud_speaker_amp_set(false);
        return false;
    }
    return true;
}

bool CloudSpeaker_EnqueuePcm16Le(uint8_t const * pcm, uint32_t length)
{
    uint32_t sample_count;
    uint32_t interrupt_state;
    uint32_t block_peak = 0U;
    uint32_t rail_samples = 0U;
    bool start_required;

    if (!s_initialized || (NULL == pcm) || (0U == length) || (0U != (length & 1U)))
    {
        return false;
    }
    /* Prepare GPT4/SSI0 as soon as the first cloud audio packet arrives.  This
     * avoids deferring all hardware-open failures until the startup
     * threshold, where the complete reply could otherwise be queued silently. */
    if (!s_output_open && !cloud_speaker_output_open())
    {
        s_write_error_count++;
        return false;
    }

    sample_count = length / 2U;
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    uint32_t const free_samples = SPEAKER_RING_SAMPLES - s_ring_count;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    if (sample_count > free_samples)
    {
        interrupt_state = __get_PRIMASK();
        __disable_irq();
        s_overflow_count++;
        if (0U == interrupt_state)
        {
            __enable_irq();
        }
        return false;
    }

    uint32_t write_head = s_ring_head;
    for (uint32_t index = 0U; index < sample_count; index++)
    {
        uint16_t const raw = (uint16_t) pcm[index * 2U] |
                             (uint16_t) ((uint16_t) pcm[(index * 2U) + 1U] << 8U);
        int16_t const signed_sample = (int16_t) raw;
        uint32_t const magnitude = (signed_sample < 0) ?
                                   (uint32_t) (-(int32_t) signed_sample) :
                                   (uint32_t) signed_sample;
        if (magnitude > block_peak)
        {
            block_peak = magnitude;
        }
        if (magnitude >= 32767U)
        {
            rail_samples++;
        }
        s_sample_ring[write_head] = signed_sample;
        write_head = (write_head + 1U) % SPEAKER_RING_SAMPLES;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_ring_head = write_head;
    s_ring_count += sample_count;
    s_end_requested = false;
    if (block_peak > s_input_peak)
    {
        s_input_peak = block_peak;
    }
    s_rail_sample_count += rail_samples;
    start_required = !s_active && !s_stop_requested &&
                     (s_ring_count >= SPEAKER_STARTUP_SAMPLES);
    if (start_required)
    {
        s_ramp_in_pending = true;
        s_last_error = FSP_SUCCESS;
    }
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    return !start_required || cloud_speaker_submit_next();
}

void CloudSpeaker_Flush(void)
{
    uint32_t interrupt_state;
    bool start_required;

    if (!s_initialized)
    {
        return;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_end_requested = s_active || (s_ring_count > 0U);
    start_required = !s_active && !s_stop_requested && (s_ring_count > 0U);
    if (start_required)
    {
        s_ramp_in_pending = true;
    }
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    if (start_required)
    {
        (void) cloud_speaker_submit_next();
    }
}

uint32_t CloudSpeaker_TrimTrailingSilence(uint32_t * p_tail_peak,
                                          uint32_t * p_tail_mean_abs)
{
    uint32_t interrupt_state;
    uint32_t ring_head;
    uint32_t ring_count;
    uint32_t quiet_samples = 0U;
    uint32_t tail_peak = 0U;
    uint32_t tail_mean_abs = 0U;
    bool first_block = true;

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    ring_head = s_ring_head;
    ring_count = s_ring_count;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }

    while ((ring_count - quiet_samples) >= SPEAKER_BLOCK_SAMPLES)
    {
        uint32_t block_peak = 0U;
        uint32_t block_sum_abs = 0U;

        for (uint32_t index = 0U; index < SPEAKER_BLOCK_SAMPLES; index++)
        {
            uint32_t const ring_index =
                (ring_head + SPEAKER_RING_SAMPLES - 1U - quiet_samples - index) %
                SPEAKER_RING_SAMPLES;
            int32_t const sample = s_sample_ring[ring_index];
            uint32_t const magnitude = (sample < 0) ?
                                       (uint32_t) (-sample) : (uint32_t) sample;
            if (magnitude > block_peak)
            {
                block_peak = magnitude;
            }
            block_sum_abs += magnitude;
        }

        uint32_t const block_mean_abs = block_sum_abs / SPEAKER_BLOCK_SAMPLES;
        if (first_block)
        {
            tail_peak = block_peak;
            tail_mean_abs = block_mean_abs;
            first_block = false;
        }
        if ((block_peak > SPEAKER_TAIL_PEAK_LIMIT) ||
            (block_mean_abs > SPEAKER_TAIL_MEAN_LIMIT))
        {
            break;
        }
        quiet_samples += SPEAKER_BLOCK_SAMPLES;
    }

    if (NULL != p_tail_peak)
    {
        *p_tail_peak = tail_peak;
    }
    if (NULL != p_tail_mean_abs)
    {
        *p_tail_mean_abs = tail_mean_abs;
    }
    if (quiet_samples <= SPEAKER_TAIL_KEEP_SAMPLES)
    {
        return 0U;
    }

    uint32_t trim_samples = quiet_samples - SPEAKER_TAIL_KEEP_SAMPLES;
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (trim_samples > s_ring_count)
    {
        trim_samples = s_ring_count;
    }
    s_ring_head = (s_ring_head + SPEAKER_RING_SAMPLES - trim_samples) %
                  SPEAKER_RING_SAMPLES;
    s_ring_count -= trim_samples;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    return trim_samples;
}

void CloudSpeaker_StopAndClear(void)
{
    uint32_t interrupt_state;

    if (s_initialized)
    {
        s_stop_requested = true;
        if (s_output_open)
        {
            (void) g_i2s0.p_api->stop(g_i2s0.p_ctrl);
        }
        fsp_err_t const err = cloud_speaker_amp_set(false);
        if (FSP_SUCCESS != err)
        {
            s_last_error = err;
            s_write_error_count++;
        }
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    s_ring_head = 0U;
    s_ring_tail = 0U;
    s_ring_count = 0U;
    s_active = false;
    s_stop_requested = false;
    s_end_requested = false;
    s_stop_after_block = false;
    s_drain_pending = false;
    s_ramp_in_pending = false;
    s_last_output_sample = 0;
    if (0U == interrupt_state)
    {
        __enable_irq();
    }
    cloud_speaker_output_close();
}

bool CloudSpeaker_IsBusy(void)
{
    return s_active || (s_ring_count > 0U);
}

uint32_t CloudSpeaker_BufferedSamples(void)
{
    return s_ring_count;
}

uint32_t CloudSpeaker_BlocksPlayed(void)
{
    return s_blocks_played;
}

uint32_t CloudSpeaker_OverflowCount(void)
{
    return s_overflow_count;
}

uint32_t CloudSpeaker_GapBlockCount(void)
{
    return s_gap_block_count;
}

uint32_t CloudSpeaker_PartialWaitCount(void)
{
    return s_partial_wait_count;
}

uint32_t CloudSpeaker_UnexpectedIdleCount(void)
{
    return s_unexpected_idle_count;
}

uint32_t CloudSpeaker_WriteErrorCount(void)
{
    return s_write_error_count;
}

uint32_t CloudSpeaker_InputPeak(void)
{
    return s_input_peak;
}

uint32_t CloudSpeaker_RailSampleCount(void)
{
    return s_rail_sample_count;
}

fsp_err_t CloudSpeaker_LastError(void)
{
    return s_last_error;
}
