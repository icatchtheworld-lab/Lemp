#include "Music_Rhythm/music_dance.h"

#include "Screen/drv_gpt_timer.h"
#include "Servo/servo.h"
#include "ServoLib/ServoDriver.h"
#include "hal_data.h"

#include <stddef.h>
#include <stdint.h>

#define MUSIC_DANCE_FRAME_COUNT               (4U)
#define MUSIC_DANCE_MINIMUM_MOVE_MS          (120U)
/*
 * 实体麦克风安装在灯体后，齿轮声和机身余振可能持续到动作后半段。
 * 舞蹈执行节拍已经限制为不高于100 BPM，因此动作拍最短为600 ms；
 * 最多屏蔽560 ms，仍为真实音乐保留至少一个32 ms频谱分析块。
 */
#define MUSIC_DANCE_MOTOR_NOISE_GUARD_MAX_MS (560U)
/* 在根据速度算出的运动时间后，再屏蔽一小段机身余振。 */
#define MUSIC_DANCE_MOTOR_NOISE_SETTLE_MS     (80U)
#define MUSIC_DANCE_HALF_TEMPO_THRESHOLD_BPM (100.0f)
#define MUSIC_DANCE_HEAD_SERVO_INDEX           (4U)
#define MUSIC_DANCE_HEAD_UP_POSITION           (2250)
#define MUSIC_DANCE_HEAD_DOWN_POSITION         (2800)
#define MUSIC_DANCE_FAST_HEAD_UP_POSITION      (2300)
#define MUSIC_DANCE_FAST_HEAD_DOWN_POSITION    (2720)
#define MUSIC_DANCE_NOD_DOWN_MOVE_MS            (240U)
#define MUSIC_DANCE_NOD_UP_MOVE_MS              (260U)
#define MUSIC_DANCE_NOD_RETURN_DELAY_MS          (240U)

typedef struct st_music_dance_keyframe
{
    int16_t position[MUSIC_DANCE_SERVO_COUNT];
    uint16_t move_ms; /* Maximum desired movement time from the recorded pose. */
    uint16_t hold_ms; /* Desired quiet time before the following beat. */
} music_dance_keyframe_t;

/*
 * 每一帧都提供清晰的大幅左右摆动或身体回弹。原来索引8～11的蜷缩、
 * 低头左右扭动和恢复动作已删除，避免连续几拍目标位置过于接近。
 * 5号舵机的位置会在每拍运行时被“低头 -> 抬头”状态机覆盖。
 */
static music_dance_keyframe_t const s_music_dance_sequence[MUSIC_DANCE_FRAME_COUNT] =
{
    {{1780, 1450, 3010, 1080, 2250}, 650U, 160U}, /* 1：身体明显摆向左侧。 */
    {{2050, 1710, 2760, 1260, 2250}, 650U, 160U}, /* 2：身体向上回弹。 */
    {{2390, 1460, 3035, 1450, 2250}, 650U, 160U}, /* 3：身体明显摆向右侧。 */
    {{2100, 1730, 2720, 1200, 2250}, 650U, 160U}, /* 4：挺起身体回到中间。 */
};

static uint8_t const s_music_dance_servo_ids[MUSIC_DANCE_SERVO_COUNT] =
{
    1U, 2U, 3U, 4U, 5U,
};

/* Servo 1 uses a higher dance speed so each wide swing can finish within one beat. */
static uint16_t const s_music_dance_speed_limits[MUSIC_DANCE_SERVO_COUNT] =
{
    2600U, 1000U, 1000U, 1600U, 2600U,
};

static uint16_t const s_music_dance_speed_minimums[MUSIC_DANCE_SERVO_COUNT] =
{
    80U, 100U, 100U, 180U, 180U,
};

static uint8_t const s_music_dance_accelerations[MUSIC_DANCE_SERVO_COUNT] =
{
    70U, 28U, 28U, 38U, 48U,
};

static int16_t const s_music_dance_center[MUSIC_DANCE_SERVO_COUNT] =
{
    2084, 1795, 2603, 1266, 2832,
};

/* Hard limits keep phrase variation inside recorded mechanical ranges. */
static int16_t const s_music_dance_position_minimums[MUSIC_DANCE_SERVO_COUNT] =
{
    1500, 1400, 2700, 1000, 1750,
};

static int16_t const s_music_dance_position_maximums[MUSIC_DANCE_SERVO_COUNT] =
{
    2600, 1750, 3075, 1450, 3200,
};

static int16_t s_music_dance_last_command[MUSIC_DANCE_SERVO_COUNT];
/* Scaled API calls expand dance frames outward from this session start pose. */
static int16_t s_music_dance_session_origin[MUSIC_DANCE_SERVO_COUNT];
static uint16_t s_music_dance_last_speed[MUSIC_DANCE_SERVO_COUNT];
static int16_t s_music_dance_phrase_offsets[MUSIC_DANCE_SERVO_COUNT];
static uint32_t s_music_dance_random_state = 0x6D2B79F5U;
static uint8_t s_music_dance_frame_index;
static bool s_music_dance_initialized;
static bool s_music_dance_half_tempo_active;
static bool s_music_dance_skip_next_beat;
/* 每拍先低头，随后由非阻塞调度器发送抬头命令。 */
static int16_t s_music_dance_nod_return_target[MUSIC_DANCE_SERVO_COUNT];
static uint32_t s_music_dance_nod_return_due_ms;
static bool s_music_dance_nod_return_pending;

static uint32_t music_dance_random_next(void)
{
    uint32_t value = s_music_dance_random_state;

    /* Xorshift32 provides bounded variation without heap or library RNG. */
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    s_music_dance_random_state = value;

    return value;
}

static int16_t music_dance_random_offset_get(uint16_t half_range)
{
    uint32_t const width = ((uint32_t) half_range * 2U) + 1U;

    return (int16_t) ((int32_t) (music_dance_random_next() % width) -
                      (int32_t) half_range);
}

static void music_dance_phrase_offsets_update(void)
{
    /*
     * Keep one random offset for a complete sub-phrase. This changes each
     * dance loop without introducing a new random twitch on every beat.
     * Coupled servos 2 and 3 receive opposite offsets.
     */
    s_music_dance_phrase_offsets[0] = music_dance_random_offset_get(45U);
    s_music_dance_phrase_offsets[1] = music_dance_random_offset_get(12U);
    s_music_dance_phrase_offsets[2] =
        (int16_t) -s_music_dance_phrase_offsets[1];
    s_music_dance_phrase_offsets[3] = music_dance_random_offset_get(10U);
    /* 5号舵机由每拍完整点头状态机控制，不叠加随机偏移。 */
    s_music_dance_phrase_offsets[4] = 0;
}

static int16_t music_dance_position_limit(uint32_t servo_index,
                                          int32_t position)
{
    if (position < s_music_dance_position_minimums[servo_index])
    {
        position = s_music_dance_position_minimums[servo_index];
    }
    else if (position > s_music_dance_position_maximums[servo_index])
    {
        position = s_music_dance_position_maximums[servo_index];
    }

    return (int16_t) position;
}

static uint32_t music_dance_absolute_distance(int16_t first, int16_t second)
{
    int32_t distance = (int32_t) first - second;

    if (distance < 0)
    {
        distance = -distance;
    }

    return (uint32_t) distance;
}

static void music_dance_position_copy(int16_t destination[MUSIC_DANCE_SERVO_COUNT],
                                      int16_t const source[MUSIC_DANCE_SERVO_COUNT])
{
    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        destination[i] = source[i];
    }
}

static uint32_t music_dance_period_ms_get(float bpm)
{
    if (bpm <= 0.0f)
    {
        return 0U;
    }

    return (uint32_t) ((60000.0f / bpm) + 0.5f);
}

/**
 * Convert a detected beat to a mechanically safe dance beat.
 *
 * Every tempo above 100 BPM is executed at half tempo: the movement duration
 * uses BPM/2 and only every second detected beat is allowed to send a command.
 * The tracker still receives and retains the original BPM.
 */
static bool music_dance_motion_tempo_prepare(float bpm,
                                             float * p_motion_bpm)
{
    bool const half_tempo_required =
        (bpm > MUSIC_DANCE_HALF_TEMPO_THRESHOLD_BPM);

    if (NULL == p_motion_bpm)
    {
        return false;
    }

    if (half_tempo_required != s_music_dance_half_tempo_active)
    {
        s_music_dance_half_tempo_active = half_tempo_required;
        s_music_dance_skip_next_beat = false;
    }

    *p_motion_bpm = half_tempo_required ? (bpm * 0.5f) : bpm;
    if (!half_tempo_required)
    {
        return true;
    }

    if (s_music_dance_skip_next_beat)
    {
        s_music_dance_skip_next_beat = false;
        return false;
    }

    s_music_dance_skip_next_beat = true;
    return true;
}

/** 根据原始BPM选择点头幅度；快速音乐保持较小幅度，避免头部抽动。 */
static void music_dance_nod_positions_get(float bpm,
                                          int16_t * p_up_position,
                                          int16_t * p_down_position)
{
    if ((NULL == p_up_position) || (NULL == p_down_position))
    {
        return;
    }

    if (bpm > MUSIC_DANCE_HALF_TEMPO_THRESHOLD_BPM)
    {
        *p_up_position = MUSIC_DANCE_FAST_HEAD_UP_POSITION;
        *p_down_position = MUSIC_DANCE_FAST_HEAD_DOWN_POSITION;
    }
    else
    {
        *p_up_position = MUSIC_DANCE_HEAD_UP_POSITION;
        *p_down_position = MUSIC_DANCE_HEAD_DOWN_POSITION;
    }
}

static uint32_t music_dance_move_ms_get(music_dance_keyframe_t const * p_keyframe,
                                        float bpm)
{
    uint32_t const period_ms = music_dance_period_ms_get(bpm);
    uint32_t move_ms;

    if ((NULL == p_keyframe) || (0U == period_ms))
    {
        return MUSIC_DANCE_MINIMUM_MOVE_MS;
    }

    /* Reserve the recorded hold time, then use the remaining beat period. */
    move_ms = (period_ms > p_keyframe->hold_ms) ?
              (period_ms - p_keyframe->hold_ms) : MUSIC_DANCE_MINIMUM_MOVE_MS;

    if (move_ms > p_keyframe->move_ms)
    {
        move_ms = p_keyframe->move_ms;
    }
    if (move_ms < MUSIC_DANCE_MINIMUM_MOVE_MS)
    {
        move_ms = MUSIC_DANCE_MINIMUM_MOVE_MS;
    }

    return move_ms;
}

static bool music_dance_command_send_timed(
    int16_t const target[MUSIC_DANCE_SERVO_COUNT],
    uint32_t move_ms,
    uint32_t head_move_ms,
    uint32_t * p_audio_guard_ms)
{
    uint16_t speeds[MUSIC_DANCE_SERVO_COUNT];
    uint32_t actual_move_ms = move_ms;

    if ((NULL == target) || (0U == move_ms))
    {
        return false;
    }

    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        uint32_t const distance =
            music_dance_absolute_distance(target[i], s_music_dance_last_command[i]);
        uint32_t const axis_requested_move_ms =
            ((MUSIC_DANCE_HEAD_SERVO_INDEX == i) && (head_move_ms > 0U)) ?
            head_move_ms : move_ms;
        uint32_t required_speed;
        uint32_t axis_move_ms;

        if (0U == distance)
        {
            /*
             * A divided-rate joint keeps the same target for several beats.
             * Preserve its previous speed so repeating that target does not
             * slow an unfinished movement down to the minimum speed.
             */
            required_speed = s_music_dance_last_speed[i];
            if (0U == required_speed)
            {
                required_speed = s_music_dance_speed_minimums[i];
            }
        }
        else
        {
            required_speed = ((distance * 1000U) + axis_requested_move_ms - 1U) /
                             axis_requested_move_ms;
        }

        if (required_speed < s_music_dance_speed_minimums[i])
        {
            required_speed = s_music_dance_speed_minimums[i];
        }
        if (required_speed > s_music_dance_speed_limits[i])
        {
            required_speed = s_music_dance_speed_limits[i];
        }

        speeds[i] = (uint16_t) required_speed;

        /*
         * move_ms只是期望时间。速度被安全上限截断后，真实运动时间可能
         * 更长，因此用最终速度重新计算每个舵机的预计运动时间。
         */
        axis_move_ms = (0U == distance) ? 0U :
            (((distance * 1000U) + required_speed - 1U) / required_speed);
        if (axis_move_ms > actual_move_ms)
        {
            actual_move_ms = axis_move_ms;
        }
    }

    if (0 == Servo_SyncWritePos(s_music_dance_servo_ids,
                                MUSIC_DANCE_SERVO_COUNT,
                                target,
                                speeds,
                                s_music_dance_accelerations))
    {
        return false;
    }

    music_dance_position_copy(s_music_dance_last_command, target);
    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        s_music_dance_last_speed[i] = speeds[i];
    }

    if (NULL != p_audio_guard_ms)
    {
        uint32_t guard_ms = actual_move_ms + MUSIC_DANCE_MOTOR_NOISE_SETTLE_MS;

        if (guard_ms > MUSIC_DANCE_MOTOR_NOISE_GUARD_MAX_MS)
        {
            guard_ms = MUSIC_DANCE_MOTOR_NOISE_GUARD_MAX_MS;
        }
        *p_audio_guard_ms = guard_ms;
    }

    return true;
}

static bool music_dance_command_send(
    int16_t const target[MUSIC_DANCE_SERVO_COUNT],
    uint32_t move_ms,
    uint32_t * p_audio_guard_ms)
{
    return music_dance_command_send_timed(target,
                                          move_ms,
                                          move_ms,
                                          p_audio_guard_ms);
}

/**
 * @brief 只向5号舵机发送一次拍内抬头命令。
 *
 * 低头命令下发240 ms后，1～4号舵机通常仍在执行本拍的身体动作。此时若
 * 再同步写入全部五个舵机，相同的身体目标也可能让舵机内部重新规划速度，
 * 造成动作中途减速或再次起步。因此回抬阶段只更新5号舵机。
 */
static bool music_dance_head_command_send(int16_t target_position,
                                          uint32_t move_ms,
                                          uint32_t * p_audio_guard_ms)
{
    uint32_t const head_index = MUSIC_DANCE_HEAD_SERVO_INDEX;
    uint32_t const distance = music_dance_absolute_distance(
        target_position,
        s_music_dance_last_command[head_index]);
    uint32_t required_speed;
    uint32_t actual_move_ms = move_ms;
    uint16_t speed;

    if (0U == move_ms)
    {
        return false;
    }

    if (0U == distance)
    {
        required_speed = s_music_dance_last_speed[head_index];
        if (0U == required_speed)
        {
            required_speed = s_music_dance_speed_minimums[head_index];
        }
    }
    else
    {
        required_speed = ((distance * 1000U) + move_ms - 1U) / move_ms;
    }

    if (required_speed < s_music_dance_speed_minimums[head_index])
    {
        required_speed = s_music_dance_speed_minimums[head_index];
    }
    if (required_speed > s_music_dance_speed_limits[head_index])
    {
        required_speed = s_music_dance_speed_limits[head_index];
    }
    speed = (uint16_t) required_speed;

    if (distance > 0U)
    {
        uint32_t const speed_limited_move_ms =
            ((distance * 1000U) + required_speed - 1U) / required_speed;

        if (speed_limited_move_ms > actual_move_ms)
        {
            actual_move_ms = speed_limited_move_ms;
        }
    }

    if (0 == Servo_SyncWritePos(&s_music_dance_servo_ids[head_index],
                                1U,
                                &target_position,
                                &speed,
                                &s_music_dance_accelerations[head_index]))
    {
        return false;
    }

    /* 回抬命令只改变5号舵机，身体四轴的目标和速度历史必须保持不变。 */
    s_music_dance_last_command[head_index] = target_position;
    s_music_dance_last_speed[head_index] = speed;

    if (NULL != p_audio_guard_ms)
    {
        uint32_t guard_ms = actual_move_ms + MUSIC_DANCE_MOTOR_NOISE_SETTLE_MS;

        if (guard_ms > MUSIC_DANCE_MOTOR_NOISE_GUARD_MAX_MS)
        {
            guard_ms = MUSIC_DANCE_MOTOR_NOISE_GUARD_MAX_MS;
        }
        *p_audio_guard_ms = guard_ms;
    }

    return true;
}

static bool music_dance_time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return ((int32_t) (now_ms - target_ms) >= 0);
}

static void music_dance_pending_nod_cancel(void)
{
    s_music_dance_nod_return_due_ms = 0U;
    s_music_dance_nod_return_pending = false;
}

/**
 * @brief 在一个节拍内启动“低头 -> 抬头”的第一段。
 *
 * 身体按照本拍关键帧继续移动，5号舵机使用独立的240 ms单程时间快速
 * 低头。抬头目标只记录下来，由主循环到期后异步发送，不阻塞音频处理。
 */
static bool music_dance_nod_begin(
    int16_t const body_target[MUSIC_DANCE_SERVO_COUNT],
    float bpm,
    uint8_t intensity_percent,
    uint32_t body_move_ms,
    uint32_t * p_audio_guard_ms)
{
    int16_t down_target[MUSIC_DANCE_SERVO_COUNT];
    int16_t up_position;
    int16_t down_position;

    if ((NULL == body_target) || s_music_dance_nod_return_pending)
    {
        return false;
    }

    music_dance_nod_positions_get(bpm, &up_position, &down_position);
    music_dance_position_copy(down_target, body_target);
    music_dance_position_copy(s_music_dance_nod_return_target, body_target);

    /* 缩放接口仍以本次舞蹈起始姿态为原点，0%时头部也不会突然跳动。 */
    down_position = (int16_t) ((int32_t) s_music_dance_session_origin[
        MUSIC_DANCE_HEAD_SERVO_INDEX] +
        ((((int32_t) down_position - s_music_dance_session_origin[
            MUSIC_DANCE_HEAD_SERVO_INDEX]) * intensity_percent) / 100));
    up_position = (int16_t) ((int32_t) s_music_dance_session_origin[
        MUSIC_DANCE_HEAD_SERVO_INDEX] +
        ((((int32_t) up_position - s_music_dance_session_origin[
            MUSIC_DANCE_HEAD_SERVO_INDEX]) * intensity_percent) / 100));

    down_target[MUSIC_DANCE_HEAD_SERVO_INDEX] = music_dance_position_limit(
        MUSIC_DANCE_HEAD_SERVO_INDEX,
        down_position);
    s_music_dance_nod_return_target[MUSIC_DANCE_HEAD_SERVO_INDEX] =
        music_dance_position_limit(MUSIC_DANCE_HEAD_SERVO_INDEX, up_position);

    if (!music_dance_command_send_timed(down_target,
                                        body_move_ms,
                                        MUSIC_DANCE_NOD_DOWN_MOVE_MS,
                                        p_audio_guard_ms))
    {
        return false;
    }

    s_music_dance_nod_return_due_ms =
        drv_gpt_timer_get_ms() + MUSIC_DANCE_NOD_RETURN_DELAY_MS;
    s_music_dance_nod_return_pending = true;
    return true;
}

bool music_dance_pending_update(uint32_t now_ms,
                                bool * p_command_sent,
                                uint32_t * p_audio_guard_ms)
{
    if (NULL != p_command_sent)
    {
        *p_command_sent = false;
    }
    if (NULL != p_audio_guard_ms)
    {
        *p_audio_guard_ms = 0U;
    }

    if (!s_music_dance_nod_return_pending ||
        !music_dance_time_reached(now_ms, s_music_dance_nod_return_due_ms))
    {
        return true;
    }

    /* 先清除pending，若通信失败由上层结束舞蹈，不能在主循环中反复发送。 */
    music_dance_pending_nod_cancel();
    if (!music_dance_head_command_send(
            s_music_dance_nod_return_target[MUSIC_DANCE_HEAD_SERVO_INDEX],
            MUSIC_DANCE_NOD_UP_MOVE_MS,
            p_audio_guard_ms))
    {
        return false;
    }

    if (NULL != p_command_sent)
    {
        *p_command_sent = true;
    }
    return true;
}

void music_dance_init(void)
{
    Servo_Init();
    Servo_NormalMode();
    Servo_Power_on();

    music_dance_session_begin(s_music_dance_center);

    /* Let the initial movement settle before microphone calibration begins. */
    R_BSP_SoftwareDelay(500U, BSP_DELAY_UNITS_MILLISECONDS);
}

void music_dance_session_begin(
    int16_t const start_pose[MUSIC_DANCE_SERVO_COUNT])
{
    if (NULL == start_pose)
    {
        return;
    }

    music_dance_position_copy(s_music_dance_last_command, start_pose);
    music_dance_position_copy(s_music_dance_session_origin, start_pose);
    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        /* Match the approximate speeds used by the existing servo poses. */
        s_music_dance_last_speed[i] =
            (2U == i) ? 1000U : 600U;
    }
    s_music_dance_frame_index = 0U;
    s_music_dance_half_tempo_active = false;
    s_music_dance_skip_next_beat = false;
    music_dance_pending_nod_cancel();
    s_music_dance_initialized = true;
}

bool music_dance_apply_intro_nod(float bpm,
                                 uint32_t * p_audio_guard_ms)
{
    int16_t target[MUSIC_DANCE_SERVO_COUNT];
    float motion_bpm = bpm;

    if (!s_music_dance_initialized)
    {
        return false;
    }

    if (NULL != p_audio_guard_ms)
    {
        *p_audio_guard_ms = 0U;
    }

    /* A skipped high-tempo beat is valid, but it must not advance intro state. */
    if (!music_dance_motion_tempo_prepare(bpm, &motion_bpm))
    {
        return true;
    }

    /* 身体保持当前姿态；本拍内部由状态机完整执行一次低头和抬头。 */
    music_dance_position_copy(target, s_music_dance_last_command);
    return music_dance_nod_begin(target,
                                 bpm,
                                 100U,
                                 MUSIC_DANCE_NOD_DOWN_MOVE_MS,
                                 p_audio_guard_ms);
}

bool music_dance_apply_beat(float bpm, uint32_t * p_audio_guard_ms)
{
    /* The standalone music test keeps using the complete recorded amplitude. */
    return music_dance_apply_beat_scaled(bpm, 100U, p_audio_guard_ms);
}

bool music_dance_apply_beat_scaled(float bpm,
                                   uint8_t intensity_percent,
                                   uint32_t * p_audio_guard_ms)
{
    music_dance_keyframe_t const * p_keyframe;
    int16_t target[MUSIC_DANCE_SERVO_COUNT];
    float motion_bpm = bpm;
    uint32_t move_ms;

    if (!s_music_dance_initialized)
    {
        return false;
    }

    if (NULL != p_audio_guard_ms)
    {
        *p_audio_guard_ms = 0U;
    }

    if (intensity_percent > 100U)
    {
        intensity_percent = 100U;
    }

    /* A skipped high-tempo beat is valid and deliberately sends no command. */
    if (!music_dance_motion_tempo_prepare(bpm, &motion_bpm))
    {
        return true;
    }

    p_keyframe = &s_music_dance_sequence[s_music_dance_frame_index];

    /* 每轮四拍开始时生成一次稳定的小幅随机差异。 */
    if (0U == s_music_dance_frame_index)
    {
        music_dance_phrase_offsets_update();
    }

    music_dance_position_copy(target, p_keyframe->position);

    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        target[i] = music_dance_position_limit(
            i,
            (int32_t) target[i] + s_music_dance_phrase_offsets[i]);

        /*
         * Keep the reusable scaled API relative to the real session pose.
         * The integrated desktop-pet mode passes 100% after its nod intro.
         */
        target[i] = (int16_t) ((int32_t) s_music_dance_session_origin[i] +
            ((((int32_t) target[i] - s_music_dance_session_origin[i]) *
               intensity_percent) / 100));
    }

    move_ms = music_dance_move_ms_get(p_keyframe, motion_bpm);

    if (!music_dance_nod_begin(target,
                               bpm,
                               intensity_percent,
                               move_ms,
                               p_audio_guard_ms))
    {
        return false;
    }

    s_music_dance_frame_index++;
    if (s_music_dance_frame_index >= MUSIC_DANCE_FRAME_COUNT)
    {
        s_music_dance_frame_index = 0U;
    }

    return true;
}

bool music_dance_return_center(uint32_t * p_audio_guard_ms)
{
    return music_dance_return_to_pose(s_music_dance_center,
                                      600U,
                                      p_audio_guard_ms);
}

bool music_dance_return_to_pose(
    int16_t const target_pose[MUSIC_DANCE_SERVO_COUNT],
    uint16_t move_ms,
    uint32_t * p_audio_guard_ms)
{
    if (!s_music_dance_initialized || (NULL == target_pose) || (0U == move_ms))
    {
        return false;
    }

    if (NULL != p_audio_guard_ms)
    {
        *p_audio_guard_ms = 0U;
    }

    /* 退舞或切换姿态前取消尚未执行的抬头，防止旧命令随后覆盖返回姿态。 */
    music_dance_pending_nod_cancel();

    if (!music_dance_command_send(target_pose, move_ms, p_audio_guard_ms))
    {
        return false;
    }

    music_dance_position_copy(s_music_dance_session_origin, target_pose);
    s_music_dance_frame_index = 0U;
    s_music_dance_half_tempo_active = false;
    s_music_dance_skip_next_beat = false;
    return true;
}

void music_dance_last_position_get(
    int16_t position[MUSIC_DANCE_SERVO_COUNT])
{
    if (NULL != position)
    {
        music_dance_position_copy(position, s_music_dance_last_command);
    }
}
