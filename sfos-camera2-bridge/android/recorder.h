/**
 * @file recorder.h
 * @brief MP4 recorder fed by a camera stream (video) and pushed PCM (audio).
 *
 * The camera renders into a persistent input surface that stays in its
 * capture session; each recording attaches a fresh hardware encoder to it,
 * so recordings start without reconfiguring the session. Microphone PCM
 * comes from sfos_camera2_record_audio_push() on another thread.
 */

#ifndef SFOS_CAMERA2_RECORDER_H
#define SFOS_CAMERA2_RECORDER_H

#include <android/native_window.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct recorder_config {
    int width;
    int height;
    int fps;
    int bitrate;
    int hevc;
    /* 0 channels: video only. */
    int audio_sample_rate;
    int audio_channels;
};

enum recorder_error {
    RECORDER_ENCODER_ERROR = -80,
    RECORDER_OUTPUT_ERROR = -81,
    RECORDER_MUXER_ERROR = -82,
    RECORDER_NO_FRAMES = -83,
    RECORDER_STATE_ERROR = -84,
};

struct recorder;

/* Persistent encoder input surface; NULL when the platform lacks the API. */
ANativeWindow *recorder_surface_create(void);
void recorder_surface_release(ANativeWindow *surface);

/* Configures and starts an encoder on the surface (no file yet). On failure
 * returns NULL with the failing stage and media status. */
struct recorder *recorder_prepare(const struct recorder_config *config,
                                  ANativeWindow *surface, const char **stage,
                                  int *media_status);

/* Opens the MP4 and starts accepting audio. The caller then points camera
 * requests at the surface. Returns 0, or a negative code with *stage set. */
int recorder_begin(struct recorder *recorder, const char *path,
                   int orientation_degrees, const char **stage);

/* Moves encoded data into the file; call often (every ~10 ms). Never
 * blocks. Returns 0, or a negative code with *stage set. */
int recorder_poll(struct recorder *recorder, const char **stage);

bool recorder_recording(const struct recorder *recorder);

/* After the caller stopped sending frames to the surface: ends both
 * streams, finishes the MP4 and writes a JSON report. Returns 0 or a
 * negative code. The recorder can only be destroyed afterwards. */
int recorder_finish(struct recorder *recorder, char *json, size_t json_size);

void recorder_destroy(struct recorder *recorder);

#endif
