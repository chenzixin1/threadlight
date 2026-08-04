#include <errno.h>
#include <hidapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define FEKER_VID 0x320F
#define FEKER_PID 0x5055
#define FEKER_USAGE_PAGE 0xFF1C
#define FEKER_USAGE 0x0092
#define FEKER_QMK_VID 0x36B0
#define FEKER_QMK_PID 0x305F
#define FEKER_QMK_USAGE_PAGE 0xFF60
#define FEKER_QMK_USAGE 0x0061
#define QMK_REPORT_SIZE 32
#define REPORT_SIZE 64
#define LED_COUNT 90
#define COLOR_BYTES (LED_COUNT * 3)
#define MAX_COLOR_PAYLOAD 0x36
#define DIRECT_REFRESH_OFFSET 18
#define DIRECT_REFRESH_INTERVAL_US 100000
#define DIRECT_TEST_HOLD_TICKS 600
#define QMK_TEST_HOLD_SECONDS 10

/*
 * Column-major matrix with six slots per column.  Slot 6 is intentionally
 * blank and is used for the direct-mode keepalive, so number 1 begins at 7.
 */
static const int number_key_leds[9] = {7, 13, 19, 25, 31, 37, 43, 49, 55};
static bool verbose_packets = false;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static void fill_colors(rgb_t colors[LED_COUNT], rgb_t color);

static void usage(const char *program) {
    fprintf(stderr,
            "FEKER Alice80 per-key RGB control\n\n"
            "Usage:\n"
            "  %s list\n"
            "  %s probe\n"
            "  %s qmk-probe\n"
            "  %s qmk-all RRGGBB\n"
            "  %s scan\n"
            "  %s off\n"
            "  %s all RRGGBB\n"
            "  %s key 1-9 RRGGBB\n"
            "  %s led 0-89 RRGGBB\n"
            "  %s slots [1=RRGGBB ... 9=RRGGBB]\n"
            "\nExamples:\n"
            "  %s key 1 00FF00\n"
            "  %s slots 1=00FF00 3=FFBF00\n",
            program, program, program, program, program, program, program, program,
            program, program, program, program);
}

static rgb_t parse_color(const char *text) {
    const char *digits = text;
    if (digits[0] == '#') {
        digits++;
    }
    if (strlen(digits) != 6) {
        fprintf(stderr, "Invalid color '%s'; expected RRGGBB.\n", text);
        exit(2);
    }

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(digits, &end, 16);
    if (errno != 0 || end == NULL || *end != '\0' || value > 0xFFFFFFUL) {
        fprintf(stderr, "Invalid color '%s'; expected RRGGBB.\n", text);
        exit(2);
    }

    return (rgb_t){
        .r = (uint8_t)((value >> 16) & 0xFF),
        .g = (uint8_t)((value >> 8) & 0xFF),
        .b = (uint8_t)(value & 0xFF),
    };
}

static void print_wide(const wchar_t *value) {
    if (value == NULL) {
        fputs("(none)", stdout);
        return;
    }
    char converted[512];
    size_t count = wcstombs(converted, value, sizeof(converted) - 1);
    if (count == (size_t)-1) {
        fputs("(unprintable)", stdout);
        return;
    }
    converted[count] = '\0';
    fputs(converted, stdout);
}

static int list_devices(void) {
    struct hid_device_info *devices = hid_enumerate(0, 0);
    bool found = false;

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        bool old_generation = item->vendor_id == FEKER_VID &&
                              item->product_id == FEKER_PID;
        bool new_generation = item->vendor_id == FEKER_QMK_VID &&
                              item->product_id == FEKER_QMK_PID;
        if (!old_generation && !new_generation) {
            continue;
        }
        found = true;
        printf("%s generation\n", new_generation ? "QMK/VIA" : "EVision");
        printf("path=%s\n", item->path != NULL ? item->path : "(none)");
        printf("  product=");
        print_wide(item->product_string);
        printf("\n  usage_page=0x%04X usage=0x%04X interface=%d\n",
               item->usage_page, item->usage, item->interface_number);
    }
    hid_free_enumeration(devices);
    if (!found) {
        fprintf(stderr,
                "No supported FEKER Alice80 USB identity is connected.\n");
        return 1;
    }
    return 0;
}

static hid_device *open_qmk_device(void) {
    struct hid_device_info *devices = hid_enumerate(FEKER_QMK_VID, FEKER_QMK_PID);
    struct hid_device_info *chosen = NULL;

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (item->usage_page == FEKER_QMK_USAGE_PAGE &&
            item->usage == FEKER_QMK_USAGE) {
            chosen = item;
            break;
        }
    }
    if (chosen == NULL) {
        hid_free_enumeration(devices);
        fprintf(stderr,
                "New-generation FEKER Raw HID interface not found "
                "(expected 36B0:305F, usage FF60:0061).\n");
        return NULL;
    }

    hid_device *device = hid_open_path(chosen->path);
    if (device == NULL) {
        fwprintf(stderr, L"Unable to open FEKER QMK Raw HID interface: %ls\n",
                 hid_error(NULL));
    }
    hid_free_enumeration(devices);
    return device;
}

static bool qmk_query(hid_device *device, uint8_t command,
                      const uint8_t *payload, size_t payload_size,
                      uint8_t response[QMK_REPORT_SIZE]) {
    if (payload_size > QMK_REPORT_SIZE - 1) {
        return false;
    }
    uint8_t report[QMK_REPORT_SIZE + 1];
    memset(report, 0, sizeof(report));
    report[1] = command;
    if (payload_size > 0) {
        memcpy(&report[2], payload, payload_size);
    }

    int written = hid_write(device, report, sizeof(report));
    if (written != (int)sizeof(report)) {
        fwprintf(stderr, L"QMK HID write failed: %ls\n", hid_error(device));
        return false;
    }
    memset(response, 0, QMK_REPORT_SIZE);
    int received = hid_read_timeout(device, response, QMK_REPORT_SIZE, 500);
    if (received <= 0) {
        fwprintf(stderr, L"QMK HID reply failed: %ls\n", hid_error(device));
        return false;
    }

    printf("query 0x%02X reply (%d bytes):", command, received);
    int shown = received < 16 ? received : 16;
    for (int index = 0; index < shown; index++) {
        printf(" %02X", response[index]);
    }
    putchar('\n');
    if (response[0] != command) {
        fprintf(stderr,
                "QMK/VIA returned an unexpected reply for command 0x%02X.\n",
                command);
        return false;
    }
    return true;
}

static int probe_qmk_device(void) {
    hid_device *device = open_qmk_device();
    if (device == NULL) {
        return 1;
    }

    uint8_t response[QMK_REPORT_SIZE];
    puts("Read-only VIA protocol probe");
    bool ok = qmk_query(device, 0x01, NULL, 0, response);
    if (ok) {
        printf("VIA protocol version: 0x%02X%02X\n", response[1], response[2]);
    }

    const uint8_t per_key_query[] = {0x00, 0x01, 0x00, 0x01};
    bool per_key_ok = qmk_query(device, 0x08, per_key_query,
                                sizeof(per_key_query), response);
    if (per_key_ok) {
        printf("Per-key RGB channel response: hue=%u saturation=%u\n",
               response[5], response[6]);
    }

    const uint8_t brightness_query[] = {0x03, 0x01};
    bool brightness_ok = qmk_query(device, 0x08, brightness_query,
                                   sizeof(brightness_query), response);
    if (brightness_ok) {
        printf("RGB matrix brightness response: %u\n", response[3]);
    }

    const uint8_t effect_query[] = {0x03, 0x02};
    bool effect_ok = qmk_query(device, 0x08, effect_query,
                               sizeof(effect_query), response);
    if (effect_ok) {
        printf("RGB matrix effect response: %u\n", response[3]);
    }

    const uint8_t color_query[] = {0x03, 0x04};
    bool color_ok = qmk_query(device, 0x08, color_query,
                              sizeof(color_query), response);
    if (color_ok) {
        printf("RGB matrix color response: hue=%u saturation=%u\n",
               response[3], response[4]);
    }

    hid_close(device);
    return ok && brightness_ok && effect_ok && color_ok && !per_key_ok ? 0 : 1;
}

static void rgb_to_hsv(rgb_t color, uint8_t *hue,
                       uint8_t *saturation, uint8_t *value) {
    uint8_t maximum = color.r;
    if (color.g > maximum) {
        maximum = color.g;
    }
    if (color.b > maximum) {
        maximum = color.b;
    }
    uint8_t minimum = color.r;
    if (color.g < minimum) {
        minimum = color.g;
    }
    if (color.b < minimum) {
        minimum = color.b;
    }
    int delta = (int)maximum - (int)minimum;
    *value = maximum;
    *saturation = maximum == 0
                      ? 0
                      : (uint8_t)((delta * 255 + maximum / 2) / maximum);
    if (delta == 0) {
        *hue = 0;
        return;
    }

    int hue_value;
    if (maximum == color.r) {
        hue_value = (43 * ((int)color.g - (int)color.b)) / delta;
    } else if (maximum == color.g) {
        hue_value = 85 + (43 * ((int)color.b - (int)color.r)) / delta;
    } else {
        hue_value = 171 + (43 * ((int)color.r - (int)color.g)) / delta;
    }
    if (hue_value < 0) {
        hue_value += 256;
    }
    *hue = (uint8_t)hue_value;
}

static bool qmk_set_menu_value(hid_device *device, uint8_t command,
                               const uint8_t *values, size_t value_count) {
    if (value_count > QMK_REPORT_SIZE - 3) {
        return false;
    }
    uint8_t payload[QMK_REPORT_SIZE - 1];
    payload[0] = 0x03;
    payload[1] = command;
    memcpy(&payload[2], values, value_count);
    uint8_t response[QMK_REPORT_SIZE];
    return qmk_query(device, 0x07, payload, value_count + 2, response);
}

static int test_qmk_whole_board(rgb_t color) {
    hid_device *device = open_qmk_device();
    if (device == NULL) {
        return 1;
    }

    uint8_t response[QMK_REPORT_SIZE];
    const uint8_t brightness_query[] = {0x03, 0x01};
    const uint8_t effect_query[] = {0x03, 0x02};
    const uint8_t color_query[] = {0x03, 0x04};
    if (!qmk_query(device, 0x08, brightness_query,
                   sizeof(brightness_query), response)) {
        hid_close(device);
        return 1;
    }
    uint8_t old_brightness = response[3];
    if (!qmk_query(device, 0x08, effect_query, sizeof(effect_query), response)) {
        hid_close(device);
        return 1;
    }
    uint8_t old_effect = response[3];
    if (!qmk_query(device, 0x08, color_query, sizeof(color_query), response)) {
        hid_close(device);
        return 1;
    }
    uint8_t old_color[] = {response[3], response[4]};

    uint8_t hue;
    uint8_t saturation;
    uint8_t brightness;
    rgb_to_hsv(color, &hue, &saturation, &brightness);
    uint8_t solid_effect = 1;
    uint8_t new_color[] = {hue, saturation};
    bool ok = qmk_set_menu_value(device, 0x02, &solid_effect, 1) &&
              qmk_set_menu_value(device, 0x04, new_color, sizeof(new_color)) &&
              qmk_set_menu_value(device, 0x01, &brightness, 1);
    if (ok) {
        printf("New-generation FEKER whole-board test active for %d seconds.\n",
               QMK_TEST_HOLD_SECONDS);
        fflush(stdout);
        sleep(QMK_TEST_HOLD_SECONDS);
    }

    bool restored = qmk_set_menu_value(device, 0x02, &old_effect, 1) &&
                    qmk_set_menu_value(device, 0x04, old_color,
                                       sizeof(old_color)) &&
                    qmk_set_menu_value(device, 0x01, &old_brightness, 1);
    hid_close(device);
    return ok && restored ? 0 : 1;
}

static hid_device *open_rgb_device(void) {
    struct hid_device_info *devices = hid_enumerate(FEKER_VID, FEKER_PID);
    struct hid_device_info *chosen = NULL;

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (item->usage_page == FEKER_USAGE_PAGE && item->usage == FEKER_USAGE) {
            chosen = item;
            break;
        }
    }

    if (chosen == NULL) {
        hid_free_enumeration(devices);
        fprintf(stderr,
                "FEKER RGB interface not found (expected usage page 0x%04X, usage 0x%04X).\n",
                FEKER_USAGE_PAGE, FEKER_USAGE);
        return NULL;
    }

    hid_device *device = hid_open_path(chosen->path);
    if (device == NULL) {
        const wchar_t *error = hid_error(NULL);
        fprintf(stderr, "Unable to open FEKER RGB interface: ");
        if (error != NULL) {
            fwprintf(stderr, L"%ls\n", error);
        } else {
            fputs("unknown HID error\n", stderr);
        }
    }
    hid_free_enumeration(devices);
    return device;
}

static void compute_checksum(uint8_t packet[REPORT_SIZE]) {
    uint16_t checksum = 0;
    for (size_t index = 3; index < REPORT_SIZE; index++) {
        checksum = (uint16_t)(checksum + packet[index]);
    }
    packet[1] = (uint8_t)(checksum & 0xFF);
    packet[2] = (uint8_t)(checksum >> 8);
}

static bool send_packet(hid_device *device, uint8_t packet[REPORT_SIZE]) {
    int written = hid_write(device, packet, REPORT_SIZE);
    if (written != REPORT_SIZE) {
        fwprintf(stderr, L"HID write failed: %ls\n", hid_error(device));
        return false;
    }

    uint8_t response[REPORT_SIZE];
    memset(response, 0, sizeof(response));
    int received = hid_read_timeout(device, response, sizeof(response), 250);
    if (received < 0) {
        fwprintf(stderr, L"HID reply failed: %ls\n", hid_error(device));
        return false;
    }
    if (received == 0) {
        fprintf(stderr, "FEKER did not acknowledge command 0x%02X.\n", packet[3]);
        return false;
    }
    if (verbose_packets) {
        printf("reply cmd=0x%02X size=%d:", packet[3], received);
        int shown = received < 24 ? received : 24;
        for (int index = 0; index < shown; index++) {
            printf(" %02X", response[index]);
        }
        putchar('\n');
    }
    if (received >= 4 && response[3] != packet[3]) {
        fprintf(stderr,
                "FEKER returned an unexpected reply (sent 0x%02X, received 0x%02X).\n",
                packet[3], response[3]);
        return false;
    }
    if (received >= 8 && response[7] != 0) {
        fprintf(stderr,
                "FEKER rejected command 0x%02X with device status 0x%02X.\n",
                packet[3], response[7]);
        return false;
    }
    return true;
}

static bool send_query(hid_device *device, uint8_t command,
                       uint16_t offset, uint8_t size) {
    uint8_t packet[REPORT_SIZE];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x04;
    packet[3] = command;
    packet[4] = size;
    packet[5] = (uint8_t)(offset & 0xFF);
    packet[6] = (uint8_t)(offset >> 8);
    compute_checksum(packet);
    return send_packet(device, packet);
}

static int probe_device(void) {
    hid_device *device = open_rgb_device();
    if (device == NULL) {
        return 1;
    }

    verbose_packets = true;
    puts("Query 1/3: capabilities (command 03, offset 0000, size 7)");
    bool ok = send_query(device, 0x03, 0x0000, 7);
    puts("Query 2/3: current profile (command 05, offset 0000, size 1)");
    ok = send_query(device, 0x05, 0x0000, 1) && ok;
    puts("Query 3/3: first profile RGB config (command 05, offset 0001, size 30)");
    ok = send_query(device, 0x05, 0x0001, 30) && ok;
    verbose_packets = false;

    hid_close(device);
    return ok ? 0 : 1;
}

static bool set_colors(hid_device *device, const rgb_t colors[LED_COUNT]) {
    uint8_t color_bytes[COLOR_BYTES];
    for (size_t index = 0; index < LED_COUNT; index++) {
        color_bytes[index * 3] = colors[index].r;
        color_bytes[index * 3 + 1] = colors[index].g;
        color_bytes[index * 3 + 2] = colors[index].b;
    }

    for (size_t offset = 0; offset < COLOR_BYTES; offset += MAX_COLOR_PAYLOAD) {
        size_t remaining = COLOR_BYTES - offset;
        size_t payload_size = remaining < MAX_COLOR_PAYLOAD ? remaining : MAX_COLOR_PAYLOAD;
        uint8_t packet[REPORT_SIZE];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x04;
        /* Alice80 uses EVision V2's live/dynamic RGB command. */
        packet[3] = 0x12;
        packet[4] = (uint8_t)payload_size;
        packet[5] = (uint8_t)(offset & 0xFF);
        packet[6] = (uint8_t)(offset >> 8);
        memcpy(&packet[8], &color_bytes[offset], payload_size);
        compute_checksum(packet);
        if (!send_packet(device, packet)) {
            return false;
        }
    }
    return true;
}

static bool refresh_direct_mode(hid_device *device) {
    /*
     * EVision V2 direct mode expires unless it receives this one-byte query.
     * OpenRGB sends the same command at least once every 200 ms.
     */
    return send_query(device, 0x12, DIRECT_REFRESH_OFFSET, 1);
}

static bool end_direct_mode(hid_device *device) {
    return send_query(device, 0x13, 0, 0);
}

static bool hold_direct_mode(hid_device *device, int ticks) {
    for (int tick = 0; tick < ticks; tick++) {
        usleep(DIRECT_REFRESH_INTERVAL_US);
        if (!refresh_direct_mode(device)) {
            return false;
        }
    }
    return true;
}

static int scan_led_bits(void) {
    hid_device *device = open_rgb_device();
    if (device == NULL) {
        return 1;
    }

    rgb_t colors[LED_COUNT];
    bool ok = true;

    /* Blue marker frame lets the camera observer synchronize to the scan. */
    fill_colors(colors, (rgb_t){0x00, 0x00, 0xFF});
    puts("scan frame: marker");
    fflush(stdout);
    ok = set_colors(device, colors) && hold_direct_mode(device, 60);

    for (int bit = 0; ok && bit < 7; bit++) {
        fill_colors(colors, (rgb_t){0, 0, 0});
        for (int led = 0; led < LED_COUNT; led++) {
            if ((led & (1 << bit)) != 0) {
                colors[led] = (rgb_t){0xFF, 0xFF, 0xFF};
            }
        }
        printf("scan frame: bit %d\n", bit);
        fflush(stdout);
        ok = set_colors(device, colors) && hold_direct_mode(device, 60);
    }

    if (ok) {
        ok = end_direct_mode(device);
    }
    hid_close(device);
    return ok ? 0 : 1;
}

static void fill_colors(rgb_t colors[LED_COUNT], rgb_t color) {
    for (size_t index = 0; index < LED_COUNT; index++) {
        colors[index] = color;
    }
}

static int apply_colors(rgb_t colors[LED_COUNT]) {
    hid_device *device = open_rgb_device();
    if (device == NULL) {
        return 1;
    }

    bool ok = set_colors(device, colors);
    if (ok) {
        puts("FEKER live RGB update acknowledged; holding direct mode for 60 seconds.");
        fflush(stdout);
        ok = hold_direct_mode(device, DIRECT_TEST_HOLD_TICKS);
    }
    if (ok) {
        ok = end_direct_mode(device);
    }
    hid_close(device);
    if (!ok) {
        return 1;
    }
    puts("Direct-mode test completed.");
    return 0;
}

int main(int argc, char **argv) {
    if (hid_init() != 0) {
        fprintf(stderr, "Unable to initialize HIDAPI.\n");
        return 1;
    }

    if (argc < 2) {
        usage(argv[0]);
        hid_exit();
        return 2;
    }

    int result = 0;
    if (strcmp(argv[1], "list") == 0) {
        result = list_devices();
    } else if (strcmp(argv[1], "probe") == 0) {
        result = probe_device();
    } else if (strcmp(argv[1], "qmk-probe") == 0) {
        result = probe_qmk_device();
    } else if (strcmp(argv[1], "qmk-all") == 0 && argc == 3) {
        result = test_qmk_whole_board(parse_color(argv[2]));
    } else if (strcmp(argv[1], "scan") == 0) {
        result = scan_led_bits();
    } else {
        rgb_t colors[LED_COUNT];
        fill_colors(colors, (rgb_t){0, 0, 0});

        if (strcmp(argv[1], "off") == 0) {
            result = apply_colors(colors);
        } else if (strcmp(argv[1], "all") == 0 && argc == 3) {
            fill_colors(colors, parse_color(argv[2]));
            result = apply_colors(colors);
        } else if (strcmp(argv[1], "key") == 0 && argc == 4) {
            int key = atoi(argv[2]);
            if (key < 1 || key > 9) {
                fprintf(stderr, "Key slot must be from 1 to 9.\n");
                result = 2;
            } else {
                colors[number_key_leds[key - 1]] = parse_color(argv[3]);
                result = apply_colors(colors);
            }
        } else if (strcmp(argv[1], "led") == 0 && argc == 4) {
            int led = atoi(argv[2]);
            if (led < 0 || led >= LED_COUNT) {
                fprintf(stderr, "LED index must be from 0 to %d.\n", LED_COUNT - 1);
                result = 2;
            } else {
                colors[led] = parse_color(argv[3]);
                result = apply_colors(colors);
            }
        } else if (strcmp(argv[1], "slots") == 0) {
            for (int index = 2; index < argc; index++) {
                const char *separator = strchr(argv[index], '=');
                if (separator == NULL || separator == argv[index] || separator[1] == '\0') {
                    fprintf(stderr, "Invalid slot '%s'; expected N=RRGGBB.\n", argv[index]);
                    result = 2;
                    break;
                }
                char key_text[8];
                size_t key_length = (size_t)(separator - argv[index]);
                if (key_length >= sizeof(key_text)) {
                    fprintf(stderr, "Invalid slot '%s'.\n", argv[index]);
                    result = 2;
                    break;
                }
                memcpy(key_text, argv[index], key_length);
                key_text[key_length] = '\0';
                int key = atoi(key_text);
                if (key < 1 || key > 9) {
                    fprintf(stderr, "Key slot must be from 1 to 9.\n");
                    result = 2;
                    break;
                }
                colors[number_key_leds[key - 1]] = parse_color(separator + 1);
            }
            if (result == 0) {
                result = apply_colors(colors);
            }
        } else {
            usage(argv[0]);
            result = 2;
        }
    }

    hid_exit();
    return result;
}
