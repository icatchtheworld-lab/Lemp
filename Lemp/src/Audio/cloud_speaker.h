#ifndef AUDIO_CLOUD_SPEAKER_H_
#define AUDIO_CLOUD_SPEAKER_H_

#include "r_i2s_api.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_SPEAKER_SAMPLE_RATE_HZ (16000U)

fsp_err_t CloudSpeaker_Init(void);
void CloudSpeaker_Poll(void);
bool CloudSpeaker_EnqueuePcm16Le(uint8_t const * pcm, uint32_t length);
void CloudSpeaker_Flush(void);
uint32_t CloudSpeaker_TrimTrailingSilence(uint32_t * p_tail_peak,
                                          uint32_t * p_tail_mean_abs);
void CloudSpeaker_StopAndClear(void);
bool CloudSpeaker_IsBusy(void);
uint32_t CloudSpeaker_BufferedSamples(void);
uint32_t CloudSpeaker_BlocksPlayed(void);
uint32_t CloudSpeaker_OverflowCount(void);
uint32_t CloudSpeaker_GapBlockCount(void);
uint32_t CloudSpeaker_PartialWaitCount(void);
uint32_t CloudSpeaker_UnexpectedIdleCount(void);
uint32_t CloudSpeaker_WriteErrorCount(void);
uint32_t CloudSpeaker_InputPeak(void);
uint32_t CloudSpeaker_RailSampleCount(void);
fsp_err_t CloudSpeaker_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CLOUD_SPEAKER_H_ */
