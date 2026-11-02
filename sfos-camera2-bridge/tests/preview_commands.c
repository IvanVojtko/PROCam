// Exercises the production parser without Android/Qt dependencies.
// Build separately when permitted: cc -std=c11 -Wall -Wextra -Werror
// tests/preview_commands.c -o /tmp/test-preview-commands
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include "../android/preview_commands.h"

static struct preview_command command;
static void invalid(const char *text) {
    memset(&command, 0, sizeof(command));
    assert(preview_parse_capture_command(text, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_INVALID);
}
static void record_invalid(const char *text) {
    memset(&command, 0, sizeof(command));
    assert(preview_parse_record_command(text, &command));
    assert(command.type == PREVIEW_COMMAND_RECORD_INVALID);
}
int main(void) {
    assert(preview_parse_record_command("record-start 90 /home/u/Videos/My clip.mp4\r", &command));
    assert(command.type == PREVIEW_COMMAND_RECORD_START);
    assert(command.orientation == 90);
    assert(!strcmp(command.path, "/home/u/Videos/My clip.mp4"));
    assert(preview_parse_record_command("record-stop", &command));
    assert(command.type == PREVIEW_COMMAND_RECORD_STOP);
    assert(preview_parse_record_command(" quit \r", &command));
    assert(command.type == PREVIEW_COMMAND_QUIT);
    assert(!preview_parse_record_command("record-stopper", &command));
    assert(!preview_parse_record_command("record-starts 0 /a.mp4", &command));
    assert(!preview_parse_record_command("focus 0.5 0.5", &command));
    record_invalid("record-start");
    record_invalid("record-start 90");
    record_invalid("record-start 90 ");
    record_invalid("record-start 360 /a.mp4");
    record_invalid("record-start -90 /a.mp4");
    record_invalid("record-start 90 relative.mp4");
    record_invalid("record-start 9x /a.mp4");
    assert(preview_parse_capture_command("capture-raw /tmp/a.raw /tmp/a.json", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW);
    assert(!strcmp(command.path, "/tmp/a.raw"));
    assert(!strcmp(command.metadata_path, "/tmp/a.json"));
    assert(preview_parse_capture_command("capture-jpeg /tmp/a.jpg", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_JPEG);
    // Regression: previously sscanf(capture-raw ...) consumed this as
    // raw_path="-bracket", metadata_path="2" before the bracket branch ran.
    const char *pair="capture-raw-bracket 2 /tmp/l.raw /tmp/l.json 1600 40000000 /tmp/s.raw /tmp/s.json 1600 10000000";
    assert(preview_parse_capture_command(pair, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET);
    assert(command.bracket_count == 2);
    assert(!strcmp(command.bracket_paths[0], "/tmp/l.raw"));
    assert(!strcmp(command.bracket_metadata_paths[1], "/tmp/s.json"));
    assert(command.bracket_sensor_sensitivity[1] == 1600);
    assert(command.bracket_exposure_time_ns[0] == 40000000);
    assert(command.bracket_exposure_time_ns[1] == 10000000);
    assert(preview_parse_capture_command(" capture-raw\t/tmp/a.raw\t/tmp/a.json\r", &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW);
    invalid("capture-raw-bracket");
    invalid("capture-raw-bracket 2");
    invalid("capture-raw-bracket 3 a b 100 40 c d 100 10 e f 100 5");
    invalid("capture-raw-bracket 2junk a b 100 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100x 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 10x");
    invalid("capture-raw-bracket 2 a b 2147483648 40 c d 2147483648 10");
    invalid("capture-raw-bracket 2 a b 100 9223372036854775808 c d 100 10");
    invalid("capture-raw-bracket 2 a b 0 40 c d 0 10");
    invalid("capture-raw-bracket 2 a b -100 40 c d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 0");
    invalid("capture-raw-bracket 2 a b 100 40 c d 200 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 40");
    invalid("capture-raw-bracket 2 a b 100 10 c d 100 40");
    invalid("capture-raw-bracket 2 a b 100 40 a d 100 10");
    invalid("capture-raw-bracket 2 a b 100 40 c d 100 10 extra");
    const char *burst="capture-raw-burst 4 800 33333333 /tmp/0.raw /tmp/0.json /tmp/1.raw /tmp/1.json "
                      "/tmp/2.raw /tmp/2.json /tmp/3.raw /tmp/3.json";
    assert(preview_parse_capture_command(burst, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET);
    assert(command.bracket_count == 4);
    assert(!strcmp(command.bracket_paths[3], "/tmp/3.raw"));
    assert(!strcmp(command.bracket_metadata_paths[2], "/tmp/2.json"));
    assert(command.bracket_sensor_sensitivity[3] == 800);
    assert(command.bracket_exposure_time_ns[0] == 33333333);
    assert(command.bracket_exposure_time_ns[3] == 33333333);
    char longest[4096];
    int used = snprintf(longest, sizeof(longest), "capture-raw-burst 16 100 1000000");
    for (int i = 0; i < 16; ++i)
        used += snprintf(longest + used, sizeof(longest) - used, " /tmp/f%d.raw /tmp/f%d.json", i, i);
    assert(preview_parse_capture_command(longest, &command));
    assert(command.type == PREVIEW_COMMAND_CAPTURE_RAW_BRACKET && command.bracket_count == 16);
    assert(!strcmp(command.bracket_paths[15], "/tmp/f15.raw"));
    invalid("capture-raw-burst");
    invalid("capture-raw-burst 1 100 1000 a b");
    invalid("capture-raw-burst 17 100 1000 a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H");
    invalid("capture-raw-burst 2 100 1000 a b c");
    invalid("capture-raw-burst 2 100 1000 a b c d e");
    invalid("capture-raw-burst 2 0 1000 a b c d");
    invalid("capture-raw-burst 2 100 0 a b c d");
    invalid("capture-raw-burst 2 100 1000 a b a d");
    invalid("capture-raw-burst 2x 100 1000 a b c d");
    invalid("capture-raw");
    invalid("capture-raw a");
    invalid("capture-raw a b extra");
    invalid("capture-raw-suffix a b");
    invalid("capture-jpeg-suffix a");
    invalid("capture-jpeg a extra");
    char oversized[9000];memset(oversized,'x',sizeof(oversized));
    memcpy(oversized,"capture-raw ",12);oversized[sizeof(oversized)-1]=0;
    invalid(oversized);
    assert(!preview_parse_capture_command("zoom 2", &command));
    puts("Capture parser regression tests passed");
}
