/*
 * voice.c
 *
 *  Created on: 2026�?�?7�?
 *      Author: 36315
 */

#include "Voice/voice.h"
#include "Audio/audio_output_owner.h"

#define VOICE_FRAME_SAMPLE_COUNT    (128U)//128个数据采样点
#define VOICE_FRAME_WORD_COUNT      (VOICE_FRAME_SAMPLE_COUNT * 2U)//两个声道�?      256
#define VOICE_CHANNEL_WORD_OFFSET   (2U)
#define VOICE_FRAME_QUEUE_DEPTH     (64U)

static volatile bool g_voice_open = false;  //I2S是否打开
static volatile bool g_voice_running = false;   //是否在运�?
static volatile bool g_voice_stop_requested = false;    //是否请求停止
static volatile uint8_t g_voice_capture_buffer_index = 0U;  //缓冲�?  �?缓冲�?  共同实现双缓�?

static volatile uint32_t g_voice_frame_counter = 0U;    //累计帧数�?
static volatile uint32_t g_voice_queue_write_count = 0U;
static volatile uint32_t g_voice_queue_read_count = 0U;
static volatile uint32_t g_voice_dropped_frame_count = 0U;
static volatile int32_t g_voice_average_abs = 0;    //当前帧率平均振幅
static volatile int32_t g_voice_peak_abs = 0;   //当前帧峰值振�?
static volatile fsp_err_t g_voice_last_error = FSP_SUCCESS;
static volatile voice_channel_t g_voice_channel = VOICE_CHANNEL_LEFT;
static volatile voice_channel_t g_voice_active_channel = VOICE_CHANNEL_LEFT;
static volatile uint64_t g_voice_left_sum_abs;
static volatile uint64_t g_voice_right_sum_abs;
static volatile uint32_t g_voice_slot_sample_count;
static volatile int32_t g_voice_left_peak_abs;
static volatile int32_t g_voice_right_peak_abs;
static volatile uint32_t g_voice_left_raw_nonzero;
static volatile uint32_t g_voice_right_raw_nonzero;
static uint32_t g_voice_rx_buffers[2][VOICE_FRAME_WORD_COUNT];//接收缓存�?
static voice_sample_t g_voice_mono_buffers[2][VOICE_FRAME_SAMPLE_COUNT];//样本缓冲�?由FIFO转换为MONO
/* AUTO mode decodes both SSI slots before choosing the one carrying audio. */
static voice_sample_t g_voice_other_channel_buffers[2][VOICE_FRAME_SAMPLE_COUNT];
static voice_sample_t g_voice_frame_queue[VOICE_FRAME_QUEUE_DEPTH][VOICE_FRAME_SAMPLE_COUNT];

static void voice_capture_stats_reset(void)
{
    g_voice_left_sum_abs = 0U;
    g_voice_right_sum_abs = 0U;
    g_voice_slot_sample_count = 0U;
    g_voice_left_peak_abs = 0;
    g_voice_right_peak_abs = 0;
    g_voice_left_raw_nonzero = 0U;
    g_voice_right_raw_nonzero = 0U;
}

static void voice_state_reset(void)
{
    g_voice_running = false;
    g_voice_stop_requested = false;
    g_voice_capture_buffer_index = 0U;
    g_voice_frame_counter = 0U;
    g_voice_queue_write_count = 0U;
    g_voice_queue_read_count = 0U;
    g_voice_dropped_frame_count = 0U;
    g_voice_average_abs = 0;
    g_voice_peak_abs = 0;
    g_voice_last_error = FSP_SUCCESS;
    voice_capture_stats_reset();
}

static voice_sample_t voice_sample_decode(uint32_t raw_word)
{
    uint32_t sample = (raw_word >> 8U) & 0x0000FFFFU;

    /* SSI delivers the signed 24-bit microphone word left-aligned in the
     * 32-bit slot.  Keep the most significant 16 audio bits, which is the
     * format used by the known-good voice and keyword implementation. */
    if ((sample & 0x00008000U) != 0U)
    {
        return (voice_sample_t) ((int32_t) sample - 65536);
    }

    return (voice_sample_t) sample;
}

static void voice_frame_process(uint8_t buffer_index)
{
    uint64_t left_sum_abs = 0U;
    uint64_t right_sum_abs = 0U;
    int32_t left_peak_abs = 0;
    int32_t right_peak_abs = 0;
    voice_channel_t selected_channel = g_voice_channel;

    for (uint32_t i = 0; i < VOICE_FRAME_SAMPLE_COUNT; i++)
    {
        uint32_t const word_index = i * VOICE_CHANNEL_WORD_OFFSET;
        uint32_t const left_raw = g_voice_rx_buffers[buffer_index][word_index];
        uint32_t const right_raw = g_voice_rx_buffers[buffer_index][word_index + 1U];
        voice_sample_t const left_sample =
            voice_sample_decode(left_raw);
        voice_sample_t const right_sample =
            voice_sample_decode(right_raw);
        int32_t const left_abs = (left_sample < 0) ?
                                 -(int32_t) left_sample : (int32_t) left_sample;
        int32_t const right_abs = (right_sample < 0) ?
                                  -(int32_t) right_sample : (int32_t) right_sample;

        g_voice_mono_buffers[buffer_index][i] = left_sample;
        g_voice_other_channel_buffers[buffer_index][i] = right_sample;
        left_sum_abs += (uint32_t) left_abs;
        right_sum_abs += (uint32_t) right_abs;

        if (left_abs > left_peak_abs)
        {
            left_peak_abs = left_abs;
        }
        if (right_abs > right_peak_abs)
        {
            right_peak_abs = right_abs;
        }
        if (0U != left_raw)
        {
            g_voice_left_raw_nonzero++;
        }
        if (0U != right_raw)
        {
            g_voice_right_raw_nonzero++;
        }
    }

    g_voice_left_sum_abs += left_sum_abs;
    g_voice_right_sum_abs += right_sum_abs;
    g_voice_slot_sample_count += VOICE_FRAME_SAMPLE_COUNT;
    if (left_peak_abs > g_voice_left_peak_abs)
    {
        g_voice_left_peak_abs = left_peak_abs;
    }
    if (right_peak_abs > g_voice_right_peak_abs)
    {
        g_voice_right_peak_abs = right_peak_abs;
    }

    if (VOICE_CHANNEL_AUTO == selected_channel)
    {
        /* A 25% margin prevents slot selection from oscillating when both
         * channels only contain a similar, very small noise floor. */
        selected_channel = g_voice_active_channel;
        if ((right_sum_abs * 4U) > (left_sum_abs * 5U))
        {
            selected_channel = VOICE_CHANNEL_RIGHT;
        }
        else if ((left_sum_abs * 4U) > (right_sum_abs * 5U))
        {
            selected_channel = VOICE_CHANNEL_LEFT;
        }
    }

    if (VOICE_CHANNEL_RIGHT == selected_channel)
    {
        for (uint32_t i = 0U; i < VOICE_FRAME_SAMPLE_COUNT; i++)
        {
            g_voice_mono_buffers[buffer_index][i] =
                g_voice_other_channel_buffers[buffer_index][i];
        }
        g_voice_peak_abs = right_peak_abs;
        g_voice_average_abs = (int32_t) (right_sum_abs / VOICE_FRAME_SAMPLE_COUNT);
    }
    else
    {
        g_voice_peak_abs = left_peak_abs;
        g_voice_average_abs = (int32_t) (left_sum_abs / VOICE_FRAME_SAMPLE_COUNT);
    }
    g_voice_active_channel = selected_channel;
}

static void voice_frame_queue_push(uint8_t buffer_index)
{
    uint32_t write_count = g_voice_queue_write_count;
    uint32_t queue_index;

    if ((write_count - g_voice_queue_read_count) >= VOICE_FRAME_QUEUE_DEPTH)
    {
        g_voice_queue_read_count++;
        g_voice_dropped_frame_count++;
    }

    queue_index = write_count % VOICE_FRAME_QUEUE_DEPTH;
    for (uint32_t index = 0U; index < VOICE_FRAME_SAMPLE_COUNT; index++)
    {
        g_voice_frame_queue[queue_index][index] = g_voice_mono_buffers[buffer_index][index];
    }
    __DMB();
    g_voice_queue_write_count = write_count + 1U;
}

static void voice_frame_queue_discard_all(void)
{
    uint32_t interrupt_state = __get_PRIMASK();

    /*
     * 停止采集期间，主循环可能没有取走队列中的旧音频帧。
     * 重新启动前让读计数追上写计数，相当于丢弃全部历史帧；
     * 短暂关闭中断可避免与I2S回调同时修改队列计数。
     */
    __disable_irq();
    g_voice_queue_read_count = g_voice_queue_write_count;
    __DMB();
    __set_PRIMASK(interrupt_state);
}

static fsp_err_t voice_read_queue(uint8_t buffer_index)
{
    return g_i2s1.p_api->read(g_i2s1.p_ctrl, g_voice_rx_buffers[buffer_index], sizeof(g_voice_rx_buffers[buffer_index]));
}

fsp_err_t Voice_Init(void)
{
    if (g_voice_open)
    {
        return FSP_SUCCESS;
    }

    voice_state_reset();
    g_voice_last_error = g_i2s1.p_api->open(g_i2s1.p_ctrl, g_i2s1.p_cfg);
    if (FSP_SUCCESS != g_voice_last_error)
    {
        return g_voice_last_error;
    }

    /* GPT1/P1.05 is the shared external AUDIO_CLK for both microphone and
     * speaker.  Use the common reference-counted manager so an alarm cannot
     * stop the clock while realtime capture is active (and vice versa). */
    g_voice_last_error = audio_output_clock_start();
    if (FSP_SUCCESS != g_voice_last_error)
    {
        (void) g_i2s1.p_api->close(g_i2s1.p_ctrl);
        return g_voice_last_error;
    }

    g_voice_open = true;
    return FSP_SUCCESS;
}

fsp_err_t Voice_Start(void)
{
    fsp_err_t err = Voice_Init();//这里是开启相关时�?

    if (FSP_SUCCESS != err)
    {
        return err;
    }
    if (g_voice_running)
    {
        return FSP_SUCCESS;
    }

    /* 每次从停止状态重新采集时，不再处理上一次运行遗留的音频。 */
    voice_frame_queue_discard_all();
    voice_capture_stats_reset();
    g_voice_last_error = voice_read_queue(g_voice_capture_buffer_index);//指引数据进入缓存�?号还�?�?
    if (FSP_SUCCESS == g_voice_last_error)
    {
        g_voice_running = true;
    }
    return g_voice_last_error;
}

fsp_err_t Voice_Stop(void)
{
    if (!g_voice_open)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (!g_voice_running)
    {
        return FSP_SUCCESS;
    }

    g_voice_stop_requested = true;
    g_voice_last_error = g_i2s1.p_api->stop(g_i2s1.p_ctrl);
    if (FSP_SUCCESS != g_voice_last_error)
    {
        g_voice_running = false;
        g_voice_stop_requested = false;
    }

    return g_voice_last_error;
}

bool Voice_Running(void)
{
    return g_voice_running;
}

void Voice_ChannelSet(voice_channel_t channel)
{
    if (((uint32_t) channel) <= ((uint32_t) VOICE_CHANNEL_AUTO))
    {
        g_voice_channel = channel;
        /* The known-good board wiring uses the right slot. AUTO starts there
         * and changes only when the other slot is measurably stronger. */
        g_voice_active_channel = (VOICE_CHANNEL_AUTO == channel) ?
                                 VOICE_CHANNEL_RIGHT : channel;
    }
}

voice_channel_t Voice_ActiveChannelGet(void)
{
    return g_voice_active_channel;
}

int32_t Voice_LeftAverageAbsGet(void)
{
    return (0U != g_voice_slot_sample_count) ?
           (int32_t) (g_voice_left_sum_abs / g_voice_slot_sample_count) : 0;
}

int32_t Voice_LeftPeakAbsGet(void)
{
    return g_voice_left_peak_abs;
}

int32_t Voice_RightAverageAbsGet(void)
{
    return (0U != g_voice_slot_sample_count) ?
           (int32_t) (g_voice_right_sum_abs / g_voice_slot_sample_count) : 0;
}

int32_t Voice_RightPeakAbsGet(void)
{
    return g_voice_right_peak_abs;
}

uint32_t Voice_LeftRawNonzeroGet(void)
{
    return g_voice_left_raw_nonzero;
}

uint32_t Voice_RightRawNonzeroGet(void)
{
    return g_voice_right_raw_nonzero;
}

uint32_t Voice_FrameRead(voice_sample_t *p_dest, uint32_t capacity)
{
    uint32_t sample_count = VOICE_FRAME_SAMPLE_COUNT;
    uint32_t interrupt_state;
    uint32_t queue_index;

    if ((NULL == p_dest) || (0U == capacity))
    {
        return 0U;
    }

    if (capacity < sample_count)
    {
        sample_count = capacity;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (g_voice_queue_read_count == g_voice_queue_write_count)
    {
        __set_PRIMASK(interrupt_state);
        return 0U;
    }

    queue_index = g_voice_queue_read_count % VOICE_FRAME_QUEUE_DEPTH;
    for (uint32_t i = 0; i < sample_count; i++)//以上内容均是判断  关键是这里将mono的数据给到p_dest指针
    {
        p_dest[i] = g_voice_frame_queue[queue_index][i];
    }
    g_voice_queue_read_count++;
    __set_PRIMASK(interrupt_state);

    return sample_count;
}

bool Voice_FrameReady(void)
{
    return (g_voice_queue_write_count != g_voice_queue_read_count);
}

uint32_t Voice_SampleCountGet(void)
{
    return VOICE_FRAME_SAMPLE_COUNT;
}

int32_t Voice_AverageAbsGet(void)
{
    return g_voice_average_abs;
}

int32_t Voice_PeakAbsGet(void)
{
    return g_voice_peak_abs;
}

uint32_t Voice_FrameCounterGet(void)
{
    return g_voice_frame_counter;
}

uint32_t Voice_DroppedFrameCountGet(void)
{
    return g_voice_dropped_frame_count;
}

fsp_err_t Voice_LastErrorGet(void)
{
    return g_voice_last_error;
}


void voice_i2s_callback(i2s_callback_args_t *p_args)
{
    if (NULL == p_args)
    {
        return;
    }
    if (I2S_EVENT_RX_FULL == p_args->event)
    {
        uint8_t completed_buffer_index = g_voice_capture_buffer_index;

        voice_frame_process(completed_buffer_index);//对音频进行处�?求其平均�?最大�?并将处理的完成的数据给到mono样本�?
        voice_frame_queue_push(completed_buffer_index);
        g_voice_frame_counter++;//一帧数据处理完�?

        if (!g_voice_stop_requested)//切换传冲�?为下一帧数据做准备
        {
            uint8_t next_buffer_index = (uint8_t) (completed_buffer_index ^ 1U);

            g_voice_capture_buffer_index = next_buffer_index;
            g_voice_last_error = voice_read_queue(next_buffer_index);//引导数据进入另外一个缓冲区 如果满了就回到上面的数据处理

            if (FSP_SUCCESS != g_voice_last_error)
            {
                g_voice_running = false;
                g_voice_stop_requested = true;
            }
        }
        else
        {
            g_voice_running = false;
        }
    }

    if (I2S_EVENT_IDLE == p_args->event)
    {
        g_voice_running = false;
        g_voice_stop_requested = false;
    }
}
