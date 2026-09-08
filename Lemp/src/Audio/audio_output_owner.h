#ifndef AUDIO_AUDIO_OUTPUT_OWNER_H_
#define AUDIO_AUDIO_OUTPUT_OWNER_H_

#include "bsp_api.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_audio_output_owner
{
    AUDIO_OUTPUT_OWNER_NONE = 0,
    AUDIO_OUTPUT_OWNER_ALARM,
    AUDIO_OUTPUT_OWNER_CHAT,
} audio_output_owner_t;

bool audio_output_owner_acquire(audio_output_owner_t owner);
void audio_output_owner_release(audio_output_owner_t owner);
fsp_err_t audio_output_clock_start(void);
void audio_output_clock_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_AUDIO_OUTPUT_OWNER_H_ */
