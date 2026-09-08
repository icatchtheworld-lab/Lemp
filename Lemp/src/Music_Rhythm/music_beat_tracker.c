#include "Music_Rhythm/music_beat_tracker.h"

#include "arm_math.h"

#include <stddef.h>
#include <string.h>

/*
 * Design reference:
 * absent42/esphome-audio-reactive BeatTracker (MIT License).
 * This C implementation uses fixed arrays and a normalized autocorrelation
 * suitable for the 31.25 Hz FFT-frame rate of the RA8P1 project.
 */

#define MUSIC_BEAT_MINIMUM_BPM                 (40.0f)
#define MUSIC_BEAT_MAXIMUM_BPM                 (200.0f)
#define MUSIC_BEAT_PREFERRED_BPM               (120.0f)
#define MUSIC_BEAT_ESTIMATE_INTERVAL_FRAMES    (32U)
#define MUSIC_BEAT_MINIMUM_ONSET_COUNT         (3U)
#define MUSIC_BEAT_LOCK_CONFIDENCE             (0.55f)
#define MUSIC_BEAT_INITIAL_CONFIRMATIONS       (2U)
#define MUSIC_BEAT_TEMPO_CHANGE_CONFIRMATIONS  (3U)
#define MUSIC_BEAT_SIMILAR_PERIOD_RATIO        (0.15f)
#define MUSIC_BEAT_SMOOTH_PERIOD_RATIO         (0.20f)
#define MUSIC_BEAT_PREDICTION_HOLD_MS          (3500U)
#define MUSIC_BEAT_PHASE_TOLERANCE_RATIO       (0.25f)

/*
 * Music with alternating strong and weak beats often produces a stronger
 * autocorrelation peak at twice the real beat period.  The shorter-period
 * candidate is accepted only when it has enough independent correlation
 * evidence, so a genuine slow beat is not doubled unconditionally.
 */
#define MUSIC_BEAT_OCTAVE_SCORE_RATIO           (0.65f)
#define MUSIC_BEAT_OCTAVE_CORRELATION_RATIO     (0.25f)
#define MUSIC_BEAT_TEMPO_CHANGE_INTERVAL_RATIO    (0.15f)
#define MUSIC_BEAT_LOCKED_INTERVAL_TOLERANCE      (0.18f)
/* 锁定后忽略半倍或双倍BPM附近的候选，避免同一首歌发生倍频跳变。 */
#define MUSIC_BEAT_OCTAVE_BPM_TOLERANCE           (10.0f)

static float music_beat_absolute_float(float value)
{
    return (value < 0.0f) ? -value : value;
}

static uint32_t music_beat_absolute_time_difference(uint32_t first, uint32_t second)
{
    return (first >= second) ? (first - second) : (second - first);
}

static bool music_beat_time_reached(uint32_t current, uint32_t target)
{
    return ((int32_t) (current - target) >= 0);
}

static float music_beat_history_read(music_beat_tracker_t const * p_tracker,
                                     uint32_t linear_index)
{
    uint32_t const physical_index =
        (p_tracker->history_write_index + linear_index) % MUSIC_BEAT_HISTORY_LENGTH;

    return p_tracker->onset_history[physical_index];
}

static uint32_t music_beat_onset_count_get(music_beat_tracker_t const * p_tracker)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < MUSIC_BEAT_HISTORY_LENGTH; i++)
    {
        if (p_tracker->onset_history[i] > 0.0f)
        {
            count++;
        }
    }

    return count;
}

static void music_beat_autocorrelation_calculate(music_beat_tracker_t * p_tracker)
{
    p_tracker->autocorrelation[0] = 1.0f;

    for (uint32_t lag = 1U; lag < MUSIC_BEAT_ACF_LENGTH; lag++)
    {
        float product_sum = 0.0f;
        float first_energy = 0.0f;
        float second_energy = 0.0f;
        float denominator = 0.0f;
        uint32_t const pair_count = MUSIC_BEAT_HISTORY_LENGTH - lag;

        for (uint32_t i = 0U; i < pair_count; i++)
        {
            float const first = music_beat_history_read(p_tracker, i);
            float const second = music_beat_history_read(p_tracker, i + lag);

            product_sum += first * second;
            first_energy += first * first;
            second_energy += second * second;
        }

        if (ARM_MATH_SUCCESS ==
            arm_sqrt_f32(first_energy * second_energy, &denominator))
        {
            p_tracker->autocorrelation[lag] =
                (denominator > 0.0f) ? (product_sum / denominator) : 0.0f;
        }
        else
        {
            p_tracker->autocorrelation[lag] = 0.0f;
        }
    }
}

static float music_beat_tempo_preference(float lag, float preferred_lag)
{
    float const normalized_distance = (lag - preferred_lag) / preferred_lag;

    /* A gentle preference resolves octave ambiguity without forcing 120 BPM. */
    return 0.90f +
           (0.10f / (1.0f + (4.0f * normalized_distance * normalized_distance)));
}

static float music_beat_period_refine(music_beat_tracker_t const * p_tracker,
                                      uint32_t lag)
{
    float const left = p_tracker->period_scores[lag - 1U];
    float const center = p_tracker->period_scores[lag];
    float const right = p_tracker->period_scores[lag + 1U];
    float const denominator = 2.0f * (left - (2.0f * center) + right);

    if (music_beat_absolute_float(denominator) < 0.000001f)
    {
        return (float) lag;
    }

    /* Quadratic interpolation estimates a sub-frame peak between FFT lags. */
    return (float) lag + ((left - right) / denominator);
}

static uint32_t music_beat_shorter_octave_lag_get(
    music_beat_tracker_t const * p_tracker,
    uint32_t best_lag,
    uint32_t minimum_lag,
    uint32_t maximum_lag)
{
    uint32_t shorter_center;
    uint32_t shorter_best_lag = 0U;
    float shorter_best_score = 0.0f;
    float const best_score = p_tracker->period_scores[best_lag];
    float const best_correlation = p_tracker->autocorrelation[best_lag];

    /* The octave candidate has approximately half the selected period. */
    if ((best_lag < (minimum_lag * 2U)) || (0.0f >= best_score))
    {
        return best_lag;
    }

    shorter_center = (best_lag + 1U) / 2U;

    /*
     * Check one lag on either side because the real period normally lies
     * between FFT frames, for example 100 BPM is about 18.75 frames.
     */
    for (int32_t offset = -1; offset <= 1; offset++)
    {
        int32_t const candidate = (int32_t) shorter_center + offset;

        if ((candidate >= (int32_t) minimum_lag) &&
            (candidate <= (int32_t) maximum_lag) &&
            (p_tracker->period_scores[(uint32_t) candidate] > shorter_best_score))
        {
            shorter_best_lag = (uint32_t) candidate;
            shorter_best_score = p_tracker->period_scores[shorter_best_lag];
        }
    }

    if (0U == shorter_best_lag)
    {
        return best_lag;
    }

    /*
     * The score contains harmonic support, while the raw autocorrelation
     * proves that actual onset energy also repeats at the shorter period.
     */
    if ((shorter_best_score >= (best_score * MUSIC_BEAT_OCTAVE_SCORE_RATIO)) &&
        (p_tracker->autocorrelation[shorter_best_lag] >=
         (best_correlation * MUSIC_BEAT_OCTAVE_CORRELATION_RATIO)))
    {
        return shorter_best_lag;
    }

    return best_lag;
}

static bool music_beat_period_estimate(music_beat_tracker_t * p_tracker,
                                       float * p_period_frames,
                                       float * p_confidence)
{
    float best_score = 0.0f;
    float second_score = 0.0f;
    float contrast;
    float periodicity;
    float preferred_lag;
    uint32_t best_lag = 0U;
    uint32_t minimum_lag;
    uint32_t maximum_lag;

    if ((NULL == p_period_frames) || (NULL == p_confidence) ||
        (music_beat_onset_count_get(p_tracker) < MUSIC_BEAT_MINIMUM_ONSET_COUNT))
    {
        return false;
    }

    music_beat_autocorrelation_calculate(p_tracker);

    minimum_lag = (uint32_t) ((p_tracker->frame_rate_hz * 60.0f /
                               MUSIC_BEAT_MAXIMUM_BPM) + 0.5f);
    maximum_lag = (uint32_t) ((p_tracker->frame_rate_hz * 60.0f /
                               MUSIC_BEAT_MINIMUM_BPM) + 0.5f);

    if (minimum_lag < 2U)
    {
        minimum_lag = 2U;
    }
    if (maximum_lag >= (MUSIC_BEAT_ACF_LENGTH - 1U))
    {
        maximum_lag = MUSIC_BEAT_ACF_LENGTH - 2U;
    }

    preferred_lag = p_tracker->frame_rate_hz * 60.0f / MUSIC_BEAT_PREFERRED_BPM;
    memset(p_tracker->period_scores, 0, sizeof(p_tracker->period_scores));

    for (uint32_t lag = minimum_lag; lag <= maximum_lag; lag++)
    {
        float score = p_tracker->autocorrelation[lag];

        /* Harmonic support favors the fundamental period over half-tempo errors. */
        if ((lag * 2U) < MUSIC_BEAT_ACF_LENGTH)
        {
            score += 0.50f * p_tracker->autocorrelation[lag * 2U];
        }
        if ((lag * 3U) < MUSIC_BEAT_ACF_LENGTH)
        {
            score += 0.25f * p_tracker->autocorrelation[lag * 3U];
        }

        score *= music_beat_tempo_preference((float) lag, preferred_lag);
        p_tracker->period_scores[lag] = score;

        if (score > best_score)
        {
            best_score = score;
            best_lag = lag;
        }
    }

    if ((0U == best_lag) || (best_score <= 0.0f))
    {
        return false;
    }

    /* Correct the common 1/2-tempo result before calculating confidence. */
    best_lag = music_beat_shorter_octave_lag_get(p_tracker,
                                                  best_lag,
                                                  minimum_lag,
                                                  maximum_lag);
    best_score = p_tracker->period_scores[best_lag];

    for (uint32_t lag = minimum_lag; lag <= maximum_lag; lag++)
    {
        uint32_t const distance = (lag >= best_lag) ?
                                  (lag - best_lag) : (best_lag - lag);

        if ((distance > 1U) && (p_tracker->period_scores[lag] > second_score))
        {
            second_score = p_tracker->period_scores[lag];
        }
    }

    periodicity = p_tracker->autocorrelation[best_lag];
    if (periodicity > 1.0f)
    {
        periodicity = 1.0f;
    }

    contrast = (best_score > 0.0f) ?
               ((best_score - second_score) / best_score) : 0.0f;
    if (contrast < 0.0f)
    {
        contrast = 0.0f;
    }
    else if (contrast > 1.0f)
    {
        contrast = 1.0f;
    }

    *p_period_frames = music_beat_period_refine(p_tracker, best_lag);
    if (*p_period_frames < (float) minimum_lag)
    {
        *p_period_frames = (float) minimum_lag;
    }
    else if (*p_period_frames > (float) maximum_lag)
    {
        *p_period_frames = (float) maximum_lag;
    }
    *p_confidence = (0.80f * periodicity) + (0.20f * contrast);

    return true;
}

static bool music_beat_period_is_similar(float first, float second, float ratio_limit)
{
    float const reference = (first > second) ? first : second;

    return (reference > 0.0f) &&
           ((music_beat_absolute_float(first - second) / reference) <= ratio_limit);
}

/**
 * @brief 判断候选周期是否只是当前节奏的一半或两倍。
 *
 * 周期与BPM成反比，因此统一换算为BPM再应用用户要求的正负10 BPM
 * 容差。首次尚未锁定时返回false，保留初次识别时选择正确倍频的能力。
 */
static bool music_beat_candidate_is_octave_alias(
    music_beat_tracker_t const * p_tracker,
    float candidate_period_frames)
{
    float current_bpm;
    float candidate_bpm;
    float half_bpm_error;
    float double_bpm_error;

    if ((NULL == p_tracker) || !p_tracker->tempo_locked ||
        (p_tracker->frame_rate_hz <= 0.0f) ||
        (p_tracker->period_frames <= 0.0f) ||
        (candidate_period_frames <= 0.0f))
    {
        return false;
    }

    current_bpm =
        (p_tracker->frame_rate_hz * 60.0f) / p_tracker->period_frames;
    candidate_bpm =
        (p_tracker->frame_rate_hz * 60.0f) / candidate_period_frames;
    half_bpm_error = music_beat_absolute_float(
        candidate_bpm - (current_bpm * 0.5f));
    double_bpm_error = music_beat_absolute_float(
        candidate_bpm - (current_bpm * 2.0f));

    return (half_bpm_error <= MUSIC_BEAT_OCTAVE_BPM_TOLERANCE) ||
           (double_bpm_error <= MUSIC_BEAT_OCTAVE_BPM_TOLERANCE);
}

static bool music_beat_period_accept(music_beat_tracker_t * p_tracker,
                                     float candidate_period,
                                     float candidate_confidence)
{
    uint8_t required_confirmations;

    if (candidate_confidence < MUSIC_BEAT_LOCK_CONFIDENCE)
    {
        return false;
    }

    if (music_beat_candidate_is_octave_alias(p_tracker, candidate_period))
    {
        /* 倍频候选不能积累成一次换速，现有拍点和相位保持不变。 */
        p_tracker->pending_period_frames = p_tracker->period_frames;
        p_tracker->pending_confirmation_count = 0U;
        return false;
    }

    if (p_tracker->tempo_locked &&
        music_beat_period_is_similar(candidate_period,
                                     p_tracker->period_frames,
                                     MUSIC_BEAT_SMOOTH_PERIOD_RATIO))
    {
        p_tracker->period_frames =
            (0.70f * p_tracker->period_frames) + (0.30f * candidate_period);
        p_tracker->confidence =
            (0.70f * p_tracker->confidence) + (0.30f * candidate_confidence);
        p_tracker->pending_confirmation_count = 0U;
        return true;
    }

    if (music_beat_period_is_similar(candidate_period,
                                     p_tracker->pending_period_frames,
                                     MUSIC_BEAT_SIMILAR_PERIOD_RATIO))
    {
        if (p_tracker->pending_confirmation_count < UINT8_MAX)
        {
            p_tracker->pending_confirmation_count++;
        }
        p_tracker->pending_period_frames =
            (0.50f * p_tracker->pending_period_frames) + (0.50f * candidate_period);
    }
    else
    {
        p_tracker->pending_period_frames = candidate_period;
        p_tracker->pending_confirmation_count = 1U;
    }

    required_confirmations = p_tracker->tempo_locked ?
                             MUSIC_BEAT_TEMPO_CHANGE_CONFIRMATIONS :
                             MUSIC_BEAT_INITIAL_CONFIRMATIONS;

    if (p_tracker->pending_confirmation_count >= required_confirmations)
    {
        p_tracker->period_frames = p_tracker->pending_period_frames;
        p_tracker->confidence = candidate_confidence;
        p_tracker->tempo_locked = true;
        p_tracker->pending_confirmation_count = 0U;
        return true;
    }

    return false;
}

static uint32_t music_beat_period_ms_get(music_beat_tracker_t const * p_tracker)
{
    if ((!p_tracker->tempo_locked) || (p_tracker->frame_rate_hz <= 0.0f))
    {
        return 0U;
    }

    return (uint32_t) (((p_tracker->period_frames * 1000.0f) /
                        p_tracker->frame_rate_hz) + 0.5f);
}

static bool music_beat_interval_matches_locked_period(uint32_t interval_ms,
                                                       uint32_t period_ms)
{
    uint32_t targets[5];
    uint32_t tolerance_ms;

    if ((0U == interval_ms) || (0U == period_ms))
    {
        return false;
    }

    /*
     * Half-period onsets are musical subdivisions. Two-, three- and four-beat
     * intervals can occur when weak beats are missed, so none of these alone
     * proves that the song tempo has changed.
     */
    targets[0] = period_ms / 2U;
    targets[1] = period_ms;
    targets[2] = period_ms * 2U;
    targets[3] = period_ms * 3U;
    targets[4] = period_ms * 4U;

    tolerance_ms =
        (uint32_t) (((float) period_ms * MUSIC_BEAT_LOCKED_INTERVAL_TOLERANCE) + 0.5f);
    if (tolerance_ms < 32U)
    {
        tolerance_ms = 32U;
    }

    for (uint32_t i = 0U; i < 5U; i++)
    {
        if (music_beat_absolute_time_difference(interval_ms, targets[i]) <=
            tolerance_ms)
        {
            return true;
        }
    }

    return false;
}

static void music_beat_history_restart(music_beat_tracker_t * p_tracker,
                                       float onset_strength)
{
    /* Remove the previous song from the autocorrelation input. */
    memset(p_tracker->onset_history, 0, sizeof(p_tracker->onset_history));
    memset(p_tracker->autocorrelation, 0, sizeof(p_tracker->autocorrelation));
    memset(p_tracker->period_scores, 0, sizeof(p_tracker->period_scores));

    p_tracker->onset_history[0] = onset_strength;
    p_tracker->history_write_index = 1U;
    p_tracker->history_count = 1U;
    p_tracker->frames_since_estimate = 0U;
    p_tracker->pending_period_frames = p_tracker->period_frames;
    p_tracker->pending_confirmation_count = 0U;
    p_tracker->tempo_change_period_frames = 0.0f;
    p_tracker->tempo_change_confirmation_count = 0U;
}

static bool music_beat_tempo_change_update(music_beat_tracker_t * p_tracker,
                                           uint32_t onset_interval_ms,
                                           float onset_strength,
                                           uint32_t onset_timestamp_ms,
                                           uint32_t period_ms)
{
    float candidate_period_frames;
    float minimum_period_frames;
    float maximum_period_frames;

    if ((0U == onset_interval_ms) ||
        music_beat_interval_matches_locked_period(onset_interval_ms, period_ms))
    {
        p_tracker->tempo_change_period_frames = 0.0f;
        p_tracker->tempo_change_confirmation_count = 0U;
        return false;
    }

    candidate_period_frames =
        ((float) onset_interval_ms * p_tracker->frame_rate_hz) / 1000.0f;
    minimum_period_frames =
        p_tracker->frame_rate_hz * 60.0f / MUSIC_BEAT_MAXIMUM_BPM;
    maximum_period_frames =
        p_tracker->frame_rate_hz * 60.0f / MUSIC_BEAT_MINIMUM_BPM;

    if ((candidate_period_frames < minimum_period_frames) ||
        (candidate_period_frames > maximum_period_frames))
    {
        return false;
    }

    if (music_beat_candidate_is_octave_alias(p_tracker,
                                              candidate_period_frames))
    {
        /* 间隔法同样不能用半倍或双倍结果覆盖已经锁定的节奏。 */
        p_tracker->tempo_change_period_frames = 0.0f;
        p_tracker->tempo_change_confirmation_count = 0U;
        return false;
    }

    if (music_beat_period_is_similar(candidate_period_frames,
                                     p_tracker->tempo_change_period_frames,
                                     MUSIC_BEAT_TEMPO_CHANGE_INTERVAL_RATIO))
    {
        p_tracker->tempo_change_period_frames =
            (0.50f * p_tracker->tempo_change_period_frames) +
            (0.50f * candidate_period_frames);

        if (p_tracker->tempo_change_confirmation_count < UINT8_MAX)
        {
            p_tracker->tempo_change_confirmation_count++;
        }
    }
    else
    {
        p_tracker->tempo_change_period_frames = candidate_period_frames;
        p_tracker->tempo_change_confirmation_count = 1U;
    }

    if (p_tracker->tempo_change_confirmation_count <
        MUSIC_BEAT_TEMPO_CHANGE_CONFIRMATIONS)
    {
        return false;
    }

    p_tracker->period_frames = p_tracker->tempo_change_period_frames;
    p_tracker->confidence = MUSIC_BEAT_LOCK_CONFIDENCE;
    period_ms = music_beat_period_ms_get(p_tracker);
    p_tracker->next_beat_timestamp_ms = onset_timestamp_ms + period_ms;
    p_tracker->prediction_active = true;
    music_beat_history_restart(p_tracker, onset_strength);

    return true;
}

static void music_beat_phase_correct(music_beat_tracker_t * p_tracker,
                                     uint32_t onset_timestamp_ms,
                                     uint32_t period_ms)
{
    uint32_t const previous_beat = p_tracker->next_beat_timestamp_ms - period_ms;
    uint32_t const previous_error =
        music_beat_absolute_time_difference(onset_timestamp_ms, previous_beat);
    uint32_t const next_error =
        music_beat_absolute_time_difference(onset_timestamp_ms,
                                            p_tracker->next_beat_timestamp_ms);
    uint32_t const tolerance =
        (uint32_t) (((float) period_ms * MUSIC_BEAT_PHASE_TOLERANCE_RATIO) + 0.5f);

    if ((previous_error <= tolerance) || (next_error <= tolerance))
    {
        p_tracker->next_beat_timestamp_ms = onset_timestamp_ms + period_ms;
    }
}

void music_beat_tracker_init(music_beat_tracker_t * p_tracker, float frame_rate_hz)
{
    if (NULL == p_tracker)
    {
        return;
    }

    memset(p_tracker, 0, sizeof(*p_tracker));
    p_tracker->frame_rate_hz = frame_rate_hz;
}

void music_beat_tracker_reset(music_beat_tracker_t * p_tracker)
{
    float frame_rate_hz;

    if (NULL == p_tracker)
    {
        return;
    }

    frame_rate_hz = p_tracker->frame_rate_hz;
    memset(p_tracker, 0, sizeof(*p_tracker));
    p_tracker->frame_rate_hz = frame_rate_hz;
}

music_beat_result_t music_beat_tracker_update(music_beat_tracker_t * p_tracker,
                                              bool onset_detected,
                                              float onset_strength,
                                              uint32_t onset_timestamp_ms,
                                              uint32_t current_timestamp_ms)
{
    music_beat_result_t result = {0};
    float candidate_period = 0.0f;
    float candidate_confidence = 0.0f;
    uint32_t period_ms;
    uint32_t onset_interval_ms = 0U;

    if ((NULL == p_tracker) || (p_tracker->frame_rate_hz <= 0.0f))
    {
        return result;
    }

    p_tracker->onset_history[p_tracker->history_write_index] =
        onset_detected ? onset_strength : 0.0f;
    p_tracker->history_write_index =
        (p_tracker->history_write_index + 1U) % MUSIC_BEAT_HISTORY_LENGTH;

    if (p_tracker->history_count < MUSIC_BEAT_HISTORY_LENGTH)
    {
        p_tracker->history_count++;
    }
    if (p_tracker->frames_since_estimate < UINT32_MAX)
    {
        p_tracker->frames_since_estimate++;
    }

    if (onset_detected)
    {
        /* Keep the interval before replacing the previous onset timestamp. */
        if (p_tracker->last_onset_valid)
        {
            onset_interval_ms = music_beat_absolute_time_difference(
                onset_timestamp_ms,
                p_tracker->last_onset_timestamp_ms);
        }

        p_tracker->last_onset_timestamp_ms = onset_timestamp_ms;
        p_tracker->last_onset_valid = true;
    }

    if ((p_tracker->history_count >= MUSIC_BEAT_HISTORY_LENGTH) &&
        (p_tracker->frames_since_estimate >= MUSIC_BEAT_ESTIMATE_INTERVAL_FRAMES))
    {
        p_tracker->frames_since_estimate = 0U;

        if (music_beat_period_estimate(p_tracker,
                                       &candidate_period,
                                       &candidate_confidence))
        {
            result.tempo_updated = music_beat_period_accept(p_tracker,
                                                            candidate_period,
                                                            candidate_confidence);
        }
    }

    period_ms = music_beat_period_ms_get(p_tracker);

    /*
     * 锁定后的半周期起音可能只是同一节奏的细分。旧代码会据此把BPM
     * 直接翻倍并重置相位；现在倍频候选统一由上方保护逻辑忽略。
     * 首次锁定前的ACF倍频选择仍然保留，不影响新音乐的初次识别。
     */

    /*
     * Three mutually consistent onset intervals that do not fit the locked
     * beat grid indicate an immediate song/tempo change. Re-lock directly to
     * that interval and discard the previous song's rolling history.
     */
    if (p_tracker->tempo_locked &&
        onset_detected &&
        (onset_interval_ms > 0U) &&
        music_beat_tempo_change_update(p_tracker,
                                       onset_interval_ms,
                                       onset_strength,
                                       onset_timestamp_ms,
                                       period_ms))
    {
        period_ms = music_beat_period_ms_get(p_tracker);
        result.tempo_updated = true;
    }

    if (p_tracker->tempo_locked && p_tracker->last_onset_valid && (period_ms > 0U))
    {
        bool const onset_is_recent =
            ((current_timestamp_ms - p_tracker->last_onset_timestamp_ms) <=
             MUSIC_BEAT_PREDICTION_HOLD_MS);

        if (onset_is_recent && !p_tracker->prediction_active)
        {
            p_tracker->next_beat_timestamp_ms =
                p_tracker->last_onset_timestamp_ms + period_ms;
            p_tracker->prediction_active = true;
        }

        if (onset_is_recent && onset_detected)
        {
            music_beat_phase_correct(p_tracker, onset_timestamp_ms, period_ms);
        }

        if (!onset_is_recent)
        {
            /*
             * A long silence ends the current music session completely.
             * Keeping tempo_locked here would let one later impact or voice
             * restart the old predicted beat immediately. Resetting the
             * tracker makes new audio prove a stable rhythm from the start.
             */
            music_beat_tracker_reset(p_tracker);
            period_ms = 0U;
        }
    }

    if (p_tracker->prediction_active && (period_ms > 0U) &&
        music_beat_time_reached(current_timestamp_ms,
                                p_tracker->next_beat_timestamp_ms))
    {
        result.beat_event = true;
        result.beat_timestamp_ms = p_tracker->next_beat_timestamp_ms;

        do
        {
            p_tracker->next_beat_timestamp_ms += period_ms;
        }
        while (music_beat_time_reached(current_timestamp_ms,
                                       p_tracker->next_beat_timestamp_ms));
    }

    result.tempo_locked = p_tracker->tempo_locked;
    result.prediction_active = p_tracker->prediction_active;
    result.confidence = p_tracker->confidence;

    if (p_tracker->tempo_locked && (p_tracker->period_frames > 0.0f))
    {
        result.bpm = p_tracker->frame_rate_hz * 60.0f / p_tracker->period_frames;

        if (p_tracker->prediction_active && (period_ms > 0U))
        {
            uint32_t const previous_beat =
                p_tracker->next_beat_timestamp_ms - period_ms;
            uint32_t const elapsed = current_timestamp_ms - previous_beat;

            result.phase = (float) elapsed / (float) period_ms;
            if (result.phase > 1.0f)
            {
                result.phase = 1.0f;
            }
        }
    }

    return result;
}
