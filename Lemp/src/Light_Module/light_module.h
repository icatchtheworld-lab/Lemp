/*
 * light_module.h
 *
 *  Created on: 2026年7月25日
 *      Author: 36315
 */

#ifndef LIGHT_MODULE_LIGHT_MODULE_H_
#define LIGHT_MODULE_LIGHT_MODULE_H_

#include "hal_data.h"

/**
 * @brief 初始化光敏模块使用的ADC。
 *
 * @return FSP_SUCCESS表示初始化成功，其他值表示对应的FSP错误。
 */
fsp_err_t light_module_init(void);

/*
 * @brief 触发一次光敏电阻的采样，读取12位原始数据
 *
 * @param[out] p_value 用于保存ADC原始值，正常范围是0~4095
 *
 */
fsp_err_t light_module_read_raw(uint16_t * p_value);

/**
 * @brief 初始化灯泡使用的GPT3 PWM，并以0%占空比启动。
 *
 * @return FSP_SUCCESS表示初始化成功，其他值表示对应的FSP错误。
 */
fsp_err_t light_bulb_init(void);

/**
 * @brief 设置灯泡PWM亮度。
 *
 * @param[in] brightness_percent 亮度百分比，范围为0～100。
 *
 * @return FSP_SUCCESS表示设置成功，其他值表示对应的FSP错误。
 */
fsp_err_t light_bulb_set_brightness(uint8_t brightness_percent);

#endif /* LIGHT_MODULE_LIGHT_MODULE_H_ */
