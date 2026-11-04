/**
 * @file video_record.c
 * @brief Standalone Camera2 video recording (helper --record-video).
 *
 * Opens a Camera2 device with the record template and the recorder's
 * persistent encoder surface as its only output, and records one or more
 * files back to back in that session (each with a fresh encoder, as the
 * viewfinder session does). Reports what the camera did (frame rate,
 * stabilization mode) and each file's recorder report as JSON.
 */

#include "camera2_bridge.h"
#include "camera2_common.h"
#include "recorder.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>

#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum video_error {
    VIDEO_INVALID_ARGUMENT = -60,
    VIDEO_UNSUPPORTED_SIZE = -61,
    VIDEO_MANAGER_ERROR = -62,
    VIDEO_CHARACTERISTICS_ERROR = -63,
    VIDEO_ENCODER_ERROR = -64,
    VIDEO_OPEN_ERROR = -65,
    VIDEO_CONFIGURATION_ERROR = -66,
    VIDEO_SUBMIT_ERROR = -67,
};

#define VIDEO_PRIVATE_FORMAT 0x22
#define VIDEO_MAX_FILES 4
/* After the last request: wait for its frames, then a little for buffers. */
#define VIDEO_SETTLE_MS 1000
#define VIDEO_BUFFER_GRACE_MS 60

struct video_context {
    struct sfos_camera2_status status;
    atomic_int camera_frames;
    atomic_int stabilization_result;
    atomic_llong first_timestamp_ns;
    atomic_llong last_timestamp_ns;
    atomic_llong exposure_time_ns;
    atomic_int sensitivity;
    /* Last capture sequence whose frames are all done (-1: none). */
    atomic_int ended_sequence;
};

static void video_device_disconnected(void *opaque, ACameraDevice *device)
{
    (void)device;
    struct video_context *context = opaque;
    atomic_store_explicit(&context->status.device_error, -1, memory_order_release);
}

static void video_device_error(void *opaque, ACameraDevice *device, int error)
{
    (void)device;
    struct video_context *context = opaque;
    atomic_store_explicit(&context->status.device_error, error ? error : -2,
                          memory_order_release);
}

static void video_capture_completed(void *opaque, ACameraCaptureSession *session,
                                    ACaptureRequest *request,
                                    const ACameraMetadata *result)
{
    (void)session;
    (void)request;
    struct video_context *context = opaque;
    const int64_t timestamp = sfos_camera2_first_i64(result, ACAMERA_SENSOR_TIMESTAMP, 0);
    long long expected = 0;
    atomic_compare_exchange_strong(&context->first_timestamp_ns, &expected, timestamp);
    atomic_store_explicit(&context->last_timestamp_ns, timestamp, memory_order_release);
    atomic_store_explicit(&context->stabilization_result,
                          sfos_camera2_first_u8(result, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, -1),
                          memory_order_release);
    atomic_store_explicit(&context->exposure_time_ns,
                          sfos_camera2_first_i64(result, ACAMERA_SENSOR_EXPOSURE_TIME, -1),
                          memory_order_release);
    atomic_store_explicit(&context->sensitivity,
                          sfos_camera2_first_i32(result, ACAMERA_SENSOR_SENSITIVITY, -1),
                          memory_order_release);
    atomic_fetch_add_explicit(&context->camera_frames, 1, memory_order_acq_rel);
}

static void video_sequence_ended(void *opaque, ACameraCaptureSession *session,
                                 int sequence_id, int64_t frame_number)
{
    (void)session;
    (void)frame_number;
    struct video_context *context = opaque;
    atomic_store_explicit(&context->ended_sequence, sequence_id, memory_order_release);
}

static void video_sequence_aborted(void *opaque, ACameraCaptureSession *session, int sequence_id)
{
    video_sequence_ended(opaque, session, sequence_id, -1);
}

/* Diagnostics for vendor video modes (MediaTek high-fps / slow motion):
 * int32 vendor settings applied as session parameters and on every
 * request, and a frame rate used even when the standard ranges stop below
 * it. Set before sfos_camera2_record_video(); reset after it. */
#define VIDEO_MAX_VENDOR 8
static int32_t g_vendor_tags[VIDEO_MAX_VENDOR];
static int32_t g_vendor_values[VIDEO_MAX_VENDOR];
static int g_vendor_count;
static int g_force_fps;

SFOS_CAMERA2_EXPORT void sfos_camera2_record_set_vendor(const int32_t *tags,
                                                        const int32_t *values, int count,
                                                        int force_fps)
{
    g_vendor_count = 0;
    for (int i = 0; tags && values && i < count && i < VIDEO_MAX_VENDOR; ++i) {
        g_vendor_tags[i] = tags[i];
        g_vendor_values[i] = values[i];
        ++g_vendor_count;
    }
    g_force_fps = force_fps;
}

int sfos_camera2_vendor_video_settings(const int32_t **tags, const int32_t **values,
                                       bool *force_fps)
{
    *tags = g_vendor_tags;
    *values = g_vendor_values;
    *force_fps = g_force_fps != 0;
    return g_vendor_count;
}

typedef camera_status_t (*create_session_with_parameters_fn)(
    ACameraDevice *, const ACaptureSessionOutputContainer *, const ACaptureRequest *,
    const ACameraCaptureSession_stateCallbacks *, ACameraCaptureSession **);

bool sfos_camera2_video_fps_range(const ACameraMetadata *metadata, int fps, int32_t range[2])
{
    ACameraMetadata_const_entry entry;
    bool found = false;
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry) != ACAMERA_OK) {
        return false;
    }
    /* The fixed [fps, fps] range if listed, else the widest one ending at fps. */
    for (uint32_t index = 0; index + 1 < entry.count; index += 2) {
        const int32_t low = entry.data.i32[index], high = entry.data.i32[index + 1];
        if (high != fps) continue;
        if (low == fps) {
            range[0] = low;
            range[1] = high;
            return true;
        }
        if (!found || low < range[0]) {
            range[0] = low;
            range[1] = high;
            found = true;
        }
    }
    return found;
}

/* "dir/name.mp4" -> "dir/name-2.mp4" for the second file, and so on. */
static void video_numbered_path(const char *path, int index, char *out, size_t size)
{
    if (index == 0) {
        snprintf(out, size, "%s", path);
        return;
    }
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) {
        snprintf(out, size, "%s-%d", path, index + 1);
    } else {
        snprintf(out, size, "%.*s-%d%s", (int)(dot - path), path, index + 1, dot);
    }
}

int sfos_camera2_record_video(const char *camera_id, int width, int height,
                              int fps, int bitrate, int duration_ms,
                              int stabilization, int hevc, int orientation_degrees,
                              int audio_sample_rate, int audio_channels, int repeat,
                              const char *output_path, char *out, size_t out_size)
{
    if (!camera_id || !output_path || !out || out_size == 0 || width <= 0 ||
            height <= 0 || fps <= 0 || bitrate <= 0 || duration_ms <= 0 ||
            audio_channels < 0 || audio_channels > 2 || repeat < 1 ||
            repeat > VIDEO_MAX_FILES || (audio_channels && audio_sample_rate <= 0)) {
        if (out && out_size) {
            snprintf(out, out_size, "{\"status\":\"error\",\"stage\":\"arguments\",\"code\":%d}",
                     VIDEO_INVALID_ARGUMENT);
        }
        return VIDEO_INVALID_ARGUMENT;
    }

    struct video_context context;
    memset(&context, 0, sizeof(context));
    sfos_camera2_status_init(&context.status);
    atomic_store(&context.stabilization_result, -1);
    atomic_store(&context.sensitivity, -1);
    atomic_store(&context.exposure_time_ns, -1);
    atomic_store(&context.ended_sequence, -1);

    const char *stage = "done";
    int result_code = 0;
    int media_status = 0;
    int32_t fps_range[2] = { 0, 0 };
    bool stabilization_supported = false;
    int64_t started_ms = sfos_camera2_now_ms(), session_ms = 0;
    char files[VIDEO_MAX_FILES][2048];
    int files_done = 0;

    ACameraManager *manager = NULL;
    ACameraMetadata *characteristics = NULL;
    ANativeWindow *surface = NULL;
    struct recorder *recorder = NULL;
    ACameraDevice *device = NULL;
    ACameraOutputTarget *target = NULL;
    ACaptureSessionOutput *output = NULL;
    ACaptureSessionOutputContainer *container = NULL;
    ACaptureRequest *request = NULL;
    ACameraCaptureSession *session = NULL;
    const struct recorder_config config = {
        .width = width,
        .height = height,
        .fps = fps,
        .bitrate = bitrate,
        .hevc = hevc,
        .audio_sample_rate = audio_sample_rate,
        .audio_channels = audio_channels,
    };

    manager = ACameraManager_create();
    if (!manager) {
        result_code = VIDEO_MANAGER_ERROR;
        stage = "manager_create";
        goto cleanup;
    }
    camera_status_t camera_status = ACameraManager_getCameraCharacteristics(
        manager, camera_id, &characteristics);
    atomic_store(&context.status.last_camera_status, camera_status);
    if (camera_status != ACAMERA_OK || !characteristics) {
        result_code = VIDEO_CHARACTERISTICS_ERROR;
        stage = "characteristics";
        goto cleanup;
    }
    if (!sfos_camera2_has_output_size(characteristics, VIDEO_PRIVATE_FORMAT, width, height)) {
        result_code = VIDEO_UNSUPPORTED_SIZE;
        stage = "video_size";
        goto cleanup;
    }
    if (!sfos_camera2_video_fps_range(characteristics, fps, fps_range)) {
        if (!g_force_fps) {
            result_code = VIDEO_INVALID_ARGUMENT;
            stage = "fps_range";
            goto cleanup;
        }
        fps_range[0] = fps_range[1] = fps;
    }
    stabilization_supported = sfos_camera2_metadata_has_u8(
        characteristics, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON);

    /* The first encoder is attached before the session is created, so the
     * camera sees the surface's size and format. */
    surface = recorder_surface_create();
    recorder = surface ? recorder_prepare(&config, surface, &stage, &media_status) : NULL;
    if (!recorder) {
        if (!surface) stage = "persistent_surface";
        result_code = VIDEO_ENCODER_ERROR;
        goto cleanup;
    }

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = &context,
        .onDisconnected = video_device_disconnected,
        .onError = video_device_error,
    };
    camera_status = ACameraManager_openCamera(manager, camera_id, &device_callbacks, &device);
    atomic_store(&context.status.last_camera_status, camera_status);
    if (camera_status != ACAMERA_OK || !device) {
        result_code = VIDEO_OPEN_ERROR;
        stage = "camera_open";
        goto cleanup;
    }
    if (ACameraOutputTarget_create(surface, &target) != ACAMERA_OK ||
            ACameraDevice_createCaptureRequest(device, TEMPLATE_RECORD, &request) != ACAMERA_OK ||
            ACaptureRequest_addTarget(request, target) != ACAMERA_OK ||
            ACaptureSessionOutput_create(surface, &output) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_create(&container) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_add(container, output) != ACAMERA_OK) {
        result_code = VIDEO_CONFIGURATION_ERROR;
        stage = "configure";
        goto cleanup;
    }
    ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps_range);
    if (stabilization_supported) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE,
                                    stabilization ? ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON
                                                  : ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    }
    if (sfos_camera2_metadata_has_u8(characteristics, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
                                     ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_MODE,
                                    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO);
    }
    for (int i = 0; i < g_vendor_count; ++i) {
        const camera_status_t set = ACaptureRequest_setEntry_i32(
            request, (uint32_t)g_vendor_tags[i], 1, &g_vendor_values[i]);
        fprintf(stderr, "vendor setting 0x%08x=%d status=%d\n", (uint32_t)g_vendor_tags[i],
                g_vendor_values[i], set);
    }
    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = &context,
        .onClosed = NULL,
        .onReady = NULL,
        .onActive = NULL,
    };
    create_session_with_parameters_fn create_with_parameters =
        (create_session_with_parameters_fn)dlsym(
            RTLD_DEFAULT, "ACameraDevice_createCaptureSessionWithSessionParameters");
    if (g_vendor_count > 0 && create_with_parameters) {
        // The request doubles as the session parameters (Android 9+).
        camera_status = create_with_parameters(device, container, request, &session_callbacks,
                                               &session);
        fprintf(stderr, "session with vendor parameters status=%d\n", camera_status);
    } else {
        camera_status = ACameraDevice_createCaptureSession(device, container, &session_callbacks,
                                                           &session);
    }
    atomic_store(&context.status.last_camera_status, camera_status);
    if (camera_status != ACAMERA_OK || !session) {
        result_code = VIDEO_CONFIGURATION_ERROR;
        stage = "session_create";
        goto cleanup;
    }
    session_ms = sfos_camera2_now_ms() - started_ms;
    ACameraCaptureSession_captureCallbacks capture_callbacks = {
        .context = &context,
        .onCaptureStarted = NULL,
        .onCaptureProgressed = NULL,
        .onCaptureCompleted = video_capture_completed,
        .onCaptureFailed = NULL,
        .onCaptureSequenceCompleted = video_sequence_ended,
        .onCaptureSequenceAborted = video_sequence_aborted,
        .onCaptureBufferLost = NULL,
    };

    for (int file = 0; file < repeat; ++file) {
        char path[2048];
        video_numbered_path(output_path, file, path, sizeof(path));
        if (!recorder) {
            recorder = recorder_prepare(&config, surface, &stage, &media_status);
            if (!recorder) {
                result_code = VIDEO_ENCODER_ERROR;
                goto cleanup;
            }
        }
        int code = recorder_begin(recorder, path, orientation_degrees, &stage);
        if (code < 0) {
            result_code = code;
            goto cleanup;
        }
        int sequence_id = -1;
        camera_status = ACameraCaptureSession_setRepeatingRequest(session, &capture_callbacks, 1,
                                                                  &request, &sequence_id);
        atomic_store(&context.status.last_camera_status, camera_status);
        if (camera_status != ACAMERA_OK) {
            result_code = VIDEO_SUBMIT_ERROR;
            stage = "repeating_request";
            goto cleanup;
        }
        const int64_t deadline = sfos_camera2_now_ms() + duration_ms;
        while (code == 0 && sfos_camera2_now_ms() < deadline &&
                atomic_load(&context.status.device_error) == 0) {
            code = recorder_poll(recorder, &stage);
            sfos_camera2_sleep_10_ms();
        }
        /* Frames still in flight belong to this file; ending the encoder
         * earlier would leave them in the surface for the next one. */
        ACameraCaptureSession_stopRepeating(session);
        const int64_t settle = sfos_camera2_now_ms() + VIDEO_SETTLE_MS;
        int64_t ended_ms = 0;
        while (code == 0 && sfos_camera2_now_ms() < settle) {
            if (!ended_ms && atomic_load(&context.ended_sequence) == sequence_id)
                ended_ms = sfos_camera2_now_ms();
            if (ended_ms && sfos_camera2_now_ms() - ended_ms >= VIDEO_BUFFER_GRACE_MS) break;
            code = recorder_poll(recorder, &stage);
            sfos_camera2_sleep_10_ms();
        }
        const int finished = recorder_finish(recorder, files[files_done], sizeof(files[0]));
        ++files_done;
        recorder_destroy(recorder);
        recorder = NULL;
        if (code < 0 || finished < 0) {
            result_code = code < 0 ? code : finished;
            stage = "recording";
            goto cleanup;
        }
        if (atomic_load(&context.status.device_error) != 0) {
            result_code = VIDEO_OPEN_ERROR;
            stage = "camera_error";
            goto cleanup;
        }
    }

cleanup:
    if (session) {
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
    }
    if (recorder) {
        if (recorder_recording(recorder) && files_done < VIDEO_MAX_FILES) {
            recorder_finish(recorder, files[files_done], sizeof(files[0]));
            ++files_done;
        }
        recorder_destroy(recorder);
    }
    if (target) ACameraOutputTarget_free(target);
    if (output) ACaptureSessionOutput_free(output);
    if (container) ACaptureSessionOutputContainer_free(container);
    if (request) ACaptureRequest_free(request);
    if (device) ACameraDevice_close(device);
    recorder_surface_release(surface);
    if (characteristics) ACameraMetadata_free(characteristics);
    if (manager) ACameraManager_delete(manager);

    const int camera_frames = atomic_load(&context.camera_frames);
    const long long span_ns = atomic_load(&context.last_timestamp_ns)
            - atomic_load(&context.first_timestamp_ns);
    int used = snprintf(out, out_size,
             "{\"status\":\"%s\",\"stage\":\"%s\",\"code\":%d,\"media_status\":%d,"
             "\"fps_range\":[%d,%d],\"stabilization_requested\":%s,"
             "\"stabilization_supported\":%s,\"stabilization_result\":%d,"
             "\"camera_frames\":%d,\"camera_fps\":%.2f,\"last_exposure_ns\":%lld,"
             "\"last_iso\":%d,\"session_ms\":%lld,\"files\":[",
             result_code == 0 ? "ok" : "error", stage, result_code, media_status,
             fps_range[0], fps_range[1], stabilization ? "true" : "false",
             stabilization_supported ? "true" : "false",
             atomic_load(&context.stabilization_result), camera_frames,
             span_ns > 0 && camera_frames > 1 ? (camera_frames - 1) * 1e9 / span_ns : 0.0,
             (long long)atomic_load(&context.exposure_time_ns), atomic_load(&context.sensitivity),
             (long long)session_ms);
    for (int file = 0; file < files_done && used > 0 && (size_t)used < out_size; ++file) {
        used += snprintf(out + used, out_size - (size_t)used, "%s%s", file ? "," : "", files[file]);
    }
    if (used > 0 && (size_t)used < out_size) snprintf(out + used, out_size - (size_t)used, "]}");
    return result_code;
}
