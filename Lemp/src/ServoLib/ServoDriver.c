#include <stddef.h>
#include <stdio.h>
#include <ServoLib/ServoDriver.h>
#include <Servo/servo.h>

#define SERVO_INST_PING (0x01U)                                       //0x01   PING（查询）	查询工作状态 参数长度为0
#define SERVO_INST_READ (0x02U)                                       //0x02   READ（查询）	查询指定寄存器的值 参数长度为2
#define SERVO_INST_WRITE (0x03U)                                      //0x03   WRITE（写入）	写入指定寄存器的值 参数长度不小于2
#define SERVO_INST_REG_WRITE (0x04U)                                  //0x04   REG_WRITE（写入寄存器）	写入指定寄存器的值 参数长度不小于4  类似于WRITE DATA，但是控制字符写入后并不马上动作，直到ACTION指令到达
#define SERVO_INST_REG_ACTION (0x05U)                                 //0x05   REG_ACTION    触发REG WRITE写入的动作 参数长度为0
#define SERVO_INST_SYNC_WRITE (0x83U)                                 //0x83   SYNC_WRITE  	用于同时控制多个舵机  参数长度不小于2

#define SERVO_Overload_torque (0x24U)                                 //0x24   OVERLOAD_TORQUE  超载保护 

#define SERVO_SMS_STS_MODE (33U)
#define SERVO_SMS_STS_TORQUE_ENABLE (40U)
#define SERVO_SMS_STS_ACC (41U)
#define SERVO_SMS_STS_GOAL_SPEED_L (46U)
#define SERVO_SMS_STS_LOCK (55U)
#define SERVO_SMS_STS_PRESENT_POSITION_L (56U)
#define SERVO_SMS_STS_PRESENT_POSITION_H (57U)
#define SERVO_SMS_STS_PRESENT_SPEED_L (58U)
#define SERVO_SMS_STS_PRESENT_SPEED_H (59U)
#define SERVO_SMS_STS_PRESENT_LOAD_L (60U)
#define SERVO_SMS_STS_PRESENT_LOAD_H (61U)
#define SERVO_SMS_STS_PRESENT_VOLTAGE (62U)
#define SERVO_SMS_STS_PRESENT_TEMPERATURE (63U)
#define SERVO_SMS_STS_MOVING (66U)
#define SERVO_SMS_STS_PRESENT_CURRENT_L (69U)
#define SERVO_SMS_STS_PRESENT_CURRENT_H (70U)
#define SERVO_SMS_STS_ASYNC_WRITE (64U)
#define SERVO_SMS_STS_STATUS (65U)
#define SERVO_SMS_STS_TARGET_POSITION_L (67U)
#define SERVO_SMS_STS_TARGET_POSITION_H (68U)

#define SERVO_STATUS_VOLTAGE  (1U << 0)
#define SERVO_STATUS_ENCODER  (1U << 1)
#define SERVO_STATUS_TEMP     (1U << 2)
#define SERVO_STATUS_CURRENT  (1U << 3)
#define SERVO_STATUS_LOAD     (1U << 5)

#define SERVO_RX_BUFFER_SIZE (256U)
#define SERVO_SYNC_MAX_IDS (32U)
#define SERVO_STATUS_RETURN_LEVEL (1U)
#define SERVO_REQUIRE_WRITE_ACK (0U)
#define SERVO_POLL_DELAY_US (10U)
#define SERVO_TX_ECHO_SETTLE_US (20U)
#define SERVO_RESPONSE_TIMEOUT_US (20000U)
#define SERVO_TX_TIMEOUT_BASE_US (5000U)
#define SERVO_PACKET_BUFFER_SIZE (32U)
#define SERVO_SYNC_PACKET_BUFFER_SIZE (266U)


static volatile uint8_t g_servo_rx_buffer[SERVO_RX_BUFFER_SIZE];
static volatile uint16_t g_servo_rx_head = 0U;
static volatile uint16_t g_servo_rx_tail = 0U;
static volatile bool g_servo_tx_complete = true;
static bool g_servo_opened = false;
static int g_servo_last_error = 0;
static uint8_t g_servo_feedback[SERVO_SMS_STS_PRESENT_CURRENT_H - SERVO_SMS_STS_PRESENT_POSITION_L + 1U];

static uint16_t servo_rx_next_index(uint16_t index)
{
    return (uint16_t) ((index + 1U) % SERVO_RX_BUFFER_SIZE);
}

static void servo_delay_us(uint32_t delay_us)
{
    if (delay_us > 0U)
    {
        R_BSP_SoftwareDelay(delay_us, BSP_DELAY_UNITS_MICROSECONDS);
    }
}

static void servo_rx_flush(void)
{
    g_servo_rx_head = 0U;
    g_servo_rx_tail = 0U;
}

static void servo_rx_push(uint8_t data)
{
    uint16_t next = servo_rx_next_index(g_servo_rx_tail);

    if (next == g_servo_rx_head)
    {
        g_servo_rx_head = servo_rx_next_index(g_servo_rx_head);
    }

    g_servo_rx_buffer[g_servo_rx_tail] = data;
    g_servo_rx_tail = next;
}

static bool servo_rx_pop(uint8_t * p_data)
{
    if ((NULL == p_data) || (g_servo_rx_head == g_servo_rx_tail))
    {
        return false;
    }

    *p_data = g_servo_rx_buffer[g_servo_rx_head];
    g_servo_rx_head = servo_rx_next_index(g_servo_rx_head);
    return true;
}

static bool servo_wait_byte(uint8_t * p_data, uint32_t timeout_us)
{
    uint32_t waited_us = 0U;

    while (waited_us <= timeout_us)
    {
        if (servo_rx_pop(p_data))
        {
            return true;
        }

        servo_delay_us(SERVO_POLL_DELAY_US);
        waited_us += SERVO_POLL_DELAY_US;
    }

    return false;
}

static int servo_read_exact(uint8_t * p_data, int length, uint32_t timeout_us)
{
    int index;

    if (NULL == p_data)
    {
        return 0;
    }

    for (index = 0; index < length; index++)
    {
        if (!servo_wait_byte(&p_data[index], timeout_us))
        {
            return index;
        }
    }

    return length;
}

static bool servo_wait_tx_complete(uint32_t timeout_us)
{
    uint32_t waited_us = 0U;

    while ((!g_servo_tx_complete) && (waited_us <= timeout_us))
    {
        servo_delay_us(SERVO_POLL_DELAY_US);
        waited_us += SERVO_POLL_DELAY_US;
    }
    return g_servo_tx_complete;
}

static void servo_host_to_sc(uint8_t * data_l, uint8_t * data_h, int data)
{
    *data_h = (uint8_t) ((data >> 8) & 0xFF);
    *data_l = (uint8_t) (data & 0xFF);
}

static int servo_sc_to_host(uint8_t data_l, uint8_t data_h)
{
    return (((int) data_h) << 8) | data_l;
}

static fsp_err_t servo_uart_write(uint8_t const * p_data, uint32_t length)
{
    fsp_err_t err;

    if (!g_servo_opened)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((NULL == p_data) || (0U == length))
    {
        return FSP_ERR_ASSERTION;
    }

    servo_rx_flush();
    g_servo_last_error = 0;
    g_servo_tx_complete = false;

    err = g_uart0.p_api->write(g_uart0.p_ctrl, p_data, length);
    if (FSP_SUCCESS != err)
    {
        g_servo_tx_complete = true;
        return err;
    }

    if (!servo_wait_tx_complete(SERVO_TX_TIMEOUT_BASE_US + (length * 20U)))
    {
        (void) g_uart0.p_api->communicationAbort(g_uart0.p_ctrl, UART_DIR_TX);
        g_servo_tx_complete = true;
        return FSP_ERR_TIMEOUT;
    }

    /*
     * SCI_B UART does not implement receiveSuspend/receiveResume.  The
     * half-duplex bus therefore places the transmitted packet back into the
     * RX callback buffer.  Wait for the final echoed byte, then discard only
     * that request before the servo's delayed status packet arrives.
     */
    servo_delay_us(SERVO_TX_ECHO_SETTLE_US);
    servo_rx_flush();
    return FSP_SUCCESS;
}
//核心                                        舵机id         目标指令地址位                参数              长度           写还是读
static fsp_err_t servo_send_instruction(uint8_t id, uint8_t mem_addr, uint8_t const * p_data, uint8_t length, uint8_t instruction)
{
    uint8_t packet[SERVO_PACKET_BUFFER_SIZE];
    uint8_t packet_length = 0U;
    uint8_t message_length = 2U;//2是id和instruction
    uint8_t checksum;
    uint8_t index;

    packet[packet_length++] = 0xFFU;
    packet[packet_length++] = 0xFFU;
    packet[packet_length++] = id;

    if (NULL != p_data)
    {
        message_length = (uint8_t) (message_length + length + 1U);
    }

    packet[packet_length++] = message_length;
    packet[packet_length++] = instruction;
    checksum = (uint8_t) (id + message_length + instruction);

    if (NULL != p_data)
    {
        packet[packet_length++] = mem_addr;
        checksum = (uint8_t) (checksum + mem_addr);
        for (index = 0U; index < length; index++)
        {
            packet[packet_length++] = p_data[index];
            checksum = (uint8_t) (checksum + p_data[index]);
        }
    }

    packet[packet_length++] = (uint8_t) (~checksum);//
    return servo_uart_write(packet, packet_length);
}

static int servo_check_head(void)
{
    uint8_t data;
    uint8_t buffer[2] = {0U, 0U};
    uint8_t count = 0U;

    while (1)
    {
        if (!servo_wait_byte(&data, SERVO_RESPONSE_TIMEOUT_US))
        {
            g_servo_last_error = 1;
            return 0;
        }

        buffer[1] = buffer[0];
        buffer[0] = data;
        if ((0xFFU == buffer[0]) && (0xFFU == buffer[1]))
        {
            break;
        }

        count++;
        if (count > 10U)
        {
            g_servo_last_error = 1;
            return 0;
        }
    }

    return 1;
}

static int servo_ack(uint8_t id)
{
    uint8_t buffer[4];
    uint8_t checksum;

    if ((SERVO_STS_BROADCAST_ID != id) && (0U != SERVO_STATUS_RETURN_LEVEL))
    {
        if (!servo_check_head())
        {
            return 0;
        }

        if (4 != servo_read_exact(buffer, 4, SERVO_RESPONSE_TIMEOUT_US))
        {
            g_servo_last_error = 1;
            return 0;
        }

        if (buffer[0] != id)
        {
            g_servo_last_error = 1;
            return 0;
        }

        if (2U != buffer[1])
        {
            g_servo_last_error = 1;
            return 0;
        }

        checksum = (uint8_t) (~(buffer[0] + buffer[1] + buffer[2]));
        if (checksum != buffer[3])
        {
            g_servo_last_error = 1;
            return 0;
        }
    }

    g_servo_last_error = 0;
    return 1;
}

static int servo_gen_write(uint8_t id, uint8_t mem_addr, uint8_t const * p_data, uint8_t length)
{
    if (FSP_SUCCESS != servo_send_instruction(id, mem_addr, p_data, length, SERVO_INST_WRITE))
    {
        g_servo_last_error = 1;
        return 0;
    }

#if SERVO_REQUIRE_WRITE_ACK
    return servo_ack(id);
#else
    FSP_PARAMETER_NOT_USED(id);
    g_servo_last_error = 0;
    return 1;
#endif
}

static int servo_reg_write(uint8_t id, uint8_t mem_addr, uint8_t const * p_data, uint8_t length)
{
    if (FSP_SUCCESS != servo_send_instruction(id, mem_addr, p_data, length, SERVO_INST_REG_WRITE))
    {
        g_servo_last_error = 1;
        return 0;
    }

#if SERVO_REQUIRE_WRITE_ACK
    return servo_ack(id);
#else
    FSP_PARAMETER_NOT_USED(id);
    g_servo_last_error = 0;
    return 1;
#endif
}

static int servo_reg_action(uint8_t id)
{
    if (FSP_SUCCESS != servo_send_instruction(id, 0U, NULL, 0U, SERVO_INST_REG_ACTION))
    {
        g_servo_last_error = 1;
        return 0;
    }

#if SERVO_REQUIRE_WRITE_ACK
    return servo_ack(id);
#else
    g_servo_last_error = 0;
    return 1;
#endif
}

static int servo_write_byte(uint8_t id, uint8_t mem_addr, uint8_t value)
{
    return servo_gen_write(id, mem_addr, &value, 1U);
}

static int servo_read(uint8_t id, uint8_t mem_addr, uint8_t * p_data, uint8_t length)
{
    int size;
    uint8_t buffer[4];
    uint8_t checksum;
    uint8_t index;

    if ((NULL == p_data) || (0U == length))
    {
        g_servo_last_error = 1;
        return 0;
    }

    if (FSP_SUCCESS != servo_send_instruction(id, mem_addr, &length, 1U, SERVO_INST_READ))
    {
        g_servo_last_error = 1;
        return 0;
    }

    if (!servo_check_head())
    {
        return 0;
    }

    if (3 != servo_read_exact(buffer, 3, SERVO_RESPONSE_TIMEOUT_US))
    {
        g_servo_last_error = 1;
        return 0;
    }

    if ((buffer[0] != id) || (buffer[1] != (uint8_t) (length + 2U)))
    {
        g_servo_last_error = 1;
        return 0;
    }

    size = servo_read_exact(p_data, length, SERVO_RESPONSE_TIMEOUT_US);
    if (size != length)
    {
        g_servo_last_error = 1;
        return 0;
    }

    if (1 != servo_read_exact(&buffer[3], 1, SERVO_RESPONSE_TIMEOUT_US))
    {
        g_servo_last_error = 1;
        return 0;
    }

    checksum = (uint8_t) (buffer[0] + buffer[1] + buffer[2]);
    for (index = 0U; index < (uint8_t) size; index++)
    {
        checksum = (uint8_t) (checksum + p_data[index]);
    }

    checksum = (uint8_t) (~checksum);
    if (checksum != buffer[3])
    {
        g_servo_last_error = 1;
        return 0;
    }

    g_servo_last_error = 0;
    return size;
}

static int servo_read_byte(uint8_t id, uint8_t mem_addr)
{
    uint8_t value;
    int size = servo_read(id, mem_addr, &value, 1U);

    if (1 != size)
    {
        return -1;
    }

    return value;
}

static int servo_read_word(uint8_t id, uint8_t mem_addr)
{
    uint8_t buffer[2];
    int size = servo_read(id, mem_addr, buffer, 2U);

    if (2 != size)
    {
        return -1;
    }

    return servo_sc_to_host(buffer[0], buffer[1]);
}

// 初始化舵机通信串口与驱动状态。
void Servo_Init(void)
{
    fsp_err_t err;

    if (g_servo_opened)
    {
        return;
    }

    err = g_uart0.p_api->open(g_uart0.p_ctrl, g_uart0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        printf("Servo_Init failed: %d\r\n", err);
        while (1)
        {
        }
    }
    servo_rx_flush();
    g_servo_tx_complete = true;
    g_servo_last_error = 0;
    g_servo_opened = true;
}

// 关闭舵机通信串口并清理驱动状态。
fsp_err_t Servo_Deinit(void)
{
    fsp_err_t err;

    if (!g_servo_opened)
    {
        return FSP_SUCCESS;
    }

    err = g_uart0.p_api->close(g_uart0.p_ctrl);
    if (FSP_SUCCESS == err)
    {
        g_servo_opened = false;
        servo_rx_flush();
        g_servo_tx_complete = true;
    }

    return err;
}

// 返回舵机驱动是否已经完成初始化。
bool Servo_IsOpened(void)
{
    return g_servo_opened;
}

// 返回最近一次舵机读写操作的错误状态。
int Servo_GetLastError(void)
{
    return g_servo_last_error;
}

// 发送 Ping 指令并检查指定 ID 的舵机是否在线。
int Servo_Ping(uint8_t id)
{
    uint8_t buffer[4];
    uint8_t checksum;

    if (FSP_SUCCESS != servo_send_instruction(id, 0U, NULL, 0U, SERVO_INST_PING))
    {
        g_servo_last_error = 1;
        return -1;
    }

    if (!servo_check_head())
    {
        return -1;
    }

    if (4 != servo_read_exact(buffer, 4, SERVO_RESPONSE_TIMEOUT_US))
    {
        g_servo_last_error = 1;
        return -1;
    }

    if ((buffer[0] != id) && (SERVO_STS_BROADCAST_ID != id))
    {
        g_servo_last_error = 1;
        return -1;
    }

    if (2U != buffer[1])
    {
        g_servo_last_error = 1;
        return -1;
    }

    checksum = (uint8_t) (~(buffer[0] + buffer[1] + buffer[2]));
    if (checksum != buffer[3])
    {
        g_servo_last_error = 1;
        return -1;
    }

    g_servo_last_error = 0;
    return buffer[0];
}

// 按给定位置、速度和加速度立即驱动舵机运动。
int Servo_WritePos(uint8_t id, int16_t position, uint16_t speed, uint8_t acc)
{
    uint8_t buffer[7];
    int position_value = position;

    if (position_value < 0)
    {
        position_value = -position_value;
        position_value |= (1 << 15);
    }

    buffer[0] = acc;
    servo_host_to_sc(buffer + 1, buffer + 2, position_value);
    servo_host_to_sc(buffer + 3, buffer + 4, 0);
    servo_host_to_sc(buffer + 5, buffer + 6, speed);

    return servo_gen_write(id, SERVO_SMS_STS_ACC, buffer, 7U);
}

// 预写入位置、速度和加速度参数，等待统一触发执行。
int Servo_RegWritePos(uint8_t id, int16_t position, uint16_t speed, uint8_t acc)
{
    uint8_t buffer[7];
    int position_value = position;

    if (position_value < 0)
    {
        position_value = -position_value;
        position_value |= (1 << 15);
    }

    buffer[0] = acc;
    servo_host_to_sc(buffer + 1, buffer + 2, position_value);
    servo_host_to_sc(buffer + 3, buffer + 4, 0);
    servo_host_to_sc(buffer + 5, buffer + 6, speed);

    return servo_reg_write(id, SERVO_SMS_STS_ACC, buffer, 7U);
}

// 触发已经通过寄存器预写入的动作命令。
void Servo_RegWriteAction(void)
{
    (void) servo_reg_action(SERVO_STS_BROADCAST_ID);
}

// 同时向多个舵机下发位置、速度和加速度命令。
int Servo_SyncWritePos(const uint8_t id[], uint8_t id_count, const int16_t position[], const uint16_t speed[], const uint8_t acc[])
{
    uint8_t packet[SERVO_SYNC_PACKET_BUFFER_SIZE];
    uint16_t packet_length = 0U;
    uint8_t message_length;
    uint8_t checksum;
    uint8_t index;
    uint8_t inner;

    if ((NULL == id) || (NULL == position) || (0U == id_count) || (id_count > SERVO_SYNC_MAX_IDS))
    {
        g_servo_last_error = 1;
        return 0;
    }

    packet[packet_length++] = 0xFFU;
    packet[packet_length++] = 0xFFU;
    packet[packet_length++] = SERVO_STS_BROADCAST_ID;
    message_length = (uint8_t) (((7U + 1U) * id_count) + 4U);
    packet[packet_length++] = message_length;
    packet[packet_length++] = SERVO_INST_SYNC_WRITE;
    packet[packet_length++] = SERVO_SMS_STS_ACC;
    packet[packet_length++] = 7U;
    checksum = (uint8_t) (SERVO_STS_BROADCAST_ID + message_length + SERVO_INST_SYNC_WRITE + SERVO_SMS_STS_ACC + 7U);

    for (index = 0U; index < id_count; index++)
    {
        int position_value = position[index];
        uint16_t speed_value = (NULL != speed) ? speed[index] : 0U;
        uint8_t acc_value = (NULL != acc) ? acc[index] : 0U;
        uint8_t payload[7];

        if (position_value < 0)
        {
            position_value = -position_value;
            position_value |= (1 << 15);
        }

        payload[0] = acc_value;
        servo_host_to_sc(payload + 1, payload + 2, position_value);
        servo_host_to_sc(payload + 3, payload + 4, 0);
        servo_host_to_sc(payload + 5, payload + 6, speed_value);

        packet[packet_length++] = id[index];
        checksum = (uint8_t) (checksum + id[index]);
        for (inner = 0U; inner < 7U; inner++)
        {
            packet[packet_length++] = payload[inner];
            checksum = (uint8_t) (checksum + payload[inner]);
        }
    }

    packet[packet_length++] = (uint8_t) (~checksum);

    if (FSP_SUCCESS != servo_uart_write(packet, packet_length))
    {
        g_servo_last_error = 1;
        return 0;
    }

    g_servo_last_error = 0;
    return 1;
}

// 将舵机切换到位置模式，目标位置会限制舵机的转动范围。
int Servo_PositionMode(uint8_t id)
{
    return servo_write_byte(id, SERVO_SMS_STS_MODE, 0U);
}

// 将舵机切换到轮式连续转动模式。
int Servo_WheelMode(uint8_t id)
{
    return servo_write_byte(id, SERVO_SMS_STS_MODE, 1U);
}

// 在轮式模式下设置舵机转速和加速度。
int Servo_WriteSpeed(uint8_t id, int16_t speed, uint8_t acc)
{
    uint8_t buffer[2];
    int speed_value = speed;

    if (speed_value < 0)
    {
        speed_value = -speed_value;
        speed_value |= (1 << 15);
    }

    buffer[0] = acc;
    if (!servo_gen_write(id, SERVO_SMS_STS_ACC, buffer, 1U))
    {
        return 0;
    }

    servo_host_to_sc(&buffer[0], &buffer[1], speed_value);
    return servo_gen_write(id, SERVO_SMS_STS_GOAL_SPEED_L, buffer, 2U);
}

// 使能或关闭舵机输出力矩。
int Servo_EnableTorque(uint8_t id, uint8_t enable)
{
    return servo_write_byte(id, SERVO_SMS_STS_TORQUE_ENABLE, enable);
}

// 解锁 EEPROM，允许修改相关配置参数。
int Servo_UnlockEprom(uint8_t id)
{
    return servo_write_byte(id, SERVO_SMS_STS_LOCK, 0U);
}

// 锁定 EEPROM，防止配置参数被继续改写。
int Servo_LockEprom(uint8_t id)
{
    return servo_write_byte(id, SERVO_SMS_STS_LOCK, 1U);
}

// 触发舵机偏差校准功能。
int Servo_CalibrationOffset(uint8_t id)
{
    return servo_write_byte(id, SERVO_SMS_STS_TORQUE_ENABLE, 128U);
}

// 一次性读取舵机运行反馈参数块，并缓存供后续 Servo_ReadXxx(-1) 使用。
int Servo_Feedback(int id)
{
    int length = servo_read((uint8_t) id, SERVO_SMS_STS_PRESENT_POSITION_L, g_servo_feedback, (uint8_t) sizeof(g_servo_feedback));

    if (length != (int) sizeof(g_servo_feedback))
    {
        g_servo_last_error = 1;
        return -1;
    }

    g_servo_last_error = 0;
    return length;
}

// 读取当前位置，id 为 -1 时从反馈缓存中取值。
int Servo_ReadPosition(int id)
{
    int position = -1;

    if (-1 == id)
    {
        position = g_servo_feedback[SERVO_SMS_STS_PRESENT_POSITION_H - SERVO_SMS_STS_PRESENT_POSITION_L];
        position <<= 8;
        position |= g_servo_feedback[0];
    }
    else
    {
        g_servo_last_error = 0;
        position = servo_read_word((uint8_t) id, SERVO_SMS_STS_PRESENT_POSITION_L);
        if (-1 == position)
        {
            g_servo_last_error = 1;
        }
    }

    if ((0 == g_servo_last_error) && (0 != (position & (1 << 15))))
    {
        position = -(position & ~(1 << 15));
    }

    return position;
}

// 读取当前速度，id 为 -1 时从反馈缓存中取值。
int Servo_ReadSpeed(int id)
{
    int speed = -1;

    if (-1 == id)
    {
        speed = g_servo_feedback[SERVO_SMS_STS_PRESENT_SPEED_H - SERVO_SMS_STS_PRESENT_POSITION_L];
        speed <<= 8;
        speed |= g_servo_feedback[SERVO_SMS_STS_PRESENT_SPEED_L - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        speed = servo_read_word((uint8_t) id, SERVO_SMS_STS_PRESENT_SPEED_L);
        if (-1 == speed)
        {
            g_servo_last_error = 1;
            return -1;
        }
    }

    if ((0 == g_servo_last_error) && (0 != (speed & (1 << 15))))
    {
        speed = -(speed & ~(1 << 15));
    }

    return speed;
}

// 读取当前负载，id 为 -1 时从反馈缓存中取值。
int Servo_ReadLoad(int id)
{
    int load = -1;

    if (-1 == id)
    {
        load = g_servo_feedback[SERVO_SMS_STS_PRESENT_LOAD_H - SERVO_SMS_STS_PRESENT_POSITION_L];
        load <<= 8;
        load |= g_servo_feedback[SERVO_SMS_STS_PRESENT_LOAD_L - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        load = servo_read_word((uint8_t) id, SERVO_SMS_STS_PRESENT_LOAD_L);
        if (-1 == load)
        {
            g_servo_last_error = 1;
        }
    }

    if ((0 == g_servo_last_error) && (0 != (load & (1 << 10))))
    {
        load = -(load & ~(1 << 10));
    }

    return load;
}

// 读取当前电压，id 为 -1 时从反馈缓存中取值。
int Servo_ReadVoltage(int id)
{
    int voltage = -1;

    if (-1 == id)
    {
        voltage = g_servo_feedback[SERVO_SMS_STS_PRESENT_VOLTAGE - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        voltage = servo_read_byte((uint8_t) id, SERVO_SMS_STS_PRESENT_VOLTAGE);
        if (-1 == voltage)
        {
            g_servo_last_error = 1;
        }
    }

    return voltage;
}

// 读取当前温度，id 为 -1 时从反馈缓存中取值。
int Servo_ReadTemperature(int id)
{
    int temperature = -1;

    if (-1 == id)
    {
        temperature = g_servo_feedback[SERVO_SMS_STS_PRESENT_TEMPERATURE - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        temperature = servo_read_byte((uint8_t) id, SERVO_SMS_STS_PRESENT_TEMPERATURE);
        if (-1 == temperature)
        {
            g_servo_last_error = 1;
        }
    }

    return temperature;
}

// 读取当前运动状态，id 为 -1 时从反馈缓存中取值。
int Servo_ReadMoving(int id)
{
    int moving = -1;

    if (-1 == id)
    {
        moving = g_servo_feedback[SERVO_SMS_STS_MOVING - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        moving = servo_read_byte((uint8_t) id, SERVO_SMS_STS_MOVING);
        if (-1 == moving)
        {
            g_servo_last_error = 1;
        }
    }

    return moving;
}

// 读取当前电流，id 为 -1 时从反馈缓存中取值。
int Servo_ReadCurrent(int id)
{
    int current = -1;

    if (-1 == id)
    {
        current = g_servo_feedback[SERVO_SMS_STS_PRESENT_CURRENT_H - SERVO_SMS_STS_PRESENT_POSITION_L];
        current <<= 8;
        current |= g_servo_feedback[SERVO_SMS_STS_PRESENT_CURRENT_L - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        current = servo_read_word((uint8_t) id, SERVO_SMS_STS_PRESENT_CURRENT_L);
        if (-1 == current)
        {
            g_servo_last_error = 1;
            return -1;
        }
    }

    if ((0 == g_servo_last_error) && (0 != (current & (1 << 15))))
    {
        current = -(current & ~(1 << 15));
    }

    return current;
}

int Servo_ReadStatus(int id)
{
    int status = -1;

    if (-1 == id)
    {
        status = g_servo_feedback[SERVO_SMS_STS_STATUS - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        status = servo_read_byte((uint8_t) id, SERVO_SMS_STS_STATUS);
        if (-1 == status)
        {
            g_servo_last_error = 1;
        }
    }

    return status;
}
//过载检测
void Servo_PrintStatus(uint8_t load_arr[],uint8_t arr_len)
{
    uint8_t has_fault = 0;   // 全局异常标志：1=存在异常，0=全部正常
    for(int i=0;i<arr_len;i++)
    {
    int id = load_arr[i];
    int status = Servo_ReadStatus(id);
    // if (status < 0)   // 通信偶发失败，重试一次  我觉得没必要测试这个
    // {
    //     R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
    //     status = Servo_ReadStatus(id);
    // }
    // if (status < 0)
    // {
    //     printf("[Servo %d] communication failed!\r\n", id);
    //     continue;
    // }
    if (0 !=status)
    {
        has_fault = 1;
        printf("[Servo %d] Abnormal\r\n", id);
    if (status & SERVO_STATUS_VOLTAGE)  {printf(" Voltage anomaly\r\n"); }
    if (status & SERVO_STATUS_ENCODER)  {printf(" Encoder anomaly\r\n"); }
    if (status & SERVO_STATUS_TEMP)     {printf(" Temperature anomaly\r\n"); }
    if (status & SERVO_STATUS_CURRENT)  {printf(" I anomaly\r\n"); }
    if (status & SERVO_STATUS_LOAD)
    {
        printf("Load anomaly and protect the servo and disable the torque\r\n");   
        Servo_DampingMode();//进入阻尼模式 防止重重的砸下来
    }
    }
    }
    if (has_fault)
    {
        printf("One of the servos has a problem and has stopped operating.\r\n");
    }
}

int Servo_ReadTargetPosition(int id)
{
    int position = -1;

    if (-1 == id)
    {
        position = g_servo_feedback[SERVO_SMS_STS_TARGET_POSITION_H - SERVO_SMS_STS_PRESENT_POSITION_L];
        position <<= 8;
        position |= g_servo_feedback[SERVO_SMS_STS_TARGET_POSITION_L - SERVO_SMS_STS_PRESENT_POSITION_L];
    }
    else
    {
        g_servo_last_error = 0;
        position = servo_read_word((uint8_t) id, SERVO_SMS_STS_TARGET_POSITION_L);
        if (-1 == position)
        {
            g_servo_last_error = 1;
            return -1;
        }
    }

    if ((0 == g_servo_last_error) && (0 != (position & (1 << 15))))
    {
        position = -(position & ~(1 << 15));
    }

    return position;
}

void g_uart0_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        servo_rx_push((uint8_t) p_args->data);
    }
    else if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        g_servo_tx_complete = true;
    }
    else if ((UART_EVENT_ERR_PARITY == p_args->event) ||
             (UART_EVENT_ERR_FRAMING == p_args->event) ||
             (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
             (UART_EVENT_BREAK_DETECT == p_args->event))
    {
        g_servo_last_error = (int) p_args->event;
    }
}
//我们写一个设置舵机过载扭矩的函数  我们发送的数据帧是ff ff 02 04 03 24 0A c8
void Servo_SetLoadTorque(uint8_t id,uint8_t torque)
{
   servo_send_instruction(id, SERVO_Overload_torque,&torque,1,SERVO_INST_WRITE);
}
