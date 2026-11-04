/**
 * @file preview.c
 * @brief Long-running Camera2 preview helper.
 *
 * Streams preview frames to stdout using ProCam's framing protocol (SF2P
 * RGB888, or SF2Y I420 with --preview-format yuv) and accepts control commands on stdin for focus, capture, and request
 * settings. It also supports warm JPEG and RAW capture from the active session.
 */

#include "camera2_bridge.h"
#include "camera2_common.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include "preview_commands.h"
#include "recorder.h"

enum preview_error {
    PREVIEW_INVALID_ARGUMENT = -30,
    PREVIEW_UNSUPPORTED_SIZE = -31,
    PREVIEW_MANAGER_ERROR = -32,
    PREVIEW_CHARACTERISTICS_ERROR = -33,
    PREVIEW_READER_ERROR = -34,
    PREVIEW_OPEN_ERROR = -35,
    PREVIEW_CONFIGURATION_ERROR = -36,
    PREVIEW_SUBMIT_ERROR = -37,
    PREVIEW_TIMEOUT = -38,
    PREVIEW_WRITE_ERROR = -39,
};

struct preview_rational {
    int32_t numerator;
    int32_t denominator;
};

struct preview_static_raw_metadata {
    int32_t cfa;
    int32_t white_level;
    int64_t exposure_time_range[2];
    int32_t black_level[4];
    int32_t active_array[4];
    float focal_length;
    struct preview_rational color1[9];
    uint32_t color1_count;
};

struct preview_result_raw_metadata {
    char physical_camera_id[128];
    int64_t timestamp_ns;
    int64_t exposure_time_ns;
    int64_t frame_duration_ns;
    int32_t sensitivity;
    int32_t control_mode;
    int32_t scene_mode;
    float color_gains[4];
    struct preview_rational neutral_color_point[3];
    struct preview_rational color_transform[9];
    double noise_profile[8];
    float dynamic_black_level[4];
    uint32_t color_gains_count;
    uint32_t neutral_count;
    uint32_t color_transform_count;
    uint32_t noise_profile_count;
    uint32_t dynamic_black_level_count;
    int32_t af_mode;
    int32_t af_state;
    int32_t lens_state;
    float lens_focus_distance;
};

struct preview_raw_image_metadata {
    int32_t width;
    int32_t height;
    int32_t format;
    int32_t planes;
    int32_t pixel_stride;
    int32_t row_stride;
    int32_t data_length;
    int64_t timestamp_ns;
};

// Results and images can arrive in either order. Pair them by sensor timestamp.
#define METER_SLOTS 32
struct meter_sample {
    int64_t timestamp;
    int32_t iso, ae, ae_mode, compensation, flicker;
    int64_t shutter;
    double luminance, clipped;
    int have_image, have_result;
};

struct preview_context {
    struct meter_sample meter_samples[METER_SLOTS];
    unsigned meter_next;

    struct sfos_camera2_status status;
    atomic_int frames_written;
    atomic_int jpeg_status;
    atomic_int raw_status;
    atomic_int raw_bracket_status[PREVIEW_MAX_RAW_BRACKET];
    atomic_int raw_result_status;
    atomic_int raw_bracket_result_status[PREVIEW_MAX_RAW_BRACKET];
    atomic_int raw_sequence_status;
    int width;
    int height;
    int frame_count;
    int output_fd;
    int control_fd;
    int jpeg_width;
    int jpeg_height;
    int jpeg_quality;
    int jpeg_orientation;
    int jpeg_data_length;
    int raw_width;
    int raw_height;
    // The "raw" stream delivers YUV_420_888 instead of RAW16 (cameras
    // without RAW output); frames are written as packed I420.
    int raw_yuv;
    // Preview frames go out as SF2Y (I420) instead of SF2P (RGB888).
    int preview_yuv;
    // Started early, before the app is ready: frames are dropped until the
    // app sends "resume".
    atomic_int paused;
    // Face detection on preview requests (0 when the camera has none), and
    // the last faces sent, so unchanged results are not repeated.
    uint8_t face_detect_mode;
    char last_faces[320];
    // Fallback when the HAL refuses a second YUV stream: the same processed
    // frames as maximum-quality JPEG (the combination warm JPEG uses).
    int raw_jpeg;
    pthread_mutex_t output_lock;
    float zoom_ratio;
    float focal_length;
    float focus_x;
    float focus_y;
    int focus_mode;
    float focus_distance;
    int focus_hold_active;
    float focus_hold_distance;
    atomic_int live_af_state;
    atomic_int live_focus_distance_micros;
    int exposure_compensation;
    int original_scene_mode;
    int scene_mode;
    int scene_mode_supported;
    int hdr_scene_supported;
    int color_temperature_kelvin;
    int color_tint;
    int32_t sensor_sensitivity;
    int64_t exposure_time_ns;
    atomic_int live_sensor_sensitivity;
    atomic_llong live_exposure_time_ns;
    int aperture;
    int noise_reduction;
    int vendor_awb_value_supported;
    int awb_off_supported;
    char jpeg_path[4096];
    char raw_path[4096];
    char raw_metadata_path[4096];
    char raw_bracket_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    char raw_bracket_metadata_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    int32_t raw_bracket_sensor_sensitivity[PREVIEW_MAX_RAW_BRACKET];
    int64_t raw_bracket_exposure_time_ns[PREVIEW_MAX_RAW_BRACKET];
    int raw_bracket_count;
    atomic_int raw_bracket_generation;
    ACaptureRequest *raw_bracket_requests[PREVIEW_MAX_RAW_BRACKET];
    char camera_id[64];
    int32_t active_array[4];
    int max_ae_regions;
    int max_af_regions;
    int64_t jpeg_command_ms;
    int64_t jpeg_submit_ms;
    int64_t jpeg_result_ms;
    int64_t jpeg_available_ms;
    int64_t jpeg_written_ms;
    int64_t jpeg_hal_exposure_time_ns;
    int64_t jpeg_hal_frame_duration_ns;
    int32_t jpeg_hal_sensitivity;
    int jpeg_effective_logged;
    int32_t jpeg_control_mode;
    int32_t jpeg_scene_mode;
    int64_t raw_command_ms;
    int64_t raw_submit_ms;
    int64_t raw_available_ms;
    int64_t raw_written_ms;
    int64_t raw_metadata_written_ms;
    struct preview_static_raw_metadata raw_static;
    struct preview_result_raw_metadata raw_result;
    struct preview_result_raw_metadata raw_bracket_result[PREVIEW_MAX_RAW_BRACKET];
    struct preview_raw_image_metadata raw_image;
    struct preview_raw_image_metadata raw_bracket_image[PREVIEW_MAX_RAW_BRACKET];
    // The RAW capture generation whose first exposure has been reported.
    atomic_int capture_started_generation;
    // Video mode: requests use the record template and video autofocus.
    int video;
    // preview_ppm entry, for the start-up timing lines.
    int64_t started_ms;
    // Viewfinder latency, sensor timestamp to the frame written to the
    // app (image callback thread only), logged every few seconds.
    int64_t latency_sum_ms;
    int64_t latency_max_ms;
    int latency_frames;
    int64_t latency_logged_ms;
    // Time spent handing frames to the app (pipe writes), the same period.
    int64_t write_sum_ms;
    int64_t write_max_ms;
    // Noise reduction and edge modes of the latest viewfinder result.
    atomic_int live_nr_mode;
    atomic_int live_edge_mode;
    // Sequence id of the latest repeating request, and the newest sequence
    // whose captures have all completed (-1: none yet).
    int repeat_sequence;
    atomic_int ended_sequence;
};

static bool preview_hal_exposure_suspect(int64_t hal_exposure_time_ns,
                                         int64_t elapsed_ms)
{
    if (hal_exposure_time_ns <= 0 || elapsed_ms <= 0) {
        return false;
    }
    return hal_exposure_time_ns > ((elapsed_ms + 100) * 1000000LL);
}

static void preview_log_warm_jpeg_effective(struct preview_context *context)
{
    if (!context || context->jpeg_effective_logged ||
            context->jpeg_submit_ms <= 0 || context->jpeg_result_ms <= 0 ||
            context->jpeg_available_ms <= 0) {
        return;
    }
    context->jpeg_effective_logged = 1;
    int64_t submit_to_image_ms = context->jpeg_available_ms -
                                 context->jpeg_submit_ms;
    int64_t submit_to_result_ms = context->jpeg_result_ms -
                                  context->jpeg_submit_ms;
    int64_t command_to_image_ms = context->jpeg_available_ms -
                                  context->jpeg_command_ms;
    fprintf(stderr,
            "capture-effective warm-jpeg requested_iso=%d "
            "requested_shutter=%lld hal_iso=%d hal_shutter=%lld "
            "hal_frame=%lld submit_to_result_ms=%lld "
            "submit_to_image_ms=%lld command_to_image_ms=%lld "
            "suspect_hal_exposure=%d\n",
            context->sensor_sensitivity,
            (long long)context->exposure_time_ns,
            context->jpeg_hal_sensitivity,
            (long long)context->jpeg_hal_exposure_time_ns,
            (long long)context->jpeg_hal_frame_duration_ns,
            (long long)submit_to_result_ms,
            (long long)submit_to_image_ms,
            (long long)command_to_image_ms,
            preview_hal_exposure_suspect(context->jpeg_hal_exposure_time_ns,
                                         submit_to_image_ms) ? 1 : 0);
}

static void preview_log_warm_raw_effective(struct preview_context *context)
{
    if (!context || context->raw_available_ms <= 0 || context->raw_submit_ms <= 0) {
        return;
    }
    int64_t submit_to_image_ms = context->raw_available_ms -
                                 context->raw_submit_ms;
    int64_t command_to_image_ms = context->raw_available_ms -
                                  context->raw_command_ms;
    fprintf(stderr,
            "capture-effective warm-raw requested_iso=%d "
            "requested_shutter=%lld hal_iso=%d hal_shutter=%lld "
            "hal_frame=%lld submit_to_image_ms=%lld "
            "command_to_image_ms=%lld suspect_hal_exposure=%d\n",
            context->sensor_sensitivity,
            (long long)context->exposure_time_ns,
            context->raw_result.sensitivity,
            (long long)context->raw_result.exposure_time_ns,
            (long long)context->raw_result.frame_duration_ns,
            (long long)submit_to_image_ms,
            (long long)command_to_image_ms,
            preview_hal_exposure_suspect(context->raw_result.exposure_time_ns,
                                         submit_to_image_ms) ? 1 : 0);
}

static bool preview_parse_scene_mode(const char *text, int *mode)
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
            return true;
        }
    }
    return false;
}

static const char *preview_scene_mode_name(int scene_mode)
{
    switch (scene_mode) {
    case SFOS_CAMERA2_SCENE_NONE: return "manual";
    case SFOS_CAMERA2_SCENE_CLOSEUP: return "closeup";
    case SFOS_CAMERA2_SCENE_PORTRAIT: return "portrait";
    case SFOS_CAMERA2_SCENE_LANDSCAPE: return "landscape";
    case SFOS_CAMERA2_SCENE_SPORT: return "sport";
    case SFOS_CAMERA2_SCENE_NIGHT: return "night";
    case SFOS_CAMERA2_SCENE_AUTO: return "auto";
    case SFOS_CAMERA2_SCENE_ACTION: return "action";
    case SFOS_CAMERA2_SCENE_NIGHT_PORTRAIT: return "night-portrait";
    case SFOS_CAMERA2_SCENE_THEATRE: return "theatre";
    case SFOS_CAMERA2_SCENE_BEACH: return "beach";
    case SFOS_CAMERA2_SCENE_SNOW: return "snow";
    case SFOS_CAMERA2_SCENE_SUNSET: return "sunset";
    case SFOS_CAMERA2_SCENE_STEADY_PHOTO: return "steady-photo";
    case SFOS_CAMERA2_SCENE_FIREWORKS: return "fireworks";
    case SFOS_CAMERA2_SCENE_PARTY: return "party";
    case SFOS_CAMERA2_SCENE_CANDLELIGHT: return "candlelight";
    case SFOS_CAMERA2_SCENE_BARCODE: return "barcode";
    case SFOS_CAMERA2_SCENE_BACKLIGHT: return "backlight";
    case SFOS_CAMERA2_SCENE_FLOWERS: return "flowers";
    case SFOS_CAMERA2_SCENE_AR: return "ar";
    case SFOS_CAMERA2_SCENE_HDR: return "hdr";
    default: return "unknown";
    }
}

static const char *preview_control_mode_name(int control_mode)
{
    switch (control_mode) {
    case ACAMERA_CONTROL_MODE_OFF: return "off";
    case ACAMERA_CONTROL_MODE_AUTO: return "auto";
    case ACAMERA_CONTROL_MODE_USE_SCENE_MODE: return "use-scene-mode";
#ifdef ACAMERA_CONTROL_MODE_OFF_KEEP_STATE
    case ACAMERA_CONTROL_MODE_OFF_KEEP_STATE: return "off-keep-state";
#endif
    default: return "unknown";
    }
}

static const char *preview_android_scene_mode_name(int scene_mode)
{
    switch (scene_mode) {
    case ACAMERA_CONTROL_SCENE_MODE_ACTION: return "action";
    case ACAMERA_CONTROL_SCENE_MODE_PORTRAIT: return "portrait";
    case ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE: return "landscape";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT: return "night";
    case ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT: return "night-portrait";
    case ACAMERA_CONTROL_SCENE_MODE_THEATRE: return "theatre";
    case ACAMERA_CONTROL_SCENE_MODE_BEACH: return "beach";
    case ACAMERA_CONTROL_SCENE_MODE_SNOW: return "snow";
    case ACAMERA_CONTROL_SCENE_MODE_SUNSET: return "sunset";
    case ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO: return "steady-photo";
    case ACAMERA_CONTROL_SCENE_MODE_FIREWORKS: return "fireworks";
    case ACAMERA_CONTROL_SCENE_MODE_SPORTS: return "sport";
    case ACAMERA_CONTROL_SCENE_MODE_PARTY: return "party";
    case ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT: return "candlelight";
    case ACAMERA_CONTROL_SCENE_MODE_BARCODE: return "barcode";
    case ACAMERA_CONTROL_SCENE_MODE_HDR: return "hdr";
    default: return "unknown";
    }
}

static uint32_t preview_copy_rational(const ACameraMetadata *metadata,
                                      uint32_t tag,
                                      struct preview_rational *destination,
                                      uint32_t wanted)
{
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK) {
        return 0;
    }
    uint32_t count = entry.count < wanted ? entry.count : wanted;
    for (uint32_t index = 0; index < count; ++index) {
        destination[index].numerator = entry.data.r[index].numerator;
        destination[index].denominator = entry.data.r[index].denominator;
    }
    return count;
}

static void preview_copy_raw_static_metadata(
    struct preview_static_raw_metadata *destination,
    const ACameraMetadata *metadata)
{
    memset(destination, 0, sizeof(*destination));
    destination->cfa = sfos_camera2_first_u8(
        metadata, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, -1);
    destination->white_level = sfos_camera2_first_i32(
        metadata, ACAMERA_SENSOR_INFO_WHITE_LEVEL, -1);
    sfos_camera2_copy_i64_array(metadata, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                                destination->exposure_time_range, 2);
    destination->focal_length = sfos_camera2_first_float(
        metadata, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, 0.0f);
    sfos_camera2_copy_i32_array(metadata, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN,
                                destination->black_level, 4);
    sfos_camera2_copy_i32_array(metadata, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                                destination->active_array, 4);
    destination->color1_count = preview_copy_rational(
        metadata, ACAMERA_SENSOR_COLOR_TRANSFORM1, destination->color1, 9);
}

static void preview_read_metering_info(const ACameraMetadata *metadata,
                                       struct preview_context *context)
{
    ACameraMetadata_const_entry entry;
    context->active_array[0] = 0;
    context->active_array[1] = 0;
    context->active_array[2] = context->width;
    context->active_array[3] = context->height;
    context->max_ae_regions = 0;
    context->max_af_regions = 0;

    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry) ==
            ACAMERA_OK && entry.count >= 4) {
        context->active_array[0] = entry.data.i32[0];
        context->active_array[1] = entry.data.i32[1];
        context->active_array[2] = entry.data.i32[2];
        context->active_array[3] = entry.data.i32[3];
    }
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_MAX_REGIONS, &entry) == ACAMERA_OK &&
            entry.count >= 3) {
        context->max_ae_regions = entry.data.i32[0];
        context->max_af_regions = entry.data.i32[2];
    }
}

static uint8_t preview_af_mode(const struct preview_context *context,
                               int trigger)
{
    switch (context->focus_mode) {
    case SFOS_CAMERA2_FOCUS_CONTINUOUS:
        return trigger || context->focus_x >= 0.0f
                ? ACAMERA_CONTROL_AF_MODE_AUTO
                : context->video ? ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO
                : ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    case SFOS_CAMERA2_FOCUS_MANUAL:
    case SFOS_CAMERA2_FOCUS_INFINITY:
    case SFOS_CAMERA2_FOCUS_NONE:
        return ACAMERA_CONTROL_AF_MODE_OFF;
    case SFOS_CAMERA2_FOCUS_AUTO:
    default:
        return ACAMERA_CONTROL_AF_MODE_AUTO;
    }
}

static bool preview_focus_regions_enabled(const struct preview_context *context)
{
    return context->focus_mode == SFOS_CAMERA2_FOCUS_AUTO ||
           context->focus_mode == SFOS_CAMERA2_FOCUS_CONTINUOUS;
}

static void preview_configure_focus_request(ACaptureRequest *request,
                                            const struct preview_context *context,
                                            float focus_x, float focus_y,
                                            int trigger)
{
    if (!request || !context || !preview_focus_regions_enabled(context) ||
            focus_x < 0.0f || focus_y < 0.0f) {
        return;
    }

    uint8_t af_mode = preview_af_mode(context, trigger);
    uint8_t af_trigger = trigger ? ACAMERA_CONTROL_AF_TRIGGER_START
                                 : ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_MODE, af_mode);
    sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_TRIGGER,
                                af_trigger);

    int32_t left = context->active_array[0];
    int32_t top = context->active_array[1];
    int32_t right = context->active_array[2];
    int32_t bottom = context->active_array[3];
    int32_t width = right - left;
    int32_t height = bottom - top;
    if (width <= 0 || height <= 0) {
        return;
    }
    int32_t box = width < height ? width / 8 : height / 8;
    if (box < 64) {
        box = 64;
    }
    int32_t cx = left + (int32_t)(focus_x * width);
    int32_t cy = top + (int32_t)(focus_y * height);
    int32_t region[5] = {
        sfos_camera2_clamp_i32(cx - box / 2, left, right - 1),
        sfos_camera2_clamp_i32(cy - box / 2, top, bottom - 1),
        sfos_camera2_clamp_i32(cx + box / 2, left + 1, right),
        sfos_camera2_clamp_i32(cy + box / 2, top + 1, bottom),
        1000,
    };
    if (context->max_af_regions > 0) {
        ACaptureRequest_setEntry_i32(
            request, ACAMERA_CONTROL_AF_REGIONS, 5, region);
    }
    if (context->max_ae_regions > 0) {
        ACaptureRequest_setEntry_i32(
            request, ACAMERA_CONTROL_AE_REGIONS, 5, region);
    }
}

/**
 * Applies controls that must track live preview state.
 */
static void preview_configure_request(ACaptureRequest *request,
                                      const struct preview_context *context,
                                      const ACameraMetadata *metadata,
                                      int still)
{
    if (!request || !context) {
        return;
    }

    if (context->focus_hold_active) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_MODE,
                                    ACAMERA_CONTROL_AF_MODE_OFF);
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_TRIGGER,
                                    ACAMERA_CONTROL_AF_TRIGGER_IDLE);
        sfos_camera2_set_request_float(request, ACAMERA_LENS_FOCUS_DISTANCE,
                                       context->focus_hold_distance);
    } else {
        uint8_t af_mode = preview_af_mode(context, 0);
        if (metadata && !sfos_camera2_metadata_has_u8(
                metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES, af_mode)) {
            af_mode = ACAMERA_CONTROL_AF_MODE_OFF;
        }
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_MODE, af_mode);
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_TRIGGER,
                                    ACAMERA_CONTROL_AF_TRIGGER_IDLE);
    }
    int32_t ev_range[2] = {0, 0};
    if (metadata) sfos_camera2_copy_i32_array(metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, ev_range, 2);
    sfos_camera2_set_request_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
        sfos_camera2_clamp_i32(context->exposure_compensation, ev_range[0], ev_range[1]));
    sfos_camera2_set_scene_mode(request, context->scene_mode);
    sfos_camera2_set_manual_sensor(metadata, request,
                                   context->sensor_sensitivity,
                                   context->exposure_time_ns);
    if (context->sensor_sensitivity == 0 && context->exposure_time_ns == 0) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_CAPTURE_INTENT,
            still ? ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE
            : context->video ? ACAMERA_CONTROL_CAPTURE_INTENT_VIDEO_RECORD
            : ACAMERA_CONTROL_CAPTURE_INTENT_PREVIEW);
    }
    sfos_camera2_set_aperture(metadata, request, context->aperture);
    // Video keeps the record template's noise reduction: the camera's own
    // video tuning (the bridge default, 0, would switch it off).
    if (!context->video)
        sfos_camera2_set_noise_reduction(metadata, request,
                                         context->noise_reduction);

    if (!context->focus_hold_active &&
            (context->focus_mode == SFOS_CAMERA2_FOCUS_MANUAL ||
            context->focus_mode == SFOS_CAMERA2_FOCUS_INFINITY)) {
        float distance = context->focus_mode == SFOS_CAMERA2_FOCUS_INFINITY
                ? 0.0f : context->focus_distance;
        sfos_camera2_set_request_float(request, ACAMERA_LENS_FOCUS_DISTANCE,
                                       distance);
    }

    if (!context->focus_hold_active &&
            preview_focus_regions_enabled(context) &&
            context->focus_x >= 0.0f && context->focus_y >= 0.0f) {
        preview_configure_focus_request(request, context, context->focus_x,
                                        context->focus_y, 0);
    }
    if (context->focus_hold_active) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_MODE,
                                    ACAMERA_CONTROL_AF_MODE_OFF);
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AF_TRIGGER,
                                    ACAMERA_CONTROL_AF_TRIGGER_IDLE);
        sfos_camera2_set_request_float(request, ACAMERA_LENS_FOCUS_DISTANCE,
                                       context->focus_hold_distance);
    }
    if (context->color_temperature_kelvin > 0) {
#ifdef ACAMERA_COLOR_CORRECTION_COLOR_TEMPERATURE
        sfos_camera2_set_request_i32(
            request, ACAMERA_COLOR_CORRECTION_COLOR_TEMPERATURE,
            context->color_temperature_kelvin);
#endif
        if (context->vendor_awb_value_supported &&
                context->awb_off_supported) {
            int32_t awb_value = sfos_camera2_clamp_i32(
                context->color_temperature_kelvin, 2000, 9000);
            sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AWB_MODE,
                                        ACAMERA_CONTROL_AWB_MODE_OFF);
            sfos_camera2_set_request_i32(request, MTK_3A_AWB_VALUE,
                                         awb_value);
        }
    } else {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_AWB_MODE,
                                    ACAMERA_CONTROL_AWB_MODE_AUTO);
    }
    if (context->color_tint != 0) {
#ifdef ACAMERA_COLOR_CORRECTION_COLOR_TINT
        sfos_camera2_set_request_i32(request, ACAMERA_COLOR_CORRECTION_COLOR_TINT,
                                     context->color_tint);
#endif
    } else {
#ifdef ACAMERA_COLOR_CORRECTION_COLOR_TINT
        sfos_camera2_set_request_i32(request, ACAMERA_COLOR_CORRECTION_COLOR_TINT,
                                     0);
#endif
    }
    if (still) {
        sfos_camera2_set_request_u8(request, ACAMERA_JPEG_QUALITY,
                                    (uint8_t)context->jpeg_quality);
        sfos_camera2_set_request_i32(request, ACAMERA_JPEG_ORIENTATION,
                                     context->jpeg_orientation);
    }
    sfos_camera2_set_zoom_ratio(metadata, request, context->zoom_ratio);
    // Faces for the app's exposure (preview only; captures need none).
    if (!still && context->face_detect_mode)
        sfos_camera2_set_request_u8(request, ACAMERA_STATISTICS_FACE_DETECT_MODE,
                                    context->face_detect_mode);
}

// Locks focus before a warm capture in continuous AF (Android's AF
// trigger): a passive scan could otherwise move the lens during a burst,
// whose darker exposure changes the contrast continuous AF watches. Returns
// true when a lock was requested; preview_unlock_focus() resumes scanning.
static bool preview_lock_focus(struct preview_context *context,
                               ACameraCaptureSession *session,
                               ACaptureRequest *preview_request,
                               const ACameraMetadata *metadata,
                               ACameraCaptureSession_captureCallbacks *callbacks)
{
    if (context->focus_hold_active ||
            context->focus_mode != SFOS_CAMERA2_FOCUS_CONTINUOUS ||
            context->focus_x >= 0.0f || // tap focus: the AUTO scan already holds
            !sfos_camera2_metadata_has_u8(metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
                                          ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE)) {
        return false;
    }
    const int64_t started = sfos_camera2_now_ms();
    atomic_store_explicit(&context->live_af_state, -1, memory_order_release);
    sfos_camera2_set_request_u8(preview_request, ACAMERA_CONTROL_AF_TRIGGER,
                                ACAMERA_CONTROL_AF_TRIGGER_START);
    ACaptureRequest *requests[] = { preview_request };
    camera_status_t status = ACameraCaptureSession_capture(session, callbacks, 1,
                                                           requests, NULL);
    sfos_camera2_set_request_u8(preview_request, ACAMERA_CONTROL_AF_TRIGGER,
                                ACAMERA_CONTROL_AF_TRIGGER_IDLE);
    if (status != ACAMERA_OK) {
        fprintf(stderr, "capture-focus lock not submitted (status %d)\n", status);
        return false;
    }
    int state = -1;
    while (sfos_camera2_now_ms() - started < 1000) {
        state = atomic_load_explicit(&context->live_af_state, memory_order_acquire);
        if (state == ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED ||
                state == ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED) {
            break;
        }
        sfos_camera2_sleep_10_ms();
    }
    fprintf(stderr, "capture-focus lock af_state=%d waited_ms=%lld distance=%.4f\n",
            state, (long long)(sfos_camera2_now_ms() - started),
            atomic_load_explicit(&context->live_focus_distance_micros,
                                 memory_order_acquire) / 1000000.0);
    return true;
}

// Queued after the capture requests, so the lock lasts for all their frames.
static void preview_unlock_focus(ACameraCaptureSession *session,
                                 ACaptureRequest *preview_request,
                                 ACameraCaptureSession_captureCallbacks *callbacks)
{
    sfos_camera2_set_request_u8(preview_request, ACAMERA_CONTROL_AF_TRIGGER,
                                ACAMERA_CONTROL_AF_TRIGGER_CANCEL);
    ACaptureRequest *requests[] = { preview_request };
    ACameraCaptureSession_capture(session, callbacks, 1, requests, NULL);
    sfos_camera2_set_request_u8(preview_request, ACAMERA_CONTROL_AF_TRIGGER,
                                ACAMERA_CONTROL_AF_TRIGGER_IDLE);
}

static void preview_configure_raw_bracket_request(
    ACaptureRequest *request, const struct preview_context *context,
    const ACameraMetadata *metadata, int32_t sensor_sensitivity,
    int64_t exposure_time_ns)
{
    struct preview_context frame_context = *context;
    frame_context.scene_mode = SFOS_CAMERA2_SCENE_MANUAL; // AE_OFF pair, no vendor HDR scene processing.
    frame_context.color_temperature_kelvin = 0;
    frame_context.color_tint = 0;
    frame_context.exposure_compensation = 0;
    frame_context.sensor_sensitivity = sensor_sensitivity;
    frame_context.exposure_time_ns = exposure_time_ns;
    // YUV bursts are merged and denoised by ProCam: the ISP's noise
    // reduction and sharpening would only smear detail first.
    // Full field of view too: ProCam crops YUV bursts for zoom itself (as
    // for RAW, which the HAL never crops), so a HAL that also applies the
    // zoom ratio to processed streams cannot zoom twice.
    if (context->raw_yuv) {
        frame_context.noise_reduction = 0;
        frame_context.zoom_ratio = 1.0f;
    }
    if (context->raw_jpeg) {
        frame_context.jpeg_quality = 100;
    }
    preview_configure_request(request, &frame_context, metadata, 1);
    if (context->raw_yuv &&
            sfos_camera2_metadata_has_u8(metadata, ACAMERA_EDGE_AVAILABLE_EDGE_MODES,
                                         ACAMERA_EDGE_MODE_OFF)) {
        sfos_camera2_set_request_u8(request, ACAMERA_EDGE_MODE, ACAMERA_EDGE_MODE_OFF);
    }
}

static bool preview_parse_command_line(const char *line,
                                       struct preview_command *command)
{
    if (!line || !command) {
        return false;
    }
    command->type = PREVIEW_COMMAND_NONE;

    float x = -1.0f;
    float y = -1.0f;
    if (!strcmp(line, "focus-reset")) {
        command->type = PREVIEW_COMMAND_FOCUS;
        command->focus_x = -1.0f;
        command->focus_y = -1.0f;
        return true;
    }
    if (!strcmp(line, "focus-hold")) {
        command->type = PREVIEW_COMMAND_FOCUS_HOLD;
        return true;
    }
    if (!strcmp(line, "focus-hold-release")) {
        command->type = PREVIEW_COMMAND_FOCUS_HOLD_RELEASE;
        return true;
    }
    if (sscanf(line, "focus %f %f", &x, &y) == 2 &&
            x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f) {
        command->type = PREVIEW_COMMAND_FOCUS;
        command->focus_x = x;
        command->focus_y = y;
        return true;
    }

    float zoom_ratio = 1.0f;
    if (sscanf(line, "zoom %f", &zoom_ratio) == 1 &&
            zoom_ratio >= 1.0f && zoom_ratio <= 100.0f) {
        command->type = PREVIEW_COMMAND_ZOOM;
        command->zoom_ratio = zoom_ratio;
        return true;
    }

    char focus_mode[32];
    float focus_distance = 0.0f;
    int exposure_compensation = 0;
    char scene_mode[32];
    int color_temperature_kelvin = 0;
    int color_tint = 0;
    int sensor_sensitivity = 0;
    long long exposure_time_ns = 0;
    int aperture = 0;
    int noise_reduction = 0;
    if (sscanf(line, "settings %31s %f %d %31s %d %d %d %lld %d %d %f",
               focus_mode, &focus_distance, &exposure_compensation, scene_mode,
               &color_temperature_kelvin, &color_tint, &sensor_sensitivity,
               &exposure_time_ns, &aperture, &noise_reduction,
               &zoom_ratio) == 11 &&
            focus_distance >= 0.0f &&
            color_temperature_kelvin >= 0 &&
            sensor_sensitivity >= 0 &&
            exposure_time_ns >= 0 &&
            aperture >= 0 && aperture <= 255 &&
            noise_reduction >= 0 &&
            zoom_ratio >= 1.0f && zoom_ratio <= 100.0f) {
        if (!strcmp(focus_mode, "none")) {
            command->focus_mode = SFOS_CAMERA2_FOCUS_NONE;
        } else if (!strcmp(focus_mode, "auto")) {
            command->focus_mode = SFOS_CAMERA2_FOCUS_AUTO;
        } else if (!strcmp(focus_mode, "continuous")) {
            command->focus_mode = SFOS_CAMERA2_FOCUS_CONTINUOUS;
        } else if (!strcmp(focus_mode, "manual")) {
            command->focus_mode = SFOS_CAMERA2_FOCUS_MANUAL;
        } else if (!strcmp(focus_mode, "infinity")) {
            command->focus_mode = SFOS_CAMERA2_FOCUS_INFINITY;
        } else {
            return false;
        }
        if (!preview_parse_scene_mode(scene_mode, &command->scene_mode)) {
            return false;
        }
        command->type = PREVIEW_COMMAND_SETTINGS;
        command->focus_distance = focus_distance;
        command->exposure_compensation = exposure_compensation;
        command->color_temperature_kelvin = color_temperature_kelvin;
        command->color_tint = color_tint;
        command->sensor_sensitivity = sensor_sensitivity;
        command->exposure_time_ns = exposure_time_ns;
        command->aperture = aperture;
        command->noise_reduction = noise_reduction;
        command->zoom_ratio = zoom_ratio;
        return true;
    }

    if (sscanf(line, "exposure-settings %d %lld %d %d %f",
               &sensor_sensitivity, &exposure_time_ns, &aperture,
               &noise_reduction, &zoom_ratio) == 5 &&
            sensor_sensitivity >= 0 &&
            exposure_time_ns >= 0 &&
            aperture >= 0 && aperture <= 255 &&
            noise_reduction >= 0 &&
            zoom_ratio >= 1.0f && zoom_ratio <= 100.0f) {
        command->type = PREVIEW_COMMAND_EXPOSURE_SETTINGS;
        command->sensor_sensitivity = sensor_sensitivity;
        command->exposure_time_ns = exposure_time_ns;
        command->aperture = aperture;
        command->noise_reduction = noise_reduction;
        command->zoom_ratio = zoom_ratio;
        return true;
    }

    if (preview_parse_record_command(line, command)) {
        return true;
    }
    return preview_parse_capture_command(line, command);
}

static void preview_read_command_data(int fd,
                                      struct preview_command_buffer *buffer)
{
    if (fd < 0 || !buffer) {
        return;
    }

    // Drain the rest of an oversized line before accepting another command.
    // Otherwise its tail could be mistaken for a fresh capture request.
    while (buffer->discarding) {
        char chunk[4096];
        ssize_t bytes = read(fd, chunk, sizeof(chunk));
        if (bytes <= 0) return;
        char *newline = memchr(chunk, '\n', (size_t)bytes);
        if (newline) {
            buffer->length = (size_t)(chunk + bytes - newline - 1);
            memcpy(buffer->data, newline + 1, buffer->length);
            buffer->data[buffer->length] = '\0';
            buffer->discarding = false;
        }
    }
    while (buffer->length < sizeof(buffer->data) - 1) {
        ssize_t bytes = read(fd, buffer->data + buffer->length,
                             sizeof(buffer->data) - buffer->length - 1);
        if (bytes > 0) {
            buffer->length += (size_t)bytes;
            buffer->data[buffer->length] = '\0';
            continue;
        }
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        return;
    }

    buffer->length = 0;
    buffer->data[0] = '\0';
}

static bool preview_pop_command(struct preview_command_buffer *buffer,
                                struct preview_command *command)
{
    if (!buffer || !command) {
        return false;
    }

    for (;;) {
        char *newline = memchr(buffer->data, '\n', buffer->length);
        if (!newline) {
            if (buffer->length == sizeof(buffer->data) - 1) {
                buffer->length = 0;
                buffer->data[0] = '\0';
                buffer->discarding = true;
                command->type = PREVIEW_COMMAND_CAPTURE_INVALID;
                return true;
            }
            return false;
        }

        size_t line_length = (size_t)(newline - buffer->data);
        char line[sizeof(buffer->data)];
        memcpy(line, buffer->data, line_length);
        line[line_length] = '\0';

        size_t consumed = (size_t)(newline - buffer->data) + 1;
        memmove(buffer->data, buffer->data + consumed,
                buffer->length - consumed);
        buffer->length -= consumed;
        buffer->data[buffer->length] = '\0';

        if (preview_parse_command_line(line, command)) {
            return true;
        }
    }
}

static unsigned char preview_clip(int value)
{
    if (value < 0) {
        return 0;
    }
    return (unsigned char)(value > 255 ? 255 : value);
}

static void preview_write_le32(unsigned char *target, uint32_t value)
{
    target[0] = (unsigned char)(value & 0xff);
    target[1] = (unsigned char)((value >> 8) & 0xff);
    target[2] = (unsigned char)((value >> 16) & 0xff);
    target[3] = (unsigned char)((value >> 24) & 0xff);
}

/**
 * @brief Samples one YUV420 chroma plane with a small bilinear filter.
 *
 * Camera2 YUV420 chroma planes are half-resolution. Using a single nearest
 * chroma sample for every 2x2 luma block makes colored edges shimmer when the
 * app scales the preview again, so odd luma rows/columns blend toward the next
 * chroma sample while edges clamp to the valid plane area.
 */
static int preview_sample_chroma(const uint8_t *plane, int length,
                                 int row_stride, int pixel_stride,
                                 int chroma_width, int chroma_height,
                                 int x, int y, int *value)
{
    if (!plane || !value || length <= 0 || row_stride <= 0 ||
            pixel_stride <= 0 || chroma_width <= 0 || chroma_height <= 0) {
        return -1;
    }

    int x0 = x / 2;
    int y0 = y / 2;
    int x1 = x0 + 1 < chroma_width ? x0 + 1 : x0;
    int y1 = y0 + 1 < chroma_height ? y0 + 1 : y0;
    int fx = x & 1;
    int fy = y & 1;
    int w00 = (2 - fx) * (2 - fy);
    int w10 = fx * (2 - fy);
    int w01 = (2 - fx) * fy;
    int w11 = fx * fy;
    int i00 = y0 * row_stride + x0 * pixel_stride;
    int i10 = y0 * row_stride + x1 * pixel_stride;
    int i01 = y1 * row_stride + x0 * pixel_stride;
    int i11 = y1 * row_stride + x1 * pixel_stride;

    if (i00 < 0 || i10 < 0 || i01 < 0 || i11 < 0 ||
            i00 >= length || i10 >= length ||
            i01 >= length || i11 >= length) {
        return -1;
    }

    *value = (plane[i00] * w00 + plane[i10] * w10 +
              plane[i01] * w01 + plane[i11] * w11 + 2) / 4;
    return 0;
}

/**
 * @brief Converts one Camera2 YUV420 preview image to framed RGB888 output.
 *
 * The Sailfish side consumes the unchanged SF2P protocol: a 16-byte header
 * followed by tightly-packed RGB888 pixels. Only chroma reconstruction happens
 * here; final preview scaling remains in the Qt scene graph.
 */
// Called under output_lock, also used to serialize SF2M and SF2P packets.
static struct meter_sample *meter_slot(struct preview_context *context, int64_t timestamp)
{
    for (unsigned i = 0; i < METER_SLOTS; ++i)
        if (context->meter_samples[i].timestamp == timestamp) return &context->meter_samples[i];
    struct meter_sample *sample = &context->meter_samples[context->meter_next++ % METER_SLOTS];
    memset(sample, 0, sizeof(*sample));
    sample->timestamp = timestamp;
    return sample;
}

static void meter_emit(struct preview_context *context, struct meter_sample *sample)
{
    if (!sample->have_image || !sample->have_result) return;
    char payload[384];
    int size = snprintf(payload, sizeof(payload),
        "meter=1 camera=%s timestamp=%lld iso=%d shutter=%lld ae=%d ae_mode=%d ev_steps=%d flicker=%d luminance=%.9g clipped=%.6f\n",
        context->camera_id, (long long)sample->timestamp, sample->iso, (long long)sample->shutter, sample->ae, sample->ae_mode,
        sample->compensation, sample->flicker, sample->luminance, sample->clipped);
    if (size > 0 && size < (int)sizeof(payload)) {
        unsigned char header[8] = { 'S', 'F', '2', 'M' };
        preview_write_le32(header + 4, (uint32_t)size);
        sfos_camera2_write_all(context->output_fd, header, sizeof(header));
        sfos_camera2_write_all(context->output_fd, payload, (size_t)size);
    }
    sample->have_image = sample->have_result = 0;
}

// Copies one plane into a tightly packed buffer, honouring the row and pixel
// strides (semi-planar chroma has a pixel stride of 2).
static int preview_pack_plane(unsigned char *target, int width, int height,
                              const uint8_t *plane, int length,
                              int row_stride, int pixel_stride)
{
    if (!plane || row_stride <= 0 || pixel_stride <= 0) {
        return -1;
    }
    for (int y = 0; y < height; ++y) {
        const long first = (long)y * row_stride;
        const long last = first + (long)(width - 1) * pixel_stride;
        if (first < 0 || last >= length) {
            return -1;
        }
        unsigned char *row = target + (size_t)y * (size_t)width;
        if (pixel_stride == 1) {
            memcpy(row, plane + first, (size_t)width);
        } else {
            const uint8_t *source = plane + first;
            for (int x = 0; x < width; ++x) {
                row[x] = source[(size_t)x * (size_t)pixel_stride];
            }
        }
    }
    return 0;
}

/**
 * @brief Writes one preview image as an SF2Y packet: a 16-byte header
 * ('SF2Y', width, height, payload bytes) followed by I420 (Y, then U and V
 * at half resolution, each tightly packed). Mostly row copies.
 */
static int preview_write_i420_frame(struct preview_context *context,
                                    int width, int height,
                                    const uint8_t *plane_y, int len_y, int row_y, int pixel_y,
                                    const uint8_t *plane_u, int len_u, int row_u, int pixel_u,
                                    const uint8_t *plane_v, int len_v, int row_v, int pixel_v)
{
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const size_t luma_size = (size_t)width * (size_t)height;
    const size_t chroma_size = (size_t)chroma_width * (size_t)chroma_height;
    const size_t frame_size = luma_size + 2 * chroma_size;
    unsigned char *frame = malloc(frame_size);
    if (!frame) {
        return -1;
    }
    int result = 0;
    if (preview_pack_plane(frame, width, height, plane_y, len_y, row_y, pixel_y) != 0 ||
            preview_pack_plane(frame + luma_size, chroma_width, chroma_height,
                               plane_u, len_u, row_u, pixel_u) != 0 ||
            preview_pack_plane(frame + luma_size + chroma_size, chroma_width, chroma_height,
                               plane_v, len_v, row_v, pixel_v) != 0) {
        result = -1;
    } else {
        unsigned char header[16] = { 'S', 'F', '2', 'Y' };
        preview_write_le32(header + 4, (uint32_t)width);
        preview_write_le32(header + 8, (uint32_t)height);
        preview_write_le32(header + 12, (uint32_t)frame_size);
        pthread_mutex_lock(&context->output_lock);
        if (sfos_camera2_write_all(context->output_fd, header, sizeof(header)) != 0 ||
                sfos_camera2_write_all(context->output_fd, frame, frame_size) != 0) {
            result = -1;
        }
        pthread_mutex_unlock(&context->output_lock);
    }
    free(frame);
    return result;
}

static int preview_write_rgb_frame(struct preview_context *context,
                                   AImage *image)
{
    uint8_t *plane_y = NULL;
    uint8_t *plane_u = NULL;
    uint8_t *plane_v = NULL;
    int len_y = 0;
    int len_u = 0;
    int len_v = 0;
    int row_y = 0;
    int row_u = 0;
    int row_v = 0;
    int pixel_y = 0;
    int pixel_u = 0;
    int pixel_v = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    int planes = 0;

    if (AImage_getWidth(image, &width) != AMEDIA_OK ||
            AImage_getHeight(image, &height) != AMEDIA_OK ||
            AImage_getFormat(image, &format) != AMEDIA_OK ||
            AImage_getNumberOfPlanes(image, &planes) != AMEDIA_OK ||
            width != context->width || height != context->height ||
            format != AIMAGE_FORMAT_YUV_420_888 || planes < 3 ||
            AImage_getPlaneData(image, 0, &plane_y, &len_y) != AMEDIA_OK ||
            AImage_getPlaneData(image, 1, &plane_u, &len_u) != AMEDIA_OK ||
            AImage_getPlaneData(image, 2, &plane_v, &len_v) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, 0, &row_y) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, 1, &row_u) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, 2, &row_v) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, 0, &pixel_y) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, 1, &pixel_u) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, 2, &pixel_v) != AMEDIA_OK ||
            !plane_y || !plane_u || !plane_v || len_y <= 0 ||
            len_u <= 0 || len_v <= 0) {
        return -1;
    }

    // Centre-weighted trimmed luminance, before RGB rendering or display gain.
    // Gamma 2.2 is an estimate for processed YUV, never a sensor-linear claim.
    int64_t timestamp = 0;
    if (AImage_getTimestamp(image, &timestamp) == AMEDIA_OK && timestamp > 0) {
        unsigned bins[256] = {0};
        unsigned total = 0, clipped = 0;
        const int step = width / 96 > 0 ? width / 96 : 1;
        for (int y = 0; y < height; y += step) {
            for (int x = 0; x < width; x += step) {
                int offset = y * row_y + x * pixel_y;
                if (offset < 0 || offset >= len_y) continue;
                unsigned weight = (x > width / 4 && x < width * 3 / 4 &&
                                   y > height / 4 && y < height * 3 / 4) ? 4 : 1;
                unsigned value = plane_y[offset];
                bins[value] += weight;
                total += weight;
                if (value <= 3 || value >= 252) clipped += weight;
            }
        }
        double sum = 0, count = 0;
        unsigned cumulative = 0;
        for (int i = 0; i < 256; ++i) {
            unsigned begin = cumulative;
            cumulative += bins[i];
            unsigned low = begin > total / 10 ? begin : total / 10;
            unsigned high = cumulative < total * 9 / 10 ? cumulative : total * 9 / 10;
            if (high > low) { sum += (high - low) * pow(i / 255.0, 2.2); count += high - low; }
        }
        pthread_mutex_lock(&context->output_lock);
        struct meter_sample *sample = meter_slot(context, timestamp);
        sample->luminance = count > 0 ? sum / count : 0;
        sample->clipped = total ? (double)clipped / total : 1;
        sample->have_image = 1;
        meter_emit(context, sample);
        pthread_mutex_unlock(&context->output_lock);
    }

    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    if (context->preview_yuv) {
        return preview_write_i420_frame(context, width, height,
                                        plane_y, len_y, row_y, pixel_y,
                                        plane_u, len_u, row_u, pixel_u,
                                        plane_v, len_v, row_v, pixel_v);
    }

    const size_t frame_size = (size_t)width * (size_t)height * 3;
    unsigned char *frame = malloc(frame_size);
    if (!frame) {
        return -1;
    }

    int result = 0;
    for (int y = 0; y < height; ++y) {
        unsigned char *row = frame + (size_t)y * (size_t)width * 3;
        for (int x = 0; x < width; ++x) {
            int y_index = y * row_y + x * pixel_y;
            int sampled_u = 0;
            int sampled_v = 0;
            if (y_index < 0 || y_index >= len_y ||
                    preview_sample_chroma(plane_u, len_u, row_u, pixel_u,
                                          chroma_width, chroma_height,
                                          x, y, &sampled_u) != 0 ||
                    preview_sample_chroma(plane_v, len_v, row_v, pixel_v,
                                          chroma_width, chroma_height,
                                          x, y, &sampled_v) != 0) {
                result = -1;
                goto done;
            }
            int yy = plane_y[y_index];
            int uu = sampled_u - 128;
            int vv = sampled_v - 128;
            row[x * 3] = preview_clip(yy + ((91881 * vv) >> 16));
            row[x * 3 + 1] = preview_clip(
                yy - ((22554 * uu + 46802 * vv) >> 16));
            row[x * 3 + 2] = preview_clip(yy + ((116130 * uu) >> 16));
        }
    }

    unsigned char header[16] = { 'S', 'F', '2', 'P' };
    preview_write_le32(header + 4, (uint32_t)width);
    preview_write_le32(header + 8, (uint32_t)height);
    preview_write_le32(header + 12, (uint32_t)frame_size);
    pthread_mutex_lock(&context->output_lock);
    if (sfos_camera2_write_all(context->output_fd, header,
                               sizeof(header)) != 0 ||
            sfos_camera2_write_all(context->output_fd, frame,
                                   frame_size) != 0) {
        result = -1;
    }
    pthread_mutex_unlock(&context->output_lock);
done:
    free(frame);
    return result;
}

// Tells the app which capture streams the session really has, so it never
// sends warm captures to a stream the HAL dropped.
// raw_error says why a requested RAW/YUV capture stream is missing
// ("reader:<status>", "session:<status>", "none" when it is up).
static void preview_write_stream_status(struct preview_context *context,
                                        bool raw, bool jpeg,
                                        const char *raw_error,
                                        int raw_width, int raw_height)
{
    char payload[192];
    int payload_size = snprintf(payload, sizeof(payload),
                                "streams=1 raw=%d jpeg=%d raw_error=%s raw_size=%dx%d raw_format=%s\n",
                                raw ? 1 : 0, jpeg ? 1 : 0,
                                raw_error && *raw_error ? raw_error : "none",
                                raw_width, raw_height,
                                context->raw_jpeg ? "jpeg" : context->raw_yuv ? "yuv" : "raw16");
    if (payload_size <= 0 || payload_size >= (int)sizeof(payload)) {
        return;
    }
    unsigned char header[8] = { 'S', 'F', '2', 'M' };
    preview_write_le32(header + 4, (uint32_t)payload_size);
    pthread_mutex_lock(&context->output_lock);
    sfos_camera2_write_all(context->output_fd, header, sizeof(header));
    sfos_camera2_write_all(context->output_fd, payload, (size_t)payload_size);
    pthread_mutex_unlock(&context->output_lock);
}

static void preview_write_metadata(struct preview_context *context,
                                   int32_t sensor_sensitivity,
                                   int64_t exposure_time_ns,
                                   int32_t af_state,
                                   float focus_distance)
{
    if (!context || context->output_fd < 0) {
        return;
    }

    int32_t previous_sensitivity = atomic_exchange_explicit(
        &context->live_sensor_sensitivity, sensor_sensitivity,
        memory_order_acq_rel);
    long long previous_exposure = atomic_exchange_explicit(
        &context->live_exposure_time_ns, (long long)exposure_time_ns,
        memory_order_acq_rel);
    int previous_af_state = atomic_exchange_explicit(
        &context->live_af_state, af_state, memory_order_acq_rel);
    int focus_distance_micros = focus_distance >= 0.0f
            ? (int)(focus_distance * 1000000.0f) : -1;
    int previous_focus_distance_micros = atomic_exchange_explicit(
        &context->live_focus_distance_micros, focus_distance_micros,
        memory_order_acq_rel);
    if (previous_sensitivity == sensor_sensitivity &&
            previous_exposure == (long long)exposure_time_ns &&
            previous_af_state == af_state &&
            previous_focus_distance_micros == focus_distance_micros) {
        return;
    }

    char payload[192];
    int payload_size = snprintf(payload, sizeof(payload),
                                "focal=%.3f iso=%d shutter=%lld "
                                "af_state=%d focus_distance=%.6f\n",
                                context->focal_length, sensor_sensitivity,
                                (long long)exposure_time_ns,
                                af_state, focus_distance);
    if (payload_size <= 0 || payload_size >= (int)sizeof(payload)) {
        return;
    }

    unsigned char header[8] = { 'S', 'F', '2', 'M' };
    preview_write_le32(header + 4, (uint32_t)payload_size);

    pthread_mutex_lock(&context->output_lock);
    sfos_camera2_write_all(context->output_fd, header, sizeof(header));
    sfos_camera2_write_all(context->output_fd, payload, (size_t)payload_size);
    pthread_mutex_unlock(&context->output_lock);
}

static void preview_write_capture_result(struct preview_context *context,
                                         const char *status,
                                         const char *path,
                                         int code)
{
    if (!context || context->output_fd < 0 || !status) {
        return;
    }

    char payload[8192];
    int payload_size = snprintf(
        payload, sizeof(payload),
        "capture-status=%s path=%s code=%d width=%d height=%d bytes=%d "
        "submit_ms=%lld available_ms=%lld written_ms=%lld "
        "scene-requested-original=%s scene-requested=%s scene-supported=%s "
        "hdr-scene-supported=%s hdr-scene-requested=%s hdr-scene-applied=%s "
        "dol-supported=false dol-source=%s dol-requested=%s dol-applied=false "
        "control-mode-applied=%s control-mode-applied-value=%d "
        "scene-mode-applied=%s scene-mode-applied-value=%d\n",
        status, path ? path : "", code, context->jpeg_width,
        context->jpeg_height, context->jpeg_data_length,
        context->jpeg_submit_ms > 0
            ? (long long)(context->jpeg_submit_ms - context->jpeg_command_ms)
            : -1LL,
        context->jpeg_available_ms > 0
            ? (long long)(context->jpeg_available_ms -
                          context->jpeg_command_ms)
            : -1LL,
        context->jpeg_written_ms > 0
            ? (long long)(context->jpeg_written_ms - context->jpeg_command_ms)
            : -1LL,
        preview_scene_mode_name(context->original_scene_mode),
        preview_scene_mode_name(context->scene_mode),
        context->scene_mode_supported ? "true" : "false",
        context->hdr_scene_supported ? "true" : "false",
        context->original_scene_mode == SFOS_CAMERA2_SCENE_HDR
            ? "true" : "false",
        context->jpeg_control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE &&
                context->jpeg_scene_mode == ACAMERA_CONTROL_SCENE_MODE_HDR
            ? "true" : "false",
        context->hdr_scene_supported ? "scene-hdr" : "none",
        context->original_scene_mode == SFOS_CAMERA2_SCENE_HDR
            ? "true" : "false",
        preview_control_mode_name(context->jpeg_control_mode),
        context->jpeg_control_mode,
        context->jpeg_control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
            ? preview_android_scene_mode_name(context->jpeg_scene_mode) : "none",
        context->jpeg_control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
            ? context->jpeg_scene_mode : -1);
    if (payload_size <= 0 || payload_size >= (int)sizeof(payload)) {
        return;
    }

    unsigned char header[8] = { 'S', 'F', '2', 'M' };
    preview_write_le32(header + 4, (uint32_t)payload_size);

    pthread_mutex_lock(&context->output_lock);
    sfos_camera2_write_all(context->output_fd, header, sizeof(header));
    sfos_camera2_write_all(context->output_fd, payload, (size_t)payload_size);
    pthread_mutex_unlock(&context->output_lock);
}

static void preview_device_disconnected(void *opaque, ACameraDevice *device)
{
    (void)device;
    struct preview_context *context = opaque;
    atomic_store_explicit(&context->status.device_error, -1,
                          memory_order_release);
}

static void preview_device_error(void *opaque, ACameraDevice *device,
                                 int error)
{
    (void)device;
    struct preview_context *context = opaque;
    atomic_store_explicit(&context->status.device_error,
                          error > 0 ? error : -1, memory_order_release);
}

static void preview_image_available(void *opaque, AImageReader *reader)
{
    struct preview_context *context = opaque;
    if (atomic_load_explicit(&context->frames_written,
                             memory_order_acquire) >= context->frame_count ||
            atomic_load_explicit(&context->status.image_status,
                                 memory_order_acquire) < 0) {
        return;
    }

    AImage *image = NULL;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    atomic_store_explicit(&context->status.last_media_status, status,
                          memory_order_release);
    if (status != AMEDIA_OK || !image) {
        return;
    }
    if (atomic_load_explicit(&context->paused, memory_order_acquire)) {
        AImage_delete(image);
        return;
    }
    int64_t sensor_ns = 0;
    AImage_getTimestamp(image, &sensor_ns);
    const int64_t write_started_ms = sfos_camera2_now_ms();
    if (preview_write_rgb_frame(context, image) == 0) {
        const int64_t write_ms = sfos_camera2_now_ms() - write_started_ms;
        context->write_sum_ms += write_ms;
        if (write_ms > context->write_max_ms) context->write_max_ms = write_ms;
        // Sensor timestamps are the start of exposure on CLOCK_BOOTTIME
        // here; a value off by hours means another clock: not counted.
        struct timespec now;
        clock_gettime(CLOCK_BOOTTIME, &now);
        const int64_t latency_ms = ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec
                                    - sensor_ns) / 1000000;
        if (sensor_ns > 0 && latency_ms >= 0 && latency_ms < 10000) {
            context->latency_sum_ms += latency_ms;
            if (latency_ms > context->latency_max_ms) context->latency_max_ms = latency_ms;
            ++context->latency_frames;
        }
        const int64_t wall_ms = sfos_camera2_now_ms();
        if (context->latency_frames > 0 && wall_ms - context->latency_logged_ms >= 15000) {
            fprintf(stderr, "timing preview-latency camera=%s avg_ms=%lld max_ms=%lld frames=%d "
                            "exposure_ms=%.1f iso=%d nr=%d edge=%d write_avg_ms=%lld write_max_ms=%lld\n",
                    context->camera_id,
                    (long long)(context->latency_sum_ms / context->latency_frames),
                    (long long)context->latency_max_ms, context->latency_frames,
                    atomic_load_explicit(&context->live_exposure_time_ns,
                                         memory_order_acquire) / 1e6,
                    atomic_load_explicit(&context->live_sensor_sensitivity, memory_order_acquire),
                    atomic_load_explicit(&context->live_nr_mode, memory_order_acquire),
                    atomic_load_explicit(&context->live_edge_mode, memory_order_acquire),
                    (long long)(context->write_sum_ms / context->latency_frames),
                    (long long)context->write_max_ms);
            context->latency_sum_ms = 0;
            context->latency_max_ms = 0;
            context->latency_frames = 0;
            context->write_sum_ms = 0;
            context->write_max_ms = 0;
            context->latency_logged_ms = wall_ms;
        }
        if (atomic_fetch_add_explicit(&context->frames_written, 1,
                                      memory_order_release) == 0) {
            fprintf(stderr, "timing bridge first-frame ms=%lld\n",
                    (long long)(sfos_camera2_now_ms() - context->started_ms));
        }
    } else {
        atomic_store_explicit(&context->status.image_status, -1,
                              memory_order_release);
    }
    AImage_delete(image);
}

// Sends the detected faces as "faces=N rects=l,t,r,b,score;...": up to five,
// normalised to the full (unzoomed) field of view, as burst frames are.
static void preview_write_faces(struct preview_context *context, const ACameraMetadata *result)
{
    if (!context->face_detect_mode || context->output_fd < 0) {
        return;
    }
    ACameraMetadata_const_entry rects = { 0 };
    ACameraMetadata_const_entry scores = { 0 };
    const int has_rects = ACameraMetadata_getConstEntry(result, ACAMERA_STATISTICS_FACE_RECTANGLES,
                                                        &rects) == ACAMERA_OK;
    ACameraMetadata_getConstEntry(result, ACAMERA_STATISTICS_FACE_SCORES, &scores);
    const float left = (float)context->active_array[0], top = (float)context->active_array[1];
    // ACTIVE_ARRAY_SIZE is (left, top, width, height).
    const float width = (float)context->active_array[2];
    const float height = (float)context->active_array[3];
    const float zoom = context->zoom_ratio < 1.0f ? 1.0f : context->zoom_ratio;
    char payload[320];
    int count = 0;
    int used = 0;
    char list[256] = "";
    if (has_rects && width > 0 && height > 0) {
        for (uint32_t i = 0; i + 3 < rects.count && count < 5; i += 4) {
            const int score = scores.count > i / 4 ? scores.data.u8[i / 4] : 100;
            if (score < 30) continue; // Camera2: below 50 is often a false hit
            float box[4];
            for (int k = 0; k < 4; ++k) {
                const float v = (float)rects.data.i32[i + k];
                const float n = k % 2 == 0 ? (v - left) / width : (v - top) / height;
                box[k] = 0.5f + (n - 0.5f) / zoom;
            }
            const int written = snprintf(list + used, sizeof(list) - (size_t)used, "%s%.3f,%.3f,%.3f,%.3f,%d",
                                         count ? ";" : "", box[0], box[1], box[2], box[3], score);
            if (written <= 0 || used + written >= (int)sizeof(list)) break;
            used += written;
            ++count;
        }
    }
    const int size = snprintf(payload, sizeof(payload), "faces=%d rects=%s\n", count, count ? list : "-");
    if (size <= 0 || size >= (int)sizeof(payload) || !strcmp(payload, context->last_faces)) {
        return;
    }
    memcpy(context->last_faces, payload, (size_t)size + 1);
    unsigned char header[8] = { 'S', 'F', '2', 'M' };
    preview_write_le32(header + 4, (uint32_t)size);
    pthread_mutex_lock(&context->output_lock);
    sfos_camera2_write_all(context->output_fd, header, sizeof(header));
    sfos_camera2_write_all(context->output_fd, payload, (size_t)size);
    pthread_mutex_unlock(&context->output_lock);
}

static void preview_capture_completed(void *opaque,
                                      ACameraCaptureSession *session,
                                      ACaptureRequest *request,
                                      const ACameraMetadata *result)
{
    (void)session;
    (void)request;
    struct preview_context *context = opaque;
    if (!context || !result) {
        return;
    }

    int64_t timestamp = sfos_camera2_first_i64(result, ACAMERA_SENSOR_TIMESTAMP, 0);
    if (timestamp > 0) {
        pthread_mutex_lock(&context->output_lock);
        struct meter_sample *sample = meter_slot(context, timestamp);
        sample->iso = sfos_camera2_first_i32(result, ACAMERA_SENSOR_SENSITIVITY, 0);
        sample->shutter = sfos_camera2_first_i64(result, ACAMERA_SENSOR_EXPOSURE_TIME, 0);
        sample->ae = sfos_camera2_first_u8(result, ACAMERA_CONTROL_AE_STATE, -1);
        sample->ae_mode = sfos_camera2_first_u8(result, ACAMERA_CONTROL_AE_MODE, -1);
        sample->compensation = sfos_camera2_first_i32(result, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 0);
        sample->flicker = sfos_camera2_first_u8(result, ACAMERA_STATISTICS_SCENE_FLICKER, 0);
        sample->have_result = 1;
        meter_emit(context, sample);
        pthread_mutex_unlock(&context->output_lock);
    }

    int32_t sensitivity = sfos_camera2_first_i32(
        result, ACAMERA_SENSOR_SENSITIVITY, 0);
    int64_t exposure_time = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_EXPOSURE_TIME, 0);
    atomic_store_explicit(&context->live_nr_mode,
                          sfos_camera2_first_u8(result, ACAMERA_NOISE_REDUCTION_MODE, -1),
                          memory_order_release);
    atomic_store_explicit(&context->live_edge_mode,
                          sfos_camera2_first_u8(result, ACAMERA_EDGE_MODE, -1),
                          memory_order_release);
    int32_t af_state = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_AF_STATE, -1);
    float focus_distance = sfos_camera2_first_float(
        result, ACAMERA_LENS_FOCUS_DISTANCE, -1.0f);
    if (sensitivity > 0 || exposure_time > 0 ||
            af_state >= 0 || focus_distance >= 0.0f) {
        preview_write_metadata(context, sensitivity, exposure_time,
                               af_state, focus_distance);
    }
    preview_write_faces(context, result);
}

static void preview_jpeg_capture_completed(void *opaque,
                                           ACameraCaptureSession *session,
                                           ACaptureRequest *request,
                                           const ACameraMetadata *result)
{
    (void)session;
    (void)request;
    struct preview_context *context = opaque;
    if (!context || !result) {
        return;
    }

    int32_t actual_sensitivity = sfos_camera2_first_i32(
        result, ACAMERA_SENSOR_SENSITIVITY, -1);
    int64_t actual_exposure_time = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_EXPOSURE_TIME, -1);
    int64_t actual_frame_duration = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_FRAME_DURATION, -1);
    context->jpeg_result_ms = sfos_camera2_now_ms();
    context->jpeg_hal_sensitivity = actual_sensitivity;
    context->jpeg_hal_exposure_time_ns = actual_exposure_time;
    context->jpeg_hal_frame_duration_ns = actual_frame_duration;
    context->jpeg_control_mode = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_MODE, -1);
    context->jpeg_scene_mode = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_SCENE_MODE, -1);
    fprintf(stderr,
            "capture-exposure warm-jpeg requested_iso=%d "
            "requested_shutter=%lld actual_iso=%d actual_shutter=%lld "
            "actual_frame=%lld\n",
            context->sensor_sensitivity,
            (long long)context->exposure_time_ns,
            actual_sensitivity,
            (long long)actual_exposure_time,
            (long long)actual_frame_duration);
    preview_log_warm_jpeg_effective(context);
}

static void preview_jpeg_image_available(void *opaque, AImageReader *reader)
{
    struct preview_context *context = opaque;
    if (atomic_load_explicit(&context->jpeg_status,
                             memory_order_acquire) != 0) {
        return;
    }

    AImage *image = NULL;
    media_status_t status = AImageReader_acquireNextImage(reader, &image);
    atomic_store_explicit(&context->status.last_media_status, status,
                          memory_order_release);
    if (status != AMEDIA_OK || !image) {
        return;
    }

    uint8_t *data = NULL;
    int data_length = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    if (AImage_getWidth(image, &width) != AMEDIA_OK ||
            AImage_getHeight(image, &height) != AMEDIA_OK ||
            AImage_getFormat(image, &format) != AMEDIA_OK ||
            AImage_getPlaneData(image, 0, &data, &data_length) != AMEDIA_OK ||
            width != context->jpeg_width ||
            height != context->jpeg_height ||
            format != AIMAGE_FORMAT_JPEG || !data || data_length <= 0) {
        atomic_store_explicit(&context->jpeg_status, -1,
                              memory_order_release);
        preview_write_capture_result(context, "error", context->jpeg_path,
                                     PREVIEW_WRITE_ERROR);
    } else {
        context->jpeg_available_ms = sfos_camera2_now_ms();
        fprintf(stderr, "capture-timing bridge warm-jpeg available t=%lld\n",
                (long long)(context->jpeg_available_ms -
                            context->jpeg_command_ms));
        if (sfos_camera2_write_file(context->jpeg_path, data,
                                    (size_t)data_length) != 0) {
            atomic_store_explicit(&context->jpeg_status, -1,
                                  memory_order_release);
            preview_write_capture_result(context, "error",
                                         context->jpeg_path,
                                         PREVIEW_WRITE_ERROR);
        } else {
            context->jpeg_written_ms = sfos_camera2_now_ms();
            fprintf(stderr, "capture-timing bridge warm-jpeg written t=%lld\n",
                    (long long)(context->jpeg_written_ms -
                                context->jpeg_command_ms));
            context->jpeg_data_length = data_length;
            preview_log_warm_jpeg_effective(context);
            atomic_store_explicit(&context->jpeg_status, 1,
                                  memory_order_release);
            preview_write_capture_result(context, "ok", context->jpeg_path, 0);
        }
    }
    AImage_delete(image);
}

static const char *preview_cfa_name(int32_t cfa)
{
    switch (cfa) {
    case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB: return "RGGB";
    case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG: return "GRBG";
    case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG: return "GBRG";
    case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR: return "BGGR";
    default: return "UNKNOWN";
    }
}

static void preview_print_json_string(FILE *file, const char *value)
{
    fputc('"', file);
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            fputc('\\', file);
            fputc(*cursor, file);
        } else if (*cursor < 0x20) {
            fprintf(file, "\\u%04x", (unsigned int)*cursor);
        } else {
            fputc(*cursor, file);
        }
        ++cursor;
    }
    fputc('"', file);
}

static void preview_print_float_array(FILE *file, const float *values,
                                      uint32_t count)
{
    fputc('[', file);
    for (uint32_t index = 0; index < count; ++index) {
        fprintf(file, "%s%.9g", index ? "," : "", values[index]);
    }
    fputc(']', file);
}

static void preview_print_double_array(FILE *file, const double *values,
                                       uint32_t count)
{
    fputc('[', file);
    for (uint32_t index = 0; index < count; ++index) {
        fprintf(file, "%s%.17g", index ? "," : "", values[index]);
    }
    fputc(']', file);
}

static void preview_print_rational_array(
    FILE *file, const struct preview_rational *values, uint32_t count)
{
    fputc('[', file);
    for (uint32_t index = 0; index < count; ++index) {
        fprintf(file, "%s[%d,%d]", index ? "," : "",
                values[index].numerator, values[index].denominator);
    }
    fputc(']', file);
}

static bool preview_write_raw_metadata(const char *camera_id,
                                       struct preview_context *context)
{
    FILE *file = fopen(context->raw_metadata_path, "w");
    if (!file) {
        return false;
    }
    const struct preview_raw_image_metadata *image = &context->raw_image;
    const struct preview_static_raw_metadata *static_data =
        &context->raw_static;
    const struct preview_result_raw_metadata *result = &context->raw_result;

    fputs("{\n  \"camera_id\":", file);
    preview_print_json_string(file, camera_id);
    fputs(",\n  \"physical_camera_id\":", file);
    preview_print_json_string(file, result->physical_camera_id);
    fputs(",\n  \"raw_path\":", file);
    preview_print_json_string(file, context->raw_path);
    fprintf(file,
            ",\n  \"format\":\"%s\",\n  \"format_value\":%d,"
            "\n  \"width\":%d,\n  \"height\":%d,"
            "\n  \"planes\":%d,\n  \"pixel_stride\":%d,"
            "\n  \"row_stride\":%d,\n  \"data_length\":%d,"
            "\n  \"image_timestamp_ns\":%lld,"
            "\n  \"sensor_timestamp_ns\":%lld,"
            "\n  \"exposure_time_ns\":%lld,\n  \"iso\":%d,"
            "\n  \"cfa\":\"%s\",\n  \"cfa_value\":%d,"
            "\n  \"white_level\":%d,"
            "\n  \"black_level_pattern\":[%d,%d,%d,%d],"
            "\n  \"active_array\":[%d,%d,%d,%d],"
            "\n  \"focal_length_mm\":%.9g,"
            "\n  \"focus_mode_requested\":\"continuous\","
            "\n  \"scene_mode_requested_original\":\"%s\","
            "\n  \"scene_mode_requested\":\"%s\","
            "\n  \"scene_mode_supported\":%s,"
            "\n  \"hdr_scene_supported\":%s,"
            "\n  \"hdr_scene_requested\":%s,"
            "\n  \"hdr_scene_applied\":%s,"
            "\n  \"dol_supported\":false,"
            "\n  \"dol_source\":\"%s\","
            "\n  \"dol_requested\":%s,"
            "\n  \"dol_applied\":false,"
            "\n  \"control_mode_applied\":\"%s\","
            "\n  \"control_mode_applied_value\":%d,"
            "\n  \"scene_mode_applied\":\"%s\","
            "\n  \"scene_mode_applied_value\":%d,"
            "\n  \"sensor_sensitivity_requested\":%d,"
            "\n  \"exposure_time_requested_ns\":%lld,"
            "\n  \"shutter_ns_range\":[%lld,%lld],"
            "\n  \"aperture_requested\":%d,"
            "\n  \"noise_reduction_requested\":%d,"
            "\n  \"color_temperature_requested_kelvin\":%d,"
            "\n  \"color_tint_requested\":%d,"
            "\n  \"timing_ms\":{\"capture_submit\":%lld,"
            "\"image_available\":%lld,\"raw_file_written\":%lld,"
            "\"metadata_written\":%lld,\"total\":%lld}",
            context->raw_jpeg ? "JPEG" : context->raw_yuv ? "I420" : "RAW16",
            image->format, image->width, image->height, image->planes,
            image->pixel_stride, image->row_stride, image->data_length,
            (long long)image->timestamp_ns,
            (long long)result->timestamp_ns,
            (long long)result->exposure_time_ns, result->sensitivity,
            preview_cfa_name(static_data->cfa), static_data->cfa,
            static_data->white_level,
            static_data->black_level[0], static_data->black_level[1],
            static_data->black_level[2], static_data->black_level[3],
            static_data->active_array[0], static_data->active_array[1],
            static_data->active_array[2], static_data->active_array[3],
            static_data->focal_length,
            preview_scene_mode_name(context->original_scene_mode),
            preview_scene_mode_name(context->scene_mode),
            context->scene_mode_supported ? "true" : "false",
            context->hdr_scene_supported ? "true" : "false",
            context->original_scene_mode == SFOS_CAMERA2_SCENE_HDR
                ? "true" : "false",
            result->control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE &&
                    result->scene_mode == ACAMERA_CONTROL_SCENE_MODE_HDR
                ? "true" : "false",
            context->hdr_scene_supported ? "scene-hdr" : "none",
            context->original_scene_mode == SFOS_CAMERA2_SCENE_HDR
                ? "true" : "false",
            preview_control_mode_name(result->control_mode),
            result->control_mode,
            result->control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? preview_android_scene_mode_name(result->scene_mode) : "none",
            result->control_mode == ACAMERA_CONTROL_MODE_USE_SCENE_MODE
                ? result->scene_mode : -1,
            context->sensor_sensitivity,
            (long long)context->exposure_time_ns,
            (long long)static_data->exposure_time_range[0],
            (long long)static_data->exposure_time_range[1],
            context->aperture,
            context->noise_reduction, context->color_temperature_kelvin,
            context->color_tint,
            (long long)(context->raw_submit_ms - context->raw_command_ms),
            (long long)(context->raw_available_ms - context->raw_command_ms),
            (long long)(context->raw_written_ms - context->raw_command_ms),
            (long long)(context->raw_metadata_written_ms -
                        context->raw_command_ms),
            (long long)(sfos_camera2_now_ms() - context->raw_command_ms));
    fputs(",\n  \"color_correction_gains\":", file);
    preview_print_float_array(file, result->color_gains,
                              result->color_gains_count);
    fputs(",\n  \"neutral_color_point\":", file);
    preview_print_rational_array(file, result->neutral_color_point,
                                 result->neutral_count);
    fputs(",\n  \"capture_color_transform\":", file);
    preview_print_rational_array(file, result->color_transform,
                                 result->color_transform_count);
    fputs(",\n  \"color_transform1\":", file);
    preview_print_rational_array(file, static_data->color1,
                                 static_data->color1_count);
    fputs(",\n  \"noise_profile\":", file);
    preview_print_double_array(file, result->noise_profile,
                               result->noise_profile_count);
    fputs(",\n  \"dynamic_black_level\":", file);
    preview_print_float_array(file, result->dynamic_black_level,
                              result->dynamic_black_level_count);
    fprintf(file, ",\n  \"zoom_ratio\":%.4f", context->zoom_ratio);
    // Focus as the HAL reports it for this frame (2 passive focused, 4
    // locked; lens_state 1 = moving).
    fprintf(file, ",\n  \"af_mode\":%d,\n  \"af_state\":%d,\n  \"lens_state\":%d,"
                  "\n  \"lens_focus_distance\":%.6f",
            result->af_mode, result->af_state, result->lens_state,
            result->lens_focus_distance);
    fputs("\n}\n", file);

    bool success = ferror(file) == 0 && fflush(file) == 0;
    if (fclose(file) != 0) {
        success = false;
    }
    if (!success) {
        unlink(context->raw_metadata_path);
    }
    return success;
}

static const char *preview_raw_format_name(const struct preview_context *context)
{
    return context->raw_jpeg ? "JPEG" : context->raw_yuv ? "YUV" : "RAW16";
}

static void preview_raw_image_available(void *opaque, AImageReader *reader);

// Creates the capture ("raw") stream reader in the context's format.
static media_status_t preview_create_raw_reader(struct preview_context *context,
                                                AImageReader **reader,
                                                ANativeWindow **window)
{
    *reader = NULL;
    *window = NULL;
    const int32_t format = context->raw_jpeg ? AIMAGE_FORMAT_JPEG
                         : context->raw_yuv ? AIMAGE_FORMAT_YUV_420_888
                                            : AIMAGE_FORMAT_RAW16;
    media_status_t status = AImageReader_new(context->raw_width, context->raw_height,
                                             format, 2, reader);
    if (status == AMEDIA_OK && *reader) {
        AImageReader_ImageListener listener = {
            .context = context,
            .onImageAvailable = preview_raw_image_available,
        };
        status = AImageReader_setImageListener(*reader, &listener);
        if (status == AMEDIA_OK) {
            status = AImageReader_getWindow(*reader, window);
        }
    }
    if (status != AMEDIA_OK || !*window) {
        if (*reader) {
            AImageReader_delete(*reader);
        }
        *reader = NULL;
        *window = NULL;
        if (status == AMEDIA_OK) {
            status = AMEDIA_ERROR_UNKNOWN;
        }
    }
    return status;
}

// Packs a YUV_420_888 image (any row/pixel strides) as I420: the Y plane,
// then U, then V, each without padding. Returns the malloc'd buffer.
static uint8_t *preview_pack_i420(AImage *image, int32_t width, int32_t height,
                                  int *length)
{
    const int32_t chroma_width = (width + 1) / 2, chroma_height = (height + 1) / 2;
    const size_t luma_size = (size_t)width * (size_t)height;
    const size_t chroma_size = (size_t)chroma_width * (size_t)chroma_height;
    uint8_t *packed = malloc(luma_size + 2 * chroma_size);
    if (!packed) {
        return NULL;
    }
    for (int plane = 0; plane < 3; ++plane) {
        uint8_t *data = NULL;
        int data_length = 0;
        int32_t row_stride = 0, pixel_stride = 0;
        if (AImage_getPlaneData(image, plane, &data, &data_length) != AMEDIA_OK ||
                AImage_getPlaneRowStride(image, plane, &row_stride) != AMEDIA_OK ||
                AImage_getPlanePixelStride(image, plane, &pixel_stride) != AMEDIA_OK ||
                !data || data_length <= 0 || row_stride <= 0 || pixel_stride <= 0) {
            free(packed);
            return NULL;
        }
        const int32_t plane_width = plane == 0 ? width : chroma_width;
        const int32_t plane_height = plane == 0 ? height : chroma_height;
        uint8_t *out = packed + (plane == 0 ? 0 : luma_size + (size_t)(plane - 1) * chroma_size);
        for (int32_t y = 0; y < plane_height; ++y) {
            const size_t row = (size_t)y * (size_t)row_stride;
            const size_t last = row + (size_t)(plane_width - 1) * (size_t)pixel_stride;
            if (last >= (size_t)data_length) {
                free(packed);
                return NULL;
            }
            if (pixel_stride == 1) {
                memcpy(out + (size_t)y * plane_width, data + row, (size_t)plane_width);
            } else {
                for (int32_t x = 0; x < plane_width; ++x) {
                    out[(size_t)y * plane_width + x] = data[row + (size_t)x * (size_t)pixel_stride];
                }
            }
        }
    }
    *length = (int)(luma_size + 2 * chroma_size);
    return packed;
}

static void preview_raw_image_available(void *opaque, AImageReader *reader)
{
    struct preview_context *context = opaque;
    int bracket_index = -1;
    const bool bracket = context->raw_bracket_count > 0;
    if (!bracket && atomic_load_explicit(&context->raw_status,
                                         memory_order_acquire) != 0) return;

    AImage *image = NULL;
    media_status_t status = AImageReader_acquireNextImage(reader, &image);
    atomic_store_explicit(&context->status.last_media_status, status,
                          memory_order_release);
    // A callback with nothing left to acquire (its image was taken by an
    // earlier one) is not a failure; a frame that never arrives is caught
    // by the capture watchdog.
    if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) return;
    if (status != AMEDIA_OK || !image) {
        fprintf(stderr, "raw-image acquire failed status=%d bracket=%d\n", (int)status, bracket ? 1 : 0);
        if (bracket) {
            bracket_index = 0;
            atomic_store_explicit(
                &context->raw_bracket_status[bracket_index], -1,
                memory_order_release);
            preview_write_capture_result(
                context, "error",
                context->raw_bracket_metadata_paths[bracket_index],
                PREVIEW_READER_ERROR);
        } else {
            atomic_store_explicit(&context->raw_status, -1,
                                  memory_order_release);
            preview_write_capture_result(context, "error", context->raw_path,
                                         PREVIEW_READER_ERROR);
        }
        return;
    }

    if (bracket) {
        int64_t timestamp = 0;
        AImage_getTimestamp(image, &timestamp);
        const int64_t deadline = sfos_camera2_now_ms() + 1000;
        // Result callbacks are independent of the image-reader callback. Match
        // by timestamp, never by arrival order; stale/duplicate images cannot
        // consume another request's slot.
        do {
            for (int i = 0; i < context->raw_bracket_count; ++i) {
                if (atomic_load_explicit(&context->raw_bracket_result_status[i],
                                         memory_order_acquire) == 1 && timestamp > 0 &&
                        context->raw_bracket_result[i].timestamp_ns == timestamp &&
                        atomic_load_explicit(&context->raw_bracket_status[i],
                                             memory_order_acquire) == 0) {
                    bracket_index = i;
                    break;
                }
            }
            if (bracket_index >= 0) break;
            sfos_camera2_sleep_10_ms();
        } while (sfos_camera2_now_ms() < deadline);
        if (bracket_index < 0) {
            fprintf(stderr, "raw-bracket unmatched image timestamp=%lld\n", (long long)timestamp);
            AImage_delete(image);
            // Leave unmatched slots pending. The existing capture watchdog
            // reports an incomplete pair if the required frame never arrives.
            return;
        }
    }

    uint8_t *data = NULL;
    uint8_t *packed = NULL;
    int data_length = 0;
    int raw_status = 1;
    struct preview_raw_image_metadata *image_metadata =
        bracket_index >= 0 ? &context->raw_bracket_image[bracket_index]
                           : &context->raw_image;
    struct preview_result_raw_metadata *result_metadata =
        bracket_index >= 0 ? &context->raw_bracket_result[bracket_index]
                           : &context->raw_result;
    atomic_int *result_status =
        bracket_index >= 0 ? &context->raw_bracket_result_status[bracket_index]
                           : &context->raw_result_status;
    const char *raw_path = bracket_index >= 0
        ? context->raw_bracket_paths[bracket_index] : context->raw_path;
    const char *metadata_path = bracket_index >= 0
        ? context->raw_bracket_metadata_paths[bracket_index]
        : context->raw_metadata_path;
    context->raw_available_ms = sfos_camera2_now_ms();
    if (context->raw_jpeg) {
        if (AImage_getWidth(image, &image_metadata->width) != AMEDIA_OK ||
                AImage_getHeight(image, &image_metadata->height) != AMEDIA_OK ||
                AImage_getFormat(image, &image_metadata->format) != AMEDIA_OK ||
                AImage_getNumberOfPlanes(image, &image_metadata->planes) != AMEDIA_OK ||
                AImage_getTimestamp(image, &image_metadata->timestamp_ns) != AMEDIA_OK ||
                image_metadata->format != AIMAGE_FORMAT_JPEG ||
                AImage_getPlaneData(image, 0, &data, &data_length) != AMEDIA_OK ||
                !data || data_length <= 4) {
            raw_status = -2;
        } else {
            // The blob buffer is larger than the JPEG: stop after EOI.
            for (int i = data_length - 2; i >= 2; --i) {
                if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                    data_length = i + 2;
                    break;
                }
            }
            image_metadata->pixel_stride = 0;
            image_metadata->row_stride = 0;
        }
    } else if (context->raw_yuv) {
        if (AImage_getWidth(image, &image_metadata->width) != AMEDIA_OK ||
                AImage_getHeight(image, &image_metadata->height) != AMEDIA_OK ||
                AImage_getFormat(image, &image_metadata->format) != AMEDIA_OK ||
                AImage_getNumberOfPlanes(image, &image_metadata->planes) != AMEDIA_OK ||
                AImage_getTimestamp(image, &image_metadata->timestamp_ns) != AMEDIA_OK ||
                image_metadata->format != AIMAGE_FORMAT_YUV_420_888 ||
                image_metadata->planes != 3 ||
                !(packed = preview_pack_i420(image, image_metadata->width,
                                             image_metadata->height, &data_length))) {
            raw_status = -2;
        } else {
            data = packed;
            image_metadata->pixel_stride = 1;
            image_metadata->row_stride = image_metadata->width;
        }
    } else if (AImage_getWidth(image, &image_metadata->width) != AMEDIA_OK ||
            AImage_getHeight(image, &image_metadata->height) != AMEDIA_OK ||
            AImage_getFormat(image, &image_metadata->format) != AMEDIA_OK ||
            AImage_getNumberOfPlanes(image, &image_metadata->planes) !=
                AMEDIA_OK ||
            AImage_getTimestamp(image, &image_metadata->timestamp_ns) !=
                AMEDIA_OK ||
            image_metadata->format != AIMAGE_FORMAT_RAW16 ||
            image_metadata->planes != 1 ||
            AImage_getPlanePixelStride(
                image, 0, &image_metadata->pixel_stride) != AMEDIA_OK ||
            AImage_getPlaneRowStride(
                image, 0, &image_metadata->row_stride) != AMEDIA_OK ||
            AImage_getPlaneData(image, 0, &data, &data_length) != AMEDIA_OK ||
            !data || data_length <= 0) {
        raw_status = -2;
    }
    image_metadata->data_length = data_length;
    if (raw_status > 0 &&
            sfos_camera2_write_file(raw_path, data, (size_t)data_length) != 0) {
        raw_status = -3;
    }
    if (raw_status > 0) {
        context->raw_written_ms = sfos_camera2_now_ms();
    } else {
        unlink(raw_path);
    }
    free(packed);
    AImage_delete(image);

    int64_t metadata_deadline = sfos_camera2_now_ms() + 500;
    while (raw_status > 0 &&
            atomic_load_explicit(result_status, memory_order_acquire) == 0 &&
            sfos_camera2_now_ms() < metadata_deadline) {
        sfos_camera2_sleep_10_ms();
    }
    if (raw_status > 0 &&
            atomic_load_explicit(result_status, memory_order_acquire) <= 0) {
        raw_status = -5;
    }
    if (raw_status > 0 && (image_metadata->timestamp_ns <= 0 ||
            image_metadata->timestamp_ns != result_metadata->timestamp_ns)) raw_status = -6;
    if (raw_status > 0) {
        context->raw_metadata_written_ms = sfos_camera2_now_ms();
        struct preview_context metadata_context = *context;
        snprintf(metadata_context.raw_path, sizeof(metadata_context.raw_path),
                 "%s", raw_path);
        snprintf(metadata_context.raw_metadata_path,
                 sizeof(metadata_context.raw_metadata_path), "%s",
                 metadata_path);
        metadata_context.raw_image = *image_metadata;
        metadata_context.raw_result = *result_metadata;
        if (bracket_index >= 0) {
            metadata_context.scene_mode = SFOS_CAMERA2_SCENE_MANUAL;
            metadata_context.original_scene_mode = SFOS_CAMERA2_SCENE_MANUAL;
            metadata_context.sensor_sensitivity =
                context->raw_bracket_sensor_sensitivity[bracket_index];
            metadata_context.exposure_time_ns =
                context->raw_bracket_exposure_time_ns[bracket_index];
        }
        if (!preview_write_raw_metadata(context->camera_id,
                                        &metadata_context)) {
            raw_status = -4;
        }
    }
    if (raw_status > 0 && bracket_index < 0) {
        preview_log_warm_raw_effective(context);
    }
    if (raw_status <= 0) {
        // -2 image format/planes, -3 file write, -4 sidecar write,
        // -5 no capture result, -6 image/result timestamps differ.
        fprintf(stderr, "raw-image failed index=%d status=%d format=0x%x planes=%d "
                "image_ts=%lld result_ts=%lld\n", bracket_index, raw_status,
                image_metadata->format, image_metadata->planes,
                (long long)image_metadata->timestamp_ns,
                (long long)result_metadata->timestamp_ns);
    }
    if (bracket_index >= 0) {
        atomic_store_explicit(&context->raw_bracket_status[bracket_index],
                              raw_status, memory_order_release);
    } else {
        atomic_store_explicit(&context->raw_status, raw_status,
                              memory_order_release);
    }
    preview_write_capture_result(context, raw_status > 0 ? "ok" : "error",
                                 metadata_path,
                                 raw_status > 0 ? 0 : PREVIEW_WRITE_ERROR);
}

static void preview_write_message(struct preview_context *context, const char *payload,
                                  int payload_size);

// The first frame of a RAW capture starts exposing: the app gives its
// shutter feedback, once per capture (generation).
static void preview_report_capture_started(struct preview_context *context, int generation)
{
    if (atomic_exchange_explicit(&context->capture_started_generation, generation,
                                 memory_order_acq_rel) == generation) {
        return;
    }
    static const char payload[] = "capture-started=1\n";
    preview_write_message(context, payload, (int)sizeof(payload) - 1);
}

static void preview_raw_capture_started(void *opaque, ACameraCaptureSession *session,
                                        const ACaptureRequest *request, int64_t timestamp)
{
    (void)session; (void)request; (void)timestamp;
    struct preview_context *context = opaque;
    preview_report_capture_started(
        context, atomic_load_explicit(&context->raw_bracket_generation, memory_order_acquire));
}

static void preview_raw_capture_completed(void *opaque,
                                          ACameraCaptureSession *session,
                                          ACaptureRequest *request,
                                          const ACameraMetadata *result)
{
    (void)session;
    struct preview_context *context = opaque;
    int bracket_index = -1;
    ACameraMetadata_const_entry shutter, iso;
    if (ACaptureRequest_getConstEntry(request, ACAMERA_SENSOR_EXPOSURE_TIME, &shutter) == ACAMERA_OK &&
            shutter.count == 1 &&
            ACaptureRequest_getConstEntry(request, ACAMERA_SENSOR_SENSITIVITY, &iso) == ACAMERA_OK && iso.count == 1) {
        // Burst frames share one exposure: take the first matching slot that
        // has no result yet (results arrive in request order).
        for (int index = 0; index < context->raw_bracket_count; ++index) {
            if (context->raw_bracket_exposure_time_ns[index] == shutter.data.i64[0] &&
                    context->raw_bracket_sensor_sensitivity[index] == iso.data.i32[0] &&
                    atomic_load_explicit(&context->raw_bracket_result_status[index],
                                         memory_order_acquire) == 0) {
                bracket_index = index;
                break;
            }
        }
    }
    if (context->raw_bracket_count > 0 && (bracket_index < 0 ||
            atomic_load_explicit(&context->raw_bracket_result_status[bracket_index],
                                 memory_order_acquire) != 0)) return;
    struct preview_result_raw_metadata *destination =
        bracket_index >= 0 ? &context->raw_bracket_result[bracket_index]
                           : &context->raw_result;
    memset(destination, 0, sizeof(*destination));
    sfos_camera2_active_physical_id(result, destination->physical_camera_id,
                                    sizeof(destination->physical_camera_id));
    destination->timestamp_ns = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_TIMESTAMP, -1);
    destination->exposure_time_ns = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_EXPOSURE_TIME, -1);
    destination->frame_duration_ns = sfos_camera2_first_i64(
        result, ACAMERA_SENSOR_FRAME_DURATION, -1);
    destination->sensitivity = sfos_camera2_first_i32(
        result, ACAMERA_SENSOR_SENSITIVITY, -1);
    destination->control_mode = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_MODE, -1);
    destination->scene_mode = sfos_camera2_first_u8(
        result, ACAMERA_CONTROL_SCENE_MODE, -1);
    destination->af_mode = sfos_camera2_first_u8(result, ACAMERA_CONTROL_AF_MODE, -1);
    destination->af_state = sfos_camera2_first_u8(result, ACAMERA_CONTROL_AF_STATE, -1);
    destination->lens_state = sfos_camera2_first_u8(result, ACAMERA_LENS_STATE, -1);
    destination->lens_focus_distance = sfos_camera2_first_float(
        result, ACAMERA_LENS_FOCUS_DISTANCE, -1.0f);
    fprintf(stderr,
            "capture-exposure warm-raw requested_iso=%d "
            "requested_shutter=%lld actual_iso=%d actual_shutter=%lld "
            "actual_frame=%lld\n",
            context->sensor_sensitivity,
            (long long)context->exposure_time_ns,
            destination->sensitivity,
            (long long)destination->exposure_time_ns,
            (long long)destination->frame_duration_ns);
    destination->color_gains_count = sfos_camera2_copy_float_array(
        result, ACAMERA_COLOR_CORRECTION_GAINS,
        destination->color_gains, 4);
    destination->neutral_count = preview_copy_rational(
        result, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT,
        destination->neutral_color_point, 3);
    destination->color_transform_count = preview_copy_rational(
        result, ACAMERA_COLOR_CORRECTION_TRANSFORM,
        destination->color_transform, 9);
    destination->noise_profile_count = sfos_camera2_copy_double_array(
        result, ACAMERA_SENSOR_NOISE_PROFILE, destination->noise_profile, 8);
    destination->dynamic_black_level_count = sfos_camera2_copy_float_array(
        result, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL,
        destination->dynamic_black_level, 4);
    int exposed = 0;
    if (bracket_index >= 0) {
        atomic_store_explicit(
            &context->raw_bracket_result_status[bracket_index], 1,
            memory_order_release);
        exposed = 1;
        for (int index = 0; index < context->raw_bracket_count; ++index) {
            if (atomic_load_explicit(&context->raw_bracket_result_status[index],
                                     memory_order_acquire) != 1) {
                exposed = 0;
                break;
            }
        }
    } else {
        atomic_store_explicit(&context->raw_result_status, 1,
                              memory_order_release);
        exposed = 1;
    }
    if (exposed) {
        // Every frame is exposed (results come after the exposure): the
        // phone may move. The files follow once written, which for a burst
        // of 25 MB RAW frames takes far longer; the app gives its shutter
        // feedback now rather than then.
        char payload[64];
        const int size = snprintf(payload, sizeof(payload), "capture-exposed=1 frames=%d\n",
                                  bracket_index >= 0 ? context->raw_bracket_count : 1);
        preview_write_message(context, payload, size);
    }
}

// The callback context supplies a generation on API 24, where request user
// contexts are not available. The sequence-end callback releases it only after
// all result/failure callbacks have returned (NDK capture callback contract).
struct preview_bracket_capture {
    struct preview_context *owner;
    int generation;
};

static void preview_bracket_completed(void *opaque, ACameraCaptureSession *session,
                                       ACaptureRequest *request, const ACameraMetadata *result)
{
    struct preview_bracket_capture *capture = opaque;
    if (capture->generation != atomic_load_explicit(&capture->owner->raw_bracket_generation,
                                                    memory_order_acquire)) return;
    preview_raw_capture_completed(capture->owner, session, request, result);
}

static void preview_bracket_started(void *opaque, ACameraCaptureSession *session,
                                    const ACaptureRequest *request, int64_t timestamp)
{
    (void)session; (void)request; (void)timestamp;
    struct preview_bracket_capture *capture = opaque;
    if (capture->generation != atomic_load_explicit(&capture->owner->raw_bracket_generation,
                                                    memory_order_acquire)) return;
    preview_report_capture_started(capture->owner, capture->generation);
}

static void preview_bracket_failed(void *opaque, ACameraCaptureSession *session,
                                    ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    (void)session; (void)request;
    struct preview_bracket_capture *capture = opaque;
    fprintf(stderr, "raw-bracket frame failed reason=%d frame=%lld captured=%d\n",
            failure ? failure->reason : -1, failure ? (long long)failure->frameNumber : -1LL,
            failure ? (int)failure->wasImageCaptured : -1);
    struct preview_context *context = capture->owner;
    if (capture->generation != atomic_load_explicit(&context->raw_bracket_generation,
                                                    memory_order_acquire)) return;
    for (int i = 0; i < context->raw_bracket_count; ++i) {
        atomic_store_explicit(&context->raw_bracket_result_status[i], -1, memory_order_release);
    }
    preview_write_capture_result(context, "error", context->raw_bracket_metadata_paths[0],
                                 PREVIEW_READER_ERROR);
}

static void preview_bracket_sequence_end(void *opaque, ACameraCaptureSession *session,
                                          int sequence_id, int64_t frame_number)
{
    (void)session; (void)sequence_id; (void)frame_number;
    free(opaque);
}

static void preview_bracket_sequence_abort(void *opaque, ACameraCaptureSession *session,
                                            int sequence_id)
{
    (void)sequence_id;
    preview_bracket_failed(opaque, session, NULL, NULL);
    free(opaque);
}

// Video mode (sfos_camera2_preview_set_video() before the preview starts):
// the encoder's persistent surface is a session output from the start, and
// record-start/record-stop add it to or remove it from the repeating
// request, so recording starts and stops without touching the viewfinder.
struct preview_video_options {
    int width;
    int height;
    int fps;
    int bitrate;
    int stabilization;
    int hevc;
    int audio_sample_rate;
    int audio_channels;
};

static struct preview_video_options g_preview_video;

SFOS_CAMERA2_EXPORT void sfos_camera2_preview_set_video(
    int width, int height, int fps, int bitrate, int stabilization, int hevc,
    int audio_sample_rate, int audio_channels)
{
    g_preview_video = (struct preview_video_options) {
        .width = width,
        .height = height,
        .fps = fps,
        .bitrate = bitrate,
        .stabilization = stabilization,
        .hevc = hevc,
        .audio_sample_rate = audio_sample_rate,
        .audio_channels = audio_channels,
    };
}

enum preview_video_state {
    PREVIEW_VIDEO_IDLE = 0,
    PREVIEW_VIDEO_RECORDING,
    PREVIEW_VIDEO_STOPPING,
};

// After the last request with the encoder: wait for its frames, then a
// little for their buffers (as the --record-video loop does).
#define PREVIEW_VIDEO_SETTLE_MS 1000
#define PREVIEW_VIDEO_BUFFER_GRACE_MS 60

struct preview_video {
    struct recorder_config config;
    int stabilization_supported;
    int32_t fps_range[2];
    ANativeWindow *surface;
    ACameraOutputTarget *target;
    ACaptureSessionOutput *output;
    struct recorder *recorder;
    enum preview_video_state state;
    int stop_sequence;
    int64_t stop_deadline_ms;
    int64_t ended_ms;
    int error;
    // The preview is ending: no encoder for a next recording.
    bool closing;
    char path[4096];
};

static int preview_request_u8(const ACaptureRequest *request, uint32_t tag)
{
    ACameraMetadata_const_entry entry;
    if (ACaptureRequest_getConstEntry(request, tag, &entry) != ACAMERA_OK || entry.count < 1)
        return -1;
    return entry.data.u8[0];
}

static void preview_sequence_ended(void *opaque, ACameraCaptureSession *session,
                                   int sequence_id, int64_t frame_number)
{
    (void)session;
    (void)frame_number;
    struct preview_context *context = opaque;
    atomic_store_explicit(&context->ended_sequence, sequence_id, memory_order_release);
}

static void preview_sequence_aborted(void *opaque, ACameraCaptureSession *session,
                                     int sequence_id)
{
    preview_sequence_ended(opaque, session, sequence_id, -1);
}

static void preview_write_message(struct preview_context *context, const char *payload,
                                  int payload_size)
{
    if (payload_size <= 0) return;
    unsigned char header[8] = { 'S', 'F', '2', 'M' };
    preview_write_le32(header + 4, (uint32_t)payload_size);
    pthread_mutex_lock(&context->output_lock);
    sfos_camera2_write_all(context->output_fd, header, sizeof(header));
    sfos_camera2_write_all(context->output_fd, payload, (size_t)payload_size);
    pthread_mutex_unlock(&context->output_lock);
}

// "record-status=started|ok|error code=N path=..." (the path last: it may
// contain spaces).
static void preview_write_record_status(struct preview_context *context, const char *status,
                                        int code, const char *path)
{
    char payload[4200];
    int size = snprintf(payload, sizeof(payload), "record-status=%s code=%d path=%s\n",
                        status, code, path && *path ? path : "-");
    if (size > 0 && size < (int)sizeof(payload)) preview_write_message(context, payload, size);
}

// The record template's extras on a request: the frame rate and
// stabilization. Preview frames get both too, so the viewfinder shows the
// stabilised frame that is recorded.
static void preview_video_configure_request(ACaptureRequest *request,
                                            const struct preview_video *video)
{
    if (!request || !video->target) return;
    ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2,
                                 video->fps_range);
    if (video->stabilization_supported) {
        sfos_camera2_set_request_u8(request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE,
                                    g_preview_video.stabilization
                                        ? ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON
                                        : ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    }
}

// Larger than 1080p failed: 1920x1080 if the camera streams it, at half the
// bitrate (a quarter of 4K's pixels needs about half the bits). Returns
// false when there is nothing smaller to fall back to.
static bool preview_video_fall_back_to_1080p(struct preview_video *video,
                                             const ACameraMetadata *characteristics)
{
    struct recorder_config *config = &video->config;
    if (config->width * config->height <= 1920 * 1080 ||
            !sfos_camera2_has_output_size(characteristics, AIMAGE_FORMAT_PRIVATE, 1920, 1080)) {
        return false;
    }
    fprintf(stderr, "record-prepare %dx%d unavailable; using 1920x1080\n",
            config->width, config->height);
    config->bitrate = (int)((long long)config->bitrate * 1920 * 1080 /
                            ((long long)config->width * config->height) * 2);
    config->width = 1920;
    config->height = 1080;
    return true;
}

static void preview_video_prepare(struct preview_video *video)
{
    const char *stage = "encoder";
    int media_status = 0;
    video->recorder = recorder_prepare(&video->config, video->surface, &stage, &media_status);
    if (!video->recorder) {
        fprintf(stderr, "record-prepare failed stage=%s media_status=%d\n", stage, media_status);
    }
}

// Above 1080p, encoders often cap the bitrate (40 Mbit/s is common): the
// requested rate, then lower ones, before the caller gives up on the size.
static void preview_video_prepare_ladder(struct preview_video *video)
{
    const int requested = video->config.bitrate;
    const int rates[] = { requested, 40000000, 30000000, 20000000 };
    preview_video_prepare(video);
    for (size_t index = 1; !video->recorder && index < sizeof(rates) / sizeof(rates[0]) &&
            video->config.width * video->config.height > 1920 * 1080; ++index) {
        if (rates[index] >= requested) continue;
        fprintf(stderr, "record-prepare %dx%d %s at %d bit/s failed; trying %d\n",
                video->config.width, video->config.height, video->config.hevc ? "hevc" : "avc",
                video->config.bitrate, rates[index]);
        video->config.bitrate = rates[index];
        preview_video_prepare(video);
    }
    if (!video->recorder) video->config.bitrate = requested;
}

static void preview_video_start(struct preview_video *video, struct preview_context *context,
                                ACameraCaptureSession *session,
                                ACameraCaptureSession_captureCallbacks *callbacks,
                                ACaptureRequest *request, ACaptureRequest *focus_request,
                                const struct preview_command *command)
{
    const char *stage = "state";
    int code = RECORDER_STATE_ERROR;
    if (!video->target) {
        code = PREVIEW_CONFIGURATION_ERROR;
        stage = "no_video_stream";
        goto failed;
    }
    if (video->state != PREVIEW_VIDEO_IDLE) goto failed;
    if (!video->recorder) preview_video_prepare(video);
    if (!video->recorder) {
        code = RECORDER_ENCODER_ERROR;
        stage = "encoder";
        goto failed;
    }
    code = recorder_begin(video->recorder, command->path, command->orientation, &stage);
    if (code < 0) {
        recorder_destroy(video->recorder);
        video->recorder = NULL;
        unlink(command->path);
        goto failed;
    }
    ACaptureRequest_addTarget(request, video->target);
    ACaptureRequest_addTarget(focus_request, video->target);
    ACaptureRequest *requests[] = { request };
    camera_status_t status = ACameraCaptureSession_setRepeatingRequest(
        session, callbacks, 1, requests, &context->repeat_sequence);
    atomic_store_explicit(&context->status.last_camera_status, status, memory_order_release);
    if (status != ACAMERA_OK) {
        ACaptureRequest_removeTarget(request, video->target);
        ACaptureRequest_removeTarget(focus_request, video->target);
        ACameraCaptureSession_setRepeatingRequest(session, callbacks, 1, requests,
                                                  &context->repeat_sequence);
        char json[64];
        recorder_finish(video->recorder, json, sizeof(json));
        recorder_destroy(video->recorder);
        video->recorder = NULL;
        unlink(command->path);
        code = PREVIEW_SUBMIT_ERROR;
        stage = "repeating_request";
        goto failed;
    }
    snprintf(video->path, sizeof(video->path), "%s", command->path);
    video->state = PREVIEW_VIDEO_RECORDING;
    video->error = 0;
    fprintf(stderr, "record-start orientation=%d path=%s\n", command->orientation, video->path);
    preview_write_record_status(context, "started", 0, video->path);
    return;

failed:
    fprintf(stderr, "record-start failed stage=%s code=%d path=%s\n", stage, code, command->path);
    preview_write_record_status(context, "error", code, command->path);
}

// Takes the encoder off the requests; the file is finished once the
// frames already on their way have reached it (preview_video_step).
static void preview_video_stop(struct preview_video *video, struct preview_context *context,
                               ACameraCaptureSession *session,
                               ACameraCaptureSession_captureCallbacks *callbacks,
                               ACaptureRequest *request, ACaptureRequest *focus_request)
{
    if (video->state != PREVIEW_VIDEO_RECORDING) return;
    ACaptureRequest_removeTarget(request, video->target);
    ACaptureRequest_removeTarget(focus_request, video->target);
    video->stop_sequence = context->repeat_sequence;
    if (session) {
        ACaptureRequest *requests[] = { request };
        ACameraCaptureSession_setRepeatingRequest(session, callbacks, 1, requests,
                                                  &context->repeat_sequence);
    }
    video->state = PREVIEW_VIDEO_STOPPING;
    video->stop_deadline_ms = sfos_camera2_now_ms() + PREVIEW_VIDEO_SETTLE_MS;
    video->ended_ms = 0;
}

static void preview_video_finish(struct preview_video *video, struct preview_context *context)
{
    char json[4096];
    int code = recorder_finish(video->recorder, json, sizeof(json));
    recorder_destroy(video->recorder);
    video->recorder = NULL;
    if (code == 0 && video->error) code = video->error;
    fprintf(stderr, "record-result %s\n", json);
    if (code == RECORDER_NO_FRAMES) unlink(video->path);
    preview_write_record_status(context, code == 0 ? "ok" : "error", code, video->path);
    video->state = PREVIEW_VIDEO_IDLE;
    // The next recording's encoder, ready before it is asked for.
    if (!video->closing) preview_video_prepare(video);
}

// Every loop pass: encoded data into the file, and the end of a stopping
// recording once its last frames are in.
static void preview_video_step(struct preview_video *video, struct preview_context *context,
                               ACameraCaptureSession *session,
                               ACameraCaptureSession_captureCallbacks *callbacks,
                               ACaptureRequest *request, ACaptureRequest *focus_request)
{
    if (video->state == PREVIEW_VIDEO_IDLE) return;
    if (!video->error) {
        const char *stage = "recording";
        const int code = recorder_poll(video->recorder, &stage);
        if (code < 0) {
            fprintf(stderr, "record-error stage=%s code=%d\n", stage, code);
            video->error = code;
            preview_video_stop(video, context, session, callbacks, request, focus_request);
        }
    }
    if (video->state != PREVIEW_VIDEO_STOPPING) return;
    const int64_t now = sfos_camera2_now_ms();
    if (!video->ended_ms &&
            atomic_load_explicit(&context->ended_sequence, memory_order_acquire) >=
                video->stop_sequence) {
        video->ended_ms = now;
    }
    if ((video->ended_ms && now - video->ended_ms >= PREVIEW_VIDEO_BUFFER_GRACE_MS) ||
            now >= video->stop_deadline_ms) {
        preview_video_finish(video, context);
    }
}

static void preview_status_json(char *out, size_t out_size, bool success,
                                const char *stage, int code,
                                const struct preview_context *context)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size,
             "{\"status\":\"%s\",\"stage\":\"%s\",\"code\":%d,"
             "\"preview\":{\"width\":%d,\"height\":%d,"
             "\"frames_requested\":%d,\"frames_written\":%d},"
             "\"jpeg\":{\"width\":%d,\"height\":%d,\"data_length\":%d},"
             "\"diagnostics\":{\"camera_status\":%d,"
             "\"media_status\":%d,\"device_error\":%d,"
             "\"image_status\":%d,\"jpeg_status\":%d}}",
             success ? "ok" : "error", stage ? stage : "unknown", code,
             context ? context->width : 0, context ? context->height : 0,
             context ? context->frame_count : 0,
             context ? atomic_load_explicit(&context->frames_written,
                                            memory_order_acquire) : 0,
             context ? context->jpeg_width : 0,
             context ? context->jpeg_height : 0,
             context ? context->jpeg_data_length : 0,
             context ? atomic_load_explicit(&context->status.last_camera_status,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.last_media_status,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.device_error,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->status.image_status,
                                            memory_order_acquire) : 0,
             context ? atomic_load_explicit(&context->jpeg_status,
                                            memory_order_acquire) : 0);
}

/**
 * @brief Run the live preview loop and service warm capture commands.
 *
 * This is the long-lived bridge entry point used by the QML preview item. It
 * writes framed RGB preview buffers and listens for focus/settings/capture
 * commands on a control file descriptor.
 */
SFOS_CAMERA2_EXPORT int sfos_camera2_preview_ppm(
    const char *camera_id, int width, int height, int frame_count,
    int timeout_ms, int output_fd, int control_fd, int jpeg_width,
    int jpeg_height, int jpeg_quality, int jpeg_orientation_degrees,
    int raw_width, int raw_height, int raw_format,
    float zoom_ratio, float focus_x, float focus_y,
    char *out, size_t out_size)
{
    int result_code = 0;
    const char *stage = "complete";
    ACameraManager *manager = NULL;
    ACameraMetadata *characteristics = NULL;
    ACameraDevice *device = NULL;
    AImageReader *reader = NULL;
    AImageReader *jpeg_reader = NULL;
    AImageReader *raw_reader = NULL;
    ANativeWindow *window = NULL;
    ANativeWindow *jpeg_window = NULL;
    ANativeWindow *raw_window = NULL;
    ACameraOutputTarget *target = NULL;
    ACameraOutputTarget *jpeg_target = NULL;
    ACameraOutputTarget *raw_target = NULL;
    ACaptureRequest *request = NULL;
    ACaptureRequest *focus_request = NULL;
    ACaptureRequest *jpeg_request = NULL;
    ACaptureRequest *raw_request = NULL;
    ACaptureSessionOutput *output = NULL;
    ACaptureSessionOutput *jpeg_output = NULL;
    ACaptureSessionOutput *raw_output = NULL;
    ACaptureSessionOutputContainer *container = NULL;
    ACameraCaptureSession *session = NULL;
    bool output_lock_ready = false;
    char raw_error[48] = "";
    bool raw_fallback_tried = false;
    int requested_raw_width = raw_width, requested_raw_height = raw_height;
    struct preview_video video;
    memset(&video, 0, sizeof(video));
    const char *video_error = "none";
    bool quit = false;

    struct preview_context context;
    memset(&context, 0, sizeof(context));
    sfos_camera2_status_init(&context.status);
    atomic_init(&context.frames_written, 0);
    atomic_init(&context.jpeg_status, -1);
    atomic_init(&context.raw_status, -1);
    atomic_init(&context.raw_bracket_generation, 0);
    atomic_init(&context.raw_result_status, -1);
    atomic_init(&context.raw_sequence_status, -1);
    for (int index = 0; index < PREVIEW_MAX_RAW_BRACKET; ++index) {
        atomic_init(&context.raw_bracket_status[index], -1);
        atomic_init(&context.raw_bracket_result_status[index], -1);
        context.raw_bracket_requests[index] = NULL;
    }
    atomic_init(&context.live_sensor_sensitivity, -1);
    atomic_init(&context.live_exposure_time_ns, -1);
    atomic_init(&context.ended_sequence, -1);
    atomic_init(&context.capture_started_generation, -1);
    atomic_init(&context.live_nr_mode, -1);
    atomic_init(&context.live_edge_mode, -1);
    context.started_ms = sfos_camera2_now_ms();
    context.repeat_sequence = -1;
    context.width = width;
    context.height = height;
    context.frame_count = frame_count;
    context.output_fd = output_fd;
    context.control_fd = control_fd;
    context.jpeg_width = jpeg_width;
    context.jpeg_height = jpeg_height;
    context.jpeg_quality = jpeg_quality;
    context.jpeg_orientation = jpeg_orientation_degrees;
    context.raw_width = raw_width;
    context.raw_height = raw_height;
    // 1: YUV burst frames; 2: the same frames as maximum-quality JPEG from
    // the start (the fallback when YUV is refused, and lighter for the
    // processing of front cameras that lag with a second YUV stream).
    context.raw_yuv = (raw_format & 0xff) == 1 || (raw_format & 0xff) == 2;
    context.raw_jpeg = (raw_format & 0xff) == 2;
    context.preview_yuv = (raw_format & SFOS_CAMERA2_PREVIEW_YUV_FRAMES) != 0;
    atomic_init(&context.paused, (raw_format & SFOS_CAMERA2_PREVIEW_START_PAUSED) != 0);
    snprintf(context.camera_id, sizeof(context.camera_id), "%s",
             camera_id ? camera_id : "");
    context.zoom_ratio = zoom_ratio < 1.0f ? 1.0f : zoom_ratio;
    context.focus_x = focus_x;
    context.focus_y = focus_y;
    context.focus_mode = SFOS_CAMERA2_FOCUS_CONTINUOUS;
    context.focus_distance = 0.0f;
    context.exposure_compensation = 0;
    context.original_scene_mode = SFOS_CAMERA2_SCENE_NONE;
    context.scene_mode = SFOS_CAMERA2_SCENE_NONE;
    context.scene_mode_supported = 1;
    context.hdr_scene_supported = 0;
    context.jpeg_control_mode = -1;
    context.jpeg_scene_mode = -1;
    context.color_temperature_kelvin = 0;
    context.color_tint = 0;
    context.sensor_sensitivity = 0;
    context.exposure_time_ns = 0;
    context.aperture = 0;
    context.noise_reduction = SFOS_CAMERA2_NOISE_REDUCTION_NONE;

		    if (!camera_id || !*camera_id || width <= 0 || height <= 0 ||
		            frame_count <= 0 || frame_count > 100000000 ||
		            timeout_ms < 1000 || output_fd < 0 ||
		            jpeg_quality < 1 || jpeg_quality > 100 ||
                    raw_width < 0 || raw_height < 0 ||
                    (raw_width == 0 && raw_height > 0) ||
                    (raw_width > 0 && raw_height == 0) ||
                    !out || out_size < 2) {
	        preview_status_json(out, out_size, false, "arguments",
	                            PREVIEW_INVALID_ARGUMENT, &context);
	        return PREVIEW_INVALID_ARGUMENT;
	    }

    if (pthread_mutex_init(&context.output_lock, NULL) != 0) {
        result_code = PREVIEW_WRITE_ERROR;
        stage = "output_lock";
        goto cleanup;
    }
    output_lock_ready = true;

    manager = ACameraManager_create();
    if (!manager) {
        result_code = PREVIEW_MANAGER_ERROR;
        stage = "manager_create";
        goto cleanup;
    }
    camera_status_t camera_status = ACameraManager_getCameraCharacteristics(
        manager, camera_id, &characteristics);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !characteristics) {
        result_code = PREVIEW_CHARACTERISTICS_ERROR;
        stage = "characteristics";
        goto cleanup;
    }
    if (!sfos_camera2_has_output_size(
            characteristics, AIMAGE_FORMAT_YUV_420_888, width, height)) {
        result_code = PREVIEW_UNSUPPORTED_SIZE;
        stage = "preview_size";
        goto cleanup;
    }
    if (jpeg_width > 0 && jpeg_height > 0 &&
            !sfos_camera2_has_output_size(
                characteristics, AIMAGE_FORMAT_JPEG, jpeg_width,
                jpeg_height)) {
        context.jpeg_width = 0;
        context.jpeg_height = 0;
    }
    // Validate against the stream's own format: a YUV burst stream on a
    // camera without RAW output has no RAW16 sizes at all.
    if (raw_width > 0 && raw_height > 0 &&
            !sfos_camera2_has_output_size(
                characteristics,
                context.raw_yuv ? AIMAGE_FORMAT_YUV_420_888 : AIMAGE_FORMAT_RAW16,
                raw_width, raw_height)) {
        fprintf(stderr, "preview %s size %dx%d is not a supported output size\n",
                preview_raw_format_name(&context), raw_width, raw_height);
        snprintf(raw_error, sizeof(raw_error), "unsupported_size");
        context.raw_width = 0;
        context.raw_height = 0;
    }
    const int32_t *vendor_tags = NULL, *vendor_values = NULL;
    bool vendor_force_fps = false;
    const int vendor_count = sfos_camera2_vendor_video_settings(&vendor_tags, &vendor_values,
                                                                &vendor_force_fps);
    if (g_preview_video.width > 0 && g_preview_video.height > 0) {
        // Only the viewfinder and the encoder, the pair every camera records
        // with: no still streams next to them.
        context.video = 1;
        context.jpeg_width = 0;
        context.jpeg_height = 0;
        context.raw_width = 0;
        context.raw_height = 0;
        video.config = (struct recorder_config) {
            .width = g_preview_video.width,
            .height = g_preview_video.height,
            .fps = g_preview_video.fps,
            .bitrate = g_preview_video.bitrate,
            .hevc = g_preview_video.hevc,
            .audio_sample_rate = g_preview_video.audio_sample_rate,
            .audio_channels = g_preview_video.audio_channels,
        };
        video.stabilization_supported = sfos_camera2_metadata_has_u8(
            characteristics, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
            ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON);
        if (!sfos_camera2_has_output_size(characteristics, AIMAGE_FORMAT_PRIVATE,
                                          video.config.width, video.config.height)) {
            video_error = "video_size";
        } else if (!sfos_camera2_video_fps_range(characteristics, video.config.fps,
                                                 video.fps_range) && !vendor_force_fps) {
            video_error = "fps_range";
        } else if (!(video.surface = recorder_surface_create())) {
            video_error = "persistent_surface";
        } else {
            // The first encoder before the session: the camera sees the
            // surface's size and format when it configures the stream.
            preview_video_prepare_ladder(&video);
            if (!video.recorder && video.config.hevc) {
                // No HEVC encoder: record H.264 rather than nothing.
                fprintf(stderr, "record-prepare hevc unavailable; using avc\n");
                video.config.hevc = 0;
                preview_video_prepare_ladder(&video);
            }
            if (!video.recorder && preview_video_fall_back_to_1080p(&video, characteristics))
                preview_video_prepare(&video);
            if (!video.recorder) video_error = "encoder";
        }
    }
		    preview_read_metering_info(characteristics, &context);
    preview_copy_raw_static_metadata(&context.raw_static, characteristics);
    context.focal_length = sfos_camera2_first_float(
        characteristics, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, 0.0f);
    atomic_store_explicit(&context.live_af_state, -1, memory_order_release);
    atomic_store_explicit(&context.live_focus_distance_micros, -1,
                          memory_order_release);
	    context.vendor_awb_value_supported = sfos_camera2_metadata_has_i32(
        characteristics, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        (int32_t)MTK_3A_AWB_VALUE);
    context.awb_off_supported = sfos_camera2_metadata_has_u8(
        characteristics, ACAMERA_CONTROL_AWB_AVAILABLE_MODES,
        ACAMERA_CONTROL_AWB_MODE_OFF);
    context.face_detect_mode =
        sfos_camera2_metadata_has_u8(characteristics, ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
                                     ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE)
            ? ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE
            : sfos_camera2_metadata_has_u8(characteristics, ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
                                           ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL)
            ? ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL : 0;
    context.hdr_scene_supported = sfos_camera2_metadata_has_u8(
        characteristics, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES,
        ACAMERA_CONTROL_SCENE_MODE_HDR);

    media_status_t media_status = AImageReader_new(
        width, height, AIMAGE_FORMAT_YUV_420_888, 4, &reader);
    atomic_store_explicit(&context.status.last_media_status, media_status,
                          memory_order_release);
    if (media_status != AMEDIA_OK || !reader) {
        result_code = PREVIEW_READER_ERROR;
        stage = "reader_create";
        goto cleanup;
    }
    AImageReader_ImageListener listener = {
        .context = &context,
        .onImageAvailable = preview_image_available,
    };
    media_status = AImageReader_setImageListener(reader, &listener);
    atomic_store_explicit(&context.status.last_media_status, media_status,
                          memory_order_release);
    if (media_status == AMEDIA_OK) {
        media_status = AImageReader_getWindow(reader, &window);
        atomic_store_explicit(&context.status.last_media_status, media_status,
                              memory_order_release);
    }
    if (media_status != AMEDIA_OK || !window) {
        result_code = PREVIEW_READER_ERROR;
        stage = "reader_configure";
        goto cleanup;
    }

    if (context.jpeg_width > 0 && context.jpeg_height > 0) {
        media_status = AImageReader_new(
            context.jpeg_width, context.jpeg_height, AIMAGE_FORMAT_JPEG, 2,
            &jpeg_reader);
        atomic_store_explicit(&context.status.last_media_status,
                              media_status, memory_order_release);
        if (media_status == AMEDIA_OK && jpeg_reader) {
            AImageReader_ImageListener jpeg_listener = {
                .context = &context,
                .onImageAvailable = preview_jpeg_image_available,
            };
            media_status = AImageReader_setImageListener(jpeg_reader,
                                                         &jpeg_listener);
            if (media_status == AMEDIA_OK) {
                media_status = AImageReader_getWindow(jpeg_reader,
                                                      &jpeg_window);
            }
        }
        atomic_store_explicit(&context.status.last_media_status,
                              media_status, memory_order_release);
        if (media_status != AMEDIA_OK || !jpeg_window) {
            context.jpeg_width = 0;
            context.jpeg_height = 0;
        } else {
            atomic_store_explicit(&context.jpeg_status, 1,
                                  memory_order_release);
        }
    }

    if (context.raw_width > 0 && context.raw_height > 0) {
        media_status = preview_create_raw_reader(&context, &raw_reader, &raw_window);
        if (media_status != AMEDIA_OK && context.raw_yuv) {
            fprintf(stderr, "preview YUV reader %dx%d failed (media status %d); trying JPEG\n",
                    context.raw_width, context.raw_height, media_status);
            snprintf(raw_error, sizeof(raw_error), "yuv_reader:%d", media_status);
            context.raw_jpeg = 1;
            media_status = preview_create_raw_reader(&context, &raw_reader, &raw_window);
        }
        atomic_store_explicit(&context.status.last_media_status,
                              media_status, memory_order_release);
        if (media_status != AMEDIA_OK) {
            fprintf(stderr, "preview %s reader %dx%d failed (media status %d)\n",
                    preview_raw_format_name(&context), context.raw_width,
                    context.raw_height, media_status);
            snprintf(raw_error, sizeof(raw_error), "reader:%d", media_status);
            context.raw_width = 0;
            context.raw_height = 0;
        } else {
            atomic_store_explicit(&context.raw_status, 1,
                                  memory_order_release);
        }
    }

    ACameraDevice_StateCallbacks device_callbacks = {
        .context = &context,
        .onDisconnected = preview_device_disconnected,
        .onError = preview_device_error,
    };
    const int64_t open_started_ms = sfos_camera2_now_ms();
    fprintf(stderr, "timing bridge before-open ms=%lld\n",
            (long long)(open_started_ms - context.started_ms));
    camera_status = ACameraManager_openCamera(
        manager, camera_id, &device_callbacks, &device);
    fprintf(stderr, "timing bridge camera-open ms=%lld\n",
            (long long)(sfos_camera2_now_ms() - context.started_ms));
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !device) {
        result_code = PREVIEW_OPEN_ERROR;
        stage = "camera_open";
        goto cleanup;
    }

    if (ACameraOutputTarget_create(window, &target) != ACAMERA_OK ||
            ACameraDevice_createCaptureRequest(device, context.video ? TEMPLATE_RECORD : TEMPLATE_PREVIEW,
                                               &request) != ACAMERA_OK ||
            ACaptureRequest_addTarget(request, target) != ACAMERA_OK ||
            ACameraDevice_createCaptureRequest(device, context.video ? TEMPLATE_RECORD : TEMPLATE_PREVIEW,
                                               &focus_request) != ACAMERA_OK ||
            ACaptureRequest_addTarget(focus_request, target) != ACAMERA_OK ||
            ACaptureSessionOutput_create(window, &output) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_create(&container) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_add(container, output) !=
                ACAMERA_OK) {
        result_code = PREVIEW_CONFIGURATION_ERROR;
        stage = "configure";
        goto cleanup;
    }
    if (video.recorder &&
            (ACameraOutputTarget_create(video.surface, &video.target) != ACAMERA_OK ||
             ACaptureSessionOutput_create(video.surface, &video.output) != ACAMERA_OK ||
             ACaptureSessionOutputContainer_add(container, video.output) != ACAMERA_OK)) {
        video_error = "video_configure";
        if (video.output) ACaptureSessionOutput_free(video.output);
        if (video.target) ACameraOutputTarget_free(video.target);
        video.output = NULL;
        video.target = NULL;
    }
    if (jpeg_window &&
            (ACameraOutputTarget_create(jpeg_window, &jpeg_target) !=
                 ACAMERA_OK ||
             ACaptureSessionOutput_create(jpeg_window, &jpeg_output) !=
                 ACAMERA_OK ||
             ACaptureSessionOutputContainer_add(container, jpeg_output) !=
                 ACAMERA_OK ||
             ACameraDevice_createCaptureRequest(device, TEMPLATE_STILL_CAPTURE,
                                                &jpeg_request) != ACAMERA_OK ||
             ACaptureRequest_addTarget(jpeg_request, jpeg_target) !=
                 ACAMERA_OK)) {
        result_code = PREVIEW_CONFIGURATION_ERROR;
        stage = "jpeg_configure";
        goto cleanup;
    }
    if (raw_window &&
            (ACameraOutputTarget_create(raw_window, &raw_target) !=
                 ACAMERA_OK ||
             ACaptureSessionOutput_create(raw_window, &raw_output) !=
                 ACAMERA_OK ||
             ACaptureSessionOutputContainer_add(container, raw_output) !=
                 ACAMERA_OK ||
             ACameraDevice_createCaptureRequest(device, TEMPLATE_STILL_CAPTURE,
                                                &raw_request) != ACAMERA_OK ||
             ACaptureRequest_addTarget(raw_request, raw_target) !=
                 ACAMERA_OK)) {
        result_code = PREVIEW_CONFIGURATION_ERROR;
        stage = "raw_configure";
        goto cleanup;
    }
    // Diagnostics from the command line (the app sets its own mode with
    // the settings command): the viewfinder's noise reduction mode.
    const char *noise_reduction_override = getenv("PROCAM_PREVIEW_NR");
    if (noise_reduction_override && *noise_reduction_override) {
        context.noise_reduction = atoi(noise_reduction_override);
    }
    fprintf(stderr, "timing bridge preview-template nr=%d edge=%d eis=%d tnr_request=%d\n",
            preview_request_u8(request, ACAMERA_NOISE_REDUCTION_MODE),
            preview_request_u8(request, ACAMERA_EDGE_MODE),
            preview_request_u8(request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE),
            context.noise_reduction);
    preview_configure_request(request, &context, characteristics, 0);
    preview_configure_request(focus_request, &context, characteristics, 0);
    preview_configure_focus_request(focus_request, &context, context.focus_x,
                                    context.focus_y, 1);
    if (context.video && vendor_force_fps && video.fps_range[1] < video.config.fps) {
        video.fps_range[0] = video.fps_range[1] = video.config.fps;
    }
    preview_video_configure_request(request, &video);
    preview_video_configure_request(focus_request, &video);
    for (int i = 0; context.video && i < vendor_count; ++i) {
        const camera_status_t set = ACaptureRequest_setEntry_i32(
            request, (uint32_t)vendor_tags[i], 1, &vendor_values[i]);
        ACaptureRequest_setEntry_i32(focus_request, (uint32_t)vendor_tags[i], 1, &vendor_values[i]);
        fprintf(stderr, "vendor setting 0x%08x=%d status=%d\n", (uint32_t)vendor_tags[i],
                vendor_values[i], set);
    }
    if (context.video) {
        fprintf(stderr, "video-template nr=%d edge=%d\n",
                preview_request_u8(request, ACAMERA_NOISE_REDUCTION_MODE),
                preview_request_u8(request, ACAMERA_EDGE_MODE));
    }
    if (jpeg_request) {
        preview_configure_request(jpeg_request, &context, characteristics, 1);
    }
    if (raw_request) {
        preview_configure_request(raw_request, &context, characteristics, 1);
    }

    ACameraCaptureSession_stateCallbacks session_callbacks = {
        .context = &context,
        .onClosed = NULL,
        .onReady = NULL,
        .onActive = NULL,
    };
    ACameraCaptureSession_captureCallbacks preview_callbacks = {
        .context = &context,
        .onCaptureStarted = NULL,
        .onCaptureProgressed = NULL,
        .onCaptureCompleted = preview_capture_completed,
        .onCaptureFailed = NULL,
        .onCaptureSequenceCompleted = preview_sequence_ended,
        .onCaptureSequenceAborted = preview_sequence_aborted,
        .onCaptureBufferLost = NULL,
    };
    typedef camera_status_t (*create_with_parameters_fn)(
        ACameraDevice *, const ACaptureSessionOutputContainer *, const ACaptureRequest *,
        const ACameraCaptureSession_stateCallbacks *, ACameraCaptureSession **);
    create_with_parameters_fn create_with_parameters = (create_with_parameters_fn)dlsym(
        RTLD_DEFAULT, "ACameraDevice_createCaptureSessionWithSessionParameters");
    if (context.video && vendor_count > 0 && create_with_parameters) {
        // Diagnostics: the request (with the vendor settings) doubles as the
        // session parameters.
        camera_status = create_with_parameters(device, container, request,
                                               &session_callbacks, &session);
        fprintf(stderr, "session with vendor parameters status=%d\n", camera_status);
    } else {
        camera_status = ACameraDevice_createCaptureSession(
            device, container, &session_callbacks, &session);
    }
    if ((camera_status != ACAMERA_OK || !session) && raw_output &&
            context.raw_yuv && !context.raw_jpeg) {
        // A second YUV stream refused: the same frames as JPEG, the stream
        // combination warm JPEG capture already uses.
        fprintf(stderr, "preview session with YUV %dx%d failed (status %d); trying JPEG\n",
                context.raw_width, context.raw_height, camera_status);
        snprintf(raw_error, sizeof(raw_error), "yuv_session:%d", camera_status);
        raw_fallback_tried = true;
        ACaptureSessionOutputContainer_remove(container, raw_output);
        ACaptureSessionOutput_free(raw_output);
        raw_output = NULL;
        ACameraOutputTarget_free(raw_target);
        raw_target = NULL;
        ACaptureRequest_free(raw_request);
        raw_request = NULL;
        AImageReader_delete(raw_reader);
        raw_reader = NULL;
        raw_window = NULL;
        session = NULL;
        context.raw_jpeg = 1;
        if (preview_create_raw_reader(&context, &raw_reader, &raw_window) == AMEDIA_OK &&
                ACameraOutputTarget_create(raw_window, &raw_target) == ACAMERA_OK &&
                ACaptureSessionOutput_create(raw_window, &raw_output) == ACAMERA_OK &&
                ACaptureSessionOutputContainer_add(container, raw_output) == ACAMERA_OK &&
                ACameraDevice_createCaptureRequest(device, TEMPLATE_STILL_CAPTURE,
                                                   &raw_request) == ACAMERA_OK &&
                ACaptureRequest_addTarget(raw_request, raw_target) == ACAMERA_OK) {
            preview_configure_request(raw_request, &context, characteristics, 1);
            camera_status = ACameraDevice_createCaptureSession(
                device, container, &session_callbacks, &session);
        } else {
            camera_status = ACAMERA_ERROR_UNKNOWN;
        }
    }
    if ((camera_status != ACAMERA_OK || !session) && (raw_output || raw_fallback_tried)) {
        // Not every HAL streams RAW next to the preview at every size: keep
        // the viewfinder and let RAW captures use the cold path instead.
        fprintf(stderr, "preview session with %s %dx%d failed (status %d); retrying without it\n",
                preview_raw_format_name(&context), context.raw_width, context.raw_height,
                camera_status);
        if (raw_fallback_tried) {
            // Keep the first (YUV) reason, add the JPEG one.
            size_t used = strlen(raw_error);
            snprintf(raw_error + used, sizeof(raw_error) - used, ",jpeg:%d", camera_status);
        } else {
            snprintf(raw_error, sizeof(raw_error), "session:%d", camera_status);
        }
        if (raw_output) {
            ACaptureSessionOutputContainer_remove(container, raw_output);
            ACaptureSessionOutput_free(raw_output);
        }
        raw_output = NULL;
        if (raw_target) {
            ACameraOutputTarget_free(raw_target);
        }
        raw_target = NULL;
        if (raw_request) {
            ACaptureRequest_free(raw_request);
        }
        raw_request = NULL;
        context.raw_width = 0;
        context.raw_height = 0;
        session = NULL;
        camera_status = ACameraDevice_createCaptureSession(
            device, container, &session_callbacks, &session);
    }
    if ((camera_status != ACAMERA_OK || !session) && video.output &&
            preview_video_fall_back_to_1080p(&video, characteristics)) {
        // 4K refused next to the viewfinder: the same surface at 1080p.
        fprintf(stderr, "preview session with video failed (status %d); trying 1920x1080\n",
                camera_status);
        recorder_destroy(video.recorder);
        video.recorder = NULL;
        preview_video_prepare(&video);
        session = NULL;
        camera_status = video.recorder
                ? ACameraDevice_createCaptureSession(device, container, &session_callbacks,
                                                     &session)
                : ACAMERA_ERROR_UNKNOWN;
    }
    if ((camera_status != ACAMERA_OK || !session) && video.output) {
        // The camera refused the encoder next to the viewfinder: keep the
        // viewfinder; recording reports the stream as missing.
        fprintf(stderr, "preview session with video %dx%d failed (status %d); retrying without it\n",
                video.config.width, video.config.height, camera_status);
        video_error = "session";
        ACaptureSessionOutputContainer_remove(container, video.output);
        ACaptureSessionOutput_free(video.output);
        video.output = NULL;
        ACameraOutputTarget_free(video.target);
        video.target = NULL;
        session = NULL;
        camera_status = ACameraDevice_createCaptureSession(
            device, container, &session_callbacks, &session);
    }
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK || !session) {
        result_code = PREVIEW_CONFIGURATION_ERROR;
        stage = "session_create";
        goto cleanup;
    }

    fprintf(stderr, "timing bridge session ms=%lld\n",
            (long long)(sfos_camera2_now_ms() - context.started_ms));
    preview_write_stream_status(&context, raw_target && raw_request,
                                jpeg_target && jpeg_request, raw_error,
                                requested_raw_width, requested_raw_height);
    if (context.video) {
        char payload[192];
        int size = snprintf(payload, sizeof(payload),
                            "video-stream=%d size=%dx%d fps=%d codec=%s stabilization=%d error=%s\n",
                            video.target ? 1 : 0, video.config.width, video.config.height,
                            video.config.fps, video.config.hevc ? "hevc" : "avc",
                            video.stabilization_supported && g_preview_video.stabilization,
                            video_error);
        if (size > 0 && size < (int)sizeof(payload)) preview_write_message(&context, payload, size);
        fprintf(stderr, "%s", payload);
    }

    ACaptureRequest *requests[] = { request };
    camera_status = ACameraCaptureSession_setRepeatingRequest(
        session, &preview_callbacks, 1, requests, &context.repeat_sequence);
    atomic_store_explicit(&context.status.last_camera_status, camera_status,
                          memory_order_release);
    if (camera_status != ACAMERA_OK) {
        result_code = PREVIEW_SUBMIT_ERROR;
        stage = "repeat_submit";
        goto cleanup;
    }
    if (focus_x >= 0.0f && focus_y >= 0.0f) {
        ACaptureRequest *focus_requests[] = { focus_request };
        ACameraCaptureSession_capture(session, NULL, 1, focus_requests, NULL);
    }
    if (control_fd >= 0) {
        fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL, 0) | O_NONBLOCK);
    }

    struct preview_command_buffer command_buffer;
    memset(&command_buffer, 0, sizeof(command_buffer));
    int64_t deadline = sfos_camera2_now_ms() + timeout_ms;
    while (sfos_camera2_now_ms() < deadline &&
            atomic_load_explicit(&context.status.device_error,
                                 memory_order_acquire) == 0 &&
            atomic_load_explicit(&context.status.image_status,
                                 memory_order_acquire) >= 0 && !quit) {
        struct preview_command command;
        preview_read_command_data(control_fd, &command_buffer);
        while (preview_pop_command(&command_buffer, &command)) {
            if (command.type == PREVIEW_COMMAND_CAPTURE_INVALID) {
                fprintf(stderr, "capture-command rejected reason=invalid-arguments\n");
                preview_write_capture_result(&context, "command-error", "-", PREVIEW_INVALID_ARGUMENT);
                continue;
            }
            if (command.type == PREVIEW_COMMAND_CAPTURE_RAW ||
                    command.type == PREVIEW_COMMAND_CAPTURE_JPEG ||
                    command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET) {
                fprintf(stderr, "capture-command received type=%s frames=%d\n",
                    command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET ? "raw-bracket" :
                    command.type == PREVIEW_COMMAND_CAPTURE_RAW ? "raw" : "jpeg",
                    command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET ? command.bracket_count : 1);
                if ((command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET && !raw_target) ||
                        (command.type == PREVIEW_COMMAND_CAPTURE_RAW && (!raw_target || !raw_request)) ||
                        (command.type == PREVIEW_COMMAND_CAPTURE_JPEG && !jpeg_request)) {
                    preview_write_capture_result(&context, "command-error", "-", PREVIEW_CONFIGURATION_ERROR);
                    continue;
                }
            }
            if (command.type == PREVIEW_COMMAND_QUIT) {
                quit = true;
                break;
            }
            if (command.type == PREVIEW_COMMAND_RESUME) {
                atomic_store_explicit(&context.paused, 0, memory_order_release);
                fprintf(stderr, "timing bridge resumed ms=%lld\n",
                        (long long)(sfos_camera2_now_ms() - context.started_ms));
                continue;
            }
            if (command.type == PREVIEW_COMMAND_RECORD_START) {
                preview_video_start(&video, &context, session, &preview_callbacks,
                                    request, focus_request, &command);
                continue;
            }
            if (command.type == PREVIEW_COMMAND_RECORD_STOP) {
                preview_video_stop(&video, &context, session, &preview_callbacks,
                                   request, focus_request);
                continue;
            }
            if (command.type == PREVIEW_COMMAND_RECORD_INVALID) {
                fprintf(stderr, "record-command rejected reason=invalid-arguments\n");
                preview_write_record_status(&context, "error", PREVIEW_INVALID_ARGUMENT, "-");
                continue;
            }
            if (command.type == PREVIEW_COMMAND_FOCUS) {
                context.focus_hold_active = 0;
                context.focus_x = command.focus_x;
                context.focus_y = command.focus_y;
                preview_configure_request(request, &context, characteristics,
                                          0);
                preview_configure_request(focus_request, &context,
                                          characteristics, 0);
                preview_configure_focus_request(
                    focus_request, &context, context.focus_x, context.focus_y,
                    1);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
                ACaptureRequest *focus_requests[] = { focus_request };
                ACameraCaptureSession_capture(session, NULL, 1, focus_requests,
                                              NULL);
            } else if (command.type == PREVIEW_COMMAND_ZOOM) {
                context.zoom_ratio = command.zoom_ratio;
                preview_configure_request(request, &context, characteristics,
                                          0);
                preview_configure_request(focus_request, &context,
                                          characteristics, 0);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
            } else if (command.type == PREVIEW_COMMAND_SETTINGS) {
                context.focus_mode = command.focus_mode;
                context.focus_distance = command.focus_distance;
                context.exposure_compensation = command.exposure_compensation;
                context.original_scene_mode = command.scene_mode;
                context.scene_mode_supported =
                    sfos_camera2_scene_mode_supported(characteristics,
                                                      command.scene_mode);
                context.scene_mode = context.scene_mode_supported
                    ? command.scene_mode : SFOS_CAMERA2_SCENE_NONE;
                context.color_temperature_kelvin =
                    command.color_temperature_kelvin;
                context.color_tint = command.color_tint;
                context.sensor_sensitivity = command.sensor_sensitivity;
                context.exposure_time_ns = command.exposure_time_ns;
                context.aperture = command.aperture;
                context.noise_reduction = command.noise_reduction;
                context.zoom_ratio = command.zoom_ratio;
                preview_configure_request(request, &context, characteristics,
                                          0);
                preview_configure_request(focus_request, &context,
                                          characteristics, 0);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
            } else if (command.type == PREVIEW_COMMAND_EXPOSURE_SETTINGS) {
                context.sensor_sensitivity = command.sensor_sensitivity;
                context.exposure_time_ns = command.exposure_time_ns;
                context.aperture = command.aperture;
                context.noise_reduction = command.noise_reduction;
                context.zoom_ratio = command.zoom_ratio;
                preview_configure_request(request, &context, characteristics,
                                          0);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
            } else if (command.type == PREVIEW_COMMAND_FOCUS_HOLD) {
                int focus_distance_micros = atomic_load_explicit(
                    &context.live_focus_distance_micros,
                    memory_order_acquire);
                if (focus_distance_micros >= 0) {
                    context.focus_hold_distance =
                        (float)focus_distance_micros / 1000000.0f;
                    context.focus_hold_active = 1;
                    fprintf(stderr,
                            "capture-focus hold distance=%.6f af_state=%d\n",
                            context.focus_hold_distance,
                            atomic_load_explicit(&context.live_af_state,
                                                 memory_order_acquire));
                } else {
                    context.focus_hold_active = 0;
                    fprintf(stderr,
                            "capture-focus hold skipped: no lens distance\n");
                }
                preview_configure_request(request, &context, characteristics,
                                          0);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
            } else if (command.type == PREVIEW_COMMAND_FOCUS_HOLD_RELEASE) {
                context.focus_hold_active = 0;
                preview_configure_request(request, &context, characteristics,
                                          0);
                if (jpeg_request) {
                    preview_configure_request(jpeg_request, &context,
                                              characteristics, 1);
                }
                if (raw_request) {
                    preview_configure_request(raw_request, &context,
                                              characteristics, 1);
                }
                ACaptureRequest *requests[] = { request };
                ACameraCaptureSession_setRepeatingRequest(
                    session, &preview_callbacks, 1, requests, &context.repeat_sequence);
            } else if (command.type == PREVIEW_COMMAND_CAPTURE_JPEG &&
                    jpeg_request) {
                snprintf(context.jpeg_path, sizeof(context.jpeg_path), "%s",
                         command.path);
                context.jpeg_data_length = 0;
                context.jpeg_command_ms = sfos_camera2_now_ms();
                context.jpeg_submit_ms = 0;
                context.jpeg_result_ms = 0;
                context.jpeg_available_ms = 0;
                context.jpeg_written_ms = 0;
                context.jpeg_hal_exposure_time_ns = -1;
                context.jpeg_hal_frame_duration_ns = -1;
                context.jpeg_hal_sensitivity = -1;
                context.jpeg_effective_logged = 0;
                fprintf(stderr, "capture-timing bridge warm-jpeg command\n");
                fprintf(stderr,
                        "capture-exposure warm-jpeg submit requested_iso=%d "
                        "requested_shutter=%lld\n",
                        context.sensor_sensitivity,
                        (long long)context.exposure_time_ns);
                atomic_store_explicit(&context.jpeg_status, 0,
                                      memory_order_release);
                ACameraCaptureSession_captureCallbacks jpeg_callbacks = {
                    .context = &context,
                    .onCaptureStarted = NULL,
                    .onCaptureProgressed = NULL,
                    .onCaptureCompleted = preview_jpeg_capture_completed,
                    .onCaptureFailed = NULL,
                    .onCaptureSequenceCompleted = NULL,
                    .onCaptureSequenceAborted = NULL,
                    .onCaptureBufferLost = NULL,
                };
                ACaptureRequest *jpeg_requests[] = { jpeg_request };
                camera_status_t jpeg_status = ACameraCaptureSession_capture(
                    session, &jpeg_callbacks, 1, jpeg_requests, NULL);
                atomic_store_explicit(&context.status.last_camera_status,
                                      jpeg_status, memory_order_release);
                if (jpeg_status == ACAMERA_OK) {
                    context.jpeg_submit_ms = sfos_camera2_now_ms();
                    fprintf(stderr,
                            "capture-timing bridge warm-jpeg submitted t=%lld\n",
                            (long long)(context.jpeg_submit_ms -
                                        context.jpeg_command_ms));
                } else {
                    atomic_store_explicit(&context.jpeg_status, -1,
                                          memory_order_release);
                    preview_write_capture_result(&context, "error",
                                                 context.jpeg_path,
                                                 jpeg_status);
                    fprintf(stderr,
                            "capture-timing bridge warm-jpeg submit-failed status=%d\n",
                            jpeg_status);
                }
            } else if (command.type == PREVIEW_COMMAND_CAPTURE_RAW &&
                    raw_request) {
                atomic_fetch_add_explicit(&context.raw_bracket_generation, 1, memory_order_acq_rel);
                context.raw_bracket_count = 0;
                snprintf(context.raw_path, sizeof(context.raw_path), "%s",
                         command.path);
                snprintf(context.raw_metadata_path,
                         sizeof(context.raw_metadata_path), "%s",
                         command.metadata_path);
                memset(&context.raw_image, 0, sizeof(context.raw_image));
                memset(&context.raw_result, 0, sizeof(context.raw_result));
                context.raw_command_ms = sfos_camera2_now_ms();
                context.raw_submit_ms = 0;
                context.raw_available_ms = 0;
                context.raw_written_ms = 0;
                context.raw_metadata_written_ms = 0;
                atomic_store_explicit(&context.raw_status, 0,
                                      memory_order_release);
                atomic_store_explicit(&context.raw_result_status, 0,
                                      memory_order_release);
                atomic_store_explicit(&context.raw_sequence_status, 0,
                                      memory_order_release);
                ACaptureRequest *raw_requests[] = { raw_request };
                ACameraCaptureSession_captureCallbacks raw_callbacks = {
                    .context = &context,
                    .onCaptureStarted = preview_raw_capture_started,
                    .onCaptureProgressed = NULL,
                    .onCaptureCompleted = preview_raw_capture_completed,
                    .onCaptureFailed = NULL,
                    .onCaptureSequenceCompleted = NULL,
                    .onCaptureSequenceAborted = NULL,
                    .onCaptureBufferLost = NULL,
                };
                const bool focus_locked = preview_lock_focus(
                    &context, session, request, characteristics, &preview_callbacks);
                camera_status_t raw_status = ACameraCaptureSession_capture(
                    session, &raw_callbacks, 1, raw_requests, NULL);
                if (focus_locked) {
                    preview_unlock_focus(session, request, &preview_callbacks);
                }
                atomic_store_explicit(&context.status.last_camera_status,
                                      raw_status, memory_order_release);
                if (raw_status == ACAMERA_OK) {
                    context.raw_submit_ms = sfos_camera2_now_ms();
                } else {
                    atomic_store_explicit(&context.raw_status, -1,
                                          memory_order_release);
                    preview_write_capture_result(&context, "error",
                                                 context.raw_metadata_path,
                                                 raw_status);
                }
            } else if (command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET &&
                    raw_target && command.bracket_count >= 2 &&
                    command.bracket_count <= PREVIEW_MAX_RAW_BRACKET) {
                for (int index = 0; index < PREVIEW_MAX_RAW_BRACKET; ++index) {
                    if (context.raw_bracket_requests[index]) {
                        ACaptureRequest_free(
                            context.raw_bracket_requests[index]);
                        context.raw_bracket_requests[index] = NULL;
                    }
                }
                const int generation = atomic_fetch_add_explicit(
                    &context.raw_bracket_generation, 1, memory_order_acq_rel) + 1;
                context.raw_bracket_count = command.bracket_count;
                context.raw_command_ms = sfos_camera2_now_ms();
                context.raw_submit_ms = 0;
                context.raw_available_ms = 0;
                context.raw_written_ms = 0;
                context.raw_metadata_written_ms = 0;
                bool request_error = false;
                for (int index = 0; index < command.bracket_count; ++index) {
                    snprintf(context.raw_bracket_paths[index],
                             sizeof(context.raw_bracket_paths[index]), "%s",
                             command.bracket_paths[index]);
                    snprintf(context.raw_bracket_metadata_paths[index],
                             sizeof(context.raw_bracket_metadata_paths[index]),
                             "%s", command.bracket_metadata_paths[index]);
                    context.raw_bracket_sensor_sensitivity[index] =
                        command.bracket_sensor_sensitivity[index];
                    context.raw_bracket_exposure_time_ns[index] =
                        command.bracket_exposure_time_ns[index];
                    memset(&context.raw_bracket_image[index], 0,
                           sizeof(context.raw_bracket_image[index]));
                    memset(&context.raw_bracket_result[index], 0,
                           sizeof(context.raw_bracket_result[index]));
                    atomic_store_explicit(
                        &context.raw_bracket_status[index], 0,
                        memory_order_release);
                    atomic_store_explicit(
                        &context.raw_bracket_result_status[index], 0,
                        memory_order_release);
                    camera_status_t create_status =
                        ACameraDevice_createCaptureRequest(
                            device, TEMPLATE_STILL_CAPTURE,
                            &context.raw_bracket_requests[index]);
                    if (create_status != ACAMERA_OK ||
                            !context.raw_bracket_requests[index] ||
                            ACaptureRequest_addTarget(
                                context.raw_bracket_requests[index],
                                raw_target) != ACAMERA_OK) {
                        request_error = true;
                        atomic_store_explicit(
                            &context.raw_bracket_status[index], -1,
                            memory_order_release);
                        preview_write_capture_result(
                            &context, "error",
                            context.raw_bracket_metadata_paths[index],
                            create_status);
                        break;
                    }
                    preview_configure_raw_bracket_request(
                        context.raw_bracket_requests[index], &context,
                        characteristics,
                        context.raw_bracket_sensor_sensitivity[index],
                        context.raw_bracket_exposure_time_ns[index]);
                    ACameraMetadata_const_entry applied_shutter, applied_iso;
                    if (ACaptureRequest_getConstEntry(context.raw_bracket_requests[index],
                            ACAMERA_SENSOR_EXPOSURE_TIME, &applied_shutter) == ACAMERA_OK && applied_shutter.count)
                        context.raw_bracket_exposure_time_ns[index] = applied_shutter.data.i64[0];
                    if (ACaptureRequest_getConstEntry(context.raw_bracket_requests[index],
                            ACAMERA_SENSOR_SENSITIVITY, &applied_iso) == ACAMERA_OK && applied_iso.count)
                        context.raw_bracket_sensor_sensitivity[index] = applied_iso.data.i32[0];
                    fprintf(stderr,
                            "capture-exposure warm-raw-bracket index=%d "
                            "requested_iso=%d requested_shutter=%lld\n",
                            index,
                            context.raw_bracket_sensor_sensitivity[index],
                            (long long)
                                context.raw_bracket_exposure_time_ns[index]);
                }
                if (!request_error) {
                    struct preview_bracket_capture *capture = calloc(1, sizeof(*capture));
                    if (!capture) {
                        preview_write_capture_result(&context, "error", context.raw_bracket_metadata_paths[0],
                                                     PREVIEW_WRITE_ERROR);
                        context.raw_bracket_count = 0;
                        continue;
                    }
                    capture->owner = &context;
                    capture->generation = generation;
                    ACameraCaptureSession_captureCallbacks raw_callbacks = {
                        .context = capture,
                        .onCaptureStarted = preview_bracket_started,
                        .onCaptureProgressed = NULL,
                        .onCaptureCompleted = preview_bracket_completed,
                        .onCaptureFailed = preview_bracket_failed,
                        .onCaptureSequenceCompleted = preview_bracket_sequence_end,
                        .onCaptureSequenceAborted = preview_bracket_sequence_abort,
                        .onCaptureBufferLost = NULL,
                    };
                    ACaptureRequest *requests[PREVIEW_MAX_RAW_BRACKET] = { NULL };
                    for (int index = 0; index < command.bracket_count;
                            ++index) {
                        requests[index] = context.raw_bracket_requests[index];
                    }
                    const bool focus_locked = preview_lock_focus(
                        &context, session, request, characteristics, &preview_callbacks);
                    camera_status_t raw_status =
                        ACameraCaptureSession_capture(
                            session, &raw_callbacks, command.bracket_count,
                            requests, NULL);
                    if (focus_locked) {
                        preview_unlock_focus(session, request, &preview_callbacks);
                    }
                    atomic_store_explicit(
                        &context.status.last_camera_status, raw_status,
                        memory_order_release);
                    fprintf(stderr, "capture-command submitted type=raw-bracket frames=%d generation=%d status=%d\n",
                            command.bracket_count, generation, raw_status);
                    if (raw_status == ACAMERA_OK) {
                        context.raw_submit_ms = sfos_camera2_now_ms();
                    } else {
                        free(capture);
                        for (int index = 0; index < command.bracket_count;
                                ++index) {
                            atomic_store_explicit(
                                &context.raw_bracket_status[index], -1,
                                memory_order_release);
                            preview_write_capture_result(
                                &context, "error",
                                context.raw_bracket_metadata_paths[index],
                                raw_status);
                        }
                        context.raw_bracket_count = 0;
                    }
                } else {
                    context.raw_bracket_count = 0;
                }
            }
        }
        preview_video_step(&video, &context, session, &preview_callbacks, request,
                           focus_request);
        sfos_camera2_sleep_10_ms();
    }
    if (atomic_load_explicit(&context.status.image_status,
                             memory_order_acquire) < 0) {
        result_code = PREVIEW_WRITE_ERROR;
        stage = "frame_write";
        goto cleanup;
    }
    if (atomic_load_explicit(&context.frames_written,
                             memory_order_acquire) <= 0) {
        result_code = PREVIEW_TIMEOUT;
        stage = "preview_wait";
        goto cleanup;
    }

cleanup:
    // A recording still running when the preview ends (quit, timeout, camera
    // error) is finished, not lost.
    video.closing = true;
    if (video.state == PREVIEW_VIDEO_RECORDING) {
        preview_video_stop(&video, &context, session, &preview_callbacks, request,
                           focus_request);
    }
    while (video.state == PREVIEW_VIDEO_STOPPING) {
        preview_video_step(&video, &context, session, &preview_callbacks, request,
                           focus_request);
        sfos_camera2_sleep_10_ms();
    }
    preview_status_json(out, out_size, result_code == 0, stage, result_code,
                        &context);
    if (session) {
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
    }
    recorder_destroy(video.recorder);
    if (video.target) {
        ACameraOutputTarget_free(video.target);
    }
    if (video.output) {
        ACaptureSessionOutput_free(video.output);
    }
    if (target) {
        ACameraOutputTarget_free(target);
    }
    if (jpeg_target) {
        ACameraOutputTarget_free(jpeg_target);
    }
    if (raw_target) {
        ACameraOutputTarget_free(raw_target);
    }
    if (output) {
        ACaptureSessionOutput_free(output);
    }
    if (jpeg_output) {
        ACaptureSessionOutput_free(jpeg_output);
    }
    if (raw_output) {
        ACaptureSessionOutput_free(raw_output);
    }
    if (container) {
        ACaptureSessionOutputContainer_free(container);
    }
    if (request) {
        ACaptureRequest_free(request);
    }
    if (focus_request) {
        ACaptureRequest_free(focus_request);
    }
    if (jpeg_request) {
        ACaptureRequest_free(jpeg_request);
    }
    if (raw_request) {
        ACaptureRequest_free(raw_request);
    }
    for (int index = 0; index < PREVIEW_MAX_RAW_BRACKET; ++index) {
        if (context.raw_bracket_requests[index]) {
            ACaptureRequest_free(context.raw_bracket_requests[index]);
        }
    }
    if (reader) {
        AImageReader_setImageListener(reader, NULL);
        AImageReader_delete(reader);
    }
    if (jpeg_reader) {
        AImageReader_setImageListener(jpeg_reader, NULL);
        AImageReader_delete(jpeg_reader);
    }
    if (raw_reader) {
        AImageReader_setImageListener(raw_reader, NULL);
        AImageReader_delete(raw_reader);
    }
    if (device) {
        ACameraDevice_close(device);
    }
    if (characteristics) {
        ACameraMetadata_free(characteristics);
    }
    if (manager) {
        ACameraManager_delete(manager);
    }
    recorder_surface_release(video.surface);
    if (output_lock_ready) {
        pthread_mutex_destroy(&context.output_lock);
    }
    return result_code;
}
