#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <hidapi.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ServiceManagement/ServiceManagement.h>
#endif

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
#define MAX_THREADS 128
#define MAX_TITLE 256

/*
 * Column-major matrix with six slots per column.  Slot 6 is intentionally
 * blank and is used for the direct-mode keepalive, so number 1 begins at 7.
 */
static const int number_key_leds[9] = {7, 13, 19, 25, 31, 37, 43, 49, 55};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef enum {
    SLOT_OFF = 0,
    SLOT_WORKING,
    SLOT_UNREAD,
    SLOT_IDLE,
    SLOT_WAITING,
    SLOT_ERROR,
} slot_status_t;

typedef enum {
    LIGHTING_PER_KEY = 0,
    LIGHTING_WHOLE_BOARD,
} lighting_mode_t;

typedef enum {
    RGB_PROTOCOL_NONE = 0,
    RGB_PROTOCOL_EVISION,
    RGB_PROTOCOL_QMK_VIA,
} rgb_protocol_t;

typedef struct {
    char thread_id[64];
    char rollout_path[PATH_MAX];
    char title[MAX_TITLE];
    off_t offset;
    int slot;
    slot_status_t status;
    time_t touched_at;
    bool initialized;
} watched_thread_t;

static volatile sig_atomic_t should_stop = 0;
static watched_thread_t watched[MAX_THREADS];
static size_t watched_count = 0;
static bool hid_permission_warning_printed = false;
static bool hid_refresh_warning_printed = false;
static hid_device *rgb_device = NULL;
static bool rgb_frame_active = false;
static rgb_protocol_t rgb_protocol = RGB_PROTOCOL_NONE;
static bool qmk_generation_warning_printed = false;
static bool qmk_original_saved = false;
static uint8_t qmk_original_brightness = 0;
static uint8_t qmk_original_effect = 0;
static uint8_t qmk_original_color[2] = {0, 0};
static char app_support_dir[PATH_MAX];
static char slot_state_path[PATH_MAX];
static char selected_slot_path[PATH_MAX];
static char test_request_path[PATH_MAX];
static char lighting_mode_path[PATH_MAX];
static char task_lights_enabled_path[PATH_MAX];
static char database_path[PATH_MAX];
static char log_path[PATH_MAX];
static char executable_path[PATH_MAX];
static slot_status_t test_statuses[9];
static time_t test_override_until = 0;
static lighting_mode_t lighting_mode = LIGHTING_PER_KEY;
static bool task_lights_enabled = true;
static bool setuid_installation = false;
static uid_t real_user_id = 0;

static void handle_signal(int signal_number) {
    (void)signal_number;
    should_stop = 1;
}

static void log_line(const char *level, const char *message) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    fprintf(stderr, "%s [%s] %s\n", timestamp, level, message);
    fflush(stderr);
}

static bool configure_process_identity(int argc, char **argv) {
    real_user_id = getuid();
    const char *delegated_uid = getenv("FEKER_USER_UID");
    if (geteuid() == 0 && real_user_id == 0 &&
        delegated_uid != NULL && delegated_uid[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(delegated_uid, &end, 10);
        bool daemon_mode =
            argc == 1 || (argc == 2 && strcmp(argv[1], "--daemon") == 0);
        if (!daemon_mode || end == delegated_uid || *end != '\0' ||
            parsed == 0 || parsed > UINT_MAX) {
            fputs("Invalid delegated helper identity.\n", stderr);
            return false;
        }
        real_user_id = (uid_t)parsed;
        struct passwd *account = getpwuid(real_user_id);
        if (account == NULL || account->pw_dir == NULL ||
            setenv("HOME", account->pw_dir, 1) != 0 ||
            seteuid(real_user_id) != 0) {
            fputs("Unable to enter the delegated user's context.\n", stderr);
            return false;
        }
        setuid_installation = true;
        return true;
    }
    if (geteuid() != 0 || real_user_id == 0) {
        return true;
    }

    bool daemon_mode =
        argc == 1 || (argc == 2 && strcmp(argv[1], "--daemon") == 0);
    if (!daemon_mode) {
        fputs("The installed privileged helper only accepts --daemon.\n", stderr);
        return false;
    }

    struct passwd *account = getpwuid(real_user_id);
    if (account == NULL || account->pw_dir == NULL ||
        account->pw_dir[0] == '\0') {
        fputs("Unable to resolve the invoking user.\n", stderr);
        return false;
    }
    if (setenv("HOME", account->pw_dir, 1) != 0 ||
        seteuid(real_user_id) != 0) {
        fputs("Unable to enter the invoking user's security context.\n", stderr);
        return false;
    }
    setuid_installation = true;
    return true;
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return true;
    }
    fprintf(stderr, "Unable to create %s: %s\n", path, strerror(errno));
    return false;
}

static bool initialize_paths(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fputs("HOME is unavailable.\n", stderr);
        return false;
    }

    char library_dir[PATH_MAX];
    char application_support_dir[PATH_MAX];
    char logs_dir[PATH_MAX];
    snprintf(library_dir, sizeof(library_dir), "%s/Library", home);
    snprintf(application_support_dir, sizeof(application_support_dir),
             "%s/Application Support", library_dir);
    snprintf(logs_dir, sizeof(logs_dir), "%s/Logs", library_dir);
    snprintf(app_support_dir, sizeof(app_support_dir),
             "%s/Feker Codex Bridge", application_support_dir);
    snprintf(slot_state_path, sizeof(slot_state_path), "%s/slots.tsv", app_support_dir);
    snprintf(selected_slot_path, sizeof(selected_slot_path),
             "%s/selected-slot.txt", app_support_dir);
    snprintf(test_request_path, sizeof(test_request_path),
             "%s/test-request.tsv", app_support_dir);
    snprintf(lighting_mode_path, sizeof(lighting_mode_path),
             "%s/lighting-mode.txt", app_support_dir);
    snprintf(task_lights_enabled_path, sizeof(task_lights_enabled_path),
             "%s/task-lights-enabled.txt", app_support_dir);
    snprintf(database_path, sizeof(database_path), "%s/.codex/state_5.sqlite", home);
    snprintf(log_path, sizeof(log_path), "%s/FekerCodexBridge.log", logs_dir);

    return ensure_directory(library_dir) &&
           ensure_directory(application_support_dir) &&
           ensure_directory(logs_dir) &&
           ensure_directory(app_support_dir);
}

static bool initialize_executable_path(void) {
#if defined(__APPLE__)
    uint32_t size = sizeof(executable_path);
    char unresolved[PATH_MAX];
    if (_NSGetExecutablePath(unresolved, &size) != 0) {
        fputs("Unable to resolve the executable path.\n", stderr);
        return false;
    }
    if (realpath(unresolved, executable_path) == NULL) {
        snprintf(executable_path, sizeof(executable_path), "%s", unresolved);
    }
    return true;
#else
    return false;
#endif
}

static void configure_background_logging(void) {
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0 && (errno == EACCES || errno == EPERM)) {
        char legacy_log_path[PATH_MAX];
        int length = snprintf(legacy_log_path, sizeof(legacy_log_path),
                              "%s.legacy-%lld", log_path,
                              (long long)time(NULL));
        if (length > 0 && (size_t)length < sizeof(legacy_log_path) &&
            rename(log_path, legacy_log_path) == 0) {
            log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        }
    }
    if (log_fd < 0) {
        fprintf(stderr, "Unable to open %s: %s\n", log_path, strerror(errno));
        return;
    }
    if (dup2(log_fd, STDOUT_FILENO) < 0 ||
        dup2(log_fd, STDERR_FILENO) < 0) {
        fprintf(stderr, "Unable to redirect background logging: %s\n",
                strerror(errno));
    }
    if (log_fd > STDERR_FILENO) {
        close(log_fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static void compute_checksum(uint8_t packet[REPORT_SIZE]) {
    uint16_t checksum = 0;
    for (size_t index = 3; index < REPORT_SIZE; index++) {
        checksum = (uint16_t)(checksum + packet[index]);
    }
    packet[1] = (uint8_t)(checksum & 0xFF);
    packet[2] = (uint8_t)(checksum >> 8);
}

static hid_device *open_rgb_device(void) {
    bool elevated = false;
    if (setuid_installation) {
        if (seteuid(0) != 0) {
            log_line("ERROR", "Unable to acquire the narrow HID privilege.");
            return NULL;
        }
        elevated = true;
    }

    struct hid_device_info *devices = hid_enumerate(FEKER_VID, FEKER_PID);
    const char *chosen_path = NULL;
    char path_copy[PATH_MAX];
    rgb_protocol_t chosen_protocol = RGB_PROTOCOL_NONE;

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (item->usage_page == FEKER_USAGE_PAGE && item->usage == FEKER_USAGE &&
            item->path != NULL) {
            snprintf(path_copy, sizeof(path_copy), "%s", item->path);
            chosen_path = path_copy;
            chosen_protocol = RGB_PROTOCOL_EVISION;
            break;
        }
    }
    hid_free_enumeration(devices);

    if (chosen_path == NULL) {
        devices = hid_enumerate(FEKER_QMK_VID, FEKER_QMK_PID);
        for (struct hid_device_info *item = devices; item != NULL;
             item = item->next) {
            if (item->usage_page == FEKER_QMK_USAGE_PAGE &&
                item->usage == FEKER_QMK_USAGE && item->path != NULL) {
                snprintf(path_copy, sizeof(path_copy), "%s", item->path);
                chosen_path = path_copy;
                chosen_protocol = RGB_PROTOCOL_QMK_VIA;
                break;
            }
        }
        hid_free_enumeration(devices);
    }

    if (chosen_path == NULL) {
        if (elevated) {
            (void)seteuid(real_user_id);
        }
        if (!hid_permission_warning_printed) {
            log_line("ERROR",
                     "No supported old- or new-generation FEKER Alice80 RGB "
                     "interface was found.");
            hid_permission_warning_printed = true;
        }
        return NULL;
    }

    hid_device *device = hid_open_path(chosen_path);
    if (elevated && seteuid(real_user_id) != 0) {
        log_line("ERROR", "Unable to drop the narrow HID privilege.");
        if (device != NULL) {
            hid_close(device);
        }
        return NULL;
    }
    if (device == NULL && !hid_permission_warning_printed) {
        log_line("PERMISSION",
                 "macOS blocked FEKER RGB access. Enable Input Monitoring for "
                 "Feker Codex Bridge in System Settings > Privacy & Security.");
        hid_permission_warning_printed = true;
    }
    if (device != NULL) {
        hid_permission_warning_printed = false;
        rgb_protocol = chosen_protocol;
        if (rgb_protocol == RGB_PROTOCOL_QMK_VIA &&
            !qmk_generation_warning_printed) {
            log_line("DEVICE",
                     "New-generation FEKER QMK/VIA keyboard detected; "
                     "using whole-keyboard status colors.");
            qmk_generation_warning_printed = true;
        }
    }
    return device;
}

static bool send_packet(hid_device *device, uint8_t packet[REPORT_SIZE]) {
    int written = hid_write(device, packet, REPORT_SIZE);
    if (written != REPORT_SIZE) {
        return false;
    }

    uint8_t response[REPORT_SIZE];
    memset(response, 0, sizeof(response));
    int received = hid_read_timeout(device, response, sizeof(response), 300);
    return received > 0 &&
           (received < 4 || response[3] == packet[3]) &&
           (received < 8 || response[7] == 0);
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

static bool qmk_exchange(hid_device *device, uint8_t command,
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
    if (hid_write(device, report, sizeof(report)) != (int)sizeof(report)) {
        return false;
    }
    memset(response, 0, QMK_REPORT_SIZE);
    int received = hid_read_timeout(device, response, QMK_REPORT_SIZE, 300);
    return received > 0 && response[0] == command;
}

static bool qmk_read_menu_value(hid_device *device, uint8_t menu_command,
                                uint8_t *values, size_t value_count) {
    uint8_t request[] = {0x03, menu_command};
    uint8_t response[QMK_REPORT_SIZE];
    if (!qmk_exchange(device, 0x08, request, sizeof(request), response)) {
        return false;
    }
    memcpy(values, &response[3], value_count);
    return true;
}

static bool qmk_set_menu_value(hid_device *device, uint8_t menu_command,
                               const uint8_t *values, size_t value_count) {
    if (value_count > QMK_REPORT_SIZE - 3) {
        return false;
    }
    uint8_t request[QMK_REPORT_SIZE - 1];
    request[0] = 0x03;
    request[1] = menu_command;
    memcpy(&request[2], values, value_count);
    uint8_t response[QMK_REPORT_SIZE];
    return qmk_exchange(device, 0x07, request, value_count + 2, response);
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

static bool capture_qmk_original(void) {
    if (qmk_original_saved || rgb_device == NULL ||
        rgb_protocol != RGB_PROTOCOL_QMK_VIA) {
        return true;
    }
    if (!qmk_read_menu_value(rgb_device, 0x01,
                             &qmk_original_brightness, 1) ||
        !qmk_read_menu_value(rgb_device, 0x02, &qmk_original_effect, 1) ||
        !qmk_read_menu_value(rgb_device, 0x04, qmk_original_color,
                             sizeof(qmk_original_color))) {
        return false;
    }
    qmk_original_saved = true;
    return true;
}

static bool apply_qmk_whole_board(rgb_t color) {
    uint8_t hue;
    uint8_t saturation;
    uint8_t brightness;
    rgb_to_hsv(color, &hue, &saturation, &brightness);
    uint8_t solid_effect = 1;
    uint8_t hsv_color[] = {hue, saturation};
    return qmk_set_menu_value(rgb_device, 0x02, &solid_effect, 1) &&
           qmk_set_menu_value(rgb_device, 0x04, hsv_color,
                              sizeof(hsv_color)) &&
           qmk_set_menu_value(rgb_device, 0x01, &brightness, 1);
}

static void restore_qmk_original(void) {
    if (!qmk_original_saved || rgb_device == NULL ||
        rgb_protocol != RGB_PROTOCOL_QMK_VIA) {
        return;
    }
    (void)qmk_set_menu_value(rgb_device, 0x02, &qmk_original_effect, 1);
    (void)qmk_set_menu_value(rgb_device, 0x04, qmk_original_color,
                             sizeof(qmk_original_color));
    (void)qmk_set_menu_value(rgb_device, 0x01, &qmk_original_brightness, 1);
}

static void close_rgb_device(bool end_direct_mode) {
    if (rgb_device == NULL) {
        return;
    }
    if (end_direct_mode && rgb_protocol == RGB_PROTOCOL_QMK_VIA) {
        restore_qmk_original();
    } else if (end_direct_mode && rgb_frame_active) {
        (void)send_query(rgb_device, 0x13, 0, 0);
    }
    hid_close(rgb_device);
    rgb_device = NULL;
    rgb_frame_active = false;
    rgb_protocol = RGB_PROTOCOL_NONE;
    qmk_original_saved = false;
}

static bool ensure_rgb_device(void) {
    if (rgb_device != NULL) {
        return true;
    }
    rgb_device = open_rgb_device();
    return rgb_device != NULL;
}

static bool qmk_device_present(void) {
    struct hid_device_info *devices =
        hid_enumerate(FEKER_QMK_VID, FEKER_QMK_PID);
    bool found = false;
    for (struct hid_device_info *item = devices; item != NULL;
         item = item->next) {
        if (item->usage_page == FEKER_QMK_USAGE_PAGE &&
            item->usage == FEKER_QMK_USAGE) {
            found = true;
            break;
        }
    }
    hid_free_enumeration(devices);
    return found;
}

static bool refresh_direct_mode(void) {
    if (!rgb_frame_active || rgb_device == NULL) {
        return false;
    }
    if (rgb_protocol == RGB_PROTOCOL_QMK_VIA) {
        return true;
    }
    if (send_query(rgb_device, 0x12, DIRECT_REFRESH_OFFSET, 1)) {
        hid_refresh_warning_printed = false;
        return true;
    }
    if (!hid_refresh_warning_printed) {
        log_line("ERROR", "FEKER direct-mode keepalive failed; reconnecting.");
        hid_refresh_warning_printed = true;
    }
    close_rgb_device(false);
    return false;
}

static bool apply_slot_colors(const slot_status_t statuses[9]) {
    /* Exact status palette used by Codex Micro in Codex 26.721.31836. */
    static const rgb_t colors_by_status[] = {
        [SLOT_OFF] = {0x00, 0x00, 0x00},
        [SLOT_WORKING] = {0x30, 0x4F, 0xFE},
        [SLOT_UNREAD] = {0x00, 0xFF, 0x4C},
        [SLOT_IDLE] = {0xFF, 0xFF, 0xFF},
        [SLOT_WAITING] = {0xFF, 0x6D, 0x00},
        [SLOT_ERROR] = {0xFF, 0x00, 0x33},
    };
    static const int priority[] = {
        [SLOT_OFF] = 0,
        [SLOT_IDLE] = 1,
        [SLOT_UNREAD] = 2,
        [SLOT_WORKING] = 3,
        [SLOT_WAITING] = 4,
        [SLOT_ERROR] = 5,
    };
    slot_status_t aggregate = SLOT_OFF;
    for (int index = 0; index < 9; index++) {
        if (priority[statuses[index]] > priority[aggregate]) {
            aggregate = statuses[index];
        }
    }

    if (!ensure_rgb_device()) {
        return false;
    }
    if (rgb_protocol == RGB_PROTOCOL_QMK_VIA) {
        bool ok = capture_qmk_original() &&
                  apply_qmk_whole_board(colors_by_status[aggregate]);
        if (!ok) {
            log_line("ERROR",
                     "New-generation FEKER did not acknowledge the VIA RGB "
                     "update.");
            close_rgb_device(false);
        } else {
            rgb_frame_active = true;
        }
        return ok;
    }

    rgb_t led_colors[LED_COUNT];
    if (lighting_mode == LIGHTING_WHOLE_BOARD) {
        for (size_t index = 0; index < LED_COUNT; index++) {
            led_colors[index] = colors_by_status[aggregate];
        }
    } else {
        memset(led_colors, 0, sizeof(led_colors));
        for (int index = 0; index < 9; index++) {
            led_colors[number_key_leds[index]] = colors_by_status[statuses[index]];
        }
    }

    bool ok = true;

    uint8_t color_bytes[COLOR_BYTES];
    for (size_t index = 0; index < LED_COUNT; index++) {
        color_bytes[index * 3] = led_colors[index].r;
        color_bytes[index * 3 + 1] = led_colors[index].g;
        color_bytes[index * 3 + 2] = led_colors[index].b;
    }

    for (size_t offset = 0; ok && offset < COLOR_BYTES; offset += MAX_COLOR_PAYLOAD) {
        size_t remaining = COLOR_BYTES - offset;
        size_t payload_size = remaining < MAX_COLOR_PAYLOAD ? remaining : MAX_COLOR_PAYLOAD;
        uint8_t packet[REPORT_SIZE];
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x04;
        /* EVision V2 live/dynamic RGB command used by this Alice80. */
        packet[3] = 0x12;
        packet[4] = (uint8_t)payload_size;
        packet[5] = (uint8_t)(offset & 0xFF);
        packet[6] = (uint8_t)(offset >> 8);
        memcpy(&packet[8], &color_bytes[offset], payload_size);
        compute_checksum(packet);
        ok = send_packet(rgb_device, packet);
    }

    if (!ok) {
        log_line("ERROR", "FEKER did not acknowledge the RGB update.");
        close_rgb_device(false);
    } else {
        rgb_frame_active = true;
        hid_refresh_warning_printed = false;
    }
    return ok;
}

static void collect_statuses(slot_status_t statuses[9]) {
    for (int index = 0; index < 9; index++) {
        statuses[index] = SLOT_OFF;
    }
    for (size_t index = 0; index < watched_count; index++) {
        if (watched[index].slot >= 1 && watched[index].slot <= 9) {
            statuses[watched[index].slot - 1] = watched[index].status;
        }
    }
}

static void refresh_lights(void) {
    if (!task_lights_enabled) {
        close_rgb_device(true);
        return;
    }
    slot_status_t statuses[9];
    if (test_override_until > time(NULL)) {
        memcpy(statuses, test_statuses, sizeof(statuses));
    } else {
        if (test_override_until != 0) {
            test_override_until = 0;
        }
        collect_statuses(statuses);
    }
    apply_slot_colors(statuses);
}

static bool write_atomic_slot_file(const char *path, int slot, int value) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%d\t%d\t%lld\n", slot, value, (long long)time(NULL));
    if (fclose(file) != 0 || rename(temporary_path, path) != 0) {
        unlink(temporary_path);
        return false;
    }
    return true;
}

static bool save_state(void) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", slot_state_path);
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    for (size_t index = 0; index < watched_count; index++) {
        watched_thread_t *item = &watched[index];
        if (item->slot > 0) {
            fprintf(file, "%d\t%d\t%lld\t%s\t%s\n", item->slot,
                    (int)item->status, (long long)item->touched_at,
                    item->thread_id, item->title);
        }
    }
    if (fclose(file) != 0 || rename(temporary_path, slot_state_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    return true;
}

static watched_thread_t *find_thread(const char *thread_id) {
    for (size_t index = 0; index < watched_count; index++) {
        if (strcmp(watched[index].thread_id, thread_id) == 0) {
            return &watched[index];
        }
    }
    return NULL;
}

static void update_thread_status(watched_thread_t *item, slot_status_t status) {
    item->status = status;
    item->touched_at = time(NULL);

    char message[512];
    const char *status_name = status == SLOT_WORKING ? "working" :
                              status == SLOT_UNREAD ? "unread" :
                              status == SLOT_IDLE ? "idle" :
                              status == SLOT_WAITING ? "waiting" :
                              status == SLOT_ERROR ? "error" : "off";
    snprintf(message, sizeof(message), "Task%s -> %s: %s",
             item->slot > 0 ? " in a visible shortcut slot" : "",
             status_name, item->title[0] != '\0' ? item->title : item->thread_id);
    log_line("TASK", message);
    if (item->slot > 0) {
        save_state();
        refresh_lights();
    }
}

static slot_status_t status_from_line(const char *line, bool *matched) {
    *matched = true;
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\"") != NULL) {
        return SLOT_WORKING;
    }
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\"") != NULL) {
        return SLOT_UNREAD;
    }
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"request_user_input\"") != NULL ||
        strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"elicitation_request\"") != NULL ||
        strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"approval_request\"") != NULL) {
        return SLOT_WAITING;
    }
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\"") != NULL ||
        strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_failed\"") != NULL ||
        strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"error\"") != NULL) {
        return SLOT_ERROR;
    }
    *matched = false;
    return SLOT_OFF;
}

static void sanitize_title(char *title) {
    for (char *cursor = title; *cursor != '\0'; cursor++) {
        if (*cursor == '\n' || *cursor == '\r' || *cursor == '\t') {
            *cursor = ' ';
        }
    }
}

static void initialize_rollout_position(watched_thread_t *item) {
    struct stat file_info;
    bool recently_updated = stat(item->rollout_path, &file_info) == 0 &&
                            difftime(time(NULL), file_info.st_mtime) <= 300.0;
    FILE *file = fopen(item->rollout_path, "r");
    if (file == NULL) {
        return;
    }
    if (fseeko(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    off_t size = ftello(file);
    off_t start = recently_updated ? 0 : (size > 131072 ? size - 131072 : 0);
    fseeko(file, start, SEEK_SET);
    if (start > 0) {
        int character;
        while ((character = fgetc(file)) != '\n' && character != EOF) {
        }
    }

    slot_status_t last_status = SLOT_OFF;
    bool saw_event = false;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        bool matched = false;
        slot_status_t status = status_from_line(line, &matched);
        if (matched) {
            last_status = status;
            saw_event = true;
        }
    }
    free(line);
    item->offset = size;
    item->initialized = true;
    fclose(file);

    if (recently_updated && saw_event &&
        (last_status == SLOT_WORKING || last_status == SLOT_WAITING ||
         last_status == SLOT_UNREAD || last_status == SLOT_ERROR)) {
        item->status = last_status;
        item->touched_at = time(NULL);
    }
}

static void process_appended_events(watched_thread_t *item) {
    FILE *file = fopen(item->rollout_path, "r");
    if (file == NULL) {
        return;
    }
    if (fseeko(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    off_t size = ftello(file);
    if (size < item->offset) {
        item->offset = 0;
    }
    if (size == item->offset) {
        fclose(file);
        return;
    }
    fseeko(file, item->offset, SEEK_SET);
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        bool matched = false;
        slot_status_t status = status_from_line(line, &matched);
        if (matched) {
            update_thread_status(item, status);
        }
    }
    free(line);
    item->offset = ftello(file);
    fclose(file);
}

static void discover_threads(sqlite3 *database) {
    static const char *query =
        "SELECT id, rollout_path, title FROM threads "
        "WHERE archived = 0 AND rollout_path <> '' AND preview <> '' "
        "ORDER BY recency_at_ms DESC, id DESC LIMIT 128";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK) {
        return;
    }

    int previous_slots[MAX_THREADS];
    size_t previous_count = watched_count;
    for (size_t index = 0; index < watched_count; index++) {
        previous_slots[index] = watched[index].slot;
        watched[index].slot = 0;
    }

    int visible_position = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *thread_id = (const char *)sqlite3_column_text(statement, 0);
        const char *path = (const char *)sqlite3_column_text(statement, 1);
        const char *title = (const char *)sqlite3_column_text(statement, 2);
        if (thread_id == NULL || path == NULL) {
            continue;
        }
        watched_thread_t *item = find_thread(thread_id);
        if (item == NULL) {
            if (watched_count >= MAX_THREADS) {
                continue;
            }
            item = &watched[watched_count++];
            memset(item, 0, sizeof(*item));
            item->status = SLOT_IDLE;
            snprintf(item->thread_id, sizeof(item->thread_id), "%s", thread_id);
            snprintf(item->rollout_path, sizeof(item->rollout_path), "%s", path);
            snprintf(item->title, sizeof(item->title), "%s", title != NULL ? title : "");
            sanitize_title(item->title);
            initialize_rollout_position(item);
        }
        visible_position++;
        if (visible_position <= 9) {
            item->slot = visible_position;
        }
    }
    sqlite3_finalize(statement);

    bool slots_changed = watched_count != previous_count;
    for (size_t index = 0; !slots_changed && index < watched_count; index++) {
        int previous = index < previous_count ? previous_slots[index] : 0;
        slots_changed = watched[index].slot != previous;
    }
    if (slots_changed) {
        save_state();
        refresh_lights();
    }
}

static bool file_changed(const char *path, struct timespec *last_mtime) {
    struct stat info;
    if (stat(path, &info) != 0) {
        return false;
    }
#if defined(__APPLE__)
    struct timespec current = info.st_mtimespec;
#else
    struct timespec current = info.st_mtim;
#endif
    if (current.tv_sec == last_mtime->tv_sec &&
        current.tv_nsec == last_mtime->tv_nsec) {
        return false;
    }
    *last_mtime = current;
    return true;
}

static bool load_lighting_mode(void) {
    FILE *file = fopen(lighting_mode_path, "r");
    if (file == NULL) {
        lighting_mode = LIGHTING_PER_KEY;
        return errno == ENOENT;
    }
    char value[32] = {0};
    bool read_ok = fgets(value, sizeof(value), file) != NULL;
    fclose(file);
    if (!read_ok) {
        return false;
    }
    value[strcspn(value, "\r\n")] = '\0';
    if (strcmp(value, "per-key") == 0) {
        lighting_mode = LIGHTING_PER_KEY;
    } else if (strcmp(value, "whole-board") == 0) {
        lighting_mode = LIGHTING_WHOLE_BOARD;
    } else {
        return false;
    }
    return true;
}

static bool save_lighting_mode(lighting_mode_t mode) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             lighting_mode_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    const char *value = mode == LIGHTING_WHOLE_BOARD ? "whole-board" : "per-key";
    fprintf(file, "%s\n", value);
    if (fclose(file) != 0 || rename(temporary_path, lighting_mode_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    lighting_mode = mode;
    return true;
}

static bool load_task_lights_enabled(void) {
    FILE *file = fopen(task_lights_enabled_path, "r");
    if (file == NULL) {
        task_lights_enabled = true;
        return errno == ENOENT;
    }
    char value[16] = {0};
    bool read_ok = fgets(value, sizeof(value), file) != NULL;
    fclose(file);
    if (!read_ok) {
        return false;
    }
    value[strcspn(value, "\r\n")] = '\0';
    if (strcmp(value, "on") == 0) {
        task_lights_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        task_lights_enabled = false;
    } else {
        return false;
    }
    return true;
}

static bool save_task_lights_enabled(bool enabled) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             task_lights_enabled_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%s\n", enabled ? "on" : "off");
    if (fclose(file) != 0 ||
        rename(temporary_path, task_lights_enabled_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    task_lights_enabled = enabled;
    return true;
}

static void process_lighting_mode(struct timespec *last_mtime) {
    if (!file_changed(lighting_mode_path, last_mtime)) {
        return;
    }
    lighting_mode_t previous = lighting_mode;
    if (!load_lighting_mode()) {
        log_line("ERROR", "Ignoring an invalid lighting-mode setting.");
        return;
    }
    if (lighting_mode != previous) {
        log_line("MODE", lighting_mode == LIGHTING_WHOLE_BOARD
                             ? "Whole-keyboard traffic-light mode enabled."
                             : "Per-task number-key mode enabled.");
        refresh_lights();
    }
}

static void process_task_lights_enabled(struct timespec *last_mtime) {
    if (!file_changed(task_lights_enabled_path, last_mtime)) {
        return;
    }
    bool previous = task_lights_enabled;
    if (!load_task_lights_enabled()) {
        log_line("ERROR", "Ignoring an invalid task-light enabled setting.");
        return;
    }
    if (task_lights_enabled == previous) {
        return;
    }
    log_line("MODE", task_lights_enabled
                         ? "Task lights enabled."
                         : "Task lights paused; restoring the keyboard lighting.");
    refresh_lights();
}

static void process_selected_slot(struct timespec *last_mtime) {
    if (!file_changed(selected_slot_path, last_mtime)) {
        return;
    }
    FILE *file = fopen(selected_slot_path, "r");
    int slot = 0;
    if (file != NULL) {
        (void)fscanf(file, "%d", &slot);
        fclose(file);
    }
    if (slot < 1 || slot > 9) {
        return;
    }
    for (size_t index = 0; index < watched_count; index++) {
        watched_thread_t *item = &watched[index];
        if (item->slot == slot &&
            (item->status == SLOT_UNREAD || item->status == SLOT_ERROR)) {
            update_thread_status(item, SLOT_IDLE);
            return;
        }
    }
}

static void process_test_request(struct timespec *last_mtime) {
    if (!file_changed(test_request_path, last_mtime)) {
        return;
    }
    FILE *file = fopen(test_request_path, "r");
    int slot = 0;
    int status = SLOT_OFF;
    if (file != NULL) {
        (void)fscanf(file, "%d\t%d", &slot, &status);
        fclose(file);
        (void)unlink(test_request_path);
    }
    if (slot < 1 || slot > 9 || status < SLOT_OFF || status > SLOT_ERROR) {
        return;
    }
    for (int index = 0; index < 9; index++) {
        test_statuses[index] = SLOT_OFF;
    }
    test_statuses[slot - 1] = (slot_status_t)status;
    test_override_until = time(NULL) + 30;
    log_line("TEST", "Showing the requested status color for 30 seconds.");
    refresh_lights();
}

static int run_daemon(void) {
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/daemon.lock", app_support_dir);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        log_line("INFO", "Feker Codex Bridge is already running.");
        if (lock_fd >= 0) {
            close(lock_fd);
        }
        return 0;
    }

    sqlite3 *database = NULL;
    if (sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        log_line("ERROR", "Unable to open the Codex task database.");
        if (database != NULL) {
            sqlite3_close(database);
        }
        close(lock_fd);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    if (!load_lighting_mode()) {
        log_line("ERROR", "Unable to read the saved lighting mode; using per-key mode.");
        lighting_mode = LIGHTING_PER_KEY;
    }
    if (!load_task_lights_enabled()) {
        log_line("ERROR", "Unable to read the saved task-light state; enabling it.");
        task_lights_enabled = true;
    }
    log_line("READY",
             "Watching Codex tasks. Use Codex native Command+1...9 to switch tasks.");
    save_state();
    refresh_lights();

    unsigned int ticks = 0;
    struct timespec mode_mtime = {0, 0};
    struct timespec enabled_mtime = {0, 0};
    struct timespec selected_mtime = {0, 0};
    struct timespec test_mtime = {0, 0};
    while (!should_stop) {
        if (ticks % 20 == 0) {
            discover_threads(database);
            if (rgb_device == NULL) {
                refresh_lights();
            }
        }
        for (size_t index = 0; index < watched_count; index++) {
            process_appended_events(&watched[index]);
        }
        process_lighting_mode(&mode_mtime);
        process_task_lights_enabled(&enabled_mtime);
        process_selected_slot(&selected_mtime);
        process_test_request(&test_mtime);
        if (test_override_until != 0 && test_override_until <= time(NULL)) {
            test_override_until = 0;
            refresh_lights();
        }
        (void)refresh_direct_mode();
        usleep(DIRECT_REFRESH_INTERVAL_US);
        ticks++;
    }

    close_rgb_device(true);
    sqlite3_close(database);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    log_line("STOP", "Feker Codex Bridge stopped.");
    return 0;
}

static int test_key(int slot, slot_status_t status) {
    if (slot < 1 || slot > 9) {
        fputs("Slot must be 1 through 9.\n", stderr);
        return 2;
    }
    slot_status_t statuses[9] = {SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF,
                                 SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF};
    statuses[slot - 1] = status;
    return apply_slot_colors(statuses) ? 0 : 1;
}

static int launch_light_service(void) {
    uid_t user_id = getuid();
    struct passwd *account = getpwuid(user_id);
    if (account == NULL || account->pw_dir == NULL) {
        fputs("Unable to resolve the current user.\n", stderr);
        return 1;
    }

    char home_argument[PATH_MAX + 6];
    char uid_argument[64];
    if (snprintf(home_argument, sizeof(home_argument), "HOME=%s",
                 account->pw_dir) >= (int)sizeof(home_argument) ||
        snprintf(uid_argument, sizeof(uid_argument), "FEKER_USER_UID=%u",
                 (unsigned int)user_id) >= (int)sizeof(uid_argument)) {
        fputs("The current user identity is too long.\n", stderr);
        return 1;
    }

    execl("/usr/bin/sudo", "sudo", "-n", "/usr/bin/env",
          home_argument, uid_argument,
          "/Library/PrivilegedHelperTools/com.chenzixin.feker-codex-bridge",
          "--daemon", (char *)NULL);
    fprintf(stderr, "Unable to launch the light service: %s\n", strerror(errno));
    return 1;
}

#ifdef __OBJC__
static NSImage *task_light_menu_icon(bool enabled) {
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(18.0, 18.0)];
    [image lockFocus];

    [[NSColor blackColor] setStroke];
    [[NSColor blackColor] setFill];
    NSBezierPath *keycap = [NSBezierPath
        bezierPathWithRoundedRect:NSMakeRect(1.5, 2.5, 15.0, 13.0)
        xRadius:3.0 yRadius:3.0];
    keycap.lineWidth = 1.5;
    [keycap stroke];

    if (enabled) {
        for (CGFloat x = 5.0; x <= 13.0; x += 4.0) {
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x - 1.0, 8.0,
                                                               2.0, 2.0)] fill];
        }
        NSBezierPath *base = [NSBezierPath bezierPath];
        base.lineWidth = 1.5;
        [base moveToPoint:NSMakePoint(5.0, 5.5)];
        [base lineToPoint:NSMakePoint(13.0, 5.5)];
        [base stroke];
    } else {
        NSBezierPath *pause = [NSBezierPath bezierPath];
        pause.lineWidth = 1.8;
        [pause moveToPoint:NSMakePoint(7.0, 6.0)];
        [pause lineToPoint:NSMakePoint(7.0, 12.0)];
        [pause moveToPoint:NSMakePoint(11.0, 6.0)];
        [pause lineToPoint:NSMakePoint(11.0, 12.0)];
        [pause stroke];
    }

    [image unlockFocus];
    image.template = YES;
    return image;
}

static bool evision_device_present(void) {
    struct hid_device_info *devices = hid_enumerate(FEKER_VID, FEKER_PID);
    bool found = false;
    for (struct hid_device_info *item = devices; item != NULL;
         item = item->next) {
        if (item->usage_page == FEKER_USAGE_PAGE && item->usage == FEKER_USAGE) {
            found = true;
            break;
        }
    }
    hid_free_enumeration(devices);
    return found;
}

@interface BridgeMenuController : NSObject <NSMenuDelegate> {
    NSStatusItem *status_item;
    NSMenu *status_menu;
    NSMenuItem *toggle_item;
    NSMenuItem *device_item;
    NSMenuItem *per_key_item;
    NSMenuItem *whole_board_item;
    NSMenuItem *test_menu_item;
    NSMenuItem *login_at_startup_item;
}
@end

@implementation BridgeMenuController

- (instancetype)init {
    self = [super init];
    if (self == nil) {
        return nil;
    }

    status_item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength];
    status_item.button.title = @" 任务灯";
    status_item.button.imagePosition = NSImageLeft;

    status_menu = [[NSMenu alloc] initWithTitle:@"FEKER 任务灯"];
    status_menu.delegate = self;
    toggle_item = [[NSMenuItem alloc]
        initWithTitle:@"任务灯已开启" action:@selector(toggleTaskLights:)
        keyEquivalent:@""];
    toggle_item.target = self;
    [status_menu addItem:toggle_item];

    device_item = [[NSMenuItem alloc]
        initWithTitle:@"正在检测键盘…" action:nil keyEquivalent:@""];
    device_item.enabled = NO;
    [status_menu addItem:device_item];
    [status_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *mode_menu_item = [[NSMenuItem alloc]
        initWithTitle:@"显示方式" action:nil keyEquivalent:@""];
    NSMenu *mode_menu = [[NSMenu alloc] initWithTitle:@"显示方式"];
    per_key_item = [[NSMenuItem alloc]
        initWithTitle:@"数字键任务灯 — 1–9 对应任务" action:@selector(choosePerKey:)
        keyEquivalent:@""];
    per_key_item.target = self;
    [mode_menu addItem:per_key_item];

    whole_board_item = [[NSMenuItem alloc]
        initWithTitle:@"整板状态灯 — 显示最高优先级" action:@selector(chooseWholeBoard:)
        keyEquivalent:@""];
    whole_board_item.target = self;
    [mode_menu addItem:whole_board_item];
    mode_menu_item.submenu = mode_menu;
    [status_menu addItem:mode_menu_item];

    test_menu_item = [[NSMenuItem alloc]
        initWithTitle:@"测试灯光" action:nil keyEquivalent:@""];
    NSMenu *test_menu = [[NSMenu alloc] initWithTitle:@"测试灯光"];
    NSArray<NSArray *> *tests = @[
        @[@"🔵  执行中", @(SLOT_WORKING)],
        @[@"🟢  完成未读", @(SLOT_UNREAD)],
        @[@"🟠  等待操作", @(SLOT_WAITING)],
        @[@"🔴  出错", @(SLOT_ERROR)],
        @[@"⚪️  空闲", @(SLOT_IDLE)],
        @[@"熄灭测试灯", @(SLOT_OFF)],
    ];
    for (NSArray *test in tests) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:test[0] action:@selector(testStatus:) keyEquivalent:@""];
        item.target = self;
        item.tag = [test[1] integerValue];
        [test_menu addItem:item];
    }
    test_menu_item.submenu = test_menu;
    [status_menu addItem:test_menu_item];

    NSMenuItem *help_item = [[NSMenuItem alloc]
        initWithTitle:@"使用说明" action:nil keyEquivalent:@""];
    NSMenu *help_menu = [[NSMenu alloc] initWithTitle:@"使用说明"];
    NSArray<NSString *> *instructions = @[
        @"左键或右键图标：打开菜单",
        @"菜单第一项：开启或暂停任务灯",
        @"Command + 1…9：切换 Codex 任务",
    ];
    for (NSString *instruction in instructions) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:instruction action:nil keyEquivalent:@""];
        item.enabled = NO;
        [help_menu addItem:item];
    }
    [help_menu addItem:[NSMenuItem separatorItem]];
    NSArray<NSString *> *color_help = @[
        @"🔵  执行中 — Codex 正在工作",
        @"🟢  完成未读 — 等你切回查看",
        @"🟠  等待操作 — 需要输入或批准",
        @"🔴  出错 — 任务执行失败",
        @"⚪️  空闲 — 已查看或暂无操作",
    ];
    for (NSString *explanation in color_help) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:explanation action:nil keyEquivalent:@""];
        item.enabled = NO;
        [help_menu addItem:item];
    }
    help_item.submenu = help_menu;
    [status_menu addItem:help_item];

    [status_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *open_log = [[NSMenuItem alloc]
        initWithTitle:@"查看运行日志…" action:@selector(openLog:) keyEquivalent:@""];
    open_log.target = self;
    [status_menu addItem:open_log];

    NSMenuItem *settings_item = [[NSMenuItem alloc]
        initWithTitle:@"设置" action:nil keyEquivalent:@""];
    NSMenu *settings_menu = [[NSMenu alloc] initWithTitle:@"设置"];
    login_at_startup_item = [[NSMenuItem alloc]
        initWithTitle:@"开机自动启动" action:@selector(toggleLoginAtStartup:)
        keyEquivalent:@""];
    login_at_startup_item.target = self;
    [settings_menu addItem:login_at_startup_item];
    [settings_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *open_permission = [[NSMenuItem alloc]
        initWithTitle:@"输入监控权限…" action:@selector(openInputMonitoring:)
        keyEquivalent:@""];
    open_permission.target = self;
    [settings_menu addItem:open_permission];
    NSMenuItem *open_login_items = [[NSMenuItem alloc]
        initWithTitle:@"打开系统登录项设置…" action:@selector(openLoginItems:)
        keyEquivalent:@""];
    open_login_items.target = self;
    [settings_menu addItem:open_login_items];
    settings_item.submenu = settings_menu;
    [status_menu addItem:settings_item];

    NSMenuItem *project_home = [[NSMenuItem alloc]
        initWithTitle:@"项目主页（GitHub）…" action:@selector(openProjectHome:)
        keyEquivalent:@""];
    project_home.target = self;
    [status_menu addItem:project_home];

    [status_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit_item = [[NSMenuItem alloc]
        initWithTitle:@"退出 FEKER 任务灯" action:@selector(quit:)
        keyEquivalent:@"q"];
    quit_item.target = self;
    [status_menu addItem:quit_item];

    status_item.menu = status_menu;
    (void)load_lighting_mode();
    (void)load_task_lights_enabled();
    [self updateAppearance];
    if (getenv("FEKER_SHOW_MENU_ON_START") != NULL) {
        [self performSelector:@selector(showStatusMenuForTesting)
                   withObject:nil afterDelay:1.0];
    }
    return self;
}

- (void)showStatusMenuForTesting {
    [status_item.button performClick:nil];
}

- (void)updateAppearance {
    toggle_item.title = task_lights_enabled ? @"任务灯已开启" : @"任务灯已暂停";
    toggle_item.state = task_lights_enabled ? NSControlStateValueOn
                                            : NSControlStateValueOff;
    test_menu_item.enabled = task_lights_enabled;
    per_key_item.state =
        lighting_mode == LIGHTING_PER_KEY ? NSControlStateValueOn : NSControlStateValueOff;
    whole_board_item.state =
        lighting_mode == LIGHTING_WHOLE_BOARD ? NSControlStateValueOn
                                              : NSControlStateValueOff;
    status_item.button.image = task_light_menu_icon(task_lights_enabled);
    status_item.button.toolTip = task_lights_enabled
                                     ? @"FEKER 任务灯已开启 · 点击打开菜单"
                                     : @"FEKER 任务灯已暂停 · 点击打开菜单";

    if (@available(macOS 13.0, *)) {
        SMAppServiceStatus login_status = [SMAppService mainAppService].status;
        login_at_startup_item.state = login_status == SMAppServiceStatusEnabled
                                          ? NSControlStateValueOn
                                          : NSControlStateValueOff;
        login_at_startup_item.title =
            login_status == SMAppServiceStatusRequiresApproval
                ? @"开机自动启动（等待系统批准）"
                : @"开机自动启动";
    } else {
        login_at_startup_item.enabled = NO;
        login_at_startup_item.title = @"开机自动启动（需要 macOS 13）";
    }

    if (qmk_device_present()) {
        device_item.title = @"新款 FEKER · 自动使用整板状态色";
    } else if (evision_device_present()) {
        device_item.title = lighting_mode == LIGHTING_PER_KEY
                                ? @"旧款 FEKER · 数字键逐灯显示"
                                : @"旧款 FEKER · 整板状态色";
    } else {
        device_item.title = @"未检测到兼容的有线键盘";
    }
}

- (void)menuWillOpen:(NSMenu *)menu {
    (void)menu;
    log_line("UI", "Status menu opened.");
    (void)load_lighting_mode();
    (void)load_task_lights_enabled();
    [self updateAppearance];
}

- (void)toggleTaskLights:(id)sender {
    (void)sender;
    if (!save_task_lights_enabled(!task_lights_enabled)) {
        log_line("ERROR", "Unable to save the task-light enabled state.");
    }
    [self updateAppearance];
}

- (void)choosePerKey:(id)sender {
    (void)sender;
    if (!save_lighting_mode(LIGHTING_PER_KEY)) {
        log_line("ERROR", "Unable to save per-key lighting mode.");
    }
    [self updateAppearance];
}

- (void)chooseWholeBoard:(id)sender {
    (void)sender;
    if (!save_lighting_mode(LIGHTING_WHOLE_BOARD)) {
        log_line("ERROR", "Unable to save whole-keyboard lighting mode.");
    }
    [self updateAppearance];
}

- (void)testStatus:(NSMenuItem *)sender {
    if (!write_atomic_slot_file(test_request_path, 1, (int)sender.tag)) {
        log_line("ERROR", "Unable to send the lighting test request.");
    }
}

- (void)openLog:(id)sender {
    (void)sender;
    NSString *path = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Logs/FekerCodexBridge.log"];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)openInputMonitoring:(id)sender {
    (void)sender;
    NSURL *url = [NSURL
        URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)openLoginItems:(id)sender {
    (void)sender;
    NSURL *url = [NSURL
        URLWithString:@"x-apple.systempreferences:com.apple.LoginItems-Settings.extension"];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)toggleLoginAtStartup:(id)sender {
    (void)sender;
    if (@available(macOS 13.0, *)) {
        SMAppService *service = [SMAppService mainAppService];
        NSError *error = nil;
        bool ok = service.status == SMAppServiceStatusEnabled
                      ? [service unregisterAndReturnError:&error]
                      : [service registerAndReturnError:&error];
        if (!ok) {
            char message[512];
            snprintf(message, sizeof(message), "Unable to change Login Item: %s",
                     error.localizedDescription.UTF8String != NULL
                         ? error.localizedDescription.UTF8String
                         : "unknown error");
            log_line("ERROR", message);
        }
        [self updateAppearance];
        if (service.status == SMAppServiceStatusRequiresApproval) {
            [self openLoginItems:nil];
        }
    }
}

- (void)openProjectHome:(id)sender {
    (void)sender;
    NSURL *url = [NSURL
        URLWithString:@"https://github.com/chenzixin1/feker-codex-bridge"];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)quit:(id)sender {
    (void)sender;
    should_stop = 1;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

@end

static BridgeMenuController *menu_controller = nil;

static void setup_menu_bar_ui(void) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    menu_controller = [[BridgeMenuController alloc] init];
}

static int slot_for_keycode(CGKeyCode keycode) {
    switch (keycode) {
        case 18: return 1;
        case 19: return 2;
        case 20: return 3;
        case 21: return 4;
        case 23: return 5;
        case 22: return 6;
        case 26: return 7;
        case 28: return 8;
        case 25: return 9;
        default: return 0;
    }
}

static CGEventRef shortcut_event_callback(CGEventTapProxy proxy, CGEventType type,
                                          CGEventRef event, void *context) {
    (void)proxy;
    (void)context;
    if (type != kCGEventKeyDown) {
        return event;
    }
    CGEventFlags flags = CGEventGetFlags(event);
    if ((flags & kCGEventFlagMaskCommand) == 0 ||
        (flags & (kCGEventFlagMaskControl | kCGEventFlagMaskAlternate)) != 0) {
        return event;
    }
    int slot = slot_for_keycode(
        (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    if (slot == 0) {
        return event;
    }

    NSRunningApplication *frontmost =
        [[NSWorkspace sharedWorkspace] frontmostApplication];
    if (![[frontmost bundleIdentifier] isEqualToString:@"com.openai.codex"]) {
        return event;
    }
    if (!write_atomic_slot_file(selected_slot_path, slot, 0)) {
        log_line("ERROR", "Unable to save the selected Codex shortcut slot.");
    }
    return event;
}

static void observer_timer_callback(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    (void)info;
    if (should_stop) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

static int run_shortcut_observer(void) {
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/observer.lock", app_support_dir);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        log_line("INFO", "Codex shortcut observer is already running.");
        if (lock_fd >= 0) {
            close(lock_fd);
        }
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    bool access_requested = false;
    CFMachPortRef tap = NULL;
    while (!should_stop && tap == NULL) {
        if (!CGPreflightListenEventAccess()) {
            if (!access_requested) {
                log_line("PERMISSION",
                         "Input Monitoring is required only to observe Codex "
                         "task-switch shortcuts.");
                (void)CGRequestListenEventAccess();
                access_requested = true;
            }
        } else {
            tap = CGEventTapCreate(
                kCGSessionEventTap, kCGHeadInsertEventTap,
                kCGEventTapOptionListenOnly, CGEventMaskBit(kCGEventKeyDown),
                shortcut_event_callback, NULL);
        }
        if (tap == NULL) {
            (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
        }
    }
    if (tap == NULL) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    CFRunLoopSourceRef source =
        CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.5, 0.5, 0, 0,
        observer_timer_callback, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    log_line("READY",
             "Passively observing native Codex Command+1...9 shortcuts.");
    CFRunLoopRun();

    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
    CFRelease(timer);
    CFRelease(source);
    CFRelease(tap);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return 0;
}

static void stop_light_service(pid_t child_pid) {
    if (child_pid <= 0) {
        return;
    }
    (void)kill(-child_pid, SIGTERM);
    for (int attempt = 0; attempt < 20; attempt++) {
        pid_t result = waitpid(child_pid, NULL, WNOHANG);
        if (result == child_pid || (result < 0 && errno == ECHILD)) {
            return;
        }
        usleep(100000);
    }
    (void)kill(-child_pid, SIGKILL);
    (void)waitpid(child_pid, NULL, 0);
}

static int run_agent(void) {
    configure_background_logging();
    bool use_unprivileged_qmk_service = qmk_device_present();
    pid_t child_pid = fork();
    if (child_pid < 0) {
        fprintf(stderr, "Unable to start the light service: %s\n", strerror(errno));
        return 1;
    }
    if (child_pid == 0) {
        if (setpgid(0, 0) != 0) {
            fprintf(stderr, "Unable to isolate the light service: %s\n", strerror(errno));
            _exit(1);
        }
        if (use_unprivileged_qmk_service) {
            execl(executable_path, executable_path, "--daemon", (char *)NULL);
            fprintf(stderr, "Unable to launch the QMK light service: %s\n",
                    strerror(errno));
            _exit(1);
        }
        _exit(launch_light_service());
    }
    if (setpgid(child_pid, child_pid) != 0 && errno != EACCES) {
        log_line("ERROR", "Unable to supervise the light-service process group.");
        (void)kill(child_pid, SIGTERM);
        (void)waitpid(child_pid, NULL, 0);
        return 1;
    }

    setup_menu_bar_ui();
    int result = run_shortcut_observer();
    stop_light_service(child_pid);
    return result;
}
#endif

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --agent\n"
            "  %s --daemon\n"
            "  %s --launch-light-service\n"
            "  %s --observer\n"
            "  %s --task-lights on|off\n"
            "  %s --mode per-key|whole-board\n"
            "  %s --select-slot 1-9\n"
            "  %s --request-test-key 1-9 [working|unread|idle|waiting|error|off]\n"
            "  %s --test-key 1-9 [working|unread|idle|waiting|error|off]\n"
            "  %s --off\n",
            program, program, program, program, program, program, program, program,
            program, program);
}

static bool parse_status(const char *name, slot_status_t *status) {
    if (strcmp(name, "working") == 0 || strcmp(name, "blue") == 0) {
        *status = SLOT_WORKING;
    } else if (strcmp(name, "unread") == 0 || strcmp(name, "green") == 0) {
        *status = SLOT_UNREAD;
    } else if (strcmp(name, "idle") == 0 || strcmp(name, "white") == 0) {
        *status = SLOT_IDLE;
    } else if (strcmp(name, "waiting") == 0 || strcmp(name, "amber") == 0 ||
               strcmp(name, "orange") == 0) {
        *status = SLOT_WAITING;
    } else if (strcmp(name, "error") == 0 || strcmp(name, "red") == 0) {
        *status = SLOT_ERROR;
    } else if (strcmp(name, "off") == 0) {
        *status = SLOT_OFF;
    } else {
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--launch-light-service") == 0) {
        return launch_light_service();
    }
    if (!configure_process_identity(argc, argv)) {
        return 1;
    }
    if (!initialize_paths()) {
        return 1;
    }
    if (!initialize_executable_path()) {
        return 1;
    }
    if (hid_init() != 0) {
        fputs("Unable to initialize HIDAPI.\n", stderr);
        return 1;
    }

    int result = 0;
#ifdef __OBJC__
    if (argc == 1 && !setuid_installation) {
        @autoreleasepool {
            result = run_agent();
        }
    } else
#endif
    if ((argc == 1 && setuid_installation) ||
        (argc == 2 && strcmp(argv[1], "--daemon") == 0)) {
        result = run_daemon();
#ifdef __OBJC__
    } else if (argc == 2 &&
               (strcmp(argv[1], "--agent") == 0 ||
                strcmp(argv[1], "--observer") == 0)) {
        @autoreleasepool {
            result = strcmp(argv[1], "--agent") == 0
                         ? run_agent()
                         : run_shortcut_observer();
        }
#endif
    } else if (argc == 3 && strcmp(argv[1], "--task-lights") == 0) {
        if (strcmp(argv[2], "on") == 0) {
            result = save_task_lights_enabled(true) ? 0 : 1;
        } else if (strcmp(argv[2], "off") == 0) {
            result = save_task_lights_enabled(false) ? 0 : 1;
        } else {
            print_usage(argv[0]);
            result = 2;
        }
    } else if (argc == 3 && strcmp(argv[1], "--mode") == 0) {
        lighting_mode_t mode;
        if (strcmp(argv[2], "per-key") == 0) {
            mode = LIGHTING_PER_KEY;
        } else if (strcmp(argv[2], "whole-board") == 0) {
            mode = LIGHTING_WHOLE_BOARD;
        } else {
            print_usage(argv[0]);
            result = 2;
            goto done;
        }
        result = save_lighting_mode(mode) ? 0 : 1;
    } else if (argc == 3 && strcmp(argv[1], "--select-slot") == 0) {
        int slot = atoi(argv[2]);
        result = slot >= 1 && slot <= 9 &&
                 write_atomic_slot_file(selected_slot_path, slot, 0) ? 0 : 2;
    } else if ((argc == 3 || argc == 4) &&
               strcmp(argv[1], "--request-test-key") == 0) {
        int slot = atoi(argv[2]);
        slot_status_t status = SLOT_UNREAD;
        if (argc == 4 && !parse_status(argv[3], &status)) {
            print_usage(argv[0]);
            result = 2;
        } else if (slot < 1 || slot > 9 ||
                   !write_atomic_slot_file(test_request_path, slot, (int)status)) {
            fputs("Unable to write the test request.\n", stderr);
            result = 2;
        }
    } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "--test-key") == 0) {
        slot_status_t status = SLOT_UNREAD;
        if (argc == 4 && !parse_status(argv[3], &status)) {
            print_usage(argv[0]);
            result = 2;
            goto done;
        }
        result = test_key(atoi(argv[2]), status);
    } else if (argc == 2 && strcmp(argv[1], "--off") == 0) {
        slot_status_t off[9] = {SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF,
                                SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF};
        result = apply_slot_colors(off) ? 0 : 1;
    } else {
        print_usage(argv[0]);
        result = 2;
    }

done:
    hid_exit();
    return result;
}
