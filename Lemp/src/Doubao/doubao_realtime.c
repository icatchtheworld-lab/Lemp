#include "Doubao/doubao_realtime.h"

#include "Audio/cloud_speaker.h"
#include "Audio/audio_shared_buffer.h"
#include "Doubao/doubao_realtime_config.h"
#include "Doubao/gzip_codec.h"
#include "Esp/esp_link.h"
#include "Esp/esp_transport.h"
#include "Voice/voice.h"
#include "hal_data.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define DOUBAO_PROTOCOL_VERSION          (1U)
#define DOUBAO_CLIENT_FULL_REQUEST       (0x1U)
#define DOUBAO_CLIENT_AUDIO_REQUEST      (0x2U)
#define DOUBAO_SERVER_FULL_RESPONSE      (0x9U)
#define DOUBAO_SERVER_ACK                (0xBU)
#define DOUBAO_SERVER_ERROR              (0xFU)
#define DOUBAO_FLAG_WITH_EVENT           (0x4U)
#define DOUBAO_SERIALIZATION_NONE        (0x0U)
#define DOUBAO_SERIALIZATION_JSON        (0x1U)
#define DOUBAO_COMPRESSION_NONE          (0x0U)
#define DOUBAO_COMPRESSION_GZIP          (0x1U)

#define DOUBAO_EVENT_START_CONNECTION    (1U)
#define DOUBAO_EVENT_START_SESSION       (100U)
#define DOUBAO_EVENT_AUDIO               (200U)
#define DOUBAO_EVENT_SAY_HELLO            (300U)
#define DOUBAO_EVENT_END_ASR              (400U)
#define DOUBAO_EVENT_TTS_RESPONSE        (352U)
#define DOUBAO_EVENT_ASR_RESPONSE        (451U)

#define DOUBAO_AUDIO_CHUNK_SAMPLES       (320U)
#define DOUBAO_AUDIO_CHUNK_BYTES         (DOUBAO_AUDIO_CHUNK_SAMPLES * 2U)
#define DOUBAO_CHUNK_DURATION_MS         (DOUBAO_AUDIO_CHUNK_SAMPLES * 1000U / DOUBAO_INPUT_RATE_HZ)
#define DOUBAO_DECODE_LIMIT              (32768U)
#define DOUBAO_HANDSHAKE_TIMEOUT_MS      (20000U)
#define DOUBAO_HEADERS_SIZE              (1024U)
#define DOUBAO_TX_BUFFER_SIZE            (4096U)
#define DOUBAO_ERROR_SIZE                (96U)
#define DOUBAO_USER_TEXT_SIZE            (192U)
#define DOUBAO_STRINGIFY_INNER(value)    #value
#define DOUBAO_STRINGIFY(value)          DOUBAO_STRINGIFY_INNER(value)

typedef enum e_doubao_audio_tx_kind
{
    DOUBAO_AUDIO_TX_NONE = 0,
    DOUBAO_AUDIO_TX_RECORDING,
    DOUBAO_AUDIO_TX_END_ASR,
} doubao_audio_tx_kind_t;

static char const s_start_session_json[] =
    "{\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},"
    "\"tts\":{\"speaker\":\"" DOUBAO_TTS_SPEAKER "\",\"audio_config\":{"
    "\"channel\":1,\"format\":\"pcm_s16le\",\"sample_rate\":"
    DOUBAO_STRINGIFY(DOUBAO_OUTPUT_RATE_HZ) "}},\"dialog\":{\"bot_name\":\"\\u706f\\u5c0f\\u8bed\"," 
    "\"system_role\":\"\\u4f60\\u53eb\\u706f\\u5c0f\\u8bed\\uff0c\\u8bf7"
    "\\u4f7f\\u7528\\u7b80\\u6d01\\u81ea\\u7136\\u7684\\u4e2d\\u6587\\u56de\\u7b54"
    "\\u3002\\u5f53\\u522b\\u4eba\\u95ee\\u4f60\\u662f\\u8c01\\u6216\\u8005\\u4f60"
    "\\u53eb\\u4ec0\\u4e48\\u540d\\u5b57\\u65f6\\uff0c\\u8bf7\\u56de\\u7b54\\u4f60"
    "\\u53eb\\u706f\\u5c0f\\u8bed\\u3002\\u56de\\u7b54\\u8981\\u81ea\\u7136"
    "\\u4eb2\\u5207\\uff0c\\u50cf\\u966a\\u4f34\\u578b\\u684c\\u5ba0\\u3002"
    "\\u666e\\u901a\\u804a\\u5929\\u6b63\\u5e38\\u56de\\u7b54\\u3002\\u5f53"
    "\\u7528\\u6237\\u53d1\\u51fa\\u8bbe\\u5907\\u52a8\\u4f5c\\u6307\\u4ee4"
    "\\u65f6\\uff0c\\u6839\\u636e\\u5177\\u4f53\\u52a8\\u4f5c\\u81ea\\u7136"
    "\\u56de\\u5e94\\u4e00\\u5230\\u4e24\\u53e5\\uff0c\\u53ef\\u4ee5\\u8f7b"
    "\\u677e\\u6d3b\\u6cfc\\u4e00\\u4e9b\\u3002\\u4e0d\\u8981\\u53ea\\u56de"
    "\\u590d\\u201c\\u597d\\u7684\\u201d\\u3001\\u201c\\u5df2\\u6267\\u884c"
    "\\u201d\\u6216\\u201c\\u6307\\u4ee4\\u6536\\u5230\\u201d\\uff0c\\u4e0d"
    "\\u8981\\u6bcf\\u6b21\\u4f7f\\u7528\\u76f8\\u540c\\u53e5\\u5f0f\\u3002"
    "\\u6bd4\\u5982\\u5f00\\u706f\\u53ef\\u4ee5\\u8bf4\\u201c\\u597d\\u5440"
    "\\uff0c\\u7ed9\\u4f60\\u628a\\u706f\\u6253\\u5f00\\u5566\\u201d\\uff1b"
    "\\u5173\\u706f\\u53ef\\u4ee5\\u8bf4\\u201c\\u597d\\uff0c\\u706f\\u5173"
    "\\u4e0a\\u4e86\\uff0c\\u9700\\u8981\\u7684\\u65f6\\u5019\\u518d\\u53eb"
    "\\u6211\\u201d\\uff1b\\u5750\\u4e0b\\u53ef\\u4ee5\\u8bf4\\u201c\\u597d"
    "\\u5440\\uff0c\\u6211\\u5750\\u4e0b\\u6765\\u966a\\u4f60\\u201d\\uff1b"
    "\\u7ad9\\u7acb\\u53ef\\u4ee5\\u8bf4\\u201c\\u6ca1\\u95ee\\u9898\\uff0c"
    "\\u6211\\u7ad9\\u8d77\\u6765\\u5566\\u201d\\uff1b\\u6447\\u5934\\u53ef"
    "\\u4ee5\\u8bf4\\u201c\\u597d\\u5427\\uff0c\\u90a3\\u6211\\u6447\\u6447"
    "\\u5934\\u7ed9\\u4f60\\u770b\\u201d\\u3002\\u4e0d\\u8981\\u8f93\\u51fa"
    "JSON\\u3001\\u4ee3\\u7801\\u6216\\u6280\\u672f\\u5b57\\u6bb5\\u3002\"," 
    "\"speaking_style\":\"warm, friendly, lively, natural and concise\",\"location\":{\"city\":"
    "\"Beijing\"},\"extra\":{\"strict_audit\":false,\"recv_timeout\":"
    DOUBAO_STRINGIFY(DOUBAO_RECV_TIMEOUT_SEC) ",\"input_mod\":\"push_to_talk\"}}}";


static doubao_realtime_state_t s_state = DOUBAO_STATE_UNINITIALIZED;
static uint32_t s_state_started_ms;
static bool s_initialized;
static bool s_session_requested;
static char const * s_custom_hello_text;
static bool s_silent_session;   /* true 时跳过 STARTING_SESSION→READY 的自动 send_say_hello */
static bool s_close_requested;
static bool s_wifi_reported;
static bool s_last_wifi_connected;
static bool s_wifi_announced;
static bool s_microphone_requested;
static bool s_microphone_running;
static bool s_capture_stop_pending;
static bool s_recording_upload_active;
static bool s_recording_overflow;
static bool s_end_asr_pending;
static doubao_audio_tx_kind_t s_audio_tx_pending;
static uint32_t s_audio_tx_pending_samples;
static voice_sample_t s_capture_frame[128];
static voice_sample_t s_audio_chunk[DOUBAO_AUDIO_CHUNK_SAMPLES];
#define s_recording_buffer g_audio_shared_buffer
static uint32_t s_recording_read_offset;
static uint32_t s_recording_write_offset;
static uint32_t s_recording_buffered_samples;
static uint32_t s_recorded_samples;
static uint32_t s_uploaded_samples;
static uint64_t s_recording_sum_abs;
static int32_t s_recording_peak_abs;
static uint32_t s_recording_nonzero_samples;
static voice_sample_t s_recording_first_sample;
static bool s_recording_first_sample_valid;
static uint32_t s_received_audio_bytes;
static uint32_t s_dropped_audio_frames;
static char s_final_user_text[DOUBAO_USER_TEXT_SIZE];
static bool s_final_user_text_ready;
static char s_connect_id[37];
static char s_session_id[37];
static char s_last_error[DOUBAO_ERROR_SIZE];

static void capture_to_recording(void);
static void recording_finish_if_stopped(uint32_t now_ms);
static void upload_recording_step(uint32_t now_ms);
static void send_end_asr_step(void);
static void audio_send_acknowledged(uint32_t now_ms);
static bool websocket_open_request(uint32_t now_ms);
static void capture_final_asr_text(uint8_t const * json, size_t length);

static void print_recording_signal_stats(void)
{
    printf("[DB][MIC] signal: mean_abs=%lu peak=%ld nonzero=%lu/%lu first=%d\r\n",
           (unsigned long) ((0U != s_recorded_samples) ?
                            (s_recording_sum_abs / s_recorded_samples) : 0U),
           (long) s_recording_peak_abs,
           (unsigned long) s_recording_nonzero_samples,
           (unsigned long) s_recorded_samples,
           s_recording_first_sample_valid ? (int) s_recording_first_sample : 0);
    printf("[DB][MIC] slots: active=%s L(mean=%ld peak=%ld raw_nonzero=%lu) "
           "R(mean=%ld peak=%ld raw_nonzero=%lu)\r\n",
           (VOICE_CHANNEL_LEFT == Voice_ActiveChannelGet()) ? "LEFT" : "RIGHT",
           (long) Voice_LeftAverageAbsGet(),
           (long) Voice_LeftPeakAbsGet(),
           (unsigned long) Voice_LeftRawNonzeroGet(),
           (long) Voice_RightAverageAbsGet(),
           (long) Voice_RightPeakAbsGet(),
           (unsigned long) Voice_RightRawNonzeroGet());
}

static uint32_t read_be32(uint8_t const * source)
{
    return ((uint32_t) source[0] << 24U) |
           ((uint32_t) source[1] << 16U) |
           ((uint32_t) source[2] << 8U) |
           (uint32_t) source[3];
}

static void write_be32(uint8_t * destination, uint32_t value)
{
    destination[0] = (uint8_t) (value >> 24U);
    destination[1] = (uint8_t) (value >> 16U);
    destination[2] = (uint8_t) (value >> 8U);
    destination[3] = (uint8_t) value;
}

static void uuid_build(char output[37], uint32_t salt)
{
    bsp_unique_id_t const * unique_id = R_BSP_UniqueIdGet();
    uint8_t bytes[16];
    uint32_t timing = DWT->CYCCNT ^ SysTick->VAL ^ salt;

    for (uint32_t index = 0U; index < sizeof(bytes); index++)
    {
        timing = (timing * 1664525U) + 1013904223U;
        bytes[index] = unique_id->unique_id_bytes[index] ^
                       (uint8_t) (timing >> ((index & 3U) * 8U));
    }
    bytes[6] = (uint8_t) ((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (uint8_t) ((bytes[8] & 0x3FU) | 0x80U);

    (void) snprintf(output,
                    37U,
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3],
                    bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11],
                    bytes[12], bytes[13], bytes[14], bytes[15]);
}

static void set_state(doubao_realtime_state_t state, uint32_t now_ms)
{
    s_state = state;
    s_state_started_ms = now_ms;
}

static void set_error(char const * stage, char const * reason)
{
    if ('\0' != s_last_error[0])
    {
        printf("[DB][SECONDARY] %s:%s (first=%s)\r\n",
               (NULL != stage) ? stage : "UNKNOWN",
               (NULL != reason) ? reason : "UNKNOWN",
               s_last_error);
        return;
    }

    (void) snprintf(s_last_error,
                    sizeof(s_last_error),
                    "%s:%s",
                    (NULL != stage) ? stage : "UNKNOWN",
                    (NULL != reason) ? reason : "UNKNOWN");
    s_state = DOUBAO_STATE_ERROR;
    s_microphone_requested = false;
    if (s_microphone_running || s_capture_stop_pending)
    {
        (void) Voice_Stop();
        s_microphone_running = false;
    }
    s_capture_stop_pending = false;
    s_recording_upload_active = false;
    s_end_asr_pending = false;
    s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
    s_audio_tx_pending_samples = 0U;
    s_final_user_text[0] = '\0';
    s_final_user_text_ready = false;
    CloudSpeaker_StopAndClear();
    printf("[DB][ERROR] %s\r\n", s_last_error);
}

static void session_runtime_clear(void)
{
    s_microphone_requested = false;
    if (s_microphone_running || s_capture_stop_pending)
    {
        (void) Voice_Stop();
    }
    s_microphone_running = false;
    s_capture_stop_pending = false;
    s_recording_upload_active = false;
    s_recording_overflow = false;
    s_end_asr_pending = false;
    s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
    s_audio_tx_pending_samples = 0U;
    s_recording_read_offset = 0U;
    s_recording_write_offset = 0U;
    s_recording_buffered_samples = 0U;
    s_recorded_samples = 0U;
    s_recording_sum_abs = 0U;
    s_recording_peak_abs = 0;
    s_recording_nonzero_samples = 0U;
    s_recording_first_sample = 0;
    s_recording_first_sample_valid = false;
    s_final_user_text[0] = '\0';
    s_final_user_text_ready = false;
    CloudSpeaker_StopAndClear();
}

static bool websocket_open_request(uint32_t now_ms)
{
    char headers[DOUBAO_HEADERS_SIZE];
    int length;

    if (!s_session_requested)
    {
        return true;
    }
    if (!s_wifi_reported || !s_last_wifi_connected)
    {
        set_state(DOUBAO_STATE_WAITING_FOR_WIFI, now_ms);
        return true;
    }

    uuid_build(s_connect_id, now_ms ^ 0x434F4E4EU);
    uuid_build(s_session_id, now_ms ^ 0x53455353U);
    length = snprintf(headers,
                      sizeof(headers),
                      "X-Api-App-ID: %s\r\n"
                      "X-Api-Access-Key: %s\r\n"
                      "X-Api-Resource-Id: %s\r\n"
                      "X-Api-App-Key: %s\r\n"
                      "X-Api-Connect-Id: %s\r\n",
                      DOUBAO_API_APP_ID,
                      DOUBAO_API_ACCESS_KEY,
                      DOUBAO_API_RESOURCE_ID,
                      DOUBAO_API_APP_KEY,
                      s_connect_id);
    if ((length <= 0) || ((size_t) length >= sizeof(headers)) ||
        !esp_transport_ws_open(DOUBAO_WS_HOST,
                               DOUBAO_WS_PORT,
                               DOUBAO_WS_PATH,
                               headers))
    {
        set_error("WS_OPEN", "BRIDGE_SEND_FAILED");
        return false;
    }
    printf("[DB][02] Opening wss://%s%s (credentials hidden)\r\n",
           DOUBAO_WS_HOST,
           DOUBAO_WS_PATH);
    set_state(DOUBAO_STATE_OPENING_WEBSOCKET, now_ms);
    return true;
}

static bool send_ws_binary(uint8_t const * payload, uint32_t length)
{
    if (!esp_transport_send(ESP_CHANNEL_DOUBAO,
                            ESP_MSG_WS_SEND,
                            0U,
                            payload,
                            length))
    {
        return false;
    }
    return true;
}

static bool send_event_payload(uint8_t message_type,
                               uint8_t serialization,
                               uint32_t event,
                               bool include_session,
                               uint8_t const * payload,
                               uint32_t payload_length)
{
    static uint8_t tx[DOUBAO_TX_BUFFER_SIZE];
    static uint8_t gzip[DOUBAO_TX_BUFFER_SIZE];
    size_t gzip_length;
    size_t offset = 0U;
    size_t session_length = include_session ? strlen(s_session_id) : 0U;

    gzip_length = gzip_encode_stored(payload,
                                     payload_length,
                                     gzip,
                                     sizeof(gzip));
    if (0U == gzip_length)
    {
        return false;
    }

    if ((4U + 4U + (include_session ? (4U + session_length) : 0U) +
         4U + gzip_length) > sizeof(tx))
    {
        return false;
    }

    tx[offset++] = (uint8_t) ((DOUBAO_PROTOCOL_VERSION << 4U) | 1U);
    tx[offset++] = (uint8_t) ((message_type << 4U) | DOUBAO_FLAG_WITH_EVENT);
    tx[offset++] = (uint8_t) ((serialization << 4U) | DOUBAO_COMPRESSION_GZIP);
    tx[offset++] = 0U;
    write_be32(&tx[offset], event);
    offset += 4U;
    if (include_session)
    {
        write_be32(&tx[offset], (uint32_t) session_length);
        offset += 4U;
        memcpy(&tx[offset], s_session_id, session_length);
        offset += session_length;
    }
    write_be32(&tx[offset], (uint32_t) gzip_length);
    offset += 4U;
    memcpy(&tx[offset], gzip, gzip_length);
    offset += gzip_length;
    return send_ws_binary(tx, (uint32_t) offset);
}

static bool send_start_connection(void)
{
    static uint8_t const empty_json[] = "{}";
    printf("[DB][04] TX StartConnection event=1\r\n");
    return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                              DOUBAO_SERIALIZATION_JSON,
                              DOUBAO_EVENT_START_CONNECTION,
                              false,
                              empty_json,
                              sizeof(empty_json) - 1U);
}

static bool send_start_session(void)
{
    printf("[DB][06] TX StartSession event=100, input=push_to_talk PCM16/16k, output=PCM16/16k\r\n");
    return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                              DOUBAO_SERIALIZATION_JSON,
                              DOUBAO_EVENT_START_SESSION,
                              true,
                              (uint8_t const *) s_start_session_json,
                              (uint32_t) (sizeof(s_start_session_json) - 1U));
}

static bool send_say_hello(void)
{
    /* SayHello requires the text that the cloud should speak.  Sending an
     * empty object is accepted by the protocol, but produces an immediate
     * event 359 with no event 352 audio packets. */
    static uint8_t const introduction_json[] =
        "{\"content\":\"\\u4f60\\u597d\\uff0c\\u6211\\u662f\\u706f\\u5c0f\\u8bed\\uff0c"
        "\\u4f60\\u53ef\\u4ee5\\u548c\\u6211\\u804a\\u5929\\u54e6\\uff01\"}";

    if (NULL != s_custom_hello_text)
    {
        char json[512];
        int length;

        length = snprintf(json, sizeof(json),
                          "{\"content\":\"%s\"}", s_custom_hello_text);
        if ((length <= 0) || ((size_t) length >= sizeof(json)))
        {
            printf("[DB][08] SayHello custom text too long, fallback to default\r\n");
        }
        else
        {
            printf("[DB][08] TX SayHello event=300, custom=\"%s\"\r\n",
                   s_custom_hello_text);
            return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                                      DOUBAO_SERIALIZATION_JSON,
                                      DOUBAO_EVENT_SAY_HELLO,
                                      true,
                                      (uint8_t const *) json,
                                      (uint32_t) length);
        }
    }

    printf("[DB][08] TX SayHello event=300, cloud introduction requested\r\n");
    return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                              DOUBAO_SERIALIZATION_JSON,
                              DOUBAO_EVENT_SAY_HELLO,
                              true,
                              introduction_json,
                              sizeof(introduction_json) - 1U);
}

static bool send_audio(uint8_t const * pcm, uint32_t length)
{
    return send_event_payload(DOUBAO_CLIENT_AUDIO_REQUEST,
                              DOUBAO_SERIALIZATION_NONE,
                              DOUBAO_EVENT_AUDIO,
                              true,
                              pcm,
                              length);
}

static bool send_end_asr(void)
{
    static uint8_t const empty_json[] = "{}";

    printf("[DB][MIC] TX EndASR event=400\r\n");
    return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                              DOUBAO_SERIALIZATION_JSON,
                              DOUBAO_EVENT_END_ASR,
                              true,
                              empty_json,
                              sizeof(empty_json) - 1U);
}

static void microphone_queue_discard(void)
{
    while (Voice_FrameReady())
    {
        (void) Voice_FrameRead(s_capture_frame,
                               sizeof(s_capture_frame) / sizeof(s_capture_frame[0]));
    }
}

static bool microphone_start(void)
{
    fsp_err_t err;

    microphone_queue_discard();
    s_recording_read_offset = 0U;
    s_recording_write_offset = 0U;
    s_recording_buffered_samples = 0U;
    s_recorded_samples = 0U;
    s_recording_overflow = false;
    s_capture_stop_pending = false;
    s_recording_upload_active = true;
    s_end_asr_pending = false;
    s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
    s_audio_tx_pending_samples = 0U;
    s_recording_sum_abs = 0U;
    s_recording_peak_abs = 0;
    s_recording_nonzero_samples = 0U;
    s_recording_first_sample = 0;
    s_recording_first_sample_valid = false;
    CloudSpeaker_StopAndClear();
    err = Voice_Start();
    if (FSP_SUCCESS != err)
    {
        s_recording_upload_active = false;
        printf("[DB][MIC] Voice_Start failed, fsp=%d\r\n", (int) err);
        return false;
    }

    s_microphone_running = true;
    printf("[DB][MIC] ON: push-to-talk streaming, %ums/%u-byte packets\r\n",
           (unsigned int) DOUBAO_CHUNK_DURATION_MS,
           (unsigned int) DOUBAO_AUDIO_CHUNK_BYTES);
    return true;
}

static bool microphone_stop(uint32_t now_ms)
{
    fsp_err_t err;

    /* Save frames completed before the release event, then wait in Poll for
     * SSI to deliver its final in-flight frame before sending EndASR. */
    capture_to_recording();
    err = Voice_Stop();

    s_microphone_running = false;
    if (FSP_SUCCESS != err)
    {
        printf("[DB][MIC] Voice_Stop failed, fsp=%d\r\n", (int) err);
        return false;
    }

    s_capture_stop_pending = true;
    recording_finish_if_stopped(now_ms);
    printf("[DB][MIC] OFF: captured=%lums, draining live audio before EndASR%s\r\n",
           (unsigned long) (s_recorded_samples * 1000U / DOUBAO_INPUT_RATE_HZ),
           s_recording_overflow ? " (stream buffer overflowed)" : "");
    return true;
}

static void capture_to_recording(void)
{
    while (Voice_FrameReady())
    {
        uint32_t const count = Voice_FrameRead(s_capture_frame,
                                                sizeof(s_capture_frame) /
                                                sizeof(s_capture_frame[0]));
        uint32_t const free_samples = AUDIO_SHARED_BUFFER_SAMPLES -
                                      s_recording_buffered_samples;
        uint32_t const copy_samples = (count < free_samples) ? count : free_samples;

        if (copy_samples > 0U)
        {
            uint32_t const first_copy =
                ((AUDIO_SHARED_BUFFER_SAMPLES - s_recording_write_offset) < copy_samples) ?
                (AUDIO_SHARED_BUFFER_SAMPLES - s_recording_write_offset) : copy_samples;
            uint32_t const second_copy = copy_samples - first_copy;

            memcpy(&s_recording_buffer[s_recording_write_offset],
                   s_capture_frame,
                   first_copy * sizeof(s_recording_buffer[0]));
            if (second_copy > 0U)
            {
                memcpy(&s_recording_buffer[0],
                       &s_capture_frame[first_copy],
                       second_copy * sizeof(s_recording_buffer[0]));
            }
            s_recording_write_offset =
                (s_recording_write_offset + copy_samples) % AUDIO_SHARED_BUFFER_SAMPLES;
            s_recording_buffered_samples += copy_samples;

            if (!s_recording_first_sample_valid)
            {
                s_recording_first_sample = s_capture_frame[0];
                s_recording_first_sample_valid = true;
            }
            for (uint32_t index = 0U; index < copy_samples; index++)
            {
                int32_t const sample = s_capture_frame[index];
                int32_t const abs_sample = (sample < 0) ? -sample : sample;

                s_recording_sum_abs += (uint32_t) abs_sample;
                if (abs_sample > s_recording_peak_abs)
                {
                    s_recording_peak_abs = abs_sample;
                }
                if (0 != sample)
                {
                    s_recording_nonzero_samples++;
                }
            }
            s_recorded_samples += copy_samples;
        }
        if ((copy_samples < count) && !s_recording_overflow)
        {
            s_recording_overflow = true;
            printf("[DB][MIC] realtime upload buffer full; later samples discarded\r\n");
        }
    }
}

static void recording_finish_if_stopped(uint32_t now_ms)
{
    if (!s_capture_stop_pending)
    {
        return;
    }

    capture_to_recording();
    if (Voice_Running() || Voice_FrameReady())
    {
        return;
    }

    s_capture_stop_pending = false;
    s_end_asr_pending = true;
    print_recording_signal_stats();
    printf("[DB][UPLOAD] capture complete: %lums, %lu samples, %lu buffered\r\n",
           (unsigned long) (s_recorded_samples * 1000U / DOUBAO_INPUT_RATE_HZ),
           (unsigned long) s_recorded_samples,
           (unsigned long) s_recording_buffered_samples);
    (void) now_ms;
}

static void upload_recording_step(uint32_t now_ms)
{
    uint32_t chunk_samples;
    uint32_t first_copy;
    uint32_t second_copy;

    if (!s_recording_upload_active ||
        (DOUBAO_AUDIO_TX_NONE != s_audio_tx_pending))
    {
        return;
    }

    if (s_recording_buffered_samples < DOUBAO_AUDIO_CHUNK_SAMPLES)
    {
        if (s_microphone_running || s_capture_stop_pending ||
            (0U == s_recording_buffered_samples))
        {
            return;
        }
    }

    chunk_samples = (s_recording_buffered_samples > DOUBAO_AUDIO_CHUNK_SAMPLES) ?
                    DOUBAO_AUDIO_CHUNK_SAMPLES : s_recording_buffered_samples;
    first_copy = ((AUDIO_SHARED_BUFFER_SAMPLES - s_recording_read_offset) < chunk_samples) ?
                 (AUDIO_SHARED_BUFFER_SAMPLES - s_recording_read_offset) : chunk_samples;
    second_copy = chunk_samples - first_copy;
    memcpy(s_audio_chunk,
           &s_recording_buffer[s_recording_read_offset],
           first_copy * sizeof(s_audio_chunk[0]));
    if (second_copy > 0U)
    {
        memcpy(&s_audio_chunk[first_copy],
               &s_recording_buffer[0],
               second_copy * sizeof(s_audio_chunk[0]));
    }

    if (!send_audio((uint8_t const *) s_audio_chunk,
                    chunk_samples * sizeof(s_audio_chunk[0])))
    {
        set_error("AUDIO_UPLOAD", "UART_OR_WS_SEND_FAILED");
        return;
    }

    s_audio_tx_pending = DOUBAO_AUDIO_TX_RECORDING;
    s_audio_tx_pending_samples = chunk_samples;
    (void) now_ms;
}

static void send_end_asr_step(void)
{
    if (!s_end_asr_pending || s_capture_stop_pending ||
        (0U != s_recording_buffered_samples) ||
        (DOUBAO_AUDIO_TX_NONE != s_audio_tx_pending))
    {
        return;
    }

    if (!send_end_asr())
    {
        set_error("END_ASR", "SEND_FAILED");
        return;
    }
    s_end_asr_pending = false;
    s_audio_tx_pending = DOUBAO_AUDIO_TX_END_ASR;
    s_audio_tx_pending_samples = 0U;
}

static void audio_send_acknowledged(uint32_t now_ms)
{
    doubao_audio_tx_kind_t const acknowledged = s_audio_tx_pending;
    uint32_t const samples = s_audio_tx_pending_samples;

    s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
    s_audio_tx_pending_samples = 0U;
    if (DOUBAO_AUDIO_TX_RECORDING == acknowledged)
    {
        s_recording_read_offset =
            (s_recording_read_offset + samples) % AUDIO_SHARED_BUFFER_SAMPLES;
        s_recording_buffered_samples -= samples;
        s_uploaded_samples += samples;
    }
    else if (DOUBAO_AUDIO_TX_END_ASR == acknowledged)
    {
        s_recording_upload_active = false;
        printf("[DB][MIC] EndASR sent; push-to-talk utterance complete\r\n");
    }
    (void) now_ms;
}

typedef struct st_server_message
{
    uint8_t type;
    uint8_t flags;
    uint8_t serialization;
    uint8_t compression;
    bool has_event;
    uint32_t event;
    uint32_t error_code;
    uint8_t const * payload;
    uint32_t payload_length;
} server_message_t;

static bool parse_server_message(uint8_t const * data,
                                 uint32_t length,
                                 server_message_t * message)
{
    uint32_t header_length;
    uint32_t offset;
    uint32_t session_length;
    uint32_t declared_payload_length;

    if ((NULL == data) || (NULL == message) || (length < 4U) ||
        ((data[0] >> 4U) != DOUBAO_PROTOCOL_VERSION))
    {
        return false;
    }

    memset(message, 0, sizeof(*message));
    header_length = (uint32_t) (data[0] & 0x0FU) * 4U;
    if ((header_length < 4U) || (header_length > length))
    {
        return false;
    }

    message->type = data[1] >> 4U;
    message->flags = data[1] & 0x0FU;
    message->serialization = data[2] >> 4U;
    message->compression = data[2] & 0x0FU;
    offset = header_length;

    if (DOUBAO_SERVER_ERROR == message->type)
    {
        if ((offset + 8U) > length)
        {
            return false;
        }
        message->error_code = read_be32(&data[offset]);
        declared_payload_length = read_be32(&data[offset + 4U]);
        offset += 8U;
        if (declared_payload_length > (length - offset))
        {
            return false;
        }
        message->payload = &data[offset];
        message->payload_length = declared_payload_length;
        return true;
    }

    if ((DOUBAO_SERVER_FULL_RESPONSE != message->type) &&
        (DOUBAO_SERVER_ACK != message->type))
    {
        return false;
    }

    if (0U != (message->flags & 0x03U))
    {
        if ((offset + 4U) > length)
        {
            return false;
        }
        offset += 4U;
    }
    if (0U != (message->flags & DOUBAO_FLAG_WITH_EVENT))
    {
        if ((offset + 4U) > length)
        {
            return false;
        }
        message->has_event = true;
        message->event = read_be32(&data[offset]);
        offset += 4U;
    }
    if ((offset + 4U) > length)
    {
        return false;
    }
    session_length = read_be32(&data[offset]);
    offset += 4U;
    if (session_length > (length - offset))
    {
        return false;
    }
    offset += session_length;
    if ((offset + 4U) > length)
    {
        return false;
    }
    declared_payload_length = read_be32(&data[offset]);
    offset += 4U;
    if (declared_payload_length > (length - offset))
    {
        return false;
    }
    message->payload = &data[offset];
    message->payload_length = declared_payload_length;
    return true;
}

static bool decode_server_payload(server_message_t const * message,
                                  uint8_t ** decoded,
                                  size_t * decoded_length,
                                  bool * allocated)
{
    unsigned inflate_error = 0U;

    *decoded = (uint8_t *) message->payload;
    *decoded_length = message->payload_length;
    *allocated = false;
    if (DOUBAO_COMPRESSION_NONE == message->compression)
    {
        return true;
    }
    if (DOUBAO_COMPRESSION_GZIP != message->compression)
    {
        return false;
    }

    if (!gzip_decode_alloc(message->payload,
                           message->payload_length,
                           DOUBAO_DECODE_LIMIT,
                           decoded,
                           decoded_length,
                           &inflate_error))
    {
        printf("[DB][RX] GZIP inflate failed, code=%u, compressed=%lu\r\n",
               inflate_error,
               (unsigned long) message->payload_length);
        return false;
    }
    *allocated = true;
    return true;
}

static int json_hex_value(uint8_t value)
{
    if ((value >= (uint8_t) '0') && (value <= (uint8_t) '9'))
    {
        return (int) (value - (uint8_t) '0');
    }
    if ((value >= (uint8_t) 'a') && (value <= (uint8_t) 'f'))
    {
        return (int) (value - (uint8_t) 'a') + 10;
    }
    if ((value >= (uint8_t) 'A') && (value <= (uint8_t) 'F'))
    {
        return (int) (value - (uint8_t) 'A') + 10;
    }
    return -1;
}

static bool json_read_hex4(uint8_t const * json,
                           size_t length,
                           size_t offset,
                           uint32_t * codepoint)
{
    uint32_t value = 0U;

    if ((NULL == codepoint) || ((offset + 4U) > length))
    {
        return false;
    }
    for (size_t index = 0U; index < 4U; index++)
    {
        int const nibble = json_hex_value(json[offset + index]);
        if (nibble < 0)
        {
            return false;
        }
        value = (value << 4U) | (uint32_t) nibble;
    }
    *codepoint = value;
    return true;
}

static bool utf8_append_codepoint(char * output,
                                  size_t capacity,
                                  size_t * used,
                                  uint32_t codepoint)
{
    uint8_t bytes[4];
    size_t count;

    if ((NULL == output) || (NULL == used) || (0U == capacity))
    {
        return false;
    }
    if ((0U == codepoint) || (codepoint > 0x10FFFFU) ||
        ((codepoint >= 0xD800U) && (codepoint <= 0xDFFFU)))
    {
        codepoint = 0xFFFDU;
    }

    if (codepoint <= 0x7FU)
    {
        bytes[0] = (uint8_t) codepoint;
        count = 1U;
    }
    else if (codepoint <= 0x7FFU)
    {
        bytes[0] = (uint8_t) (0xC0U | (codepoint >> 6U));
        bytes[1] = (uint8_t) (0x80U | (codepoint & 0x3FU));
        count = 2U;
    }
    else if (codepoint <= 0xFFFFU)
    {
        bytes[0] = (uint8_t) (0xE0U | (codepoint >> 12U));
        bytes[1] = (uint8_t) (0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[2] = (uint8_t) (0x80U | (codepoint & 0x3FU));
        count = 3U;
    }
    else
    {
        bytes[0] = (uint8_t) (0xF0U | (codepoint >> 18U));
        bytes[1] = (uint8_t) (0x80U | ((codepoint >> 12U) & 0x3FU));
        bytes[2] = (uint8_t) (0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[3] = (uint8_t) (0x80U | (codepoint & 0x3FU));
        count = 4U;
    }

    if ((*used + count) >= capacity)
    {
        return false;
    }
    memcpy(&output[*used], bytes, count);
    *used += count;
    output[*used] = '\0';
    return true;
}

static bool json_decode_string(uint8_t const * json,
                               size_t length,
                               size_t quote_offset,
                               char * output,
                               size_t capacity,
                               size_t * end_offset)
{
    size_t input;
    size_t used = 0U;

    if ((NULL == json) || (NULL == output) || (capacity < 2U) ||
        (quote_offset >= length) || ((uint8_t) '"' != json[quote_offset]))
    {
        return false;
    }
    output[0] = '\0';
    input = quote_offset + 1U;
    while (input < length)
    {
        uint8_t value = json[input++];

        if ((uint8_t) '"' == value)
        {
            if (NULL != end_offset)
            {
                *end_offset = input;
            }
            return true;
        }
        if ((uint8_t) '\\' == value)
        {
            uint32_t codepoint;

            if (input >= length)
            {
                return false;
            }
            value = json[input++];
            if ((uint8_t) 'u' == value)
            {
                if (!json_read_hex4(json, length, input, &codepoint))
                {
                    return false;
                }
                input += 4U;
                if ((codepoint >= 0xD800U) && (codepoint <= 0xDBFFU) &&
                    ((input + 6U) <= length) &&
                    ((uint8_t) '\\' == json[input]) &&
                    ((uint8_t) 'u' == json[input + 1U]))
                {
                    uint32_t low_surrogate;
                    if (json_read_hex4(json, length, input + 2U, &low_surrogate) &&
                        (low_surrogate >= 0xDC00U) && (low_surrogate <= 0xDFFFU))
                    {
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                                    (low_surrogate - 0xDC00U);
                        input += 6U;
                    }
                }
                if (!utf8_append_codepoint(output, capacity, &used, codepoint))
                {
                    return false;
                }
                continue;
            }

            switch (value)
            {
                case (uint8_t) 'b': value = (uint8_t) '\b'; break;
                case (uint8_t) 'f': value = (uint8_t) '\f'; break;
                case (uint8_t) 'n': value = (uint8_t) '\n'; break;
                case (uint8_t) 'r': value = (uint8_t) '\r'; break;
                case (uint8_t) 't': value = (uint8_t) '\t'; break;
                case (uint8_t) '"':
                case (uint8_t) '\\':
                case (uint8_t) '/':
                    break;
                default:
                    return false;
            }
        }

        if ((used + 1U) >= capacity)
        {
            return false;
        }
        output[used++] = (char) value;
        output[used] = '\0';
    }
    return false;
}

static size_t json_find_bytes(uint8_t const * json,
                              size_t start,
                              size_t end,
                              char const * token)
{
    size_t token_length;

    if ((NULL == json) || (NULL == token) || (start > end))
    {
        return end;
    }
    token_length = strlen(token);
    if (token_length > (end - start))
    {
        return end;
    }
    for (size_t offset = start; (offset + token_length) <= end; offset++)
    {
        if (0 == memcmp(&json[offset], token, token_length))
        {
            return offset;
        }
    }
    return end;
}

static bool json_object_bounds(uint8_t const * json,
                               size_t length,
                               size_t marker,
                               size_t * object_start,
                               size_t * object_end)
{
    size_t starts[12];
    size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;

    if ((NULL == json) || (marker >= length) ||
        (NULL == object_start) || (NULL == object_end))
    {
        return false;
    }
    for (size_t offset = 0U; offset <= marker; offset++)
    {
        uint8_t const value = json[offset];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if ((uint8_t) '\\' == value)
            {
                escaped = true;
            }
            else if ((uint8_t) '"' == value)
            {
                in_string = false;
            }
            continue;
        }
        if ((uint8_t) '"' == value)
        {
            in_string = true;
        }
        else if ((uint8_t) '{' == value)
        {
            if (depth >= (sizeof(starts) / sizeof(starts[0])))
            {
                return false;
            }
            starts[depth++] = offset;
        }
        else if ((uint8_t) '}' == value)
        {
            if (0U == depth)
            {
                return false;
            }
            depth--;
        }
    }
    if (0U == depth)
    {
        return false;
    }
    *object_start = starts[depth - 1U];

    in_string = false;
    escaped = false;
    depth = 0U;
    for (size_t offset = *object_start; offset < length; offset++)
    {
        uint8_t const value = json[offset];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if ((uint8_t) '\\' == value)
            {
                escaped = true;
            }
            else if ((uint8_t) '"' == value)
            {
                in_string = false;
            }
            continue;
        }
        if ((uint8_t) '"' == value)
        {
            in_string = true;
        }
        else if ((uint8_t) '{' == value)
        {
            depth++;
        }
        else if ((uint8_t) '}' == value)
        {
            if (0U == depth)
            {
                return false;
            }
            depth--;
            if (0U == depth)
            {
                *object_end = offset + 1U;
                return true;
            }
        }
    }
    return false;
}

static bool json_object_has_final_result(uint8_t const * json,
                                         size_t object_start,
                                         size_t object_end)
{
    static char const interim_key[] = "\"is_interim\"";
    size_t offset = json_find_bytes(json,
                                    object_start,
                                    object_end,
                                    interim_key);

    if (offset == object_end)
    {
        return false;
    }
    offset += sizeof(interim_key) - 1U;
    while ((offset < object_end) &&
           ((json[offset] == (uint8_t) ' ') || (json[offset] == (uint8_t) '\t') ||
            (json[offset] == (uint8_t) '\r') || (json[offset] == (uint8_t) '\n') ||
            (json[offset] == (uint8_t) ':')))
    {
        offset++;
    }
    return ((offset + 5U) <= object_end) &&
           (0 == memcmp(&json[offset], "false", 5U));
}

static void capture_final_asr_text(uint8_t const * json, size_t length)
{
    static char const text_key[] = "\"text\"";
    char decoded_text[DOUBAO_USER_TEXT_SIZE];
    size_t cursor = 0U;

    while (cursor < length)
    {
        size_t const key_offset = json_find_bytes(json, cursor, length, text_key);
        size_t object_start;
        size_t object_end;
        size_t value_offset;
        size_t string_end;

        if (key_offset == length)
        {
            break;
        }
        cursor = key_offset + sizeof(text_key) - 1U;
        if (!json_object_bounds(json, length, key_offset, &object_start, &object_end) ||
            !json_object_has_final_result(json, object_start, object_end))
        {
            continue;
        }

        value_offset = cursor;
        while ((value_offset < object_end) &&
               ((json[value_offset] == (uint8_t) ' ') ||
                (json[value_offset] == (uint8_t) '\t') ||
                (json[value_offset] == (uint8_t) '\r') ||
                (json[value_offset] == (uint8_t) '\n') ||
                (json[value_offset] == (uint8_t) ':')))
        {
            value_offset++;
        }
        if (!json_decode_string(json,
                                object_end,
                                value_offset,
                                decoded_text,
                                sizeof(decoded_text),
                                &string_end))
        {
            cursor = object_end;
            continue;
        }

        {
            uint32_t const interrupt_state = __get_PRIMASK();
            __disable_irq();
            (void) snprintf(s_final_user_text,
                            sizeof(s_final_user_text),
                            "%s",
                            decoded_text);
            s_final_user_text_ready = true;
            __set_PRIMASK(interrupt_state);
        }
        printf("[DB][ASR] final user text captured, utf8_bytes=%lu\r\n",
               (unsigned long) strlen(decoded_text));
        cursor = string_end;
    }
}

static void process_server_message(uint8_t const * data,
                                   uint32_t length,
                                   uint32_t now_ms)
{
    server_message_t message;
    uint8_t * decoded;
    size_t decoded_length;
    bool allocated;
    bool is_binary_payload;
    bool is_audio_payload;

    if (!parse_server_message(data, length, &message))
    {
        set_error("DOUBAO_PARSE", "BAD_SERVER_FRAME");
        return;
    }
    if (!decode_server_payload(&message, &decoded, &decoded_length, &allocated))
    {
        set_error("DOUBAO_GZIP", "DECODE_FAILED");
        return;
    }

    if (DOUBAO_SERVER_ERROR == message.type)
    {
        char reason[64] = {0};
        size_t const copy_length = (decoded_length < (sizeof(reason) - 1U)) ?
                                   decoded_length : (sizeof(reason) - 1U);
        if (copy_length > 0U)
        {
            memcpy(reason, decoded, copy_length);
        }
        printf("[DB][RX] SERVER_ERROR code=%lu payload=%s\r\n",
               (unsigned long) message.error_code,
               reason);
        if (allocated)
        {
            gzip_decode_free(decoded);
        }
        set_error("DOUBAO_SERVER", reason);
        return;
    }

    is_binary_payload = ((DOUBAO_SERVER_ACK == message.type) ||
                         (DOUBAO_SERVER_FULL_RESPONSE == message.type)) &&
                        (DOUBAO_SERIALIZATION_NONE == message.serialization) &&
                        (decoded_length > 0U);
    is_audio_payload = is_binary_payload && message.has_event &&
                       (DOUBAO_EVENT_TTS_RESPONSE == message.event);

    /* Audio ACKs arrive frequently.  Printing every packet at 115200 baud
     * stalls the foreground receive loop long enough to create playback
     * jitter, so audio progress is reported by the periodic STAT counters. */
    if (message.has_event && !is_audio_payload)
    {
        printf("[DB][RX] type=0x%X event=%lu payload=%lu\r\n",
               message.type,
               (unsigned long) message.event,
               (unsigned long) decoded_length);

        /* JSON may contain raw UTF-8 or Unicode escapes.  Do not print the
         * payload through a terminal with an unknown code page; parse it from
         * the original bytes below instead. */
    }

    if (is_binary_payload && !is_audio_payload)
    {
        printf("[DB][RX] ignored non-TTS binary ACK event=%lu payload=%lu\r\n",
               (unsigned long) (message.has_event ? message.event : 0U),
               (unsigned long) decoded_length);
    }

    if (message.has_event &&
        (DOUBAO_EVENT_ASR_RESPONSE == message.event) &&
        (DOUBAO_SERIALIZATION_JSON == message.serialization))
    {
        capture_final_asr_text(decoded, decoded_length);
    }

    if (is_audio_payload)
    {
        if (!CloudSpeaker_EnqueuePcm16Le(decoded, (uint32_t) decoded_length))
        {
            s_dropped_audio_frames++;
            printf("[DB][SPK] queue full/write failed, dropped=%lu buffered=%lu fsp=%d\r\n",
                   (unsigned long) s_dropped_audio_frames,
                   (unsigned long) CloudSpeaker_BufferedSamples(),
                   (int) CloudSpeaker_LastError());
        }
        else
        {
            s_received_audio_bytes += (uint32_t) decoded_length;
        }
    }

    if (message.has_event && (450U == message.event))
    {
        CloudSpeaker_StopAndClear();
        printf("[DB][VAD] User speech detected; stale speaker audio cleared\r\n");
    }
    else if (message.has_event && (359U == message.event))
    {
        uint32_t tail_peak;
        uint32_t tail_mean_abs;
        uint32_t const trimmed_samples =
            CloudSpeaker_TrimTrailingSilence(&tail_peak, &tail_mean_abs);
        CloudSpeaker_Flush();
        printf("[DB][TTS] Response audio finished: trimmed=%lums "
               "tail_peak=%lu tail_mean=%lu buffered=%lums\r\n",
               (unsigned long) (trimmed_samples * 1000U /
                                CLOUD_SPEAKER_SAMPLE_RATE_HZ),
               (unsigned long) tail_peak,
               (unsigned long) tail_mean_abs,
               (unsigned long) (CloudSpeaker_BufferedSamples() * 1000U /
                                CLOUD_SPEAKER_SAMPLE_RATE_HZ));
    }
    else if (message.has_event && (459U == message.event))
    {
        printf("[DB][ASR] User utterance ended\r\n");
    }

    if (allocated)
    {
        gzip_decode_free(decoded);
    }

    if (DOUBAO_STATE_STARTING_CONNECTION == s_state)
    {
        printf("[DB][05] StartConnection accepted\r\n");
        if (!send_start_session())
        {
            set_error("START_SESSION", "SEND_FAILED");
            return;
        }
        set_state(DOUBAO_STATE_STARTING_SESSION, now_ms);
    }
    else if (DOUBAO_STATE_STARTING_SESSION == s_state)
    {
        printf("[DB][07] Session ready, session_id=%s\r\n", s_session_id);
        set_state(DOUBAO_STATE_READY, now_ms);
        if (s_silent_session)
        {
            printf("[DB][07] silent session: skip SayHello\r\n");
        }
        else if (!send_say_hello())
        {
            set_error("SAY_HELLO", "SEND_FAILED");
            return;
        }
        if (s_microphone_requested && !microphone_start())
        {
            set_error("MIC_START", "VOICE_START_FAILED");
        }
    }
}

void DoubaoRealtime_HandleTransportFrame(esp_transport_frame_t const * frame,
                                         uint32_t now_ms)
{
    if (NULL == frame)
    {
        return;
    }

    /*
     * The integrated UI initializes the shared ESP transport at boot for
     * weather/time, before the chat page initializes this module.  READY is a
     * one-shot bridge status frame, so cache it even while Doubao is not yet
     * initialized.  Otherwise entering chat later waits forever for a Wi-Fi
     * notification which has already been consumed by weather_client_poll().
     */
    if ((ESP_MSG_READY == frame->message) &&
        (ESP_CHANNEL_CONTROL == frame->channel) &&
        (1U == frame->payload_length))
    {
        bool const connected = (0U != frame->payload[0]);
        bool const changed = !s_wifi_reported || (connected != s_last_wifi_connected);

        s_wifi_reported = true;
        s_last_wifi_connected = connected;
        if (!s_initialized)
        {
            return;
        }
        if (s_session_requested && (!s_wifi_announced || changed))
        {
            printf("[DB][01] ESP32C3 bridge ready, Wi-Fi=%s\r\n",
                   connected ? "CONNECTED" : "DISCONNECTED");
            s_wifi_announced = true;
        }
        if (connected && s_session_requested && !s_close_requested &&
            (DOUBAO_STATE_WAITING_FOR_WIFI == s_state))
        {
            (void) websocket_open_request(now_ms);
        }
        return;
    }

    if (!s_initialized)
    {
        return;
    }

    if ((ESP_CHANNEL_DOUBAO != frame->channel) &&
        (ESP_CHANNEL_CONTROL != frame->channel))
    {
        return;
    }

    switch (frame->message)
    {
        case ESP_MSG_WS_CONFIGURED:
            if (frame->payload_length > 0U)
            {
                char report[64] = {0};
                uint32_t const copy_length =
                    (frame->payload_length < (sizeof(report) - 1U)) ?
                    frame->payload_length : (sizeof(report) - 1U);
                memcpy(report, frame->payload, copy_length);
                printf("[DB][ESP][CFG] %s\r\n", report);
            }
            break;

        case ESP_MSG_WS_OPENED:
            if (!s_session_requested || s_close_requested)
            {
                (void) esp_transport_send(ESP_CHANNEL_DOUBAO,
                                          ESP_MSG_WS_CLOSE,
                                          0U,
                                          NULL,
                                          0U);
                break;
            }
            printf("[DB][03] TLS WebSocket connected\r\n");
            if (!send_start_connection())
            {
                set_error("START_CONNECTION", "SEND_FAILED");
                return;
            }
            set_state(DOUBAO_STATE_STARTING_CONNECTION, now_ms);
            break;

        case ESP_MSG_WS_DATA:
            if (s_session_requested && !s_close_requested)
            {
                process_server_message(frame->payload, frame->payload_length, now_ms);
            }
            break;

        case ESP_MSG_WS_SENT:
            if (DOUBAO_AUDIO_TX_NONE != s_audio_tx_pending)
            {
                audio_send_acknowledged(now_ms);
            }
            else if ((DOUBAO_STATE_READY != s_state) && (frame->payload_length > 0U))
            {
                char report[96] = {0};
                uint32_t const copy_length =
                    (frame->payload_length < (sizeof(report) - 1U)) ?
                    frame->payload_length : (sizeof(report) - 1U);
                memcpy(report, frame->payload, copy_length);
                printf("[DB][ESP][TX] %s\r\n", report);
            }
            break;

        case ESP_MSG_WS_CLOSED:
            /* Replacing a previous WebSocket can deliver its close event after
             * a new open request has already been sent.  It does not belong to
             * the connection currently being established. */
            if ((DOUBAO_STATE_WAITING_FOR_WIFI == s_state) ||
                (DOUBAO_STATE_OPENING_WEBSOCKET == s_state))
            {
                printf("[DB][ESP] stale WebSocket close ignored\r\n");
                break;
            }
            if (s_close_requested || !s_session_requested)
            {
                s_close_requested = false;
                s_last_error[0] = '\0';
                if (s_session_requested)
                {
                    set_state(DOUBAO_STATE_WAITING_FOR_WIFI, now_ms);
                    (void) websocket_open_request(now_ms);
                }
                else
                {
                    set_state(DOUBAO_STATE_UNINITIALIZED, now_ms);
                }
            }
            else
            {
                set_error("WEBSOCKET", "CLOSED");
            }
            break;

        case ESP_MSG_ERROR:
        {
            char reason[64] = {0};
            uint32_t const copy_length = (frame->payload_length < (sizeof(reason) - 1U)) ?
                                         frame->payload_length : (sizeof(reason) - 1U);
            memcpy(reason, frame->payload, copy_length);
            printf("[DB][ESP] bridge error: %s\r\n", reason);
            if ((0 == strcmp(reason, "UART_FRAME_CRC")) &&
                (DOUBAO_AUDIO_TX_NONE != s_audio_tx_pending))
            {
                doubao_audio_tx_kind_t const retry_kind = s_audio_tx_pending;

                s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
                s_audio_tx_pending_samples = 0U;
                if (DOUBAO_AUDIO_TX_END_ASR == retry_kind)
                {
                    s_end_asr_pending = true;
                }
                printf("[DB][ESP] retry current push-to-talk packet after UART CRC\r\n");
                break;
            }
            set_error("ESP_BRIDGE", reason);
            break;
        }

        default:
            printf("[DB][ESP] ignored message=0x%02X channel=%u length=%lu\r\n",
                   frame->message,
                   frame->channel,
                   (unsigned long) frame->payload_length);
            break;
    }
}

bool DoubaoRealtime_Init(void)
{
    fsp_err_t speaker_error;

    if (s_initialized)
    {
        return true;
    }

    s_last_error[0] = '\0';
    s_session_requested = false;
    s_close_requested = false;
    s_wifi_announced = false;
    /* Preserve a READY/Wi-Fi frame cached before the chat page was opened. */
    s_microphone_requested = false;
    s_microphone_running = false;
    s_capture_stop_pending = false;
    s_recording_upload_active = false;
    s_recording_overflow = false;
    s_end_asr_pending = false;
    s_audio_tx_pending = DOUBAO_AUDIO_TX_NONE;
    s_audio_tx_pending_samples = 0U;
    s_recording_read_offset = 0U;
    s_recording_write_offset = 0U;
    s_recording_buffered_samples = 0U;
    s_recorded_samples = 0U;
    s_uploaded_samples = 0U;
    s_recording_sum_abs = 0U;
    s_recording_peak_abs = 0;
    s_recording_nonzero_samples = 0U;
    s_recording_first_sample = 0;
    s_recording_first_sample_valid = false;
    s_received_audio_bytes = 0U;
    s_dropped_audio_frames = 0U;
    s_final_user_text[0] = '\0';
    s_final_user_text_ready = false;
    s_connect_id[0] = '\0';
    s_session_id[0] = '\0';

    speaker_error = CloudSpeaker_Init();
    if (FSP_SUCCESS != speaker_error)
    {
        (void) snprintf(s_last_error,
                        sizeof(s_last_error),
                        "SPEAKER_INIT:FSP_%d",
                        (int) speaker_error);
        s_state = DOUBAO_STATE_ERROR;
        return false;
    }
    if (!esp_transport_init())
    {
        (void) snprintf(s_last_error, sizeof(s_last_error), "ESP_UART:INIT_FAILED");
        s_state = DOUBAO_STATE_ERROR;
        return false;
    }

    s_initialized = true;
    set_state(DOUBAO_STATE_UNINITIALIZED, 0U);
    printf("[DB][00] RA8P1 protocol engine initialized; waiting for ESP32C3 READY\r\n");
    return true;
}

bool DoubaoRealtime_Start(uint32_t now_ms)
{
    if (!s_initialized)
    {
        return false;
    }
    if (s_session_requested && (DOUBAO_STATE_ERROR != s_state))
    {
        return true;
    }

    s_session_requested = true;
    s_last_error[0] = '\0';
    session_runtime_clear();
    s_uploaded_samples = 0U;
    s_received_audio_bytes = 0U;
    s_dropped_audio_frames = 0U;

    if (s_wifi_reported && !s_wifi_announced)
    {
        printf("[DB][01] ESP32C3 bridge ready, Wi-Fi=%s\r\n",
               s_last_wifi_connected ? "CONNECTED" : "DISCONNECTED");
        s_wifi_announced = true;
    }

    if (s_close_requested)
    {
        set_state(DOUBAO_STATE_WAITING_FOR_WIFI, now_ms);
        return true;
    }
    return websocket_open_request(now_ms);
}

bool DoubaoRealtime_Stop(uint32_t now_ms)
{
    if (!s_initialized)
    {
        return false;
    }

    s_session_requested = false;
    s_wifi_announced = false;
    session_runtime_clear();
    if (s_close_requested)
    {
        return true;
    }

    s_close_requested = true;
    set_state(DOUBAO_STATE_UNINITIALIZED, now_ms);
    if (!esp_transport_send(ESP_CHANNEL_DOUBAO,
                            ESP_MSG_WS_CLOSE,
                            0U,
                            NULL,
                            0U))
    {
        s_close_requested = false;
        (void) snprintf(s_last_error,
                        sizeof(s_last_error),
                        "WS_CLOSE:BRIDGE_SEND_FAILED");
        return false;
    }
    return true;
}

void DoubaoRealtime_Poll(uint32_t now_ms)
{
    if (!s_initialized)
    {
        return;
    }

    CloudSpeaker_Poll();

    if (((DOUBAO_STATE_OPENING_WEBSOCKET == s_state) ||
         (DOUBAO_STATE_STARTING_CONNECTION == s_state) ||
         (DOUBAO_STATE_STARTING_SESSION == s_state)) &&
        ((uint32_t) (now_ms - s_state_started_ms) >= DOUBAO_HANDSHAKE_TIMEOUT_MS))
    {
        set_error("HANDSHAKE", "TIMEOUT");
        return;
    }

    if (DOUBAO_STATE_READY == s_state)
    {
        if (s_microphone_running || s_capture_stop_pending)
        {
            capture_to_recording();
        }
        recording_finish_if_stopped(now_ms);
        upload_recording_step(now_ms);
        send_end_asr_step();
    }
}

void DoubaoRealtime_NotifyTransportError(char const * reason)
{
    if (!s_initialized || (DOUBAO_STATE_ERROR == s_state))
    {
        return;
    }
    set_error("ESP_TRANSPORT", (NULL != reason) ? reason : "UNKNOWN");
}

bool DoubaoRealtime_SetMicrophone(bool enabled, uint32_t now_ms)
{
    if (!s_initialized || (DOUBAO_STATE_ERROR == s_state))
    {
        return false;
    }
    if (enabled && s_microphone_running)
    {
        return true;
    }
    if (enabled && (s_capture_stop_pending || s_recording_upload_active))
    {
        printf("[DB][MIC] start rejected: previous utterance is still finishing\r\n");
        return false;
    }

    s_microphone_requested = enabled;
    if (DOUBAO_STATE_READY != s_state)
    {
        printf("[DB][MIC] request queued until session is ready: %s\r\n",
               enabled ? "ON" : "OFF");
        return true;
    }

    if (enabled)
    {
        if (s_microphone_running || microphone_start())
        {
            return true;
        }
        s_microphone_requested = false;
        return false;
    }
    if (s_microphone_running)
    {
        return microphone_stop(now_ms);
    }
    return true;
}

bool DoubaoRealtime_MicrophoneRequested(void)
{
    return s_microphone_requested;
}

bool DoubaoRealtime_MicrophoneRunning(void)
{
    return s_microphone_running;
}

bool DoubaoRealtime_UploadingRecording(void)
{
    return s_capture_stop_pending ||
           (!s_microphone_running && s_recording_upload_active);
}

bool DoubaoRealtime_TakeFinalUserText(char * destination, size_t capacity)
{
    bool available;
    uint32_t interrupt_state;

    if ((NULL == destination) || (0U == capacity))
    {
        return false;
    }

    interrupt_state = __get_PRIMASK();
    __disable_irq();
    available = s_final_user_text_ready;
    if (available)
    {
        (void) snprintf(destination, capacity, "%s", s_final_user_text);
        s_final_user_text_ready = false;
    }
    __set_PRIMASK(interrupt_state);

    if (!available)
    {
        destination[0] = '\0';
    }
    return available;
}

uint32_t DoubaoRealtime_RecordedSamples(void)
{
    return s_recorded_samples;
}

doubao_realtime_state_t DoubaoRealtime_State(void)
{
    return s_state;
}

bool DoubaoRealtime_SendTtsPrompt(char const * text)
{
    char json[512];
    int length;

    if ((NULL == text) || ('\0' == text[0]))
    {
        return false;
    }

    if (DOUBAO_STATE_READY != s_state)
    {
        printf("[DB][TTS] SayHello rejected: state=%s\r\n",
               DoubaoRealtime_StateText());
        return false;
    }

    length = snprintf(json, sizeof(json),
                      "{\"content\":\"%s\"}", text);
    if ((length <= 0) || ((size_t) length >= sizeof(json)))
    {
        printf("[DB][TTS] SayHello JSON too long: %d bytes\r\n", length);
        return false;
    }

    printf("[DB][TTS] TX SayHello event=300, text=\"%s\"\r\n", text);
    return send_event_payload(DOUBAO_CLIENT_FULL_REQUEST,
                              DOUBAO_SERIALIZATION_JSON,
                              DOUBAO_EVENT_SAY_HELLO,
                              true,
                              (uint8_t const *) json,
                              (uint32_t) length);
}

void DoubaoRealtime_SetSayHelloText(char const * text)
{
    s_custom_hello_text = text;
}

void DoubaoRealtime_SetSilentSession(bool silent)
{
    s_silent_session = silent;
}

char const * DoubaoRealtime_StateText(void)
{
    static char const * const names[] =
    {
        "UNINITIALIZED",
        "WAIT_WIFI",
        "WS_OPENING",
        "START_CONNECTION",
        "START_SESSION",
        "READY",
        "ERROR",
    };

    if ((uint32_t) s_state >= (sizeof(names) / sizeof(names[0])))
    {
        return "INVALID";
    }
    return names[(uint32_t) s_state];
}

char const * DoubaoRealtime_LastError(void)
{
    return s_last_error;
}

uint32_t DoubaoRealtime_UploadedSamples(void)
{
    return s_uploaded_samples;
}

uint32_t DoubaoRealtime_ReceivedAudioBytes(void)
{
    return s_received_audio_bytes;
}

uint32_t DoubaoRealtime_DroppedAudioFrames(void)
{
    return s_dropped_audio_frames;
}
