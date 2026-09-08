#ifndef MUSIC_RHYTHM_MUSIC_BEAT_TRACKER_H_
#define MUSIC_RHYTHM_MUSIC_BEAT_TRACKER_H_

#include <stdbool.h>
#include <stdint.h>

#define MUSIC_BEAT_HISTORY_LENGTH  (128U)
#define MUSIC_BEAT_ACF_LENGTH      (64U)

/** Result returned after processing one onset-analysis frame. */
typedef struct st_music_beat_result
{
    bool tempo_locked;      /* A stable tempo has been accepted. */
    bool tempo_updated;     /* The internal tempo estimate changed this frame. */
    bool prediction_active; /* Predicted beats are currently allowed. */
    bool beat_event;        /* One predicted beat is due on this frame. */
    float bpm;              /* Current tempo in beats per minute. */
    float confidence;       /* Tempo confidence in the range 0.0 to 1.0. */
    float phase;            /* Position inside the current beat: 0.0 to 1.0. */
    uint32_t beat_timestamp_ms;
} music_beat_result_t;

/**
 * Fixed-memory beat tracker.
 *
 * The tracker keeps about four seconds of onset pulses, calculates normalized
 * autocorrelation, checks harmonic periods, and then predicts future beats.
 */
typedef struct st_music_beat_tracker
{
    float onset_history[MUSIC_BEAT_HISTORY_LENGTH];
    float autocorrelation[MUSIC_BEAT_ACF_LENGTH];
    float period_scores[MUSIC_BEAT_ACF_LENGTH];
    float frame_rate_hz;
    float period_frames;
    float confidence;
    float pending_period_frames;
    float tempo_change_period_frames;
    uint32_t history_write_index;
    uint32_t history_count;
    uint32_t frames_since_estimate;
    uint32_t last_onset_timestamp_ms;
    uint32_t next_beat_timestamp_ms;
    uint8_t pending_confirmation_count;
    uint8_t tempo_change_confirmation_count;
    bool tempo_locked;
    bool last_onset_valid;
    bool prediction_active;
} music_beat_tracker_t;

/** Initialize a tracker for the rate at which FFT/onset frames are produced. */
void music_beat_tracker_init(music_beat_tracker_t * p_tracker, float frame_rate_hz);

/** Clear rhythm history while preserving the configured frame rate. */
void music_beat_tracker_reset(music_beat_tracker_t * p_tracker);

/**
 * Process one analysis frame.
 *
 * @param[in,out] p_tracker          Tracker state.
 * @param[in]     onset_detected     True if the onset detector confirmed a peak.
 * @param[in]     onset_strength     Confirmed onset strength, otherwise zero.
 * @param[in]     onset_timestamp_ms Timestamp returned by the onset detector.
 * @param[in]     current_timestamp_ms Timestamp of the current FFT frame.
 */
music_beat_result_t music_beat_tracker_update(music_beat_tracker_t * p_tracker,
                                              bool onset_detected,
                                              float onset_strength,
                                              uint32_t onset_timestamp_ms,
                                              uint32_t current_timestamp_ms);

#endif /* MUSIC_RHYTHM_MUSIC_BEAT_TRACKER_H_ */
