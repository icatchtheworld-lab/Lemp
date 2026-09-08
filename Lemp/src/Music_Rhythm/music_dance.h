#ifndef MUSIC_RHYTHM_MUSIC_DANCE_H_
#define MUSIC_RHYTHM_MUSIC_DANCE_H_

#include <stdbool.h>
#include <stdint.h>

#define MUSIC_DANCE_SERVO_COUNT (5U)

/** Initialize all five servos and move the lamp to its known center pose. */
void music_dance_init(void);

/**
 * Prepare one dance session from the lamp's current pose.
 *
 * This function only resets dance state; it does not initialize the servos,
 * delay the main loop, or send a new position command.
 */
void music_dance_session_begin(
    int16_t const start_pose[MUSIC_DANCE_SERVO_COUNT]);

/**
 * Send one beat-synchronized intro nod while holding servos 1 through 4.
 *
 * One accepted beat starts a complete down/up nod. The down command is sent
 * immediately; the non-blocking up command is completed by
 * music_dance_pending_update(). For BPM above 100 every second input beat is
 * intentionally skipped and returns true with audio_guard_ms=0.
 */
bool music_dance_apply_intro_nod(float bpm,
                                 uint32_t * p_audio_guard_ms);

/**
 * Complete the delayed up-half of a beat nod without blocking the main loop.
 * p_command_sent is true only when this call actually sent the return command.
 */
bool music_dance_pending_update(uint32_t now_ms,
                                bool * p_command_sent,
                                uint32_t * p_audio_guard_ms);

/**
 * Send the next dance pose for one confirmed beat.
 *
 * The requested keyframe time is adjusted to the locked BPM. On success,
 * p_audio_guard_ms receives the estimated period for which servo movement and
 * residual body vibration should be ignored by the onset detector.
 */
bool music_dance_apply_beat(float bpm, uint32_t * p_audio_guard_ms);

/**
 * Send one beat pose with a gradually adjustable movement amplitude.
 *
 * intensity_percent is limited to 0...100. At 0 percent every dance frame
 * remains at the session start pose; at 100 percent the recorded dance frame
 * is used in full. Intermediate values interpolate all five servos together.
 */
bool music_dance_apply_beat_scaled(float bpm,
                                   uint8_t intensity_percent,
                                   uint32_t * p_audio_guard_ms);

/** Return to the normal power-on pose when beat prediction stops. */
bool music_dance_return_center(uint32_t * p_audio_guard_ms);

/** Return smoothly to a caller-supplied pose and reset the dance phrase. */
bool music_dance_return_to_pose(
    int16_t const target_pose[MUSIC_DANCE_SERVO_COUNT],
    uint16_t move_ms,
    uint32_t * p_audio_guard_ms);

/** Copy the most recently commanded dance pose to the caller. */
void music_dance_last_position_get(
    int16_t position[MUSIC_DANCE_SERVO_COUNT]);

#endif /* MUSIC_RHYTHM_MUSIC_DANCE_H_ */
