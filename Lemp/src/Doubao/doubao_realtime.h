#ifndef DOUBAO_DOUBAO_REALTIME_H_
#define DOUBAO_DOUBAO_REALTIME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Esp/esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum e_doubao_realtime_state
{
    DOUBAO_STATE_UNINITIALIZED = 0,
    DOUBAO_STATE_WAITING_FOR_WIFI,
    DOUBAO_STATE_OPENING_WEBSOCKET,
    DOUBAO_STATE_STARTING_CONNECTION,
    DOUBAO_STATE_STARTING_SESSION,
    DOUBAO_STATE_READY,
    DOUBAO_STATE_ERROR,
} doubao_realtime_state_t;

bool DoubaoRealtime_Init(void);
bool DoubaoRealtime_Start(uint32_t now_ms);
bool DoubaoRealtime_Stop(uint32_t now_ms);
void DoubaoRealtime_Poll(uint32_t now_ms);
void DoubaoRealtime_HandleTransportFrame(esp_transport_frame_t const * frame,
                                         uint32_t now_ms);
void DoubaoRealtime_NotifyTransportError(char const * reason);
bool DoubaoRealtime_SetMicrophone(bool enabled, uint32_t now_ms);
bool DoubaoRealtime_MicrophoneRequested(void);
bool DoubaoRealtime_MicrophoneRunning(void);
bool DoubaoRealtime_UploadingRecording(void);
bool DoubaoRealtime_TakeFinalUserText(char * destination, size_t capacity);
uint32_t DoubaoRealtime_RecordedSamples(void);
doubao_realtime_state_t DoubaoRealtime_State(void);
bool DoubaoRealtime_SendTtsPrompt(char const * text);
void DoubaoRealtime_SetSayHelloText(char const * text);
void DoubaoRealtime_SetSilentSession(bool silent);
char const * DoubaoRealtime_StateText(void);
char const * DoubaoRealtime_LastError(void);
uint32_t DoubaoRealtime_UploadedSamples(void);
uint32_t DoubaoRealtime_ReceivedAudioBytes(void);
uint32_t DoubaoRealtime_DroppedAudioFrames(void);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_DOUBAO_REALTIME_H_ */
