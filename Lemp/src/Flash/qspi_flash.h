/*
 * qspi_flash.h
 *
 *  Created on: 2026年7月23日
 *      Author: 36315
 */

#ifndef FLASH_QSPI_FLASH_H_
#define FLASH_QSPI_FLASH_H_

#include "hal_data.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
   uint8_t hour;
   uint8_t minute;
   uint8_t enable;
   uint8_t repeat_mask;
} alarm_setting_t;

#define ALARM_SETTING_MAX_COUNT (4U)

fsp_err_t qspi_flash_init(void);
fsp_err_t qspi_flash_wait_ready(void);
fsp_err_t alarm_settings_save(const alarm_setting_t * settings, uint8_t count);
bool alarm_settings_load(alarm_setting_t * settings, uint8_t capacity, uint8_t * count);

/* Compatibility helpers for code that stores only one alarm. */
fsp_err_t alarm_setting_save(const alarm_setting_t *setting);
bool alarm_setting_load(alarm_setting_t *setting);
#endif /* FLASH_QSPI_FLASH_H_ */
