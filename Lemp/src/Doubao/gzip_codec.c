#include "Doubao/gzip_codec.h"

/* LVGL already carries LodePNG's complete RFC1951 inflater.  Compile only its
 * allocation and DEFLATE decoder sections here; the normal LVGL build has
 * LV_USE_PNG=0, so this does not duplicate an enabled PNG implementation. */
#include "Middlewares/lvgl/lvgl.h"
#undef LV_USE_PNG
#define LV_USE_PNG 1
#define LODEPNG_NO_COMPILE_PNG
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ERROR_TEXT
#include "Middlewares/lvgl/src/extra/libs/png/lodepng.c"

#include <stdlib.h>

#define GZIP_HEADER_SIZE       (10U)
#define GZIP_TRAILER_SIZE      (8U)
#define GZIP_STORED_OVERHEAD   (23U)

static uint32_t gzip_crc32(uint8_t const * data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t index = 0U; index < length; index++)
    {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t read_le32(uint8_t const * source)
{
    return (uint32_t) source[0] |
           ((uint32_t) source[1] << 8U) |
           ((uint32_t) source[2] << 16U) |
           ((uint32_t) source[3] << 24U);
}

static void write_le16(uint8_t * destination, uint16_t value)
{
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8U);
}

static void write_le32(uint8_t * destination, uint32_t value)
{
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8U);
    destination[2] = (uint8_t) (value >> 16U);
    destination[3] = (uint8_t) (value >> 24U);
}

size_t gzip_encode_stored(uint8_t const * input,
                          size_t input_length,
                          uint8_t * output,
                          size_t output_capacity)
{
    size_t offset;
    uint16_t length16;

    if (((NULL == input) && (input_length > 0U)) || (NULL == output) ||
        (input_length > UINT16_MAX) ||
        (output_capacity < (input_length + GZIP_STORED_OVERHEAD)))
    {
        return 0U;
    }

    output[0] = 0x1FU;
    output[1] = 0x8BU;
    output[2] = 0x08U;
    output[3] = 0x00U;
    output[4] = 0x00U;
    output[5] = 0x00U;
    output[6] = 0x00U;
    output[7] = 0x00U;
    output[8] = 0x00U;
    output[9] = 0xFFU;

    /* One final, byte-aligned, uncompressed DEFLATE block.  Audio PCM is not
     * usefully compressible, and this bounds CPU time in the capture loop. */
    offset = GZIP_HEADER_SIZE;
    output[offset++] = 0x01U;
    length16 = (uint16_t) input_length;
    write_le16(&output[offset], length16);
    offset += 2U;
    write_le16(&output[offset], (uint16_t) ~length16);
    offset += 2U;
    if (input_length > 0U)
    {
        memcpy(&output[offset], input, input_length);
        offset += input_length;
    }

    write_le32(&output[offset], gzip_crc32(input, input_length));
    offset += 4U;
    write_le32(&output[offset], (uint32_t) input_length);
    offset += 4U;
    return offset;
}

static bool gzip_skip_zero_terminated(uint8_t const * input,
                                      size_t trailer_offset,
                                      size_t * offset)
{
    while (*offset < trailer_offset)
    {
        if (0U == input[(*offset)++])
        {
            return true;
        }
    }
    return false;
}

bool gzip_decode_alloc(uint8_t const * input,
                       size_t input_length,
                       size_t output_limit,
                       uint8_t ** output,
                       size_t * output_length,
                       unsigned * inflate_error)
{
    LodePNGDecompressSettings settings;
    size_t offset = GZIP_HEADER_SIZE;
    size_t trailer_offset;
    uint8_t flags;
    unsigned error;

    if (NULL != inflate_error)
    {
        *inflate_error = 0U;
    }
    if ((NULL == input) || (NULL == output) || (NULL == output_length) ||
        (input_length < (GZIP_HEADER_SIZE + GZIP_TRAILER_SIZE + 1U)))
    {
        return false;
    }

    *output = NULL;
    *output_length = 0U;
    trailer_offset = input_length - GZIP_TRAILER_SIZE;
    flags = input[3];
    if ((0x1FU != input[0]) || (0x8BU != input[1]) || (0x08U != input[2]) ||
        (0U != (flags & 0xE0U)))
    {
        return false;
    }

    if (0U != (flags & 0x04U))
    {
        uint16_t extra_length;
        if ((offset + 2U) > trailer_offset)
        {
            return false;
        }
        extra_length = (uint16_t) input[offset] | (uint16_t) ((uint16_t) input[offset + 1U] << 8U);
        offset += 2U;
        if ((offset + extra_length) > trailer_offset)
        {
            return false;
        }
        offset += extra_length;
    }
    if ((0U != (flags & 0x08U)) && !gzip_skip_zero_terminated(input, trailer_offset, &offset))
    {
        return false;
    }
    if ((0U != (flags & 0x10U)) && !gzip_skip_zero_terminated(input, trailer_offset, &offset))
    {
        return false;
    }
    if (0U != (flags & 0x02U))
    {
        if ((offset + 2U) > trailer_offset)
        {
            return false;
        }
        offset += 2U;
    }
    if (offset >= trailer_offset)
    {
        return false;
    }

    lodepng_decompress_settings_init(&settings);
    settings.max_output_size = output_limit;
    error = lodepng_inflate(output,
                            output_length,
                            &input[offset],
                            trailer_offset - offset,
                            &settings);
    if (NULL != inflate_error)
    {
        *inflate_error = error;
    }
    if ((0U != error) || (*output_length > output_limit) ||
        (*output_length != (size_t) read_le32(&input[trailer_offset + 4U])) ||
        (gzip_crc32(*output, *output_length) != read_le32(&input[trailer_offset])))
    {
        free(*output);
        *output = NULL;
        *output_length = 0U;
        return false;
    }
    return true;
}

void gzip_decode_free(void * output)
{
    free(output);
}

