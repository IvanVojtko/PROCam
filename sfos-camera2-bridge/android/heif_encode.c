/**
 * @file heif_encode.c
 * @brief HEIC stills: the hardware HEIC encoder (image/vnd.android.heic,
 * which cuts the picture into 512x512 tiles itself) and the Android muxer's
 * HEIF output (a grid image, its rotation and EXIF).
 *
 * The input is an I420 file. Encoders want a size aligned to 16 pixels, so
 * the picture is padded by repeating its edge; the grid image keeps the real
 * size, so viewers crop the padding.
 */

#include "camera2_bridge.h"
#include "camera2_common.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEIF_MIME "image/vnd.android.heic"
#define HEIF_TILE 512
#define HEIF_ALIGN 16
#define HEIF_COLOR_PLANAR 19
#define HEIF_COLOR_SEMIPLANAR 21
#define HEIF_BITRATE_MODE_CQ 0
/* AMEDIAMUXER_OUTPUT_FORMAT_HEIF (the NDK header says API 34; the muxer has
 * written HEIF since Android 9). */
#define HEIF_MUXER_FORMAT 3
/* MediaCodec BUFFER_FLAG_MUXER_DATA: the sample is EXIF for the image. */
#define HEIF_MUXER_DATA_FLAG 16
#define HEIF_TIMEOUT_US 20000
#define HEIF_DEADLINE_MS 15000

/* API 28; the bridge targets API 24, so these are looked up. */
typedef AMediaFormat *(*get_input_format_fn)(AMediaCodec *);
typedef void (*set_rect_fn)(AMediaFormat *, const char *, int32_t, int32_t, int32_t, int32_t);

/* android::MediaImage2: where each plane of a flexible YUV buffer is. */
struct heif_plane_info {
    uint32_t offset;
    int32_t col_inc;
    int32_t row_inc;
    uint32_t horiz_subsampling;
    uint32_t vert_subsampling;
};

struct heif_media_image {
    int32_t type; /* 1: YUV */
    uint32_t num_planes;
    uint32_t width;
    uint32_t height;
    uint32_t bit_depth;
    uint32_t bit_depth_allocated;
    struct heif_plane_info plane[4];
};

struct heif_report {
    char *json;
    size_t size;
};

static int heif_fail(struct heif_report *report, int code, const char *stage,
                     int media_status)
{
    snprintf(report->json, report->size,
             "{\"ok\":false,\"stage\":\"%s\",\"media_status\":%d}", stage, media_status);
    fprintf(stderr, "heif-encode failed stage=%s media_status=%d\n", stage, media_status);
    return code;
}

static uint8_t heif_sample(const uint8_t *plane, int width, int height, int x, int y)
{
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    return plane[(size_t)y * (size_t)width + (size_t)x];
}

/* Copies the I420 picture into the encoder's input buffer, padding the
 * aligned size by repeating the last row and column. */
static bool heif_fill(uint8_t *buffer, size_t capacity, const uint8_t *i420,
                      int width, int height, int aligned_width, int aligned_height,
                      AMediaFormat *input_format)
{
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const uint8_t *source[3] = {
        i420,
        i420 + (size_t)width * (size_t)height,
        i420 + (size_t)width * (size_t)height + (size_t)chroma_width * (size_t)chroma_height,
    };
    const int source_width[3] = { width, chroma_width, chroma_width };
    const int source_height[3] = { height, chroma_height, chroma_height };

    struct heif_media_image image;
    memset(&image, 0, sizeof(image));
    void *data = NULL;
    size_t data_size = 0;
    if (input_format &&
            AMediaFormat_getBuffer(input_format, "image-data", &data, &data_size) &&
            data_size >= sizeof(image)) {
        memcpy(&image, data, sizeof(image));
    }
    if (image.type != 1 || image.num_planes != 3) {
        /* No plane description: planar or semi-planar by stride and slice
         * height. */
        int32_t color = HEIF_COLOR_PLANAR, stride = aligned_width, slice = aligned_height;
        if (input_format) {
            AMediaFormat_getInt32(input_format, "color-format", &color);
            AMediaFormat_getInt32(input_format, "stride", &stride);
            AMediaFormat_getInt32(input_format, "slice-height", &slice);
        }
        if (stride < aligned_width) stride = aligned_width;
        if (slice < aligned_height) slice = aligned_height;
        image.type = 1;
        image.num_planes = 3;
        image.plane[0] = (struct heif_plane_info){ 0, 1, stride, 1, 1 };
        const uint32_t chroma = (uint32_t)stride * (uint32_t)slice;
        if (color == HEIF_COLOR_SEMIPLANAR) {
            image.plane[1] = (struct heif_plane_info){ chroma, 2, stride, 2, 2 };
            image.plane[2] = (struct heif_plane_info){ chroma + 1, 2, stride, 2, 2 };
        } else {
            image.plane[1] = (struct heif_plane_info){ chroma, 1, stride / 2, 2, 2 };
            image.plane[2] = (struct heif_plane_info){
                chroma + (uint32_t)(stride / 2) * (uint32_t)(slice / 2), 1, stride / 2, 2, 2 };
        }
    }

    for (int p = 0; p < 3; ++p) {
        const struct heif_plane_info *plane = &image.plane[p];
        const int sub_x = plane->horiz_subsampling ? (int)plane->horiz_subsampling : 1;
        const int sub_y = plane->vert_subsampling ? (int)plane->vert_subsampling : 1;
        const int plane_width = (aligned_width + sub_x - 1) / sub_x;
        const int plane_height = (aligned_height + sub_y - 1) / sub_y;
        const int64_t last = (int64_t)plane->offset
                + (int64_t)(plane_height - 1) * plane->row_inc
                + (int64_t)(plane_width - 1) * plane->col_inc;
        if (plane->row_inc <= 0 || plane->col_inc <= 0 || last < 0 || (size_t)last >= capacity) {
            return false;
        }
        for (int y = 0; y < plane_height; ++y) {
            uint8_t *row = buffer + plane->offset + (size_t)y * (size_t)plane->row_inc;
            for (int x = 0; x < plane_width; ++x) {
                row[(size_t)x * (size_t)plane->col_inc] =
                    heif_sample(source[p], source_width[p], source_height[p], x, y);
            }
        }
    }
    return true;
}

static uint8_t *heif_read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    uint8_t *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
            data = malloc((size_t)length);
            if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
                free(data);
                data = NULL;
            }
            if (data) *size = (size_t)length;
        }
    }
    fclose(file);
    return data;
}

SFOS_CAMERA2_EXPORT int sfos_camera2_encode_heic(const char *input_path, int width, int height,
                                                 int quality, int rotation_degrees,
                                                 const char *exif_path, const char *output_path,
                                                 char *json, size_t json_size)
{
    struct heif_report report = { json, json_size };
    if (!input_path || !output_path || !json || json_size == 0 ||
            width < 16 || height < 16 || width > 16383 || height > 16383 ||
            quality < 0 || quality > 100) {
        return heif_fail(&report, -1, "arguments", 0);
    }
    const int64_t started = sfos_camera2_now_ms();
    size_t input_size = 0;
    uint8_t *i420 = heif_read_file(input_path, &input_size);
    const size_t expected = (size_t)width * (size_t)height
            + 2 * (size_t)((width + 1) / 2) * (size_t)((height + 1) / 2);
    if (!i420 || input_size < expected) {
        free(i420);
        return heif_fail(&report, -2, "input", 0);
    }
    /* EXIF, when given, is part of the photo: no HEIC without it. */
    size_t exif_size = 0;
    uint8_t *exif = exif_path && *exif_path ? heif_read_file(exif_path, &exif_size) : NULL;
    if (exif_path && *exif_path && (!exif || exif_size <= 6)) {
        free(exif);
        free(i420);
        return heif_fail(&report, -13, "exif-input", 0);
    }

    const int aligned_width = (width + HEIF_ALIGN - 1) / HEIF_ALIGN * HEIF_ALIGN;
    const int aligned_height = (height + HEIF_ALIGN - 1) / HEIF_ALIGN * HEIF_ALIGN;
    const int grid_cols = (aligned_width + HEIF_TILE - 1) / HEIF_TILE;
    const int grid_rows = (aligned_height + HEIF_TILE - 1) / HEIF_TILE;

    int code = 0;
    const char *stage = "codec";
    media_status_t status = AMEDIA_OK;
    AMediaMuxer *muxer = NULL;
    AMediaFormat *input_format = NULL;
    int fd = -1;
    int tiles = 0;
    bool muxer_started = false;
    ssize_t track = -1;

    AMediaCodec *codec = AMediaCodec_createEncoderByType(HEIF_MIME);
    AMediaFormat *format = AMediaFormat_new();
    if (!codec || !format) {
        code = heif_fail(&report, -3, stage, 0);
        goto done;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, HEIF_MIME);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, aligned_width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, aligned_height);
    AMediaFormat_setInt32(format, "tile-width", HEIF_TILE);
    AMediaFormat_setInt32(format, "tile-height", HEIF_TILE);
    AMediaFormat_setInt32(format, "grid-rows", grid_rows);
    AMediaFormat_setInt32(format, "grid-cols", grid_cols);
    AMediaFormat_setInt32(format, "bitrate-mode", HEIF_BITRATE_MODE_CQ);
    AMediaFormat_setInt32(format, "quality", quality);
    /* NV12 explicitly: asked for flexible YUV without describing it, the
     * MediaTek encoder reads the chroma as VU. */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, HEIF_COLOR_SEMIPLANAR);
    /* Full-range BT.601 with the sRGB curve, as JPEG: the input is ProCam's
     * JPEG-style YCbCr, and viewers read it the same way. */
    AMediaFormat_setInt32(format, "color-range", 1);     /* COLOR_RANGE_FULL */
    AMediaFormat_setInt32(format, "color-standard", 4);  /* COLOR_STANDARD_BT601_PAL */
    AMediaFormat_setInt32(format, "color-transfer", 3);  /* COLOR_TRANSFER_SDR_VIDEO */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, grid_rows * grid_cols);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 0);
    status = AMediaCodec_configure(codec, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status == AMEDIA_OK) status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        code = heif_fail(&report, -4, "configure", status);
        goto done;
    }
    get_input_format_fn get_input_format =
        (get_input_format_fn)dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat");
    input_format = get_input_format ? get_input_format(codec) : NULL;
    set_rect_fn set_rect = (set_rect_fn)dlsym(RTLD_DEFAULT, "AMediaFormat_setRect");

    /* The whole picture in one input buffer, then the end of the stream. */
    bool queued = false, ended = false, output_done = false;
    int64_t time_us = 0;
    while (!output_done) {
        if (sfos_camera2_now_ms() - started > HEIF_DEADLINE_MS) {
            code = heif_fail(&report, -5, "timeout", 0);
            goto done;
        }
        if (!ended) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, HEIF_TIMEOUT_US);
            if (index >= 0) {
                size_t capacity = 0;
                uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)index, &capacity);
                if (!queued) {
                    if (!buffer || !heif_fill(buffer, capacity, i420, width, height,
                                              aligned_width, aligned_height, input_format)) {
                        code = heif_fail(&report, -6, "input-buffer", (int)capacity);
                        goto done;
                    }
                    const size_t used = capacity < (size_t)aligned_width * (size_t)aligned_height * 3 / 2
                            ? capacity : (size_t)aligned_width * (size_t)aligned_height * 3 / 2;
                    AMediaCodec_queueInputBuffer(codec, (size_t)index, 0, used, 0, 0);
                    queued = true;
                } else {
                    AMediaCodec_queueInputBuffer(codec, (size_t)index, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    ended = true;
                }
            }
        }
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, HEIF_TIMEOUT_US);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *output = AMediaCodec_getOutputFormat(codec);
            /* The grid image: its tiles and the real (unpadded) size. */
            /* The muxer wants the crop to match the size. */
            AMediaFormat_setInt32(output, AMEDIAFORMAT_KEY_WIDTH, width);
            AMediaFormat_setInt32(output, AMEDIAFORMAT_KEY_HEIGHT, height);
            if (set_rect) set_rect(output, "crop", 0, 0, width - 1, height - 1);
            AMediaFormat_setInt32(output, "tile-width", HEIF_TILE);
            AMediaFormat_setInt32(output, "tile-height", HEIF_TILE);
            AMediaFormat_setInt32(output, "grid-rows", grid_rows);
            AMediaFormat_setInt32(output, "grid-cols", grid_cols);
            fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            muxer = fd >= 0 ? AMediaMuxer_new(fd, HEIF_MUXER_FORMAT) : NULL;
            if (!muxer) {
                AMediaFormat_delete(output);
                code = heif_fail(&report, -7, fd < 0 ? "output" : "muxer", 0);
                goto done;
            }
            AMediaMuxer_setOrientationHint(muxer, ((rotation_degrees % 360) + 360) % 360);
            track = AMediaMuxer_addTrack(muxer, output);
            AMediaFormat_delete(output);
            status = track >= 0 ? AMediaMuxer_start(muxer) : AMEDIA_ERROR_UNKNOWN;
            if (status != AMEDIA_OK) {
                code = heif_fail(&report, -8, "muxer-start", status);
                goto done;
            }
            muxer_started = true;
            if (exif) {
                AMediaCodecBufferInfo exif_info = { 0, (int32_t)exif_size, 0, HEIF_MUXER_DATA_FLAG };
                status = AMediaMuxer_writeSampleData(muxer, (size_t)track, exif, &exif_info);
                if (status != AMEDIA_OK) {
                    code = heif_fail(&report, -14, "exif", status);
                    goto done;
                }
            }
        } else if (index >= 0) {
            size_t size = 0;
            uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)index, &size);
            const bool config = info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
            if (!config && info.size > 0 && data) {
                if (!muxer_started) {
                    AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
                    code = heif_fail(&report, -9, "no-format", 0);
                    goto done;
                }
                AMediaCodecBufferInfo sample = { info.offset, info.size, time_us, 0 };
                time_us += 1000;
                status = AMediaMuxer_writeSampleData(muxer, (size_t)track, data, &sample);
                if (status != AMEDIA_OK) {
                    AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
                    code = heif_fail(&report, -10, "write", status);
                    goto done;
                }
                ++tiles;
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) output_done = true;
            AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
        }
    }
    if (tiles < grid_rows * grid_cols) {
        code = heif_fail(&report, -11, "tiles", tiles);
        goto done;
    }
    status = AMediaMuxer_stop(muxer);
    muxer_started = false;
    if (status != AMEDIA_OK) {
        code = heif_fail(&report, -12, "muxer-stop", status);
        goto done;
    }
    snprintf(json, json_size,
             "{\"ok\":true,\"width\":%d,\"height\":%d,\"grid\":[%d,%d],\"tiles\":%d,"
             "\"quality\":%d,\"rotation\":%d,\"exif_bytes\":%zu,\"encode_ms\":%lld}",
             width, height, grid_cols, grid_rows, tiles, quality, rotation_degrees,
             exif ? exif_size : (size_t)0, (long long)(sfos_camera2_now_ms() - started));

done:
    if (muxer) {
        if (muxer_started) AMediaMuxer_stop(muxer);
        AMediaMuxer_delete(muxer);
    }
    if (fd >= 0) close(fd);
    if (code != 0) unlink(output_path);
    if (input_format) AMediaFormat_delete(input_format);
    if (format) AMediaFormat_delete(format);
    if (codec) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    free(exif);
    free(i420);
    return code;
}
