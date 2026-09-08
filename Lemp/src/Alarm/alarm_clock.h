#ifndef ALARM_ALARM_CLOCK_H_
#define ALARM_ALARM_CLOCK_H_

#include "Middlewares/lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool alarm_clock_init(void);
lv_obj_t * alarm_clock_screen_create(lv_event_cb_t back_event_cb);
void alarm_clock_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_ALARM_CLOCK_H_ */
