// Standalone control protocol definitions and capture parser (no Android dependencies).
#ifndef PROCAM_PREVIEW_COMMANDS_H
#define PROCAM_PREVIEW_COMMANDS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
// Pairs use two slots; equal-exposure bursts (capture-raw-burst) up to eight.
// Night mode bursts of 16 frames; normal HDR uses 8. 16 frames' paths
// (about 2.6 KB) fit the 8 KB command line.
#define PREVIEW_MAX_RAW_BRACKET 16
#define PREVIEW_CAPTURE_MAX_ARGS (3 + 2 * PREVIEW_MAX_RAW_BRACKET)

enum preview_command_type {
    PREVIEW_COMMAND_NONE = 0,
    PREVIEW_COMMAND_CAPTURE_INVALID,
    PREVIEW_COMMAND_FOCUS,
    PREVIEW_COMMAND_CAPTURE_JPEG,
    PREVIEW_COMMAND_CAPTURE_RAW,
    PREVIEW_COMMAND_CAPTURE_RAW_BRACKET,
    PREVIEW_COMMAND_ZOOM,
    PREVIEW_COMMAND_SETTINGS,
    PREVIEW_COMMAND_EXPOSURE_SETTINGS,
    PREVIEW_COMMAND_FOCUS_HOLD,
    PREVIEW_COMMAND_FOCUS_HOLD_RELEASE,
    PREVIEW_COMMAND_RECORD_START,
    PREVIEW_COMMAND_RECORD_STOP,
    PREVIEW_COMMAND_RECORD_INVALID,
    PREVIEW_COMMAND_QUIT,
};

struct preview_command {
    enum preview_command_type type;
    float focus_x;
    float focus_y;
    float zoom_ratio;
    int focus_mode;
    float focus_distance;
    int exposure_compensation;
    int scene_mode;
    int color_temperature_kelvin;
    int color_tint;
    int32_t sensor_sensitivity;
    int64_t exposure_time_ns;
    int aperture;
    int noise_reduction;
    int orientation;
    char path[4096];
    char metadata_path[4096];
    int bracket_count;
    char bracket_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    char bracket_metadata_paths[PREVIEW_MAX_RAW_BRACKET][4096];
    int32_t bracket_sensor_sensitivity[PREVIEW_MAX_RAW_BRACKET];
    int64_t bracket_exposure_time_ns[PREVIEW_MAX_RAW_BRACKET];
};

struct preview_command_buffer {
    char data[8192];
    size_t length;
    bool discarding;
};


// Strict positive decimal integers: reject signs, suffixes and overflow.
static bool preview_capture_integer(const char *text, int64_t maximum, int64_t *value)
{
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || *end || parsed <= 0 || parsed > maximum) return false;
    *value = parsed;
    return true;
}

// Returns true for capture commands, including malformed ones. Callers must
// acknowledge CAPTURE_INVALID so the host doesn't wait for a nonexistent file.
static bool preview_parse_capture_command(const char *line, struct preview_command *command)
{
    const char *start = line;
    while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
    if (strncmp(start, "capture-", 8)) return false;
    command->type = PREVIEW_COMMAND_CAPTURE_INVALID;
    if (strlen(start) >= sizeof(((struct preview_command_buffer *)0)->data)) return true;
    char copy[sizeof(((struct preview_command_buffer *)0)->data)];
    strcpy(copy, start);
    char *save = NULL;
    char *name = strtok_r(copy, " \t\r", &save);
    char *args[PREVIEW_CAPTURE_MAX_ARGS];
    int n = 0;
    char *token;
    while ((token = strtok_r(NULL, " \t\r", &save))) {
        if (n == PREVIEW_CAPTURE_MAX_ARGS) return true;
        args[n++] = token;
    }
    if (!strcmp(name, "capture-jpeg") && n == 1) {
        if (strlen(args[0]) >= sizeof(command->path)) return true;
        strcpy(command->path, args[0]);
        command->type = PREVIEW_COMMAND_CAPTURE_JPEG;
    } else if (!strcmp(name, "capture-raw") && n == 2) {
        if (strlen(args[0]) >= sizeof(command->path) ||
                strlen(args[1]) >= sizeof(command->metadata_path)) return true;
        strcpy(command->path, args[0]);
        strcpy(command->metadata_path, args[1]);
        command->type = PREVIEW_COMMAND_CAPTURE_RAW;
    } else if (!strcmp(name, "capture-raw-bracket") && n == 9 && !strcmp(args[0], "2")) {
        for (int i = 0; i < 2; ++i) {
            char *raw = args[1+i*4], *metadata = args[2+i*4];
            int64_t iso, exposure;
            if (strlen(raw) >= sizeof(command->bracket_paths[i]) ||
                    strlen(metadata) >= sizeof(command->bracket_metadata_paths[i]) ||
                    !preview_capture_integer(args[3+i*4], INT32_MAX, &iso) ||
                    !preview_capture_integer(args[4+i*4], INT64_MAX, &exposure)) return true;
            strcpy(command->bracket_paths[i], raw);
            strcpy(command->bracket_metadata_paths[i], metadata);
            command->bracket_sensor_sensitivity[i] = (int32_t)iso;
            command->bracket_exposure_time_ns[i] = exposure;
        }
        // RAW pair contract: fixed ISO, long followed by distinct short, unique files.
        if (command->bracket_sensor_sensitivity[0] != command->bracket_sensor_sensitivity[1] ||
                command->bracket_exposure_time_ns[0] <= command->bracket_exposure_time_ns[1]) return true;
        const char *paths[] = {args[1], args[2], args[5], args[6]};
        for (int i=0; i<4; ++i) for (int j=i+1; j<4; ++j)
            if (!strcmp(paths[i], paths[j])) return true;
        command->bracket_count = 2;
        command->type = PREVIEW_COMMAND_CAPTURE_RAW_BRACKET;
    } else if (!strcmp(name, "capture-raw-burst") && n >= 3) {
        // capture-raw-burst <count> <iso> <exposure_ns> (<raw> <metadata>){count}
        // Equal exposures for merging; shares the bracket capture slots.
        int64_t count, iso, exposure;
        if (!preview_capture_integer(args[0], PREVIEW_MAX_RAW_BRACKET, &count) || count < 2 ||
                n != 3 + 2 * count ||
                !preview_capture_integer(args[1], INT32_MAX, &iso) ||
                !preview_capture_integer(args[2], INT64_MAX, &exposure)) return true;
        for (int i = 0; i < count; ++i) {
            const char *raw = args[3 + 2 * i], *metadata = args[4 + 2 * i];
            if (strlen(raw) >= sizeof(command->bracket_paths[i]) ||
                    strlen(metadata) >= sizeof(command->bracket_metadata_paths[i])) return true;
            strcpy(command->bracket_paths[i], raw);
            strcpy(command->bracket_metadata_paths[i], metadata);
            command->bracket_sensor_sensitivity[i] = (int32_t)iso;
            command->bracket_exposure_time_ns[i] = exposure;
        }
        for (int i = 3; i < n; ++i) for (int j = i + 1; j < n; ++j)
            if (!strcmp(args[i], args[j])) return true;
        command->bracket_count = (int)count;
        command->type = PREVIEW_COMMAND_CAPTURE_RAW_BRACKET;
    }
    return true;
}

// Video recording: "record-start <orientation> <path>" (the path is the rest
// of the line and may contain spaces), "record-stop", and "quit", which ends
// the preview and finishes a recording on the way out. Returns true for
// these, including a malformed record-start (RECORD_INVALID, answered so the
// host doesn't wait for a recording that never starts).
static bool preview_parse_record_command(const char *line, struct preview_command *command)
{
    const char *start = line;
    while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
    size_t length = strlen(start);
    while (length && (start[length - 1] == '\r' || start[length - 1] == ' ' ||
                      start[length - 1] == '\t')) --length;
    if (length == 4 && !strncmp(start, "quit", 4)) {
        command->type = PREVIEW_COMMAND_QUIT;
        return true;
    }
    if (length == 11 && !strncmp(start, "record-stop", 11)) {
        command->type = PREVIEW_COMMAND_RECORD_STOP;
        return true;
    }
    if (strncmp(start, "record-start", 12) || (length > 12 && start[12] != ' ')) return false;
    command->type = PREVIEW_COMMAND_RECORD_INVALID;
    const char *cursor = start + 12;
    while (*cursor == ' ') ++cursor;
    int orientation = 0, digits = 0;
    while (*cursor >= '0' && *cursor <= '9' && digits < 4) {
        orientation = orientation * 10 + (*cursor++ - '0');
        ++digits;
    }
    if (!digits || orientation > 359 || *cursor != ' ') return true;
    while (*cursor == ' ') ++cursor;
    const size_t path_length = length - (size_t)(cursor - start);
    if ((size_t)(cursor - start) >= length || *cursor != '/' ||
            path_length >= sizeof(command->path)) return true;
    memcpy(command->path, cursor, path_length);
    command->path[path_length] = '\0';
    command->orientation = orientation;
    command->type = PREVIEW_COMMAND_RECORD_START;
    return true;
}
#endif
