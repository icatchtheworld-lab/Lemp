#include "Music_Rhythm/music_onset.h"

#include "arm_math.h"

#include <stddef.h>
#include <string.h>

/*
 * Design reference:
 * absent42/esphome-audio-reactive OnsetDetector (MIT License).
 * The implementation here is a C, fixed-memory adaptation for the RA8P1.
 * See THIRD_PARTY_NOTICE.md in this directory for attribution.
 */

#define MUSIC_ONSET_MINIMUM_THRESHOLD  (0.0000000001f)
#define MUSIC_ONSET_MINIMUM_STRENGTH   (0.1f)
#define MUSIC_ONSET_MAXIMUM_STRENGTH   (1.0f)

/*
 * A candidate is confirmed only after two later FFT frames have arrived and
 * it is still the largest value around that instant. At a 32 ms frame period,
 * this adds 64 ms of latency but prevents one attack from firing repeatedly
 * on several rising frames.
 */
#define MUSIC_ONSET_PEAK_PRE_FRAMES    (2U)
#define MUSIC_ONSET_PEAK_POST_FRAMES   (2U)

static uint8_t music_onset_sensitivity_limit(uint8_t sensitivity)
{
    if (sensitivity < 1U)
    {
        return 1U;
    }
    if (sensitivity > 100U)
    {
        return 100U;
    }

    return sensitivity;
}

static void music_onset_history_push(music_onset_detector_t * p_detector,
                                     float value,
                                     uint32_t timestamp_ms,
                                     bool detection_allowed)
{
    if (p_detector->history_count >= MUSIC_ONSET_HISTORY_LENGTH)
    {
        float const removed = p_detector->history[p_detector->history_head];

        p_detector->history_sum -= removed;
        p_detector->history_square_sum -= removed * removed;
    }
    else
    {
        p_detector->history_count++;
    }

    p_detector->history[p_detector->history_head] = value;
    p_detector->history_timestamps_ms[p_detector->history_head] = timestamp_ms;
    p_detector->history_detection_allowed[p_detector->history_head] =
        detection_allowed;
    p_detector->history_sum += value;
    p_detector->history_square_sum += value * value;

    p_detector->history_head++;
    if (p_detector->history_head >= MUSIC_ONSET_HISTORY_LENGTH)
    {
        p_detector->history_head = 0U;
    }
}

static uint32_t music_onset_history_index_from_age(
    music_onset_detector_t const * p_detector,
    uint32_t age)
{
    return (p_detector->history_head + MUSIC_ONSET_HISTORY_LENGTH - 1U - age) %
           MUSIC_ONSET_HISTORY_LENGTH;
}

static float music_onset_history_value_from_age(
    music_onset_detector_t const * p_detector,
    uint32_t age)
{
    return p_detector->history[music_onset_history_index_from_age(p_detector, age)];
}

static uint32_t music_onset_history_timestamp_from_age(
    music_onset_detector_t const * p_detector,
    uint32_t age)
{
    return p_detector->history_timestamps_ms[
        music_onset_history_index_from_age(p_detector, age)];
}

static bool music_onset_history_detection_allowed_from_age(
    music_onset_detector_t const * p_detector,
    uint32_t age)
{
    return p_detector->history_detection_allowed[
        music_onset_history_index_from_age(p_detector, age)];
}

static bool music_onset_candidate_is_local_peak(
    music_onset_detector_t const * p_detector,
    uint32_t candidate_age,
    float candidate_value)
{
    uint32_t const youngest_age = candidate_age - MUSIC_ONSET_PEAK_POST_FRAMES;
    uint32_t const oldest_age = candidate_age + MUSIC_ONSET_PEAK_PRE_FRAMES;

    for (uint32_t age = youngest_age; age <= oldest_age; age++)
    {
        if ((age != candidate_age) &&
            (music_onset_history_value_from_age(p_detector, age) >= candidate_value))
        {
            return false;
        }
    }

    return true;
}

static float music_onset_threshold_get(music_onset_detector_t const * p_detector)
{
    float count;
    float mean;
    float variance;
    float standard_deviation;
    float threshold;

    if (0U == p_detector->history_count)
    {
        return MUSIC_ONSET_MINIMUM_THRESHOLD;
    }

    count = (float) p_detector->history_count;
    mean = p_detector->history_sum / count;
    variance = (p_detector->history_square_sum / count) - (mean * mean);

    /* Floating-point rounding may make a theoretically zero variance negative. */
    if (variance < 0.0f)
    {
        variance = 0.0f;
    }

    /* CMSIS-DSP is already used by the FFT path, so use its square-root helper. */
    if (ARM_MATH_SUCCESS != arm_sqrt_f32(variance, &standard_deviation))
    {
        standard_deviation = 0.0f;
    }

    /*
     * A perfectly steady history has zero standard deviation.  Retaining at
     * least 10 percent of the mean prevents an unrealistically low threshold.
     */
    if (standard_deviation < (mean * 0.1f))
    {
        standard_deviation = mean * 0.1f;
    }

    threshold = mean + (p_detector->threshold_multiplier * standard_deviation);
    if (threshold < MUSIC_ONSET_MINIMUM_THRESHOLD)
    {
        threshold = MUSIC_ONSET_MINIMUM_THRESHOLD;
    }

    return threshold;
}

void music_onset_init(music_onset_detector_t * p_detector,
                      uint8_t sensitivity,
                      uint32_t minimum_interval_ms)
{
    uint8_t const limited_sensitivity = music_onset_sensitivity_limit(sensitivity);

    if (NULL == p_detector)
    {
        return;
    }

    memset(p_detector, 0, sizeof(*p_detector));

    /*
     * This mapping follows the reference algorithm:
     * sensitivity 1   -> multiplier about 2.975 (strict)
     * sensitivity 100 -> multiplier 0.5       (sensitive)
     */
    p_detector->threshold_multiplier =
        3.0f - (((float) limited_sensitivity / 100.0f) * 2.5f);
    p_detector->minimum_interval_ms = minimum_interval_ms;
}

void music_onset_reset(music_onset_detector_t * p_detector)
{
    float threshold_multiplier;
    uint32_t minimum_interval_ms;

    if (NULL == p_detector)
    {
        return;
    }

    threshold_multiplier = p_detector->threshold_multiplier;
    minimum_interval_ms = p_detector->minimum_interval_ms;

    memset(p_detector, 0, sizeof(*p_detector));

    p_detector->threshold_multiplier = threshold_multiplier;
    p_detector->minimum_interval_ms = minimum_interval_ms;
}

music_onset_result_t music_onset_update(music_onset_detector_t * p_detector,
                                        float const p_bands[MUSIC_ONSET_BAND_COUNT],
                                        uint32_t timestamp_ms,
                                        bool detection_allowed)
{
    music_onset_result_t result = {0};
    float spectral_flux = 0.0f;
    float candidate_value;
    uint32_t candidate_timestamp_ms;
    bool candidate_detection_allowed;
    bool interval_ready;
    bool local_peak;

    if ((NULL == p_detector) || (NULL == p_bands))
    {
        return result;
    }

    if (p_detector->previous_bands_valid)
    {
        for (uint32_t i = 0U; i < MUSIC_ONSET_BAND_COUNT; i++)
        {
            float const difference = p_bands[i] - p_detector->previous_bands[i];

            /*
             * Only newly rising energy contributes to an onset. Squaring the
             * difference emphasizes a sharp musical attack over slow changes.
             */
            if (difference > 0.0f)
            {
                spectral_flux += difference * difference;
            }
        }
    }

    memcpy(p_detector->previous_bands, p_bands, sizeof(p_detector->previous_bands));
    p_detector->previous_bands_valid = true;

    result.threshold = music_onset_threshold_get(p_detector);

    music_onset_history_push(p_detector,
                             spectral_flux,
                             timestamp_ms,
                             detection_allowed);

    result.ready = (p_detector->history_count >= (MUSIC_ONSET_HISTORY_LENGTH / 2U));

    candidate_value = music_onset_history_value_from_age(
        p_detector,
        MUSIC_ONSET_PEAK_POST_FRAMES);
    candidate_timestamp_ms = music_onset_history_timestamp_from_age(
        p_detector,
        MUSIC_ONSET_PEAK_POST_FRAMES);
    candidate_detection_allowed = music_onset_history_detection_allowed_from_age(
        p_detector,
        MUSIC_ONSET_PEAK_POST_FRAMES);
    result.value = candidate_value;

    local_peak = music_onset_candidate_is_local_peak(
        p_detector,
        MUSIC_ONSET_PEAK_POST_FRAMES,
        candidate_value);

    interval_ready =
        !p_detector->last_onset_valid ||
        ((candidate_timestamp_ms - p_detector->last_onset_ms) >=
         p_detector->minimum_interval_ms);

    if (result.ready && candidate_detection_allowed && local_peak && interval_ready &&
        (candidate_value > result.threshold))
    {
        float strength = (candidate_value - result.threshold) / result.threshold;

        if (strength < MUSIC_ONSET_MINIMUM_STRENGTH)
        {
            strength = MUSIC_ONSET_MINIMUM_STRENGTH;
        }
        else if (strength > MUSIC_ONSET_MAXIMUM_STRENGTH)
        {
            strength = MUSIC_ONSET_MAXIMUM_STRENGTH;
        }

        result.detected = true;
        result.strength = strength;
        result.interval_ms = p_detector->last_onset_valid ?
                             (candidate_timestamp_ms - p_detector->last_onset_ms) : 0U;
        result.timestamp_ms = candidate_timestamp_ms;

        p_detector->last_onset_ms = candidate_timestamp_ms;
        p_detector->last_onset_valid = true;
    }

    return result;
}
