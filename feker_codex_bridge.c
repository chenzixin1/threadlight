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
#import <QuartzCore/QuartzCore.h>
#import <ServiceManagement/ServiceManagement.h>
#endif

#define FEKER_QMK_VID 0x36B0
#define FEKER_QMK_PID 0x305F
#define FEKER_QMK_USAGE_PAGE 0xFF60
#define FEKER_QMK_USAGE 0x0061
#define QMK_REPORT_SIZE 32
#define DAEMON_TICK_INTERVAL_US 33333
#define EVENT_POLL_TICK_COUNT 3
#define THREAD_DISCOVERY_TICK_COUNT 60
#define SETTINGS_PREVIEW_INTERVAL (1.0 / 60.0)
#define WORKING_BREATH_SECONDS 3.6
#define COMPLETE_BREATH_CYCLE_SECONDS 1.4
#define COMPLETE_BREATH_TOTAL_SECONDS 2.8
#define WAITING_BREATH_SECONDS 4.4
#define ERROR_ALERT_SECONDS 0.8
#define COMPLETE_RECOVERY_WINDOW_SECONDS 300
#define MAX_THREADS 128
#define MAX_TITLE 256

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

typedef struct {
    char thread_id[64];
    char rollout_path[PATH_MAX];
    char title[MAX_TITLE];
    off_t offset;
    slot_status_t status;
    time_t touched_at;
    bool initialized;
    bool active;
} watched_thread_t;

static volatile sig_atomic_t should_stop = 0;
static watched_thread_t watched[MAX_THREADS];
static size_t watched_count = 0;
static bool hid_permission_warning_printed = false;
static hid_device *rgb_device = NULL;
static bool qmk_generation_warning_printed = false;
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
static char color_scheme_path[PATH_MAX];
static char brightness_path[PATH_MAX];
static char database_path[PATH_MAX];
static char log_path[PATH_MAX];
static char executable_path[PATH_MAX];
static slot_status_t test_status = SLOT_OFF;
static time_t test_override_until = 0;
static bool task_lights_enabled = true;
static color_scheme_t color_scheme = COLOR_SCHEME_CODEX;
static uint8_t light_brightness_percent = 68;
static double animation_started_at = 0.0;
static slot_status_t rendered_status = SLOT_OFF;
static bool completion_latched = false;

static void handle_signal(int signal_number) {
    (void)signal_number;
    should_stop = 1;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
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
    snprintf(app_support_dir, sizeof(app_support_dir),
             "%s/Feker Codex Bridge", application_support_dir);
    snprintf(test_request_path, sizeof(test_request_path),
             "%s/test-request.tsv", app_support_dir);
    snprintf(task_lights_enabled_path, sizeof(task_lights_enabled_path),
             "%s/task-lights-enabled.txt", app_support_dir);
    snprintf(color_scheme_path, sizeof(color_scheme_path),
             "%s/color-scheme.txt", app_support_dir);
    snprintf(brightness_path, sizeof(brightness_path),
             "%s/brightness.txt", app_support_dir);
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

static hid_device *open_rgb_device(void) {
    struct hid_device_info *devices =
        hid_enumerate(FEKER_QMK_VID, FEKER_QMK_PID);
    const char *chosen_path = NULL;
    char path_copy[PATH_MAX];

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (item->usage_page == FEKER_QMK_USAGE_PAGE &&
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
                     "(expected 36B0:305F, usage FF60:0061).");
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
                     "FEKER QMK/VIA keyboard detected; using whole-keyboard "
                     "status colors.");
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
        restore_qmk_original();
    }
    hid_close(rgb_device);
    rgb_device = NULL;
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

static slot_status_t aggregate_watched_status(void) {
    slot_status_t aggregate = SLOT_OFF;
    for (size_t index = 0; index < watched_count; index++) {
        slot_status_t status = watched[index].status == SLOT_COMPLETE
                                   ? SLOT_IDLE
                                   : watched[index].status;
        if (watched[index].active &&
            status_priority(status) >
                status_priority(aggregate)) {
            aggregate = status;
        }
    }
    if (completion_latched && aggregate != SLOT_ERROR &&
        aggregate != SLOT_WAITING) {
        return SLOT_COMPLETE;
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
            [SLOT_WORKING] = {0x30, 0x4F, 0xFE},
            [SLOT_COMPLETE] = {0x00, 0xFF, 0x4C},
            [SLOT_IDLE] = {0xFF, 0xFF, 0xFF},
            [SLOT_WAITING] = {0xFF, 0x6D, 0x00},
            [SLOT_ERROR] = {0xFF, 0x00, 0x33},
        },
        [COLOR_SCHEME_OCEAN] = {
            [SLOT_OFF] = {0x00, 0x00, 0x00},
            [SLOT_WORKING] = {0x00, 0xB8, 0xFF},
            [SLOT_COMPLETE] = {0x00, 0xE5, 0xA8},
            [SLOT_IDLE] = {0xBD, 0xEB, 0xFF},
            [SLOT_WAITING] = {0xFF, 0xB0, 0x00},
            [SLOT_ERROR] = {0xFF, 0x41, 0x6C},
        },
        [COLOR_SCHEME_VIOLET] = {
            [SLOT_OFF] = {0x00, 0x00, 0x00},
            [SLOT_WORKING] = {0x8B, 0x5C, 0xF6},
            [SLOT_COMPLETE] = {0x2D, 0xD4, 0xBF},
            [SLOT_IDLE] = {0xF3, 0xE8, 0xFF},
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
    } else if (status == SLOT_COMPLETE &&
               elapsed_seconds < COMPLETE_BREATH_TOTAL_SECONDS) {
        scale = smooth_breath_scale(elapsed_seconds,
                                    COMPLETE_BREATH_CYCLE_SECONDS, 118, 255);
    } else if (status == SLOT_WAITING) {
        scale = smooth_breath_scale(elapsed_seconds,
                                    WAITING_BREATH_SECONDS, 96, 255);
    } else if (status == SLOT_ERROR) {
        scale = error_alert_scale(elapsed_seconds);
    }
    return scale;
}

static bool apply_status_color(slot_status_t status) {
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

static void refresh_lights(void) {
    if (!task_lights_enabled) {
        close_rgb_device(true);
        return;
    }
    slot_status_t status;
    if (test_override_until > time(NULL)) {
        status = test_status;
    } else {
        if (test_override_until != 0) {
            test_override_until = 0;
        }
        status = aggregate_watched_status();
    }
    if (status != rendered_status) {
        rendered_status = status;
        animation_started_at = monotonic_seconds();
    }
    apply_status_color(status);
}

static bool status_animation_needs_tick(void) {
    if (!task_lights_enabled || rgb_device == NULL) {
        return false;
    }
    slot_status_t status;
    if (test_override_until > time(NULL)) {
        status = test_status;
    } else {
        status = aggregate_watched_status();
    }
    double elapsed_seconds = monotonic_seconds() - animation_started_at;
    return status == SLOT_WORKING || status == SLOT_WAITING ||
           (status == SLOT_COMPLETE &&
            elapsed_seconds < COMPLETE_BREATH_TOTAL_SECONDS) ||
           (status == SLOT_ERROR && elapsed_seconds < ERROR_ALERT_SECONDS);
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

static void update_thread_status(watched_thread_t *item, slot_status_t status) {
    if (status == SLOT_WORKING) {
        completion_latched = false;
    } else if (status == SLOT_COMPLETE) {
        completion_latched = true;
    }
    item->status = status;
    item->touched_at = time(NULL);

    char message[512];
    const char *status_name = status == SLOT_WORKING ? "working" :
                              status == SLOT_COMPLETE ? "complete" :
                              status == SLOT_IDLE ? "idle" :
                              status == SLOT_WAITING ? "waiting" :
                              status == SLOT_ERROR ? "error" : "off";
    snprintf(message, sizeof(message), "Task -> %s: %s",
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

    bool recent_completion =
        difftime(time(NULL), file_info.st_mtime) <=
            COMPLETE_RECOVERY_WINDOW_SECONDS;
    if (recently_updated && saw_event &&
        (last_status == SLOT_WORKING || last_status == SLOT_WAITING ||
         last_status == SLOT_ERROR ||
         (last_status == SLOT_COMPLETE && recent_completion))) {
        item->status = last_status;
        item->touched_at = file_info.st_mtime;
        if (last_status == SLOT_COMPLETE) {
            completion_latched = true;
        }
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

    size_t previous_count = watched_count;
    bool previous_active[MAX_THREADS];
    for (size_t index = 0; index < watched_count; index++) {
        previous_active[index] = watched[index].active;
        watched[index].active = false;
    }

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
        item->active = true;
    }
    sqlite3_finalize(statement);

    bool active_changed = watched_count != previous_count;
    for (size_t index = 0; !active_changed && index < watched_count; index++) {
        bool previous = index < previous_count ? previous_active[index] : false;
        active_changed = watched[index].active != previous;
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
        log_line("MODE", "Whole-board color scheme changed.");
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
        log_line("MODE", "Whole-board brightness changed.");
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
    if (!load_task_lights_enabled()) {
        log_line("ERROR", "Unable to read the saved task-light state; enabling it.");
        task_lights_enabled = true;
    }
    if (!load_color_scheme()) {
        log_line("ERROR", "Unable to read the color scheme; using Codex colors.");
        color_scheme = COLOR_SCHEME_CODEX;
    }
    if (!load_brightness()) {
        log_line("ERROR", "Unable to read brightness; using 68 percent.");
        light_brightness_percent = 68;
    }
    log_line("READY", "Watching Codex task status for whole-board lighting.");
    refresh_lights();

    unsigned int ticks = 0;
    struct timespec enabled_mtime = {0, 0};
    struct timespec scheme_mtime = {0, 0};
    struct timespec brightness_mtime = {0, 0};
    struct timespec test_mtime = {0, 0};
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
            process_color_scheme(&scheme_mtime);
            process_brightness(&brightness_mtime);
            process_test_request(&test_mtime);
        }
        if (test_override_until != 0 && test_override_until <= time(NULL)) {
            test_override_until = 0;
            refresh_lights();
        }
        if (status_animation_needs_tick()) {
            refresh_lights();
        }
        usleep(DAEMON_TICK_INTERVAL_US);
        ticks++;
    }

    close_rgb_device(true);
    sqlite3_close(database);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    log_line("STOP", "Feker Codex Bridge stopped.");
    return 0;
}

static int test_status_color(slot_status_t status) {
    (void)load_color_scheme();
    (void)load_brightness();
    animation_started_at = monotonic_seconds();
    return apply_status_color(status) ? 0 : 1;
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
    NSColor *fill = [NSColor.controlBackgroundColor
        colorWithAlphaComponent:0.52];
    [fill setFill];
    [path fill];
    [[NSColor.separatorColor colorWithAlphaComponent:0.42] setStroke];
    path.lineWidth = 0.5;
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
        [self.layer addSublayer:fill_layer];

        title_layer = [CATextLayer layer];
        title_layer.string = preview_title;
        title_layer.alignmentMode = kCAAlignmentCenter;
        title_layer.truncationMode = kCATruncationEnd;
        title_layer.wrapped = NO;
        title_layer.font = (__bridge CFTypeRef)
            [NSFont systemFontOfSize:10.5 weight:NSFontWeightSemibold];
        title_layer.fontSize = 10.5;
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
    title_layer.frame = CGRectMake(4.0, 5.0,
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

    CGFloat top_red = fmin(1.0, rendered_red * 1.04 + 0.012);
    CGFloat top_green = fmin(1.0, rendered_green * 1.04 + 0.012);
    CGFloat top_blue = fmin(1.0, rendered_blue * 1.04 + 0.012);
    CGFloat bottom_red = rendered_red * 0.96;
    CGFloat bottom_green = rendered_green * 0.96;
    CGFloat bottom_blue = rendered_blue * 0.96;
    NSColor *top_color = [NSColor colorWithSRGBRed:top_red
                                             green:top_green
                                              blue:top_blue alpha:1.0];
    NSColor *bottom_color = [NSColor colorWithSRGBRed:bottom_red
                                                green:bottom_green
                                                 blue:bottom_blue alpha:1.0];

    CGFloat luminance = rendered_red * 0.299 +
                        rendered_green * 0.587 +
                        rendered_blue * 0.114;
    NSColor *text_color = luminance > 0.66 ? NSColor.blackColor
                                           : NSColor.whiteColor;
    NSColor *border_color = luminance > 0.66
                                ? [NSColor colorWithWhite:0.0 alpha:0.10]
                                : [NSColor colorWithWhite:1.0 alpha:0.16];

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    fill_layer.colors = @[(id)top_color.CGColor, (id)bottom_color.CGColor];
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
    NSMenuItem *device_item;
    NSMenuItem *scheme_items[COLOR_SCHEME_COUNT];
    NSMenuItem *test_menu_item;
    NSMenuItem *login_at_startup_item;
    NSWindow *light_settings_window;
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

    status_menu = [[NSMenu alloc] initWithTitle:L("app.menu_title", "FEKER Task Lights")];
    status_menu.delegate = self;
    toggle_item = [[NSMenuItem alloc]
        initWithTitle:L("task_lights.on", "Task lights on")
                action:@selector(toggleTaskLights:)
        keyEquivalent:@""];
    toggle_item.target = self;
    [status_menu addItem:toggle_item];

    device_item = [[NSMenuItem alloc]
        initWithTitle:L("device.detecting", "Detecting keyboard…")
                action:nil keyEquivalent:@""];
    device_item.enabled = NO;
    [status_menu addItem:device_item];
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
        @[L("test.working", "Working (breathing)"), @(SLOT_WORKING)],
        @[L("test.complete", "Complete (two breaths, then solid)"),
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
    ];
    for (NSString *instruction in instructions) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:instruction action:nil keyEquivalent:@""];
        item.enabled = NO;
        [help_menu addItem:item];
    }
    [help_menu addItem:[NSMenuItem separatorItem]];
    NSArray<NSString *> *color_help = @[
        L("help.working", "Working — breathing work color"),
        L("help.complete", "Complete — two breaths, then solid"),
        L("help.waiting", "Waiting — input or approval needed"),
        L("help.error", "Failed — the task could not finish"),
        L("help.idle", "Idle — no task is running"),
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
        initWithTitle:L("quit.title", "Quit FEKER Task Lights")
                action:@selector(quit:)
        keyEquivalent:@"q"];
    quit_item.target = self;
    [status_menu addItem:quit_item];

    status_item.menu = status_menu;
    (void)load_task_lights_enabled();
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
        initWithContentRect:NSMakeRect(0, 0, 328, 262)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    light_settings_window.title = L("window.title", "Task Lights");
    light_settings_window.releasedWhenClosed = NO;
    light_settings_window.delegate = self;
    [light_settings_window center];

    NSView *content = light_settings_window.contentView;
    InsetGroupView *status_group = [[InsetGroupView alloc]
        initWithFrame:NSMakeRect(8, 124, 312, 105)];
    [content addSubview:status_group];

    NSTextField *status_title =
        [NSTextField labelWithString:L("status.section", "Status")];
    status_title.frame = NSMakeRect(14, 234, 150, 16);
    status_title.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:status_title];

    NSArray<NSString *> *status_names =
        @[L("status.working", "Working"),
          L("status.complete", "Complete"),
          L("status.waiting", "Waiting"),
          L("status.error", "Failed"),
          L("status.idle", "Idle")];
    NSArray<NSString *> *effect_names =
        @[L("effect.breathing", "Breathing"),
          L("effect.two_breaths", "2 breaths → solid"),
          L("effect.slow_pulse", "Slow pulse"),
          L("effect.two_flashes", "2 flashes → solid"),
          L("effect.restore", "Restore RGB")];
    NSMutableArray<StatusPreviewView *> *cards = [NSMutableArray array];
    for (NSInteger index = 0; index < 5; index++) {
        NSInteger column = index % 3;
        NSInteger row = index / 3;
        CGFloat x = row == 0 ? 14 + column * 102
                             : 65 + column * 102;
        CGFloat y = 196 - row * 50;

        StatusPreviewView *card = [[StatusPreviewView alloc]
            initWithFrame:NSMakeRect(x, y, 95, 25)
                    title:status_names[index]];
        [content addSubview:card];
        [cards addObject:card];

        NSTextField *effect = [NSTextField labelWithString:effect_names[index]];
        effect.frame = NSMakeRect(x - 2, y - 17, 99, 13);
        effect.alignment = NSTextAlignmentCenter;
        effect.font = [NSFont systemFontOfSize:8.5];
        effect.textColor = NSColor.secondaryLabelColor;
        [content addSubview:effect];
    }
    status_cards = [cards copy];

    NSTextField *scheme_label =
        [NSTextField labelWithString:L("scheme.title", "Color scheme")];
    scheme_label.frame = NSMakeRect(14, 109, 140, 16);
    scheme_label.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:scheme_label];

    scheme_control = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(14, 78, 300, 24)];
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
    brightness_label.frame = NSMakeRect(14, 45, 110, 16);
    brightness_label.font =
        [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [content addSubview:brightness_label];

    brightness_value_label = [NSTextField labelWithString:@"68%"];
    brightness_value_label.frame = NSMakeRect(266, 45, 48, 16);
    brightness_value_label.alignment = NSTextAlignmentRight;
    brightness_value_label.font = [NSFont systemFontOfSize:11];
    brightness_value_label.textColor = NSColor.secondaryLabelColor;
    [content addSubview:brightness_value_label];

    brightness_slider = [[NSSlider alloc]
        initWithFrame:NSMakeRect(14, 14, 300, 18)];
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
        rgb_t base = status == SLOT_IDLE
                         ? (rgb_t){0x8E, 0x8E, 0x93}
                         : base_color_for_status(color_scheme, status);
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
    (void)load_color_scheme();
    (void)load_brightness();
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
    if (scheme_control != nil) {
        scheme_control.selectedSegment = color_scheme;
    }
    if (brightness_slider != nil) {
        brightness_slider.integerValue = light_brightness_percent;
        brightness_value_label.stringValue =
            [NSString stringWithFormat:@"%u%%", light_brightness_percent];
    }
    status_item.button.image = task_light_menu_icon(task_lights_enabled);
    status_item.button.toolTip = task_lights_enabled
                                     ? L("tooltip.on", "FEKER task lights on · Click for menu")
                                     : L("tooltip.off", "FEKER task lights paused · Click for menu");

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

    if (qmk_device_present()) {
        device_item.title =
            L("device.connected", "FEKER QMK/VIA · Whole-board lights");
    } else {
        device_item.title =
            L("device.not_found", "No compatible wired keyboard found");
    }
}

- (void)menuWillOpen:(NSMenu *)menu {
    (void)menu;
    log_line("UI", "Status menu opened.");
    (void)load_task_lights_enabled();
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
    log_line("UI", "Menu action: change whole-board color scheme.");
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
        stringByAppendingPathComponent:@"Library/Logs/FekerCodexBridge.log"];
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
        URLWithString:@"https://github.com/chenzixin1/feker-codex-bridge"];
    if (url != nil) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

- (void)quit:(id)sender {
    (void)sender;
    log_line("UI", "Menu action: request application quit.");
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = L("quit.message", "Quit FEKER Task Lights?");
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
            "  %s --scheme codex|ocean|violet\n"
            "  %s --brightness 20..100\n"
            "  %s --request-test [working|complete|idle|waiting|error|off]\n"
            "  %s --test [working|complete|idle|waiting|error|off]\n"
            "  %s --off\n",
            program, program, program, program, program, program, program,
            program);
}

static bool parse_status(const char *name, slot_status_t *status) {
    if (strcmp(name, "working") == 0 || strcmp(name, "blue") == 0) {
        *status = SLOT_WORKING;
    } else if (strcmp(name, "complete") == 0 || strcmp(name, "green") == 0) {
        *status = SLOT_COMPLETE;
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
        result = apply_status_color(SLOT_OFF) ? 0 : 1;
    } else {
        print_usage(argv[0]);
        result = 2;
    }

done:
    hid_exit();
    return result;
}
