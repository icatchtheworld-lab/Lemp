/*
 * time.h
 *
 *  Created on: 2026年6月25日
 *      Author: 36315
 */

#ifndef TIME_TIME_H_
#define TIME_TIME_H_

#include "hal_data.h"

bool time_start(void);
bool time_init(void);
bool time_set(rtc_time_t *p_time);
bool time_get(rtc_time_t *p_time);
bool time_is_running(void);

#endif /* TIME_TIME_H_ */
