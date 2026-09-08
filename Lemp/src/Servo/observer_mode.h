#ifndef SERVO_OBSERVER_MODE_H_
#define SERVO_OBSERVER_MODE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * 音乐识别专项诊断开关。
 *
 * 1U：进入桌宠模式后跳过巡查和待机动作、关闭NPU视觉，只测试麦克风、
 *     节拍识别和舞蹈，并输出详细节拍日志。
 * 0U：恢复完整桌宠模式。
 */
#define OBSERVER_MUSIC_DIAGNOSTIC_ENABLE (0U)

/** 观察者页面可以显示的基础表情。 */
typedef enum e_observer_expression
{
    OBSERVER_EXPRESSION_NEUTRAL = 0,
    OBSERVER_EXPRESSION_HAPPY,
    OBSERVER_EXPRESSION_CURIOUS_LEFT,
    OBSERVER_EXPRESSION_CURIOUS_RIGHT,
    OBSERVER_EXPRESSION_FOCUSED,
    OBSERVER_EXPRESSION_ALERT,
    OBSERVER_EXPRESSION_LISTENING,
    OBSERVER_EXPRESSION_SLEEPING,
    OBSERVER_EXPRESSION_RESPONDING
} observer_expression_t;

/** 观察者模式内部行为状态，保证同一时刻只有一套动作控制舵机。 */
typedef enum e_observer_behavior
{
    OBSERVER_BEHAVIOR_OBSERVING = 0,
    OBSERVER_BEHAVIOR_CALIBRATING,     /* 舵机静止，专门采集环境底噪。 */
    OBSERVER_BEHAVIOR_STANDBY,
    OBSERVER_BEHAVIOR_NODDING,
    OBSERVER_BEHAVIOR_TRACKING,        /* 人脸确认后跟踪，期间暂停音乐 */
    OBSERVER_BEHAVIOR_MUSIC_LISTENING, /* 音乐按钮开启后静止等待节拍。 */
    OBSERVER_BEHAVIOR_DANCING,
    OBSERVER_BEHAVIOR_GOING_TO_SLEEP,
    OBSERVER_BEHAVIOR_SLEEPING,
    OBSERVER_BEHAVIOR_WAKING
} observer_behavior_t;

/** NPU产生一组新识别结果后提交给观察者状态机的目标类型。 */
typedef enum e_observer_visual_target
{
    OBSERVER_VISUAL_TARGET_NONE = 0,
    OBSERVER_VISUAL_TARGET_BOOK,
    OBSERVER_VISUAL_TARGET_FACE
} observer_visual_target_t;

void ObserverMode_Start(void);
void ObserverMode_Stop(void);
void ObserverMode_Update(void);

/** 开启后静止监听音乐并暂停视觉；校准期间的开启请求会在校准完成后生效。 */
void ObserverMode_SetMusicModeEnabled(bool enabled);
/** 返回音乐按钮是否已开启（包括校准期间尚未生效的开启请求）。 */
bool ObserverMode_IsMusicModeEnabled(void);

/** 每次NPU产生一组新结果时调用一次，不能对同一帧重复调用。 */
void ObserverMode_SubmitVisionResult(observer_visual_target_t target);

/**
 * 在 TRACKING 状态下提交一帧人脸检测结果。
 * valid 为 false 表示本帧未检测到人脸；首个无脸结果启动4秒丢失计时。
 * 计时期间保留最后的人脸目标，任意有效帧都会立即恢复跟踪。
 * box_center_x/y 为人脸框中心在 160x120 图像中的坐标。
 * 同一帧只提交一次；未被 Update 消费前的新提交会覆盖旧结果。
 */
void ObserverMode_SubmitTrackingFrame(bool valid, int16_t box_center_x, int16_t box_center_y);

/** 从麦克风队列中读取少量数据，完成轻量声音唤醒和连续声音判断。 */
void ObserverMode_AudioUpdate(void);

bool ObserverMode_IsActive(void);
bool ObserverMode_IsVisionRequired(void);
/** 只在入场校准、音乐监听和跳舞期间需要麦克风。 */
bool ObserverMode_IsHearingRequired(void);
/** 舞蹈以完整幅度运行时返回100，非舞蹈状态返回0。 */
uint8_t ObserverMode_GetDanceIntensity(void);
/** 舞蹈时返回经过倍频保护的稳定BPM，其他状态返回0。 */
uint16_t ObserverMode_GetMusicBpm(void);
uint8_t ObserverMode_GetActionIndex(void);
uint8_t ObserverMode_GetActionCount(void);
observer_behavior_t ObserverMode_GetBehavior(void);
observer_expression_t ObserverMode_GetExpression(void);

#endif /* SERVO_OBSERVER_MODE_H_ */
