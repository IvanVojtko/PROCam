/**
 * @file main.c
 * @brief Sailfish command-line frontend for the Android Camera2 bridge.
 *
 * Loads the Android bridge library through libhybris, parses ProCam helper
 * command-line options, and dispatches probe, preview, RAW, or JPEG operations.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../android/camera2_bridge.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef void *(*android_dlopen_fn)(const char *name, int flags);
typedef void *(*android_dlsym_fn)(void *handle, const char *name);
typedef int (*probe_fn)(char *out, size_t out_size);
typedef int (*capture_options_fn)(
    const char *camera_id, int width, int height,
    const char *raw_path, const char *metadata_path, int timeout_ms,
    const struct sfos_camera2_capture_options *options,
    char *out, size_t out_size);
typedef int (*preview_ppm_fn)(
    const char *camera_id, int width, int height, int frame_count,
    int timeout_ms, int output_fd, int control_fd,
    int jpeg_width, int jpeg_height, int jpeg_quality,
    int jpeg_orientation_degrees, int raw_width, int raw_height,
    int raw_format, float zoom_ratio,
    float focus_x, float focus_y,
    char *out, size_t out_size);
typedef int (*capture_jpeg_fn)(
    const char *camera_id, int width, int height, const char *jpeg_path,
    int timeout_ms, int jpeg_quality, int orientation_degrees,
    int scene_mode, int sensor_sensitivity, long long exposure_time_ns,
    int aperture, int noise_reduction, float zoom_ratio,
    char *out, size_t out_size);
typedef int (*record_video_fn)(
    const char *camera_id, int width, int height, int fps, int bitrate,
    int duration_ms, int stabilization, int hevc, int orientation_degrees,
    int audio_sample_rate, int audio_channels, int repeat,
    const char *output_path, char *out, size_t out_size);
typedef void (*record_audio_push_fn)(const int16_t *pcm, int samples,
                                     long long age_us);
typedef void (*preview_set_video_fn)(int width, int height, int fps,
                                     int bitrate, int stabilization, int hevc,
                                     int audio_sample_rate,
                                     int audio_channels);
typedef void (*preview_set_max_fps_fn)(int fps);
typedef int (*probe_vendor_tags_fn)(char *out, size_t out_size);
typedef void (*record_set_vendor_fn)(const int32_t *tags, const int32_t *values,
                                     int count, int force_fps);
typedef int (*encode_heic_fn)(const char *input_path, int width, int height,
                              int quality, int rotation_degrees,
                              const char *exif_path, const char *output_path,
                              char *json, size_t json_size);
typedef const char *(*version_fn)(void);
typedef void (*droid_media_init_fn)(void);
typedef void (*droid_media_deinit_fn)(void);

static droid_media_deinit_fn g_droid_media_deinit = NULL;

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void deinit_droid_media(void)
{
    if (g_droid_media_deinit) {
        g_droid_media_deinit();
        g_droid_media_deinit = NULL;
    }
}

/**
 * @brief Write bridge JSON next to an output image or RAW prefix.
 *
 * @return 0 on success, -1 on invalid input or file I/O failure.
 */
static int write_json_sidecar(const char *output_path, const char *json)
{
    char metadata_path[PATH_MAX];
    if (!output_path || !*output_path || !json || !*json) {
        return -1;
    }

    const char *slash = strrchr(output_path, '/');
    const char *name = slash ? slash + 1 : output_path;
    const char *dot = strrchr(name, '.');
    size_t prefix_length = dot ? (size_t)(dot - output_path)
                               : strlen(output_path);

    if (prefix_length + 5 >= sizeof(metadata_path)) {
        return -1;
    }

    memcpy(metadata_path, output_path, prefix_length);
    memcpy(metadata_path + prefix_length, ".json", 6);

    FILE *file = fopen(metadata_path, "w");
    if (!file) {
        return -1;
    }
    if (fputs(json, file) < 0 || fputc('\n', file) == EOF) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

enum focus_mode {
    FOCUS_NONE = 0,
    FOCUS_AUTO = 1,
    FOCUS_CONTINUOUS = 2,
    FOCUS_MANUAL = 3,
    FOCUS_INFINITY = 4,
};

/* Microphone capture for --record-video: 10 ms PulseAudio reads pushed to
 * the bridge with their age (stream latency + chunk length), from which the
 * bridge timestamps them on the camera's clock. */
struct audio_capture {
    pa_simple *stream;
    record_audio_push_fn push;
    int sample_rate;
    int channels;
    atomic_int stop;
    int reads;
    int error;
};

static void *audio_capture_main(void *opaque)
{
    struct audio_capture *capture = opaque;
    const int frames = capture->sample_rate / 100;
    int16_t pcm[2048];
    const size_t bytes = (size_t)frames * capture->channels * sizeof(int16_t);
    while (!atomic_load(&capture->stop)) {
        int error = 0;
        if (pa_simple_read(capture->stream, pcm, bytes, &error) < 0) {
            capture->error = error;
            break;
        }
        pa_usec_t latency = pa_simple_get_latency(capture->stream, &error);
        if (latency == (pa_usec_t)-1) {
            latency = 0;
        }
        capture->push(pcm, frames * capture->channels,
                      (long long)latency +
                          (long long)frames * 1000000 / capture->sample_rate);
        ++capture->reads;
    }
    return NULL;
}

static int audio_capture_open(struct audio_capture *capture)
{
    const pa_sample_spec spec = {
        .format = PA_SAMPLE_S16LE,
        .rate = (uint32_t)capture->sample_rate,
        .channels = (uint8_t)capture->channels,
    };
    const uint32_t chunk = (uint32_t)(capture->sample_rate / 100 *
                                      capture->channels * sizeof(int16_t));
    const pa_buffer_attr attributes = {
        .maxlength = (uint32_t)-1,
        .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1,
        .minreq = (uint32_t)-1,
        .fragsize = chunk,
    };
    int error = 0;
    capture->stream = pa_simple_new(NULL, "ProCam", PA_STREAM_RECORD, NULL,
                                    "video recording", &spec, NULL,
                                    &attributes, &error);
    if (!capture->stream) {
        fprintf(stderr, "Microphone unavailable (%s); recording without "
                        "audio\n", pa_strerror(error));
        return -1;
    }
    return 0;
}

/* Video preview: stdin goes to the bridge through a pipe, so the helper sees
 * record-start/record-stop and keeps the microphone open only while
 * recording. It opens before record-start reaches the bridge, and closes a
 * little after record-stop, once the last frames' sound is in. */
#define RELAY_AUDIO_LINGER_MS 2000

struct control_relay {
    int bridge_fd;
    int audio;
    struct audio_capture capture;
    pthread_t audio_thread;
    int audio_running;
    long long audio_stop_ms;
    pthread_mutex_t lock;
};

static int g_relay_fd = -1;

static void relay_quit_signal(int signal_number)
{
    (void)signal_number;
    if (g_relay_fd >= 0) {
        static const char quit[] = "quit\n";
        ssize_t written = write(g_relay_fd, quit, sizeof(quit) - 1);
        (void)written;
    }
}

static int relay_write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

static void relay_audio_start(struct control_relay *relay)
{
    pthread_mutex_lock(&relay->lock);
    relay->audio_stop_ms = 0;
    if (relay->audio && !relay->audio_running &&
            audio_capture_open(&relay->capture) == 0) {
        atomic_store(&relay->capture.stop, 0);
        relay->audio_running = pthread_create(&relay->audio_thread, NULL,
                                              audio_capture_main,
                                              &relay->capture) == 0;
        if (!relay->audio_running) {
            pa_simple_free(relay->capture.stream);
            relay->capture.stream = NULL;
        }
    }
    pthread_mutex_unlock(&relay->lock);
}

static void relay_audio_stop(struct control_relay *relay)
{
    pthread_mutex_lock(&relay->lock);
    relay->audio_stop_ms = 0;
    if (relay->audio_running) {
        atomic_store(&relay->capture.stop, 1);
        pthread_join(relay->audio_thread, NULL);
        pa_simple_free(relay->capture.stream);
        relay->capture.stream = NULL;
        relay->audio_running = 0;
        if (relay->capture.error) {
            fprintf(stderr, "Microphone read failed after %d reads: %s\n",
                    relay->capture.reads,
                    pa_strerror(relay->capture.error));
        }
    }
    pthread_mutex_unlock(&relay->lock);
}

static int relay_starts_with(const char *line, size_t length, const char *word)
{
    const size_t word_length = strlen(word);
    return length >= word_length && !memcmp(line, word, word_length) &&
           (length == word_length || line[word_length] == ' ' ||
            line[word_length] == '\n' || line[word_length] == '\r');
}

static void *relay_main(void *opaque)
{
    struct control_relay *relay = opaque;
    char buffer[8192];
    size_t length = 0;
    for (;;) {
        pthread_mutex_lock(&relay->lock);
        const long long stop_ms = relay->audio_stop_ms;
        pthread_mutex_unlock(&relay->lock);
        int timeout = -1;
        if (stop_ms) {
            const long long wait = stop_ms - monotonic_ms();
            timeout = wait > 0 ? (int)wait : 0;
        }
        struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
        const int ready = poll(&input, 1, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            relay_audio_stop(relay);
            continue;
        }
        ssize_t bytes = ready > 0
                ? read(STDIN_FILENO, buffer + length, sizeof(buffer) - length)
                : -1;
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            /* The app is gone: end the preview, finishing any recording. */
            relay_write_all(relay->bridge_fd, "\nquit\n", 6);
            break;
        }
        length += (size_t)bytes;
        char *newline;
        while ((newline = memchr(buffer, '\n', length))) {
            const size_t line_length = (size_t)(newline - buffer) + 1;
            if (relay_starts_with(buffer, line_length, "record-start")) {
                relay_audio_start(relay);
            } else if (relay_starts_with(buffer, line_length, "record-stop") ||
                       relay_starts_with(buffer, line_length, "quit")) {
                pthread_mutex_lock(&relay->lock);
                if (relay->audio_running) {
                    relay->audio_stop_ms = monotonic_ms() + RELAY_AUDIO_LINGER_MS;
                }
                pthread_mutex_unlock(&relay->lock);
            }
            if (relay_write_all(relay->bridge_fd, buffer, line_length) != 0) {
                return NULL;
            }
            length -= line_length;
            memmove(buffer, buffer + line_length, length);
        }
        if (length == sizeof(buffer)) {
            /* No newline in 8 KB: pass it on; the bridge drops the line. */
            if (relay_write_all(relay->bridge_fd, buffer, length) != 0) {
                return NULL;
            }
            length = 0;
        }
    }
    return NULL;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage:\n"
            "  %s\n"
            "  %s --capture --output PREFIX [--camera ID] [--size WIDTHxHEIGHT]\n"
            "     [--timeout SECONDS] [--focus MODE] [--focus-distance D]\n"
            "     [--focus-timeout SECONDS] [--focus-failure capture|abort]\n"
            "     [--scene MODE] [--portrait]\n"
            "     [--color-temperature K] [--color-tint N]\n"
            "     [--iso N] [--shutter-ns N] [--aperture N] [--ev-steps N]\n"
            "     [--noise-reduction MODE] [--zoom R]\n"
            "     [--force]\n"
            "  %s --preview [--camera ID] [--size WIDTHxHEIGHT]\n"
            "     [--frames N] [--timeout SECONDS] [--output PATH]\n"
            "     [--jpeg-size WIDTHxHEIGHT] [--quality N]\n"
            "     [--orientation DEGREES] [--zoom R]\n"
            "     [--raw-size WIDTHxHEIGHT] [--raw-format raw16|yuv|jpeg]\n"
            "     [--preview-format rgb|yuv] [--focus-x X --focus-y Y]\n"
            "     [--start-paused] [--max-fps FPS]\n"
            "     [--video-size WIDTHxHEIGHT [--video-fps N]\n"
            "      [--video-bitrate BPS] [--stabilization on|off]\n"
            "      [--codec avc|hevc] [--audio on|off] [--audio-channels 1|2]]\n"
            "  %s --vendor-tags   (vendor characteristics by id, diagnostics)\n"
            "  %s --record-video --output PATH.mp4 [--camera ID]\n"
            "     [--size WIDTHxHEIGHT] [--fps N] [--seconds N]\n"
            "     [--bitrate BPS] [--stabilization on|off] [--codec avc|hevc]\n"
            "     [--orientation DEGREES] [--audio on|off]\n"
            "     [--audio-channels 1|2] [--repeat N] [--force]\n"
            "     [--vendor TAG=VALUE]... [--force-fps]\n"
            "  %s --capture-jpeg --output PATH [--camera ID]\n"
            "     [--size WIDTHxHEIGHT] [--timeout SECONDS]\n"
            "     [--quality N] [--orientation DEGREES]\n"
            "     [--scene MODE] [--portrait]\n"
            "     [--iso N] [--shutter-ns N] [--aperture N] [--ev-steps N]\n"
            "     [--noise-reduction MODE] [--zoom R]\n"
            "     [--force]\n\n"
            "  %s --calibrate-shutter [--camera ID] [--size WIDTHxHEIGHT]\n"
            "     [--iso N] [--quality N] [--timeout SECONDS]\n"
            "     [--output-dir PATH] [--force]\n"
            "  %s --encode-heic INPUT.i420 WIDTH HEIGHT QUALITY ROTATION\n"
            "     OUTPUT.heic [EXIF]   (hardware HEIC encoder; EXIF file is\n"
            "     \"Exif\\0\\0\" and TIFF data)\n\n"
            "Without arguments, print Camera2 capabilities. Capture mode creates\n"
            "PREFIX.raw16 or PREFIX.raw10 and PREFIX.json. Focus modes are none (default), auto,\n"
            "continuous, manual, and infinity. Manual distance is in diopters.\n"
            "Noise reduction is none, bayer, ycc, temporal, fixed, extra, or a flag value.\n"
            "Color temperature/tint use bridge Camera2 request tags when present.\n"
            "Preview mode writes consecutive binary RGB888 frames. With\n"
            "--video-size it also takes record-start DEGREES PATH.mp4 and\n"
            "record-stop on standard input.\n",
            program, program, program, program, program, program, program, program);
}

static long long absolute_i64(long long value)
{
    return value < 0 ? -value : value;
}

static const char *find_json_value(const char *json, const char *key)
{
    const char *found = strstr(json, key);
    if (!found) {
        return NULL;
    }
    found = strchr(found, ':');
    if (!found) {
        return NULL;
    }
    return found + 1;
}

static long long json_i64(const char *json, const char *key, long long fallback)
{
    const char *value = find_json_value(json, key);
    if (!value) {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(value, &end, 10);
    return errno || end == value ? fallback : parsed;
}

static int shutter_honored(long long requested_ns, long long observed_ms)
{
    if (requested_ns <= 0 || observed_ms <= 0) {
        return 0;
    }
    long long requested_ms = requested_ns / 1000000LL;
    long long tolerance_ms = requested_ms / 4;
    if (tolerance_ms < 150) {
        tolerance_ms = 150;
    }
    return absolute_i64(observed_ms - requested_ms) <= tolerance_ms;
}

static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed <= 0 || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_nonnegative_int(const char *text, int *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < 0 || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_nonnegative_i64(const char *text, long long *value)
{
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno || !end || *end || parsed < 0) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_int(const char *text, int *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_size(const char *text, int *width, int *height)
{
    char tail = '\0';
    if (sscanf(text, "%dx%d%c", width, height, &tail) != 2 ||
            *width <= 0 || *height <= 0) {
        return -1;
    }
    return 0;
}

static int parse_nonnegative_float(const char *text, float *value)
{
    char *end = NULL;
    errno = 0;
    float parsed = strtof(text, &end);
    if (errno || !end || *end || parsed < 0.0f || parsed > 10000.0f ||
            parsed != parsed) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_unit_float(const char *text, float *value)
{
    char *end = NULL;
    errno = 0;
    float parsed = strtof(text, &end);
    if (errno || !end || *end || parsed < 0.0f || parsed > 1.0f ||
            parsed != parsed) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int parse_raw_format(const char *text, int *format)
{
    if (!strcmp(text, "raw16")) {
        *format = SFOS_CAMERA2_RAW_FORMAT_RAW16;
        return 0;
    }
    if (!strcmp(text, "raw10")) {
        *format = SFOS_CAMERA2_RAW_FORMAT_RAW10;
        return 0;
    }
    return -1;
}

static int parse_focus_mode(const char *text, int *mode)
{
    if (!strcmp(text, "none")) {
        *mode = FOCUS_NONE;
    } else if (!strcmp(text, "auto")) {
        *mode = FOCUS_AUTO;
    } else if (!strcmp(text, "continuous")) {
        *mode = FOCUS_CONTINUOUS;
    } else if (!strcmp(text, "manual")) {
        *mode = FOCUS_MANUAL;
    } else if (!strcmp(text, "infinity")) {
        *mode = FOCUS_INFINITY;
    } else {
        return -1;
    }
    return 0;
}

static int parse_scene_mode(const char *text, int *mode)
{
    static const struct {
        const char *name;
        int mode;
    } scenes[] = {
        { "none", SFOS_CAMERA2_SCENE_NONE },
        { "manual", SFOS_CAMERA2_SCENE_MANUAL },
        { "closeup", SFOS_CAMERA2_SCENE_CLOSEUP },
        { "portrait", SFOS_CAMERA2_SCENE_PORTRAIT },
        { "landscape", SFOS_CAMERA2_SCENE_LANDSCAPE },
        { "sport", SFOS_CAMERA2_SCENE_SPORT },
        { "night", SFOS_CAMERA2_SCENE_NIGHT },
        { "auto", SFOS_CAMERA2_SCENE_AUTO },
        { "action", SFOS_CAMERA2_SCENE_ACTION },
        { "night-portrait", SFOS_CAMERA2_SCENE_NIGHT_PORTRAIT },
        { "theatre", SFOS_CAMERA2_SCENE_THEATRE },
        { "beach", SFOS_CAMERA2_SCENE_BEACH },
        { "snow", SFOS_CAMERA2_SCENE_SNOW },
        { "sunset", SFOS_CAMERA2_SCENE_SUNSET },
        { "steady-photo", SFOS_CAMERA2_SCENE_STEADY_PHOTO },
        { "fireworks", SFOS_CAMERA2_SCENE_FIREWORKS },
        { "party", SFOS_CAMERA2_SCENE_PARTY },
        { "candlelight", SFOS_CAMERA2_SCENE_CANDLELIGHT },
        { "barcode", SFOS_CAMERA2_SCENE_BARCODE },
        { "backlight", SFOS_CAMERA2_SCENE_BACKLIGHT },
        { "flowers", SFOS_CAMERA2_SCENE_FLOWERS },
        { "ar", SFOS_CAMERA2_SCENE_AR },
        { "hdr", SFOS_CAMERA2_SCENE_HDR },
    };

    for (size_t index = 0; index < sizeof(scenes) / sizeof(scenes[0]);
            ++index) {
        if (!strcmp(text, scenes[index].name)) {
            *mode = scenes[index].mode;
            return 0;
        }
    }
    return -1;
}

static int parse_noise_reduction(const char *text, int *mode)
{
    static const struct {
        const char *name;
        int mode;
    } modes[] = {
        { "none", SFOS_CAMERA2_NOISE_REDUCTION_NONE },
        { "off", SFOS_CAMERA2_NOISE_REDUCTION_NONE },
        { "fast", SFOS_CAMERA2_NOISE_REDUCTION_FAST },
        { "high_quality", SFOS_CAMERA2_NOISE_REDUCTION_HIGH_QUALITY },
        { "high-quality", SFOS_CAMERA2_NOISE_REDUCTION_HIGH_QUALITY },
        { "minimal", SFOS_CAMERA2_NOISE_REDUCTION_MINIMAL },
        { "zsl", SFOS_CAMERA2_NOISE_REDUCTION_ZERO_SHUTTER_LAG },
        { "zero_shutter_lag",
          SFOS_CAMERA2_NOISE_REDUCTION_ZERO_SHUTTER_LAG },
        { "zero-shutter-lag",
          SFOS_CAMERA2_NOISE_REDUCTION_ZERO_SHUTTER_LAG },
        { "bayer", SFOS_CAMERA2_NOISE_REDUCTION_BAYER },
        { "ycc", SFOS_CAMERA2_NOISE_REDUCTION_YCC },
        { "temporal", SFOS_CAMERA2_NOISE_REDUCTION_TEMPORAL },
        { "fixed", SFOS_CAMERA2_NOISE_REDUCTION_FIXED },
        { "extra", SFOS_CAMERA2_NOISE_REDUCTION_EXTRA },
    };

    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]);
            ++index) {
        if (!strcmp(text, modes[index].name)) {
            *mode = modes[index].mode;
            return 0;
        }
    }
    return parse_nonnegative_int(text, mode);
}

static void *open_libhybris(const char **loaded_name)
{
    const char *override = getenv("SFOS_LIBHYBRIS");
    const char *candidates[] = {
        override,
        "libhybris-common.so.1",
        "libhybris-common.so",
        NULL,
    };

    for (size_t index = 0; candidates[index] || index == 0; ++index) {
        if (!candidates[index]) {
            continue;
        }
        void *handle = dlopen(candidates[index], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            *loaded_name = candidates[index];
            return handle;
        }
    }
    return NULL;
}

/**
 * @brief Sailfish helper entry point.
 *
 * Loads the Android bridge, parses command-line mode/options, and dispatches to
 * probe, preview, RAW capture, or JPEG capture.
 */
int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--help") ||
                     !strcmp(argv[1], "-h"))) {
        usage(stdout, argv[0]);
        return 0;
    }

    const long long started_ms = monotonic_ms();
    const int timing = argc > 1 && !strcmp(argv[1], "--preview");
    const char *hybris_name = NULL;
    void *hybris = open_libhybris(&hybris_name);
    if (!hybris) {
        fprintf(stderr, "Unable to load libhybris-common: %s\n", dlerror());
        return 10;
    }

    android_dlopen_fn android_dlopen =
        (android_dlopen_fn)dlsym(hybris, "android_dlopen");
    android_dlsym_fn android_dlsym =
        (android_dlsym_fn)dlsym(hybris, "android_dlsym");
    if (!android_dlopen || !android_dlsym) {
        fprintf(stderr, "%s does not export android_dlopen/android_dlsym\n",
                hybris_name);
        return 11;
    }

    if (timing) {
        fprintf(stderr, "timing helper libhybris ms=%lld\n", monotonic_ms() - started_ms);
    }
    /* Match the initialization used by normal gst-droid clients. */
    void *droidmedia = android_dlopen("libdroidmedia.so", RTLD_NOW);
    if (droidmedia) {
        droid_media_init_fn initialize =
            (droid_media_init_fn)android_dlsym(droidmedia, "_droid_media_init");
        if (initialize) {
            initialize();
            g_droid_media_deinit = (droid_media_deinit_fn)android_dlsym(
                droidmedia, "_droid_media_deinit");
            if (g_droid_media_deinit) {
                atexit(deinit_droid_media);
            }
        } else {
            fprintf(stderr,
                    "Warning: libdroidmedia has no _droid_media_init symbol\n");
        }
    } else {
        fprintf(stderr,
                "Warning: could not preload libdroidmedia; continuing probe\n");
    }

    const char *bridge_name = getenv("SFOS_CAMERA2_BRIDGE");
    if (!bridge_name || !*bridge_name) {
        bridge_name = "libprocamcamera2.so";
    }

    if (timing) {
        fprintf(stderr, "timing helper droidmedia ms=%lld\n", monotonic_ms() - started_ms);
    }
    void *bridge = android_dlopen(bridge_name, RTLD_NOW);
    if (timing) {
        fprintf(stderr, "timing helper bridge ms=%lld\n", monotonic_ms() - started_ms);
    }
    if (!bridge) {
        fprintf(stderr,
                "Android linker could not load %s. Install it under "
                "/usr/libexec/droid-hybris/system/lib64/.\n",
                bridge_name);
        return 12;
    }

    void (*set_capture_compensation)(int) = (void (*)(int))android_dlsym(
        bridge, "sfos_camera2_set_capture_compensation");
    probe_fn probe = (probe_fn)android_dlsym(bridge, "sfos_camera2_probe");
    capture_options_fn capture_options = (capture_options_fn)android_dlsym(
        bridge, "sfos_camera2_capture_raw_options");
    preview_ppm_fn preview_ppm = (preview_ppm_fn)android_dlsym(
        bridge, "sfos_camera2_preview_ppm");
    capture_jpeg_fn capture_jpeg = (capture_jpeg_fn)android_dlsym(
        bridge, "sfos_camera2_capture_jpeg");
    record_video_fn record_video = (record_video_fn)android_dlsym(
        bridge, "sfos_camera2_record_video");
    record_audio_push_fn record_audio_push = (record_audio_push_fn)
        android_dlsym(bridge, "sfos_camera2_record_audio_push");
    preview_set_video_fn preview_set_video = (preview_set_video_fn)
        android_dlsym(bridge, "sfos_camera2_preview_set_video");
    version_fn version =
        (version_fn)android_dlsym(bridge, "sfos_camera2_bridge_version");
    if (!probe || !capture_options || !version) {
        fprintf(stderr, "%s does not export the expected bridge API\n",
                bridge_name);
        return 13;
    }

    char json[131072] = {0};
    int result;

    if (argc >= 8 && argc <= 9 && !strcmp(argv[1], "--encode-heic")) {
        encode_heic_fn encode_heic = (encode_heic_fn)android_dlsym(
            bridge, "sfos_camera2_encode_heic");
        if (!encode_heic) {
            fprintf(stderr, "The installed bridge cannot encode HEIC.\n");
            return 14;
        }
        result = encode_heic(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
                             atoi(argv[6]), argc == 9 ? argv[8] : NULL, argv[7],
                             json, sizeof(json));
        printf("%s\n", json);
        return result == 0 ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--vendor-tags")) {
        /* Diagnostics: vendor characteristics (MediaTek high-fps modes and
         * the like) by id; can exceed the normal probe's buffer. */
        probe_vendor_tags_fn vendor_tags = (probe_vendor_tags_fn)android_dlsym(
            bridge, "sfos_camera2_probe_vendor_tags");
        if (!vendor_tags) {
            fprintf(stderr, "The installed bridge cannot list vendor tags.\n");
            return 14;
        }
        const size_t size = 4 * 1024 * 1024;
        char *buffer = calloc(1, size);
        if (!buffer) return 3;
        result = vendor_tags(buffer, size);
        printf("%s\n", buffer);
        free(buffer);
        return result == 0 ? 0 : 1;
    }
    if (argc == 1) {
        result = probe(json, sizeof(json));
    } else if (!strcmp(argv[1], "--preview")) {
        const char *camera_id = "0";
        const char *output_path = NULL;
        int width = 640;
        int height = 480;
        int frames = 30;
        int timeout_seconds = 10;
        int jpeg_width = 0;
        int jpeg_height = 0;
        int jpeg_quality = 92;
        int jpeg_orientation = 0;
        int raw_width = 0;
        int raw_height = 0;
        int raw_format = 0;
        int preview_yuv = 0;
        int start_paused = 0;
        float zoom_ratio = 1.0f;
        float focus_x = -1.0f;
        float focus_y = -1.0f;
        int video_width = 0;
        int video_height = 0;
        int32_t preview_vendor_tags[8];
        int32_t preview_vendor_values[8];
        int preview_vendor_count = 0;
        int preview_force_fps = 0;
        int preview_max_fps = 0;
        int video_fps = 30;
        int video_bitrate = 20000000;
        int stabilization = 1;
        int hevc = 0;
        int audio = 1;
        int audio_channels = 1;

        if (!preview_ppm) {
            fprintf(stderr,
                    "The installed bridge does not support Camera2 preview. "
                    "Install libprocamcamera2.so 0.10.0 or newer.\n");
            return 14;
        }

        for (int index = 2; index < argc; ++index) {
            if (!strcmp(argv[index], "--camera") && index + 1 < argc) {
                camera_id = argv[++index];
            } else if (!strcmp(argv[index], "--size") && index + 1 < argc) {
                if (parse_size(argv[++index], &width, &height) != 0) {
                    fprintf(stderr, "Invalid --size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--frames") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &frames) != 0) {
                    fprintf(stderr, "Invalid --frames value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--timeout") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &timeout_seconds) != 0 ||
                        timeout_seconds > INT_MAX / 1000) {
                    fprintf(stderr, "Invalid --timeout value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--output") && index + 1 < argc) {
                output_path = argv[++index];
            } else if (!strcmp(argv[index], "--jpeg-size") && index + 1 < argc) {
                if (parse_size(argv[++index], &jpeg_width, &jpeg_height) != 0) {
                    fprintf(stderr, "Invalid --jpeg-size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--raw-format") && index + 1 < argc) {
                const char *format = argv[++index];
                if (!strcmp(format, "raw16")) {
                    raw_format = 0;
                } else if (!strcmp(format, "yuv")) {
                    raw_format = 1;
                } else if (!strcmp(format, "jpeg")) {
                    raw_format = 2;
                } else {
                    fprintf(stderr, "Invalid --raw-format value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--preview-format") && index + 1 < argc) {
                const char *format = argv[++index];
                if (!strcmp(format, "rgb")) {
                    preview_yuv = 0;
                } else if (!strcmp(format, "yuv")) {
                    preview_yuv = 1;
                } else {
                    fprintf(stderr, "Invalid --preview-format value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--raw-size") && index + 1 < argc) {
                if (parse_size(argv[++index], &raw_width, &raw_height) != 0) {
                    fprintf(stderr, "Invalid --raw-size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--quality") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &jpeg_quality) != 0 ||
                        jpeg_quality > 100) {
                    fprintf(stderr, "Invalid --quality value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--orientation") && index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index], &jpeg_orientation) != 0 ||
                        jpeg_orientation > 359) {
                    fprintf(stderr, "Invalid --orientation value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--zoom") && index + 1 < argc) {
                if (parse_nonnegative_float(argv[++index], &zoom_ratio) != 0 ||
                        zoom_ratio < 1.0f) {
                    fprintf(stderr, "Invalid --zoom value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-x") && index + 1 < argc) {
                if (parse_unit_float(argv[++index], &focus_x) != 0) {
                    fprintf(stderr, "Invalid --focus-x value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-y") && index + 1 < argc) {
                if (parse_unit_float(argv[++index], &focus_y) != 0) {
                    fprintf(stderr, "Invalid --focus-y value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--start-paused")) {
                start_paused = 1;
            } else if (!strcmp(argv[index], "--max-fps") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &preview_max_fps) != 0 ||
                        preview_max_fps > 240) {
                    fprintf(stderr, "Invalid --max-fps value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--force-fps")) {
                preview_force_fps = 1;
            } else if (!strcmp(argv[index], "--vendor") && index + 1 < argc) {
                /* Diagnostics for video mode: TAG=VALUE int32 vendor settings. */
                char *end = NULL;
                const char *text = argv[++index];
                const unsigned long tag = strtoul(text, &end, 0);
                long value = 0;
                if (end && *end == '=') value = strtol(end + 1, &end, 0);
                if (!end || *end || preview_vendor_count >= 8) {
                    fprintf(stderr, "--vendor is TAG=VALUE (at most 8)\n");
                    return 2;
                }
                preview_vendor_tags[preview_vendor_count] = (int32_t)tag;
                preview_vendor_values[preview_vendor_count] = (int32_t)value;
                ++preview_vendor_count;
            } else if (!strcmp(argv[index], "--video-size") && index + 1 < argc) {
                if (parse_size(argv[++index], &video_width, &video_height) != 0) {
                    fprintf(stderr, "Invalid --video-size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--video-fps") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &video_fps) != 0 ||
                        video_fps > 240) {
                    fprintf(stderr, "Invalid --video-fps value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--video-bitrate") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &video_bitrate) != 0) {
                    fprintf(stderr, "Invalid --video-bitrate value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--stabilization") && index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "on") || !strcmp(value, "off")) {
                    stabilization = !strcmp(value, "on");
                } else {
                    fprintf(stderr, "--stabilization is on or off\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--codec") && index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "avc") || !strcmp(value, "hevc")) {
                    hevc = !strcmp(value, "hevc");
                } else {
                    fprintf(stderr, "--codec is avc or hevc\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--audio") && index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "on") || !strcmp(value, "off")) {
                    audio = !strcmp(value, "on");
                } else {
                    fprintf(stderr, "--audio is on or off\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--audio-channels") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &audio_channels) != 0 ||
                        audio_channels > 2) {
                    fprintf(stderr, "--audio-channels is 1 or 2\n");
                    return 2;
                }
            } else {
                fprintf(stderr, "Unknown or incomplete option: %s\n",
                        argv[index]);
                usage(stderr, argv[0]);
                return 2;
            }
        }

        int output_fd = STDOUT_FILENO;
        if (output_path && *output_path) {
            output_fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
            if (output_fd < 0) {
                fprintf(stderr, "Could not open preview output: %s\n",
                        strerror(errno));
                return 3;
            }
        }
        if ((focus_x < 0.0f) != (focus_y < 0.0f)) {
            fprintf(stderr, "--focus-x and --focus-y must be used together\n");
            return 2;
        }
        int control_fd = STDIN_FILENO;
        struct control_relay relay;
        memset(&relay, 0, sizeof(relay));
        if (video_width > 0) {
            if (!preview_set_video || !record_audio_push) {
                fprintf(stderr, "The installed bridge does not record from "
                                "the preview. Install libprocamcamera2.so "
                                "0.14.0 or newer.\n");
                return 14;
            }
            int pipe_fds[2];
            pthread_t relay_thread;
            if (pipe(pipe_fds) != 0) {
                fprintf(stderr, "Could not create the control pipe: %s\n",
                        strerror(errno));
                return 3;
            }
            relay.bridge_fd = pipe_fds[1];
            relay.audio = audio;
            relay.capture.push = record_audio_push;
            relay.capture.sample_rate = 48000;
            relay.capture.channels = audio_channels;
            pthread_mutex_init(&relay.lock, NULL);
            if (pthread_create(&relay_thread, NULL, relay_main, &relay) != 0) {
                fprintf(stderr, "Could not start the control relay\n");
                return 3;
            }
            pthread_detach(relay_thread);
            control_fd = pipe_fds[0];
            /* SIGTERM ends the preview through the bridge, so a recording
             * in progress is finished rather than left without its index. */
            g_relay_fd = relay.bridge_fd;
            signal(SIGTERM, relay_quit_signal);
            signal(SIGINT, relay_quit_signal);
            signal(SIGPIPE, SIG_IGN);
            record_set_vendor_fn set_vendor = (record_set_vendor_fn)android_dlsym(
                bridge, "sfos_camera2_record_set_vendor");
            if (set_vendor) {
                set_vendor(preview_vendor_tags, preview_vendor_values, preview_vendor_count,
                           preview_force_fps);
            }
            preview_set_video(video_width, video_height, video_fps,
                              video_bitrate, stabilization, hevc,
                              relay.capture.sample_rate,
                              audio ? audio_channels : 0);
        }
        if (preview_max_fps > 0) {
            preview_set_max_fps_fn set_max_fps = (preview_set_max_fps_fn)android_dlsym(
                bridge, "sfos_camera2_preview_set_max_fps");
            if (set_max_fps) set_max_fps(preview_max_fps);
            else fprintf(stderr, "bridge has no frame rate cap; --max-fps ignored\n");
        }
        result = preview_ppm(camera_id, width, height, frames,
                             timeout_seconds * 1000, output_fd, control_fd,
                             jpeg_width, jpeg_height, jpeg_quality,
                             jpeg_orientation, raw_width, raw_height,
                             raw_format | (preview_yuv ? SFOS_CAMERA2_PREVIEW_YUV_FRAMES : 0)
                                 | (start_paused ? SFOS_CAMERA2_PREVIEW_START_PAUSED : 0),
                             zoom_ratio, focus_x, focus_y,
                             json, sizeof(json));
        if (video_width > 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            relay_audio_stop(&relay);
        }
        if (output_fd != STDOUT_FILENO) {
            close(output_fd);
        }
        if (json[0]) {
            fprintf(output_path && *output_path ? stdout : stderr,
                    "%s\n", json);
        }
        if (result != 0) {
            fprintf(stderr, "Camera2 bridge %s failed with code %d\n",
                    version(), result);
            return 20 - result;
        }
        return 0;
    } else if (!strcmp(argv[1], "--calibrate-shutter")) {
        static const long long shutters[] = {
            100000000LL,
            1000000000LL,
            2000000000LL,
            4000000000LL,
            8000000000LL,
            16000000000LL,
        };
        const char *camera_id = "0";
        const char *output_dir = "/tmp";
        int width = 4096;
        int height = 3072;
        int timeout_seconds = 25;
        int quality = 85;
        int sensor_sensitivity = 800;
        int force = 0;

        if (!capture_jpeg) {
            fprintf(stderr,
                    "The installed bridge does not support direct JPEG capture. "
                    "Install libprocamcamera2.so 0.6.0 or newer.\n");
            return 14;
        }

        for (int index = 2; index < argc; ++index) {
            if (!strcmp(argv[index], "--camera") && index + 1 < argc) {
                camera_id = argv[++index];
            } else if (!strcmp(argv[index], "--size") && index + 1 < argc) {
                if (parse_size(argv[++index], &width, &height) != 0) {
                    fprintf(stderr, "Invalid --size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--output-dir") &&
                       index + 1 < argc) {
                output_dir = argv[++index];
            } else if (!strcmp(argv[index], "--timeout") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &timeout_seconds) != 0 ||
                        timeout_seconds > INT_MAX / 1000) {
                    fprintf(stderr, "Invalid --timeout value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--quality") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &quality) != 0 ||
                        quality > 100) {
                    fprintf(stderr, "Invalid --quality value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--iso") && index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index],
                                          &sensor_sensitivity) != 0 ||
                        sensor_sensitivity <= 0) {
                    fprintf(stderr, "Invalid --iso value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--force")) {
                force = 1;
            } else {
                fprintf(stderr, "Unknown or incomplete option: %s\n",
                        argv[index]);
                usage(stderr, argv[0]);
                return 2;
            }
        }

        int failures = 0;
        int capture_errors = 0;
        printf("{\"status\":\"ok\",\"test\":\"shutter-calibration\","
               "\"camera_id\":\"%s\",\"iso_requested\":%d,"
               "\"width\":%d,\"height\":%d,\"results\":[\n",
               camera_id, sensor_sensitivity, width, height);
        for (size_t index = 0; index < sizeof(shutters) / sizeof(shutters[0]);
                ++index) {
            char output_path[PATH_MAX];
            if (snprintf(output_path, sizeof(output_path),
                         "%s/procam-shutter-calibration-%ld-%zu.jpg",
                         output_dir, (long)getpid(), index) >=
                    (int)sizeof(output_path)) {
                fprintf(stderr, "Calibration output path is too long\n");
                return 2;
            }
            if (!force && access(output_path, F_OK) == 0) {
                fprintf(stderr,
                        "Calibration output exists; pass --force or use "
                        "another --output-dir\n");
                return 3;
            }

            memset(json, 0, sizeof(json));
            long long requested_ns = shutters[index];
            int per_capture_timeout = timeout_seconds * 1000;
            long long requested_timeout = requested_ns / 1000000LL + 5000;
            if (requested_timeout > per_capture_timeout) {
                per_capture_timeout = requested_timeout > INT_MAX
                    ? INT_MAX : (int)requested_timeout;
            }
            long long started_ms = monotonic_ms();
            result = capture_jpeg(camera_id, width, height, output_path,
                                  per_capture_timeout, quality, 0,
                                  SFOS_CAMERA2_SCENE_NONE,
                                  sensor_sensitivity, requested_ns,
                                  0, SFOS_CAMERA2_NOISE_REDUCTION_NONE, 1.0f,
                                  json, sizeof(json));
            long long wall_ms = monotonic_ms() - started_ms;
            long long submit_ms = json_i64(json, "\"capture_submit\"", -1);
            long long image_ms = json_i64(json, "\"image_available\"", -1);
            long long observed_ms = submit_ms >= 0 && image_ms >= submit_ms
                ? image_ms - submit_ms : wall_ms;
            long long hal_iso = json_i64(json, "\"iso\"", -1);
            long long hal_shutter = json_i64(json, "\"exposure_time_ns\"", -1);
            long long hal_frame = json_i64(json, "\"frame_duration_ns\"", -1);
            int honored = result == 0 &&
                shutter_honored(requested_ns, observed_ms);

            fprintf(stderr,
                    "capture-calibration requested_shutter=%lld "
                    "requested_iso=%d hal_iso=%lld hal_shutter=%lld "
                    "hal_frame=%lld observed_ms=%lld wall_ms=%lld "
                    "honored=%d result=%d path=%s\n",
                    requested_ns, sensor_sensitivity, hal_iso, hal_shutter,
                    hal_frame, observed_ms, (long long)wall_ms, honored,
                    result, output_path);
            printf("%s  {\"requested_shutter_ns\":%lld,"
                   "\"requested_iso\":%d,\"hal_iso\":%lld,"
                   "\"hal_shutter_ns\":%lld,\"hal_frame_ns\":%lld,"
                   "\"observed_ms\":%lld,\"wall_ms\":%lld,"
                   "\"honored\":%s,\"result\":%d,\"path\":\"%s\"}\n",
                   index == 0 ? "" : ",\n",
                   requested_ns, sensor_sensitivity, hal_iso, hal_shutter,
                   hal_frame, observed_ms, (long long)wall_ms,
                   honored ? "true" : "false", result, output_path);

            if (result != 0 || !honored) {
                ++failures;
            }
            if (result != 0) {
                ++capture_errors;
            }
        }
        printf("],\"failures\":%d,\"capture_errors\":%d}\n",
               failures, capture_errors);
        return capture_errors > 0 ? 1 : 0;
    } else if (!strcmp(argv[1], "--capture-jpeg")) {
        const char *camera_id = "0";
        const char *output_path = NULL;
        int width = 4096;
        int height = 3072;
        int timeout_seconds = 10;
        int quality = 92;
        int orientation = 0;
        int scene_mode = SFOS_CAMERA2_SCENE_NONE;
        int sensor_sensitivity = 0;
        long long exposure_time_ns = 0;
        int aperture = 0;
        int noise_reduction = SFOS_CAMERA2_NOISE_REDUCTION_NONE;
        float zoom_ratio = 1.0f;
        int force = 0;

        if (!capture_jpeg) {
            fprintf(stderr,
                    "The installed bridge does not support direct JPEG capture. "
                    "Install libprocamcamera2.so 0.6.0 or newer.\n");
            return 14;
        }

        for (int index = 2; index < argc; ++index) {
            if (!strcmp(argv[index], "--camera") && index + 1 < argc) {
                camera_id = argv[++index];
            } else if (!strcmp(argv[index], "--size") && index + 1 < argc) {
                if (parse_size(argv[++index], &width, &height) != 0) {
                    fprintf(stderr, "Invalid --size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--output") && index + 1 < argc) {
                output_path = argv[++index];
            } else if (!strcmp(argv[index], "--timeout") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &timeout_seconds) != 0 ||
                        timeout_seconds > INT_MAX / 1000) {
                    fprintf(stderr, "Invalid --timeout value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--quality") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &quality) != 0 ||
                        quality > 100) {
                    fprintf(stderr, "Invalid --quality value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--orientation") &&
                       index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index], &orientation) != 0) {
                    fprintf(stderr, "Invalid --orientation value\n");
                    return 2;
                }
                orientation %= 360;
            } else if (!strcmp(argv[index], "--zoom") && index + 1 < argc) {
                if (parse_nonnegative_float(argv[++index], &zoom_ratio) != 0 ||
                        zoom_ratio < 1.0f) {
                    fprintf(stderr, "Invalid --zoom value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--scene") && index + 1 < argc) {
                if (parse_scene_mode(argv[++index], &scene_mode) != 0) {
                    fprintf(stderr, "Invalid --scene mode\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--portrait")) {
                scene_mode = SFOS_CAMERA2_SCENE_PORTRAIT;
            } else if (!strcmp(argv[index], "--iso") && index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index],
                                          &sensor_sensitivity) != 0) {
                    fprintf(stderr, "Invalid --iso value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--shutter-ns") &&
                       index + 1 < argc) {
                if (parse_nonnegative_i64(argv[++index],
                                          &exposure_time_ns) != 0) {
                    fprintf(stderr, "Invalid --shutter-ns value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--noise-reduction") &&
                       index + 1 < argc) {
                if (parse_noise_reduction(argv[++index],
                                          &noise_reduction) != 0) {
                    fprintf(stderr, "Invalid --noise-reduction value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--ev-steps") && index + 1 < argc) {
                int steps = 0;
                if (parse_int(argv[++index], &steps) != 0 || (steps != 0 && !set_capture_compensation)) {
                    fprintf(stderr, "Exposure compensation unavailable or invalid\n");
                    return 2;
                }
                if (set_capture_compensation) set_capture_compensation(steps);
            } else if (!strcmp(argv[index], "--aperture") &&
                       index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index], &aperture) != 0 ||
                        aperture > 255) {
                    fprintf(stderr, "Invalid --aperture value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--force")) {
                force = 1;
            } else {
                fprintf(stderr, "Unknown or incomplete option: %s\n",
                        argv[index]);
                usage(stderr, argv[0]);
                return 2;
            }
        }
        if (!output_path || !*output_path) {
            fprintf(stderr, "--output PATH is required for JPEG capture\n");
            return 2;
        }
        if (!force && access(output_path, F_OK) == 0) {
            fprintf(stderr,
                    "Output exists; choose another path or pass --force\n");
            return 3;
        }
        result = capture_jpeg(camera_id, width, height, output_path,
                              timeout_seconds * 1000, quality, orientation,
                              scene_mode, sensor_sensitivity, exposure_time_ns,
                              aperture, noise_reduction, zoom_ratio,
                              json, sizeof(json));
        if (result == 0 && write_json_sidecar(output_path, json) != 0) {
            fprintf(stderr, "Could not write JPEG metadata sidecar\n");
            return 15;
        }
    } else if (!strcmp(argv[1], "--record-video")) {
        const char *camera_id = "0";
        const char *output_path = NULL;
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int seconds = 10;
        int bitrate = 20000000;
        int stabilization = 1;
        int hevc = 0;
        int orientation = 0;
        int audio = 1;
        int audio_channels = 1;
        int repeat = 1;
        int force = 0;
        int32_t vendor_tags[8];
        int32_t vendor_values[8];
        int vendor_count = 0;
        int force_fps = 0;

        if (!record_video || !record_audio_push) {
            fprintf(stderr, "The installed bridge does not support video "
                            "recording. Install libprocamcamera2.so 0.13.0 "
                            "or newer.\n");
            return 14;
        }
        for (int index = 2; index < argc; ++index) {
            if (!strcmp(argv[index], "--camera") && index + 1 < argc) {
                camera_id = argv[++index];
            } else if (!strcmp(argv[index], "--size") && index + 1 < argc) {
                if (parse_size(argv[++index], &width, &height) != 0) {
                    fprintf(stderr, "Invalid --size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--output") && index + 1 < argc) {
                output_path = argv[++index];
            } else if (!strcmp(argv[index], "--fps") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &fps) != 0 || fps > 240) {
                    fprintf(stderr, "Invalid --fps value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--seconds") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &seconds) != 0 ||
                        seconds > 600) {
                    fprintf(stderr, "Invalid --seconds value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--bitrate") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &bitrate) != 0) {
                    fprintf(stderr, "Invalid --bitrate value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--stabilization") &&
                       index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "on")) {
                    stabilization = 1;
                } else if (!strcmp(value, "off")) {
                    stabilization = 0;
                } else {
                    fprintf(stderr, "--stabilization is on or off\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--codec") && index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "avc")) {
                    hevc = 0;
                } else if (!strcmp(value, "hevc")) {
                    hevc = 1;
                } else {
                    fprintf(stderr, "--codec is avc or hevc\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--orientation") &&
                       index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index], &orientation) != 0 ||
                        orientation % 90 != 0 || orientation >= 360) {
                    fprintf(stderr, "Invalid --orientation value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--audio") && index + 1 < argc) {
                const char *value = argv[++index];
                if (!strcmp(value, "on")) {
                    audio = 1;
                } else if (!strcmp(value, "off")) {
                    audio = 0;
                } else {
                    fprintf(stderr, "--audio is on or off\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--repeat") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &repeat) != 0 ||
                        repeat > 4) {
                    fprintf(stderr, "--repeat is 1 to 4\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--audio-channels") &&
                       index + 1 < argc) {
                if (parse_positive_int(argv[++index], &audio_channels) != 0 ||
                        audio_channels > 2) {
                    fprintf(stderr, "--audio-channels is 1 or 2\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--force")) {
                force = 1;
            } else if (!strcmp(argv[index], "--force-fps")) {
                force_fps = 1;
            } else if (!strcmp(argv[index], "--vendor") && index + 1 < argc) {
                /* TAG=VALUE, both decimal or 0x hex: an int32 vendor setting
                 * for the session and every request (diagnostics). */
                char *end = NULL;
                const char *text = argv[++index];
                const unsigned long tag = strtoul(text, &end, 0);
                if (!end || *end != '=' || vendor_count >= 8) {
                    fprintf(stderr, "--vendor is TAG=VALUE (at most 8)\n");
                    return 2;
                }
                const long value = strtol(end + 1, &end, 0);
                if (*end) {
                    fprintf(stderr, "--vendor is TAG=VALUE (at most 8)\n");
                    return 2;
                }
                vendor_tags[vendor_count] = (int32_t)tag;
                vendor_values[vendor_count] = (int32_t)value;
                ++vendor_count;
            } else {
                fprintf(stderr, "Unknown or incomplete option: %s\n",
                        argv[index]);
                usage(stderr, argv[0]);
                return 2;
            }
        }
        if (!output_path || !*output_path) {
            fprintf(stderr, "--output PATH.mp4 is required for recording\n");
            return 2;
        }
        if (!force && access(output_path, F_OK) == 0) {
            fprintf(stderr,
                    "Output exists; choose another path or pass --force\n");
            return 3;
        }
        struct audio_capture capture = {
            .push = record_audio_push,
            .sample_rate = 48000,
            .channels = audio_channels,
        };
        pthread_t audio_thread;
        int audio_running = 0;
        if (audio && audio_capture_open(&capture) == 0) {
            atomic_init(&capture.stop, 0);
            audio_running = pthread_create(&audio_thread, NULL,
                                           audio_capture_main, &capture) == 0;
            if (!audio_running) {
                pa_simple_free(capture.stream);
                capture.stream = NULL;
            }
        }
        record_set_vendor_fn set_vendor = (record_set_vendor_fn)android_dlsym(
            bridge, "sfos_camera2_record_set_vendor");
        if ((vendor_count || force_fps) && !set_vendor) {
            fprintf(stderr, "The installed bridge has no vendor video settings.\n");
            return 14;
        }
        if (set_vendor) {
            set_vendor(vendor_tags, vendor_values, vendor_count, force_fps);
        }
        result = record_video(camera_id, width, height, fps, bitrate,
                              seconds * 1000, stabilization, hevc, orientation,
                              capture.sample_rate,
                              audio_running ? audio_channels : 0, repeat,
                              output_path, json, sizeof(json));
        if (audio_running) {
            atomic_store(&capture.stop, 1);
            pthread_join(audio_thread, NULL);
            pa_simple_free(capture.stream);
            if (capture.error) {
                fprintf(stderr, "Microphone read failed after %d reads: %s\n",
                        capture.reads, pa_strerror(capture.error));
            }
        }
    } else if (!strcmp(argv[1], "--capture")) {
        const char *camera_id = "0";
        const char *prefix = NULL;
        int width = 4096;
        int height = 3072;
        int timeout_seconds = 30;
        int focus_mode = FOCUS_NONE;
        float focus_distance = 0.0f;
        int focus_timeout_seconds = 3;
        int capture_on_focus_failure = 1;
        int scene_mode = SFOS_CAMERA2_SCENE_NONE;
        int color_temperature = 0;
        int color_tint = 0;
        int sensor_sensitivity = 0;
        long long exposure_time_ns = 0;
        int aperture = 0;
        int noise_reduction = SFOS_CAMERA2_NOISE_REDUCTION_NONE;
        int raw_format = SFOS_CAMERA2_RAW_FORMAT_RAW16;
        float zoom_ratio = 1.0f;
        float focus_x = -1.0f;
        float focus_y = -1.0f;
        int force = 0;

        for (int index = 2; index < argc; ++index) {
            if (!strcmp(argv[index], "--camera") && index + 1 < argc) {
                camera_id = argv[++index];
            } else if (!strcmp(argv[index], "--size") && index + 1 < argc) {
                if (parse_size(argv[++index], &width, &height) != 0) {
                    fprintf(stderr, "Invalid --size value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--output") && index + 1 < argc) {
                prefix = argv[++index];
            } else if (!strcmp(argv[index], "--timeout") && index + 1 < argc) {
                if (parse_positive_int(argv[++index], &timeout_seconds) != 0 ||
                        timeout_seconds > INT_MAX / 1000) {
                    fprintf(stderr, "Invalid --timeout value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus") && index + 1 < argc) {
                if (parse_focus_mode(argv[++index], &focus_mode) != 0) {
                    fprintf(stderr, "Invalid --focus mode\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-distance") &&
                       index + 1 < argc) {
                if (parse_nonnegative_float(argv[++index],
                                            &focus_distance) != 0) {
                    fprintf(stderr, "Invalid --focus-distance value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-timeout") &&
                       index + 1 < argc) {
                if (parse_positive_int(argv[++index],
                                       &focus_timeout_seconds) != 0 ||
                        focus_timeout_seconds > INT_MAX / 1000) {
                    fprintf(stderr, "Invalid --focus-timeout value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-failure") &&
                       index + 1 < argc) {
                const char *policy = argv[++index];
                if (!strcmp(policy, "capture")) {
                    capture_on_focus_failure = 1;
                } else if (!strcmp(policy, "abort")) {
                    capture_on_focus_failure = 0;
                } else {
                    fprintf(stderr,
                            "--focus-failure must be capture or abort\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-x") && index + 1 < argc) {
                if (parse_unit_float(argv[++index], &focus_x) != 0) {
                    fprintf(stderr, "Invalid --focus-x value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--focus-y") && index + 1 < argc) {
                if (parse_unit_float(argv[++index], &focus_y) != 0) {
                    fprintf(stderr, "Invalid --focus-y value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--scene") && index + 1 < argc) {
                if (parse_scene_mode(argv[++index], &scene_mode) != 0) {
                    fprintf(stderr, "Invalid --scene mode\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--portrait")) {
                scene_mode = SFOS_CAMERA2_SCENE_PORTRAIT;
            } else if (!strcmp(argv[index], "--color-temperature") &&
                       index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index],
                                          &color_temperature) != 0) {
                    fprintf(stderr, "Invalid --color-temperature value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--color-tint") &&
                       index + 1 < argc) {
                if (parse_int(argv[++index], &color_tint) != 0) {
                    fprintf(stderr, "Invalid --color-tint value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--iso") && index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index],
                                          &sensor_sensitivity) != 0) {
                    fprintf(stderr, "Invalid --iso value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--shutter-ns") &&
                       index + 1 < argc) {
                if (parse_nonnegative_i64(argv[++index],
                                          &exposure_time_ns) != 0) {
                    fprintf(stderr, "Invalid --shutter-ns value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--noise-reduction") &&
                       index + 1 < argc) {
                if (parse_noise_reduction(argv[++index],
                                          &noise_reduction) != 0) {
                    fprintf(stderr, "Invalid --noise-reduction value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--ev-steps") && index + 1 < argc) {
                int steps = 0;
                if (parse_int(argv[++index], &steps) != 0 || (steps != 0 && !set_capture_compensation)) {
                    fprintf(stderr, "Exposure compensation unavailable or invalid\n");
                    return 2;
                }
                if (set_capture_compensation) set_capture_compensation(steps);
            } else if (!strcmp(argv[index], "--aperture") &&
                       index + 1 < argc) {
                if (parse_nonnegative_int(argv[++index], &aperture) != 0 ||
                        aperture > 255) {
                    fprintf(stderr, "Invalid --aperture value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--zoom") && index + 1 < argc) {
                if (parse_nonnegative_float(argv[++index], &zoom_ratio) != 0 ||
                        zoom_ratio < 1.0f) {
                    fprintf(stderr, "Invalid --zoom value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--raw-format") && index + 1 < argc) {
                if (parse_raw_format(argv[++index], &raw_format) != 0) {
                    fprintf(stderr, "Invalid --raw-format value\n");
                    return 2;
                }
            } else if (!strcmp(argv[index], "--force")) {
                force = 1;
            } else {
                fprintf(stderr, "Unknown or incomplete option: %s\n",
                        argv[index]);
                usage(stderr, argv[0]);
                return 2;
            }
        }
        if (!prefix || !*prefix) {
            fprintf(stderr, "--output PREFIX is required for capture\n");
            return 2;
        }
        if ((focus_x < 0.0f) != (focus_y < 0.0f)) {
            fprintf(stderr, "--focus-x and --focus-y must be used together\n");
            return 2;
        }

        char raw_path[PATH_MAX];
        char metadata_path[PATH_MAX];
        const char *raw_suffix = raw_format == SFOS_CAMERA2_RAW_FORMAT_RAW10
                ? ".raw10" : ".raw16";
        if (snprintf(raw_path, sizeof(raw_path), "%s%s", prefix, raw_suffix) >=
                    (int)sizeof(raw_path) ||
                snprintf(metadata_path, sizeof(metadata_path), "%s.json", prefix) >=
                    (int)sizeof(metadata_path)) {
            fprintf(stderr, "Output prefix is too long\n");
            return 2;
        }
        if (!force && (access(raw_path, F_OK) == 0 ||
                       access(metadata_path, F_OK) == 0)) {
            fprintf(stderr,
                    "Output exists; choose another prefix or pass --force\n");
            return 3;
        }

        struct sfos_camera2_capture_options options = {
            .size = sizeof(options),
            .focus_mode = focus_mode,
            .focus_distance_diopters = focus_distance,
            .focus_timeout_ms = focus_timeout_seconds * 1000,
            .capture_on_focus_failure = capture_on_focus_failure,
            .scene_mode = scene_mode,
            .color_temperature_kelvin = color_temperature,
            .color_tint = color_tint,
            .zoom_ratio = zoom_ratio,
            .sensor_sensitivity = sensor_sensitivity,
            .exposure_time_ns = exposure_time_ns,
            .aperture = aperture,
            .noise_reduction = noise_reduction,
            .raw_format = raw_format,
            .focus_x = focus_x,
            .focus_y = focus_y,
        };
        result = capture_options(
            camera_id, width, height, raw_path, metadata_path,
            timeout_seconds * 1000, &options, json, sizeof(json));
    } else {
        usage(stderr, argv[0]);
        return 2;
    }

    if (json[0]) {
        puts(json);
    }
    if (result != 0) {
        fprintf(stderr, "Camera2 bridge %s failed with code %d\n",
                version(), result);
        return 20 - result;
    }

    return 0;
}
