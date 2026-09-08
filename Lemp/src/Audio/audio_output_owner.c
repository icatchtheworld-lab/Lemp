#include "Audio/audio_output_owner.h"

#include "hal_data.h"

static volatile audio_output_owner_t s_owner = AUDIO_OUTPUT_OWNER_NONE;
static bool s_audio_clock_started;
static uint32_t s_audio_clock_users;

fsp_err_t audio_output_clock_start(void)
{
    fsp_err_t err;

    if (s_audio_clock_started)
    {
        s_audio_clock_users++;
        return FSP_SUCCESS;
    }

    /* Both SSI0 (speaker) and SSI1 (microphone) use the external AUDIO_CLK
     * input on P4.02.  On this board GPT1 drives that input through P1.05.
     * Starting the same clock here lets alarm playback work before the first
     * realtime-chat microphone session. */
    err = g_timer1.p_api->open(g_timer1.p_ctrl, g_timer1.p_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = g_timer1.p_api->start(g_timer1.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) g_timer1.p_api->close(g_timer1.p_ctrl);
        return err;
    }

    s_audio_clock_started = true;
    s_audio_clock_users = 1U;
    return FSP_SUCCESS;
}

void audio_output_clock_stop(void)
{
    if (!s_audio_clock_started)
    {
        return;
    }
    if (s_audio_clock_users > 1U)
    {
        s_audio_clock_users--;
        return;
    }

    (void) g_timer1.p_api->stop(g_timer1.p_ctrl);
    (void) g_timer1.p_api->close(g_timer1.p_ctrl);
    s_audio_clock_users = 0U;
    s_audio_clock_started = false;
}

bool audio_output_owner_acquire(audio_output_owner_t owner)
{
    bool acquired = false;
    uint32_t interrupt_state;

    if (AUDIO_OUTPUT_OWNER_NONE == owner)
    {
        return false;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if ((AUDIO_OUTPUT_OWNER_NONE == s_owner) || (owner == s_owner))
    {
        s_owner = owner;
        acquired = true;
    }
    __set_PRIMASK(interrupt_state);
    return acquired;
}

void audio_output_owner_release(audio_output_owner_t owner)
{
    uint32_t interrupt_state = __get_PRIMASK();

    __disable_irq();
    if (owner == s_owner)
    {
        s_owner = AUDIO_OUTPUT_OWNER_NONE;
    }
    __set_PRIMASK(interrupt_state);
}
