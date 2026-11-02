/**
 * @file recorder.c
 * @brief MP4 recorder: hardware video encoder on a persistent input surface,
 * AAC from pushed microphone PCM, Android muxer.
 *
 * Timing: the camera framework stamps frames sent to an encoder surface on
 * CLOCK_MONOTONIC (sensor timestamps on the boot clock are converted), and
 * microphone chunks carry both clocks; the first encoded frame decides which
 * one matches, so audio lines up even if a platform differs. Encoded video
 * is held in memory until the audio format is known (the muxer needs every
 * track before it starts), and audio from before the first frame or after
 * the stop is dropped.
 */

#include "recorder.h"
#include "camera2_bridge.h"
#include "camera2_common.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define RECORDER_COLOR_FORMAT_SURFACE 0x7F000789
#define RECORDER_BUFFER_FLAG_KEY_FRAME 1
#define RECORDER_AAC_LC 2
#define RECORDER_BITRATE_MODE_CBR 2
#define RECORDER_AVC_PROFILE_HIGH 8
#define RECORDER_HEVC_PROFILE_MAIN 1
#define RECORDER_AUDIO_BITRATE 128000
/* Without an audio format this long after the first frame, the file gets
 * video only rather than nothing. */
#define RECORDER_AUDIO_WAIT_MS 1500
/* Encoded samples held while the muxer waits for the other track. */
#define RECORDER_PENDING_MAX 600

/* API 26; the bridge targets API 24, so these are looked up. */
typedef media_status_t (*create_persistent_surface_fn)(ANativeWindow **);
typedef media_status_t (*release_persistent_surface_fn)(const ANativeWindow *);
typedef media_status_t (*set_input_surface_fn)(AMediaCodec *, ANativeWindow *);
typedef media_status_t (*signal_end_of_input_fn)(AMediaCodec *);

/* Microphone PCM: a single-producer single-consumer ring of chunks,
 * lock-free so the Sailfish (glibc) thread pushing it never takes a bionic
 * lock. Each chunk carries its capture time on both clocks. */
#define AUDIO_RING_CHUNKS 256
#define AUDIO_CHUNK_SAMPLES 2048

struct audio_chunk {
    int samples;
    int64_t monotonic_us;
    int64_t boottime_us;
    int16_t pcm[AUDIO_CHUNK_SAMPLES];
};

static struct audio_chunk g_audio_ring[AUDIO_RING_CHUNKS];
static atomic_uint g_audio_head;
static atomic_uint g_audio_tail;
static atomic_int g_audio_accepting;
static atomic_int g_audio_dropped;
static atomic_int g_audio_pushed;

static int64_t clock_us(clockid_t clock)
{
    struct timespec now;
    clock_gettime(clock, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

void sfos_camera2_record_audio_push(const int16_t *pcm, int samples, long long age_us)
{
    if (!pcm || samples <= 0 || samples > AUDIO_CHUNK_SAMPLES ||
            !atomic_load_explicit(&g_audio_accepting, memory_order_acquire)) {
        return;
    }
    const unsigned head = atomic_load_explicit(&g_audio_head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&g_audio_tail, memory_order_acquire);
    if (head - tail >= AUDIO_RING_CHUNKS) {
        atomic_fetch_add(&g_audio_dropped, 1);
        return;
    }
    struct audio_chunk *chunk = &g_audio_ring[head % AUDIO_RING_CHUNKS];
    chunk->samples = samples;
    chunk->monotonic_us = clock_us(CLOCK_MONOTONIC) - age_us;
    chunk->boottime_us = clock_us(CLOCK_BOOTTIME) - age_us;
    memcpy(chunk->pcm, pcm, (size_t)samples * sizeof(int16_t));
    atomic_store_explicit(&g_audio_head, head + 1, memory_order_release);
    atomic_fetch_add(&g_audio_pushed, 1);
}

enum recorder_clock { RECORDER_CLOCK_UNDECIDED = 0, RECORDER_CLOCK_MONOTONIC, RECORDER_CLOCK_BOOTTIME };

struct pending_sample {
    uint8_t *data;
    AMediaCodecBufferInfo info;
};

struct track {
    AMediaCodec *codec;
    bool started;
    AMediaFormat *format;
    ssize_t index;
    bool ignored;
    bool eos;
    int frames;
    int keyframes;
    long long bytes;
    int64_t first_us;
    int64_t last_us;
    struct pending_sample *pending;
    int pending_count;
};

struct recorder {
    struct recorder_config config;
    ANativeWindow *surface;
    struct track video;
    struct track audio;
    AMediaMuxer *muxer;
    int fd;
    bool recording;
    bool muxer_started;
    bool stopping;
    int clock;
    int64_t first_frame_ms;
    /* recorder_begin() on both clocks, to spot frames from before it. */
    int64_t begin_monotonic_us;
    int64_t begin_boottime_us;
    int stale_frames;
    /* Configured with constant bitrate and the High/Main profile. */
    bool tuned;
    int audio_skipped;
    int audio_late;
    int output_errno;
    char path[4096];
};

static void *media_symbol(const char *name)
{
    return dlsym(RTLD_DEFAULT, name);
}

ANativeWindow *recorder_surface_create(void)
{
    create_persistent_surface_fn create =
        (create_persistent_surface_fn)media_symbol("AMediaCodec_createPersistentInputSurface");
    ANativeWindow *surface = NULL;
    if (!create || create(&surface) != AMEDIA_OK) return NULL;
    return surface;
}

void recorder_surface_release(ANativeWindow *surface)
{
    release_persistent_surface_fn release =
        (release_persistent_surface_fn)media_symbol("AMediaCodec_releasePersistentInputSurface");
    if (surface && release) release(surface);
}

struct recorder *recorder_prepare(const struct recorder_config *config,
                                  ANativeWindow *surface, const char **stage,
                                  int *media_status)
{
    set_input_surface_fn set_input_surface =
        (set_input_surface_fn)media_symbol("AMediaCodec_setInputSurface");
    *media_status = 0;
    if (!config || !surface || !set_input_surface ||
            !media_symbol("AMediaCodec_signalEndOfInputStream")) {
        *stage = "encoder_api";
        return NULL;
    }
    struct recorder *recorder = calloc(1, sizeof(*recorder));
    if (!recorder) {
        *stage = "allocation";
        return NULL;
    }
    recorder->config = *config;
    recorder->surface = surface;
    recorder->fd = -1;
    recorder->video.index = -1;
    recorder->audio.index = -1;

    const char *mime = config->hevc ? "video/hevc" : "video/avc";
    recorder->video.codec = AMediaCodec_createEncoderByType(mime);
    media_status_t status = AMEDIA_ERROR_UNKNOWN;
    /* Tuned first (constant bitrate, High/Main profile, real-time): in
     * variable mode the encoder spent a quarter of the bitrate on noisy
     * scenes and blocked them up. The encoder's defaults if it refuses. */
    for (int tuned = 1; tuned >= 0 && recorder->video.codec && status != AMEDIA_OK; --tuned) {
        AMediaFormat *format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, config->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, config->height);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, config->bitrate);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, config->fps);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, RECORDER_COLOR_FORMAT_SURFACE);
        if (tuned) {
            AMediaFormat_setInt32(format, "bitrate-mode", RECORDER_BITRATE_MODE_CBR);
            AMediaFormat_setInt32(format, "profile", config->hevc ? RECORDER_HEVC_PROFILE_MAIN
                                                                  : RECORDER_AVC_PROFILE_HIGH);
            AMediaFormat_setInt32(format, "priority", 0);
        }
        status = AMediaCodec_configure(recorder->video.codec, format, NULL, NULL,
                                       AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(format);
        recorder->tuned = tuned && status == AMEDIA_OK;
        if (status != AMEDIA_OK && tuned) {
            /* A refused configure can leave the codec unusable: a new one. */
            AMediaCodec_delete(recorder->video.codec);
            recorder->video.codec = AMediaCodec_createEncoderByType(mime);
        }
    }
    if (status == AMEDIA_OK) status = set_input_surface(recorder->video.codec, surface);
    if (status == AMEDIA_OK) status = AMediaCodec_start(recorder->video.codec);
    if (status != AMEDIA_OK) {
        *stage = recorder->video.codec ? "video_encoder" : "video_encoder_create";
        *media_status = status;
        recorder_destroy(recorder);
        return NULL;
    }
    recorder->video.started = true;

    if (config->audio_channels > 0) {
        AMediaFormat *format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, config->audio_sample_rate);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, config->audio_channels);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, RECORDER_AUDIO_BITRATE);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_AAC_PROFILE, RECORDER_AAC_LC);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                              AUDIO_CHUNK_SAMPLES * (int)sizeof(int16_t));
        recorder->audio.codec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        status = recorder->audio.codec
                ? AMediaCodec_configure(recorder->audio.codec, format, NULL, NULL,
                                        AMEDIACODEC_CONFIGURE_FLAG_ENCODE)
                : AMEDIA_ERROR_UNKNOWN;
        AMediaFormat_delete(format);
        if (status == AMEDIA_OK) status = AMediaCodec_start(recorder->audio.codec);
        if (status != AMEDIA_OK) {
            *stage = "audio_encoder";
            *media_status = status;
            recorder_destroy(recorder);
            return NULL;
        }
        recorder->audio.started = true;
    } else {
        recorder->audio.ignored = true;
        recorder->audio.eos = true;
    }
    return recorder;
}

int recorder_begin(struct recorder *recorder, const char *path,
                   int orientation_degrees, const char **stage)
{
    if (!recorder || recorder->recording || recorder->muxer || !path) {
        *stage = "state";
        return RECORDER_STATE_ERROR;
    }
    snprintf(recorder->path, sizeof(recorder->path), "%s", path);
    recorder->fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (recorder->fd < 0) {
        recorder->output_errno = errno;
        *stage = "output_open";
        return RECORDER_OUTPUT_ERROR;
    }
    recorder->muxer = AMediaMuxer_new(recorder->fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!recorder->muxer) {
        *stage = "muxer_create";
        return RECORDER_MUXER_ERROR;
    }
    AMediaMuxer_setOrientationHint(recorder->muxer, ((orientation_degrees % 360) + 360) % 360);
    recorder->begin_monotonic_us = clock_us(CLOCK_MONOTONIC);
    recorder->begin_boottime_us = clock_us(CLOCK_BOOTTIME);
    atomic_store(&g_audio_tail, atomic_load(&g_audio_head));
    atomic_store(&g_audio_dropped, 0);
    atomic_store(&g_audio_pushed, 0);
    atomic_store_explicit(&g_audio_accepting, recorder->audio.ignored ? 0 : 1,
                          memory_order_release);
    recorder->recording = true;
    return 0;
}

bool recorder_recording(const struct recorder *recorder)
{
    return recorder && recorder->recording;
}

static int recorder_write(struct recorder *recorder, struct track *track,
                          const uint8_t *data, const AMediaCodecBufferInfo *info)
{
    if (AMediaMuxer_writeSampleData(recorder->muxer, (size_t)track->index, data, info) != AMEDIA_OK)
        return -1;
    return 0;
}

static int recorder_start_muxer(struct recorder *recorder)
{
    struct track *tracks[] = { &recorder->video, &recorder->audio };
    for (int i = 0; i < 2; ++i) {
        struct track *track = tracks[i];
        if (track->ignored || !track->format) continue;
        track->index = AMediaMuxer_addTrack(recorder->muxer, track->format);
        if (track->index < 0) return -1;
    }
    if (AMediaMuxer_start(recorder->muxer) != AMEDIA_OK) return -1;
    recorder->muxer_started = true;
    for (int i = 0; i < 2; ++i) {
        struct track *track = tracks[i];
        for (int k = 0; k < track->pending_count; ++k) {
            struct pending_sample *sample = &track->pending[k];
            if (!track->ignored && recorder_write(recorder, track, sample->data, &sample->info) < 0)
                return -1;
            free(sample->data);
        }
        free(track->pending);
        track->pending = NULL;
        track->pending_count = 0;
    }
    return 0;
}

/* One encoded sample: decides the audio clock at the first frame, then
 * writes it or holds it until the muxer starts. */
static int recorder_sample(struct recorder *recorder, struct track *track,
                           const uint8_t *data, const AMediaCodecBufferInfo *info)
{
    if (track == &recorder->video && track->frames == 0) {
        const int64_t to_monotonic = llabs(clock_us(CLOCK_MONOTONIC) - info->presentationTimeUs);
        const int64_t to_boottime = llabs(clock_us(CLOCK_BOOTTIME) - info->presentationTimeUs);
        recorder->clock = to_boottime < to_monotonic ? RECORDER_CLOCK_BOOTTIME
                                                     : RECORDER_CLOCK_MONOTONIC;
        recorder->first_frame_ms = sfos_camera2_now_ms();
    }
    if (track == &recorder->video &&
            info->presentationTimeUs < (recorder->clock == RECORDER_CLOCK_BOOTTIME
                                        ? recorder->begin_boottime_us
                                        : recorder->begin_monotonic_us)) {
        ++recorder->stale_frames;
    }
    if (track->frames == 0) track->first_us = info->presentationTimeUs;
    track->last_us = info->presentationTimeUs;
    ++track->frames;
    if (info->flags & RECORDER_BUFFER_FLAG_KEY_FRAME) ++track->keyframes;
    track->bytes += info->size;
    if (recorder->muxer_started) return recorder_write(recorder, track, data, info);
    if (track->pending_count >= RECORDER_PENDING_MAX) return -1;
    if (!track->pending) {
        track->pending = calloc(RECORDER_PENDING_MAX, sizeof(*track->pending));
        if (!track->pending) return -1;
    }
    struct pending_sample *sample = &track->pending[track->pending_count];
    sample->data = malloc((size_t)info->size);
    if (!sample->data) return -1;
    memcpy(sample->data, data, (size_t)info->size);
    sample->info = *info;
    sample->info.offset = 0;
    ++track->pending_count;
    return 0;
}

/* Drains one encoder without waiting longer than timeout_us. */
static int recorder_drain(struct recorder *recorder, struct track *track, int timeout_us)
{
    if (!track->codec || track->eos) return 0;
    for (;;) {
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(track->codec, &info, timeout_us);
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
        timeout_us = 0;
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (!track->format && !recorder->muxer_started)
                track->format = AMediaCodec_getOutputFormat(track->codec);
            else if (!recorder->muxer_started || track->index < 0)
                track->ignored = true; /* too late for this file */
            continue;
        }
        if (index < 0) continue;
        size_t size = 0;
        uint8_t *data = AMediaCodec_getOutputBuffer(track->codec, (size_t)index, &size);
        int result = 0;
        if (data && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) &&
                !track->ignored && recorder->recording) {
            result = recorder_sample(recorder, track, data + info.offset, &info);
        }
        AMediaCodec_releaseOutputBuffer(track->codec, (size_t)index, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) track->eos = true;
        if (result < 0) return -1;
        if (track->eos) return 0;
    }
}

/* Queued microphone chunks into the AAC encoder, from the first video frame
 * to the newest one: audio waits for the video, so that once the video has
 * ended (stopping) the sound can end with its last frame. */
static void recorder_feed_audio(struct recorder *recorder)
{
    struct track *audio = &recorder->audio;
    if (audio->ignored || !audio->codec || recorder->clock == RECORDER_CLOCK_UNDECIDED) return;
    const int channels = recorder->config.audio_channels;
    const int rate = recorder->config.audio_sample_rate;
    const int64_t video_end_us = recorder->video.last_us + 1000000 / recorder->config.fps;
    for (;;) {
        const unsigned tail = atomic_load_explicit(&g_audio_tail, memory_order_relaxed);
        if (tail == atomic_load_explicit(&g_audio_head, memory_order_acquire)) return;
        const struct audio_chunk *chunk = &g_audio_ring[tail % AUDIO_RING_CHUNKS];
        const int64_t pts = recorder->clock == RECORDER_CLOCK_BOOTTIME ? chunk->boottime_us
                                                                       : chunk->monotonic_us;
        const int64_t duration = (int64_t)chunk->samples / channels * 1000000 / rate;
        if (pts + duration <= recorder->video.first_us) {
            ++recorder->audio_skipped;
        } else if (pts + duration > video_end_us && !recorder->stopping) {
            return; /* the video has not got this far yet */
        } else if (pts >= video_end_us) {
            ++recorder->audio_late;
        } else {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(audio->codec, 0);
            if (index < 0) return;
            size_t capacity = 0;
            uint8_t *buffer = AMediaCodec_getInputBuffer(audio->codec, (size_t)index, &capacity);
            size_t bytes = buffer ? (size_t)chunk->samples * sizeof(int16_t) : 0;
            if (bytes > capacity) bytes = capacity;
            if (bytes) memcpy(buffer, chunk->pcm, bytes);
            AMediaCodec_queueInputBuffer(audio->codec, (size_t)index, 0, bytes, (uint64_t)pts, 0);
        }
        atomic_store_explicit(&g_audio_tail, tail + 1, memory_order_release);
    }
}

static int recorder_step(struct recorder *recorder, int video_timeout_us, const char **stage)
{
    if (recorder_drain(recorder, &recorder->video, video_timeout_us) < 0) {
        *stage = "mux_video";
        return RECORDER_MUXER_ERROR;
    }
    recorder_feed_audio(recorder);
    if (recorder_drain(recorder, &recorder->audio,
                       recorder->video.eos ? video_timeout_us : 0) < 0) {
        *stage = "mux_audio";
        return RECORDER_MUXER_ERROR;
    }
    if (!recorder->audio.ignored && !recorder->audio.format && recorder->first_frame_ms &&
            sfos_camera2_now_ms() - recorder->first_frame_ms > RECORDER_AUDIO_WAIT_MS) {
        /* No audio after all: the video alone rather than nothing. */
        recorder->audio.ignored = true;
        atomic_store(&g_audio_accepting, 0);
    }
    if (!recorder->muxer_started && recorder->video.format &&
            (recorder->audio.ignored || recorder->audio.format) &&
            recorder_start_muxer(recorder) < 0) {
        *stage = "muxer_start";
        return RECORDER_MUXER_ERROR;
    }
    return 0;
}

int recorder_poll(struct recorder *recorder, const char **stage)
{
    if (!recorder || !recorder->recording) return 0;
    return recorder_step(recorder, 0, stage);
}

int recorder_finish(struct recorder *recorder, char *json, size_t json_size)
{
    const char *stage = "done";
    int code = 0;
    if (!recorder || !recorder->recording) {
        if (json && json_size) snprintf(json, json_size, "{\"status\":\"error\",\"stage\":\"state\"}");
        return RECORDER_STATE_ERROR;
    }
    signal_end_of_input_fn signal_end =
        (signal_end_of_input_fn)media_symbol("AMediaCodec_signalEndOfInputStream");
    /* The video to its end first: its last frame is where the sound ends. */
    signal_end(recorder->video.codec);
    const int64_t video_deadline = sfos_camera2_now_ms() + 2000;
    while (!recorder->video.eos && sfos_camera2_now_ms() < video_deadline) {
        code = recorder_step(recorder, 10000, &stage);
        if (code < 0) break;
    }
    recorder->stopping = true;
    atomic_store(&g_audio_accepting, 0);

    /* Audio up to the last frame, then its end of stream. */
    if (code == 0 && !recorder->audio.ignored) {
        const int64_t deadline = sfos_camera2_now_ms() + 500;
        while (atomic_load(&g_audio_tail) != atomic_load(&g_audio_head) &&
                sfos_camera2_now_ms() < deadline && !recorder->audio.ignored) {
            if ((code = recorder_step(recorder, 0, &stage)) < 0) break;
            sfos_camera2_sleep_10_ms();
        }
        ssize_t index = -1;
        while (index < 0 && sfos_camera2_now_ms() < deadline && !recorder->audio.ignored) {
            index = AMediaCodec_dequeueInputBuffer(recorder->audio.codec, 10000);
            if (index < 0 && recorder_step(recorder, 0, &stage) < 0) break;
        }
        if (index >= 0) {
            AMediaCodec_queueInputBuffer(recorder->audio.codec, (size_t)index, 0, 0,
                                         (uint64_t)(recorder->audio.last_us + 1),
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        } else {
            recorder->audio.eos = true;
        }
    }
    const int64_t deadline = sfos_camera2_now_ms() + 1000;
    while (code == 0 && (!recorder->video.eos || (!recorder->audio.eos && !recorder->audio.ignored)) &&
            sfos_camera2_now_ms() < deadline) {
        code = recorder_step(recorder, 10000, &stage);
        if (code < 0) break;
    }
    if (code == 0 && !recorder->muxer_started && recorder->video.format) {
        /* Audio never produced a format: write the video alone. */
        recorder->audio.ignored = true;
        if (recorder_start_muxer(recorder) < 0) {
            code = RECORDER_MUXER_ERROR;
            stage = "muxer_start";
        }
    }
    if (code == 0 && recorder->video.frames == 0) {
        code = RECORDER_NO_FRAMES;
        stage = "no_frames";
    }
    if (recorder->muxer) {
        if (recorder->muxer_started) AMediaMuxer_stop(recorder->muxer);
        AMediaMuxer_delete(recorder->muxer);
        recorder->muxer = NULL;
    }
    if (recorder->fd >= 0) {
        close(recorder->fd);
        recorder->fd = -1;
    }
    recorder->recording = false;

    const struct track *video = &recorder->video, *audio = &recorder->audio;
    const double seconds = video->frames > 1 ? (video->last_us - video->first_us) / 1e6 : 0.0;
    const double audio_seconds = audio->frames > 1 ? (audio->last_us - audio->first_us) / 1e6 : 0.0;
    const bool audio_written = !audio->ignored && audio->frames > 0;
    if (json && json_size) {
        snprintf(json, json_size,
                 "{\"status\":\"%s\",\"stage\":\"%s\",\"code\":%d,\"path\":\"%s\","
                 "\"output_error\":\"%s\",\"codec\":\"%s\",\"width\":%d,\"height\":%d,"
                 "\"frames\":%d,\"keyframes\":%d,\"seconds\":%.3f,\"fps\":%.2f,"
                 "\"bytes\":%lld,\"bitrate\":%.0f,\"target_bitrate\":%d,\"tuned\":%s,"
                 "\"stale_frames\":%d,"
                 "\"audio\":{\"requested\":%s,\"written\":%s,\"aac_frames\":%d,\"seconds\":%.3f,"
                 "\"chunks_pushed\":%d,\"chunks_dropped\":%d,\"chunks_before_video\":%d,"
                 "\"chunks_after_stop\":%d,\"start_offset_ms\":%.1f,\"end_offset_ms\":%.1f},"
                 "\"clock\":\"%s\"}",
                 code == 0 ? "ok" : "error", stage, code, recorder->path,
                 recorder->output_errno ? strerror(recorder->output_errno) : "",
                 recorder->config.hevc ? "hevc" : "avc", recorder->config.width,
                 recorder->config.height, video->frames, video->keyframes, seconds,
                 seconds > 0 ? (video->frames - 1) / seconds : 0.0, video->bytes,
                 seconds > 0 ? video->bytes * 8.0 / seconds : 0.0, recorder->config.bitrate,
                 recorder->tuned ? "true" : "false", recorder->stale_frames,
                 recorder->config.audio_channels > 0 ? "true" : "false",
                 audio_written ? "true" : "false", audio->frames, audio_seconds,
                 atomic_load(&g_audio_pushed), atomic_load(&g_audio_dropped),
                 recorder->audio_skipped, recorder->audio_late,
                 audio_written && video->frames ? (audio->first_us - video->first_us) / 1000.0 : 0.0,
                 audio_written && video->frames ? (audio->last_us - video->last_us) / 1000.0 : 0.0,
                 recorder->clock == RECORDER_CLOCK_BOOTTIME ? "boottime"
                 : recorder->clock == RECORDER_CLOCK_MONOTONIC ? "monotonic" : "unknown");
    }
    return code;
}

static void track_free(struct track *track)
{
    if (track->codec) {
        if (track->started) AMediaCodec_stop(track->codec);
        AMediaCodec_delete(track->codec);
    }
    if (track->format) AMediaFormat_delete(track->format);
    for (int k = 0; k < track->pending_count; ++k) free(track->pending[k].data);
    free(track->pending);
}

void recorder_destroy(struct recorder *recorder)
{
    if (!recorder) return;
    if (recorder->recording) {
        char json[64];
        recorder_finish(recorder, json, sizeof(json));
    }
    atomic_store(&g_audio_accepting, 0);
    if (recorder->muxer) AMediaMuxer_delete(recorder->muxer);
    if (recorder->fd >= 0) close(recorder->fd);
    track_free(&recorder->video);
    track_free(&recorder->audio);
    free(recorder);
}
