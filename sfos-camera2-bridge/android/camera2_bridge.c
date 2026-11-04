/**
 * @file camera2_bridge.c
 * @brief Camera2 capability probe for the Sailfish Camera2 bridge.
 *
 * Runs inside the Android/libhybris side of the bridge. The probe enumerates
 * Camera2 devices, extracts static metadata, and returns a compact JSON
 * description that the Sailfish-side helper can print for the QML application.
 */
#include "camera2_bridge.h"
#include "camera2_common.h"

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct json_writer {
    char *data;
    size_t capacity;
    size_t length;
    bool truncated;
};

static void json_appendf(struct json_writer *writer, const char *format, ...)
{
    if (writer->truncated || writer->length >= writer->capacity) {
        writer->truncated = true;
        return;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(writer->data + writer->length,
                            writer->capacity - writer->length,
                            format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= writer->capacity - writer->length) {
        writer->length = writer->capacity - 1;
        writer->data[writer->length] = '\0';
        writer->truncated = true;
        return;
    }

    writer->length += (size_t)written;
}

static void json_string(struct json_writer *writer, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    json_appendf(writer, "\"");
    while (*cursor && !writer->truncated) {
        switch (*cursor) {
        case '\"': json_appendf(writer, "\\\""); break;
        case '\\': json_appendf(writer, "\\\\"); break;
        case '\b': json_appendf(writer, "\\b"); break;
        case '\f': json_appendf(writer, "\\f"); break;
        case '\n': json_appendf(writer, "\\n"); break;
        case '\r': json_appendf(writer, "\\r"); break;
        case '\t': json_appendf(writer, "\\t"); break;
        default:
            if (*cursor < 0x20) {
                json_appendf(writer, "\\u%04x", (unsigned int)*cursor);
            } else {
                json_appendf(writer, "%c", *cursor);
            }
        }
        ++cursor;
    }
    json_appendf(writer, "\"");
}

/* https://developer.android.com/ndk/reference/group/camera#acamera_metadata_enum_acamera_info_supported_hardware_level */

static const char *hardware_level_name(int32_t level)
{
    switch (level) {
    case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY: return "legacy";
    case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED: return "limited";
    case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_FULL: return "full";
    case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_3: return "level_3";
#ifdef ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL
    case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL: return "external";
#endif
    default: return "unknown";
    }
}

static const char *stream_format_name(int32_t format)
{
    switch (format) {
    case AIMAGE_FORMAT_RAW16: return "RAW16";
    case AIMAGE_FORMAT_RAW10: return "RAW10";
    case AIMAGE_FORMAT_RAW12: return "RAW12";
    case AIMAGE_FORMAT_RAW_PRIVATE: return "RAW_PRIVATE";
    case AIMAGE_FORMAT_JPEG: return "JPEG";
    case AIMAGE_FORMAT_YUV_420_888: return "YUV_420_888";
    default: return NULL;
    }
}

static const char *af_mode_name(uint8_t mode)
{
    switch (mode) {
    case ACAMERA_CONTROL_AF_MODE_OFF: return "off";
    case ACAMERA_CONTROL_AF_MODE_AUTO: return "auto";
    case ACAMERA_CONTROL_AF_MODE_MACRO: return "macro";
    case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO: return "continuous_video";
    case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
        return "continuous_picture";
    case ACAMERA_CONTROL_AF_MODE_EDOF: return "edof";
    default: return "unknown";
    }
}

static const char *focus_calibration_name(int32_t calibration)
{
    switch (calibration) {
        //  acamera_metadata_enum_acamera_lens_info_focus_distance_calibration
        // Setting the lens to the same focus distance on separate occasions may
        // result in a different real focus distance, depending on factors such as the orientation
        // of the device, the age of the focusing mechanism, and the device temperature.
        
    case ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED:
        return "uncalibrated";
    case ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE:
        return "approximate";
    case ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_CALIBRATED:
        return "calibrated";
    default: return "unknown";
    }
}

static const char *scene_mode_name(uint8_t mode)
{
    switch (mode) {
    case 1: return "face-priority";
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

static const char *noise_reduction_mode_name(uint8_t mode)
{
    switch (mode) {
    case ACAMERA_NOISE_REDUCTION_MODE_OFF: return "off";
    case ACAMERA_NOISE_REDUCTION_MODE_FAST: return "fast";
    case ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY: return "high_quality";
    case 3: return "minimal";
    case 4: return "zero_shutter_lag";
    default: return "unknown";
    }
}

static void append_scene_capabilities(struct json_writer *writer,
                                      const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    bool first = true;

    json_appendf(writer, "{\"modes\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            uint8_t mode = entry.data.u8[index];
            json_appendf(writer, "%s{\"name\":", first ? "" : ",");
            json_string(writer, scene_mode_name(mode));
            json_appendf(writer, ",\"value\":%u}", (unsigned int)mode);
            first = false;
        }
    }
    json_appendf(writer, "]}");
}

static void append_hdr_dol_capabilities(struct json_writer *writer,
                                        const ACameraMetadata *metadata)
{
    const bool hdr_scene_supported = sfos_camera2_metadata_has_u8(
        metadata, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES,
        ACAMERA_CONTROL_SCENE_MODE_HDR);
    const bool manual_bracketing_supported =
        sfos_camera2_metadata_has_i32(
            metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
            ACAMERA_SENSOR_SENSITIVITY) &&
        sfos_camera2_metadata_has_i32(
            metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
            ACAMERA_SENSOR_EXPOSURE_TIME);

    json_appendf(writer,
                 "{\"hdr_scene_supported\":%s,"
                 "\"manual_bracketing_supported\":%s,"
                 "\"dol_supported\":false,"
                 "\"dol_source\":",
                 hdr_scene_supported ? "true" : "false",
                 manual_bracketing_supported ? "true" : "false");
    json_string(writer, hdr_scene_supported ? "scene-hdr" : "none");
    json_appendf(writer,
                 ",\"vendor_probe\":\"not-available-in-ndk-bridge\"}");
}

static void append_noise_reduction_capabilities(struct json_writer *writer,
                                                const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    bool first = true;
    bool request_supported = sfos_camera2_metadata_has_i32(
        metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        ACAMERA_NOISE_REDUCTION_MODE);

    json_appendf(writer, "{\"request_supported\":%s,\"modes\":[",
                 request_supported ? "true" : "false");
    if (ACameraMetadata_getConstEntry(
            metadata,
            ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            uint8_t mode = entry.data.u8[index];
            json_appendf(writer, "%s{\"name\":", first ? "" : ",");
            json_string(writer, noise_reduction_mode_name(mode));
            json_appendf(writer, ",\"value\":%u}", (unsigned int)mode);
            first = false;
        }
    }
    json_appendf(writer, "]}");
}

static void append_focus_capabilities(struct json_writer *writer,
                                      const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    bool first = true;
    float minimum_distance = sfos_camera2_first_float(
        metadata, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, 0.0f);
    int32_t calibration = sfos_camera2_first_u8(
        metadata, ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, -1);

    json_appendf(writer, "{\"af_modes\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            uint8_t mode = entry.data.u8[index];
            json_appendf(writer, "%s{\"name\":", first ? "" : ",");
            json_string(writer, af_mode_name(mode));
            json_appendf(writer, ",\"value\":%u}", (unsigned int)mode);
            first = false;
        }
    }
    json_appendf(writer,
                 "],\"minimum_focus_distance_diopters\":%.9g,"
                 "\"fixed_focus\":%s,\"focus_distance_calibration\":",
                 minimum_distance, minimum_distance > 0.0f ? "false" : "true");
    json_string(writer, focus_calibration_name(calibration));
    json_appendf(writer, ",\"focus_distance_calibration_value\":%d}",
                 calibration);
}

static void append_lens_capabilities(struct json_writer *writer,
                                     const ACameraMetadata *metadata)
{
    float focal_length = sfos_camera2_first_float(
        metadata, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, 0.0f);
    int32_t facing = sfos_camera2_first_u8(metadata, ACAMERA_LENS_FACING, -1);
    int32_t orientation = -1;
    float physical_size[2] = { 0.0f, 0.0f };

    sfos_camera2_copy_i32_array(metadata, ACAMERA_SENSOR_ORIENTATION,
                                &orientation, 1);
    sfos_camera2_copy_float_array(metadata, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE,
                                  physical_size, 2);
    json_appendf(writer, "{\"focal_length_mm\":%.9g,\"facing\":", focal_length);
    json_string(writer, facing == ACAMERA_LENS_FACING_FRONT ? "front"
                        : facing == ACAMERA_LENS_FACING_BACK ? "back"
                        : facing == ACAMERA_LENS_FACING_EXTERNAL ? "external"
                        : "unknown");
    json_appendf(writer,
                 ",\"sensor_orientation\":%d,"
                 "\"sensor_physical_size_mm\":[%.9g,%.9g]}",
                 orientation, physical_size[0], physical_size[1]);
}

static void append_zoom_capabilities(struct json_writer *writer,
                                     const ACameraMetadata *metadata)
{
    float ratio_range[2] = { 1.0f, 1.0f };
    float max_digital_zoom = sfos_camera2_first_float(
        metadata, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, 1.0f);
    bool ratio_supported = sfos_camera2_metadata_has_i32(
        metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        ACAMERA_CONTROL_ZOOM_RATIO);

    sfos_camera2_copy_float_array(
        metadata, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, ratio_range, 2);

    json_appendf(writer,
                 "{\"zoom_ratio_supported\":%s,"
                 "\"zoom_ratio_range\":[%.9g,%.9g],"
                 "\"max_digital_zoom\":%.9g}",
                 ratio_supported ? "true" : "false",
                 ratio_range[0], ratio_range[1], max_digital_zoom);
}

static void append_exposure_capabilities(struct json_writer *writer,
                                         const ACameraMetadata *metadata)
{
    int32_t sensitivity_range[2] = { 0, 0 };
    int64_t exposure_time_range[2] = { 0, 0 };
    bool iso_supported = sfos_camera2_metadata_has_i32(
        metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        ACAMERA_SENSOR_SENSITIVITY);
    bool shutter_supported = sfos_camera2_metadata_has_i32(
        metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        ACAMERA_SENSOR_EXPOSURE_TIME);

    sfos_camera2_copy_i32_array(
        metadata, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE,
        sensitivity_range, 2);
    sfos_camera2_copy_i64_array(
        metadata, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE,
        exposure_time_range, 2);

    int32_t ev_range[2] = { 0, 0 };
    sfos_camera2_copy_i32_array(metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, ev_range, 2);
    ACameraMetadata_const_entry step;
    double ev_step = 0.0;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &step) == ACAMERA_OK &&
            step.count && step.data.r[0].denominator != 0) {
        ev_step = (double)step.data.r[0].numerator / step.data.r[0].denominator;
    }
    bool manual = iso_supported && shutter_supported && sfos_camera2_metadata_has_u8(
        metadata, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
        ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);
    bool results = sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_SENSOR_SENSITIVITY) &&
        sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_SENSOR_EXPOSURE_TIME) &&
        sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_SENSOR_TIMESTAMP) &&
        sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_CONTROL_AE_MODE) &&
        sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_CONTROL_AE_STATE) &&
        sfos_camera2_metadata_has_i32(metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION);
    json_appendf(writer,
        "{\"manual_supported\":%s,\"iso_supported\":%s,\"iso_range\":[%d,%d],"
        "\"shutter_supported\":%s,\"shutter_ns_range\":[%lld,%lld],"
        "\"result_exposure_supported\":%s,\"compensation_range\":[%d,%d],\"compensation_step_ev\":%.9g,"
        "\"max_frame_duration_ns\":%lld,\"native_priority_available\":null,\"native_priority_supported_by_bridge\":false,\"native_priority_verified\":false,"
        "\"manual_verified\":false,\"metering_verified\":false,\"ae_modes\":[",
        manual ? "true" : "false", iso_supported ? "true" : "false", sensitivity_range[0], sensitivity_range[1],
        shutter_supported ? "true" : "false", (long long)exposure_time_range[0], (long long)exposure_time_range[1],
        results ? "true" : "false", ev_range[0], ev_range[1], ev_step,
        (long long)sfos_camera2_first_i64(metadata, ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION, 0));
    ACameraMetadata_const_entry modes;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_AVAILABLE_MODES, &modes) == ACAMERA_OK) {
        for (uint32_t i = 0; i < modes.count; ++i) json_appendf(writer, "%s%d", i ? "," : "", modes.data.u8[i]);
    }
    json_appendf(writer, "],\"antibanding_modes\":[");
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, &modes) == ACAMERA_OK) {
        for (uint32_t i = 0; i < modes.count; ++i) json_appendf(writer, "%s%d", i ? "," : "", modes.data.u8[i]);
    }
    json_appendf(writer, "]}");
}

static void append_aperture_capabilities(struct json_writer *writer,
                                         const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    bool first = true;
    bool request_supported = sfos_camera2_metadata_has_i32(
        metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
        ACAMERA_LENS_APERTURE);

    json_appendf(writer, "{\"request_supported\":%s,\"values\":[",
                 request_supported ? "true" : "false");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_LENS_INFO_AVAILABLE_APERTURES,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            json_appendf(writer, "%s%.3g", first ? "" : ",",
                         entry.data.f[index]);
            first = false;
        }
    }
    json_appendf(writer, "]}");
}

static void append_outputs_for(struct json_writer *writer,
                               const ACameraMetadata *metadata,
                               uint32_t tag, int32_t wanted_format)
{
    ACameraMetadata_const_entry entry;
    bool first = true;

    json_appendf(writer, "[");
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index + 3 < entry.count; index += 4) {
            int32_t format = entry.data.i32[index];
            int32_t width = entry.data.i32[index + 1];
            int32_t height = entry.data.i32[index + 2];
            int32_t direction = entry.data.i32[index + 3];
            const char *name = stream_format_name(format);

            if (format != wanted_format || !name || direction !=
                    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
                continue;
            }

            json_appendf(writer, "%s{\"format\":", first ? "" : ",");
            json_string(writer, name);
            json_appendf(writer,
                         ",\"format_value\":%d,\"width\":%d,\"height\":%d}",
                         format, width, height);
            first = false;
        }
    }
    json_appendf(writer, "]");
}

static void append_outputs(struct json_writer *writer,
                           const ACameraMetadata *metadata,
                           int32_t wanted_format)
{
    append_outputs_for(writer, metadata,
                       ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                       wanted_format);
}

static void append_i32_array(struct json_writer *writer,
                             const ACameraMetadata *metadata, uint32_t tag)
{
    ACameraMetadata_const_entry entry;
    json_appendf(writer, "[");
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            json_appendf(writer, "%s%d", index ? "," : "", entry.data.i32[index]);
        }
    }
    json_appendf(writer, "]");
}

/* Capabilities and the full-resolution mode of quad-Bayer (ultra high
 * resolution) sensors, whose default outputs are binned (Android 12+;
 * empty on older HALs). */
static void append_sensor_modes(struct json_writer *writer,
                                const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    bool ultra_high_resolution = false;
    json_appendf(writer, "{\"capabilities\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            json_appendf(writer, "%s%d", index ? "," : "", entry.data.u8[index]);
            ultra_high_resolution = ultra_high_resolution ||
                entry.data.u8[index] ==
                    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_ULTRA_HIGH_RESOLUTION_SENSOR;
        }
    }
    json_appendf(writer, "],\"ultra_high_resolution_sensor\":%s",
                 ultra_high_resolution ? "true" : "false");
    json_appendf(writer, ",\"binning_factor\":");
    append_i32_array(writer, metadata, ACAMERA_SENSOR_INFO_BINNING_FACTOR);
    json_appendf(writer, ",\"active_array_maximum_resolution\":");
    append_i32_array(writer, metadata,
                     ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE_MAXIMUM_RESOLUTION);
    json_appendf(writer, ",\"raw_outputs_maximum_resolution\":");
    append_outputs_for(writer, metadata,
                       ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_MAXIMUM_RESOLUTION,
                       AIMAGE_FORMAT_RAW16);
    json_appendf(writer, ",\"jpeg_outputs_maximum_resolution\":");
    append_outputs_for(writer, metadata,
                       ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_MAXIMUM_RESOLUTION,
                       AIMAGE_FORMAT_JPEG);
    json_appendf(writer, "}");
}

/* Not in the NDK headers (Java-only features); the NDK may also filter
 * them out of the metadata, so absence here is not proof the HAL lacks
 * them. */
#define SFOS_CAPABILITY_CONSTRAINED_HIGH_SPEED_VIDEO 9
#define SFOS_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS (ACAMERA_CONTROL_START + 35)

/* Video frame rates: the AE target ranges a normal session accepts, the
 * constrained high-speed modes (120/240 fps; Java-only sessions on most
 * releases), and the fastest frame rate of each video-sized output in a
 * normal session (from its minimum frame duration). */
static void append_video_capabilities(struct json_writer *writer,
                                      const ACameraMetadata *metadata)
{
    ACameraMetadata_const_entry entry;
    json_appendf(writer, "{\"constrained_high_speed\":%s",
                 sfos_camera2_metadata_has_u8(
                     metadata, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
                     SFOS_CAPABILITY_CONSTRAINED_HIGH_SPEED_VIDEO)
                     ? "true" : "false");
    json_appendf(writer, ",\"ae_target_fps_ranges\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index + 1 < entry.count; index += 2) {
            json_appendf(writer, "%s[%d,%d]", index ? "," : "",
                         entry.data.i32[index], entry.data.i32[index + 1]);
        }
    }
    json_appendf(writer, "],\"high_speed_configurations\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, SFOS_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS,
            &entry) == ACAMERA_OK) {
        /* width, height, fps min, fps max, max batch size */
        for (uint32_t index = 0; index + 4 < entry.count; index += 5) {
            json_appendf(writer,
                         "%s{\"width\":%d,\"height\":%d,\"fps_min\":%d,"
                         "\"fps_max\":%d,\"batch\":%d}",
                         index ? "," : "",
                         entry.data.i32[index], entry.data.i32[index + 1],
                         entry.data.i32[index + 2], entry.data.i32[index + 3],
                         entry.data.i32[index + 4]);
        }
    }
    json_appendf(writer, "],\"max_fps\":[");
    bool first = true;
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &entry) == ACAMERA_OK) {
        /* format, width, height, minimum frame duration (ns); YUV and
         * PRIVATE (encoder / display surfaces), 720p and larger. */
        for (uint32_t index = 0; index + 3 < entry.count; index += 4) {
            const int64_t format = entry.data.i64[index];
            const int64_t width = entry.data.i64[index + 1];
            const int64_t height = entry.data.i64[index + 2];
            const int64_t duration = entry.data.i64[index + 3];
            if ((format != AIMAGE_FORMAT_YUV_420_888 && format != 0x22) ||
                    width * height < 1280 * 720 || duration <= 0) {
                continue;
            }
            json_appendf(writer,
                         "%s{\"format\":\"%s\",\"width\":%lld,\"height\":%lld,"
                         "\"fps\":%.1f}",
                         first ? "" : ",",
                         format == 0x22 ? "PRIVATE" : "YUV_420_888",
                         (long long)width, (long long)height, 1e9 / (double)duration);
            first = false;
        }
    }
    /* Stabilization: electronic (video_stabilization: 0 off, 1 on, 2 preview
     * stabilization, Android 13+) and optical (0 off, 1 on), and whether
     * requests may set them. */
    static const char *const video_modes[] = { "off", "on", "preview" };
    json_appendf(writer, "],\"video_stabilization_modes\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            const uint8_t mode = entry.data.u8[index];
            if (mode < 3) {
                json_appendf(writer, "%s\"%s\"", index ? "," : "", video_modes[mode]);
            } else {
                json_appendf(writer, "%s%u", index ? "," : "", (unsigned int)mode);
            }
        }
    }
    json_appendf(writer, "],\"video_stabilization_settable\":%s",
                 sfos_camera2_metadata_has_i32(
                     metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
                     ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE) ? "true" : "false");
    json_appendf(writer, ",\"optical_stabilization_modes\":[");
    if (ACameraMetadata_getConstEntry(
            metadata, ACAMERA_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
            &entry) == ACAMERA_OK) {
        for (uint32_t index = 0; index < entry.count; ++index) {
            const uint8_t mode = entry.data.u8[index];
            if (mode < 2) {
                json_appendf(writer, "%s\"%s\"", index ? "," : "", mode ? "on" : "off");
            } else {
                json_appendf(writer, "%s%u", index ? "," : "", (unsigned int)mode);
            }
        }
    }
    json_appendf(writer, "],\"optical_stabilization_settable\":%s}",
                 sfos_camera2_metadata_has_i32(
                     metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS,
                     ACAMERA_LENS_OPTICAL_STABILIZATION_MODE) ? "true" : "false");
}

/**
 * @brief Return the bridge ABI version string exported to Sailfish.
 */
const char *sfos_camera2_bridge_version(void)
{
    return "0.14.0";
}

/**
 * @brief Build a JSON capability report for all available Camera2 devices.
 */
int sfos_camera2_probe(char *out, size_t out_size)
{
    if (!out || out_size < 2) {
        return -1;
    }

    struct json_writer writer = {
        .data = out,
        .capacity = out_size,
        .length = 0,
        .truncated = false,
    };
    out[0] = '\0';

    ACameraManager *manager = ACameraManager_create();
    if (!manager) {
        snprintf(out, out_size,
                 "{\"status\":\"error\",\"stage\":\"manager_create\"}");
        return -2;
    }

    ACameraIdList *camera_ids = NULL;
    camera_status_t status =
        ACameraManager_getCameraIdList(manager, &camera_ids);
    if (status != ACAMERA_OK || !camera_ids) {
        snprintf(out, out_size,
                 "{\"status\":\"error\",\"stage\":\"camera_list\","
                 "\"camera_status\":%d}", status);
        ACameraManager_delete(manager);
        return -3;
    }

    json_appendf(&writer,
                 "{\"status\":\"ok\",\"bridge_version\":\"%s\","
                 "\"camera_count\":%d,\"cameras\":[",
                 sfos_camera2_bridge_version(), camera_ids->numCameras);

    for (int camera_index = 0;
         camera_index < camera_ids->numCameras;
         ++camera_index) {
        const char *camera_id = camera_ids->cameraIds[camera_index];
        ACameraMetadata *metadata = NULL;

        if (camera_index != 0) {
            json_appendf(&writer, ",");
        }
        json_appendf(&writer, "{\"id\":");
        json_string(&writer, camera_id);

        status = ACameraManager_getCameraCharacteristics(
            manager, camera_id, &metadata);
        if (status != ACAMERA_OK || !metadata) {
            json_appendf(&writer,
                         ",\"status\":\"error\",\"camera_status\":%d}",
                         status);
            continue;
        }

        bool raw_capability = sfos_camera2_metadata_has_u8(
            metadata, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
            ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW);
        int32_t hardware_level = sfos_camera2_first_u8(
            metadata, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, -1);

        json_appendf(&writer,
                     ",\"status\":\"ok\",\"raw_capability\":%s,"
                     "\"hardware_level\":",
                     raw_capability ? "true" : "false");
        json_string(&writer, hardware_level_name(hardware_level));
        json_appendf(&writer, ",\"hardware_level_value\":%d,\"lens\":",
                     hardware_level);
        append_lens_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"focus\":");
        append_focus_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"scene\":");
        append_scene_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"hdr_dol\":");
        append_hdr_dol_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"zoom\":");
        append_zoom_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"exposure\":");
        append_exposure_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"aperture\":");
        append_aperture_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"noise_reduction\":");
        append_noise_reduction_capabilities(&writer, metadata);
        json_appendf(&writer, ",\"raw_outputs\":");
        append_outputs(&writer, metadata, AIMAGE_FORMAT_RAW16);
        json_appendf(&writer, ",\"raw10_outputs\":");
        append_outputs(&writer, metadata, AIMAGE_FORMAT_RAW10);
        json_appendf(&writer, ",\"jpeg_outputs\":");
        append_outputs(&writer, metadata, AIMAGE_FORMAT_JPEG);
        json_appendf(&writer, ",\"preview_outputs\":");
        append_outputs(&writer, metadata, AIMAGE_FORMAT_YUV_420_888);
        json_appendf(&writer, ",\"sensor_modes\":");
        append_sensor_modes(&writer, metadata);
        json_appendf(&writer, ",\"video\":");
        append_video_capabilities(&writer, metadata);
        json_appendf(&writer, "}");

        ACameraMetadata_free(metadata);
    }

    json_appendf(&writer, "]}");
    ACameraManager_deleteCameraIdList(camera_ids);
    ACameraManager_delete(manager);

    return writer.truncated ? -4 : 0;
}

/* Vendor-specific characteristics (tag ids from 0x80000000), for finding
 * features such as MediaTek's high-fps and slow-motion video modes. Names
 * are not available through the NDK, only ids, types and values. Separate
 * from sfos_camera2_probe(): the app parses that one, and vendor metadata
 * can be large. */
static void append_tag_list(struct json_writer *writer, const ACameraMetadata *metadata,
                            uint32_t tag, bool vendor_only)
{
    ACameraMetadata_const_entry entry;
    json_appendf(writer, "[");
    bool first = true;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) == ACAMERA_OK) {
        for (uint32_t i = 0; i < entry.count; ++i) {
            const uint32_t key = (uint32_t)entry.data.i32[i];
            if (vendor_only && key < 0x80000000u) continue;
            json_appendf(writer, "%s\"0x%08x\"", first ? "" : ",", key);
            first = false;
        }
    }
    json_appendf(writer, "]");
}

static void append_vendor_entry(struct json_writer *writer, const ACameraMetadata_const_entry *entry)
{
    static const char *const types[] = { "u8", "i32", "f", "i64", "d", "r" };
    const uint32_t shown = entry->count < 96 ? entry->count : 96;
    json_appendf(writer, "{\"tag\":\"0x%08x\",\"type\":\"%s\",\"count\":%u,\"values\":[",
                 entry->tag, entry->type < 6 ? types[entry->type] : "?", entry->count);
    for (uint32_t i = 0; i < shown && !writer->truncated; ++i) {
        const char *comma = i ? "," : "";
        switch (entry->type) {
        case ACAMERA_TYPE_BYTE: json_appendf(writer, "%s%u", comma, entry->data.u8[i]); break;
        case ACAMERA_TYPE_INT32: json_appendf(writer, "%s%d", comma, entry->data.i32[i]); break;
        case ACAMERA_TYPE_FLOAT: json_appendf(writer, "%s%g", comma, entry->data.f[i]); break;
        case ACAMERA_TYPE_INT64:
            json_appendf(writer, "%s%lld", comma, (long long)entry->data.i64[i]); break;
        case ACAMERA_TYPE_DOUBLE: json_appendf(writer, "%s%g", comma, entry->data.d[i]); break;
        case ACAMERA_TYPE_RATIONAL:
            json_appendf(writer, "%s\"%d/%d\"", comma, entry->data.r[i].numerator,
                         entry->data.r[i].denominator);
            break;
        default: break;
        }
    }
    json_appendf(writer, "]}");
}

SFOS_CAMERA2_EXPORT int sfos_camera2_probe_vendor_tags(char *out, size_t out_size)
{
    if (!out || out_size < 64) return -1;
    struct json_writer writer = { out, out_size, 0, false };
    out[0] = '\0';
    ACameraManager *manager = ACameraManager_create();
    ACameraIdList *camera_ids = NULL;
    if (!manager || ACameraManager_getCameraIdList(manager, &camera_ids) != ACAMERA_OK || !camera_ids) {
        if (manager) ACameraManager_delete(manager);
        snprintf(out, out_size, "{\"status\":\"error\",\"stage\":\"camera_list\"}");
        return -2;
    }
    json_appendf(&writer, "{\"status\":\"ok\",\"cameras\":[");
    for (int index = 0; index < camera_ids->numCameras; ++index) {
        const char *id = camera_ids->cameraIds[index];
        ACameraMetadata *metadata = NULL;
        json_appendf(&writer, "%s{\"id\":", index ? "," : "");
        json_string(&writer, id);
        if (ACameraManager_getCameraCharacteristics(manager, id, &metadata) != ACAMERA_OK ||
                !metadata) {
            json_appendf(&writer, ",\"status\":\"error\"}");
            continue;
        }
        json_appendf(&writer, ",\"session_keys\":");
        append_tag_list(&writer, metadata, ACAMERA_REQUEST_AVAILABLE_SESSION_KEYS, false);
        json_appendf(&writer, ",\"vendor_request_keys\":");
        append_tag_list(&writer, metadata, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS, true);
        json_appendf(&writer, ",\"vendor_result_keys\":");
        append_tag_list(&writer, metadata, ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS, true);
        json_appendf(&writer, ",\"vendor_characteristics\":[");
        int32_t count = 0;
        const uint32_t *tags = NULL;
        bool first = true;
        if (ACameraMetadata_getAllTags(metadata, &count, &tags) == ACAMERA_OK) {
            for (int32_t i = 0; i < count && !writer.truncated; ++i) {
                ACameraMetadata_const_entry entry;
                if (tags[i] < 0x80000000u ||
                        ACameraMetadata_getConstEntry(metadata, tags[i], &entry) != ACAMERA_OK)
                    continue;
                if (!first) json_appendf(&writer, ",");
                append_vendor_entry(&writer, &entry);
                first = false;
            }
        }
        json_appendf(&writer, "]}");
        ACameraMetadata_free(metadata);
    }
    json_appendf(&writer, "]}");
    ACameraManager_deleteCameraIdList(camera_ids);
    ACameraManager_delete(manager);
    return writer.truncated ? -4 : 0;
}
