#ifndef KEYWORD_KEYWORD_DETECTOR_H_
#define KEYWORD_KEYWORD_DETECTOR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYWORD_DETECTOR_SAMPLE_RATE_HZ       (16000U)
#define KEYWORD_DETECTOR_CLASS_COUNT          (5U)

typedef enum e_keyword_detector_class
{
    KEYWORD_DETECTOR_CLASS_NOISE = 0,
    KEYWORD_DETECTOR_CLASS_OFF = 1,
    KEYWORD_DETECTOR_CLASS_ON = 2,
    KEYWORD_DETECTOR_CLASS_TIME = 3,
    KEYWORD_DETECTOR_CLASS_WEATHER = 4,
} keyword_detector_class_t;

typedef struct st_keyword_detector_result
{
    keyword_detector_class_t class_id;
    uint16_t confidence_permille;
    uint32_t inference_count;
    uint32_t event_sequence;
} keyword_detector_result_t;

bool keyword_detector_init(void);
void keyword_detector_deinit(void);
void keyword_detector_reset(void);
bool keyword_detector_poll(void);
bool keyword_detector_get_latest(keyword_detector_result_t * result);
bool keyword_detector_take_event(keyword_detector_result_t * result);
char const * keyword_detector_class_name(keyword_detector_class_t class_id);

#ifdef __cplusplus
}
#endif

#endif /* KEYWORD_KEYWORD_DETECTOR_H_ */
