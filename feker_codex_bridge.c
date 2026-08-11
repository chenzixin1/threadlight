#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <hidapi.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <ServiceManagement/ServiceManagement.h>
#endif

#define FEKER_QMK_VID 0x36B0
#define FEKER_ALICE80_QMK_PID 0x305F
#define FEKER_ALICE80_TRIMODE_PID 0x3042
#define FEKER_QMK_USAGE_PAGE 0xFF60
#define FEKER_QMK_USAGE 0x0061
#define QMK_REPORT_SIZE 32
#define RGB9_COMMAND 0xB0
#define RGB9_INDICATOR_COUNT 9
#define RGB9_ALL_KEYS_MASK 0x01FF
#define RGB9_FLAG_FOCUS 0x01
#define DAEMON_TICK_INTERVAL_US 33333
#define EVENT_POLL_TICK_COUNT 3
#define THREAD_DISCOVERY_TICK_COUNT 60
#define LIGHT_RECONCILE_TICK_COUNT 150
#define IPC_RECONNECT_TICK_COUNT 150
#define IPC_FRAME_BUFFER_SIZE (256 * 1024)
#define SETTINGS_PREVIEW_INTERVAL (1.0 / 60.0)
#define WORKING_BREATH_SECONDS 3.6
#define WAITING_BREATH_SECONDS 4.4
#define ERROR_ALERT_SECONDS 0.8
#define MAX_THREADS 128
#define MAX_TITLE 256
#define SIDEBAR_CATALOG_LIMIT RGB9_INDICATOR_COUNT

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef enum {
    SLOT_OFF = 0,
    SLOT_WORKING,
    SLOT_COMPLETE,
    SLOT_IDLE,
    SLOT_WAITING,
    SLOT_ERROR,
} slot_status_t;

typedef enum {
    COLOR_SCHEME_CODEX = 0,
    COLOR_SCHEME_OCEAN,
    COLOR_SCHEME_VIOLET,
    COLOR_SCHEME_COUNT,
} color_scheme_t;

typedef enum {
    LIGHTING_SCOPE_WHOLE_BOARD = 0,
    LIGHTING_SCOPE_NUMBER_KEYS,
    LIGHTING_SCOPE_COUNT,
} lighting_scope_t;

typedef struct {
    char thread_id[64];
    char rollout_path[PATH_MAX];
    char title[MAX_TITLE];
    off_t offset;
    int slot;
    slot_status_t status;
    time_t touched_at;
    int64_t status_changed_at_ms;
    bool initialized;
    bool active;
    bool unread;
    bool live_read_state_known;
} watched_thread_t;

typedef struct {
    char thread_id[64];
    char rollout_path[PATH_MAX];
    char title[MAX_TITLE];
} discovered_thread_t;

typedef struct {
    char thread_id[64];
    int64_t changed_at_ms;
    bool unread;
} read_state_entry_t;

static volatile sig_atomic_t should_stop = 0;
static watched_thread_t watched[MAX_THREADS];
static size_t watched_count = 0;
static bool hid_permission_warning_printed = false;
static hid_device *rgb_device = NULL;
static bool qmk_generation_warning_printed = false;
static bool rgb9_capability_checked = false;
static bool rgb9_supported = false;
static uint8_t rgb9_capability_flags = 0;
static bool rgb9_overlay_active = false;
static bool rgb9_requirement_warning_printed = false;
static bool rgb9_focus_warning_printed = false;
static bool qmk_original_saved = false;
static uint8_t qmk_original_brightness = 0;
static uint8_t qmk_original_effect = 0;
static uint8_t qmk_original_color[2] = {0, 0};
static bool qmk_task_mode_configured = false;
static uint8_t qmk_applied_hue = 0;
static uint8_t qmk_applied_saturation = 0;
static int qmk_applied_brightness = -1;
static char app_support_dir[PATH_MAX];
static char test_request_path[PATH_MAX];
static char task_lights_enabled_path[PATH_MAX];
static char lighting_scope_path[PATH_MAX];
static char color_scheme_path[PATH_MAX];
static char brightness_path[PATH_MAX];
static char read_state_path[PATH_MAX];
static char database_path[PATH_MAX];
static char global_state_path[PATH_MAX];
static char ipc_socket_path[PATH_MAX];
static char log_path[PATH_MAX];
static char executable_path[PATH_MAX];
static slot_status_t test_status = SLOT_OFF;
static time_t test_override_until = 0;
static bool task_lights_enabled = true;
static lighting_scope_t lighting_scope = LIGHTING_SCOPE_WHOLE_BOARD;
static color_scheme_t color_scheme = COLOR_SCHEME_CODEX;
static uint8_t light_brightness_percent = 68;
static double animation_started_at = 0.0;
static slot_status_t rendered_status = SLOT_OFF;
static slot_status_t rendered_number_key_statuses[RGB9_INDICATOR_COUNT];
static read_state_entry_t read_states[MAX_THREADS];
static size_t read_state_count = 0;
static int codex_ipc_fd = -1;
static uint8_t codex_ipc_buffer[IPC_FRAME_BUFFER_SIZE];
static size_t codex_ipc_buffer_length = 0;
static bool codex_ipc_connected_once = false;

static void handle_signal(int signal_number) {
    (void)signal_number;
    should_stop = 1;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int64_t realtime_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
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
    char legacy_app_support_dir[PATH_MAX];
    snprintf(app_support_dir, sizeof(app_support_dir),
             "%s/Threadlight", application_support_dir);
    snprintf(legacy_app_support_dir, sizeof(legacy_app_support_dir),
             "%s/Feker Codex Bridge", application_support_dir);
    if (access(app_support_dir, F_OK) != 0 &&
        access(legacy_app_support_dir, F_OK) == 0) {
        (void)rename(legacy_app_support_dir, app_support_dir);
    }
    snprintf(test_request_path, sizeof(test_request_path),
             "%s/test-request.tsv", app_support_dir);
    snprintf(task_lights_enabled_path, sizeof(task_lights_enabled_path),
             "%s/task-lights-enabled.txt", app_support_dir);
    snprintf(lighting_scope_path, sizeof(lighting_scope_path),
             "%s/lighting-scope.txt", app_support_dir);
    snprintf(color_scheme_path, sizeof(color_scheme_path),
             "%s/color-scheme.txt", app_support_dir);
    snprintf(brightness_path, sizeof(brightness_path),
             "%s/brightness.txt", app_support_dir);
    snprintf(read_state_path, sizeof(read_state_path),
             "%s/codex-read-state.json", app_support_dir);
    snprintf(database_path, sizeof(database_path), "%s/.codex/state_5.sqlite", home);
    snprintf(global_state_path, sizeof(global_state_path),
             "%s/.codex/.codex-global-state.json", home);
    snprintf(ipc_socket_path, sizeof(ipc_socket_path),
             "%s/.codex/ipc/ipc.sock", home);
    snprintf(log_path, sizeof(log_path), "%s/Threadlight.log", logs_dir);

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

static bool is_supported_feker_pid(unsigned short product_id) {
    return product_id == FEKER_ALICE80_QMK_PID ||
           product_id == FEKER_ALICE80_TRIMODE_PID;
}

static hid_device *open_rgb_device(void) {
    struct hid_device_info *devices =
        hid_enumerate(FEKER_QMK_VID, 0);
    const char *chosen_path = NULL;
    char path_copy[PATH_MAX];

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (is_supported_feker_pid(item->product_id) &&
            item->usage_page == FEKER_QMK_USAGE_PAGE &&
            item->usage == FEKER_QMK_USAGE && item->path != NULL) {
            snprintf(path_copy, sizeof(path_copy), "%s", item->path);
            chosen_path = path_copy;
            break;
        }
    }
    hid_free_enumeration(devices);

    if (chosen_path == NULL) {
        if (!hid_permission_warning_printed) {
            log_line("ERROR",
                     "FEKER QMK/VIA Raw HID interface was not found "
                     "(expected Alice80 36B0:305F or 36B0:3042, usage FF60:0061).");
            hid_permission_warning_printed = true;
        }
        return NULL;
    }

    hid_device *device = hid_open_path(chosen_path);
    if (device == NULL && !hid_permission_warning_printed) {
        log_line("PERMISSION",
                 "Unable to open FEKER Raw HID. Reconnect the keyboard and "
                 "close other RGB control software.");
        hid_permission_warning_printed = true;
    }
    if (device != NULL) {
        hid_permission_warning_printed = false;
        if (!qmk_generation_warning_printed) {
            log_line("DEVICE",
                     "FEKER QMK/VIA keyboard detected; task-light output is available.");
            qmk_generation_warning_printed = true;
        }
    }
    return device;
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

static bool qmk_rgb9_has_capability(void) {
    if (rgb9_capability_checked) {
        return rgb9_supported;
    }
    rgb9_capability_checked = true;
    uint8_t subcommand = 0x00;
    uint8_t response[QMK_REPORT_SIZE];
    rgb9_supported = qmk_exchange(rgb_device, RGB9_COMMAND,
                                  &subcommand, 1, response) &&
                     response[1] == 0x00 &&
                     response[2] == 0x01 &&
                     response[3] == RGB9_INDICATOR_COUNT &&
                     response[4] == 0x01 &&
                     response[5] == QMK_REPORT_SIZE;
    if (rgb9_supported) {
        rgb9_capability_flags = response[6];
        log_line("DEVICE",
                 "Threadlight RGB9 firmware detected; number-key lighting is available.");
    }
    return rgb9_supported;
}

static bool qmk_rgb9_set_colors(uint16_t mask,
                                const rgb_t colors[RGB9_INDICATOR_COUNT],
                                uint8_t flags) {
    if (!qmk_rgb9_has_capability()) {
        return false;
    }
    uint8_t payload[4 + RGB9_INDICATOR_COUNT * 3];
    payload[0] = 0x04;
    payload[1] = (uint8_t)(mask & 0xFF);
    payload[2] = (uint8_t)(mask >> 8);
    for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        payload[3 + index * 3] = colors[index].r;
        payload[4 + index * 3] = colors[index].g;
        payload[5 + index * 3] = colors[index].b;
    }
    payload[3 + RGB9_INDICATOR_COUNT * 3] = flags;
    uint8_t response[QMK_REPORT_SIZE];
    bool ok = qmk_exchange(rgb_device, RGB9_COMMAND, payload,
                           sizeof(payload), response) &&
              response[1] == 0x04 && response[2] == 0x00 &&
              response[3] == (uint8_t)(mask & 0xFF) &&
              response[4] == (uint8_t)(mask >> 8);
    if (ok) {
        rgb9_overlay_active = mask != 0 || flags != 0;
    }
    return ok;
}

static bool qmk_rgb9_clear(void) {
    if (!rgb9_overlay_active || rgb_device == NULL) {
        return true;
    }
    uint8_t subcommand = 0x03;
    uint8_t response[QMK_REPORT_SIZE];
    bool ok = qmk_exchange(rgb_device, RGB9_COMMAND,
                           &subcommand, 1, response) &&
              response[1] == 0x03 && response[2] == 0x00 &&
              response[3] == 0x00 && response[4] == 0x00;
    if (ok) {
        rgb9_overlay_active = false;
    }
    return ok;
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
    if (qmk_original_saved || rgb_device == NULL) {
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

static bool apply_qmk_whole_board(rgb_t color, uint8_t animation_scale) {
    uint8_t hue;
    uint8_t saturation;
    uint8_t brightness;
    rgb_to_hsv(color, &hue, &saturation, &brightness);
    uint32_t combined_scale =
        (uint32_t)animation_scale * light_brightness_percent;
    brightness = (uint8_t)(((uint32_t)brightness * combined_scale + 12750) /
                           25500);
    uint8_t solid_effect = 1;
    uint8_t hsv_color[] = {hue, saturation};
    if (!qmk_task_mode_configured || hue != qmk_applied_hue ||
        saturation != qmk_applied_saturation) {
        if (!qmk_set_menu_value(rgb_device, 0x02, &solid_effect, 1) ||
            !qmk_set_menu_value(rgb_device, 0x04, hsv_color,
                                sizeof(hsv_color))) {
            return false;
        }
        qmk_task_mode_configured = true;
        qmk_applied_hue = hue;
        qmk_applied_saturation = saturation;
        qmk_applied_brightness = -1;
    }
    if (qmk_applied_brightness == brightness) {
        return true;
    }
    if (!qmk_set_menu_value(rgb_device, 0x01, &brightness, 1)) {
        return false;
    }
    qmk_applied_brightness = brightness;
    return true;
}

static void restore_qmk_original(void) {
    if (!qmk_original_saved || rgb_device == NULL) {
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
    if (end_direct_mode) {
        (void)qmk_rgb9_clear();
        restore_qmk_original();
    }
    hid_close(rgb_device);
    rgb_device = NULL;
    rgb9_capability_checked = false;
    rgb9_supported = false;
    rgb9_capability_flags = 0;
    rgb9_overlay_active = false;
    qmk_original_saved = false;
    qmk_task_mode_configured = false;
    qmk_applied_brightness = -1;
}

static bool ensure_rgb_device(void) {
    if (rgb_device != NULL) {
        return true;
    }
    rgb_device = open_rgb_device();
    return rgb_device != NULL;
}

static int status_priority(slot_status_t status) {
    static const int priority[] = {
        [SLOT_OFF] = 0,
        [SLOT_IDLE] = 1,
        [SLOT_COMPLETE] = 2,
        [SLOT_WORKING] = 3,
        [SLOT_WAITING] = 4,
        [SLOT_ERROR] = 5,
    };
    return priority[status];
}

static slot_status_t visible_status_for_thread(
    const watched_thread_t *item) {
    if (item->status == SLOT_WORKING || item->status == SLOT_WAITING ||
        item->status == SLOT_ERROR) {
        return item->status;
    }
    return item->status == SLOT_COMPLETE && item->unread
               ? SLOT_COMPLETE
               : SLOT_OFF;
}

static slot_status_t aggregate_watched_status(void) {
    slot_status_t aggregate = SLOT_OFF;
    for (size_t index = 0; index < watched_count; index++) {
        slot_status_t status = visible_status_for_thread(&watched[index]);
        if (watched[index].active &&
            status_priority(status) >
                status_priority(aggregate)) {
            aggregate = status;
        }
    }
    return aggregate;
}

static uint8_t smooth_breath_scale(double elapsed_seconds,
                                   double cycle_seconds,
                                   uint8_t minimum, uint8_t maximum) {
    double phase = cycle_seconds <= 0.0
                       ? 0.0
                       : fmod(elapsed_seconds, cycle_seconds) / cycle_seconds;
    double triangle = fabs(2.0 * phase - 1.0);
    double smooth = triangle * triangle * (3.0 - 2.0 * triangle);
    double value = minimum + ((double)maximum - minimum) * smooth;
    return (uint8_t)lround(value);
}

static uint8_t error_alert_scale(double elapsed_seconds) {
    if (elapsed_seconds >= ERROR_ALERT_SECONDS) {
        return 255;
    }
    int phase = (int)(elapsed_seconds / 0.2);
    return phase % 2 == 0 ? 255 : 72;
}

static rgb_t base_color_for_status(color_scheme_t scheme,
                                   slot_status_t status) {
    static const rgb_t palettes[COLOR_SCHEME_COUNT][SLOT_ERROR + 1] = {
        [COLOR_SCHEME_CODEX] = {
            [SLOT_OFF] = {0x00, 0x00, 0x00},
            [SLOT_WORKING] = {0x00, 0xFF, 0x4C},
            [SLOT_COMPLETE] = {0x30, 0x4F, 0xFE},
            [SLOT_IDLE] = {0x00, 0x00, 0x00},
            [SLOT_WAITING] = {0xFF, 0x6D, 0x00},
            [SLOT_ERROR] = {0xFF, 0x00, 0x33},
        },
        [COLOR_SCHEME_OCEAN] = {
            [SLOT_OFF] = {0x00, 0x00, 0x00},
            [SLOT_WORKING] = {0x00, 0xE5, 0xA8},
            [SLOT_COMPLETE] = {0x00, 0xB8, 0xFF},
            [SLOT_IDLE] = {0x00, 0x00, 0x00},
            [SLOT_WAITING] = {0xFF, 0xB0, 0x00},
            [SLOT_ERROR] = {0xFF, 0x41, 0x6C},
        },
        [COLOR_SCHEME_VIOLET] = {
            [SLOT_OFF] = {0x00, 0x00, 0x00},
            [SLOT_WORKING] = {0x2D, 0xD4, 0xBF},
            [SLOT_COMPLETE] = {0x8B, 0x5C, 0xF6},
            [SLOT_IDLE] = {0x00, 0x00, 0x00},
            [SLOT_WAITING] = {0xF5, 0x9E, 0x0B},
            [SLOT_ERROR] = {0xE1, 0x1D, 0x48},
        },
    };
    return palettes[scheme][status];
}

static uint8_t animation_scale_for_status(slot_status_t status,
                                          double elapsed_seconds) {
    uint8_t scale = 255;
    if (status == SLOT_WORKING) {
        scale = smooth_breath_scale(elapsed_seconds,
                                    WORKING_BREATH_SECONDS, 112, 255);
    } else if (status == SLOT_WAITING) {
        scale = smooth_breath_scale(elapsed_seconds,
                                    WAITING_BREATH_SECONDS, 96, 255);
    } else if (status == SLOT_ERROR) {
        scale = error_alert_scale(elapsed_seconds);
    }
    return scale;
}

static rgb_t animated_color_for_status(slot_status_t status,
                                       double elapsed_seconds) {
    rgb_t color = base_color_for_status(color_scheme, status);
    uint8_t animation_scale =
        animation_scale_for_status(status, elapsed_seconds);
    uint32_t combined_scale =
        (uint32_t)animation_scale * light_brightness_percent;
    color.r = (uint8_t)(((uint32_t)color.r * combined_scale + 12750) /
                        25500);
    color.g = (uint8_t)(((uint32_t)color.g * combined_scale + 12750) /
                        25500);
    color.b = (uint8_t)(((uint32_t)color.b * combined_scale + 12750) /
                        25500);
    return color;
}

static void collect_number_key_statuses(
    slot_status_t statuses[RGB9_INDICATOR_COUNT]) {
    for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        statuses[index] = SLOT_OFF;
    }
    for (size_t index = 0; index < watched_count; index++) {
        if (watched[index].active && watched[index].slot >= 1 &&
            watched[index].slot <= RGB9_INDICATOR_COUNT) {
            statuses[watched[index].slot - 1] =
                visible_status_for_thread(&watched[index]);
        }
    }
}

static bool apply_whole_board_status(slot_status_t status) {
    if (status == SLOT_IDLE || status == SLOT_OFF) {
        close_rgb_device(true);
        return true;
    }
    if (!ensure_rgb_device()) {
        return false;
    }
    double elapsed_seconds = monotonic_seconds() - animation_started_at;
    rgb_t base_color = base_color_for_status(color_scheme, status);
    uint8_t animation_scale =
        animation_scale_for_status(status, elapsed_seconds);
    bool ok = capture_qmk_original() &&
              apply_qmk_whole_board(base_color, animation_scale);
    if (!ok) {
        log_line("ERROR", "FEKER did not acknowledge the VIA RGB update.");
        close_rgb_device(false);
    }
    return ok;
}

static uint16_t compose_number_key_frame(
    const slot_status_t statuses[RGB9_INDICATOR_COUNT],
    double elapsed_seconds,
    rgb_t colors[RGB9_INDICATOR_COUNT]) {
    memset(colors, 0, sizeof(rgb_t) * RGB9_INDICATOR_COUNT);
    for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        if (statuses[index] == SLOT_OFF) {
            continue;
        }
        colors[index] = animated_color_for_status(statuses[index],
                                                  elapsed_seconds);
    }
    return RGB9_ALL_KEYS_MASK;
}

static bool apply_number_key_statuses(
    const slot_status_t statuses[RGB9_INDICATOR_COUNT]) {
    rgb_t colors[RGB9_INDICATOR_COUNT];
    double elapsed_seconds = monotonic_seconds() - animation_started_at;
    uint16_t mask =
        compose_number_key_frame(statuses, elapsed_seconds, colors);
    if (!ensure_rgb_device()) {
        return false;
    }
    if (!qmk_rgb9_has_capability()) {
        if (!rgb9_requirement_warning_printed) {
            log_line("ERROR",
                     "Number-key lighting requires Threadlight RGB9 firmware for this Alice80.");
            rgb9_requirement_warning_printed = true;
        }
        close_rgb_device(false);
        return false;
    }
    uint8_t flags = 0;
    if ((rgb9_capability_flags & RGB9_FLAG_FOCUS) != 0) {
        flags |= RGB9_FLAG_FOCUS;
        rgb9_focus_warning_printed = false;
    } else if (!rgb9_focus_warning_printed) {
        log_line("NOTICE",
                 "Installed RGB9 firmware does not support focus blackout; "
                 "using the safe nine-key overlay until v0.2.0 is installed.");
        rgb9_focus_warning_printed = true;
    }
    bool ok = qmk_rgb9_set_colors(mask, colors, flags);
    if (!ok) {
        log_line("ERROR", "FEKER did not acknowledge the RGB9 update.");
        close_rgb_device(true);
        return false;
    }
    rgb9_requirement_warning_printed = false;
    return true;
}

static void refresh_lights(void) {
    if (!task_lights_enabled) {
        close_rgb_device(true);
        return;
    }
    bool testing = test_override_until > time(NULL);
    if (!testing && test_override_until != 0) {
        test_override_until = 0;
    }
    if (lighting_scope == LIGHTING_SCOPE_WHOLE_BOARD) {
        slot_status_t status = testing ? test_status
                                       : aggregate_watched_status();
        if (status != rendered_status) {
            rendered_status = status;
            animation_started_at = monotonic_seconds();
        }
        apply_whole_board_status(status);
        return;
    }

    slot_status_t statuses[RGB9_INDICATOR_COUNT];
    if (testing) {
        for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
            statuses[index] = test_status;
        }
    } else {
        collect_number_key_statuses(statuses);
    }
    if (memcmp(statuses, rendered_number_key_statuses,
               sizeof(statuses)) != 0) {
        memcpy(rendered_number_key_statuses, statuses, sizeof(statuses));
        animation_started_at = monotonic_seconds();
    }
    apply_number_key_statuses(statuses);
}

static bool status_animation_needs_tick(void) {
    if (!task_lights_enabled || rgb_device == NULL) {
        return false;
    }
    double elapsed_seconds = monotonic_seconds() - animation_started_at;
    if (lighting_scope == LIGHTING_SCOPE_WHOLE_BOARD) {
        slot_status_t status = test_override_until > time(NULL)
                                   ? test_status
                                   : aggregate_watched_status();
        return status == SLOT_WORKING || status == SLOT_WAITING ||
               (status == SLOT_ERROR && elapsed_seconds < ERROR_ALERT_SECONDS);
    }
    slot_status_t statuses[RGB9_INDICATOR_COUNT];
    if (test_override_until > time(NULL)) {
        for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
            statuses[index] = test_status;
        }
    } else {
        collect_number_key_statuses(statuses);
    }
    for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        slot_status_t status = statuses[index];
        if (status == SLOT_WORKING || status == SLOT_WAITING ||
            (status == SLOT_ERROR && elapsed_seconds < ERROR_ALERT_SECONDS)) {
            return true;
        }
    }
    return false;
}

static bool write_atomic_test_request(slot_status_t status) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             test_request_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%d\n", (int)status);
    if (fclose(file) != 0 || rename(temporary_path, test_request_path) != 0) {
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

static read_state_entry_t *find_read_state(const char *thread_id) {
    for (size_t index = 0; index < read_state_count; index++) {
        if (strcmp(read_states[index].thread_id, thread_id) == 0) {
            return &read_states[index];
        }
    }
    return NULL;
}

static read_state_entry_t *upsert_read_state(const char *thread_id) {
    read_state_entry_t *entry = find_read_state(thread_id);
    if (entry != NULL) {
        return entry;
    }
    if (read_state_count < MAX_THREADS) {
        entry = &read_states[read_state_count++];
    } else {
        size_t oldest_index = 0;
        for (size_t index = 1; index < read_state_count; index++) {
            if (read_states[index].changed_at_ms <
                read_states[oldest_index].changed_at_ms) {
                oldest_index = index;
            }
        }
        entry = &read_states[oldest_index];
    }
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->thread_id, sizeof(entry->thread_id), "%s", thread_id);
    return entry;
}

static bool save_read_state_cache(void) {
#ifdef __OBJC__
    if (read_state_path[0] == '\0') {
        return false;
    }
    @autoreleasepool {
        NSMutableDictionary *states = [NSMutableDictionary dictionary];
        for (size_t index = 0; index < read_state_count; index++) {
            NSString *thread_id =
                [NSString stringWithUTF8String:read_states[index].thread_id];
            if (thread_id == nil) {
                continue;
            }
            states[thread_id] = @{
                @"unread" : @(read_states[index].unread),
                @"changedAtMs" : @(read_states[index].changed_at_ms),
            };
        }
        NSDictionary *root = @{
            @"version" : @1,
            @"threads" : states,
        };
        NSError *error = nil;
        NSData *data = [NSJSONSerialization dataWithJSONObject:root
                                                       options:0
                                                         error:&error];
        if (data == nil || error != nil) {
            return false;
        }
        NSString *path = [NSString stringWithUTF8String:read_state_path];
        if (path == nil || ![data writeToFile:path atomically:YES]) {
            return false;
        }
        return chmod(read_state_path, 0600) == 0;
    }
#else
    return false;
#endif
}

static bool load_read_state_cache(void) {
    read_state_count = 0;
    memset(read_states, 0, sizeof(read_states));
#ifdef __OBJC__
    if (read_state_path[0] == '\0') {
        return false;
    }
    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:read_state_path];
        NSData *data = path == nil ? nil : [NSData dataWithContentsOfFile:path];
        if (data == nil) {
            return access(read_state_path, F_OK) != 0;
        }
        id root = [NSJSONSerialization JSONObjectWithData:data
                                                  options:0
                                                    error:nil];
        if (![root isKindOfClass:[NSDictionary class]]) {
            return false;
        }
        id states = [(NSDictionary *)root objectForKey:@"threads"];
        if (![states isKindOfClass:[NSDictionary class]]) {
            return false;
        }
        for (id key in (NSDictionary *)states) {
            if (read_state_count >= MAX_THREADS ||
                ![key isKindOfClass:[NSString class]]) {
                continue;
            }
            const char *thread_id = [(NSString *)key UTF8String];
            id value = [(NSDictionary *)states objectForKey:key];
            if (thread_id == NULL || thread_id[0] == '\0' ||
                strlen(thread_id) >= sizeof(read_states[0].thread_id) ||
                ![value isKindOfClass:[NSDictionary class]]) {
                continue;
            }
            id unread = [(NSDictionary *)value objectForKey:@"unread"];
            id changed_at =
                [(NSDictionary *)value objectForKey:@"changedAtMs"];
            if (![unread isKindOfClass:[NSNumber class]] ||
                ![changed_at isKindOfClass:[NSNumber class]]) {
                continue;
            }
            read_state_entry_t *entry = &read_states[read_state_count++];
            snprintf(entry->thread_id, sizeof(entry->thread_id), "%s",
                     thread_id);
            entry->unread = [(NSNumber *)unread boolValue];
            entry->changed_at_ms = [(NSNumber *)changed_at longLongValue];
        }
        return true;
    }
#else
    return false;
#endif
}

static void apply_persisted_read_state(watched_thread_t *item) {
    if (item->live_read_state_known || item->status != SLOT_COMPLETE) {
        return;
    }
    read_state_entry_t *entry = find_read_state(item->thread_id);
    if (entry == NULL || entry->changed_at_ms < item->status_changed_at_ms) {
        return;
    }
    item->unread = entry->unread;
}

static void handle_read_state_change(const char *thread_id, bool unread,
                                     int64_t changed_at_ms,
                                     bool persist) {
    if (thread_id == NULL || thread_id[0] == '\0' ||
        strlen(thread_id) >= sizeof(read_states[0].thread_id)) {
        return;
    }
    read_state_entry_t *entry = upsert_read_state(thread_id);
    entry->unread = unread;
    entry->changed_at_ms = changed_at_ms;
    if (persist && !save_read_state_cache()) {
        log_line("WARN", "Unable to persist the Codex read-state cache.");
    }

    watched_thread_t *item = find_thread(thread_id);
    if (item == NULL) {
        return;
    }
    bool changed = item->unread != unread || !item->live_read_state_known;
    item->unread = unread;
    item->live_read_state_known = true;
    if (changed) {
        char message[512];
        snprintf(message, sizeof(message), "Task %d -> %s: %s", item->slot,
                 unread ? "unread" : "read",
                 item->title[0] != '\0' ? item->title : item->thread_id);
        log_line("READ", message);
        refresh_lights();
    }
}

static void update_thread_status(watched_thread_t *item, slot_status_t status) {
    item->status = status;
    item->touched_at = time(NULL);
    item->status_changed_at_ms = realtime_milliseconds();
    if (status == SLOT_WORKING) {
        item->unread = false;
        item->live_read_state_known = codex_ipc_fd >= 0;
    } else if (status == SLOT_COMPLETE) {
        // A completion belongs to the new turn. Read state from an older turn
        // must not suppress the unread result that Codex is about to publish.
        item->unread = false;
        item->live_read_state_known = false;
    }

    char message[512];
    const char *status_name = status == SLOT_WORKING ? "working" :
                              status == SLOT_COMPLETE ? "complete" :
                              status == SLOT_IDLE ? "idle" :
                              status == SLOT_WAITING ? "waiting" :
                              status == SLOT_ERROR ? "error" : "off";
    snprintf(message, sizeof(message), "Task %d -> %s: %s", item->slot,
             status_name, item->title[0] != '\0' ? item->title : item->thread_id);
    log_line("TASK", message);
    refresh_lights();
}

static slot_status_t status_from_line(const char *line, bool *matched) {
    *matched = true;
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\"") != NULL) {
        return SLOT_WORKING;
    }
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\"") != NULL) {
        return SLOT_COMPLETE;
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
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\"") != NULL) {
        return SLOT_IDLE;
    }
    if (strstr(line,
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

static bool load_global_thread_state(
    char pinned_thread_ids[MAX_THREADS][64], size_t *pinned_count,
    bool *pinned_order_available,
    const discovered_thread_t discovered[SIDEBAR_CATALOG_LIMIT],
    size_t discovered_count,
    bool unread_threads[SIDEBAR_CATALOG_LIMIT]) {
    *pinned_count = 0;
    *pinned_order_available = false;
    memset(unread_threads, 0,
           sizeof(bool) * SIDEBAR_CATALOG_LIMIT);
#ifdef __OBJC__
    if (global_state_path[0] == '\0') {
        return false;
    }
    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:global_state_path];
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data == nil) {
            return false;
        }
        id root = [NSJSONSerialization JSONObjectWithData:data
                                                  options:0
                                                    error:nil];
        if (![root isKindOfClass:[NSDictionary class]]) {
            return false;
        }
        NSDictionary *root_dictionary = (NSDictionary *)root;
        id persisted =
            [root_dictionary objectForKey:@"electron-persisted-atom-state"];
        NSDictionary *persisted_dictionary =
            [persisted isKindOfClass:[NSDictionary class]]
                ? (NSDictionary *)persisted
                : nil;

        id pinned_values =
            [root_dictionary objectForKey:@"pinned-thread-ids"];
        if (![pinned_values isKindOfClass:[NSArray class]] &&
            persisted_dictionary != nil) {
            pinned_values =
                [persisted_dictionary objectForKey:@"pinned-thread-ids"];
        }
        if ([pinned_values isKindOfClass:[NSArray class]]) {
            *pinned_order_available = true;
            for (id value in (NSArray *)pinned_values) {
                if (*pinned_count >= MAX_THREADS ||
                    ![value isKindOfClass:[NSString class]]) {
                    continue;
                }
                const char *thread_id = [(NSString *)value UTF8String];
                if (thread_id == NULL || thread_id[0] == '\0' ||
                    strlen(thread_id) >= sizeof(pinned_thread_ids[0])) {
                    continue;
                }
                bool duplicate = false;
                for (size_t index = 0; index < *pinned_count; index++) {
                    if (strcmp(pinned_thread_ids[index], thread_id) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }
                snprintf(pinned_thread_ids[*pinned_count],
                         sizeof(pinned_thread_ids[*pinned_count]), "%s",
                         thread_id);
                (*pinned_count)++;
            }
        }

        id unread_by_host =
            persisted_dictionary == nil
                ? nil
                : [persisted_dictionary
                      objectForKey:@"unread-thread-ids-by-host-v1"];
        if (unread_by_host == nil) {
            unread_by_host =
                [root_dictionary
                    objectForKey:@"unread-thread-ids-by-host-v1"];
        }
        id unread_values = unread_by_host;
        if ([unread_by_host isKindOfClass:[NSDictionary class]]) {
            unread_values =
                [(NSDictionary *)unread_by_host objectForKey:@"local"];
        }
        if ([unread_values isKindOfClass:[NSArray class]]) {
            for (id value in (NSArray *)unread_values) {
                if (![value isKindOfClass:[NSString class]]) {
                    continue;
                }
                const char *thread_id = [(NSString *)value UTF8String];
                if (thread_id == NULL) {
                    continue;
                }
                for (size_t index = 0; index < discovered_count; index++) {
                    if (strcmp(discovered[index].thread_id, thread_id) == 0) {
                        unread_threads[index] = true;
                        break;
                    }
                }
            }
        }
        return true;
    }
#else
    (void)pinned_thread_ids;
    (void)discovered;
    (void)discovered_count;
    return false;
#endif
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
         last_status == SLOT_ERROR || last_status == SLOT_COMPLETE)) {
        item->status = last_status;
        item->touched_at = file_info.st_mtime;
        item->status_changed_at_ms =
            (int64_t)file_info.st_mtime * 1000;
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
        "AND COALESCE(thread_source, '') <> 'subagent' "
        "ORDER BY recency_at_ms DESC, id DESC LIMIT ?";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int(statement, 1, SIDEBAR_CATALOG_LIMIT);

    discovered_thread_t discovered[SIDEBAR_CATALOG_LIMIT];
    size_t discovered_count = 0;
    while (sqlite3_step(statement) == SQLITE_ROW &&
           discovered_count < SIDEBAR_CATALOG_LIMIT) {
        const char *thread_id = (const char *)sqlite3_column_text(statement, 0);
        const char *path = (const char *)sqlite3_column_text(statement, 1);
        const char *title = (const char *)sqlite3_column_text(statement, 2);
        if (thread_id == NULL || path == NULL) {
            continue;
        }
        discovered_thread_t *entry = &discovered[discovered_count++];
        snprintf(entry->thread_id, sizeof(entry->thread_id), "%s", thread_id);
        snprintf(entry->rollout_path, sizeof(entry->rollout_path), "%s", path);
        snprintf(entry->title, sizeof(entry->title), "%s",
                 title != NULL ? title : "");
        sanitize_title(entry->title);
    }
    sqlite3_finalize(statement);

    char pinned_thread_ids[MAX_THREADS][64];
    size_t pinned_count = 0;
    bool pinned_order_available = false;
    bool unread_threads[SIDEBAR_CATALOG_LIMIT];
    bool global_state_loaded = load_global_thread_state(
        pinned_thread_ids, &pinned_count, &pinned_order_available,
        discovered, discovered_count, unread_threads);
    bool ordered[SIDEBAR_CATALOG_LIMIT] = {false};
    size_t display_order[SIDEBAR_CATALOG_LIMIT];
    size_t display_count = 0;
    if (pinned_order_available) {
        for (size_t pinned_index = 0; pinned_index < pinned_count;
             pinned_index++) {
            for (size_t candidate_index = 0;
                 candidate_index < discovered_count; candidate_index++) {
                if (!ordered[candidate_index] &&
                    strcmp(discovered[candidate_index].thread_id,
                           pinned_thread_ids[pinned_index]) == 0) {
                    display_order[display_count++] = candidate_index;
                    ordered[candidate_index] = true;
                    break;
                }
            }
        }
    }
    for (size_t candidate_index = 0; candidate_index < discovered_count;
         candidate_index++) {
        if (!ordered[candidate_index]) {
            display_order[display_count++] = candidate_index;
        }
    }

    size_t previous_count = watched_count;
    bool previous_active[MAX_THREADS];
    bool previous_unread[MAX_THREADS];
    int previous_slots[MAX_THREADS];
    for (size_t index = 0; index < watched_count; index++) {
        previous_active[index] = watched[index].active;
        previous_unread[index] = watched[index].unread;
        previous_slots[index] = watched[index].slot;
        watched[index].active = false;
        watched[index].slot = 0;
    }

    for (size_t position = 0; position < display_count; position++) {
        discovered_thread_t *entry = &discovered[display_order[position]];
        watched_thread_t *item = find_thread(entry->thread_id);
        if (item == NULL) {
            if (watched_count >= MAX_THREADS) {
                continue;
            }
            item = &watched[watched_count++];
            memset(item, 0, sizeof(*item));
            item->status = SLOT_IDLE;
            snprintf(item->thread_id, sizeof(item->thread_id), "%s",
                     entry->thread_id);
            snprintf(item->rollout_path, sizeof(item->rollout_path), "%s",
                     entry->rollout_path);
            snprintf(item->title, sizeof(item->title), "%s", entry->title);
            initialize_rollout_position(item);
        } else {
            snprintf(item->title, sizeof(item->title), "%s", entry->title);
        }
        item->active = true;
        if (global_state_loaded && !item->live_read_state_known) {
            item->unread = unread_threads[display_order[position]];
        }
        apply_persisted_read_state(item);
        if (position < RGB9_INDICATOR_COUNT) {
            item->slot = (int)position + 1;
        }
    }

    bool active_changed = watched_count != previous_count;
    for (size_t index = 0; !active_changed && index < watched_count; index++) {
        bool previous = index < previous_count ? previous_active[index] : false;
        bool was_unread = index < previous_count ? previous_unread[index] : false;
        int previous_slot = index < previous_count ? previous_slots[index] : 0;
        active_changed = watched[index].active != previous ||
                         watched[index].unread != was_unread ||
                         watched[index].slot != previous_slot;
    }
    if (active_changed) {
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

static void process_global_state(sqlite3 *database,
                                 struct timespec *last_mtime) {
    if (file_changed(global_state_path, last_mtime)) {
        discover_threads(database);
    }
}

static void close_codex_ipc(bool report_disconnect) {
    if (codex_ipc_fd >= 0) {
        close(codex_ipc_fd);
        codex_ipc_fd = -1;
    }
    codex_ipc_buffer_length = 0;
    for (size_t index = 0; index < watched_count; index++) {
        watched[index].live_read_state_known = false;
    }
    if (report_disconnect && codex_ipc_connected_once) {
        log_line("IPC", "Codex read-state stream disconnected; retrying.");
    }
}

#ifdef __OBJC__
static bool send_codex_ipc_object(NSDictionary *object) {
    if (codex_ipc_fd < 0 || object == nil) {
        return false;
    }
    NSData *payload = [NSJSONSerialization dataWithJSONObject:object
                                                      options:0
                                                        error:nil];
    if (payload == nil || payload.length == 0 ||
        payload.length > UINT32_MAX) {
        return false;
    }
    uint32_t length = (uint32_t)payload.length;
    uint8_t header[4] = {
        (uint8_t)(length & 0xFF),
        (uint8_t)((length >> 8) & 0xFF),
        (uint8_t)((length >> 16) & 0xFF),
        (uint8_t)((length >> 24) & 0xFF),
    };
    const uint8_t *parts[2] = {header, payload.bytes};
    size_t lengths[2] = {sizeof(header), payload.length};
    for (size_t part = 0; part < 2; part++) {
        size_t sent = 0;
        while (sent < lengths[part]) {
            ssize_t result = send(codex_ipc_fd, parts[part] + sent,
                                  lengths[part] - sent, 0);
            if (result > 0) {
                sent += (size_t)result;
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
    }
    return true;
}

static bool handle_codex_ipc_payload(const uint8_t *payload, size_t length,
                                     bool persist) {
    @autoreleasepool {
        NSData *data = [NSData dataWithBytes:payload length:length];
        id root = [NSJSONSerialization JSONObjectWithData:data
                                                  options:0
                                                    error:nil];
        if (![root isKindOfClass:[NSDictionary class]]) {
            return false;
        }
        NSDictionary *message = (NSDictionary *)root;
        id type = [message objectForKey:@"type"];
        if (![type isKindOfClass:[NSString class]]) {
            return false;
        }
        if ([(NSString *)type isEqualToString:@"client-discovery-request"]) {
            id request_id = [message objectForKey:@"requestId"];
            if (![request_id isKindOfClass:[NSString class]]) {
                return false;
            }
            return send_codex_ipc_object(@{
                @"type" : @"client-discovery-response",
                @"requestId" : request_id,
                @"response" : @{ @"canHandle" : @NO },
            });
        }
        if (![(NSString *)type isEqualToString:@"broadcast"]) {
            return true;
        }
        id method = [message objectForKey:@"method"];
        if (![method isKindOfClass:[NSString class]] ||
            ![(NSString *)method
                isEqualToString:@"thread-read-state-changed"]) {
            return true;
        }
        id params = [message objectForKey:@"params"];
        if (![params isKindOfClass:[NSDictionary class]]) {
            return false;
        }
        id host_id = [(NSDictionary *)params objectForKey:@"hostId"];
        if ([host_id isKindOfClass:[NSString class]] &&
            ![(NSString *)host_id isEqualToString:@"local"]) {
            return true;
        }
        id thread_id =
            [(NSDictionary *)params objectForKey:@"conversationId"];
        id unread =
            [(NSDictionary *)params objectForKey:@"hasUnreadTurn"];
        if (![thread_id isKindOfClass:[NSString class]] ||
            ![unread isKindOfClass:[NSNumber class]]) {
            return false;
        }
        handle_read_state_change([(NSString *)thread_id UTF8String],
                                 [(NSNumber *)unread boolValue],
                                 realtime_milliseconds(), persist);
        return true;
    }
}
#endif

static bool connect_codex_ipc(void) {
    if (codex_ipc_fd >= 0 || ipc_socket_path[0] == '\0') {
        return codex_ipc_fd >= 0;
    }
    if (strlen(ipc_socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return false;
    }
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return false;
    }
#if defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void)setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                     &no_sigpipe, sizeof(no_sigpipe));
#endif
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             ipc_socket_path);
    if (connect(descriptor, (struct sockaddr *)&address,
                sizeof(address)) != 0) {
        close(descriptor);
        return false;
    }
    codex_ipc_fd = descriptor;
    codex_ipc_buffer_length = 0;
#ifdef __OBJC__
    NSString *request_id = [[NSUUID UUID] UUIDString];
    if (!send_codex_ipc_object(@{
            @"type" : @"request",
            @"requestId" : request_id,
            @"method" : @"initialize",
            @"params" : @{ @"clientType" : @"threadlight" },
        })) {
        close_codex_ipc(false);
        return false;
    }
#endif
    int flags = fcntl(codex_ipc_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(codex_ipc_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close_codex_ipc(false);
        return false;
    }
    codex_ipc_connected_once = true;
    for (size_t index = 0; index < watched_count; index++) {
        if (watched[index].active && watched[index].status == SLOT_WORKING) {
            watched[index].unread = false;
            watched[index].live_read_state_known = true;
        }
    }
    log_line("IPC", "Listening for Codex read-state changes.");
    return true;
}

static void process_codex_ipc(void) {
    if (codex_ipc_fd < 0) {
        return;
    }
    for (;;) {
        if (codex_ipc_buffer_length >= sizeof(codex_ipc_buffer)) {
            close_codex_ipc(true);
            return;
        }
        ssize_t received = recv(
            codex_ipc_fd,
            codex_ipc_buffer + codex_ipc_buffer_length,
            sizeof(codex_ipc_buffer) - codex_ipc_buffer_length, 0);
        if (received > 0) {
            codex_ipc_buffer_length += (size_t)received;
        } else if (received == 0) {
            close_codex_ipc(true);
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            close_codex_ipc(true);
            return;
        }
    }

    size_t consumed = 0;
    while (codex_ipc_buffer_length - consumed >= 4) {
        const uint8_t *frame = codex_ipc_buffer + consumed;
        uint32_t length = (uint32_t)frame[0] |
                          ((uint32_t)frame[1] << 8) |
                          ((uint32_t)frame[2] << 16) |
                          ((uint32_t)frame[3] << 24);
        if (length == 0 || length > sizeof(codex_ipc_buffer) - 4) {
            close_codex_ipc(true);
            return;
        }
        if (codex_ipc_buffer_length - consumed < (size_t)length + 4) {
            break;
        }
#ifdef __OBJC__
        if (!handle_codex_ipc_payload(frame + 4, length, true)) {
            log_line("WARN", "Ignoring a malformed Codex IPC message.");
        }
#endif
        consumed += (size_t)length + 4;
    }
    if (consumed > 0) {
        memmove(codex_ipc_buffer, codex_ipc_buffer + consumed,
                codex_ipc_buffer_length - consumed);
        codex_ipc_buffer_length -= consumed;
    }
}

static const char *lighting_scope_identifier(lighting_scope_t scope) {
    return scope == LIGHTING_SCOPE_NUMBER_KEYS ? "number-keys"
                                               : "whole-board";
}

static bool parse_lighting_scope(const char *value,
                                 lighting_scope_t *scope) {
    if (strcmp(value, "whole-board") == 0 || strcmp(value, "keyboard") == 0) {
        *scope = LIGHTING_SCOPE_WHOLE_BOARD;
        return true;
    }
    if (strcmp(value, "number-keys") == 0 || strcmp(value, "per-key") == 0 ||
        strcmp(value, "1-9") == 0) {
        *scope = LIGHTING_SCOPE_NUMBER_KEYS;
        return true;
    }
    return false;
}

static bool load_lighting_scope(void) {
    FILE *file = fopen(lighting_scope_path, "r");
    if (file == NULL) {
        lighting_scope = LIGHTING_SCOPE_WHOLE_BOARD;
        return errno == ENOENT;
    }
    char value[32] = {0};
    bool read_ok = fgets(value, sizeof(value), file) != NULL;
    fclose(file);
    if (!read_ok) {
        return false;
    }
    value[strcspn(value, "\r\n")] = '\0';
    return parse_lighting_scope(value, &lighting_scope);
}

static bool save_lighting_scope(lighting_scope_t scope) {
    if (scope < 0 || scope >= LIGHTING_SCOPE_COUNT) {
        return false;
    }
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             lighting_scope_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%s\n", lighting_scope_identifier(scope));
    if (fclose(file) != 0 || rename(temporary_path, lighting_scope_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    lighting_scope = scope;
    return true;
}

static const char *color_scheme_identifier(color_scheme_t scheme) {
    static const char *identifiers[COLOR_SCHEME_COUNT] = {
        [COLOR_SCHEME_CODEX] = "codex",
        [COLOR_SCHEME_OCEAN] = "ocean",
        [COLOR_SCHEME_VIOLET] = "violet",
    };
    return identifiers[scheme];
}

static bool parse_color_scheme(const char *value, color_scheme_t *scheme) {
    if (strcmp(value, "sunset") == 0) {
        *scheme = COLOR_SCHEME_CODEX;
        return true;
    }
    for (int index = 0; index < COLOR_SCHEME_COUNT; index++) {
        if (strcmp(value, color_scheme_identifier((color_scheme_t)index)) == 0) {
            *scheme = (color_scheme_t)index;
            return true;
        }
    }
    return false;
}

static bool load_color_scheme(void) {
    FILE *file = fopen(color_scheme_path, "r");
    if (file == NULL) {
        color_scheme = COLOR_SCHEME_CODEX;
        return errno == ENOENT;
    }
    char value[32] = {0};
    bool read_ok = fgets(value, sizeof(value), file) != NULL;
    fclose(file);
    if (!read_ok) {
        return false;
    }
    value[strcspn(value, "\r\n")] = '\0';
    return parse_color_scheme(value, &color_scheme);
}

static bool save_color_scheme(color_scheme_t scheme) {
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             color_scheme_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%s\n", color_scheme_identifier(scheme));
    if (fclose(file) != 0 || rename(temporary_path, color_scheme_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    color_scheme = scheme;
    return true;
}

static bool load_brightness(void) {
    FILE *file = fopen(brightness_path, "r");
    if (file == NULL) {
        light_brightness_percent = 68;
        return errno == ENOENT;
    }
    int value = 0;
    bool read_ok = fscanf(file, "%d", &value) == 1;
    fclose(file);
    if (!read_ok || value < 20 || value > 100) {
        return false;
    }
    light_brightness_percent = (uint8_t)value;
    return true;
}

static bool save_brightness(uint8_t percent) {
    if (percent < 20 || percent > 100) {
        return false;
    }
    char temporary_path[PATH_MAX];
    snprintf(temporary_path, sizeof(temporary_path), "%s.%ld.tmp",
             brightness_path, (long)getpid());
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "%u\n", percent);
    if (fclose(file) != 0 || rename(temporary_path, brightness_path) != 0) {
        unlink(temporary_path);
        return false;
    }
    light_brightness_percent = percent;
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

static void process_lighting_scope(struct timespec *last_mtime) {
    if (!file_changed(lighting_scope_path, last_mtime)) {
        return;
    }
    lighting_scope_t previous = lighting_scope;
    if (!load_lighting_scope()) {
        log_line("ERROR", "Ignoring an invalid lighting-scope setting.");
        return;
    }
    if (lighting_scope == previous) {
        return;
    }
    close_rgb_device(true);
    rendered_status = SLOT_OFF;
    memset(rendered_number_key_statuses, 0,
           sizeof(rendered_number_key_statuses));
    log_line("MODE", lighting_scope == LIGHTING_SCOPE_WHOLE_BOARD
                         ? "Whole-keyboard lighting scope enabled."
                         : "Number-key 1-9 lighting scope enabled.");
    refresh_lights();
}

static void process_color_scheme(struct timespec *last_mtime) {
    if (!file_changed(color_scheme_path, last_mtime)) {
        return;
    }
    color_scheme_t previous = color_scheme;
    if (!load_color_scheme()) {
        log_line("ERROR", "Ignoring an invalid color-scheme setting.");
        return;
    }
    if (color_scheme != previous) {
        log_line("MODE", "Task-light color scheme changed.");
        refresh_lights();
    }
}

static void process_brightness(struct timespec *last_mtime) {
    if (!file_changed(brightness_path, last_mtime)) {
        return;
    }
    uint8_t previous = light_brightness_percent;
    if (!load_brightness()) {
        log_line("ERROR", "Ignoring an invalid brightness setting.");
        return;
    }
    if (light_brightness_percent != previous) {
        log_line("MODE", "Task-light brightness changed.");
        refresh_lights();
    }
}

static void process_test_request(struct timespec *last_mtime) {
    if (!file_changed(test_request_path, last_mtime)) {
        return;
    }
    FILE *file = fopen(test_request_path, "r");
    int status = SLOT_OFF;
    if (file != NULL) {
        (void)fscanf(file, "%d", &status);
        fclose(file);
        (void)unlink(test_request_path);
    }
    if (status < SLOT_OFF || status > SLOT_ERROR) {
        return;
    }
    test_status = (slot_status_t)status;
    test_override_until = time(NULL) + 30;
    rendered_status = SLOT_OFF;
    animation_started_at = monotonic_seconds();
    log_line("TEST", "Showing the requested status color for 30 seconds.");
    refresh_lights();
}

static int run_daemon(void) {
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s/daemon.lock", app_support_dir);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        log_line("INFO", "Threadlight is already running.");
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
    if (!load_task_lights_enabled()) {
        log_line("ERROR", "Unable to read the saved task-light state; enabling it.");
        task_lights_enabled = true;
    }
    if (!load_lighting_scope()) {
        log_line("ERROR", "Unable to read lighting scope; using whole keyboard.");
        lighting_scope = LIGHTING_SCOPE_WHOLE_BOARD;
    }
    if (!load_color_scheme()) {
        log_line("ERROR", "Unable to read the color scheme; using Codex colors.");
        color_scheme = COLOR_SCHEME_CODEX;
    }
    if (!load_brightness()) {
        log_line("ERROR", "Unable to read brightness; using 68 percent.");
        light_brightness_percent = 68;
    }
    if (!load_read_state_cache()) {
        log_line("WARN", "Ignoring an invalid Codex read-state cache.");
    }
    log_line("READY", lighting_scope == LIGHTING_SCOPE_WHOLE_BOARD
                          ? "Watching Codex task status on the whole keyboard."
                          : "Watching the first nine Codex tasks on number keys 1-9.");
    refresh_lights();

    unsigned int ticks = 0;
    struct timespec enabled_mtime = {0, 0};
    struct timespec scope_mtime = {0, 0};
    struct timespec scheme_mtime = {0, 0};
    struct timespec brightness_mtime = {0, 0};
    struct timespec test_mtime = {0, 0};
    struct timespec global_state_mtime = {0, 0};
    (void)file_changed(global_state_path, &global_state_mtime);
    while (!should_stop) {
        if (ticks % THREAD_DISCOVERY_TICK_COUNT == 0) {
            discover_threads(database);
            if (rgb_device == NULL) {
                refresh_lights();
            }
        }
        if (ticks % EVENT_POLL_TICK_COUNT == 0) {
            for (size_t index = 0; index < watched_count; index++) {
                if (watched[index].active) {
                    process_appended_events(&watched[index]);
                }
            }
            process_task_lights_enabled(&enabled_mtime);
            process_lighting_scope(&scope_mtime);
            process_color_scheme(&scheme_mtime);
            process_brightness(&brightness_mtime);
            process_test_request(&test_mtime);
            process_global_state(database, &global_state_mtime);
        }
        if (codex_ipc_fd < 0 && ticks % IPC_RECONNECT_TICK_COUNT == 0) {
            (void)connect_codex_ipc();
        }
        process_codex_ipc();
        if (test_override_until != 0 && test_override_until <= time(NULL)) {
            test_override_until = 0;
            refresh_lights();
        }
        if (status_animation_needs_tick() ||
            ticks % LIGHT_RECONCILE_TICK_COUNT == 0) {
            refresh_lights();
        }
        usleep(DAEMON_TICK_INTERVAL_US);
        ticks++;
    }

    close_codex_ipc(false);
    close_rgb_device(true);
    sqlite3_close(database);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    log_line("STOP", "Threadlight stopped.");
    return 0;
}

static int test_status_color(slot_status_t status) {
    (void)load_lighting_scope();
    (void)load_color_scheme();
    (void)load_brightness();
    animation_started_at = monotonic_seconds();
    if (lighting_scope == LIGHTING_SCOPE_WHOLE_BOARD) {
        return apply_whole_board_status(status) ? 0 : 1;
    }
    slot_status_t statuses[RGB9_INDICATOR_COUNT];
    for (int index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        statuses[index] = status;
    }
    return apply_number_key_statuses(statuses) ? 0 : 1;
}

#ifdef __OBJC__
static NSImage *threadlight_menu_icon(bool enabled) {
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(18.0, 18.0)];
    [image lockFocus];

    [[NSColor blackColor] setStroke];
    [[NSColor blackColor] setFill];
    NSBezierPath *keycap = [NSBezierPath
        bezierPathWithRoundedRect:NSMakeRect(2.0, 2.0, 14.0, 14.0)
        xRadius:3.1 yRadius:3.1];

    if (enabled) {
        NSBezierPath *one = [NSBezierPath bezierPath];
        [one moveToPoint:NSMakePoint(6.1, 10.4)];
        [one lineToPoint:NSMakePoint(8.0, 12.0)];
        [one curveToPoint:NSMakePoint(8.8, 12.3)
              controlPoint1:NSMakePoint(8.2, 12.2)
              controlPoint2:NSMakePoint(8.5, 12.3)];
        [one lineToPoint:NSMakePoint(10.2, 12.3)];
        [one curveToPoint:NSMakePoint(10.7, 11.8)
              controlPoint1:NSMakePoint(10.5, 12.3)
              controlPoint2:NSMakePoint(10.7, 12.1)];
        [one lineToPoint:NSMakePoint(10.7, 5.7)];
        [one curveToPoint:NSMakePoint(10.2, 5.2)
              controlPoint1:NSMakePoint(10.7, 5.4)
              controlPoint2:NSMakePoint(10.5, 5.2)];
        [one lineToPoint:NSMakePoint(8.7, 5.2)];
        [one curveToPoint:NSMakePoint(8.2, 5.7)
              controlPoint1:NSMakePoint(8.4, 5.2)
              controlPoint2:NSMakePoint(8.2, 5.4)];
        [one lineToPoint:NSMakePoint(8.2, 9.5)];
        [one lineToPoint:NSMakePoint(7.3, 8.8)];
        [one curveToPoint:NSMakePoint(6.7, 8.9)
              controlPoint1:NSMakePoint(7.1, 8.6)
              controlPoint2:NSMakePoint(6.8, 8.7)];
        [one lineToPoint:NSMakePoint(6.0, 9.7)];
        [one curveToPoint:NSMakePoint(6.1, 10.4)
              controlPoint1:NSMakePoint(5.8, 10.0)
              controlPoint2:NSMakePoint(5.9, 10.2)];
        [one closePath];

        [keycap appendBezierPath:one];
        keycap.windingRule = NSWindingRuleEvenOdd;
        [keycap fill];
    } else {
        keycap.lineWidth = 1.45;
        keycap.lineJoinStyle = NSLineJoinStyleRound;
        [keycap stroke];

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

static NSString *localized_string(NSString *key, NSString *fallback) {
    NSBundle *bundle = [NSBundle mainBundle];
    const char *language_override = getenv("FEKER_LANGUAGE");
    if (language_override != NULL && language_override[0] != '\0') {
        NSString *language = [NSString stringWithUTF8String:language_override];
        NSString *path = [bundle pathForResource:language ofType:@"lproj"];
        NSBundle *language_bundle = path == nil
                                        ? nil
                                        : [NSBundle bundleWithPath:path];
        if (language_bundle != nil) {
            bundle = language_bundle;
        }
    }
    return [bundle localizedStringForKey:key value:fallback table:nil];
}

#define L(key, fallback) localized_string(@key, @fallback)

@interface InsetGroupView : NSView
@end

@implementation InsetGroupView

- (BOOL)isOpaque {
    return NO;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty_rect {
    (void)dirty_rect;
    NSRect border_rect = NSInsetRect(self.bounds, 0.25, 0.25);
    NSBezierPath *path = [NSBezierPath
        bezierPathWithRoundedRect:border_rect xRadius:9.0 yRadius:9.0];
    NSColor *fill = [NSColor.labelColor colorWithAlphaComponent:0.012];
    [fill setFill];
    [path fill];
    [[NSColor.separatorColor colorWithAlphaComponent:0.18] setStroke];
    path.lineWidth = 0.35;
    [path stroke];
}

@end

@interface StatusPreviewView : NSView {
    NSString *preview_title;
    CAGradientLayer *fill_layer;
    CATextLayer *title_layer;
    CGFloat preview_red;
    CGFloat preview_green;
    CGFloat preview_blue;
    CGFloat preview_intensity;
}
- (instancetype)initWithFrame:(NSRect)frame title:(NSString *)title;
- (void)setColor:(NSColor *)color intensity:(CGFloat)intensity;
@end

@implementation StatusPreviewView

- (instancetype)initWithFrame:(NSRect)frame title:(NSString *)title {
    self = [super initWithFrame:frame];
    if (self != nil) {
        preview_title = [title copy];
        preview_red = 0.25;
        preview_green = 0.48;
        preview_blue = 0.95;
        preview_intensity = 1.0;
        self.wantsLayer = YES;

        fill_layer = [CAGradientLayer layer];
        fill_layer.startPoint = CGPointMake(0.5, 1.0);
        fill_layer.endPoint = CGPointMake(0.5, 0.0);
        fill_layer.cornerRadius = 7.0;
        fill_layer.borderWidth = 0.5;
        fill_layer.shadowColor = NSColor.blackColor.CGColor;
        fill_layer.shadowOpacity = 0.10;
        fill_layer.shadowRadius = 1.25;
        fill_layer.shadowOffset = CGSizeMake(0.0, -0.75);
        [self.layer addSublayer:fill_layer];

        title_layer = [CATextLayer layer];
        title_layer.string = preview_title;
        title_layer.alignmentMode = kCAAlignmentCenter;
        title_layer.truncationMode = kCATruncationEnd;
        title_layer.wrapped = NO;
        title_layer.font = (__bridge CFTypeRef)
            [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
        title_layer.fontSize = 11.0;
        [self.layer addSublayer:title_layer];
        [self setColor:NSColor.controlAccentColor intensity:1.0];
    }
    return self;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    CGFloat scale = self.window.screen.backingScaleFactor;
    if (scale <= 0.0) {
        scale = NSScreen.mainScreen.backingScaleFactor;
    }
    fill_layer.contentsScale = scale;
    title_layer.contentsScale = scale;
    [self setNeedsLayout:YES];
}

- (void)layout {
    [super layout];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    fill_layer.frame = self.bounds;
    title_layer.frame = CGRectMake(4.0, 5.25,
                                   NSWidth(self.bounds) - 8.0, 14.0);
    [CATransaction commit];
}

- (void)setColor:(NSColor *)color intensity:(CGFloat)intensity {
    NSColor *rgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    CGFloat red = 0.0;
    CGFloat green = 0.0;
    CGFloat blue = 0.0;
    CGFloat alpha = 1.0;
    [rgb getRed:&red green:&green blue:&blue alpha:&alpha];
    CGFloat next_intensity = fmax(0.04, fmin(1.0, intensity));
    if (fabs(preview_red - red) < 0.0005 &&
        fabs(preview_green - green) < 0.0005 &&
        fabs(preview_blue - blue) < 0.0005 &&
        fabs(preview_intensity - next_intensity) < 0.0005) {
        return;
    }
    preview_red = red;
    preview_green = green;
    preview_blue = blue;
    preview_intensity = next_intensity;
    CGFloat rendered_red = preview_red * preview_intensity;
    CGFloat rendered_green = preview_green * preview_intensity;
    CGFloat rendered_blue = preview_blue * preview_intensity;

    CGFloat top_red = fmin(1.0, rendered_red * 1.12 + 0.018);
    CGFloat top_green = fmin(1.0, rendered_green * 1.12 + 0.018);
    CGFloat top_blue = fmin(1.0, rendered_blue * 1.12 + 0.018);
    CGFloat middle_red = fmin(1.0, rendered_red * 1.025 + 0.004);
    CGFloat middle_green = fmin(1.0, rendered_green * 1.025 + 0.004);
    CGFloat middle_blue = fmin(1.0, rendered_blue * 1.025 + 0.004);
    CGFloat bottom_red = rendered_red * 0.84;
    CGFloat bottom_green = rendered_green * 0.84;
    CGFloat bottom_blue = rendered_blue * 0.84;
    NSColor *top_color = [NSColor colorWithSRGBRed:top_red
                                             green:top_green
                                              blue:top_blue alpha:1.0];
    NSColor *bottom_color = [NSColor colorWithSRGBRed:bottom_red
                                                green:bottom_green
                                                 blue:bottom_blue alpha:1.0];
    NSColor *middle_color = [NSColor colorWithSRGBRed:middle_red
                                                green:middle_green
                                                 blue:middle_blue alpha:1.0];

    CGFloat luminance = rendered_red * 0.299 +
                        rendered_green * 0.587 +
                        rendered_blue * 0.114;
    NSColor *text_color = luminance > 0.66 ? NSColor.blackColor
                                           : NSColor.whiteColor;
    NSColor *border_color = luminance > 0.66
                                ? [NSColor colorWithWhite:0.0 alpha:0.12]
                                : [NSColor colorWithWhite:1.0 alpha:0.22];

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    fill_layer.colors = @[(id)top_color.CGColor,
                          (id)middle_color.CGColor,
                          (id)bottom_color.CGColor];
    fill_layer.locations = @[@0.0, @0.48, @1.0];
    fill_layer.borderColor = border_color.CGColor;
    title_layer.foregroundColor = text_color.CGColor;
    [CATransaction commit];
}

- (void)dealloc {
    [preview_title release];
    [super dealloc];
}

@end

@interface BridgeMenuController : NSObject <NSMenuDelegate, NSWindowDelegate> {
    NSStatusItem *status_item;
    NSMenu *status_menu;
    NSMenuItem *toggle_item;
    NSMenuItem *scheme_items[COLOR_SCHEME_COUNT];
    NSMenuItem *test_menu_item;
    NSMenuItem *login_at_startup_item;
    NSWindow *light_settings_window;
    NSSegmentedControl *scope_control;
    NSSegmentedControl *scheme_control;
    NSSlider *brightness_slider;
    NSTextField *brightness_value_label;
    NSArray<StatusPreviewView *> *status_cards;
    NSTimer *preview_timer;
    CFTimeInterval preview_started_at;
}
@end

@implementation BridgeMenuController

- (instancetype)init {
    self = [super init];
    if (self == nil) {
        return nil;
    }

    status_item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];
    status_item.button.title = @"";
    status_item.button.imagePosition = NSImageOnly;

    status_menu = [[NSMenu alloc] initWithTitle:L("app.menu_title", "Threadlight")];
    status_menu.delegate = self;
    toggle_item = [[NSMenuItem alloc]
        initWithTitle:L("task_lights.on", "Task lights on")
                action:@selector(toggleTaskLights:)
        keyEquivalent:@""];
    toggle_item.target = self;
    [status_menu addItem:toggle_item];
    [status_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *scheme_menu_item = [[NSMenuItem alloc]
        initWithTitle:L("scheme.title", "Color scheme")
                action:nil keyEquivalent:@""];
    NSMenu *scheme_menu = [[NSMenu alloc]
        initWithTitle:L("scheme.title", "Color scheme")];
    NSArray<NSString *> *scheme_names = @[
        L("scheme.codex", "Codex Default"),
        L("scheme.ocean", "Ocean"),
        L("scheme.violet", "Violet"),
    ];
    for (NSInteger index = 0; index < COLOR_SCHEME_COUNT; index++) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:scheme_names[index]
                   action:@selector(chooseColorScheme:)
            keyEquivalent:@""];
        item.target = self;
        item.tag = index;
        scheme_items[index] = item;
        [scheme_menu addItem:item];
    }
    scheme_menu_item.submenu = scheme_menu;
    [status_menu addItem:scheme_menu_item];

    NSMenuItem *light_settings_item = [[NSMenuItem alloc]
        initWithTitle:L("light_settings", "Light Settings…")
                action:@selector(showLightSettings:)
        keyEquivalent:@""];
    light_settings_item.target = self;
    [status_menu addItem:light_settings_item];

    test_menu_item = [[NSMenuItem alloc]
        initWithTitle:L("test.title", "Test Lights")
                action:nil keyEquivalent:@""];
    NSMenu *test_menu = [[NSMenu alloc]
        initWithTitle:L("test.title", "Test Lights")];
    NSArray<NSArray *> *tests = @[
        @[L("test.working", "Working (green breathing)"), @(SLOT_WORKING)],
        @[L("test.complete", "Completed and unread (solid blue)"),
          @(SLOT_COMPLETE)],
        @[L("test.waiting", "Waiting for input"), @(SLOT_WAITING)],
        @[L("test.error", "Task failed"), @(SLOT_ERROR)],
        @[L("test.idle", "Idle"), @(SLOT_IDLE)],
        @[L("test.off", "Turn off test"), @(SLOT_OFF)],
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
        initWithTitle:L("help.title", "How It Works")
                action:nil keyEquivalent:@""];
    NSMenu *help_menu = [[NSMenu alloc]
        initWithTitle:L("help.title", "How It Works")];
    NSArray<NSString *> *instructions = @[
        L("help.open_menu", "Click the icon to open this menu"),
        L("help.toggle", "Use the first item to pause task lights"),
        L("help.scope_board", "Whole Keyboard — highest-priority task"),
        L("help.scope_keys", "Keys 1–9 — first nine recent tasks"),
    ];
    for (NSString *instruction in instructions) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:instruction action:nil keyEquivalent:@""];
        item.enabled = NO;
        [help_menu addItem:item];
    }
    [help_menu addItem:[NSMenuItem separatorItem]];
    NSArray<NSString *> *color_help = @[
        L("help.working", "Working — breathing green"),
        L("help.complete", "Unread result — solid blue until viewed"),
        L("help.waiting", "Waiting — input or approval needed"),
        L("help.error", "Failed — the task could not finish"),
        L("help.idle", "Idle or viewed — key off"),
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
        initWithTitle:L("log.open", "Open Log…")
                action:@selector(openLog:) keyEquivalent:@""];
    open_log.target = self;
    [status_menu addItem:open_log];

    NSMenuItem *settings_item = [[NSMenuItem alloc]
        initWithTitle:L("settings.title", "Settings")
                action:nil keyEquivalent:@""];
    NSMenu *settings_menu = [[NSMenu alloc]
        initWithTitle:L("settings.title", "Settings")];
    login_at_startup_item = [[NSMenuItem alloc]
        initWithTitle:L("startup.title", "Launch at Login")
                action:@selector(toggleLoginAtStartup:)
        keyEquivalent:@""];
    login_at_startup_item.target = self;
    [settings_menu addItem:login_at_startup_item];
    [settings_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *open_login_items = [[NSMenuItem alloc]
        initWithTitle:L("startup.open_settings", "Open Login Items Settings…")
                action:@selector(openLoginItems:)
        keyEquivalent:@""];
    open_login_items.target = self;
    [settings_menu addItem:open_login_items];
    settings_item.submenu = settings_menu;
    [status_menu addItem:settings_item];

    NSMenuItem *project_home = [[NSMenuItem alloc]
        initWithTitle:L("github.open", "Project on GitHub…")
                action:@selector(openProjectHome:)
        keyEquivalent:@""];
    project_home.target = self;
    [status_menu addItem:project_home];

    [status_menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit_item = [[NSMenuItem alloc]
        initWithTitle:L("quit.title", "Quit Threadlight")
                action:@selector(quit:)
        keyEquivalent:@"q"];
    quit_item.target = self;
    [status_menu addItem:quit_item];

    status_item.menu = status_menu;
    (void)load_task_lights_enabled();
    (void)load_lighting_scope();
    (void)load_color_scheme();
    (void)load_brightness();
    [self updateAppearance];
    if (getenv("FEKER_SHOW_MENU_ON_START") != NULL) {
        [self performSelector:@selector(showStatusMenuForTesting)
                   withObject:nil afterDelay:1.0];
    }
    if (getenv("FEKER_SHOW_SETTINGS_ON_START") != NULL) {
        [self performSelector:@selector(showLightSettings:)
                   withObject:nil afterDelay:1.0];
    }
    if (getenv("FEKER_RUN_MENU_SELF_TEST") != NULL) {
        [self performSelector:@selector(runMenuActionSelfTest)
                   withObject:nil afterDelay:1.0];
    }
    if (getenv("FEKER_RUN_MENU_TRACKING_TEST") != NULL) {
        [self performSelector:@selector(showStatusMenuForTesting)
                   withObject:nil afterDelay:1.0];
        NSTimer *timer = [NSTimer
            timerWithTimeInterval:2.0 target:self
                         selector:@selector(closeTrackedMenuAndTestAction:)
                         userInfo:nil repeats:NO];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)buildLightSettingsWindow {
    if (light_settings_window != nil) {
        return;
    }

    light_settings_window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 328, 360)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    light_settings_window.title = L("window.title", "Threadlight");
    light_settings_window.releasedWhenClosed = NO;
    light_settings_window.delegate = self;
    [light_settings_window center];

    NSView *content = light_settings_window.contentView;
    InsetGroupView *status_group = [[InsetGroupView alloc]
        initWithFrame:NSMakeRect(10, 213, 308, 111)];
    [content addSubview:status_group];

    NSTextField *status_title =
        [NSTextField labelWithString:L("status.section", "Status")];
    status_title.frame = NSMakeRect(14, 332, 150, 16);
    status_title.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:status_title];

    NSArray<NSString *> *status_names =
        @[L("status.working", "Working"),
          L("status.complete", "Unread"),
          L("status.waiting", "Waiting"),
          L("status.error", "Failed"),
          L("status.idle", "Idle")];
    NSArray<NSString *> *effect_names =
        @[L("effect.breathing", "Breathing"),
          L("effect.until_viewed", "Solid until viewed"),
          L("effect.slow_pulse", "Slow pulse"),
          L("effect.two_flashes", "2 flashes → solid"),
          L("effect.lights_off", "Lights off")];
    NSMutableArray<StatusPreviewView *> *cards = [NSMutableArray array];
    for (NSInteger index = 0; index < 5; index++) {
        NSInteger column = index % 3;
        NSInteger row = index / 3;
        CGFloat x = row == 0 ? 18 + column * 102
                             : 69 + column * 102;
        CGFloat y = 287 - row * 51;

        StatusPreviewView *card = [[StatusPreviewView alloc]
            initWithFrame:NSMakeRect(x, y, 88, 25)
                    title:status_names[index]];
        [content addSubview:card];
        [cards addObject:card];

        NSTextField *effect = [NSTextField labelWithString:effect_names[index]];
        effect.frame = NSMakeRect(x - 4, y - 17, 96, 13);
        effect.alignment = NSTextAlignmentCenter;
        effect.font = [NSFont systemFontOfSize:9.0];
        effect.textColor =
            [NSColor.secondaryLabelColor colorWithAlphaComponent:0.90];
        [content addSubview:effect];
    }
    status_cards = [cards copy];

    NSTextField *scope_label =
        [NSTextField labelWithString:L("scope.title", "Lighting Scope")];
    scope_label.frame = NSMakeRect(14, 194, 140, 16);
    scope_label.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:scope_label];

    InsetGroupView *scope_group = [[InsetGroupView alloc]
        initWithFrame:NSMakeRect(10, 159, 308, 32)];
    [content addSubview:scope_group];

    scope_control = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(14, 163, 300, 24)];
    scope_control.segmentCount = LIGHTING_SCOPE_COUNT;
    [scope_control setLabel:L("scope.whole_board.short", "Whole Keyboard")
                 forSegment:LIGHTING_SCOPE_WHOLE_BOARD];
    [scope_control setLabel:L("scope.number_keys.short", "Keys 1–9")
                 forSegment:LIGHTING_SCOPE_NUMBER_KEYS];
    scope_control.segmentStyle = NSSegmentStyleRounded;
    scope_control.controlSize = NSControlSizeSmall;
    scope_control.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    scope_control.target = self;
    scope_control.action = @selector(chooseScopeFromSettings:);
    [content addSubview:scope_control];

    NSTextField *scope_tip = [NSTextField
        labelWithString:L("scope.rgb9_note", "Tip: Number Keys 1–9 requires Threadlight RGB9 v0.2+ firmware on a tri-mode Alice80.")];
    scope_tip.frame = NSMakeRect(14, 122, 300, 30);
    scope_tip.font = [NSFont systemFontOfSize:10.5];
    scope_tip.textColor = NSColor.secondaryLabelColor;
    scope_tip.lineBreakMode = NSLineBreakByWordWrapping;
    scope_tip.maximumNumberOfLines = 2;
    [content addSubview:scope_tip];

    NSTextField *scheme_label =
        [NSTextField labelWithString:L("scheme.title", "Color scheme")];
    scheme_label.frame = NSMakeRect(14, 108, 140, 16);
    scheme_label.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:scheme_label];

    InsetGroupView *scheme_group = [[InsetGroupView alloc]
        initWithFrame:NSMakeRect(10, 73, 308, 32)];
    [content addSubview:scheme_group];

    scheme_control = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(14, 77, 300, 24)];
    scheme_control.segmentCount = 3;
    [scheme_control setLabel:@"Codex" forSegment:0];
    [scheme_control setLabel:@"Ocean" forSegment:1];
    [scheme_control setLabel:@"Violet" forSegment:2];
    scheme_control.segmentStyle = NSSegmentStyleRounded;
    scheme_control.controlSize = NSControlSizeSmall;
    scheme_control.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    scheme_control.target = self;
    scheme_control.action = @selector(chooseSchemeFromSettings:);
    [content addSubview:scheme_control];

    NSTextField *brightness_label =
        [NSTextField labelWithString:L("brightness.title", "Brightness")];
    brightness_label.frame = NSMakeRect(14, 44, 110, 16);
    brightness_label.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:brightness_label];

    brightness_value_label = [NSTextField labelWithString:@"68%"];
    brightness_value_label.frame = NSMakeRect(266, 44, 48, 16);
    brightness_value_label.alignment = NSTextAlignmentRight;
    brightness_value_label.font = [NSFont systemFontOfSize:11];
    brightness_value_label.textColor = NSColor.secondaryLabelColor;
    [content addSubview:brightness_value_label];

    brightness_slider = [[NSSlider alloc]
        initWithFrame:NSMakeRect(14, 13, 300, 18)];
    brightness_slider.minValue = 20;
    brightness_slider.maxValue = 100;
    brightness_slider.continuous = YES;
    brightness_slider.controlSize = NSControlSizeSmall;
    [brightness_slider sendActionOn:NSEventMaskLeftMouseDown |
                                   NSEventMaskLeftMouseDragged |
                                   NSEventMaskLeftMouseUp];
    brightness_slider.target = self;
    brightness_slider.action = @selector(brightnessChanged:);
    [content addSubview:brightness_slider];

}

- (void)startSettingsPreviewTimer {
    [preview_timer invalidate];
    preview_timer = [NSTimer
        timerWithTimeInterval:SETTINGS_PREVIEW_INTERVAL target:self
                     selector:@selector(animateSettingsPreview:)
                     userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:preview_timer
                              forMode:NSRunLoopCommonModes];
}

- (void)stopSettingsPreviewTimer {
    [preview_timer invalidate];
    preview_timer = nil;
}

- (void)windowWillClose:(NSNotification *)notification {
    if (notification.object == light_settings_window) {
        [self stopSettingsPreviewTimer];
    }
}

- (void)animateSettingsPreview:(NSTimer *)timer {
    (void)timer;
    if (light_settings_window == nil || !light_settings_window.visible) {
        return;
    }
    static const slot_status_t statuses[] = {
        SLOT_WORKING, SLOT_COMPLETE, SLOT_WAITING, SLOT_ERROR, SLOT_IDLE,
    };
    double elapsed_seconds = CACurrentMediaTime() - preview_started_at;
    for (NSInteger index = 0; index < 5; index++) {
        slot_status_t status = statuses[index];
        uint8_t scale = animation_scale_for_status(status, elapsed_seconds);
        rgb_t base = base_color_for_status(color_scheme, status);
        NSColor *color = [NSColor colorWithSRGBRed:base.r / 255.0
                                             green:base.g / 255.0
                                              blue:base.b / 255.0
                                             alpha:1.0];
        CGFloat intensity = ((CGFloat)scale / 255.0) *
                            ((CGFloat)light_brightness_percent / 100.0);
        [status_cards[index] setColor:color intensity:intensity];
    }
}

- (void)showLightSettings:(id)sender {
    (void)sender;
    [self buildLightSettingsWindow];
    (void)load_lighting_scope();
    (void)load_color_scheme();
    (void)load_brightness();
    scope_control.selectedSegment = lighting_scope;
    scheme_control.selectedSegment = color_scheme;
    brightness_slider.integerValue = light_brightness_percent;
    brightness_value_label.stringValue =
        [NSString stringWithFormat:@"%u%%", light_brightness_percent];
    preview_started_at = CACurrentMediaTime();
    [self startSettingsPreviewTimer];
    [NSApp activateIgnoringOtherApps:YES];
    [light_settings_window makeKeyAndOrderFront:nil];
    [self animateSettingsPreview:nil];
}

- (void)chooseScopeFromSettings:(NSSegmentedControl *)sender {
    lighting_scope_t selected =
        (lighting_scope_t)sender.selectedSegment;
    if (selected < 0 || selected >= LIGHTING_SCOPE_COUNT) {
        return;
    }
    if (!save_lighting_scope(selected)) {
        log_line("ERROR", "Unable to save the lighting scope.");
        return;
    }
    [self updateAppearance];
}

- (void)chooseSchemeFromSettings:(NSSegmentedControl *)sender {
    color_scheme_t selected = (color_scheme_t)sender.selectedSegment;
    if (selected < 0 || selected >= COLOR_SCHEME_COUNT) {
        return;
    }
    if (!save_color_scheme(selected)) {
        log_line("ERROR", "Unable to save the color scheme.");
        return;
    }
    preview_started_at = CACurrentMediaTime();
    [self animateSettingsPreview:nil];
    [self updateAppearance];
}

- (void)brightnessChanged:(NSSlider *)sender {
    NSInteger requested = llround(sender.doubleValue);
    uint8_t percent = (uint8_t)MAX(20, MIN(100, requested));
    light_brightness_percent = percent;
    brightness_value_label.stringValue =
        [NSString stringWithFormat:@"%u%%", percent];
    [self animateSettingsPreview:nil];
    if (!save_brightness(percent)) {
        log_line("ERROR", "Unable to save brightness.");
    }
}

- (void)showStatusMenuForTesting {
    [status_item.button performClick:nil];
}

- (void)runMenuActionSelfTest {
    bool original_enabled = task_lights_enabled;
    lighting_scope_t original_scope = lighting_scope;
    color_scheme_t original_scheme = color_scheme;
    uint8_t original_brightness = light_brightness_percent;
    log_line("UI-TEST", "Starting menu action regression test.");

    [NSApp sendAction:toggle_item.action
                   to:toggle_item.target from:toggle_item];
    [NSApp sendAction:toggle_item.action
                   to:toggle_item.target from:toggle_item];
    if (task_lights_enabled != original_enabled) {
        (void)save_task_lights_enabled(original_enabled);
    }

    [self buildLightSettingsWindow];
    for (NSInteger index = 0; index < LIGHTING_SCOPE_COUNT; index++) {
        scope_control.selectedSegment = index;
        [self chooseScopeFromSettings:scope_control];
    }
    (void)save_lighting_scope(original_scope);

    for (NSInteger index = 0; index < COLOR_SCHEME_COUNT; index++) {
        [NSApp sendAction:scheme_items[index].action
                       to:scheme_items[index].target
                     from:scheme_items[index]];
    }
    (void)save_color_scheme(original_scheme);
    [self updateAppearance];

    [self showLightSettings:nil];
    brightness_slider.integerValue = original_brightness == 100
                                         ? 99
                                         : original_brightness + 1;
    [self brightnessChanged:brightness_slider];
    (void)save_brightness(original_brightness);
    [light_settings_window orderOut:nil];
    [self stopSettingsPreviewTimer];
    [self updateAppearance];

    NSMenuItem *off_test = [test_menu_item.submenu itemAtIndex:5];
    [NSApp sendAction:off_test.action
                   to:off_test.target from:off_test];
    log_line("UI-TEST", "Menu action regression test passed.");
}

- (void)closeTrackedMenuAndTestAction:(NSTimer *)timer {
    (void)timer;
    [status_menu cancelTracking];
    [NSApp sendAction:toggle_item.action
                   to:toggle_item.target from:toggle_item];
    [NSApp sendAction:toggle_item.action
                   to:toggle_item.target from:toggle_item];
    log_line("UI-TEST", "Tracked menu open/close regression test passed.");
}

- (void)updateAppearance {
    toggle_item.title = task_lights_enabled
                            ? L("task_lights.on", "Task lights on")
                            : L("task_lights.off", "Task lights paused");
    toggle_item.state = task_lights_enabled ? NSControlStateValueOn
                                            : NSControlStateValueOff;
    test_menu_item.enabled = task_lights_enabled;
    for (NSInteger index = 0; index < COLOR_SCHEME_COUNT; index++) {
        scheme_items[index].state =
            (NSInteger)color_scheme == index ? NSControlStateValueOn
                                             : NSControlStateValueOff;
    }
    if (scope_control != nil) {
        scope_control.selectedSegment = lighting_scope;
    }
    if (scheme_control != nil) {
        scheme_control.selectedSegment = color_scheme;
    }
    if (brightness_slider != nil) {
        brightness_slider.integerValue = light_brightness_percent;
        brightness_value_label.stringValue =
            [NSString stringWithFormat:@"%u%%", light_brightness_percent];
    }
    status_item.button.image = threadlight_menu_icon(task_lights_enabled);
    status_item.button.toolTip = task_lights_enabled
                                     ? L("tooltip.on", "Threadlight is on · Click for menu")
                                     : L("tooltip.off", "Threadlight is paused · Click for menu");

    if (@available(macOS 13.0, *)) {
        SMAppServiceStatus login_status = [SMAppService mainAppService].status;
        login_at_startup_item.state = login_status == SMAppServiceStatusEnabled
                                          ? NSControlStateValueOn
                                          : NSControlStateValueOff;
        login_at_startup_item.title =
            login_status == SMAppServiceStatusRequiresApproval
                ? L("startup.requires_approval",
                    "Launch at Login (approval required)")
                : L("startup.title", "Launch at Login");
    } else {
        login_at_startup_item.enabled = NO;
        login_at_startup_item.title =
            L("startup.requires_macos", "Launch at Login (requires macOS 13)");
    }

}

- (void)menuWillOpen:(NSMenu *)menu {
    (void)menu;
    log_line("UI", "Status menu opened.");
    (void)load_task_lights_enabled();
    (void)load_lighting_scope();
    (void)load_color_scheme();
    (void)load_brightness();
    [self updateAppearance];
}

- (void)toggleTaskLights:(id)sender {
    (void)sender;
    log_line("UI", task_lights_enabled
                       ? "Menu action: pause task lights."
                       : "Menu action: enable task lights.");
    if (!save_task_lights_enabled(!task_lights_enabled)) {
        log_line("ERROR", "Unable to save the task-light enabled state.");
    }
    [self updateAppearance];
}

- (void)chooseColorScheme:(NSMenuItem *)sender {
    color_scheme_t selected = (color_scheme_t)sender.tag;
    if (selected < 0 || selected >= COLOR_SCHEME_COUNT) {
        return;
    }
    log_line("UI", "Menu action: change the task-light color scheme.");
    if (!save_color_scheme(selected)) {
        log_line("ERROR", "Unable to save the color scheme.");
    }
    preview_started_at = CACurrentMediaTime();
    [self animateSettingsPreview:nil];
    [self updateAppearance];
}

- (void)testStatus:(NSMenuItem *)sender {
    log_line("UI", "Menu action: run a lighting test.");
    if (!write_atomic_test_request((slot_status_t)sender.tag)) {
        log_line("ERROR", "Unable to send the lighting test request.");
    }
}

- (void)openLog:(id)sender {
    (void)sender;
    NSString *path = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Logs/Threadlight.log"];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
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
    log_line("UI", "Menu action: toggle launch at login.");
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
    log_line("UI", "Menu action: open the GitHub project page.");
    NSURL *url = [NSURL
        URLWithString:@"https://github.com/chenzixin1/threadlight"];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)quit:(id)sender {
    (void)sender;
    log_line("UI", "Menu action: request application quit.");
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = L("quit.message", "Quit Threadlight?");
    alert.informativeText =
        L("quit.informative",
          "Task lights will stop and the keyboard's original RGB effect will return.");
    [alert addButtonWithTitle:L("quit.cancel", "Cancel")];
    [alert addButtonWithTitle:L("quit.confirm", "Quit")];
    [NSApp activateIgnoringOtherApps:YES];
    if ([alert runModal] != NSAlertSecondButtonReturn) {
        log_line("UI", "Application quit cancelled.");
        return;
    }
    log_line("UI", "Application quit confirmed.");
    should_stop = 1;
    [NSApp stop:nil];
    CFRunLoopStop(CFRunLoopGetCurrent());
}

- (void)pollForTermination:(NSTimer *)timer {
    (void)timer;
    if (should_stop) {
        [NSApp stop:nil];
    }
}

@end

static BridgeMenuController *menu_controller = nil;

static void setup_menu_bar_ui(void) {
    [[NSProcessInfo processInfo]
        disableAutomaticTermination:@"FEKER task-light background service"];
    [[NSProcessInfo processInfo] disableSuddenTermination];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    menu_controller = [[BridgeMenuController alloc] init];
    NSTimer *termination_timer = [NSTimer
        timerWithTimeInterval:0.25 target:menu_controller
                     selector:@selector(pollForTermination:)
                     userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:termination_timer
                              forMode:NSRunLoopCommonModes];
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
        execl(executable_path, executable_path, "--daemon", (char *)NULL);
        fprintf(stderr, "Unable to launch the QMK light service: %s\n",
                strerror(errno));
        _exit(1);
    }
    if (setpgid(child_pid, child_pid) != 0 && errno != EACCES) {
        log_line("ERROR", "Unable to supervise the light-service process group.");
        (void)kill(child_pid, SIGTERM);
        (void)waitpid(child_pid, NULL, 0);
        return 1;
    }

    setup_menu_bar_ui();
    log_line("READY", "Menu bar UI is accepting mouse input.");
    [NSApp run];
    stop_light_service(child_pid);
    return 0;
}
#endif

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --agent\n"
            "  %s --daemon\n"
            "  %s --task-lights on|off\n"
            "  %s --scope whole-board|number-keys\n"
            "  %s --scheme codex|ocean|violet\n"
            "  %s --brightness 20..100\n"
            "  %s --request-test [working|complete|idle|waiting|error|off]\n"
            "  %s --test [working|complete|idle|waiting|error|off]\n"
            "  %s --off\n",
            program, program, program, program, program, program, program,
            program, program);
}

static bool parse_status(const char *name, slot_status_t *status) {
    if (strcmp(name, "working") == 0 || strcmp(name, "green") == 0) {
        *status = SLOT_WORKING;
    } else if (strcmp(name, "complete") == 0 || strcmp(name, "blue") == 0 ||
               strcmp(name, "unread") == 0) {
        *status = SLOT_COMPLETE;
    } else if (strcmp(name, "idle") == 0 || strcmp(name, "black") == 0 ||
               strcmp(name, "white") == 0) {
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
    if (argc == 1) {
        @autoreleasepool {
            result = run_agent();
        }
    } else
#endif
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0) {
        result = run_daemon();
#ifdef __OBJC__
    } else if (argc == 2 && strcmp(argv[1], "--agent") == 0) {
        @autoreleasepool {
            result = run_agent();
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
    } else if (argc == 3 &&
               (strcmp(argv[1], "--scope") == 0 ||
                strcmp(argv[1], "--mode") == 0)) {
        lighting_scope_t scope;
        if (!parse_lighting_scope(argv[2], &scope)) {
            print_usage(argv[0]);
            result = 2;
        } else {
            result = save_lighting_scope(scope) ? 0 : 1;
        }
    } else if (argc == 3 && strcmp(argv[1], "--scheme") == 0) {
        color_scheme_t scheme;
        if (!parse_color_scheme(argv[2], &scheme)) {
            print_usage(argv[0]);
            result = 2;
        } else {
            result = save_color_scheme(scheme) ? 0 : 1;
        }
    } else if (argc == 3 && strcmp(argv[1], "--brightness") == 0) {
        char *end = NULL;
        long value = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || value < 20 || value > 100) {
            print_usage(argv[0]);
            result = 2;
        } else {
            result = save_brightness((uint8_t)value) ? 0 : 1;
        }
    } else if ((argc == 2 || argc == 3) &&
               strcmp(argv[1], "--request-test") == 0) {
        slot_status_t status = SLOT_COMPLETE;
        if (argc == 3 && !parse_status(argv[2], &status)) {
            print_usage(argv[0]);
            result = 2;
        } else if (!write_atomic_test_request(status)) {
            fputs("Unable to write the test request.\n", stderr);
            result = 2;
        }
    } else if ((argc == 2 || argc == 3) && strcmp(argv[1], "--test") == 0) {
        slot_status_t status = SLOT_COMPLETE;
        if (argc == 3 && !parse_status(argv[2], &status)) {
            print_usage(argv[0]);
            result = 2;
            goto done;
        }
        result = test_status_color(status);
    } else if (argc == 2 && strcmp(argv[1], "--off") == 0) {
        result = test_status_color(SLOT_OFF);
    } else {
        print_usage(argv[0]);
        result = 2;
    }

done:
    hid_exit();
    return result;
}
