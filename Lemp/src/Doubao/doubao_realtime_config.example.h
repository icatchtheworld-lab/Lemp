#ifndef DOUBAO_REALTIME_CONFIG_H_
#define DOUBAO_REALTIME_CONFIG_H_

/* Copy this file to doubao_realtime_config.h and fill in local credentials. */
#define DOUBAO_API_APP_ID       "YOUR_DOUBAO_APP_ID"
#define DOUBAO_API_ACCESS_KEY   "YOUR_DOUBAO_ACCESS_KEY"
#define DOUBAO_API_RESOURCE_ID  "volc.speech.dialog"
#define DOUBAO_API_APP_KEY      "YOUR_DOUBAO_APP_KEY"

#define DOUBAO_WS_HOST          "openspeech.bytedance.com"
#define DOUBAO_WS_PORT          (443U)
#define DOUBAO_WS_PATH          "/api/v3/realtime/dialogue"

#define DOUBAO_INPUT_RATE_HZ    16000
#define DOUBAO_OUTPUT_RATE_HZ   16000
#define DOUBAO_TTS_SPEAKER      "zh_male_yunzhou_jupiter_bigtts"
#define DOUBAO_RECV_TIMEOUT_SEC 120

#endif /* DOUBAO_REALTIME_CONFIG_H_ */
