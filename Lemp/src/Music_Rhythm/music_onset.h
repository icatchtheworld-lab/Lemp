#ifndef MUSIC_RHYTHM_MUSIC_ONSET_H_
#define MUSIC_RHYTHM_MUSIC_ONSET_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The FFT spectrum is divided into 16 relatively narrow bands before it is
 * passed to the onset detector.  Keeping more frequency detail than the old
 * BASS/MID/HIGH division makes the detector less dependent on one timbre.
 */
#define MUSIC_ONSET_BAND_COUNT       (16U)
#define MUSIC_ONSET_HISTORY_LENGTH   (64U)

/** Result produced for one FFT analysis frame. */
typedef struct st_music_onset_result
{
    bool detected;       /* True only when this frame contains a new onset. */
    bool ready;          /* False while the rolling statistics are warming up. */
    float value;         /* Positive spectral-flux value of the current frame. */
    float threshold;     /* Adaptive threshold calculated from recent history. */
    float strength;      /* Normalized onset strength in the range 0.1 to 1.0. */
    uint32_t interval_ms;/* Time since the previous detected onset; zero for the first one. */
    uint32_t timestamp_ms;/* Audio timestamp of the confirmed local peak. */
} music_onset_result_t;

/**
 * Fixed-memory state of the onset detector.
 *
 * No heap allocation is used, which makes this module suitable for the
 * RA8P1 bare-metal application.
 */
typedef struct st_music_onset_detector
{
    float previous_bands[MUSIC_ONSET_BAND_COUNT];
    float history[MUSIC_ONSET_HISTORY_LENGTH];
    uint32_t history_timestamps_ms[MUSIC_ONSET_HISTORY_LENGTH];
    bool history_detection_allowed[MUSIC_ONSET_HISTORY_LENGTH];
    float history_sum;
    float history_square_sum;
    float threshold_multiplier;
    uint32_t history_count;
    uint32_t history_head;
    uint32_t minimum_interval_ms;
    uint32_t last_onset_ms;
    bool previous_bands_valid;
    bool last_onset_valid;
} music_onset_detector_t;

/**
 * @brief Initialize an onset detector.
 *
 * @param[in,out] p_detector        Detector state supplied by the caller.
 * @param[in]     sensitivity       Range 1 to 100. A larger value detects
 *                                  weaker attacks but may also detect noise.
 * @param[in]     minimum_interval_ms Minimum time allowed between two onsets.
 */
void music_onset_init(music_onset_detector_t * p_detector,
                      uint8_t sensitivity,
                      uint32_t minimum_interval_ms);

/**
 * @brief Clear audio history while preserving sensitivity and interval settings.
 *
 * Call this after a microphone-frame discontinuity so samples from two
 * discontinuous time periods are not compared with each other.
 */
void music_onset_reset(music_onset_detector_t * p_detector);

/**
 * @brief Process one array of 16 FFT-band magnitudes.
 *
 * @param[in,out] p_detector Detector state.
 * @param[in]     p_bands    Current non-negative magnitude of each band.
 * @param[in]     timestamp_ms Monotonically increasing audio timestamp.
 * @param[in]     detection_allowed False keeps statistics updated but prevents
 *                                  this frame from becoming an onset event.
 *
 * @return Detection result for the current frame.
 */
music_onset_result_t music_onset_update(music_onset_detector_t * p_detector,
                                        float const p_bands[MUSIC_ONSET_BAND_COUNT],
                                        uint32_t timestamp_ms,
                                        bool detection_allowed);

#endif /* MUSIC_RHYTHM_MUSIC_ONSET_H_ */
