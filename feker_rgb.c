#include <errno.h>
#include <hidapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define FEKER_VID 0x320F
#define FEKER_PID 0x5055
#define FEKER_USAGE_PAGE 0xFF1C
#define FEKER_USAGE 0x0092
#define REPORT_SIZE 64
#define LED_COUNT 128
#define COLOR_BYTES (LED_COUNT * 3)
#define MAX_COLOR_PAYLOAD 0x36

static const int number_key_leds[9] = {22, 23, 24, 25, 26, 27, 28, 29, 30};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static void usage(const char *program) {
    fprintf(stderr,
            "FEKER Alice80 per-key RGB control\n\n"
            "Usage:\n"
            "  %s list\n"
            "  %s off\n"
            "  %s all RRGGBB\n"
            "  %s key 1-9 RRGGBB\n"
            "  %s led 0-127 RRGGBB\n"
            "  %s slots [1=RRGGBB ... 9=RRGGBB]\n"
            "\nExamples:\n"
            "  %s key 1 00FF00\n"
            "  %s slots 1=00FF00 3=FFBF00\n",
            program, program, program, program, program, program, program, program);
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
    struct hid_device_info *devices = hid_enumerate(FEKER_VID, FEKER_PID);
    if (devices == NULL) {
        fprintf(stderr, "FEKER 320F:5055 is not connected.\n");
        return 1;
    }

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        printf("path=%s\n", item->path != NULL ? item->path : "(none)");
        printf("  product=");
        print_wide(item->product_string);
        printf("\n  usage_page=0x%04X usage=0x%04X interface=%d\n",
               item->usage_page, item->usage, item->interface_number);
    }
    hid_free_enumeration(devices);
    return 0;
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
    if (received >= 4 && response[3] != packet[3]) {
        fprintf(stderr,
                "FEKER returned an unexpected reply (sent 0x%02X, received 0x%02X).\n",
                packet[3], response[3]);
        return false;
    }
    return true;
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
    hid_close(device);
    if (!ok) {
        return 1;
    }
    puts("FEKER live RGB update acknowledged.");
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
