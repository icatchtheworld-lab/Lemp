#ifndef DOUBAO_GZIP_CODEC_H_
#define DOUBAO_GZIP_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t gzip_encode_stored(uint8_t const * input,
                          size_t input_length,
                          uint8_t * output,
                          size_t output_capacity);
bool gzip_decode_alloc(uint8_t const * input,
                       size_t input_length,
                       size_t output_limit,
                       uint8_t ** output,
                       size_t * output_length,
                       unsigned * inflate_error);
void gzip_decode_free(void * output);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_GZIP_CODEC_H_ */

