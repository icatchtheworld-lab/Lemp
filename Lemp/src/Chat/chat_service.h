#ifndef CHAT_CHAT_SERVICE_H_
#define CHAT_CHAT_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "Doubao/doubao_realtime.h"

#ifdef __cplusplus
extern "C" {
#endif

bool chat_service_enter(uint32_t now_ms);
void chat_service_leave(uint32_t now_ms);
void chat_service_poll(uint32_t now_ms);
bool chat_service_set_microphone(bool enabled, uint32_t now_ms);
bool chat_service_is_initialized(void);
char const * chat_service_last_error(void);
bool chat_service_play_rest_reminder(uint32_t now_ms);
bool chat_service_is_playing_prompt(void);
void chat_service_stop_prompt(void);
bool chat_service_prewarm(uint32_t now_ms);
void chat_service_prewarm_poll(uint32_t now_ms);
void chat_service_teardown_prewarm(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_CHAT_SERVICE_H_ */
