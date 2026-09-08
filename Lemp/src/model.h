#ifndef BOOK_FACE_V5_MODEL_H
#define BOOK_FACE_V5_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "sub_0001_tensors.h"

#define BOOK_FACE_V5_INPUT_WIDTH          (256U)
#define BOOK_FACE_V5_INPUT_HEIGHT         (256U)
#define BOOK_FACE_V5_INPUT_CHANNELS       (3U)
#define BOOK_FACE_V5_OUTPUT16_GRID        (16U)
#define BOOK_FACE_V5_OUTPUT8_GRID         (8U)
#define BOOK_FACE_V5_OUTPUT_CHANNELS      (24U)

#define BOOK_FACE_V5_INPUT_ZERO_POINT     (-1)
#define BOOK_FACE_V5_INPUT_SCALE          (0.007843137718737125F)
#define BOOK_FACE_V5_OUTPUT16_ZERO_POINT  (-1)
#define BOOK_FACE_V5_OUTPUT16_SCALE       (0.191580131649971F)
#define BOOK_FACE_V5_OUTPUT8_ZERO_POINT   (0)
#define BOOK_FACE_V5_OUTPUT8_SCALE        (0.1988992691040039F)

/*
 * These pointers address the NPU arena directly. The application prepares
 * quantized NHWC input in-place and decodes quantized NCHW outputs in-place,
 * avoiding generated float input/output buffers and their staging copies.
 */
int8_t * GetModelInputPtr_book_face_v5_images(void);
int8_t const * GetModelOutputPtr_book_face_v5_output0_70440_70594(void);
int8_t const * GetModelOutputPtr_book_face_v5__837_70448_70593(void);
int RunModel_book_face_v5(bool clean_outputs);

#endif
