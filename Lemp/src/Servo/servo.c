/*
 * servo.c
 *
 *  Created on: 2026年5月30日
 *      Author: 36315
 */

/*  在这里我们编写台灯模式的代码，台灯模式下舵机处于阻尼模式使得5个舵机都处于阻尼模式 */

#include "servo.h"
#include "hal_data.h"
#include "Screen/drv_gpt_timer.h"
#include "ServoLib/ServoDriver.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <math.h>
#define PI 3.1415926f


// 硬件参数
#define BASE_H      5.5f//舵机2号的高度
#define ARM_L1      24.4f
#define ARM_L2      35.5f
#define SERVO2_MIN  1028 //舵机2的最小位置  相当于0°
#define SERVO3_ZERO 1563 //舵机3的中位置  相当于90°
#define SERVO3_HW_MIN 961
#define SERVO3_HW_MAX 3080
#define Each_count 0.088f //官方给的文档中说 1个计数值是0.088f

/* 用户推动承重关节后，保持600 ms阻尼时间再恢复支撑扭矩。 */
#define DAMPING_RELEASE_TIME_MS    (600U)
#define DAMPING_SUPPORT_STABLE_MS  (300U)
#define SERVO2_LOAD_THRESHOLD      (150)
#define SERVO3_LOAD_THRESHOLD      (220)

typedef struct st_damping_joint_state
{
    bool     released;
    bool     baseline_valid;
    uint32_t release_start_ms;
    uint32_t support_start_ms;
    int      baseline_load;
} damping_joint_state_t;

uint8_t load_arr[5] = {1,2,3,4,5};
//舵机当前位置
static int servo1_pos = 0;
static int servo2_pos = 0;
static int servo3_pos = 0;
static int servo4_pos = 0;
static int servo5_pos = 0;
//舵机当前速度
static uint16_t speed[5] = {0,0,0,0,0};
//舵机当前加速度
static uint16_t acc[5] = {0,0,0,0,0};

static bool                  s_damping_pro_started;
static damping_joint_state_t s_servo2_damping_state;
static damping_joint_state_t s_servo3_damping_state;
/**
 * @brief  计算3号舵机的完整安全范围（上下限）
 */
static void calc_servo3_safe_range(int *out_min, int *out_max)
{
    // 1. 获取当前舵机位置
    servo2_pos = Servo_ReadPosition(2);
    // 2. 2号舵机计数 → 角度 → 弧度
    float theta1_deg = (float)(servo2_pos - SERVO2_MIN) * Each_count;
    float theta1_rad = theta1_deg * PI / 180.0f;  // 修正拼写错误

    // 3. 计算临界正弦值
    float sin_critical = -(BASE_H + ARM_L1 * sinf(theta1_rad)) / ARM_L2;

    // 4. 完全安全分支：末端永远不会碰地
    if (sin_critical <= -1.0f)
    {
        *out_min = SERVO3_HW_MIN;
        *out_max = SERVO3_HW_MAX;
        return;
    }

    // 5. 解正弦不等式，得到小臂绝对角度的上下限（弧度）
    float x_min_rad = asinf(sin_critical);
    float x_max_rad = PI - asinf(sin_critical);

    // ========== 修正：绝对角度 → 大小臂相对夹角 ==========
    float phi_min_rad = x_min_rad - theta1_rad;
    float phi_max_rad = x_max_rad - theta1_rad;

    // 6. 弧度 → 角度
    float phi_min_deg = phi_min_rad * 180.0f / PI;
    float phi_max_deg = phi_max_rad * 180.0f / PI;

    // ========== 修正：用零位做基准计算舵机计数 ==========
    *out_min = SERVO3_ZERO + (int)(phi_min_deg / Each_count);
    *out_max = SERVO3_ZERO + (int)(phi_max_deg / Each_count);

    // 7. 钳位到硬件物理极限
    if (*out_min < SERVO3_HW_MIN) *out_min = SERVO3_HW_MIN;
    if (*out_max > SERVO3_HW_MAX) *out_max = SERVO3_HW_MAX;
}
//阻尼模式
/*
 * Pace consecutive torque commands so the half-duplex servo bus and each
 * servo have time to process the previous command. Write acknowledgements
 * are disabled in the driver, so a failed UART transfer is retried once.
 */
static void servo_enable_torque_paced(uint8_t servo_id, uint8_t torque_mode)
{
    if (!Servo_EnableTorque(servo_id, torque_mode))
    {
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
        (void) Servo_EnableTorque(servo_id, torque_mode);
    }

    R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
}

void Servo_DampingMode()
{
   Servo_EnableTorque(1,2);
   Servo_EnableTorque(4,2);
   Servo_EnableTorque(5,2);
   Servo_EnableTorque(2,2);
   Servo_EnableTorque(3,2);
   printf("All servos are in damping mode!\r\n");
}
//正常模式（固定模式）
void Servo_NormalMode()
{
   /*
    * 扭矩使能并不等于位置模式。先显式退出可能残留的轮式模式，
    * 避免后续位置命令中的速度字段让舵机持续旋转。
    */
   if (!Servo_PositionMode(SERVO_STS_BROADCAST_ID))
   {
       R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
       (void) Servo_PositionMode(SERVO_STS_BROADCAST_ID);
   }
   R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);

   /* Restore holding torque one servo at a time before sending positions. */
   servo_enable_torque_paced(1U, 1U);
   servo_enable_torque_paced(4U, 1U);
   servo_enable_torque_paced(5U, 1U);
   servo_enable_torque_paced(2U, 1U);
   servo_enable_torque_paced(3U, 1U);
//    //延时1s，准备
//    R_BSP_SoftwareDelay(1000, BSP_DELAY_UNITS_MILLISECONDS);
}
static void damping_joint_update(uint8_t servo_id,
                                 int load_threshold,
                                 damping_joint_state_t * p_state,
                                 uint32_t now_ms)
{
    int moving;
    int load;
    int load_change;

    if (p_state->released)
    {
        if ((now_ms - p_state->release_start_ms) >= DAMPING_RELEASE_TIME_MS)
        {
            Servo_EnableTorque(servo_id, 1);
            p_state->released = false;
            p_state->baseline_valid = false;
            p_state->support_start_ms = now_ms;
        }
        return;
    }

    /*
     * 刚恢复正常扭矩时，舵机内部控制器和机械结构需要短暂稳定。
     * 此时不读取负载，避免启动瞬间的负载峰值被误认为用户正在掰动。
     */
    if ((now_ms - p_state->support_start_ms) < DAMPING_SUPPORT_STABLE_MS)
    {
        return;
    }

    moving = Servo_ReadMoving(servo_id);
    load = Servo_ReadLoad(servo_id);
    if ((moving < 0) || (load < -1023) || (load > 1023) || (0 != Servo_GetLastError()))
    {
        return;
    }

    if (!p_state->baseline_valid)
    {
        /* 记录当前姿态下由台灯自重产生的静态负载。 */
        if (0 == moving)
        {
            p_state->baseline_load = load;
            p_state->baseline_valid = true;
        }
        return;
    }

    load_change = abs(load - p_state->baseline_load);
    if ((0 == moving) && (load_change > load_threshold))
    {
        /*
         * 只有负载相对于台灯自重基准发生明显变化，才认为用户正在推动。
         * 随后切入阻尼，但不在这里阻塞600 ms。
         * 后续主循环会继续刷新界面，并在时间到达后恢复支撑扭矩。
         */
        Servo_EnableTorque(servo_id, 2);
        p_state->released = true;
        p_state->baseline_valid = false;
        p_state->release_start_ms = now_ms;
    }
    else if ((0 == moving) && (load_change < (load_threshold / 3)))
    {
        /*
         * 仅在负载变化很小时缓慢修正基准，用于适应温漂和轻微姿态变化；
         * 明显的用户施力不会被基准更新吸收掉。
         */
        p_state->baseline_load += (load - p_state->baseline_load) / 8;
    }
}

/* 台灯模式下持续保护2、3号承重舵机，同时允许用户手动调节姿态。 */
void Servo_DampingMode_Pro(void)
{
    uint32_t now_ms = drv_gpt_timer_get_ms();

    if (!s_damping_pro_started)
    {
        /* 1、4、5号保持阻尼；2、3号使用正常扭矩支撑台灯。 */
        servo_enable_torque_paced(1U, 2U);
        servo_enable_torque_paced(4U, 2U);
        servo_enable_torque_paced(5U, 2U);
        servo_enable_torque_paced(2U, 1U);
        servo_enable_torque_paced(3U, 1U);

        s_servo2_damping_state.released = false;
        s_servo2_damping_state.baseline_valid = false;
        s_servo2_damping_state.support_start_ms = now_ms;
        s_servo3_damping_state.released = false;
        s_servo3_damping_state.baseline_valid = false;
        s_servo3_damping_state.support_start_ms = now_ms;
        s_damping_pro_started = true;
        return;
    }

    damping_joint_update(2U, SERVO2_LOAD_THRESHOLD, &s_servo2_damping_state, now_ms);
    damping_joint_update(3U, SERVO3_LOAD_THRESHOLD, &s_servo3_damping_state, now_ms);
}

void Servo_DampingMode_ProStop(void)
{
    if (s_damping_pro_started)
    {
        /* 离开台灯模式前取消等待状态，保证收灯或观察者动作能够立即接管。 */
        servo_enable_torque_paced(2U, 1U);
        servo_enable_torque_paced(3U, 1U);
    }

    s_damping_pro_started = false;
    s_servo2_damping_state.released = false;
    s_servo2_damping_state.baseline_valid = false;
    s_servo3_damping_state.released = false;
    s_servo3_damping_state.baseline_valid = false;
}
//设置初始化 上电位置
void Servo_Power_on()
{
    //设置速度和加速度
    uint16_t speed[]={600,600,1000,600,600};
    uint8_t acce[]={5,5,10,5,5};
    //添加一个存舵机位置的数组Servo1 : 2084, Servo2 : 1795, Servo3 : 2603, Servo4: 1266, Servo5 : 2832
    int16_t servo_positions[] = {2084, 1795, 2603, 1266, 2832};
    uint8_t servo_id;

    /*
     * 进入台灯模式前刚刚恢复了位置模式和扭矩，若立即发送一次同步写，
     * 1～4号舵机可能尚未处理完前一条指令，从而一起漏掉展开命令。
     * 这里改为逐个发送并留出处理间隔；发送失败时只重试一次。
     */
    for (servo_id = 1U; servo_id <= 4U; servo_id++)
    {
        if (!Servo_WritePos(servo_id,
                            servo_positions[servo_id - 1U],
                            speed[servo_id - 1U],
                            acce[servo_id - 1U]))
        {
            R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
            (void) Servo_WritePos(servo_id,
                                  servo_positions[servo_id - 1U],
                                  speed[servo_id - 1U],
                                  acce[servo_id - 1U]);
        }

        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    /* 5号舵机最后抬头，保留原有机械展开顺序。 */
    R_BSP_SoftwareDelay(250, BSP_DELAY_UNITS_MILLISECONDS);
    if (!Servo_WritePos(5U, servo_positions[4], speed[4], acce[4]))
    {
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
        (void) Servo_WritePos(5U, servo_positions[4], speed[4], acce[4]);
    }
}
//结束位置，断电位置 
void Servo_Power_off()
{
    const uint8_t  ALL_IDS[]          = {1, 2, 3, 4, 5};
    const int16_t  FINAL_POS[]        = {2057, 1639, 3178, 1254, 3179};
    const uint16_t SPEED[]            = {400, 1000, 500, 1000, 1000};
    const uint8_t  ACC[]              = {5, 5, 10, 10, 10};

    /* 第一步：1/2/4/5 先收到中间位，给 3 号让路 */
    const uint8_t  STEP1_IDS[]        = {1, 2, 4, 5};
    const int16_t  STEP1_POS[]        = {2057, 1729, 1276, 2967};
    Servo_SyncWritePos(STEP1_IDS, sizeof(STEP1_IDS), STEP1_POS, SPEED, ACC);

    R_BSP_SoftwareDelay(500, BSP_DELAY_UNITS_MILLISECONDS);

    /* 第二步：全部舵机进终点位置 */
    Servo_SyncWritePos(ALL_IDS, sizeof(ALL_IDS), FINAL_POS, SPEED, ACC);
}
//这里我们开始编写舵机待机模式下的代码 该模式下台灯会观察四周 之后会配合摄像头如若识别到物体则产生中断
void Servo_StandbyMode()
{
    Servo_PrintStatus(load_arr, 5);//首先检查舵机是否都处于正常模式 有错误则进入死循环
    // 开启所有舵机扭矩
    Servo_NormalMode();
    Servo_Power_on();
    // ========== 舵机1：底座旋转，模拟身体转身张望 ==========
    const int servo1_center = 2084;  // 中心位置
    const int servo1_min    = 1750;  // 最左
    const int servo1_max    = 2350;  // 最右
    const int servo1_step   = 8;     // 每步移动量（较慢，模拟大范围缓慢转动）

    // ========== 舵机2：大臂/肩部，水平扫动 ==========
    const int servo2_center = 1500;
    const int servo2_min    = 1028;
    const int servo2_max    = 2200;
    const int servo2_step   = 15;

    // ========== 舵机3：小臂/肘部，安全范围内上下摆动 ==========
    const int servo3_center = SERVO3_ZERO;

    // ========== 舵机4：侧倾，模拟好奇歪头 ==========
    const int servo4_center = 1266;
    const int servo4_min    = 1166;
    const int servo4_max    = 1366;
    const int servo4_step   = 3;

    // ========== 舵机5：灯头，模拟点头/抬头观察 ==========
    const int servo5_center = 2832;
    const int servo5_min    = 2632;
    const int servo5_max    = 3000;
    const int servo5_step   = 5;

    int servo1_pos = servo1_center;
    int servo2_pos = servo2_center;
    int servo4_pos = servo4_center;
    int servo5_pos = servo5_center;

    int servo3_target;
    int safe_min, safe_max;
    int dir1 = 1, dir2 = 1, dir4 = 1, dir5 = 1;  // 各舵机独立方向
    int cycle = 0;  // 全局周期计数器，营造有机感

    printf("=== Standby Observer Mode (5-Axis) Start ===\r\n");

    while(1)
    {
        // 1. 计算当前2号舵机位置下3号舵机的安全范围
        calc_servo3_safe_range(&safe_min, &safe_max);

        // 2. 3号舵机目标：基于cycle做节拍切换，在安全范围内上下张望
        if ((cycle & 0x10) == 0)
            servo3_target = servo3_center + 150;
        else
            servo3_target = servo3_center - 150;

        // 确保3号舵机目标位置在安全范围内
        if (servo3_target < safe_min) servo3_target = safe_min;
        if (servo3_target > safe_max) servo3_target = safe_max;

        // 3. 同步写入1/2/4号舵机（提高效率）
        {
            uint8_t  ids[]       = {1, 2, 4};
            int16_t  positions[] = {servo1_pos, servo2_pos, servo4_pos};
            uint16_t speeds[]    = {100, 100, 60};
            uint8_t  accs[]      = {15, 15, 10};
            Servo_SyncWritePos(ids, 3, positions, speeds, accs);
        }
        R_BSP_SoftwareDelay(40, BSP_DELAY_UNITS_MILLISECONDS);

        // 4. 写入3号和5号舵机（独立的运动节奏）
        Servo_WritePos(3, servo3_target, 120, 15);
        Servo_WritePos(5, servo5_pos, 80, 12);
        R_BSP_SoftwareDelay(60, BSP_DELAY_UNITS_MILLISECONDS);

        // 5. 更新各舵机位置
        servo1_pos += servo1_step * dir1;
        servo2_pos += servo2_step * dir2;
        servo4_pos += servo4_step * dir4;
        servo5_pos += servo5_step * dir5;

        // 6. 各舵机到达边界时独立换向
        if (servo1_pos >= servo1_max || servo1_pos <= servo1_min) dir1 *= -1;
        if (servo2_pos >= servo2_max || servo2_pos <= servo2_min) dir2 *= -1;
        if (servo4_pos >= servo4_max || servo4_pos <= servo4_min) dir4 *= -1;
        if (servo5_pos >= servo5_max || servo5_pos <= servo5_min) dir5 *= -1;

        cycle++;

        // 7. 打印状态信息
        printf("S1:%4d S2:%4d S3:%4d S4:%4d S5:%4d Safe:[%4d,%4d]\r\n",
               servo1_pos, servo2_pos, servo3_target, servo4_pos, servo5_pos,
               safe_min, safe_max);
    }
}
