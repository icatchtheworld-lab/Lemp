#include "Servo/observer_mode.h"

#include "Screen/drv_gpt_timer.h"
#include "Music_Rhythm/music_beat_tracker.h"
#include "Music_Rhythm/music_dance.h"
#include "Music_Rhythm/music_onset.h"
#include "Servo/servo.h"
#include "ServoLib/ServoDriver.h"
#include "Voice/voice.h"
#include "arm_math.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 动作调度参数 ---- */
#define OBS_SERVO_COUNT                   (5U)
#define OBS_UPDATE_PERIOD_MS             (20U)
#define OBS_MOTION_SETTLE_MARGIN_MS         (0U)
/* 进入桌宠模式并完成环境底噪校准后，先执行一轮大范围巡视动作。 */
#define OBS_INITIAL_PATROL_ENABLE           (1U)
/*
 * 设为1时只循环巡查动作，供动作设计阶段反复观察；正常运行必须设为0，
 * 恢复“巡查一次 -> 待机约30秒 -> 睡眠 -> 声音唤醒”的完整状态流。
 */
#define OBS_PATROL_REVIEW_LOOP_ENABLE       (0U)
/*
 * hold_ms为0时把该帧视为连续路径的中间点，提前开放下一帧命令。
 * 数值越大越流畅，但舵机越可能不完全经过中间关键点的精确位置。
 */
#define OBS_CONTINUOUS_BLEND_LEAD_MS       (150U)
/*
 * 非零动作时间最少为一个观察者更新周期。move_ms写成0U时不使用该下限，
 * 而是让每个发生位移的舵机按照下方配置的速度上限尽快到达目标。
 */
#define OBS_MIN_COMMAND_MOVE_MS            (50U)
#define OBS_NOD_SEQUENCE_MAX_FRAMES         (4U)
#define OBS_BOOK_NOD_OFFSET_COUNTS          (220)
#define OBS_NOD_SERVO5_MIN               (2400)
#define OBS_NOD_SERVO5_MAX               (3000)
#define OBS_FACE_CHEST_SERVO2_POSITION    (2030)
#define OBS_FACE_CHEST_SERVO3_POSITION    (2630)
#define OBS_FACE_NOD_UP_POSITION          (2400)
#define OBS_FACE_NOD_DOWN_POSITION        (2820)
#define OBS_NOD_COOLDOWN_MS              (3000U)

/* ---- 目标跟踪参数（移植自摄像头预览测试 app_camera_lcd_preview_test.c） ---- */
#define OBS_TRACK_IMAGE_W                  (160U)  /* SCC8660_W */
#define OBS_TRACK_IMAGE_H                  (120U)  /* SCC8660_H */
#define OBS_TRACK_DEAD_ZONE_PIXELS         (8)
#define OBS_TRACK_SERVO1_MIN               (1500)
#define OBS_TRACK_SERVO1_MAX               (2700)
#define OBS_TRACK_SERVO2_MIN               (1500)
#define OBS_TRACK_SERVO2_MAX               (2050)
#define OBS_TRACK_SERVO5_MIN               (2000)
#define OBS_TRACK_SERVO5_MAX               (3200)
#define OBS_TRACK_SERVO1_COUNTS_PER_PIXEL  (20)
#define OBS_TRACK_SERVO2_COUNTS_PER_PIXEL  (20)
#define OBS_TRACK_SERVO5_COUNTS_PER_PIXEL  (60)
#define OBS_TRACK_COMMAND_CHANGE_MIN       (6)
#define OBS_TRACK_SERVO_SPEED              (300U)
#define OBS_TRACK_SERVO_ACC                (10U)
#define OBS_TRACK_FACE_LOST_TIMEOUT_MS   (4000U)

/* 队友的跟踪版本以响应速度优先：首个可信人脸结果即可取得舵机控制权。 */
#define OBS_VISION_CONFIRM_COUNT            (1U)
#define OBS_VISION_ALLOWED_MISS_COUNT       (2U)

/*
 * 麦克风每帧128点约8 ms，13帧组成约104 ms响度分析窗口。NPU推理期间
 * 音频队列仍会积累，因此每轮最多追赶16帧，避免视觉开启后音乐识别退化。
 */
#define OBS_AUDIO_FRAME_CAPACITY          (128U)
#define OBS_AUDIO_FRAMES_PER_UPDATE_MAX    (16U)
#define OBS_AUDIO_FRAMES_PER_WINDOW        (13U)
#define OBS_AUDIO_CALIBRATION_WINDOWS      (20U)
/* 睡眠唤醒使用独立响度门限，避免音乐灵敏度调整影响睡眠状态。 */
#define OBS_AUDIO_LOUD_THRESHOLD_NUMERATOR  (4U)
#define OBS_AUDIO_LOUD_THRESHOLD_DENOMINATOR (1U)
/* 连续6个约104 ms窗口均为明显声音才唤醒，可过滤短促杂音。 */
#define OBS_AUDIO_WAKE_CONFIRM_WINDOWS      (6U)
#define OBS_AUDIO_BASELINE_FILTER_WEIGHT   (31U)
#define OBS_AUDIO_BASELINE_FILTER_DIVISOR  (32U)

/* ---- 待机音乐节奏识别参数 ---- */
#define OBS_MUSIC_SAMPLE_RATE_HZ          (16000U)
#define OBS_MUSIC_FFT_SIZE                  (512U)
#define OBS_MUSIC_FFT_HALF_SIZE             (OBS_MUSIC_FFT_SIZE / 2U)
#define OBS_MUSIC_BLOCK_DURATION_MS         \
    ((OBS_MUSIC_FFT_SIZE * 1000U) / OBS_MUSIC_SAMPLE_RATE_HZ)
#define OBS_MUSIC_FRAME_RATE_HZ             \
    (1000.0f / (float32_t) OBS_MUSIC_BLOCK_DURATION_MS)
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
/* 诊断期间与MUSIC_TEST_ENABLE使用完全相同的检测参数，便于公平对比。 */
#define OBS_MUSIC_ONSET_SENSITIVITY         (82U)
#define OBS_MUSIC_NOISE_MEAN_MULTIPLIER     (1.50f)
#define OBS_MUSIC_NOISE_DEVIATION_MULTIPLIER (4.00f)
#else
/* 音乐只会由用户主动开启，因此可提高起音灵敏度而不影响摄像头待机。 */
#define OBS_MUSIC_ONSET_SENSITIVITY         (92U)
#define OBS_MUSIC_NOISE_MEAN_MULTIPLIER     (1.30f)
#define OBS_MUSIC_NOISE_DEVIATION_MULTIPLIER (3.00f)
#endif
#define OBS_MUSIC_ONSET_MINIMUM_INTERVAL_MS (200U)
/* 160个32 ms频谱块约为5.12秒，用于测量待机环境底噪。 */
#define OBS_MUSIC_CALIBRATION_BLOCKS        (160U)
#define OBS_MUSIC_CALIBRATION_REPORT_STEP    (40U)
#define OBS_MUSIC_SAMPLE_SCALE              (1.0f / 32768.0f)
#define OBS_MUSIC_MAGNITUDE_SCALE           (4.0f / (float32_t) OBS_MUSIC_FFT_SIZE)
#define OBS_MUSIC_MINIMUM_ACTIVE_BANDS      (2U)
/* 装入外壳后频谱可能变窄；单频带必须达到总底噪两倍才允许进入起音检测。 */
#define OBS_MUSIC_SINGLE_BAND_TOTAL_MULTIPLIER (2.00f)
#define OBS_MUSIC_MOTOR_NOISE_GUARD_MAX_MS  (200U)
/*
 * 舵机到达目标后，机身和麦克风支架仍可能短暂振动。校准阶段额外等待
 * 200 ms，且丢弃跨越保护边界的半帧音频，避免把结构传声学习成底噪。
 */
#define OBS_MUSIC_CALIBRATION_SETTLE_MS      (200U)
/*
 * 舵机上电姿态包含较大的展开动作；等待2秒让舵机到位并消除灯体余振，
 * 然后才启动麦克风。这样电机声不会进入I2S队列，更不会参与底噪校准。
 */
#define OBS_MUSIC_STARTUP_SETTLE_MS          (2000U)
/* 切入音乐模式后保持静止一段时间，避开上一条摄像头/待机舵机命令的余振。 */
#define OBS_MUSIC_MODE_START_SETTLE_MS       (1200U)
/* 2秒静置加约5.12秒采样，正常约7.1秒完成；超时后降级为仅摄像头。 */
#define OBS_MUSIC_CALIBRATION_TIMEOUT_MS    (15000U)

/* 锁定节奏后先明显点头四拍，再用一拍衔接到完整舞蹈。 */
#define OBS_MUSIC_DANCE_INTRO_NOD_COUNT      (4U)
#define OBS_MUSIC_DANCE_FULL_PERCENT       (100U)
#define OBS_MUSIC_DANCE_RETURN_MS           (900U)
/* 退出舞蹈后的返回动作无需保留听音窗口，应覆盖完整返回和余振时间。 */
#define OBS_MUSIC_DANCE_RETURN_GUARD_MS       \
    (OBS_MUSIC_DANCE_RETURN_MS + OBS_MUSIC_CALIBRATION_SETTLE_MS)
typedef struct st_observer_keyframe
{
    int16_t  position[OBS_SERVO_COUNT]; /* 1～5号舵机在该关键帧的目标位置。 */
    uint16_t move_ms;                   /* 期望从上一帧移动到本帧所用的时间，单位ms。 */
    uint16_t hold_ms;                   /* 到达目标位置后继续保持的时间，单位ms。 */
} observer_keyframe_t;

/** 一个频带在512点FFT中的首、末频点，两个端点都包含在内。 */
typedef struct st_observer_music_band_range
{
    uint16_t first_bin;
    uint16_t last_bin;
} observer_music_band_range_t;

/*
 * 一个FFT频点代表31.25 Hz。16个近似对数频带覆盖31.25 Hz～6 kHz，
 * 与之前独立音乐测试使用的频带完全一致，避免重新改变已验证的灵敏度。
 */
static const observer_music_band_range_t
    g_music_band_ranges[MUSIC_ONSET_BAND_COUNT] =
{
    {  1U,   2U}, {  3U,   4U}, {  5U,   6U}, {  7U,   9U},
    { 10U,  13U}, { 14U,  18U}, { 19U,  24U}, { 25U,  31U},
    { 32U,  40U}, { 41U,  51U}, { 52U,  64U}, { 65U,  80U},
    { 81U, 101U}, {102U, 127U}, {128U, 159U}, {160U, 192U},
};

/*
 * 一轮观察动作采用“头部先发现、身体连续跟随”的编排方式。每次大范围
 * 环视只在起点安排一次头部先行动作；随后1号底座连续分三段转向，4号
 * 脖子在三段中逐步反向回中，5号同步改变俯仰。中间不再插入头部单独
 * 运动的停顿帧，避免形成“身体一下、头部一下”的机械节奏。
 */
static const observer_keyframe_t g_observe_sequence[] =
{
//     {{2084, 1795, 2350, 1266, 2832}, 500U, 10U}, /*  0: 上电展开*/
//
//    /* 好奇的起来  灵活的观察前面的物品  */
//    {{2060, 1612, 2610, 1195, 2988}, 700U, 80U}, /*  1: 抬起身体来 */
//    {{1954, 1653, 2621, 762, 2772}, 0U, 100U}, /*  2: 头部左侧发现东西 */
//    {{2199, 1694, 2545, 1951, 3030}, 0U, 100U}, /*  3: 头部右侧发现东西 */
//    {{3900, 1860, 2250, 1266, 2550}, 850U, 180U}, /*  4: 身体到达右后方，头部回正 */
//
//    /* 返回时只在起点重新确定一次前方视线，随后身体连续回正。 */
//    {{3900, 1860, 2250, 1580, 2380}, 700U,  60U}, /*  5: 头部先找回正前方方向 */
//    {{3300, 1940, 2450, 1480, 2500}, 850U,   0U}, /*  6: 身体开始返回，头部反向补偿 */
//    {{2700, 1900, 2720, 1370, 2420}, 800U,   0U}, /*  7: 身体继续回正，头部持续回中 */
//    {{2084, 1840, 2380, 1266, 2580}, 850U, 100U}, /*  8: 身体回到正前方 */
//
//    /* 左侧采用镜像动作：一次头部先行，身体随后连续转向左后方。 */
//    {{2084, 1840, 2380, 1580, 2380}, 700U,  60U}, /*  9: 头部先发现左侧目标 */
//    {{1500, 1880, 2720, 1480, 2500}, 850U,   0U}, /* 10: 身体开始左转，头部轻微补偿 */
//    {{ 800, 1940, 2480, 1370, 2420}, 900U,   0U}, /* 11: 身体继续转动，头部持续回中 */
//    {{ 250, 1840, 2230, 1266, 2550}, 800U, 180U}, /* 12: 身体到达左后方，头部回正 */
//
//    /* 左后方返回时同样取消中途头部停顿，让身体连续回到正前方。 */
//    {{ 250, 1840, 2230,  950, 2380}, 700U,  60U}, /* 13: 头部先找回正前方方向 */
//    {{ 800, 1940, 2450, 1050, 2500}, 800U,   0U}, /* 14: 身体开始返回，头部反向补偿 */
//    {{1500, 1900, 2700, 1160, 2420}, 900U,   0U}, /* 15: 身体继续回正，头部持续回中 */
//    {{2084, 1840, 2380, 1266, 2580}, 850U, 100U}, /* 16: 身体回到正前方 */
//
//    /* 侧耳倾听同样遵循“头先转、身体后转”，不再整体同步摆动。 */
//    {{2084, 1840, 2380, 1530, 2400},  700U,  60U}, /* 17: 头部先转向左侧声音 */
//    {{1300, 1980, 2750, 1266, 2500}, 1100U, 200U}, /* 18: 3号伸展，身体跟向左侧倾听 */
//    {{1300, 1980, 2750, 1000, 2400},  700U,  60U}, /* 19: 保持身体张力，头部转向右侧 */
//    {{2850, 1980, 2650, 1266, 2500}, 2100U, 200U}, /* 20: 3号略收，身体越过前方倾听 */
//    {{2850, 1980, 2650, 1530, 2400},  700U,  60U}, /* 21: 头部先找回正前方 */
//    {{2084, 1880, 2350, 1266, 2580}, 1050U, 100U}, /* 22: 3号收回，身体回到正前方 */
//
//    /* 最后用2、3号完成一次轻微收缩和伸展，然后回到闭环起点。 */
//    {{2084, 1740, 2180, 1280, 2900}, 1000U,  80U}, /* 23: 3号明显收紧，身体蓄力低头 */
//    {{2084, 1795, 2603, 1266, 2580},  900U, 220U}  /* 24: 挺起身体并平视远方 */

//自行设计   短暂注释时间是400ms   尽量运行时间和速度尽量别相同  这样活人感更强
//1号舵机  逆时针-->数据变小          顺时针-->数据变大     1号舵机的正前方是2067
//2号舵机   往后-->数据变小         往前-->数据变大
//3号舵机  往上-->数据变小           往下-->数据变大
//4号舵机   逆时针-->数据变小          顺时针-->数据变大
// 5号舵机 抬头-->数据变小           低头-->数据变大
      {{2074, 1641, 3025, 1252, 3209}, 800U, 0},//1上电时的位置
       {{2046, 1644, 3034, 1260, 2020}, 200, 200},//2快速把抬起头
    {{2300, 1647, 2750, 1891, 2791},  300, 600},//3.弯着腰向左大量
    {{1681, 1650, 2851, 606, 2977}, 200, 400},//4.同样弯着腰鬼鬼祟祟向右打量
      {{1600, 1814, 2297, 1789, 2900}, 800U, 400U},/*  5：右转  */
      {{2398, 1632, 2390, 630, 3000}, 800U, 400U},/*  6：左转*/
      {{2134, 1700, 2400, 332, 2649}, 800U, 100U},//7.发现右上的东西
     {{1788, 1829, 2167, 500, 2723}, 800U, 200},//8.环视右上侧的场景
    {{1800, 1700, 2200, 800, 2723}, 800U, 0},//9.四号舵机逐步逼近1884 
    {{1866, 1600, 2300, 1000, 2723}, 800U, 0},//10.
    {{2000, 1650, 2400, 1255, 2222}, 850, 0},//11.边点头边环视四周  
    {{2200, 1650, 2500, 1561, 2500}, 850, 0},//12.
     {{2400, 1650, 2500, 1884, 2222}, 850, 0},//13.
    {{2600, 1650, 2500, 1955, 2500}, 850, 0},//14.巡视完毕  
    {{2665, 1886, 2172, 1986, 2863}, 800U, 0},//15开始看中间和下面的内容  从最左边开始
     {{2438, 1727, 2467, 1304, 2589}, 800U, 0},//16
     {{2100, 1535, 2687, 1492, 2500}, 800U, 100},//17该位置由正前方步入右半侧
     {{1961, 1454, 2572, 862, 2800}, 800U, 0},//18
     {{1476, 1400, 2800, 1100, 3331}, 800U, 0},//19
     {{1310, 1537, 2700, 700, 2600}, 1000, 100},//20.
    {{1137, 1500, 2711, 900, 2866}, 800U, 100U},//21.
    {{900, 1400, 2710, 1000, 2443}, 800U, 0},//19
    {{700, 1400, 2711, 900, 2781}, 1000, 100U},//20
     {{600, 1386, 2600, 1074, 2400}, 1000, 0},//21
    {{1517, 1467, 2649, 1257, 3331}, 400, 300},//22.突然发现之前的东西吸引我了 往回转 1517
{{1478, 1465, 3020, 1259, 2994}, 800, 100},//23.开始看自己脚下
    {{1600, 1414, 3060, 997, 3042}, 1200, 0},//24.开始细致的观察
    {{1400, 1413, 2961, 1301, 3130}, 1200, 0},//25.细致的观察中2
{{1476, 1416, 2720, 1245, 3326}, 50, 400},//25.不可思议的快速远离
    {{1489, 1624, 2590, 2100, 3011}, 50, 400},//26.偷偷看看周围有没有人  看左边  
   {{1507, 1482, 2826, 558, 2638}, 25, 400},//27.偷偷看看周围有没有人  看右边 
    {{1476, 1416, 2720, 1245, 3326},100, 400},//28.会看中间
    {{1409, 1479, 3063, 1237, 3051}, 900, 100U},//28.缓缓的继续看自己刚发现的东西
    {{1517, 1415, 3034, 1299, 2993}, 800U, 200},//29.慢慢接近
    /* 34～37：观察屏幕后快速左右摇头，表达台灯对屏幕内容的反应。 */
    {{1517, 1418, 3036,  800, 3131}, 200U,   0U}, /* 34：第一次向左摇头。 */
    {{1517, 1415, 3035, 1600, 3131}, 100U,   0U}, /* 35：快速转向右侧。 */
    {{1517, 1418, 3036,  800, 3131}, 100U,   0U}, /* 36：再次转回左侧。 */
    {{1517, 1415, 3035, 1600, 3131}, 100U, 120U}, /* 37：回到右侧并让摇头动作收住。 */

    /* 38：身体保持原位，头部先回正，明确结束观察屏幕的反应。 */
    {{1517, 1420, 3035, 1252, 3160}, 480U, 220U},

    /* 39：身体从屏幕前逐步抬起，2、3号关节同时向待机中心展开。 */
    {{1780, 1510, 2750, 1235, 3070}, 800U,   0U},

    /* 40：完整落到待机中心，切换状态时不再额外蜷缩或改变姿态。 */
    {{2075, 1650, 2437, 1204, 2916}, 1000U, 350U},




};

static const observer_expression_t g_observe_expressions[] =
{
    OBSERVER_EXPRESSION_NEUTRAL,       /* 0：从初始姿态开始巡查。 */
    OBSERVER_EXPRESSION_ALERT,         /* 1：快速抬头，被周围环境吸引。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 2：弯腰查看左侧。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 3：转头查看右侧。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 4：身体继续向右巡视。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 5：回头查看左侧。 */
    OBSERVER_EXPRESSION_ALERT,         /* 6：发现右上方目标。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 7：继续观察右上方。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 8：头部逐步跟随视线。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 9：保持右侧观察。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 10：边点头边环视。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 11：视线继续向右移动。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 12：巡视右侧远处。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 13：右侧巡视结束。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 14：开始查看中间和下方。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 15：视线逐渐回到前方。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 16：进入另一侧观察区域。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 17：观察右侧近处。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 18：低头确认下方环境。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 19：身体继续向左巡视。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 20：保持低位观察。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 21：观察左侧环境。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 22：继续转向左侧。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 23：到达左侧巡视终点。 */
    OBSERVER_EXPRESSION_ALERT,         /* 24：突然重新发现感兴趣的目标。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 25：开始查看真实屏幕。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 26：靠近屏幕仔细观察。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 27：继续分析屏幕内容。 */
    OBSERVER_EXPRESSION_ALERT,         /* 28：对屏幕内容感到意外并后退。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 29：偷偷查看左侧是否有人。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 30：快速查看右侧是否有人。 */
    OBSERVER_EXPRESSION_ALERT,         /* 31：回到中间再次确认环境。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 32：重新靠近屏幕。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 33：继续观察屏幕内容。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 34：第一次向左摇头。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 35：快速向右摇头。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 36：再次向左摇头。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 37：回到右侧并收住动作。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 38：头部回正，结束屏幕反应。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 39：身体逐步展开并接近待机中心。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 40：自然落到待机中心姿态。 */
};

/*
 * 巡查结束后执行的正式待机动作已经通过实机测试。2、3号身体关节保持
 * 稳定，4、5号负责主要头部细节；1号只完成明确转向，不产生前倾突进。
 */
static const observer_keyframe_t g_standby_sequence[] =
{
    /* 0：在中心安静待机，体现巡查结束后的放松状态。 */
    {{2075, 1650, 2437, 1204, 2916}, 700U, 2500U},

    /* 1：4号脖子明显看向左侧，5号头部同时轻微抬起。 */
    {{2075, 1650, 2437,  870, 2750}, 500U,  400U},

    /* 2：1号底座明确左转，4、5号补偿身体转向并继续注视左侧。 */
    {{1650, 1650, 2437, 1030, 2820}, 900U, 1200U},

    /* 3：保持朝左，4、5号做一次幅度更明确的低头查看。 */
    {{1650, 1650, 2437,  900, 3100}, 500U,  900U},

    /* 4：头部先回到身体正前方。 */
    {{1650, 1650, 2437, 1204, 2916}, 550U,  300U},

    /* 5：1号底座回到中心，并安静停留。 */
    {{2075, 1650, 2437, 1204, 2916}, 900U, 1900U},

    /* 6：4号脖子明显看向右侧，5号头部同时轻微抬起。 */
    {{2075, 1650, 2437, 1540, 2740}, 520U,  400U},

    /* 7：1号底座明确右转，4、5号补偿身体转向并继续注视右侧。 */
    {{2500, 1650, 2437, 1380, 2820}, 900U, 1200U},

    /* 8：保持朝右，4、5号做一次幅度更明确的低头查看。 */
    {{2500, 1650, 2437, 1520, 3110}, 520U,  900U},

    /* 9：头部先回到身体正前方。 */
    {{2500, 1650, 2437, 1204, 2916}, 560U,  300U},

    /* 10：1号底座回到中心，再次进入安静待机。 */
    {{2075, 1650, 2437, 1204, 2916}, 900U, 1900U},

    /* 11：5号头部明显低下，开始查看真实屏幕。 */
    {{2075, 1650, 2437, 1204, 3200}, 550U,  500U},

    /* 12：保持低头，4号以更清晰的幅度看向屏幕左侧。 */
    {{2075, 1650, 2437, 1030, 3260}, 500U, 1300U},

    /* 13：4号平缓转向屏幕右侧，5号保持专注低头。 */
    {{2075, 1650, 2437, 1380, 3210}, 550U, 1200U},

    /* 14：回到屏幕正中，并以更明显的低头姿态短暂停留。 */
    {{2075, 1650, 2437, 1204, 3250}, 480U, 1000U},

    /* 15：连续抬头到接近中心；过渡帧不驻留。 */
    {{2075, 1650, 2437, 1204, 3050}, 500U,    0U},

    /* 16：完全抬头回到中心，安静等待睡眠或下一轮测试。 */
    {{2075, 1650, 2437, 1204, 2916}, 520U, 3700U},
};

static const observer_expression_t g_standby_expressions[] =
{
    OBSERVER_EXPRESSION_NEUTRAL,       /* 0：待机中心。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 1：脖子先看左侧。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 2：底座跟随左转。 */
    OBSERVER_EXPRESSION_CURIOUS_LEFT,  /* 3：左侧低头查看。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 4：头部恢复平视。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 5：底座回到中心。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 6：脖子先看右侧。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 7：底座跟随右转。 */
    OBSERVER_EXPRESSION_CURIOUS_RIGHT, /* 8：右侧低头查看。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 9：头部恢复平视。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 10：底座回到中心。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 11：开始查看屏幕。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 12：查看屏幕左侧。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 13：查看屏幕右侧。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 14：回到屏幕正中。 */
    OBSERVER_EXPRESSION_FOCUSED,       /* 15：从屏幕前抬头。 */
    OBSERVER_EXPRESSION_NEUTRAL,       /* 16：回到中心。 */
};

/* 平缓移动到用户实机记录的睡眠姿态。 */
static const observer_keyframe_t g_sleep_sequence[] =
{
    {{2007, 1262, 3043, 1263, 3115}, 1500U, 500U}
};

/*
 * 被声音惊醒时，先从睡眠姿态稍微展开，再让2、3号舵机同步折叠收紧。
 * 4、5号头部使用更明显的左右摆动和抬头动作表达惊讶、警惕和确认环境，
 * 随后身体按照“头先转、身体跟随”的方式观察两侧并恢复正常观察姿态。
 */
static const observer_keyframe_t g_wake_sequence[] =
{
 {{2043, 1282, 3218, 1168, 2334}, 800U, 10U}, /*  0: 卧起的状态*/

/* 好奇的起来  灵活的观察前面的物品  */
{{2060, 1612, 2610, 1195, 2988}, 700U, 80U}, /*  1: 抬起身体来 */
{{1954, 1653, 2621, 762, 2772}, 500U,400U}, /*  2: 头部左侧发现东西 */
{{2199, 1694, 2545, 1951, 3030}, 500U, 400U}, /*  3: 头部右侧发现东西 */




};

#define OBS_ARRAY_COUNT(array) ((uint8_t) (sizeof(array) / sizeof((array)[0])))

static const uint8_t  g_servo_ids[OBS_SERVO_COUNT] = {1U, 2U, 3U, 4U, 5U};
/*
 * move_ms为0U时会直接使用这里的速度。2、3号不再额外降速；4、5号负责
 * 头部动作，允许使用更高速度，以便快速完成“发现目标”一类动作。
 */
static const uint16_t g_servo_speed_limits[OBS_SERVO_COUNT] = {1000U, 1000U, 1000U, 1600U, 1400U};
/*
 * 4、5号位移通常小于底座；设置适度的最低速度，避免小位移因速度过低
 * 出现明显迟滞。1～3号仍完全按照关键帧时间计算，不额外提高最低速度。
 */
static const uint16_t g_servo_speed_minimums[OBS_SERVO_COUNT] = {1U, 1U, 1U, 160U, 160U};
/*
 * 2、3号不再使用较缓的专用加速度；4、5号头部使用更高加速度，缩短
 * 快速观察动作的起步和制动时间。
 */
static const uint8_t  g_servo_accelerations[OBS_SERVO_COUNT] = {18U, 22U, 22U, 32U, 28U};
static const int16_t  g_power_on_position[OBS_SERVO_COUNT] = {2084, 1795, 2603, 1266, 2832};

static bool g_active;
static observer_behavior_t g_behavior = OBSERVER_BEHAVIOR_OBSERVING;
static uint8_t g_sequence_index;
static int16_t g_last_command[OBS_SERVO_COUNT];
static bool g_last_command_valid;
static uint32_t g_last_update_ms;
static uint32_t g_keyframe_start_ms;
static uint16_t g_keyframe_motion_ms;
static bool g_keyframe_command_sent;

/* 点头会暂时打断待机动作；回应结束后重新从待机中心开始。 */
static observer_keyframe_t g_nod_sequence[OBS_NOD_SEQUENCE_MAX_FRAMES];
static uint8_t g_nod_sequence_length;
static uint32_t g_nod_cooldown_until_ms;
static observer_visual_target_t g_pending_visual_target;

/* 跟踪方向动作（与预览测试 camera_track_action_t 对应）。 */
typedef enum e_observer_track_action
{
    OBS_TRACK_ACTION_UNKNOWN = 0,
    OBS_TRACK_ACTION_STOP,
    OBS_TRACK_ACTION_NEGATIVE,
    OBS_TRACK_ACTION_POSITIVE,
} observer_track_action_t;

static int16_t g_tracking_servo1_position;     /* 最近下发的 1 号目标位置 */
static int16_t g_tracking_servo2_position;     /* 最近下发的 2 号目标位置 */
static int16_t g_tracking_servo5_position;     /* 最近下发的 5 号目标位置 */
static observer_track_action_t g_tracking_servo1_action;
static observer_track_action_t g_tracking_servo2_action;
static observer_track_action_t g_tracking_servo5_action;
static bool g_tracking_servo1_read_error;      /* 实时读取失败只报一次 */
static bool g_tracking_servo2_read_error;
static bool g_tracking_servo5_read_error;
static uint8_t g_tracking_miss_count;          /* 丢失帧数，仅用于串口诊断 */
static bool g_tracking_face_lost;
static uint32_t g_tracking_face_lost_since_ms;
static bool g_tracking_frame_pending;          /* 有未消费的跟踪帧 */
static bool g_tracking_frame_valid;
static int16_t g_tracking_frame_x;
static int16_t g_tracking_frame_y;

static observer_visual_target_t g_visual_candidate;
static uint8_t g_visual_confirm_count;
static uint8_t g_visual_miss_count;

/* The target project exposes microphone frames as signed PCM16 samples. */
static voice_sample_t g_audio_frame[OBS_AUDIO_FRAME_CAPACITY];
static uint64_t g_audio_window_abs_sum;
static uint32_t g_audio_window_sample_count;
static uint32_t g_audio_window_frame_count;
static uint32_t g_audio_baseline;
static uint32_t g_audio_calibration_count;
static uint8_t g_audio_loud_window_count;
/* 舵机起动后的短时间内不学习底噪，也不把电机声识别为音乐起音。 */
static uint32_t g_audio_motor_noise_ignore_until_ms;
/* 校准期间屏蔽完整运动时间，避免安装在灯体上的麦克风学习结构振动。 */
static uint32_t g_music_calibration_motor_ignore_until_ms;
static uint32_t g_music_calibration_deadline_ms;
/* 超时降级后，本次桌宠会话不再启动听觉，重新进入模式时清零。 */
static bool g_hearing_disabled_for_session;

static arm_rfft_fast_instance_f32 g_music_fft_instance;
static float32_t g_music_fft_input[OBS_MUSIC_FFT_SIZE];
static float32_t g_music_fft_output[OBS_MUSIC_FFT_SIZE];
static float32_t g_music_fft_window[OBS_MUSIC_FFT_SIZE];
static float32_t g_music_magnitude[OBS_MUSIC_FFT_HALF_SIZE];
static float32_t g_music_noise_mean[MUSIC_ONSET_BAND_COUNT];
static float32_t g_music_noise_square_difference_sum[MUSIC_ONSET_BAND_COUNT];
static float32_t g_music_noise_gate[MUSIC_ONSET_BAND_COUNT];
static float32_t g_music_total_noise_mean;
static float32_t g_music_total_noise_square_difference_sum;
static float32_t g_music_total_noise_gate;
static music_onset_detector_t g_music_onset_detector;
static music_beat_tracker_t g_music_beat_tracker;
static uint32_t g_music_fft_sample_count;
static uint32_t g_music_calibration_count;
static uint32_t g_music_audio_timestamp_ms;
/* 使用音频时间轴标记舞蹈噪声，避免FIFO积压让旧机械声逃出保护窗口。 */
static uint32_t g_music_motor_noise_ignore_until_audio_ms;
static uint32_t g_music_last_dropped_frame_count;
/* 音乐专项调试日志状态，对齐MUSIC_TEST_ENABLE的串口输出。 */
static uint32_t g_music_onset_count;
static uint32_t g_music_beat_count;
static uint32_t g_music_last_reported_bpm;
/* 半倍/双倍过滤后，当前真正用于节拍预测和舞蹈的稳定BPM。 */
static uint16_t g_music_active_bpm;
static uint8_t g_music_dance_intensity;
static uint8_t g_music_dance_intro_nod_count;
static bool g_music_fft_ready;
static bool g_music_analysis_active;
static bool g_music_mode_enabled;
static uint32_t g_music_mode_listen_after_ms;
static bool g_music_previous_prediction_active;
static bool g_music_warmup_message_printed;
static bool g_music_tempo_lock_reported;

static void observer_copy_position(int16_t destination[OBS_SERVO_COUNT],
                                   int16_t const source[OBS_SERVO_COUNT])
{
    for (uint8_t index = 0U; index < OBS_SERVO_COUNT; index++)
    {
        destination[index] = source[index];
    }
}

/**
 * @brief 向舵机下发一张完整目标姿态。
 *
 * 每张关键帧只发送一次。速度由位移量和期望运动时间计算，随后交给舵机
 * 内部运动规划器连续执行，不再依赖CPU每20 ms发送一个插值位置。
 */
static bool observer_send_command(int16_t command[OBS_SERVO_COUNT],
                                  uint16_t requested_move_ms,
                                  uint16_t * p_actual_move_ms)
{
    bool changed = !g_last_command_valid;
    bool const fastest_move = (0U == requested_move_ms);
    uint16_t speeds[OBS_SERVO_COUNT];
    uint32_t actual_move_ms = 0U;

    for (uint8_t index = 0U; (index < OBS_SERVO_COUNT) && !changed; index++)
    {
        changed = (command[index] != g_last_command[index]);
    }

    if (!changed)
    {
        *p_actual_move_ms = 0U;
        return true;
    }

    /*
     * 0U表示按各舵机的速度上限尽快运动。非零值仍表示期望运动时间；
     * 小于一个调度周期的非零值统一按一个周期处理，避免时间换算失真。
     */
    if (!fastest_move && (requested_move_ms < OBS_MIN_COMMAND_MOVE_MS))
    {
        requested_move_ms = OBS_MIN_COMMAND_MOVE_MS;
    }

    for (uint8_t index = 0U; index < OBS_SERVO_COUNT; index++)
    {
        int32_t distance = (int32_t) command[index] - g_last_command[index];
        uint32_t required_speed;
        uint32_t axis_move_ms;

        if (distance < 0)
        {
            distance = -distance;
        }

        if (0 == distance)
        {
            /* 该轴位置没有变化，速度字段取最小非零值。 */
            speeds[index] = 1U;
            continue;
        }

        if (fastest_move)
        {
            required_speed = g_servo_speed_limits[index];
        }
        else
        {
            required_speed = (((uint32_t) distance * 1000U) + requested_move_ms - 1U) /
                             requested_move_ms;
        }
        /* 速度至少为1，避免协议中的0被舵机解释为不限速。 */
        if (0U == required_speed)
        {
            required_speed = 1U;
        }
        if (required_speed < g_servo_speed_minimums[index])
        {
            required_speed = g_servo_speed_minimums[index];
        }
        if (required_speed > g_servo_speed_limits[index])
        {
            required_speed = g_servo_speed_limits[index];
        }
        speeds[index] = (uint16_t) required_speed;

        axis_move_ms = (((uint32_t) distance * 1000U) + required_speed - 1U) /
                       required_speed;
        if (axis_move_ms > actual_move_ms)
        {
            actual_move_ms = axis_move_ms;
        }
    }

    if (!Servo_SyncWritePos(g_servo_ids,
                            OBS_SERVO_COUNT,
                            command,
                            speeds,
                            g_servo_accelerations))
    {
        return false;
    }

    observer_copy_position(g_last_command, command);
    g_last_command_valid = true;
    actual_move_ms += OBS_MOTION_SETTLE_MARGIN_MS;
    if (actual_move_ms > UINT16_MAX)
    {
        actual_move_ms = UINT16_MAX;
    }
    *p_actual_move_ms = (uint16_t) actual_move_ms;

    if (actual_move_ms > 0U)
    {
        uint32_t const now_ms = drv_gpt_timer_get_ms();
        uint32_t const guard_ms =
            (actual_move_ms < OBS_MUSIC_MOTOR_NOISE_GUARD_MAX_MS) ?
            actual_move_ms : OBS_MUSIC_MOTOR_NOISE_GUARD_MAX_MS;

        /* 正常识别只短暂避开电机起动声，避免漏掉后续音乐节拍。 */
        g_audio_motor_noise_ignore_until_ms = now_ms + guard_ms;
        g_music_motor_noise_ignore_until_audio_ms =
            g_music_audio_timestamp_ms + guard_ms;

        /* 底噪校准还要等待机身残余振动衰减。 */
        g_music_calibration_motor_ignore_until_ms =
            now_ms + actual_move_ms + OBS_MUSIC_CALIBRATION_SETTLE_MS;
    }
    return true;
}

/**
 * @brief 更新一张关键帧序列。
 *
 * @return true 表示整张序列刚刚执行完成，false表示仍在执行。
 */
static bool observer_sequence_update(observer_keyframe_t const * p_sequence,
                                     uint8_t sequence_length)
{
    observer_keyframe_t const * p_keyframe = &p_sequence[g_sequence_index];
    uint32_t const now_ms = drv_gpt_timer_get_ms();
    uint32_t frame_wait_ms;

    if (!g_keyframe_command_sent)
    {
        int16_t command[OBS_SERVO_COUNT];

        observer_copy_position(command, p_keyframe->position);
        if (!observer_send_command(command,
                                   p_keyframe->move_ms,
                                   &g_keyframe_motion_ms))
        {
            return false;
        }

        g_keyframe_start_ms = now_ms;
        g_keyframe_command_sent = true;
        return false;
    }

    frame_wait_ms = (uint32_t) g_keyframe_motion_ms + p_keyframe->hold_ms;

    /*
     * hold_ms为0表示该姿态是连续动作的过渡点。提前结束当前帧的等待，
     * 下一次20 ms调度会在舵机完全减速前下发后续目标，减少逐帧顿挫。
     */
    if ((0U == p_keyframe->hold_ms) &&
        (g_keyframe_motion_ms > OBS_CONTINUOUS_BLEND_LEAD_MS))
    {
        frame_wait_ms = (uint32_t) g_keyframe_motion_ms - OBS_CONTINUOUS_BLEND_LEAD_MS;
    }

    if ((now_ms - g_keyframe_start_ms) >= frame_wait_ms)
    {
        g_sequence_index++;
        g_keyframe_command_sent = false;

        if (g_sequence_index >= sequence_length)
        {
            g_sequence_index = 0U;
            return true;
        }
    }

    return false;
}

static void observer_sequence_begin(observer_behavior_t behavior)
{
    g_behavior = behavior;
    g_sequence_index = 0U;
    g_keyframe_command_sent = false;
    g_keyframe_motion_ms = 0U;
}

static void observer_visual_confirmation_reset(void)
{
    g_visual_candidate = OBSERVER_VISUAL_TARGET_NONE;
    g_visual_confirm_count = 0U;
    g_visual_miss_count = 0U;
}

/** 重新进入待机，并从用户指定的待机中心姿态开始约30秒的小动作。 */
static void observer_standby_begin(void)
{
    g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
    observer_visual_confirmation_reset();
    observer_sequence_begin(OBSERVER_BEHAVIOR_STANDBY);
}

/** 环境底噪校准完成后，按开关进入首次巡查或直接开放桌宠待机功能。 */
static void observer_ready_behavior_begin(void)
{
#if OBS_INITIAL_PATROL_ENABLE
    observer_sequence_begin(OBSERVER_BEHAVIOR_OBSERVING);
#else
    observer_standby_begin();
#endif
}

/** 进入音乐按钮的静止监听状态，不再下发待机或视觉动作。 */
static void observer_music_listening_begin(void)
{
    g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
    observer_visual_confirmation_reset();
    observer_sequence_begin(OBSERVER_BEHAVIOR_MUSIC_LISTENING);
}

static int observer_tracking_abs(int value)
{
    return (value < 0) ? -value : value;
}

static int16_t observer_tracking_limit_position(int position, int minimum, int maximum)
{
    if (position < minimum)
    {
        return (int16_t) minimum;
    }
    if (position > maximum)
    {
        return (int16_t) maximum;
    }
    return (int16_t) position;
}

static observer_track_action_t observer_tracking_axis_action(int target_center,
                                                             int image_center)
{
    if (target_center < (image_center - OBS_TRACK_DEAD_ZONE_PIXELS))
    {
        return OBS_TRACK_ACTION_NEGATIVE;
    }
    if (target_center > (image_center + OBS_TRACK_DEAD_ZONE_PIXELS))
    {
        return OBS_TRACK_ACTION_POSITIVE;
    }
    return OBS_TRACK_ACTION_STOP;
}

/* 单次读取舵机实时位置；失败返回 false 并只报一次错。不阻塞状态机。 */
static bool observer_tracking_read_live_position(uint8_t servo_id,
                                                 int * position,
                                                 bool * error_reported)
{
    int const current = Servo_ReadPosition(servo_id);

    if ((current < SERVO_STS_POSITION_MIN) || (current > SERVO_STS_POSITION_MAX))
    {
        if (!*error_reported)
        {
            printf("Observer: SERVO%u live position read failed\r\n",
                   (unsigned int) servo_id);
            *error_reported = true;
        }
        return false;
    }
    *error_reported = false;
    *position = current;
    return true;
}

/* 开始跟踪：读取 1、2、5 号实时位置作为起点，切换到 TRACKING。 */
static void observer_tracking_begin(void)
{
    int live1;
    int live2;
    int live5;
    bool const s1_ok = observer_tracking_read_live_position(1U,
                                                            &live1,
                                                            &g_tracking_servo1_read_error);
    bool const s2_ok = observer_tracking_read_live_position(2U,
                                                            &live2,
                                                            &g_tracking_servo2_read_error);
    bool const s5_ok = observer_tracking_read_live_position(5U,
                                                            &live5,
                                                            &g_tracking_servo5_read_error);

    /* 读取失败时用上次命令位置回退，保证跟踪起点与真实位置不脱节。 */
    g_tracking_servo1_position = s1_ok ? (int16_t) live1 : g_last_command[0];
    g_tracking_servo2_position = s2_ok ? (int16_t) live2 : g_last_command[1];
    g_tracking_servo5_position = s5_ok ? (int16_t) live5 : g_last_command[4];
    g_tracking_servo1_action   = OBS_TRACK_ACTION_UNKNOWN;
    g_tracking_servo2_action   = OBS_TRACK_ACTION_UNKNOWN;
    g_tracking_servo5_action   = OBS_TRACK_ACTION_UNKNOWN;
    g_tracking_miss_count      = 0U;
    g_tracking_face_lost       = false;
    g_tracking_face_lost_since_ms = 0U;
    g_tracking_frame_pending   = false;
    g_tracking_frame_valid     = false;
    g_tracking_frame_x         = 0;
    g_tracking_frame_y         = 0;
    g_pending_visual_target    = OBSERVER_VISUAL_TARGET_NONE;
    observer_visual_confirmation_reset();
    g_keyframe_command_sent    = false;  /* 切换状态后不再继续待机关键帧 */
    g_behavior = OBSERVER_BEHAVIOR_TRACKING;
    printf("Observer: face confirmed; tracking started. S1=%d S2=%d S5=%d\r\n",
           (int) g_tracking_servo1_position,
           (int) g_tracking_servo2_position,
           (int) g_tracking_servo5_position);
}

/* 跟踪期间每收到一帧人脸坐标，计算并下发 1、2、5 号舵机目标。 */
static void observer_tracking_servo_update(int16_t box_center_x, int16_t box_center_y)
{
    uint8_t servo_ids[3];
    int16_t servo_positions[3];
    uint16_t servo_speeds[3];
    uint8_t servo_accelerations[3];
    uint8_t command_count = 0U;
    observer_track_action_t const servo1_action =
        observer_tracking_axis_action(box_center_x, (int) (OBS_TRACK_IMAGE_W / 2U));
    observer_track_action_t const servo2_action =
        observer_tracking_axis_action(box_center_y, (int) (OBS_TRACK_IMAGE_H / 2U));

    /* ---- 1 号舵机（X / 底座） ---- */
    if (OBS_TRACK_ACTION_STOP == servo1_action)
    {
        if (OBS_TRACK_ACTION_STOP != g_tracking_servo1_action)
        {
            int cur;
            if (observer_tracking_read_live_position(1U,
                                                      &cur,
                                                      &g_tracking_servo1_read_error) &&
                (cur != g_tracking_servo1_position))
            {
                servo_ids[command_count] = 1U;
                servo_positions[command_count] = (int16_t) cur;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    else
    {
        int cur;
        if (observer_tracking_read_live_position(1U, &cur, &g_tracking_servo1_read_error))
        {
            int const pixel_error = box_center_x - (int) (OBS_TRACK_IMAGE_W / 2U);
            int16_t const next = observer_tracking_limit_position(
                cur + pixel_error * OBS_TRACK_SERVO1_COUNTS_PER_PIXEL,
                OBS_TRACK_SERVO1_MIN, OBS_TRACK_SERVO1_MAX);
            if (observer_tracking_abs((int) next - (int) g_tracking_servo1_position)
                >= OBS_TRACK_COMMAND_CHANGE_MIN)
            {
                servo_ids[command_count] = 1U;
                servo_positions[command_count] = next;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    g_tracking_servo1_action = servo1_action;

    /* ---- 2 号舵机（Y / 俯仰） ---- */
    if (OBS_TRACK_ACTION_STOP == servo2_action)
    {
        if (OBS_TRACK_ACTION_STOP != g_tracking_servo2_action)
        {
            int cur;
            if (observer_tracking_read_live_position(2U,
                                                      &cur,
                                                      &g_tracking_servo2_read_error) &&
                (cur != g_tracking_servo2_position))
            {
                servo_ids[command_count] = 2U;
                servo_positions[command_count] = (int16_t) cur;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    else
    {
        int cur;
        if (observer_tracking_read_live_position(2U, &cur, &g_tracking_servo2_read_error))
        {
            int const pixel_error = box_center_y - (int) (OBS_TRACK_IMAGE_H / 2U);
            int16_t const next = observer_tracking_limit_position(
                cur + pixel_error * OBS_TRACK_SERVO2_COUNTS_PER_PIXEL,
                OBS_TRACK_SERVO2_MIN, OBS_TRACK_SERVO2_MAX);
            if (observer_tracking_abs((int) next - (int) g_tracking_servo2_position)
                >= OBS_TRACK_COMMAND_CHANGE_MIN)
            {
                servo_ids[command_count] = 2U;
                servo_positions[command_count] = next;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    g_tracking_servo2_action = servo2_action;

    /* ---- 5 号舵机（Y / 头部俯仰） ---- */
    if (OBS_TRACK_ACTION_STOP == servo2_action)
    {
        if (OBS_TRACK_ACTION_STOP != g_tracking_servo5_action)
        {
            int cur;
            if (observer_tracking_read_live_position(5U,
                                                      &cur,
                                                      &g_tracking_servo5_read_error) &&
                (cur != g_tracking_servo5_position))
            {
                /* 人脸进入垂直死区时，让头部停在当前实际位置。 */
                servo_ids[command_count] = 5U;
                servo_positions[command_count] = (int16_t) cur;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    else
    {
        int cur;
        if (observer_tracking_read_live_position(5U, &cur, &g_tracking_servo5_read_error))
        {
            int const pixel_error = box_center_y - (int) (OBS_TRACK_IMAGE_H / 2U);
            int16_t const next = observer_tracking_limit_position(
                cur + pixel_error * OBS_TRACK_SERVO5_COUNTS_PER_PIXEL,
                OBS_TRACK_SERVO5_MIN, OBS_TRACK_SERVO5_MAX);
            if (observer_tracking_abs((int) next - (int) g_tracking_servo5_position)
                >= OBS_TRACK_COMMAND_CHANGE_MIN)
            {
                servo_ids[command_count] = 5U;
                servo_positions[command_count] = next;
                servo_speeds[command_count] = OBS_TRACK_SERVO_SPEED;
                servo_accelerations[command_count] = OBS_TRACK_SERVO_ACC;
                command_count++;
            }
        }
    }
    /* 5 号舵机与 2 号舵机共用人脸的垂直方向判断。 */
    g_tracking_servo5_action = servo2_action;

    if (0U == command_count)
    {
        return;
    }

    if (!Servo_SyncWritePos(servo_ids,
                            command_count,
                            servo_positions,
                            servo_speeds,
                            servo_accelerations))
    {
        printf("Observer: tracking servo write failed\r\n");
        return;
    }

    for (uint8_t i = 0U; i < command_count; i++)
    {
        if (1U == servo_ids[i])
        {
            g_tracking_servo1_position = servo_positions[i];
        }
        else if (2U == servo_ids[i])
        {
            g_tracking_servo2_position = servo_positions[i];
        }
        else
        {
            g_tracking_servo5_position = servo_positions[i];
        }
    }
}

/** 离开人脸跟踪前保存1、2、5号舵机实时位置，避免下一个动作跳变。 */
static void observer_tracking_position_sync(void)
{
    int live1;
    int live2;
    int live5;

    if (observer_tracking_read_live_position(1U, &live1, &g_tracking_servo1_read_error))
    {
        g_last_command[0] = (int16_t) live1;
    }
    else
    {
        g_last_command[0] = g_tracking_servo1_position;  /* 回退到最近下发位置 */
    }
    if (observer_tracking_read_live_position(2U, &live2, &g_tracking_servo2_read_error))
    {
        g_last_command[1] = (int16_t) live2;
    }
    else
    {
        g_last_command[1] = g_tracking_servo2_position;
    }
    if (observer_tracking_read_live_position(5U, &live5, &g_tracking_servo5_read_error))
    {
        g_last_command[4] = (int16_t) live5;
    }
    else
    {
        g_last_command[4] = g_tracking_servo5_position;
    }
    /* 3、4 号在跟踪期间未动，g_last_command[2..3] 保持有效。 */
    g_last_command_valid = true;
}

/* 丢失目标：读实时位置回写 g_last_command，平滑回到待机。 */
static void observer_tracking_stop(void)
{
    observer_tracking_position_sync();

    printf("Observer: face lost for %lu ms (%u frames); tracking stopped. S1=%d S2=%d S5=%d\r\n",
           (unsigned long) OBS_TRACK_FACE_LOST_TIMEOUT_MS,
           (unsigned int) g_tracking_miss_count,
           (int) g_last_command[0],
           (int) g_last_command[1],
           (int) g_last_command[4]);
    observer_standby_begin();
}

static void observer_nod_begin(observer_visual_target_t target)
{
    for (uint8_t index = 0U; index < OBS_NOD_SEQUENCE_MAX_FRAMES; index++)
    {
        observer_copy_position(g_nod_sequence[index].position, g_last_command);
    }

    if (OBSERVER_VISUAL_TARGET_FACE == target)
    {
        /*
         * 识别人脸后，2、3号先展开形成“挺胸”姿态；5号在保持身体姿态时
         * 完成一次幅度较明显的低头和抬头，最后恢复到被打断的观察动作。
         */
        for (uint8_t index = 0U; index < 3U; index++)
        {
            g_nod_sequence[index].position[1] = OBS_FACE_CHEST_SERVO2_POSITION;
            g_nod_sequence[index].position[2] = OBS_FACE_CHEST_SERVO3_POSITION;
        }

        g_nod_sequence[0].position[4] = OBS_FACE_NOD_UP_POSITION;
        g_nod_sequence[0].move_ms = 900U;
        g_nod_sequence[0].hold_ms = 120U;

        g_nod_sequence[1].position[4] = OBS_FACE_NOD_DOWN_POSITION;
        g_nod_sequence[1].move_ms = 700U;
        g_nod_sequence[1].hold_ms = 100U;

        g_nod_sequence[2].position[4] = OBS_FACE_NOD_UP_POSITION;
        g_nod_sequence[2].move_ms = 700U;
        g_nod_sequence[2].hold_ms = 180U;

        g_nod_sequence[3].move_ms = 900U;
        g_nod_sequence[3].hold_ms = 120U;
        g_nod_sequence_length = 4U;
    }
    else
    {
        int nod_position;

        /* 识别书本时只做一次幅度较轻的点头，不改变2、3号身体姿态。 */
        if (((int) g_last_command[4] + OBS_BOOK_NOD_OFFSET_COUNTS) <= OBS_NOD_SERVO5_MAX)
        {
            nod_position = (int) g_last_command[4] + OBS_BOOK_NOD_OFFSET_COUNTS;
        }
        else
        {
            nod_position = (int) g_last_command[4] - OBS_BOOK_NOD_OFFSET_COUNTS;
            if (nod_position < OBS_NOD_SERVO5_MIN)
            {
                nod_position = OBS_NOD_SERVO5_MIN;
            }
        }

        g_nod_sequence[0].position[4] = (int16_t) nod_position;
        g_nod_sequence[0].move_ms = 700U;
        g_nod_sequence[0].hold_ms = 140U;
        g_nod_sequence[1].move_ms = 700U;
        g_nod_sequence[1].hold_ms = 160U;
        g_nod_sequence_length = 2U;
    }

    observer_sequence_begin(OBSERVER_BEHAVIOR_NODDING);
    printf("Observer: %s confirmed; response started.\r\n",
           (OBSERVER_VISUAL_TARGET_FACE == target) ? "face" : "book");
}

static void observer_wake_begin(void)
{
    observer_sequence_begin(OBSERVER_BEHAVIOR_WAKING);
    observer_visual_confirmation_reset();
    printf("Observer: noticeable sound; waking up.\r\n");
}

static bool observer_time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return ((int32_t) (now_ms - target_ms) >= 0);
}

static float32_t observer_music_band_average(uint32_t first_bin,
                                             uint32_t last_bin)
{
    float32_t sum = 0.0f;

    for (uint32_t bin = first_bin; bin <= last_bin; bin++)
    {
        sum += g_music_magnitude[bin];
    }

    return sum / (float32_t) ((last_bin - first_bin) + 1U);
}

static float32_t observer_music_noise_gate_calculate(
    float32_t mean,
    float32_t square_difference_sum,
    uint32_t sample_count)
{
    float32_t standard_deviation = 0.0f;
    float32_t const relative_gate = mean * OBS_MUSIC_NOISE_MEAN_MULTIPLIER;
    float32_t deviation_gate;

    if (sample_count > 1U)
    {
        float32_t const variance =
            square_difference_sum / (float32_t) (sample_count - 1U);

        if (ARM_MATH_SUCCESS != arm_sqrt_f32(variance, &standard_deviation))
        {
            standard_deviation = 0.0f;
        }
    }

    deviation_gate = mean +
                     (standard_deviation * OBS_MUSIC_NOISE_DEVIATION_MULTIPLIER);

    return (deviation_gate > relative_gate) ? deviation_gate : relative_gate;
}

/** 把一组512点PCM样本转换成16个频带的平均幅值。 */
static void observer_music_fft_analyze(
    float32_t bands[MUSIC_ONSET_BAND_COUNT])
{
    float32_t mean = 0.0f;

    /* 先移除麦克风直流偏置，再乘Hann窗减少频谱泄漏。 */
    for (uint32_t index = 0U; index < OBS_MUSIC_FFT_SIZE; index++)
    {
        mean += g_music_fft_input[index];
    }
    mean /= (float32_t) OBS_MUSIC_FFT_SIZE;

    for (uint32_t index = 0U; index < OBS_MUSIC_FFT_SIZE; index++)
    {
        g_music_fft_input[index] =
            (g_music_fft_input[index] - mean) * g_music_fft_window[index];
    }

    arm_rfft_fast_f32(&g_music_fft_instance,
                      g_music_fft_input,
                      g_music_fft_output,
                      0U);

    g_music_magnitude[0] = 0.0f;
    arm_cmplx_mag_f32(&g_music_fft_output[2],
                      &g_music_magnitude[1],
                      OBS_MUSIC_FFT_HALF_SIZE - 1U);

    for (uint32_t bin = 1U; bin < OBS_MUSIC_FFT_HALF_SIZE; bin++)
    {
        g_music_magnitude[bin] *= OBS_MUSIC_MAGNITUDE_SCALE;
    }

    for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
    {
        bands[band] = observer_music_band_average(
            g_music_band_ranges[band].first_bin,
            g_music_band_ranges[band].last_bin);
    }
}

/** 清除节拍历史，但保留已经测得的环境噪声门限。 */
static void observer_music_tracking_reset(void)
{
    g_music_fft_sample_count = 0U;
    g_music_previous_prediction_active = false;
    g_music_last_reported_bpm = 0U;
    g_music_active_bpm = 0U;
    g_music_warmup_message_printed = false;
    g_music_tempo_lock_reported = false;
    music_onset_reset(&g_music_onset_detector);
    music_beat_tracker_reset(&g_music_beat_tracker);
}

/** 每次进入观察者模式时重新测量一次环境噪声。 */
static void observer_music_state_reset(void)
{
    memset(g_music_noise_mean, 0, sizeof(g_music_noise_mean));
    memset(g_music_noise_square_difference_sum,
           0,
           sizeof(g_music_noise_square_difference_sum));
    memset(g_music_noise_gate, 0, sizeof(g_music_noise_gate));

    g_music_total_noise_mean = 0.0f;
    g_music_total_noise_square_difference_sum = 0.0f;
    g_music_total_noise_gate = 0.0f;
    g_music_fft_sample_count = 0U;
    g_music_calibration_count = 0U;
    g_music_audio_timestamp_ms = 0U;
    g_music_motor_noise_ignore_until_audio_ms = 0U;
    g_music_last_dropped_frame_count = Voice_DroppedFrameCountGet();
    g_music_onset_count = 0U;
    g_music_beat_count = 0U;
    g_music_last_reported_bpm = 0U;
    g_music_active_bpm = 0U;
    g_music_dance_intensity = 0U;
    g_music_dance_intro_nod_count = 0U;
    g_music_analysis_active = false;
    g_music_mode_enabled = false;
    g_music_mode_listen_after_ms = 0U;
    g_music_previous_prediction_active = false;
    g_music_warmup_message_printed = false;
    g_music_tempo_lock_reported = false;

    music_onset_init(&g_music_onset_detector,
                     OBS_MUSIC_ONSET_SENSITIVITY,
                     OBS_MUSIC_ONSET_MINIMUM_INTERVAL_MS);
    music_beat_tracker_init(&g_music_beat_tracker, OBS_MUSIC_FRAME_RATE_HZ);
}

/** 节奏首次锁定时，以当前真实待机姿态开始四拍点头入场。 */
static void observer_music_dance_begin(float bpm)
{
    music_dance_session_begin(g_last_command);
    g_music_dance_intensity = OBS_MUSIC_DANCE_FULL_PERCENT;
    g_music_dance_intro_nod_count = 0U;
    g_music_active_bpm = (uint16_t) (bpm + 0.5f);
    g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
    observer_visual_confirmation_reset();
    g_behavior = OBSERVER_BEHAVIOR_DANCING;

    printf("Observer: rhythm confirmed, beat nod intro started at %lu BPM.\r\n",
           (unsigned long) (bpm + 0.5f));
}

/** 节拍消失后平缓回到中心，但保持音乐按钮开启并继续静止监听。 */
static void observer_music_dance_stop(void)
{
    uint32_t audio_guard_ms = 0U;

    if (music_dance_return_to_pose(g_standby_sequence[0].position,
                                   OBS_MUSIC_DANCE_RETURN_MS,
                                   &audio_guard_ms))
    {
        uint32_t return_guard_ms = audio_guard_ms;

        if (return_guard_ms < OBS_MUSIC_DANCE_RETURN_GUARD_MS)
        {
            return_guard_ms = OBS_MUSIC_DANCE_RETURN_GUARD_MS;
        }

        music_dance_last_position_get(g_last_command);
        g_last_command_valid = true;
        g_audio_motor_noise_ignore_until_ms =
            drv_gpt_timer_get_ms() + return_guard_ms;
        g_music_motor_noise_ignore_until_audio_ms =
            g_music_audio_timestamp_ms + return_guard_ms;
    }
    else
    {
        printf("Observer: dance return command failed.\r\n");
    }

    g_music_dance_intensity = 0U;
    g_music_dance_intro_nod_count = 0U;
    observer_music_tracking_reset();
    observer_music_listening_begin();
    printf("Observer: rhythm lost; music listening resumed.\r\n");
}

/** 新舵机动作只能延长噪声保护，不能把尚未结束的旧保护缩短。 */
static void observer_music_motor_guard_extend(uint32_t audio_guard_ms)
{
    uint32_t const wall_guard_until_ms =
        drv_gpt_timer_get_ms() + audio_guard_ms;
    uint32_t const audio_guard_until_ms =
        g_music_audio_timestamp_ms + audio_guard_ms;

    if (observer_time_reached(wall_guard_until_ms,
                              g_audio_motor_noise_ignore_until_ms))
    {
        g_audio_motor_noise_ignore_until_ms = wall_guard_until_ms;
    }
    if (observer_time_reached(audio_guard_until_ms,
                              g_music_motor_noise_ignore_until_audio_ms))
    {
        g_music_motor_noise_ignore_until_audio_ms = audio_guard_until_ms;
    }
}

/** 在主循环中完成同一拍的抬头半程，不使用阻塞延时。 */
static void observer_music_pending_nod_update(uint32_t now_ms)
{
    bool command_sent = false;
    uint32_t audio_guard_ms = 0U;

    if (OBSERVER_BEHAVIOR_DANCING != g_behavior)
    {
        return;
    }

    if (!music_dance_pending_update(now_ms,
                                    &command_sent,
                                    &audio_guard_ms))
    {
        printf("Observer: beat nod return command failed.\r\n");
        observer_music_dance_stop();
        return;
    }

    if (command_sent)
    {
        music_dance_last_position_get(g_last_command);
        g_last_command_valid = true;
        observer_music_motor_guard_extend(audio_guard_ms);
        /* 抬头属于同一拍的第二段，只同步姿态并延长舵机噪声保护。 */
    }
}

/** 前四个有效拍各完成一次完整点头，随后直接进入四帧主舞蹈。 */
static void observer_music_dance_beat_apply(float bpm)
{
    uint32_t audio_guard_ms = 0U;
    bool command_ok;

    if (g_music_dance_intro_nod_count < OBS_MUSIC_DANCE_INTRO_NOD_COUNT)
    {
        command_ok = music_dance_apply_intro_nod(bpm,
                                                  &audio_guard_ms);
    }
    else
    {
        command_ok = music_dance_apply_beat_scaled(
            bpm,
            OBS_MUSIC_DANCE_FULL_PERCENT,
            &audio_guard_ms);
    }

    if (!command_ok)
    {
        printf("Observer: dance servo command failed.\r\n");
        observer_music_dance_stop();
        return;
    }

    /* 大于100 BPM时舞蹈模块会跳过隔拍；未下发命令就不推进点头计数。 */
    if (0U == audio_guard_ms)
    {
        return;
    }

    /* 同步观察者的最后目标位置，确保退舞时的速度计算基于真实舞姿。 */
    music_dance_last_position_get(g_last_command);
    g_last_command_valid = true;
    if (audio_guard_ms > 0U)
    {
        observer_music_motor_guard_extend(audio_guard_ms);
    }

    if (g_music_dance_intro_nod_count < OBS_MUSIC_DANCE_INTRO_NOD_COUNT)
    {
        g_music_dance_intro_nod_count++;
        if (g_music_dance_intro_nod_count == OBS_MUSIC_DANCE_INTRO_NOD_COUNT)
        {
            /* 删除55%桥接拍，下一有效拍直接进入幅度明确的主舞蹈。 */
            printf("Observer: beat nod intro complete; main dance started.\r\n");
        }
    }
}

#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
/** 输出与MUSIC_TEST_ENABLE一致的起音、锁拍和节拍调试信息。 */
static void observer_music_detection_report(
    music_onset_result_t const * p_onset_result,
    music_beat_result_t const * p_beat_result)
{
    if ((NULL == p_onset_result) || (NULL == p_beat_result))
    {
        return;
    }

    if (!p_onset_result->ready && !g_music_warmup_message_printed)
    {
        printf("Onset detector warming up for about 1 second.\r\n");
        g_music_warmup_message_printed = true;
    }

    if (p_onset_result->detected)
    {
        uint32_t const strength_percent =
            (uint32_t) ((p_onset_result->strength * 100.0f) + 0.5f);

        g_music_onset_count++;
        printf("ONSET #%lu: strength=%lu%%, interval=%lu ms\r\n",
               (unsigned long) g_music_onset_count,
               (unsigned long) strength_percent,
               (unsigned long) p_onset_result->interval_ms);
    }

    if (p_beat_result->tempo_locked)
    {
        uint32_t const bpm = (uint32_t) (p_beat_result->bpm + 0.5f);
        uint32_t const confidence_percent =
            (uint32_t) ((p_beat_result->confidence * 100.0f) + 0.5f);
        uint32_t const bpm_difference =
            (bpm >= g_music_last_reported_bpm) ?
            (bpm - g_music_last_reported_bpm) :
            (g_music_last_reported_bpm - bpm);

        if (!g_music_tempo_lock_reported)
        {
            printf("TEMPO LOCK: bpm=%lu, confidence=%lu%%\r\n",
                   (unsigned long) bpm,
                   (unsigned long) confidence_percent);
            g_music_tempo_lock_reported = true;
            g_music_last_reported_bpm = bpm;
        }
        else if (p_beat_result->tempo_updated && (bpm_difference >= 3U))
        {
            printf("TEMPO UPDATE: bpm=%lu, confidence=%lu%%\r\n",
                   (unsigned long) bpm,
                   (unsigned long) confidence_percent);
            g_music_last_reported_bpm = bpm;
        }
    }

    if (g_music_previous_prediction_active &&
        !p_beat_result->prediction_active)
    {
        printf("BEAT prediction paused: no onset for 3.5 seconds.\r\n");
        g_music_tempo_lock_reported = false;
        g_music_last_reported_bpm = 0U;
    }

    if (p_beat_result->beat_event)
    {
        uint32_t const bpm = (uint32_t) (p_beat_result->bpm + 0.5f);
        uint32_t const confidence_percent =
            (uint32_t) ((p_beat_result->confidence * 100.0f) + 0.5f);

        g_music_beat_count++;
        printf("BEAT #%lu: bpm=%lu, confidence=%lu%%, predicted=1\r\n",
               (unsigned long) g_music_beat_count,
               (unsigned long) bpm,
               (unsigned long) confidence_percent);
    }
}
#endif

/** 分析一个32 ms频谱块，并按锁定的节拍驱动舞蹈状态。 */
static void observer_music_block_process(uint32_t now_ms)
{
    float32_t bands[MUSIC_ONSET_BAND_COUNT];
    bool detection_allowed;
    bool calibration_allowed;

    observer_music_fft_analyze(bands);
    g_music_audio_timestamp_ms += OBS_MUSIC_BLOCK_DURATION_MS;

    /*
     * 舞蹈噪声保护必须使用音频时间轴。若使用当前系统时间，FIFO中的旧
     * 机械声可能在延迟出队后越过保护截止时间，被误当成新的音乐起音。
     */
    detection_allowed = observer_time_reached(
        g_music_audio_timestamp_ms,
        g_music_motor_noise_ignore_until_audio_ms);
    calibration_allowed = detection_allowed &&
        observer_time_reached(now_ms,
                              g_music_calibration_motor_ignore_until_ms);

    if (g_music_calibration_count < OBS_MUSIC_CALIBRATION_BLOCKS)
    {
        uint32_t next_calibration_count;
        float32_t frame_total = 0.0f;

        /* 整个舵机运动期间暂停校准，避免把机身振动写进底噪门限。 */
        if (!calibration_allowed)
        {
            return;
        }

        next_calibration_count = g_music_calibration_count + 1U;
        for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
        {
            float32_t const difference =
                bands[band] - g_music_noise_mean[band];

            g_music_noise_mean[band] +=
                difference / (float32_t) next_calibration_count;
            g_music_noise_square_difference_sum[band] +=
                difference * (bands[band] - g_music_noise_mean[band]);
            frame_total += bands[band];
        }

        {
            float32_t const difference =
                frame_total - g_music_total_noise_mean;

            g_music_total_noise_mean +=
                difference / (float32_t) next_calibration_count;
            g_music_total_noise_square_difference_sum +=
                difference * (frame_total - g_music_total_noise_mean);
        }

        g_music_calibration_count = next_calibration_count;
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
        if ((0U == (g_music_calibration_count %
                    OBS_MUSIC_CALIBRATION_REPORT_STEP)) ||
            (g_music_calibration_count == OBS_MUSIC_CALIBRATION_BLOCKS))
        {
            printf("Onset calibration: %lu/%lu\r\n",
                   (unsigned long) g_music_calibration_count,
                   (unsigned long) OBS_MUSIC_CALIBRATION_BLOCKS);
        }
#endif
        if (g_music_calibration_count == OBS_MUSIC_CALIBRATION_BLOCKS)
        {
            for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
            {
                g_music_noise_gate[band] =
                    observer_music_noise_gate_calculate(
                        g_music_noise_mean[band],
                        g_music_noise_square_difference_sum[band],
                        g_music_calibration_count);
            }

            g_music_total_noise_gate = observer_music_noise_gate_calculate(
                g_music_total_noise_mean,
                g_music_total_noise_square_difference_sum,
                g_music_calibration_count);
            observer_music_tracking_reset();
            /* 校准只执行一次；完成后立即停止音乐分析，默认交还给摄像头。 */
            g_music_analysis_active = false;
            if (OBSERVER_BEHAVIOR_CALIBRATING == g_behavior)
            {
                if (g_music_mode_enabled)
                {
                    /* 用户在校准期间已按下按钮，校准完成后直接保持听音。 */
                    g_music_mode_listen_after_ms = now_ms;
                    observer_music_listening_begin();
                    printf("Observer: environment calibration complete; music listening enabled.\r\n");
                }
                else
                {
                    /* 校准完成前不运行巡查、待机动作或NPU，保证底噪来自静止环境。 */
                    observer_ready_behavior_begin();
#if OBS_INITIAL_PATROL_ENABLE
                    printf("Observer: environment calibration complete; initial patrol started.\r\n");
#else
                    printf("Observer: environment calibration complete; camera detection enabled.\r\n");
#endif
                }
            }
            if (!g_music_mode_enabled)
            {
                printf("Calibration complete. Press the music button before playing music.\r\n");
            }
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
            printf("MIC: channel=RIGHT(%u) Lavg=%ld Lpeak=%ld Ravg=%ld Rpeak=%ld dropped=%lu\r\n",
                   (unsigned int) Voice_ActiveChannelGet(),
                   (long) Voice_LeftAverageAbsGet(),
                   (long) Voice_LeftPeakAbsGet(),
                   (long) Voice_RightAverageAbsGet(),
                   (long) Voice_RightPeakAbsGet(),
                   (unsigned long) Voice_DroppedFrameCountGet());
#endif
        }
        return;
    }

    {
        music_onset_result_t onset_result;
        music_beat_result_t beat_result;
        float32_t frame_total = 0.0f;
        uint32_t active_band_count = 0U;
        bool spectrum_active;

        for (uint32_t band = 0U; band < MUSIC_ONSET_BAND_COUNT; band++)
        {
            float32_t const raw_band = bands[band];

            frame_total += raw_band;
            if (raw_band > g_music_noise_gate[band])
            {
                bands[band] = raw_band - g_music_noise_gate[band];
                active_band_count++;
            }
            else
            {
                bands[band] = 0.0f;
            }
        }

        spectrum_active =
            (frame_total > g_music_total_noise_gate) &&
            (active_band_count >= OBS_MUSIC_MINIMUM_ACTIVE_BANDS);

#if !OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
        if (!spectrum_active && (1U == active_band_count))
        {
            /*
             * 外壳遮挡可能让强鼓点只集中在一个频带。只有总能量明显高于
             * 环境门限时才放行，普通风扇等单频连续噪声仍会被挡住。
             */
            spectrum_active =
                (frame_total >
                 (g_music_total_noise_gate *
                  OBS_MUSIC_SINGLE_BAND_TOTAL_MULTIPLIER));
        }
#endif

        if (!spectrum_active)
        {
            memset(bands, 0, sizeof(bands));
        }

        if (!detection_allowed &&
            (OBSERVER_BEHAVIOR_DANCING == g_behavior))
        {
            /*
             * 舞蹈动作保护期内不仅禁止上报ONSET，也把频谱送零。否则机械
             * 振动仍会进入起音检测器的动态历史，保护结束时可能形成一个
             * 延迟峰值，再次为旧节拍续命。
             */
            memset(bands, 0, sizeof(bands));
        }

        onset_result = music_onset_update(&g_music_onset_detector,
                                          bands,
                                          g_music_audio_timestamp_ms,
                                          detection_allowed);
        beat_result = music_beat_tracker_update(
            &g_music_beat_tracker,
            onset_result.detected,
            onset_result.detected ? onset_result.strength : 0.0f,
            onset_result.timestamp_ms,
            g_music_audio_timestamp_ms);

        if (beat_result.tempo_locked)
        {
            /* UI与舵机统一使用节拍器过滤后的稳定BPM。 */
            g_music_active_bpm = (uint16_t) (beat_result.bpm + 0.5f);
        }

#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
        observer_music_detection_report(&onset_result, &beat_result);
#endif

        if ((OBSERVER_BEHAVIOR_DANCING == g_behavior) &&
            g_music_previous_prediction_active &&
            !beat_result.prediction_active)
        {
            observer_music_dance_stop();
            return;
        }
        g_music_previous_prediction_active = beat_result.prediction_active;

        if (beat_result.beat_event && beat_result.tempo_locked &&
            beat_result.prediction_active)
        {
            if (OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior)
            {
                observer_music_dance_begin(beat_result.bpm);
            }

            if (OBSERVER_BEHAVIOR_DANCING == g_behavior)
            {
                /* 每个有效拍都继续执行舞蹈，不再每8拍强制停住监听。 */
                observer_music_dance_beat_apply(beat_result.bpm);
            }
        }
    }
}

static void observer_audio_state_reset(void)
{
    g_audio_window_abs_sum = 0U;
    g_audio_window_sample_count = 0U;
    g_audio_window_frame_count = 0U;
    g_audio_baseline = 0U;
    g_audio_calibration_count = 0U;
    g_audio_loud_window_count = 0U;
    g_audio_motor_noise_ignore_until_ms = 0U;
    g_music_calibration_motor_ignore_until_ms = 0U;
}

static void observer_audio_window_process(uint32_t average_abs, uint32_t now_ms)
{
    uint32_t threshold;
    bool loud;

    /* 舵机起动噪声既不能参与底噪校准，也不能用来唤醒台灯。 */
    if (!observer_time_reached(now_ms, g_audio_motor_noise_ignore_until_ms))
    {
        return;
    }

    if (g_audio_calibration_count < OBS_AUDIO_CALIBRATION_WINDOWS)
    {
        /* 响度基线也只在舵机完全静止时累计。 */
        if (!observer_time_reached(
                now_ms,
                g_music_calibration_motor_ignore_until_ms))
        {
            return;
        }

        g_audio_baseline = ((g_audio_baseline * g_audio_calibration_count) + average_abs) /
                           (g_audio_calibration_count + 1U);
        g_audio_calibration_count++;
        return;
    }

    threshold = (g_audio_baseline * OBS_AUDIO_LOUD_THRESHOLD_NUMERATOR) /
                OBS_AUDIO_LOUD_THRESHOLD_DENOMINATOR;
    if (0U == threshold)
    {
        threshold = 1U;
    }
    loud = (average_abs > threshold);

    if (loud)
    {
        if (g_audio_loud_window_count < UINT8_MAX)
        {
            g_audio_loud_window_count++;
        }

        if ((OBSERVER_BEHAVIOR_SLEEPING == g_behavior) &&
            (g_audio_loud_window_count >= OBS_AUDIO_WAKE_CONFIRM_WINDOWS))
        {
            g_audio_loud_window_count = 0U;
            observer_wake_begin();
        }
    }
    else
    {
        g_audio_loud_window_count = 0U;

        /* 只用非明显声音缓慢更新底噪，避免音乐把阈值越推越高。 */
        g_audio_baseline = ((g_audio_baseline * OBS_AUDIO_BASELINE_FILTER_WEIGHT) + average_abs) /
                           OBS_AUDIO_BASELINE_FILTER_DIVISOR;
    }
}

void ObserverMode_Start(void)
{
    uint32_t start_ms;

    if (g_active)
    {
        return;
    }

    Servo_NormalMode();
    Servo_Power_on();

    observer_copy_position(g_last_command, g_power_on_position);
    g_last_command_valid = true;
    g_nod_cooldown_until_ms = 0U;
    g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
    observer_visual_confirmation_reset();
    observer_audio_state_reset();
    g_music_fft_ready =
        (ARM_MATH_SUCCESS ==
         arm_rfft_fast_init_f32(&g_music_fft_instance, OBS_MUSIC_FFT_SIZE));
    if (g_music_fft_ready)
    {
        arm_hanning_f32(g_music_fft_window, OBS_MUSIC_FFT_SIZE);
    }
    else
    {
        /* FFT失败时本次桌宠会话只保留摄像头人脸检测。 */
        printf("Observer: music FFT initialization failed.\r\n");
    }
    observer_music_state_reset();
    start_ms = drv_gpt_timer_get_ms();
    g_last_update_ms = start_ms;
    g_audio_motor_noise_ignore_until_ms =
        start_ms + OBS_MUSIC_STARTUP_SETTLE_MS;
    g_music_calibration_motor_ignore_until_ms =
        start_ms + OBS_MUSIC_STARTUP_SETTLE_MS;
    g_music_calibration_deadline_ms =
        start_ms + OBS_MUSIC_CALIBRATION_TIMEOUT_MS;
    g_hearing_disabled_for_session = false;

    /* 丢弃上一次桌宠会话可能残留的跟踪结果。 */
    g_tracking_frame_pending = false;
    g_tracking_frame_valid = false;
    g_tracking_face_lost = false;
    g_tracking_face_lost_since_ms = 0U;

    if (g_music_fft_ready)
    {
        /*
         * 校准期间不执行任何关键帧，也不开放NPU。2秒余振保护
         * 结束后才启动麦克风，保证底噪样本全部来自舵机静止期。
         */
        observer_sequence_begin(OBSERVER_BEHAVIOR_CALIBRATING);
    }
    else
    {
        /* FFT不可用时跳过频谱底噪校准，直接进入摄像头待机。 */
        observer_ready_behavior_begin();
    }
    g_active = true;

    if (g_music_fft_ready)
    {
        printf("Observer mode started; servos held still for environment calibration.\r\n");
    }
    else
    {
        printf("Observer mode started without music analysis; camera detection enabled.\r\n");
    }
}

void ObserverMode_Stop(void)
{
    if (!g_active)
    {
        return;
    }

    g_active = false;
    g_music_mode_enabled = false;
    g_music_mode_listen_after_ms = 0U;
    observer_visual_confirmation_reset();
    printf("Observer mode stopped.\r\n");
}

void ObserverMode_Update(void)
{
    uint32_t const now_ms = drv_gpt_timer_get_ms();
    bool sequence_complete;

    if (g_active && (OBSERVER_BEHAVIOR_DANCING == g_behavior))
    {
        /* 先检查拍内抬头，不能被下方20 ms普通动作刷新周期限制。 */
        observer_music_pending_nod_update(now_ms);
    }

    if (!g_active || ((now_ms - g_last_update_ms) < OBS_UPDATE_PERIOD_MS))
    {
        return;
    }
    g_last_update_ms = now_ms;

    switch (g_behavior)
    {
        case OBSERVER_BEHAVIOR_CALIBRATING:
            /* 校准完全由音频处理推进；此状态禁止舵机和视觉产生动作。 */
            if (observer_time_reached(now_ms, g_music_calibration_deadline_ms))
            {
                /*
                 * 麦克风永久故障时不能让摄像头也被锁死。本次会话关闭听觉和
                 * 音乐分析，直接进入待机；重新进入桌宠模式时会再次尝试校准。
                 */
                g_hearing_disabled_for_session = true;
                g_music_fft_ready = false;
                g_music_analysis_active = false;
                g_music_mode_enabled = false;
                g_music_mode_listen_after_ms = 0U;
                observer_ready_behavior_begin();
                printf("Observer: audio calibration timeout; camera-only mode enabled.\r\n");
            }
            break;

        case OBSERVER_BEHAVIOR_OBSERVING:
        {
            sequence_complete = observer_sequence_update(g_observe_sequence,
                                                         OBS_ARRAY_COUNT(g_observe_sequence));
            if (sequence_complete)
            {
#if OBS_PATROL_REVIEW_LOOP_ENABLE
                /* 调试阶段持续循环首次巡查，不启动麦克风和NPU识别。 */
                observer_sequence_begin(OBSERVER_BEHAVIOR_OBSERVING);
                printf("Observer: patrol review loop restarted.\r\n");
#else
                /* 首次巡查只执行一轮；完成后才开启待机阶段的视觉和听觉。 */
                observer_standby_begin();
                printf("Observer: patrol complete; standby sensors active.\r\n");
#endif
            }
            break;
        }

        case OBSERVER_BEHAVIOR_STANDBY:
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
            /* 音乐专项测试保持静止且不进入睡眠，避免动作和状态切换清空节拍。 */
            break;
#else
            /*
             * 人脸确认后立即进入跟踪，不等当前关键帧执行完，保证响应及时。
             * 普通待机只开放摄像头；音乐必须由屏幕按钮显式开启。
             */
            if (OBSERVER_VISUAL_TARGET_FACE == g_pending_visual_target)
            {
                g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
                observer_tracking_begin();
                break;
            }
            /*
             * 待机阶段允许摄像头提交目标，但只在两张小动作关键帧之间插入
             * 点头回应，避免NPU结果覆盖正在执行的舵机命令。
             * app_main 不再提交 BOOK，此分支实际不可达，保留以防恢复书本点头。
             */
            if ((OBSERVER_VISUAL_TARGET_NONE != g_pending_visual_target) &&
                !g_keyframe_command_sent)
            {
                observer_visual_target_t const target = g_pending_visual_target;
                g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
                observer_nod_begin(target);
                break;
            }

            sequence_complete = observer_sequence_update(g_standby_sequence,
                                                          OBS_ARRAY_COUNT(g_standby_sequence));
            if (sequence_complete)
            {
                if (OBSERVER_VISUAL_TARGET_NONE != g_pending_visual_target)
                {
                    observer_visual_target_t const target = g_pending_visual_target;
                    g_pending_visual_target = OBSERVER_VISUAL_TARGET_NONE;
                    observer_nod_begin(target);
                }
                else
                {
                    if (g_hearing_disabled_for_session)
                    {
                        /* 麦克风不可用时无法被声音唤醒，因此继续循环待机动作。 */
                        observer_standby_begin();
                    }
                    else
                    {
                        /* 17帧待机动作全部完成且没有触发事件，进入睡眠姿态。 */
                        observer_visual_confirmation_reset();
                        observer_sequence_begin(OBSERVER_BEHAVIOR_GOING_TO_SLEEP);
                        printf("Observer: standby timeout; going to sleep.\r\n");
                    }
                }
            }
            break;
#endif

        case OBSERVER_BEHAVIOR_NODDING:
            sequence_complete = observer_sequence_update(g_nod_sequence,
                                                         g_nod_sequence_length);
            if (sequence_complete)
            {
                g_nod_cooldown_until_ms = now_ms + OBS_NOD_COOLDOWN_MS;
                observer_visual_confirmation_reset();
                /* 点头结束后回到待机中心，并重新开始一轮约30秒待机。 */
                observer_standby_begin();
                printf("Observer: response complete; standby resumed.\r\n");
            }
            break;

        case OBSERVER_BEHAVIOR_TRACKING:
            /*
             * 跟踪期间不发送关键帧动作，舵机命令完全由 SubmitTrackingFrame
             * 提交的人脸坐标驱动。无脸时保持最后目标，不用无效结果覆盖舵机
             * 命令；从首个无脸结果起连续4秒没有重新发现人脸才回到待机。
             */
            if (g_tracking_frame_pending)
            {
                g_tracking_frame_pending = false;
                if (g_tracking_frame_valid)
                {
                    g_tracking_miss_count = 0U;
                    g_tracking_face_lost = false;
                    g_tracking_face_lost_since_ms = 0U;
                    observer_tracking_servo_update(g_tracking_frame_x,
                                                   g_tracking_frame_y);
                }
                else
                {
                    if (!g_tracking_face_lost)
                    {
                        g_tracking_face_lost = true;
                        g_tracking_face_lost_since_ms = now_ms;
                    }
                    if (g_tracking_miss_count < UINT8_MAX)
                    {
                        g_tracking_miss_count++;
                    }
                }
            }
            if (g_tracking_face_lost &&
                ((now_ms - g_tracking_face_lost_since_ms) >=
                 OBS_TRACK_FACE_LOST_TIMEOUT_MS))
            {
                observer_tracking_stop();
            }
            break;

        case OBSERVER_BEHAVIOR_MUSIC_LISTENING:
            /* 音乐按钮已开启：舵机保持静止，动作只能由确认的节拍触发。 */
            break;

        case OBSERVER_BEHAVIOR_DANCING:
            /*
             * 舞蹈命令严格由ObserverMode_AudioUpdate()产生的节拍触发；
             * 此处不额外发送定时动作，防止舵机运动与音乐节奏脱离。
             */
            break;

        case OBSERVER_BEHAVIOR_GOING_TO_SLEEP:
            /* 进入用户实机记录的睡眠姿态。 */
            sequence_complete = observer_sequence_update(g_sleep_sequence,
                                                          OBS_ARRAY_COUNT(g_sleep_sequence));
            if (sequence_complete)
            {
                /*
                 * 从真正进入睡眠的时刻重新累计唤醒声音，避免收灯过程中残留的
                 * 高音量窗口使台灯刚睡下便立即醒来；环境底噪基线继续保留。
                 */
                g_audio_window_abs_sum = 0U;
                g_audio_window_sample_count = 0U;
                g_audio_window_frame_count = 0U;
                g_audio_loud_window_count = 0U;
                g_behavior = OBSERVER_BEHAVIOR_SLEEPING;
                printf("Observer: sleeping; vision paused, hearing active.\r\n");
            }
            break;

        case OBSERVER_BEHAVIOR_WAKING:
            sequence_complete = observer_sequence_update(g_wake_sequence,
                                                         OBS_ARRAY_COUNT(g_wake_sequence));
            if (sequence_complete)
            {
                /* 被声音唤醒后直接回到待机，不再重复首次大范围巡查。 */
                observer_standby_begin();
                printf("Observer: awake; standby resumed.\r\n");
            }
            break;

        case OBSERVER_BEHAVIOR_SLEEPING:
        default:
            /* 睡眠期间不发送舵机命令，只保留声音监听。 */
            break;
    }
}

void ObserverMode_SetMusicModeEnabled(bool enabled)
{
    if (!g_active)
    {
        return;
    }

    if (enabled)
    {
        if ((OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior) ||
            (OBSERVER_BEHAVIOR_DANCING == g_behavior))
        {
            g_music_mode_enabled = true;
            return;
        }

        if (OBSERVER_BEHAVIOR_CALIBRATING == g_behavior)
        {
            /* 先记住按钮请求，待本次唯一的底噪校准完成后自动进入音乐监听。 */
            g_music_mode_enabled = true;
            return;
        }

        /* 底噪尚未校准或FFT不可用时，不允许进入无有效门限的音乐识别。 */
        if (!g_music_fft_ready || g_hearing_disabled_for_session ||
            (g_music_calibration_count < OBS_MUSIC_CALIBRATION_BLOCKS))
        {
            return;
        }

        if (OBSERVER_BEHAVIOR_TRACKING == g_behavior)
        {
            /* 音乐按钮优先：保存当前跟踪姿态，再停止后续视觉控制。 */
            observer_tracking_position_sync();
            g_tracking_frame_pending = false;
            g_tracking_frame_valid = false;
            g_tracking_face_lost = false;
            g_tracking_face_lost_since_ms = 0U;
        }
        else if (OBSERVER_BEHAVIOR_STANDBY != g_behavior)
        {
            /* 其他过渡动作期间不抢占舵机控制权。 */
            return;
        }

        g_music_mode_enabled = true;
        /* 保留首次校准得到的噪声门限，只清除上一次节拍历史。 */
        observer_music_tracking_reset();
        g_music_analysis_active = false;
        g_music_mode_listen_after_ms =
            drv_gpt_timer_get_ms() + OBS_MUSIC_MODE_START_SETTLE_MS;
        observer_music_listening_begin();
        printf("Observer: music mode enabled; camera paused.\r\n");
        return;
    }

    g_music_mode_enabled = false;
    g_music_mode_listen_after_ms = 0U;

    if (OBSERVER_BEHAVIOR_DANCING == g_behavior)
    {
        /* 用户主动关闭时仍使用原有的平缓退舞动作。 */
        observer_music_dance_stop();
    }

    if (OBSERVER_BEHAVIOR_MUSIC_LISTENING != g_behavior)
    {
        return;
    }

    g_music_analysis_active = false;
    observer_music_tracking_reset();
    observer_standby_begin();
    printf("Observer: music mode disabled; camera resumed.\r\n");
}

bool ObserverMode_IsMusicModeEnabled(void)
{
    return g_active && g_music_mode_enabled;
}

void ObserverMode_SubmitVisionResult(observer_visual_target_t target)
{
    uint32_t const now_ms = drv_gpt_timer_get_ms();

    /* 书本不再触发任何回应（既不点头也不跟踪），防御性忽略。 */
    if (OBSERVER_VISUAL_TARGET_BOOK == target)
    {
        return;
    }

    if (!g_active || (OBSERVER_BEHAVIOR_STANDBY != g_behavior) ||
        (OBSERVER_VISUAL_TARGET_NONE != g_pending_visual_target))
    {
        return;
    }

    if (OBSERVER_VISUAL_TARGET_NONE == target)
    {
        if (g_visual_miss_count < UINT8_MAX)
        {
            g_visual_miss_count++;
        }
        if (g_visual_miss_count > OBS_VISION_ALLOWED_MISS_COUNT)
        {
            observer_visual_confirmation_reset();
        }
        return;
    }

    g_visual_miss_count = 0U;
    if (target == g_visual_candidate)
    {
        if (g_visual_confirm_count < UINT8_MAX)
        {
            g_visual_confirm_count++;
        }
    }
    else
    {
        g_visual_candidate = target;
        g_visual_confirm_count = 1U;
    }

    if ((g_visual_confirm_count >= OBS_VISION_CONFIRM_COUNT) &&
        (OBSERVER_BEHAVIOR_STANDBY == g_behavior) &&
        ((int32_t) (now_ms - g_nod_cooldown_until_ms) >= 0))
    {
        g_pending_visual_target = g_visual_candidate;
        observer_visual_confirmation_reset();
    }
}

void ObserverMode_SubmitTrackingFrame(bool valid, int16_t box_center_x, int16_t box_center_y)
{
    if (!g_active || (OBSERVER_BEHAVIOR_TRACKING != g_behavior))
    {
        return;
    }
    /*
     * 控制周期内只消费一帧，但始终保存最新NPU结果。若上一帧尚未消费，
     * 新坐标直接覆盖旧坐标，避免舵机继续追逐已经过时的人脸位置。
     */
    g_tracking_frame_valid = valid;
    g_tracking_frame_x = box_center_x;
    g_tracking_frame_y = box_center_y;
    g_tracking_frame_pending = true;
}

void ObserverMode_AudioUpdate(void)
{
    uint32_t frames_processed = 0U;
    bool music_allowed;

    if (!g_active)
    {
        return;
    }

    music_allowed = g_music_fft_ready &&
        ((OBSERVER_BEHAVIOR_CALIBRATING == g_behavior) ||
         (OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior) ||
         (OBSERVER_BEHAVIOR_DANCING == g_behavior));

    if (g_active && (OBSERVER_BEHAVIOR_DANCING == g_behavior))
    {
        /* 音频轮询入口也补查一次，避免LVGL刷新延迟拍内抬头命令。 */
        observer_music_pending_nod_update(drv_gpt_timer_get_ms());
    }

    /*
     * 先同步音乐分析状态，再检查麦克风是否已经停下。这样切入人脸跟踪时，
     * 即使主程序已关闭I2S，也能立即清除旧节拍，避免退出跟踪后续接旧音乐。
     */
    if (music_allowed && !g_music_analysis_active)
    {
        observer_music_tracking_reset();
        g_music_analysis_active = true;
    }
    else if (!music_allowed && g_music_analysis_active)
    {
        observer_music_tracking_reset();
        g_music_analysis_active = false;
    }

    if (!Voice_Running())
    {
        return;
    }

    {
        uint32_t const dropped_frame_count = Voice_DroppedFrameCountGet();

        if (dropped_frame_count != g_music_last_dropped_frame_count)
        {
            /* 丢帧会破坏FFT连续性；舞蹈中发生丢帧时先安全返回待机。 */
            g_music_last_dropped_frame_count = dropped_frame_count;
            if (OBSERVER_BEHAVIOR_DANCING == g_behavior)
            {
                observer_music_dance_stop();
            }
            else
            {
                observer_music_tracking_reset();
            }
            printf("Observer: microphone frame gap, rhythm history reset; dropped=%lu.\r\n",
                   (unsigned long) dropped_frame_count);
        }
    }

    while ((frames_processed < OBS_AUDIO_FRAMES_PER_UPDATE_MAX) && Voice_FrameReady())
    {
        uint32_t const sample_count = Voice_FrameRead(g_audio_frame, OBS_AUDIO_FRAME_CAPACITY);
        uint32_t const frame_now_ms = drv_gpt_timer_get_ms();
        bool music_samples_allowed = g_music_analysis_active;
        bool loudness_samples_allowed = true;

        if (0U == sample_count)
        {
            break;
        }

        if (music_samples_allowed &&
            (g_music_calibration_count < OBS_MUSIC_CALIBRATION_BLOCKS) &&
            !observer_time_reached(
                frame_now_ms,
                g_music_calibration_motor_ignore_until_ms))
        {
            /*
             * 校准等待期间不把样本拼进下一块FFT。同步清空不足512点的
             * 半块数据，保证恢复后采用的第一块频谱全部来自舵机静止期。
             */
            g_music_fft_sample_count = 0U;
            music_samples_allowed = false;
        }

        if ((g_audio_calibration_count < OBS_AUDIO_CALIBRATION_WINDOWS) &&
            !observer_time_reached(
                frame_now_ms,
                g_music_calibration_motor_ignore_until_ms))
        {
            /* 响度基线同样只接收完整的静止音频窗口。 */
            g_audio_window_abs_sum = 0U;
            g_audio_window_sample_count = 0U;
            g_audio_window_frame_count = 0U;
            loudness_samples_allowed = false;
        }

        for (uint32_t index = 0U; index < sample_count; index++)
        {
            int64_t const sample = g_audio_frame[index];

            if (loudness_samples_allowed)
            {
                g_audio_window_abs_sum +=
                    (uint64_t) ((sample < 0) ? -sample : sample);
            }

            if (music_samples_allowed)
            {
                g_music_fft_input[g_music_fft_sample_count++] =
                    (float32_t) g_audio_frame[index] * OBS_MUSIC_SAMPLE_SCALE;

                if (g_music_fft_sample_count >= OBS_MUSIC_FFT_SIZE)
                {
                    observer_music_block_process(drv_gpt_timer_get_ms());
                    g_music_fft_sample_count = 0U;
                }
            }
        }
        if (loudness_samples_allowed)
        {
            g_audio_window_sample_count += sample_count;
            g_audio_window_frame_count++;
        }
        frames_processed++;

        if (loudness_samples_allowed &&
            (g_audio_window_frame_count >= OBS_AUDIO_FRAMES_PER_WINDOW))
        {
            uint32_t const average_abs = (0U != g_audio_window_sample_count) ?
                (uint32_t) (g_audio_window_abs_sum / g_audio_window_sample_count) : 0U;

            observer_audio_window_process(average_abs, frame_now_ms);
            g_audio_window_abs_sum = 0U;
            g_audio_window_sample_count = 0U;
            g_audio_window_frame_count = 0U;
        }
    }
}

bool ObserverMode_IsActive(void)
{
    return g_active;
}

bool ObserverMode_IsVisionRequired(void)
{
#if OBSERVER_MUSIC_DIAGNOSTIC_ENABLE
    /* 音乐专项测试关闭NPU，排除推理阻塞和视觉状态切换。 */
    return false;
#else
    /*
     * 默认待机和人脸跟踪执行视觉推理；校准、音乐监听与跳舞期间
     * 暂停NPU，保证麦克风音乐识别与摄像头跟踪互斥。
     */
    return g_active &&
           ((OBSERVER_BEHAVIOR_STANDBY == g_behavior) ||
            (OBSERVER_BEHAVIOR_TRACKING == g_behavior));
#endif
}

bool ObserverMode_IsHearingRequired(void)
{
    if (!g_active)
    {
        return false;
    }

    if (g_hearing_disabled_for_session)
    {
        return false;
    }

    if (OBSERVER_BEHAVIOR_CALIBRATING == g_behavior)
    {
        /*
         * 台灯展开和余振完全结束后才真正启动麦克风。CALIBRATING状态本身
         * 不发送舵机命令，因此随后约5.12秒采集期间台灯会一直保持静止。
         */
        return observer_time_reached(
            drv_gpt_timer_get_ms(),
            g_music_calibration_motor_ignore_until_ms);
    }

    if (OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior)
    {
        /* 切换按钮后先等待上一条舵机命令和机身余振结束，再启动I2S。 */
        return observer_time_reached(drv_gpt_timer_get_ms(),
                                     g_music_mode_listen_after_ms);
    }

    if (OBSERVER_BEHAVIOR_DANCING == g_behavior)
    {
        /* 舞蹈期间持续听音，才能维持节拍预测并判断音乐是否结束。 */
        return true;
    }

    /*
     * 睡眠流程只保留轻量响度唤醒监听。ObserverMode_AudioUpdate()仅在
     * CALIBRATING、MUSIC_LISTENING和DANCING状态运行FFT与节奏分析，
     * 因此这里开启麦克风不会把睡眠期间的声音识别成音乐节拍。
     */
    return (OBSERVER_BEHAVIOR_GOING_TO_SLEEP == g_behavior) ||
           (OBSERVER_BEHAVIOR_SLEEPING == g_behavior) ||
           (OBSERVER_BEHAVIOR_WAKING == g_behavior);
}

uint8_t ObserverMode_GetDanceIntensity(void)
{
    return (OBSERVER_BEHAVIOR_DANCING == g_behavior) ?
           g_music_dance_intensity : 0U;
}

uint16_t ObserverMode_GetMusicBpm(void)
{
    return (g_active && (OBSERVER_BEHAVIOR_DANCING == g_behavior)) ?
           g_music_active_bpm : 0U;
}

uint8_t ObserverMode_GetActionIndex(void)
{
    /* 点头入场期间每个实际动作拍刷新一次桌宠表情。 */
    if (OBSERVER_BEHAVIOR_DANCING == g_behavior)
    {
        return g_music_dance_intro_nod_count;
    }

    /* 校准、跟踪和音乐监听均不推进关键帧索引，返回固定值避免UI越界。 */
    if ((OBSERVER_BEHAVIOR_CALIBRATING == g_behavior) ||
        (OBSERVER_BEHAVIOR_TRACKING == g_behavior) ||
        (OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior))
    {
        return 0U;
    }

    return g_sequence_index;
}

uint8_t ObserverMode_GetActionCount(void)
{
    if (OBSERVER_BEHAVIOR_STANDBY == g_behavior)
    {
        return OBS_ARRAY_COUNT(g_standby_sequence);
    }

    if ((OBSERVER_BEHAVIOR_CALIBRATING == g_behavior) ||
        (OBSERVER_BEHAVIOR_TRACKING == g_behavior) ||
        (OBSERVER_BEHAVIOR_MUSIC_LISTENING == g_behavior))
    {
        return 1U;
    }

    return OBS_ARRAY_COUNT(g_observe_sequence);
}

observer_behavior_t ObserverMode_GetBehavior(void)
{
    return g_behavior;
}

observer_expression_t ObserverMode_GetExpression(void)
{
    switch (g_behavior)
    {
        case OBSERVER_BEHAVIOR_CALIBRATING:
            return OBSERVER_EXPRESSION_LISTENING;

        case OBSERVER_BEHAVIOR_NODDING:
            return OBSERVER_EXPRESSION_RESPONDING;

        case OBSERVER_BEHAVIOR_TRACKING:
            return OBSERVER_EXPRESSION_FOCUSED;

        case OBSERVER_BEHAVIOR_MUSIC_LISTENING:
            return OBSERVER_EXPRESSION_LISTENING;

        case OBSERVER_BEHAVIOR_DANCING:
            return OBSERVER_EXPRESSION_HAPPY;

        case OBSERVER_BEHAVIOR_GOING_TO_SLEEP:
            return OBSERVER_EXPRESSION_FOCUSED;

        case OBSERVER_BEHAVIOR_SLEEPING:
            return OBSERVER_EXPRESSION_SLEEPING;

        case OBSERVER_BEHAVIOR_WAKING:
            return OBSERVER_EXPRESSION_ALERT;

        case OBSERVER_BEHAVIOR_STANDBY:
            if (g_sequence_index < OBS_ARRAY_COUNT(g_standby_expressions))
            {
                return g_standby_expressions[g_sequence_index];
            }
            return OBSERVER_EXPRESSION_NEUTRAL;

        case OBSERVER_BEHAVIOR_OBSERVING:
        default:
            if (g_sequence_index < OBS_ARRAY_COUNT(g_observe_expressions))
            {
                return g_observe_expressions[g_sequence_index];
            }
            return OBSERVER_EXPRESSION_NEUTRAL;
    }
}
