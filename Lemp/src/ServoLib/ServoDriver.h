/*
 * Servo.h
 *
 *  Created on: 2026年5月26日
 *      Author: 36315
 */

#ifndef SERVOLIB_SERVODRIVER_H_
#define SERVOLIB_SERVODRIVER_H_



#include <stdbool.h>
#include <stdint.h>
#include "hal_data.h"

#define SERVO_STS_BROADCAST_ID (0xFEU)
#define SERVO_STS_POSITION_MIN (0)
#define SERVO_STS_POSITION_MAX (4095)

#define SERVO_STATUS_VOLTAGE  (1U << 0)
#define SERVO_STATUS_ENCODER  (1U << 1)
#define SERVO_STATUS_TEMP     (1U << 2)
#define SERVO_STATUS_CURRENT  (1U << 3)
#define SERVO_STATUS_LOAD     (1U << 5)

void Servo_Init(void);
fsp_err_t Servo_Deinit(void);
bool Servo_IsOpened(void);
int Servo_Ping(uint8_t id);
int Servo_WritePos(uint8_t id, int16_t position, uint16_t speed, uint8_t acc);
int Servo_RegWritePos(uint8_t id, int16_t position, uint16_t speed, uint8_t acc);
void Servo_RegWriteAction(void);
int Servo_SyncWritePos(const uint8_t id[], uint8_t id_count, const int16_t position[], const uint16_t speed[], const uint8_t acc[]);
int Servo_PositionMode(uint8_t id);
int Servo_WheelMode(uint8_t id);
int Servo_WriteSpeed(uint8_t id, int16_t speed, uint8_t acc);
int Servo_EnableTorque(uint8_t id, uint8_t enable);
int Servo_UnlockEprom(uint8_t id);
int Servo_LockEprom(uint8_t id);
int Servo_CalibrationOffset(uint8_t id);
int Servo_Feedback(int id);
int Servo_ReadPosition(int id);
int Servo_ReadSpeed(int id);
int Servo_ReadLoad(int id);
int Servo_ReadVoltage(int id);
int Servo_ReadTemperature(int id);
int Servo_ReadMoving(int id);
int Servo_ReadCurrent(int id);  
int Servo_ReadStatus(int id);
void Servo_PrintStatus(uint8_t load_arr[],uint8_t arr_len);
int Servo_ReadTargetPosition(int id);
int Servo_GetLastError(void);

void Servo_SetLoadTorque(uint8_t id,uint8_t torque);
#endif /* SERVOLIB_SERVODRIVER_H_ */
