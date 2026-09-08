#ifndef ALARM_ALARM_TONE_H_
#define ALARM_ALARM_TONE_H_

#include "r_i2s_api.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool alarm_tone_init(void);
bool alarm_tone_start(void);
bool alarm_tone_start_prompt(void);
void alarm_tone_stop(void);
void alarm_tone_poll(void);
void alarm_tone_i2s_callback(i2s_callback_args_t * p_args);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_ALARM_TONE_H_ */
