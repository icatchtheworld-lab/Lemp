#ifndef AUDIO_AUDIO_SHARED_BUFFER_H_
#define AUDIO_AUDIO_SHARED_BUFFER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Recording and cloud playback never run at the same time.  Sharing this
 * on-chip buffer avoids the uninitialized external SDRAM region. */
#define AUDIO_SHARED_BUFFER_SAMPLES (320000U)

extern int16_t g_audio_shared_buffer[AUDIO_SHARED_BUFFER_SAMPLES];

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_AUDIO_SHARED_BUFFER_H_ */
