#include "app.h"

#include "Printf/printf.h"
#include "Voice/voice.h"

#include <stdint.h>
#include <stdio.h>

/* The microphone produces 128 mono samples every 8 ms at a 16 kHz sample rate. */
#define MUSIC_TEST_FRAME_CAPACITY          (128U)
#define MUSIC_TEST_ANALYSIS_FRAME_COUNT    (13U)
#define MUSIC_TEST_CALIBRATION_WINDOWS      (50U)
#define MUSIC_TEST_CALIBRATION_REPORT_STEP  (1U)
#define MUSIC_TEST_STATUS_INTERVAL_WINDOWS  (1U)
#define MUSIC_TEST_THRESHOLD_NUMERATOR      (3U)
#define MUSIC_TEST_THRESHOLD_DENOMINATOR    (2U)
#define MUSIC_TEST_MIN_BEAT_GAP_WINDOWS     (2U)
#define MUSIC_TEST_MIN_RISE_PERCENT         (10U)
#define MUSIC_TEST_BASELINE_FILTER_WEIGHT   (31U)
#define MUSIC_TEST_BASELINE_FILTER_DIVISOR  (32U)

static voice_sample_t s_music_test_frame[MUSIC_TEST_FRAME_CAPACITY];

void app_music_beat_test(void)
{
    uint64_t window_abs_sum = 0U;
    uint32_t window_sample_count = 0U;
    uint32_t window_frame_count = 0U;
    uint32_t skipped_frame_count = 0U;
    uint32_t last_frame_counter = 0U;
    uint32_t calibration_window_count = 0U;
    uint32_t adaptive_baseline = 0U;
    uint32_t previous_average_abs = 0U;
    uint32_t windows_since_beat = MUSIC_TEST_MIN_BEAT_GAP_WINDOWS;
    uint32_t beat_count = 0U;
    uint32_t status_window_count = 0U;
    uint32_t status_energy_max = 0U;
    uint32_t status_skipped_frame_count = 0U;
    int32_t window_peak_abs = 0;
    int32_t status_peak_max = 0;
    bool last_frame_valid = false;
    fsp_err_t err;

    /* The existing microphone test confirms that the useful data is on the right channel. */
    Voice_ChannelSet(VOICE_CHANNEL_RIGHT);
    err = Voice_Start();
    if (FSP_SUCCESS != err)
    {
        app_fatal_error("Music test microphone start", err);
    }

    printf("Music beat test started. Keep quiet for about 5 seconds.\r\n");

    while (1)
    {
        uint32_t frame_counter;
        uint32_t frame_count;

        if (!Voice_FrameReady())
        {
            /* Wait for the next interrupt instead of occupying the CPU in an empty loop. */
            __WFI();
            continue;
        }

        frame_counter = Voice_FrameCounterGet();
        frame_count = Voice_FrameRead(s_music_test_frame, MUSIC_TEST_FRAME_CAPACITY);
        if (0U == frame_count)
        {
            continue;
        }

        if (last_frame_valid)
        {
            uint32_t const frame_delta = frame_counter - last_frame_counter;

            /* A delta greater than one means an intermediate frame was overwritten before it was read. */
            if (frame_delta > 1U)
            {
                skipped_frame_count += frame_delta - 1U;
            }
        }

        last_frame_counter = frame_counter;
        last_frame_valid = true;

        for (uint32_t i = 0U; i < frame_count; i++)
        {
            int32_t const sample = s_music_test_frame[i];
            int32_t const sample_abs = (sample < 0) ? -sample : sample;

            window_abs_sum += (uint32_t) sample_abs;
            window_sample_count++;

            if (sample_abs > window_peak_abs)
            {
                window_peak_abs = sample_abs;
            }
        }

        window_frame_count++;
        if (window_frame_count >= MUSIC_TEST_ANALYSIS_FRAME_COUNT)
        {
            uint32_t const average_abs =
                (0U != window_sample_count) ? (uint32_t) (window_abs_sum / window_sample_count) : 0U;
            uint32_t threshold = 0U;
            uint8_t beat_detected = 0U;

            if (calibration_window_count < MUSIC_TEST_CALIBRATION_WINDOWS)
            {
                /* Build a stable initial noise baseline from approximately five seconds of quiet audio. */
                adaptive_baseline =
                    ((adaptive_baseline * calibration_window_count) + average_abs) /
                    (calibration_window_count + 1U);
                calibration_window_count++;

                /* Report calibration progress at the configured serial output interval. */
                if ((1U == calibration_window_count) ||
                    (0U == (calibration_window_count % MUSIC_TEST_CALIBRATION_REPORT_STEP)))
                {
                    printf("MIC calibration: %lu/%lu, average=%lu, peak=%ld\r\n",
                           (unsigned long) calibration_window_count,
                           (unsigned long) MUSIC_TEST_CALIBRATION_WINDOWS,
                           (unsigned long) average_abs,
                           (long) window_peak_abs);
                }

                if (calibration_window_count == MUSIC_TEST_CALIBRATION_WINDOWS)
                {
                    previous_average_abs = average_abs;
                    printf("Calibration complete. Start playing music.\r\n");
                }
            }
            else
            {
                uint32_t minimum_rise = previous_average_abs / MUSIC_TEST_MIN_RISE_PERCENT;

                if (windows_since_beat < UINT32_MAX)
                {
                    windows_since_beat++;
                }

                if (0U == minimum_rise)
                {
                    minimum_rise = 1U;
                }

                threshold = (adaptive_baseline * MUSIC_TEST_THRESHOLD_NUMERATOR) /
                            MUSIC_TEST_THRESHOLD_DENOMINATOR;

                /*
                 * A beat must be louder than the adaptive background, be on a rising edge,
                 * and be sufficiently far from the previous beat to avoid repeated triggers.
                 */
                if ((average_abs > threshold) &&
                    (average_abs > (previous_average_abs + minimum_rise)) &&
                    (windows_since_beat >= MUSIC_TEST_MIN_BEAT_GAP_WINDOWS))
                {
                    beat_detected = 1U;
                    windows_since_beat = 0U;
                    beat_count++;
                }

                if (0U != beat_detected)
                {
                    /* Keep this message short so the UART output does not delay audio processing. */
                    printf("\r\n========== BEAT #%lu  E=%lu  T=%lu ==========\r\n",
                           (unsigned long) beat_count,
                           (unsigned long) average_abs,
                           (unsigned long) threshold);
                }

                /* Accumulate and print status at the configured serial output interval. */
                status_window_count++;
                status_skipped_frame_count += skipped_frame_count;

                if (average_abs > status_energy_max)
                {
                    status_energy_max = average_abs;
                }

                if (window_peak_abs > status_peak_max)
                {
                    status_peak_max = window_peak_abs;
                }

                if (status_window_count >= MUSIC_TEST_STATUS_INTERVAL_WINDOWS)
                {
                    printf("MIC status: energy_max=%lu, baseline=%lu, threshold=%lu, peak_max=%ld, skipped=%lu\r\n",
                           (unsigned long) status_energy_max,
                           (unsigned long) adaptive_baseline,
                           (unsigned long) threshold,
                           (long) status_peak_max,
                           (unsigned long) status_skipped_frame_count);

                    status_window_count = 0U;
                    status_energy_max = 0U;
                    status_peak_max = 0;
                    status_skipped_frame_count = 0U;
                }

                /* Slowly follow changes in music volume without allowing one beat to move the baseline abruptly. */
                adaptive_baseline =
                    ((adaptive_baseline * MUSIC_TEST_BASELINE_FILTER_WEIGHT) + average_abs) /
                    MUSIC_TEST_BASELINE_FILTER_DIVISOR;
                previous_average_abs = average_abs;
            }

            /* Clear the approximately 100 ms statistics window before collecting the next one. */
            window_abs_sum = 0U;
            window_sample_count = 0U;
            window_frame_count = 0U;
            skipped_frame_count = 0U;
            window_peak_abs = 0;
        }
    }
}
