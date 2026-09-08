/*
 * voice.h
 *
 *  Created on: 2026年5月17日
 *      Author: 36315
 */

#ifndef VOICE_VOICE_H_
#define VOICE_VOICE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include "bsp_api.h"
extern "C" {
#else
#include "hal_data.h"
#endif

typedef enum e_voice_channel
{
    VOICE_CHANNEL_LEFT = 0,
    VOICE_CHANNEL_RIGHT = 1,
    /* Select the slot with the stronger microphone signal for each frame. */
    VOICE_CHANNEL_AUTO = 2,
} voice_channel_t;

/* Microphone frames are exposed as signed PCM16, matching the keyword model
 * training data and the cloud audio protocol. */
typedef int16_t voice_sample_t;

fsp_err_t Voice_Init(void);
fsp_err_t Voice_Start(void);
fsp_err_t Voice_Stop(void);
bool Voice_Running(void);
void Voice_ChannelSet(voice_channel_t channel);
voice_channel_t Voice_ActiveChannelGet(void);
int32_t Voice_LeftAverageAbsGet(void);
int32_t Voice_LeftPeakAbsGet(void);
int32_t Voice_RightAverageAbsGet(void);
int32_t Voice_RightPeakAbsGet(void);
uint32_t Voice_LeftRawNonzeroGet(void);
uint32_t Voice_RightRawNonzeroGet(void);
uint32_t Voice_FrameRead(voice_sample_t *p_dest, uint32_t capacity);
bool Voice_FrameReady(void);
uint32_t Voice_SampleCountGet(void);
int32_t Voice_AverageAbsGet(void);
int32_t Voice_PeakAbsGet(void);
uint32_t Voice_FrameCounterGet(void);
uint32_t Voice_DroppedFrameCountGet(void);
fsp_err_t Voice_LastErrorGet(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_VOICE_H_ */
