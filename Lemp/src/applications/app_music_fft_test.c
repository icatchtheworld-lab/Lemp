#include "app.h"

#include "Printf/printf.h"
#include "Servo/servo.h"
#include "ServoLib/ServoDriver.h"
#include "Voice/voice.h"
#include "arm_math.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MUSIC_FFT_SAMPLE_RATE_HZ             (16000U)
#define MUSIC_FFT_SIZE                       (512U)
#define MUSIC_FFT_HALF_SIZE                  (MUSIC_FFT_SIZE / 2U)
#define MUSIC_FFT_MIC_FRAME_CAPACITY         (128U)
#define MUSIC_FFT_BLOCK_DURATION_MS          ((MUSIC_FFT_SIZE * 1000U) / MUSIC_FFT_SAMPLE_RATE_HZ)

/* One FFT covers 32 ms and each frequency bin represents 31.25 Hz. */
#define MUSIC_FFT_BASS_FIRST_BIN             (1U)
#define MUSIC_FFT_BASS_LAST_BIN              (8U)
#define MUSIC_FFT_MID_FIRST_BIN              (9U)
#define MUSIC_FFT_MID_LAST_BIN               (64U)
#define MUSIC_FFT_HIGH_FIRST_BIN             (65U)
#define MUSIC_FFT_HIGH_LAST_BIN              (192U)

/* 160 FFT blocks collect approximately 5.12 seconds of environmental noise. */
#define MUSIC_FFT_CALIBRATION_BLOCKS         (160U)
#define MUSIC_FFT_CALIBRATION_REPORT_STEP    (32U)
#define MUSIC_FFT_SAMPLE_SCALE               (1.0f / 32768.0f)
#define MUSIC_FFT_MAGNITUDE_SCALE            (4.0f / (float32_t) MUSIC_FFT_SIZE)
#define MUSIC_FFT_EVENT_THRESHOLD_RATIO      (3.0f)
#define MUSIC_FFT_NOISE_THRESHOLD_RATIO      (4.0f)
#define MUSIC_FFT_BASS_THRESHOLD_MULTIPLIER  (2.0f)
#define MUSIC_FFT_MID_THRESHOLD_MULTIPLIER   (0.75f)
#define MUSIC_FFT_HIGH_THRESHOLD_MULTIPLIER  (0.60f)
#define MUSIC_FFT_FLUX_FILTER_WEIGHT         (63.0f)
#define MUSIC_FFT_FLUX_FILTER_DIVISOR        (64.0f)
#define MUSIC_FFT_MIN_EVENT_GAP_BLOCKS       (8U)
#define MUSIC_FFT_MIN_FLUX_THRESHOLD         (0.000001f)

/* Rhythm is confirmed only by repeated events with similar strength and timing. */
#define MUSIC_RHYTHM_BAND_COUNT              (3U)
#define MUSIC_RHYTHM_MIN_INTERVAL_BLOCKS     (8U)
#define MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS     (47U)
#define MUSIC_RHYTHM_CONFIRM_HITS            (3U)
#define MUSIC_RHYTHM_MIN_TIME_TOLERANCE      (2U)
#define MUSIC_RHYTHM_TIME_TOLERANCE_DIVISOR  (5U)
#define MUSIC_RHYTHM_STRENGTH_MIN_RATIO      (0.65f)
#define MUSIC_RHYTHM_STRENGTH_MAX_RATIO      (1.45f)

/* One FFT block is 32 ms: verify for 5 s, pause after 3.5 s without beats, stop after 3 s of silence. */
#define MUSIC_DANCE_START_CONFIRM_BLOCKS     (157U)
#define MUSIC_DANCE_START_MIN_HITS           (8U)
#define MUSIC_DANCE_VERIFY_MAX_GAP_BLOCKS    (47U)
#define MUSIC_DANCE_BEAT_PAUSE_BLOCKS        (110U)
#define MUSIC_DANCE_STOP_SILENCE_BLOCKS      (94U)
#define MUSIC_DANCE_SERVO_COUNT              (5U)
#define MUSIC_DANCE_BASS_SERVO_COUNT         (2U)
#define MUSIC_DANCE_MID_SERVO_COUNT          (2U)
#define MUSIC_DANCE_HIGH_SERVO_COUNT         (1U)
#define MUSIC_DANCE_EVENT_SERVO_COUNT_MAX    (2U)
#define MUSIC_DANCE_MOVE_TIME_PERCENT        (75U)
#define MUSIC_DANCE_MIN_MOVE_TIME_MS         (120U)
#define MUSIC_DANCE_ACCELERATION_UNIT         (100U)
#define MUSIC_DANCE_DISTANCE_SAFETY_PERCENT  (80U)
#define MUSIC_DANCE_SCALE_PERMILLE           (1000U)
#define MUSIC_DANCE_DEFAULT_PERIOD_BLOCKS    (16U)

/* Servo 1 is limited to about 90 degrees in total: 45 degrees on each side. */
#define MUSIC_DANCE_SERVO1_CENTER_POSITION   (2084)
#define MUSIC_DANCE_SERVO1_HALF_RANGE_COUNTS (511)
#define MUSIC_DANCE_SERVO1_MIN_POSITION      \
    (MUSIC_DANCE_SERVO1_CENTER_POSITION - MUSIC_DANCE_SERVO1_HALF_RANGE_COUNTS)
#define MUSIC_DANCE_SERVO1_MAX_POSITION      \
    (MUSIC_DANCE_SERVO1_CENTER_POSITION + MUSIC_DANCE_SERVO1_HALF_RANGE_COUNTS)

typedef struct st_music_fft_bands
{
    float32_t bass;
    float32_t mid;
    float32_t high;
} music_fft_bands_t;

typedef enum e_music_fft_event
{
    MUSIC_FFT_EVENT_NONE = 0,
    MUSIC_FFT_EVENT_BASS,
    MUSIC_FFT_EVENT_MID,
    MUSIC_FFT_EVENT_HIGH,
} music_fft_event_t;

typedef struct st_music_rhythm_track
{
    bool valid;
    bool confirmed;
    uint8_t matched_hits;
    uint32_t age_blocks;
    uint32_t period_blocks;
    float32_t strength_reference;
} music_rhythm_track_t;

typedef struct st_music_dance_state
{
    bool bass_pose_b;
    bool mid_pose_b;
    bool high_pose_b;
    bool active;
    bool away_from_center;
    uint32_t verify_blocks;
    uint32_t verify_hit_count;
    uint32_t blocks_since_hit;
    uint32_t blocks_since_beat;
    uint32_t event_votes[MUSIC_RHYTHM_BAND_COUNT];
    float32_t event_score_sums[MUSIC_RHYTHM_BAND_COUNT];
    music_fft_event_t motion_event;
    int16_t commanded_positions[MUSIC_DANCE_SERVO_COUNT];
} music_dance_state_t;

static arm_rfft_fast_instance_f32 s_music_fft_instance;
static voice_sample_t s_music_fft_mic_frame[MUSIC_FFT_MIC_FRAME_CAPACITY];
static float32_t s_music_fft_input[MUSIC_FFT_SIZE];
static float32_t s_music_fft_output[MUSIC_FFT_SIZE];
static float32_t s_music_fft_window[MUSIC_FFT_SIZE];
static float32_t s_music_fft_magnitude[MUSIC_FFT_HALF_SIZE];
static float32_t s_music_fft_previous_magnitude[MUSIC_FFT_HALF_SIZE];
static bool s_music_fft_previous_valid;

static uint8_t const s_music_dance_all_servo_ids[MUSIC_DANCE_SERVO_COUNT] = {1U, 2U, 3U, 4U, 5U};
static uint16_t const s_music_dance_center_speeds[MUSIC_DANCE_SERVO_COUNT] = {800U, 800U, 850U, 800U, 900U};
static uint8_t const s_music_dance_center_accelerations[MUSIC_DANCE_SERVO_COUNT] = {14U, 14U, 10U, 14U, 16U};

/* Center pose is identical to the existing Servo_Power_on pose. */
static int16_t const s_music_dance_center[MUSIC_DANCE_SERVO_COUNT] =
{
    MUSIC_DANCE_SERVO1_CENTER_POSITION, 1795, 2603, 1266, 2832,
};

/* Bass moves the coupled shoulder/elbow axes around their known safe center. */
static uint8_t const s_music_dance_bass_servo_ids[MUSIC_DANCE_BASS_SERVO_COUNT] = {2U, 3U};
static uint16_t const s_music_dance_bass_speed_limits[MUSIC_DANCE_BASS_SERVO_COUNT] = {1000U, 1500U};
static uint8_t const s_music_dance_bass_accelerations[MUSIC_DANCE_BASS_SERVO_COUNT] = {30U, 45U};
static int16_t const s_music_dance_bass_poses[2][MUSIC_DANCE_BASS_SERVO_COUNT] =
{
    {1715, 2483},
    {1875, 2723},
};

/* Mid drives the base and side-tilt axes for a broad left/right sway. */
static uint8_t const s_music_dance_mid_servo_ids[MUSIC_DANCE_MID_SERVO_COUNT] = {1U, 4U};
static uint16_t const s_music_dance_mid_speed_limits[MUSIC_DANCE_MID_SERVO_COUNT] = {2200U, 2200U};
static uint8_t const s_music_dance_mid_accelerations[MUSIC_DANCE_MID_SERVO_COUNT] = {80U, 80U};
static int16_t const s_music_dance_mid_poses[2][MUSIC_DANCE_MID_SERVO_COUNT] =
{
    {1700, 1500},
    {2460, 1000},
};

/* High drives only the lamp-head axis for a quick accent. */
static uint8_t const s_music_dance_high_servo_ids[MUSIC_DANCE_HIGH_SERVO_COUNT] = {5U};
static uint16_t const s_music_dance_high_speed_limits[MUSIC_DANCE_HIGH_SERVO_COUNT] = {2000U};
static uint8_t const s_music_dance_high_accelerations[MUSIC_DANCE_HIGH_SERVO_COUNT] = {80U};
static int16_t const s_music_dance_high_poses[2][MUSIC_DANCE_HIGH_SERVO_COUNT] =
{
    {2670},
    {2970},
};

static float32_t music_fft_band_average(uint32_t first_bin, uint32_t last_bin)
{
    float32_t sum = 0.0f;

    for (uint32_t bin = first_bin; bin <= last_bin; bin++)
    {
        sum += s_music_fft_magnitude[bin];
    }

    return sum / (float32_t) ((last_bin - first_bin) + 1U);
}

static float32_t music_fft_band_positive_flux(uint32_t first_bin, uint32_t last_bin)
{
    float32_t sum = 0.0f;

    for (uint32_t bin = first_bin; bin <= last_bin; bin++)
    {
        float32_t const difference =
            s_music_fft_magnitude[bin] - s_music_fft_previous_magnitude[bin];

        /* Spectral flux keeps only newly increased frequency energy. */
        if (difference > 0.0f)
        {
            sum += difference;
        }
    }

    return sum / (float32_t) ((last_bin - first_bin) + 1U);
}

static void music_fft_analyze(music_fft_bands_t * p_bands, music_fft_bands_t * p_flux)
{
    float32_t mean = 0.0f;

    /* Removing the mean suppresses the DC bin caused by microphone offset. */
    for (uint32_t i = 0U; i < MUSIC_FFT_SIZE; i++)
    {
        mean += s_music_fft_input[i];
    }
    mean /= (float32_t) MUSIC_FFT_SIZE;

    /* The Hann window reduces spectral leakage between adjacent frequency bins. */
    for (uint32_t i = 0U; i < MUSIC_FFT_SIZE; i++)
    {
        s_music_fft_input[i] = (s_music_fft_input[i] - mean) * s_music_fft_window[i];
    }

    arm_rfft_fast_f32(&s_music_fft_instance, s_music_fft_input, s_music_fft_output, 0U);

    /*
     * In CMSIS real-FFT output, bins 1..255 start at output index 2 as
     * real/imaginary pairs. DC and the Nyquist value are not used here.
     */
    s_music_fft_magnitude[0] = 0.0f;
    arm_cmplx_mag_f32(&s_music_fft_output[2],
                      &s_music_fft_magnitude[1],
                      MUSIC_FFT_HALF_SIZE - 1U);

    for (uint32_t bin = 1U; bin < MUSIC_FFT_HALF_SIZE; bin++)
    {
        s_music_fft_magnitude[bin] *= MUSIC_FFT_MAGNITUDE_SCALE;
    }

    /* Average each band so that a wider band does not win only because it has more bins. */
    p_bands->bass = music_fft_band_average(MUSIC_FFT_BASS_FIRST_BIN, MUSIC_FFT_BASS_LAST_BIN);
    p_bands->mid = music_fft_band_average(MUSIC_FFT_MID_FIRST_BIN, MUSIC_FFT_MID_LAST_BIN);
    p_bands->high = music_fft_band_average(MUSIC_FFT_HIGH_FIRST_BIN, MUSIC_FFT_HIGH_LAST_BIN);

    if (s_music_fft_previous_valid)
    {
        p_flux->bass = music_fft_band_positive_flux(MUSIC_FFT_BASS_FIRST_BIN, MUSIC_FFT_BASS_LAST_BIN);
        p_flux->mid = music_fft_band_positive_flux(MUSIC_FFT_MID_FIRST_BIN, MUSIC_FFT_MID_LAST_BIN);
        p_flux->high = music_fft_band_positive_flux(MUSIC_FFT_HIGH_FIRST_BIN, MUSIC_FFT_HIGH_LAST_BIN);
    }
    else
    {
        p_flux->bass = 0.0f;
        p_flux->mid = 0.0f;
        p_flux->high = 0.0f;
    }

    for (uint32_t bin = 0U; bin < MUSIC_FFT_HALF_SIZE; bin++)
    {
        s_music_fft_previous_magnitude[bin] = s_music_fft_magnitude[bin];
    }
    s_music_fft_previous_valid = true;
}

static float32_t music_fft_remove_noise(float32_t level, float32_t noise_level)
{
    return (level > noise_level) ? (level - noise_level) : 0.0f;
}

static float32_t music_fft_flux_threshold(float32_t adaptive_flux, float32_t noise_flux)
{
    float32_t threshold = adaptive_flux * MUSIC_FFT_EVENT_THRESHOLD_RATIO;
    float32_t const noise_threshold = noise_flux * MUSIC_FFT_NOISE_THRESHOLD_RATIO;

    if (noise_threshold > threshold)
    {
        threshold = noise_threshold;
    }
    if (threshold < MUSIC_FFT_MIN_FLUX_THRESHOLD)
    {
        threshold = MUSIC_FFT_MIN_FLUX_THRESHOLD;
    }

    return threshold;
}

static float32_t music_fft_flux_filter(float32_t previous, float32_t current)
{
    return ((previous * MUSIC_FFT_FLUX_FILTER_WEIGHT) + current) /
           MUSIC_FFT_FLUX_FILTER_DIVISOR;
}

static char const * music_fft_event_name(music_fft_event_t event)
{
    if (MUSIC_FFT_EVENT_BASS == event)
    {
        return "BASS";
    }
    if (MUSIC_FFT_EVENT_MID == event)
    {
        return "MID";
    }
    if (MUSIC_FFT_EVENT_HIGH == event)
    {
        return "HIGH";
    }

    return "NONE";
}

static music_fft_event_t music_fft_dominant_event(music_fft_bands_t const * p_scores,
                                                   float32_t * p_dominant_score)
{
    music_fft_event_t dominant_event = MUSIC_FFT_EVENT_NONE;
    float32_t dominant_score = 0.0f;

    if (NULL != p_scores)
    {
        if (p_scores->bass > dominant_score)
        {
            dominant_event = MUSIC_FFT_EVENT_BASS;
            dominant_score = p_scores->bass;
        }
        if ((p_scores->mid >= dominant_score) && (p_scores->mid > 0.0f))
        {
            dominant_event = MUSIC_FFT_EVENT_MID;
            dominant_score = p_scores->mid;
        }
        if ((p_scores->high >= dominant_score) && (p_scores->high > 0.0f))
        {
            /* An exact tie is resolved toward HIGH instead of causing a LOW/MID twitch. */
            dominant_event = MUSIC_FFT_EVENT_HIGH;
            dominant_score = p_scores->high;
        }
    }

    if (NULL != p_dominant_score)
    {
        *p_dominant_score = dominant_score;
    }

    return dominant_event;
}

static void music_rhythm_track_reset(music_rhythm_track_t * p_track)
{
    p_track->valid = false;
    p_track->confirmed = false;
    p_track->matched_hits = 0U;
    p_track->age_blocks = 0U;
    p_track->period_blocks = 0U;
    p_track->strength_reference = 0.0f;
}

static void music_rhythm_reset_all(music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT])
{
    for (uint32_t i = 0U; i < MUSIC_RHYTHM_BAND_COUNT; i++)
    {
        music_rhythm_track_reset(&tracks[i]);
    }
}

static music_rhythm_track_t * music_rhythm_track_get(
    music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT],
    music_fft_event_t event)
{
    if ((event < MUSIC_FFT_EVENT_BASS) || (event > MUSIC_FFT_EVENT_HIGH))
    {
        return NULL;
    }

    return &tracks[(uint32_t) event - (uint32_t) MUSIC_FFT_EVENT_BASS];
}

static void music_rhythm_track_seed(music_rhythm_track_t * p_track, float32_t strength)
{
    music_rhythm_track_reset(p_track);
    p_track->valid = true;
    p_track->matched_hits = 1U;
    p_track->strength_reference = strength;
}

static uint32_t music_rhythm_interval_tolerance(uint32_t interval_blocks)
{
    uint32_t tolerance = interval_blocks / MUSIC_RHYTHM_TIME_TOLERANCE_DIVISOR;

    if (tolerance < MUSIC_RHYTHM_MIN_TIME_TOLERANCE)
    {
        tolerance = MUSIC_RHYTHM_MIN_TIME_TOLERANCE;
    }

    return tolerance;
}

static bool music_rhythm_interval_matches(uint32_t measured_blocks, uint32_t expected_blocks)
{
    uint32_t const difference = (measured_blocks > expected_blocks) ?
                                (measured_blocks - expected_blocks) :
                                (expected_blocks - measured_blocks);

    return difference <= music_rhythm_interval_tolerance(expected_blocks);
}

static bool music_rhythm_strength_matches(music_rhythm_track_t const * p_track, float32_t strength)
{
    return (strength >= (p_track->strength_reference * MUSIC_RHYTHM_STRENGTH_MIN_RATIO)) &&
           (strength <= (p_track->strength_reference * MUSIC_RHYTHM_STRENGTH_MAX_RATIO));
}

static void music_rhythm_reference_update(music_rhythm_track_t * p_track,
                                          float32_t strength,
                                          uint32_t measured_period_blocks)
{
    /* A 3:1 moving average follows gradual changes without chasing a single accent. */
    p_track->strength_reference = ((p_track->strength_reference * 3.0f) + strength) / 4.0f;
    p_track->period_blocks = ((p_track->period_blocks * 3U) + measured_period_blocks + 2U) / 4U;
    p_track->age_blocks = 0U;
}

static void music_rhythm_time_update(music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT])
{
    for (uint32_t i = 0U; i < MUSIC_RHYTHM_BAND_COUNT; i++)
    {
        music_rhythm_track_t * const p_track = &tracks[i];
        uint32_t max_age_blocks = MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS;

        if (!p_track->valid)
        {
            continue;
        }

        if (p_track->age_blocks < UINT32_MAX)
        {
            p_track->age_blocks++;
        }

        if (p_track->confirmed && (p_track->period_blocks > 0U))
        {
            uint32_t const double_period = p_track->period_blocks * 2U;

            /* Keep a locked rhythm alive long enough to tolerate one missed beat. */
            max_age_blocks = double_period + music_rhythm_interval_tolerance(double_period);
        }

        if (p_track->age_blocks > max_age_blocks)
        {
            music_rhythm_track_reset(p_track);
        }
    }
}

static bool music_rhythm_event_confirmed(
    music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT],
    music_fft_event_t event)
{
    music_rhythm_track_t * const p_track = music_rhythm_track_get(tracks, event);

    return (NULL != p_track) && p_track->confirmed;
}

static uint32_t music_rhythm_period_get(
    music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT],
    music_fft_event_t event)
{
    music_rhythm_track_t * const p_track = music_rhythm_track_get(tracks, event);

    return (NULL != p_track) ? p_track->period_blocks : 0U;
}

static bool music_rhythm_register_candidate(
    music_rhythm_track_t tracks[MUSIC_RHYTHM_BAND_COUNT],
    music_fft_event_t event,
    float32_t strength)
{
    music_rhythm_track_t * const p_track = music_rhythm_track_get(tracks, event);
    uint32_t measured_period;

    if ((NULL == p_track) || (strength <= 0.0f))
    {
        return false;
    }

    if (!p_track->valid)
    {
        music_rhythm_track_seed(p_track, strength);
        return false;
    }

    measured_period = p_track->age_blocks;

    if (!p_track->confirmed && (measured_period > MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS))
    {
        music_rhythm_track_seed(p_track, strength);
        return false;
    }

    if (measured_period < MUSIC_RHYTHM_MIN_INTERVAL_BLOCKS)
    {
        return false;
    }

    /* An event with a different strength does not disturb the rhythm being learned. */
    if (!music_rhythm_strength_matches(p_track, strength))
    {
        return false;
    }

    if (p_track->confirmed)
    {
        uint32_t normalized_period = 0U;

        if (music_rhythm_interval_matches(measured_period, p_track->period_blocks))
        {
            normalized_period = measured_period;
        }
        else if (music_rhythm_interval_matches(measured_period, p_track->period_blocks * 2U))
        {
            /* A double interval means one expected beat was probably missed. */
            normalized_period = (measured_period + 1U) / 2U;
        }
        else
        {
            return false;
        }

        if ((normalized_period < MUSIC_RHYTHM_MIN_INTERVAL_BLOCKS) ||
            (normalized_period > MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS))
        {
            return false;
        }

        music_rhythm_reference_update(p_track, strength, normalized_period);
        return true;
    }

    if (1U == p_track->matched_hits)
    {
        p_track->period_blocks = measured_period;
        p_track->matched_hits = 2U;
        p_track->strength_reference = ((p_track->strength_reference * 3.0f) + strength) / 4.0f;
        p_track->age_blocks = 0U;
        return false;
    }

    if (!music_rhythm_interval_matches(measured_period, p_track->period_blocks))
    {
        /* This event becomes the first sample of a possible new rhythm. */
        music_rhythm_track_seed(p_track, strength);
        return false;
    }

    music_rhythm_reference_update(p_track, strength, measured_period);
    if (p_track->matched_hits < MUSIC_RHYTHM_CONFIRM_HITS)
    {
        p_track->matched_hits++;
    }

    if (p_track->matched_hits >= MUSIC_RHYTHM_CONFIRM_HITS)
    {
        p_track->confirmed = true;
        return true;
    }

    return false;
}

static void music_dance_commanded_positions_reset(music_dance_state_t * p_state)
{
    for (uint32_t i = 0U; i < MUSIC_DANCE_SERVO_COUNT; i++)
    {
        p_state->commanded_positions[i] = s_music_dance_center[i];
    }
}

static uint32_t music_dance_move_time_get(uint32_t period_blocks)
{
    uint32_t period_ms;
    uint32_t move_time_ms;

    if (0U == period_blocks)
    {
        period_blocks = MUSIC_DANCE_DEFAULT_PERIOD_BLOCKS;
    }
    else if (period_blocks < MUSIC_RHYTHM_MIN_INTERVAL_BLOCKS)
    {
        period_blocks = MUSIC_RHYTHM_MIN_INTERVAL_BLOCKS;
    }
    else if (period_blocks > MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS)
    {
        period_blocks = MUSIC_RHYTHM_MAX_INTERVAL_BLOCKS;
    }

    period_ms = period_blocks * MUSIC_FFT_BLOCK_DURATION_MS;
    move_time_ms = (period_ms * MUSIC_DANCE_MOVE_TIME_PERCENT) / 100U;
    if (move_time_ms < MUSIC_DANCE_MIN_MOVE_TIME_MS)
    {
        move_time_ms = MUSIC_DANCE_MIN_MOVE_TIME_MS;
    }

    return move_time_ms;
}

static uint32_t music_dance_axis_max_distance(uint32_t period_blocks,
                                              uint16_t speed_limit,
                                              uint8_t acceleration)
{
    uint64_t const move_time_ms = music_dance_move_time_get(period_blocks);
    uint64_t const acceleration_counts =
        (uint64_t) acceleration * MUSIC_DANCE_ACCELERATION_UNIT;
    uint64_t max_distance;

    if ((0U == speed_limit) || (0U == acceleration_counts))
    {
        return 1U;
    }

    /*
     * SMS/STS acceleration units are approximately 100 position counts/s^2,
     * while one speed unit is approximately one position count/s.  Calculate a
     * conservative rest-to-rest distance so the motion can finish before the
     * next beat instead of being overwritten by the following command.
     */
    if ((acceleration_counts * move_time_ms) <=
        ((uint64_t) 2U * speed_limit * 1000U))
    {
        /* Triangular profile: the axis never reaches its configured speed limit. */
        max_distance = (acceleration_counts * move_time_ms * move_time_ms) / 4000000U;
    }
    else
    {
        /* Trapezoidal profile: accelerate, run at the speed limit, then decelerate. */
        max_distance = ((uint64_t) speed_limit *
                        ((acceleration_counts * move_time_ms) -
                         ((uint64_t) speed_limit * 1000U))) /
                       (acceleration_counts * 1000U);
    }

    max_distance = (max_distance * MUSIC_DANCE_DISTANCE_SAFETY_PERCENT) / 100U;
    if (0U == max_distance)
    {
        max_distance = 1U;
    }

    return (max_distance > UINT32_MAX) ? UINT32_MAX : (uint32_t) max_distance;
}

static uint32_t music_dance_scale_limit(uint32_t scale,
                                        int32_t distance,
                                        uint32_t max_distance)
{
    uint32_t absolute_distance = (distance < 0) ?
                                 (uint32_t) (-distance) :
                                 (uint32_t) distance;

    if ((absolute_distance > max_distance) && (absolute_distance > 0U))
    {
        uint32_t const limited_scale =
            (uint32_t) (((uint64_t) max_distance * MUSIC_DANCE_SCALE_PERMILLE) /
                        absolute_distance);

        if (limited_scale < scale)
        {
            scale = limited_scale;
        }
    }

    return scale;
}

static int16_t music_dance_position_interpolate(int16_t start_position,
                                                int16_t end_position,
                                                uint32_t scale)
{
    int32_t const distance = (int32_t) end_position - start_position;

    return (int16_t) ((int32_t) start_position +
                      ((distance * (int32_t) scale) / MUSIC_DANCE_SCALE_PERMILLE));
}

static int16_t music_dance_position_limit(uint8_t servo_id, int16_t position)
{
    if (1U == servo_id)
    {
        if (position < MUSIC_DANCE_SERVO1_MIN_POSITION)
        {
            position = MUSIC_DANCE_SERVO1_MIN_POSITION;
        }
        else if (position > MUSIC_DANCE_SERVO1_MAX_POSITION)
        {
            position = MUSIC_DANCE_SERVO1_MAX_POSITION;
        }
    }

    return position;
}

static bool music_dance_group_plan(uint8_t const * p_servo_ids,
                                   uint8_t servo_count,
                                   int16_t const * p_requested_positions,
                                   int16_t const * p_opposite_positions,
                                   uint16_t const * p_speed_limits,
                                   uint8_t const * p_accelerations,
                                   music_dance_state_t const * p_state,
                                   uint32_t period_blocks,
                                   int16_t planned_positions[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX],
                                   uint16_t planned_speeds[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX])
{
    uint32_t max_distances[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX] = {0U};
    int16_t symmetric_targets[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX] = {0};
    uint32_t pose_scale = MUSIC_DANCE_SCALE_PERMILLE;
    uint32_t movement_scale = MUSIC_DANCE_SCALE_PERMILLE;

    if ((NULL == p_servo_ids) || (NULL == p_requested_positions) ||
        (NULL == p_opposite_positions) || (NULL == p_speed_limits) ||
        (NULL == p_accelerations) || (NULL == p_state) ||
        (NULL == planned_positions) || (NULL == planned_speeds) ||
        (0U == servo_count) || (servo_count > MUSIC_DANCE_EVENT_SERVO_COUNT_MAX))
    {
        return false;
    }

    for (uint32_t i = 0U; i < servo_count; i++)
    {
        uint8_t const servo_id = p_servo_ids[i];

        if ((0U == servo_id) || (servo_id > MUSIC_DANCE_SERVO_COUNT))
        {
            return false;
        }

        max_distances[i] = music_dance_axis_max_distance(period_blocks,
                                                         p_speed_limits[i],
                                                         p_accelerations[i]);
        pose_scale = music_dance_scale_limit(
            pose_scale,
            (int32_t) p_requested_positions[i] - p_opposite_positions[i],
            max_distances[i]);
    }

    for (uint32_t i = 0U; i < servo_count; i++)
    {
        uint8_t const servo_id = p_servo_ids[i];
        int16_t const midpoint =
            (int16_t) (((int32_t) p_requested_positions[i] + p_opposite_positions[i]) / 2);

        /* All axes share pose_scale so shortening the gesture does not distort its direction. */
        symmetric_targets[i] = music_dance_position_interpolate(midpoint,
                                                                 p_requested_positions[i],
                                                                 pose_scale);
        movement_scale = music_dance_scale_limit(
            movement_scale,
            (int32_t) symmetric_targets[i] - p_state->commanded_positions[servo_id - 1U],
            max_distances[i]);
    }

    for (uint32_t i = 0U; i < servo_count; i++)
    {
        uint8_t const servo_id = p_servo_ids[i];

        /* Limit the complete group together; all axes start from the same confirmed beat. */
        planned_positions[i] = music_dance_position_interpolate(
            p_state->commanded_positions[servo_id - 1U],
            symmetric_targets[i],
            movement_scale);
        planned_positions[i] = music_dance_position_limit(servo_id, planned_positions[i]);
        planned_speeds[i] = p_speed_limits[i];
    }

    return true;
}

static bool music_dance_apply_event(music_fft_event_t event,
                                    music_dance_state_t * p_state,
                                    uint32_t period_blocks)
{
    uint8_t const * p_servo_ids = NULL;
    int16_t const * p_positions = NULL;
    int16_t const * p_opposite_positions = NULL;
    uint16_t const * p_speed_limits = NULL;
    uint8_t const * p_accelerations = NULL;
    int16_t planned_positions[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX] = {0};
    uint16_t planned_speeds[MUSIC_DANCE_EVENT_SERVO_COUNT_MAX] = {0U};
    uint8_t servo_count = 0U;
    bool next_pose_b = false;

    if (NULL == p_state)
    {
        return false;
    }

    if (MUSIC_FFT_EVENT_BASS == event)
    {
        next_pose_b = !p_state->bass_pose_b;
        p_servo_ids = s_music_dance_bass_servo_ids;
        p_positions = s_music_dance_bass_poses[next_pose_b ? 1U : 0U];
        p_opposite_positions = s_music_dance_bass_poses[next_pose_b ? 0U : 1U];
        p_speed_limits = s_music_dance_bass_speed_limits;
        p_accelerations = s_music_dance_bass_accelerations;
        servo_count = MUSIC_DANCE_BASS_SERVO_COUNT;
    }
    else if (MUSIC_FFT_EVENT_MID == event)
    {
        next_pose_b = !p_state->mid_pose_b;
        p_servo_ids = s_music_dance_mid_servo_ids;
        p_positions = s_music_dance_mid_poses[next_pose_b ? 1U : 0U];
        p_opposite_positions = s_music_dance_mid_poses[next_pose_b ? 0U : 1U];
        p_speed_limits = s_music_dance_mid_speed_limits;
        p_accelerations = s_music_dance_mid_accelerations;
        servo_count = MUSIC_DANCE_MID_SERVO_COUNT;
    }
    else if (MUSIC_FFT_EVENT_HIGH == event)
    {
        next_pose_b = !p_state->high_pose_b;
        p_servo_ids = s_music_dance_high_servo_ids;
        p_positions = s_music_dance_high_poses[next_pose_b ? 1U : 0U];
        p_opposite_positions = s_music_dance_high_poses[next_pose_b ? 0U : 1U];
        p_speed_limits = s_music_dance_high_speed_limits;
        p_accelerations = s_music_dance_high_accelerations;
        servo_count = MUSIC_DANCE_HIGH_SERVO_COUNT;
    }

    if (!music_dance_group_plan(p_servo_ids,
                                servo_count,
                                p_positions,
                                p_opposite_positions,
                                p_speed_limits,
                                p_accelerations,
                                p_state,
                                period_blocks,
                                planned_positions,
                                planned_speeds))
    {
        return false;
    }

    if (0 == Servo_SyncWritePos(p_servo_ids,
                                 servo_count,
                                 planned_positions,
                                 planned_speeds,
                                 p_accelerations))
    {
        return false;
    }

    for (uint32_t i = 0U; i < servo_count; i++)
    {
        p_state->commanded_positions[p_servo_ids[i] - 1U] = planned_positions[i];
    }

    /* Only change the alternating direction after the command was accepted. */
    if (MUSIC_FFT_EVENT_BASS == event)
    {
        p_state->bass_pose_b = next_pose_b;
    }
    else if (MUSIC_FFT_EVENT_MID == event)
    {
        p_state->mid_pose_b = next_pose_b;
    }
    else
    {
        p_state->high_pose_b = next_pose_b;
    }

    p_state->away_from_center = true;
    p_state->blocks_since_beat = 0U;
    return true;
}

static bool music_dance_return_center(music_dance_state_t * p_state)
{
    if (0 == Servo_SyncWritePos(s_music_dance_all_servo_ids,
                                MUSIC_DANCE_SERVO_COUNT,
                                s_music_dance_center,
                                s_music_dance_center_speeds,
                                s_music_dance_center_accelerations))
    {
        return false;
    }

    p_state->bass_pose_b = false;
    p_state->mid_pose_b = false;
    p_state->high_pose_b = false;
    p_state->away_from_center = false;
    p_state->blocks_since_beat = 0U;
    music_dance_commanded_positions_reset(p_state);
    return true;
}

static bool music_dance_event_index_get(music_fft_event_t event, uint32_t * p_index)
{
    if ((event < MUSIC_FFT_EVENT_BASS) || (event > MUSIC_FFT_EVENT_HIGH) ||
        (NULL == p_index))
    {
        return false;
    }

    *p_index = (uint32_t) event - (uint32_t) MUSIC_FFT_EVENT_BASS;
    return true;
}

static void music_dance_event_vote_add(music_dance_state_t * p_state,
                                       music_fft_event_t event,
                                       float32_t score)
{
    uint32_t event_index;

    if ((NULL == p_state) || !music_dance_event_index_get(event, &event_index))
    {
        return;
    }

    if (p_state->event_votes[event_index] < UINT32_MAX)
    {
        p_state->event_votes[event_index]++;
    }
    p_state->event_score_sums[event_index] += score;
}

static music_fft_event_t music_dance_event_vote_winner(
    music_dance_state_t const * p_state,
    music_fft_event_t preferred_event)
{
    uint32_t max_votes = 0U;
    uint32_t preferred_index;
    music_fft_event_t winner = MUSIC_FFT_EVENT_NONE;
    float32_t winner_score = 0.0f;

    if (NULL == p_state)
    {
        return MUSIC_FFT_EVENT_NONE;
    }

    for (uint32_t i = 0U; i < MUSIC_RHYTHM_BAND_COUNT; i++)
    {
        if (p_state->event_votes[i] > max_votes)
        {
            max_votes = p_state->event_votes[i];
        }
    }

    if (0U == max_votes)
    {
        return MUSIC_FFT_EVENT_NONE;
    }

    /* On a vote tie, keep the already confirmed rhythm source when possible. */
    if (music_dance_event_index_get(preferred_event, &preferred_index) &&
        (p_state->event_votes[preferred_index] == max_votes))
    {
        return preferred_event;
    }

    for (uint32_t i = 0U; i < MUSIC_RHYTHM_BAND_COUNT; i++)
    {
        if ((p_state->event_votes[i] == max_votes) &&
            ((MUSIC_FFT_EVENT_NONE == winner) ||
             (p_state->event_score_sums[i] >= winner_score)))
        {
            winner = (music_fft_event_t) ((uint32_t) MUSIC_FFT_EVENT_BASS + i);
            winner_score = p_state->event_score_sums[i];
        }
    }

    return winner;
}

static void music_dance_verification_reset(music_dance_state_t * p_state)
{
    p_state->verify_blocks = 0U;
    p_state->verify_hit_count = 0U;
    p_state->blocks_since_hit = 0U;
    p_state->blocks_since_beat = 0U;
    p_state->motion_event = MUSIC_FFT_EVENT_NONE;

    for (uint32_t i = 0U; i < MUSIC_RHYTHM_BAND_COUNT; i++)
    {
        p_state->event_votes[i] = 0U;
        p_state->event_score_sums[i] = 0.0f;
    }
}

static void music_dance_time_update(music_dance_state_t * p_state)
{
    if (p_state->blocks_since_hit < UINT32_MAX)
    {
        p_state->blocks_since_hit++;
    }
    if (p_state->blocks_since_beat < UINT32_MAX)
    {
        p_state->blocks_since_beat++;
    }

    if (!p_state->active && (p_state->verify_hit_count > 0U))
    {
        if (p_state->verify_blocks < UINT32_MAX)
        {
            p_state->verify_blocks++;
        }

        /* A long gap means the earlier hits were isolated noise, not continuous music. */
        if (p_state->blocks_since_hit > MUSIC_DANCE_VERIFY_MAX_GAP_BLOCKS)
        {
            music_dance_verification_reset(p_state);
        }
    }
}

static bool music_dance_register_hit(music_dance_state_t * p_state,
                                     music_fft_event_t dominant_event,
                                     float32_t dominant_score,
                                     music_fft_event_t preferred_event)
{
    p_state->blocks_since_hit = 0U;

    if (p_state->active)
    {
        return false;
    }

    if (0U == p_state->verify_hit_count)
    {
        p_state->verify_blocks = 0U;
    }

    /* One globally de-duplicated onset contributes exactly one frequency vote. */
    music_dance_event_vote_add(p_state, dominant_event, dominant_score);

    if (p_state->verify_hit_count < UINT32_MAX)
    {
        p_state->verify_hit_count++;
    }

    if ((p_state->verify_blocks >= MUSIC_DANCE_START_CONFIRM_BLOCKS) &&
        (p_state->verify_hit_count >= MUSIC_DANCE_START_MIN_HITS))
    {
        p_state->motion_event = music_dance_event_vote_winner(p_state, preferred_event);
        if (MUSIC_FFT_EVENT_NONE == p_state->motion_event)
        {
            return false;
        }

        p_state->active = true;
        p_state->verify_blocks = 0U;
        p_state->verify_hit_count = 0U;
        return true;
    }

    return false;
}

void app_music_fft_test(void)
{
    music_fft_bands_t noise = {0.0f, 0.0f, 0.0f};
    music_fft_bands_t noise_flux = {0.0f, 0.0f, 0.0f};
    music_fft_bands_t adaptive_flux = {0.0f, 0.0f, 0.0f};
    music_rhythm_track_t rhythm_tracks[MUSIC_RHYTHM_BAND_COUNT] = {0};
    music_dance_state_t dance_state = {0};
    music_fft_event_t rhythm_source = MUSIC_FFT_EVENT_NONE;
    uint32_t fft_sample_count = 0U;
    uint32_t calibration_count = 0U;
    uint32_t last_frame_counter = 0U;
    uint32_t raw_hit_gap_blocks = MUSIC_FFT_MIN_EVENT_GAP_BLOCKS;
    uint32_t event_count = 0U;
    bool last_frame_valid = false;
    fsp_err_t err;

    if (ARM_MATH_SUCCESS != arm_rfft_fast_init_f32(&s_music_fft_instance, MUSIC_FFT_SIZE))
    {
        app_fatal_error("Music FFT initialization", FSP_ERR_INTERNAL);
    }

    arm_hanning_f32(s_music_fft_window, MUSIC_FFT_SIZE);
    s_music_fft_previous_valid = false;

    /* Move to the known safe pose before microphone calibration starts. */
    Servo_Init();
    Servo_NormalMode();
    Servo_Power_on();
    music_dance_commanded_positions_reset(&dance_state);
    R_BSP_SoftwareDelay(1000U, BSP_DELAY_UNITS_MILLISECONDS);

    Voice_ChannelSet(VOICE_CHANNEL_RIGHT);
    err = Voice_Start();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Music FFT microphone start", err);
    }

    printf("Music FFT test started: 512 points, bin=31.25 Hz.\r\n");
    printf("Keep quiet for about 5 seconds while the frequency noise floor is measured.\r\n");

    while (1)
    {
        uint32_t frame_counter;
        uint32_t frame_count;

        if (!Voice_FrameReady())
        {
            __WFI();
            continue;
        }

        frame_counter = Voice_FrameCounterGet();
        frame_count = Voice_FrameRead(s_music_fft_mic_frame, MUSIC_FFT_MIC_FRAME_CAPACITY);
        if (0U == frame_count)
        {
            continue;
        }

        if (last_frame_valid)
        {
            uint32_t const frame_delta = frame_counter - last_frame_counter;

            if (frame_delta > 1U)
            {
                /* Discard an incomplete FFT block when the time-domain samples are discontinuous. */
                fft_sample_count = 0U;
                s_music_fft_previous_valid = false;
                music_rhythm_reset_all(rhythm_tracks);
                rhythm_source = MUSIC_FFT_EVENT_NONE;
                raw_hit_gap_blocks = MUSIC_FFT_MIN_EVENT_GAP_BLOCKS;

                /* Do not join verification hits across a discontinuous microphone timeline. */
                if (!dance_state.active)
                {
                    music_dance_verification_reset(&dance_state);
                }
            }
        }

        last_frame_counter = frame_counter;
        last_frame_valid = true;

        for (uint32_t i = 0U; i < frame_count; i++)
        {
            s_music_fft_input[fft_sample_count++] =
                (float32_t) s_music_fft_mic_frame[i] * MUSIC_FFT_SAMPLE_SCALE;

            if (fft_sample_count >= MUSIC_FFT_SIZE)
            {
                music_fft_bands_t bands;
                music_fft_bands_t flux;

                music_fft_analyze(&bands, &flux);
                fft_sample_count = 0U;

                if (calibration_count < MUSIC_FFT_CALIBRATION_BLOCKS)
                {
                    float32_t const divisor = (float32_t) (calibration_count + 1U);

                    /* Running averages form one independent noise floor for each frequency band. */
                    noise.bass += (bands.bass - noise.bass) / divisor;
                    noise.mid += (bands.mid - noise.mid) / divisor;
                    noise.high += (bands.high - noise.high) / divisor;
                    noise_flux.bass += (flux.bass - noise_flux.bass) / divisor;
                    noise_flux.mid += (flux.mid - noise_flux.mid) / divisor;
                    noise_flux.high += (flux.high - noise_flux.high) / divisor;
                    calibration_count++;

                    if ((0U == (calibration_count % MUSIC_FFT_CALIBRATION_REPORT_STEP)) ||
                        (calibration_count == MUSIC_FFT_CALIBRATION_BLOCKS))
                    {
                        printf("FFT calibration: %lu/%lu\r\n",
                               (unsigned long) calibration_count,
                               (unsigned long) MUSIC_FFT_CALIBRATION_BLOCKS);
                    }

                    if (calibration_count == MUSIC_FFT_CALIBRATION_BLOCKS)
                    {
                        adaptive_flux = noise_flux;
                        music_rhythm_reset_all(rhythm_tracks);
                        rhythm_source = MUSIC_FFT_EVENT_NONE;
                        printf("FFT calibration complete. Start playing music.\r\n");
                    }
                }
                else
                {
                    music_fft_bands_t flux_threshold;
                    music_fft_bands_t event_scores = {0.0f, 0.0f, 0.0f};
                    music_fft_event_t dominant_event = MUSIC_FFT_EVENT_NONE;
                    music_fft_event_t rhythm_event = MUSIC_FFT_EVENT_NONE;
                    float32_t dominant_score = 0.0f;
                    float32_t rhythm_event_score = 0.0f;
                    bool bass_beat = false;
                    bool mid_beat = false;
                    bool high_beat = false;
                    bool raw_hit_detected = false;

                    bands.bass = music_fft_remove_noise(bands.bass, noise.bass);
                    bands.mid = music_fft_remove_noise(bands.mid, noise.mid);
                    bands.high = music_fft_remove_noise(bands.high, noise.high);

                    /*
                     * Bass contains more room vibration, so its threshold is raised.
                     * Mid and high thresholds are lowered slightly to retain more musical details.
                     */
                    flux_threshold.bass =
                        music_fft_flux_threshold(adaptive_flux.bass, noise_flux.bass) *
                        MUSIC_FFT_BASS_THRESHOLD_MULTIPLIER;
                    flux_threshold.mid =
                        music_fft_flux_threshold(adaptive_flux.mid, noise_flux.mid) *
                        MUSIC_FFT_MID_THRESHOLD_MULTIPLIER;
                    flux_threshold.high =
                        music_fft_flux_threshold(adaptive_flux.high, noise_flux.high) *
                        MUSIC_FFT_HIGH_THRESHOLD_MULTIPLIER;

                    if (raw_hit_gap_blocks < UINT32_MAX)
                    {
                        raw_hit_gap_blocks++;
                    }

                    /* Advance music verification, dance silence, and all three rhythm clocks. */
                    music_dance_time_update(&dance_state);
                    music_rhythm_time_update(rhythm_tracks);

                    if ((MUSIC_FFT_EVENT_NONE != rhythm_source) &&
                        !music_rhythm_event_confirmed(rhythm_tracks, rhythm_source))
                    {
                        printf("RHYTHM: LOST %s\r\n", music_fft_event_name(rhythm_source));
                        rhythm_source = MUSIC_FFT_EVENT_NONE;
                    }

                    /* Each band now produces an independent normalized onset-strength candidate. */
                    if ((bands.bass > 0.0f) && (flux.bass > flux_threshold.bass))
                    {
                        event_scores.bass = flux.bass / flux_threshold.bass;
                    }
                    if ((bands.mid > 0.0f) && (flux.mid > flux_threshold.mid))
                    {
                        event_scores.mid = flux.mid / flux_threshold.mid;
                    }
                    if ((bands.high > 0.0f) && (flux.high > flux_threshold.high))
                    {
                        event_scores.high = flux.high / flux_threshold.high;
                    }

                    dominant_event = music_fft_dominant_event(&event_scores, &dominant_score);

                    /*
                     * Accept at most one band every 256 ms. This prevents one broad drum hit
                     * from updating several band trackers in adjacent FFT blocks.
                     */
                    if ((raw_hit_gap_blocks >= MUSIC_FFT_MIN_EVENT_GAP_BLOCKS) &&
                        (MUSIC_FFT_EVENT_NONE != dominant_event))
                    {
                        raw_hit_detected = true;
                        raw_hit_gap_blocks = 0U;

                        if (MUSIC_FFT_EVENT_BASS == dominant_event)
                        {
                            bass_beat = music_rhythm_register_candidate(rhythm_tracks,
                                                                        dominant_event,
                                                                        dominant_score);
                        }
                        else if (MUSIC_FFT_EVENT_MID == dominant_event)
                        {
                            mid_beat = music_rhythm_register_candidate(rhythm_tracks,
                                                                       dominant_event,
                                                                       dominant_score);
                        }
                        else
                        {
                            high_beat = music_rhythm_register_candidate(rhythm_tracks,
                                                                        dominant_event,
                                                                        dominant_score);
                        }
                    }

                    if (MUSIC_FFT_EVENT_BASS == rhythm_source)
                    {
                        rhythm_event = bass_beat ? MUSIC_FFT_EVENT_BASS : MUSIC_FFT_EVENT_NONE;
                    }
                    else if (MUSIC_FFT_EVENT_MID == rhythm_source)
                    {
                        rhythm_event = mid_beat ? MUSIC_FFT_EVENT_MID : MUSIC_FFT_EVENT_NONE;
                    }
                    else if (MUSIC_FFT_EVENT_HIGH == rhythm_source)
                    {
                        rhythm_event = high_beat ? MUSIC_FFT_EVENT_HIGH : MUSIC_FFT_EVENT_NONE;
                    }
                    else
                    {
                        /* The strongest newly confirmed band becomes the sole rhythm source. */
                        if (bass_beat && (event_scores.bass > rhythm_event_score))
                        {
                            rhythm_event = MUSIC_FFT_EVENT_BASS;
                            rhythm_event_score = event_scores.bass;
                        }
                        if (mid_beat && (event_scores.mid > rhythm_event_score))
                        {
                            rhythm_event = MUSIC_FFT_EVENT_MID;
                            rhythm_event_score = event_scores.mid;
                        }
                        if (high_beat && (event_scores.high > rhythm_event_score))
                        {
                            rhythm_event = MUSIC_FFT_EVENT_HIGH;
                            rhythm_event_score = event_scores.high;
                        }

                        if (MUSIC_FFT_EVENT_NONE != rhythm_event)
                        {
                            uint32_t const period_blocks =
                                music_rhythm_period_get(rhythm_tracks, rhythm_event);
                            uint32_t const period_ms = period_blocks * MUSIC_FFT_BLOCK_DURATION_MS;
                            uint32_t const bpm = (period_ms > 0U) ? (60000U / period_ms) : 0U;

                            rhythm_source = rhythm_event;
                            printf("RHYTHM: LOCK %s, period=%lu ms, bpm=%lu\r\n",
                                   music_fft_event_name(rhythm_source),
                                   (unsigned long) period_ms,
                                   (unsigned long) bpm);
                        }
                    }

                    if (raw_hit_detected)
                    {
                        bool const verification_started =
                            (!dance_state.active && (0U == dance_state.verify_hit_count));
                        bool const dance_started = music_dance_register_hit(&dance_state,
                                                                            dominant_event,
                                                                            dominant_score,
                                                                            rhythm_source);

                        /* Raw music activity starts verification but still does not move a servo. */
                        if (verification_started && !dance_started)
                        {
                            printf("MUSIC: VERIFYING\r\n");
                        }

                        if (dance_started)
                        {
                            event_count = 0U;
                            printf("MUSIC: ACTIVE, motion=%s\r\n",
                                   music_fft_event_name(dance_state.motion_event));
                        }
                    }

                    /*
                     * A confirmed source still supplies beat timing, but the five-second vote
                     * locks the servo group so a brief band error cannot cause a mechanical twitch.
                     */
                    if ((MUSIC_FFT_EVENT_NONE != rhythm_event) && dance_state.active)
                    {
                        uint32_t const action_period_blocks =
                            music_rhythm_period_get(rhythm_tracks, rhythm_event);

                        if (music_dance_apply_event(dance_state.motion_event,
                                                    &dance_state,
                                                    action_period_blocks))
                        {
                            event_count++;
                            printf("BEAT #%lu: source=%s, motion=%s, period=%lu ms\r\n",
                                   (unsigned long) event_count,
                                   music_fft_event_name(rhythm_event),
                                   music_fft_event_name(dance_state.motion_event),
                                   (unsigned long) (action_period_blocks * MUSIC_FFT_BLOCK_DURATION_MS));
                        }
                        else
                        {
                            printf("Dance servo command failed: motion=%s, error=%d\r\n",
                                   music_fft_event_name(dance_state.motion_event),
                                   Servo_GetLastError());
                        }
                    }

                    /* Music may continue without a stable beat; do not hold the last dance pose. */
                    if (dance_state.active && dance_state.away_from_center &&
                        (dance_state.blocks_since_beat >= MUSIC_DANCE_BEAT_PAUSE_BLOCKS))
                    {
                        if (music_dance_return_center(&dance_state))
                        {
                            printf("DANCE: PAUSE\r\n");
                        }
                        else
                        {
                            dance_state.blocks_since_beat = 0U;
                            printf("Dance pause command failed: error=%d\r\n", Servo_GetLastError());
                        }
                    }

                    /* Leave dance mode only after about three seconds without any detected hit. */
                    if (dance_state.active &&
                        (dance_state.blocks_since_hit >= MUSIC_DANCE_STOP_SILENCE_BLOCKS))
                    {
                        if (!dance_state.away_from_center || music_dance_return_center(&dance_state))
                        {
                            dance_state.active = false;
                            music_dance_verification_reset(&dance_state);
                            music_rhythm_reset_all(rhythm_tracks);
                            rhythm_source = MUSIC_FFT_EVENT_NONE;
                            raw_hit_gap_blocks = 0U;
                            printf("DANCE: CENTER\r\n");
                        }
                        else
                        {
                            /* Wait another silence interval before retrying a failed command. */
                            dance_state.blocks_since_hit = 0U;
                            printf("Dance center command failed: error=%d\r\n", Servo_GetLastError());
                        }
                    }

                    adaptive_flux.bass = music_fft_flux_filter(adaptive_flux.bass, flux.bass);
                    adaptive_flux.mid = music_fft_flux_filter(adaptive_flux.mid, flux.mid);
                    adaptive_flux.high = music_fft_flux_filter(adaptive_flux.high, flux.high);
                }
            }
        }
    }
}
