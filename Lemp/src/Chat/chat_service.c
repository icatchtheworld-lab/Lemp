#include "Chat/chat_service.h"

#include "Audio/cloud_speaker.h"
#include "applications/app.h"
#include "Voice/voice.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CHAT_TEXT_SIZE                    (192U)
#define CHAT_COMMAND_DUPLICATE_WINDOW_MS (1500U)

/* UTF-8 is expressed as bytes so command matching is independent of the
 * compiler source-code page and of how the serial terminal renders Chinese. */
#define UTF8_LIGHT_ON          "\xE5\xBC\x80\xE7\x81\xAF"
#define UTF8_TURN_ON_LIGHT     "\xE6\x89\x93\xE5\xBC\x80\xE7\x81\xAF"
#define UTF8_LIGHT_TURN_ON     "\xE7\x81\xAF\xE6\x89\x93\xE5\xBC\x80"
#define UTF8_TURN_ON_ONCE      "\xE5\xBC\x80\xE4\xB8\x80\xE4\xB8\x8B\xE7\x81\xAF"
#define UTF8_LIGHT_UP          "\xE4\xBA\xAE\xE7\x81\xAF"
#define UTF8_LIGHT_OFF         "\xE5\x85\xB3\xE7\x81\xAF"
#define UTF8_TURN_OFF_LIGHT    "\xE5\x85\xB3\xE9\x97\xAD\xE7\x81\xAF"
#define UTF8_LIGHT_TURN_OFF    "\xE7\x81\xAF\xE5\x85\xB3\xE6\x8E\x89"
#define UTF8_TURN_OFF_ONCE     "\xE5\x85\xB3\xE4\xB8\x80\xE4\xB8\x8B\xE7\x81\xAF"
#define UTF8_EXTINGUISH_LIGHT  "\xE7\x86\x84\xE7\x81\xAF"
#define UTF8_SIT_DOWN          "\xE5\x9D\x90\xE4\xB8\x8B"
#define UTF8_LIE_DOWN          "\xE8\xB6\xB4\xE4\xB8\x8B"
#define UTF8_FOLD_UP           "\xE6\x94\xB6\xE8\xB5\xB7"
#define UTF8_STAND             "\xE7\xAB\x99\xE7\xAB\x8B"
#define UTF8_STAND_UP          "\xE7\xAB\x99\xE8\xB5\xB7\xE6\x9D\xA5"
#define UTF8_RISE              "\xE8\xB5\xB7\xE8\xBA\xAB"
#define UTF8_SHAKE_HEAD        "\xE6\x91\x87\xE5\xA4\xB4"
#define UTF8_SHAKE_HEAD_TWICE  "\xE6\x91\x87\xE6\x91\x87\xE5\xA4\xB4"
#define UTF8_SHAKE_HEAD_ONCE   "\xE6\x91\x87\xE4\xB8\x80\xE4\xB8\x8B\xE5\xA4\xB4"
#define UTF8_DO_NOT            "\xE4\xB8\x8D\xE8\xA6\x81"
#define UTF8_DONT              "\xE5\x88\xAB"
#define UTF8_NO_NEED           "\xE4\xB8\x8D\xE7\x94\xA8"

static bool s_initialized;
static char s_last_error[64];
static char s_last_command_text[CHAT_TEXT_SIZE];
static uint32_t s_last_command_ms;
static bool s_prompt_playing;
static bool s_prewarmed;            /* 台灯模式期间会话已预热 */
static bool s_prompt_via_prewarm;   /* 当前提醒走 prewarm 即发路径（完成时保留会话） */

static bool chat_text_contains(char const * text, char const * token)
{
    return (NULL != text) && (NULL != token) && (NULL != strstr(text, token));
}

static app_chat_command_t chat_command_from_text(char const * text)
{
    if ((NULL == text) || ('\0' == text[0]))
    {
        return APP_CHAT_COMMAND_NONE;
    }
    if (chat_text_contains(text, UTF8_DO_NOT) ||
        chat_text_contains(text, UTF8_DONT) ||
        chat_text_contains(text, UTF8_NO_NEED))
    {
        return APP_CHAT_COMMAND_NONE;
    }

    if (chat_text_contains(text, UTF8_LIGHT_OFF) ||
        chat_text_contains(text, UTF8_TURN_OFF_LIGHT) ||
        chat_text_contains(text, UTF8_LIGHT_TURN_OFF) ||
        chat_text_contains(text, UTF8_TURN_OFF_ONCE) ||
        chat_text_contains(text, UTF8_EXTINGUISH_LIGHT) ||
        chat_text_contains(text, "LIGHT_OFF"))
    {
        return APP_CHAT_COMMAND_LIGHT_OFF;
    }
    if (chat_text_contains(text, UTF8_LIGHT_ON) ||
        chat_text_contains(text, UTF8_TURN_ON_LIGHT) ||
        chat_text_contains(text, UTF8_LIGHT_TURN_ON) ||
        chat_text_contains(text, UTF8_TURN_ON_ONCE) ||
        chat_text_contains(text, UTF8_LIGHT_UP) ||
        chat_text_contains(text, "LIGHT_ON"))
    {
        return APP_CHAT_COMMAND_LIGHT_ON;
    }
    if (chat_text_contains(text, UTF8_SIT_DOWN) ||
        chat_text_contains(text, UTF8_LIE_DOWN) ||
        chat_text_contains(text, UTF8_FOLD_UP) ||
        chat_text_contains(text, "SIT"))
    {
        return APP_CHAT_COMMAND_SIT;
    }
    if (chat_text_contains(text, UTF8_STAND) ||
        chat_text_contains(text, UTF8_STAND_UP) ||
        chat_text_contains(text, UTF8_RISE) ||
        chat_text_contains(text, "STAND"))
    {
        return APP_CHAT_COMMAND_STAND;
    }
    if (chat_text_contains(text, UTF8_SHAKE_HEAD_TWICE) ||
        chat_text_contains(text, UTF8_SHAKE_HEAD_ONCE) ||
        chat_text_contains(text, UTF8_SHAKE_HEAD) ||
        chat_text_contains(text, "SHAKE_HEAD"))
    {
        return APP_CHAT_COMMAND_SHAKE_HEAD;
    }
    return APP_CHAT_COMMAND_NONE;
}

static void chat_service_handle_final_text(uint32_t now_ms)
{
    char text[CHAT_TEXT_SIZE];
    app_chat_command_t command;

    if (!DoubaoRealtime_TakeFinalUserText(text, sizeof(text)))
    {
        return;
    }
    command = chat_command_from_text(text);
    if (APP_CHAT_COMMAND_NONE == command)
    {
        return;
    }
    if ((0 == strcmp(text, s_last_command_text)) &&
        ((uint32_t) (now_ms - s_last_command_ms) < CHAT_COMMAND_DUPLICATE_WINDOW_MS))
    {
        printf("[CHAT_CMD] duplicate ASR result ignored\r\n");
        return;
    }

    (void) snprintf(s_last_command_text, sizeof(s_last_command_text), "%s", text);
    s_last_command_ms = now_ms;
    if (!app_main_request_chat_command(command))
    {
        printf("[CHAT_CMD] request rejected, command=%u\r\n", (unsigned int) command);
        return;
    }
    printf("[CHAT_CMD] accepted, command=%u utf8_bytes=%lu\r\n",
           (unsigned int) command,
           (unsigned long) strlen(text));
}

static void chat_service_error_set(char const * stage, fsp_err_t err)
{
    (void) snprintf(s_last_error,
                    sizeof(s_last_error),
                    "%s:FSP_%d",
                    (NULL != stage) ? stage : "INIT",
                    (int) err);
}

bool chat_service_enter(uint32_t now_ms)
{
    fsp_err_t err;

    /* Board revisions may strap the microphone to either I2S slot. */
    Voice_ChannelSet(VOICE_CHANNEL_AUTO);

    if (!s_initialized)
    {
        s_last_error[0] = '\0';
        err = Voice_Init();
        if (FSP_SUCCESS != err)
        {
            chat_service_error_set("MIC_INIT", err);
            return false;
        }

        if (!DoubaoRealtime_Init())
        {
            (void) snprintf(s_last_error,
                            sizeof(s_last_error),
                            "%s",
                            DoubaoRealtime_LastError());
            return false;
        }
        s_initialized = true;
    }

    if (!DoubaoRealtime_Start(now_ms))
    {
        (void) snprintf(s_last_error,
                        sizeof(s_last_error),
                        "%s",
                        DoubaoRealtime_LastError());
        return false;
    }

    s_last_error[0] = '\0';
    s_last_command_text[0] = '\0';
    s_last_command_ms = 0U;
    printf("[UI] Chat screen button ready: tap once to start, tap again to send EndASR\r\n");
    return true;
}

void chat_service_leave(uint32_t now_ms)
{
    if (!s_initialized)
    {
        return;
    }

    (void) DoubaoRealtime_Stop(now_ms);
}

void chat_service_poll(uint32_t now_ms)
{
    if (!s_initialized)
    {
        return;
    }

    DoubaoRealtime_Poll(now_ms);
    chat_service_handle_final_text(now_ms);
    /*
     * 自动清理 is_playing_prompt：原函数零调用导致 s_prompt_playing 永不清除，
     * 后续提醒会被 play_rest_reminder 顶部的 s_prompt_playing 守卫拦截。
     * 函数内部已用 s_prompt_playing 早返回，非播报期间零开销。
     */
    (void) chat_service_is_playing_prompt();
}

bool chat_service_set_microphone(bool enabled, uint32_t now_ms)
{
    if (!s_initialized)
    {
        (void) snprintf(s_last_error, sizeof(s_last_error), "CHAT_NOT_READY");
        return false;
    }

    printf("[UI][CHAT] screen button -> %s\r\n",
           enabled ? "start live microphone stream" : "stop stream and send EndASR");
    if (!DoubaoRealtime_SetMicrophone(enabled, now_ms))
    {
        (void) snprintf(s_last_error,
                        sizeof(s_last_error),
                        "%s",
                        DoubaoRealtime_LastError());
        return false;
    }

    s_last_error[0] = '\0';
    return true;
}

bool chat_service_is_initialized(void)
{
    return s_initialized;
}

char const * chat_service_last_error(void)
{
    if ('\0' != s_last_error[0])
    {
        return s_last_error;
    }
    return DoubaoRealtime_LastError();
}

bool chat_service_play_rest_reminder(uint32_t now_ms)
{
    doubao_realtime_state_t state;

    if (s_prompt_playing)
    {
        printf("[CHAT][PROMPT] rest reminder already playing, skip\r\n");
        return false;
    }

    state = DoubaoRealtime_State();

    /*
     * Path A：prewarm 已就绪 → 即时发 SayHello，绕过握手延迟。
     * SendTtsPrompt 内部直接用传入 text 拼 JSON，不读 s_custom_hello_text，
     * 所以这里不需要 SetSayHelloText。
     */
    if (s_prewarmed && (DOUBAO_STATE_READY == state))
    {
        if (!DoubaoRealtime_SendTtsPrompt("请注意休息，保护眼睛哦"))
        {
            printf("[CHAT][PROMPT] prewarm SendTtsPrompt failed, fallback\r\n");
            /* 落入下方原流程 */
        }
        else
        {
            s_prompt_playing = true;
            s_prompt_via_prewarm = true;
            printf("[CHAT][PROMPT] rest reminder fired via prewarm (immediate)\r\n");
            return true;
        }
    }

    /*
     * Path B/C：prewarm 握手中 或 无 prewarm。
     * Path B（s_prewarmed=true 但 state!=READY）：prewarm 握手中，
     *   必须清 s_silent_session，否则 STARTING_SESSION→READY 跳转处会跳过 SayHello。
     * Path C（!s_prewarmed）：完全原行为。
     */
    if (s_prewarmed)
    {
        DoubaoRealtime_SetSilentSession(false);
    }
    /*
     * 必须在 chat_service_enter() 之前设置自定义 SayHello 文本。
     * 会话连接建立后 send_say_hello() 会自动使用该文本，
     * 从而与实时聊天页面的"灯小语自我介绍"区分开。
     */
    DoubaoRealtime_SetSayHelloText("请注意休息，保护眼睛哦");

    if (!chat_service_enter(now_ms))
    {
        printf("[CHAT][PROMPT] enter failed: %s\r\n", chat_service_last_error());
        DoubaoRealtime_SetSayHelloText(NULL);
        if (s_prewarmed)
        {
            /* 回滚：恢复静默以便 prewarm_poll 后续重连不自动 SayHello */
            DoubaoRealtime_SetSilentSession(true);
        }
        return false;
    }

    s_prompt_playing = true;
    s_prompt_via_prewarm = false;
    printf("[CHAT][PROMPT] rest reminder queued via handshake path\r\n");
    return true;
}

bool chat_service_is_playing_prompt(void)
{
    doubao_realtime_state_t state;

    if (!s_prompt_playing)
    {
        return false;
    }

    state = DoubaoRealtime_State();
    if (DOUBAO_STATE_ERROR == state)
    {
        printf("[CHAT][PROMPT] Doubao error during prompt: %s\r\n",
               DoubaoRealtime_LastError());
        DoubaoRealtime_SetSayHelloText(NULL);
        if (s_prompt_via_prewarm)
        {
            /*
             * prewarm 路径出错：不主动 leave，让 prewarm_poll 检测到 ERROR 后重连。
             * 仅清播报标志；s_prewarmed 保留。
             */
            s_prompt_via_prewarm = false;
            s_prompt_playing = false;
        }
        else
        {
            chat_service_leave(0U);
            s_prompt_playing = false;
        }
        return false;
    }

    /* 语音播报结束后自动清理：CloudSpeaker不再忙碌且没有缓冲数据。 */
    if (!CloudSpeaker_IsBusy() && (DOUBAO_STATE_READY == state))
    {
        DoubaoRealtime_SetSayHelloText(NULL);
        if (s_prompt_via_prewarm)
        {
            /*
             * prewarm 路径 TTS 完成：保留会话供下次提醒复用。
             * 恢复 s_silent_session=true 以防 Path B 曾临时把它设回 false。
             */
            DoubaoRealtime_SetSilentSession(true);
            s_prompt_via_prewarm = false;
            s_prompt_playing = false;
            printf("[CHAT][PROMPT] TTS finished (prewarm path), session kept\r\n");
        }
        else
        {
            chat_service_leave(0U);
            s_prompt_playing = false;
            printf("[CHAT][PROMPT] TTS finished, cleanup done\r\n");
        }
        return false;
    }

    return true;
}

void chat_service_stop_prompt(void)
{
    if (!s_prompt_playing)
    {
        return;
    }

    DoubaoRealtime_SetSayHelloText(NULL);
    /*
     * 始终 teardown：豆包协议无客户端中断 TTS 的事件，
     * 只有 WS_CLOSE 才能让服务端停止推送 352 音频帧。
     * s_prewarmed 不清：让 prewarm_poll 在下一 tick 重新预热。
     */
    chat_service_leave(0U);
    s_prompt_playing = false;
    s_prompt_via_prewarm = false;
    printf("[CHAT][PROMPT] stopped by user\r\n");
}

bool chat_service_prewarm(uint32_t now_ms)
{
    if (s_prewarmed)
    {
        return true;
    }
    if (s_prompt_playing)
    {
        /* 提醒正在播报或排队，不打扰；播报完成后由 prewarm_poll 重新预热 */
        return false;
    }

    /*
     * 静默会话：握手到 READY 时不自动触发 send_say_hello()。
     * 提醒触发时由 DoubaoRealtime_SendTtsPrompt 直接发 event=300。
     */
    DoubaoRealtime_SetSilentSession(true);
    if (!chat_service_enter(now_ms))
    {
        printf("[CHAT][PREWARM] enter failed: %s\r\n", chat_service_last_error());
        DoubaoRealtime_SetSilentSession(false);
        return false;
    }
    s_prewarmed = true;
    printf("[CHAT][PREWARM] prewarm started\r\n");
    return true;
}

void chat_service_prewarm_poll(uint32_t now_ms)
{
    doubao_realtime_state_t state;

    if (!s_prewarmed)
    {
        return;
    }
    if (s_prompt_playing)
    {
        /* 提醒进行中，让 is_playing_prompt 处理；它完成后才会回到这里 */
        return;
    }

    state = DoubaoRealtime_State();
    if ((DOUBAO_STATE_ERROR == state) ||
        (DOUBAO_STATE_UNINITIALIZED == state))
    {
        /*
         * 120s 服务端空闲超时或网络波动导致 WS 关闭。
         * DoubaoRealtime_Start 对 ERROR 状态不 early-return，会重新发起握手。
         */
        printf("[CHAT][PREWARM] state=%s, re-prewarming\r\n",
               DoubaoRealtime_StateText());
        DoubaoRealtime_SetSilentSession(true);
        (void) chat_service_enter(now_ms);
    }
}

void chat_service_teardown_prewarm(void)
{
    if (!s_prewarmed)
    {
        return;
    }

    /*
     * 退出台灯模式或进入聊天页前调用：
     * - 清静默标志，恢复聊天页的自动 SayHello 行为
     * - 若提醒尚在播报，强制停止（DoubaoRealtime_Stop -> session_runtime_clear
     *   -> CloudSpeaker_StopAndClear）
     * - 关闭 WS，s_prewarmed=false 让下次进入台灯模式可重新预热
     */
    DoubaoRealtime_SetSilentSession(false);
    DoubaoRealtime_SetSayHelloText(NULL);
    s_prompt_playing = false;
    s_prompt_via_prewarm = false;
    chat_service_leave(0U);
    s_prewarmed = false;
    printf("[CHAT][PREWARM] teardown done\r\n");
}
