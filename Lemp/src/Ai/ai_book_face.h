#ifndef AI_BOOK_FACE_H
#define AI_BOOK_FACE_H

#include <stdbool.h>
#include <stdint.h>

#define AI_BOOK_FACE_CLASS_CLOSED_BOOK     (0U)
#define AI_BOOK_FACE_CLASS_OPEN_BOOK       (1U)
#define AI_BOOK_FACE_CLASS_FACE            (2U)
#define AI_BOOK_FACE_CLASS_COUNT           (3U)
#define AI_BOOK_FACE_MAX_DETECTIONS        (8U)

typedef struct st_ai_book_face_detection
{
    uint8_t class_id;
    uint32_t frame_id;
    int score_permille;
    int x_min;
    int y_min;
    int x_max;
    int y_max;
    int center_x;
    int center_y;
} ai_book_face_detection_t;

bool ai_book_face_init(void);
bool ai_book_face_poll(void);
void ai_book_face_set_reporting_enabled(bool enabled);
/** 暂停或恢复CEU取帧；关闭时不会重新配置或断电摄像头芯片。 */
bool ai_book_face_set_camera_enabled(bool enabled);
uint32_t ai_book_face_get_detections(ai_book_face_detection_t * detections, uint32_t capacity);
bool ai_book_face_get_best_detection(uint8_t class_id, ai_book_face_detection_t * detection);
bool ai_book_face_get_latest_frame(uint16_t const ** frame, uint32_t * frame_id);
char const * ai_book_face_class_name(uint8_t class_id);

#endif
