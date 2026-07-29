#define _DARWIN_C_SOURCE

#include <ApplicationServices/ApplicationServices.h>
#include <errno.h>
#include <fcntl.h>
#include <hidapi.h>
#include <limits.h>
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
#include <time.h>
#include <unistd.h>

#define FEKER_VID 0x320F
#define FEKER_PID 0x5055
#define FEKER_USAGE_PAGE 0xFF1C
#define FEKER_USAGE 0x0092
#define REPORT_SIZE 64
#define LED_COUNT 128
#define COLOR_BYTES (LED_COUNT * 3)
#define MAX_COLOR_PAYLOAD 0x36
#define MAX_THREADS 128
#define MAX_TITLE 256

static const int number_key_leds[9] = {22, 23, 24, 25, 26, 27, 28, 29, 30};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef enum {
    SLOT_OFF = 0,
    SLOT_RUNNING,
    SLOT_COMPLETE,
    SLOT_WAITING,
    SLOT_ERROR,
} slot_status_t;

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
static char app_support_dir[PATH_MAX];
static char slot_state_path[PATH_MAX];
static char acknowledgement_path[PATH_MAX];
static char database_path[PATH_MAX];

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
    snprintf(library_dir, sizeof(library_dir), "%s/Library", home);
    snprintf(application_support_dir, sizeof(application_support_dir),
             "%s/Application Support", library_dir);
    snprintf(app_support_dir, sizeof(app_support_dir),
             "%s/Feker Codex Bridge", application_support_dir);
    snprintf(slot_state_path, sizeof(slot_state_path), "%s/slots.tsv", app_support_dir);
    snprintf(acknowledgement_path, sizeof(acknowledgement_path),
             "%s/ack.tsv", app_support_dir);
    snprintf(database_path, sizeof(database_path), "%s/.codex/state_5.sqlite", home);

    return ensure_directory(library_dir) &&
           ensure_directory(application_support_dir) &&
           ensure_directory(app_support_dir);
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
    struct hid_device_info *devices = hid_enumerate(FEKER_VID, FEKER_PID);
    const char *chosen_path = NULL;
    char path_copy[PATH_MAX];

    for (struct hid_device_info *item = devices; item != NULL; item = item->next) {
        if (item->usage_page == FEKER_USAGE_PAGE && item->usage == FEKER_USAGE &&
            item->path != NULL) {
            snprintf(path_copy, sizeof(path_copy), "%s", item->path);
            chosen_path = path_copy;
            break;
        }
    }

    if (chosen_path == NULL) {
        hid_free_enumeration(devices);
        if (!hid_permission_warning_printed) {
            log_line("ERROR", "FEKER Alice80 RGB interface was not found.");
            hid_permission_warning_printed = true;
        }
        return NULL;
    }

    hid_device *device = hid_open_path(chosen_path);
    hid_free_enumeration(devices);
    if (device == NULL && !hid_permission_warning_printed) {
        log_line("PERMISSION",
                 "macOS blocked FEKER RGB access. Enable Input Monitoring for "
                 "Feker Codex Bridge in System Settings > Privacy & Security.");
        hid_permission_warning_printed = true;
    }
    if (device != NULL) {
        hid_permission_warning_printed = false;
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
    return hid_read_timeout(device, response, sizeof(response), 300) > 0;
}

static bool apply_slot_colors(const slot_status_t statuses[9]) {
    static const rgb_t colors_by_status[] = {
        [SLOT_OFF] = {0x00, 0x00, 0x00},
        [SLOT_RUNNING] = {0x00, 0x6E, 0xFF},
        [SLOT_COMPLETE] = {0x19, 0xD1, 0x7F},
        [SLOT_WAITING] = {0xFF, 0xAD, 0x33},
        [SLOT_ERROR] = {0xFF, 0x4D, 0x4D},
    };
    rgb_t led_colors[LED_COUNT];
    memset(led_colors, 0, sizeof(led_colors));
    for (int index = 0; index < 9; index++) {
        led_colors[number_key_leds[index]] = colors_by_status[statuses[index]];
    }

    hid_device *device = open_rgb_device();
    if (device == NULL) {
        return false;
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
        ok = send_packet(device, packet);
    }

    hid_close(device);
    if (!ok) {
        log_line("ERROR", "FEKER did not acknowledge the RGB update.");
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
    slot_status_t statuses[9];
    collect_statuses(statuses);
    apply_slot_colors(statuses);
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

static int choose_slot(void) {
    bool used[9] = {false};
    for (size_t index = 0; index < watched_count; index++) {
        if (watched[index].slot >= 1 && watched[index].slot <= 9) {
            used[watched[index].slot - 1] = true;
        }
    }
    for (int index = 0; index < 9; index++) {
        if (!used[index]) {
            return index + 1;
        }
    }

    watched_thread_t *oldest = NULL;
    for (size_t index = 0; index < watched_count; index++) {
        watched_thread_t *candidate = &watched[index];
        if (candidate->status == SLOT_RUNNING || candidate->status == SLOT_WAITING) {
            continue;
        }
        if (oldest == NULL || candidate->touched_at < oldest->touched_at) {
            oldest = candidate;
        }
    }
    if (oldest == NULL) {
        return 0;
    }
    int slot = oldest->slot;
    oldest->slot = 0;
    oldest->status = SLOT_OFF;
    return slot;
}

static void update_thread_status(watched_thread_t *item, slot_status_t status) {
    if (item->slot == 0) {
        item->slot = choose_slot();
    }
    if (item->slot == 0) {
        log_line("WARN", "All nine task-light slots are currently occupied.");
        return;
    }
    item->status = status;
    item->touched_at = time(NULL);

    char message[512];
    const char *status_name = status == SLOT_RUNNING ? "running" :
                              status == SLOT_COMPLETE ? "complete" :
                              status == SLOT_WAITING ? "waiting" :
                              status == SLOT_ERROR ? "error" : "off";
    snprintf(message, sizeof(message), "Slot %d -> %s: %s", item->slot,
             status_name, item->title[0] != '\0' ? item->title : item->thread_id);
    log_line("TASK", message);
    save_state();
    refresh_lights();
}

static slot_status_t status_from_line(const char *line, bool *matched) {
    *matched = true;
    if (strstr(line,
               "\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\"") != NULL) {
        return SLOT_RUNNING;
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
        (last_status == SLOT_RUNNING || last_status == SLOT_WAITING)) {
        update_thread_status(item, last_status);
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
        "WHERE archived = 0 AND rollout_path <> '' "
        "ORDER BY updated_at_ms DESC LIMIT 128";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *thread_id = (const char *)sqlite3_column_text(statement, 0);
        const char *path = (const char *)sqlite3_column_text(statement, 1);
        const char *title = (const char *)sqlite3_column_text(statement, 2);
        if (thread_id == NULL || path == NULL || find_thread(thread_id) != NULL ||
            watched_count >= MAX_THREADS) {
            continue;
        }
        watched_thread_t *item = &watched[watched_count++];
        memset(item, 0, sizeof(*item));
        snprintf(item->thread_id, sizeof(item->thread_id), "%s", thread_id);
        snprintf(item->rollout_path, sizeof(item->rollout_path), "%s", path);
        snprintf(item->title, sizeof(item->title), "%s", title != NULL ? title : "");
        sanitize_title(item->title);
        initialize_rollout_position(item);
    }
    sqlite3_finalize(statement);
}

static void consume_acknowledgement(void) {
    FILE *file = fopen(acknowledgement_path, "r");
    if (file == NULL) {
        return;
    }
    int slot = 0;
    char thread_id[64];
    if (fscanf(file, "%d\t%63s", &slot, thread_id) == 2) {
        for (size_t index = 0; index < watched_count; index++) {
            watched_thread_t *item = &watched[index];
            if (item->slot == slot && strcmp(item->thread_id, thread_id) == 0) {
                item->slot = 0;
                item->status = SLOT_OFF;
                item->touched_at = time(NULL);
                save_state();
                refresh_lights();
                break;
            }
        }
    }
    fclose(file);
    unlink(acknowledgement_path);
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
    log_line("READY", "Watching Codex tasks. Option+1...9 opens the matching lit task.");
    save_state();
    refresh_lights();

    unsigned int ticks = 0;
    while (!should_stop) {
        if (ticks % 4 == 0) {
            discover_threads(database);
        }
        for (size_t index = 0; index < watched_count; index++) {
            process_appended_events(&watched[index]);
        }
        consume_acknowledgement();
        usleep(500000);
        ticks++;
    }

    slot_status_t off[9] = {SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF,
                            SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF};
    apply_slot_colors(off);
    sqlite3_close(database);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    log_line("STOP", "Feker Codex Bridge stopped.");
    return 0;
}

static int open_slot(int slot) {
    FILE *file = fopen(slot_state_path, "r");
    if (file == NULL) {
        return 1;
    }
    int stored_slot = 0;
    int status = 0;
    long long touched = 0;
    char thread_id[64];
    char title[MAX_TITLE];
    bool found = false;
    while (fscanf(file, "%d\t%d\t%lld\t%63s\t%255[^\n]\n", &stored_slot,
                  &status, &touched, thread_id, title) >= 4) {
        if (stored_slot == slot) {
            found = true;
            break;
        }
    }
    fclose(file);
    if (!found) {
        return 1;
    }

    FILE *acknowledgement = fopen(acknowledgement_path, "w");
    if (acknowledgement != NULL) {
        fprintf(acknowledgement, "%d\t%s\n", slot, thread_id);
        fclose(acknowledgement);
    }

    char url[160];
    snprintf(url, sizeof(url), "codex://threads/%s", thread_id);
    pid_t child = fork();
    if (child == 0) {
        execl("/usr/bin/open", "open", url, (char *)NULL);
        _exit(127);
    }

    slot_status_t statuses[9] = {SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF,
                                 SLOT_OFF, SLOT_OFF, SLOT_OFF, SLOT_OFF};
    file = fopen(slot_state_path, "r");
    if (file != NULL) {
        while (fscanf(file, "%d\t%d\t%lld\t%63s\t%255[^\n]\n", &stored_slot,
                      &status, &touched, thread_id, title) >= 4) {
            if (stored_slot >= 1 && stored_slot <= 9 && stored_slot != slot &&
                status >= SLOT_OFF && status <= SLOT_ERROR) {
                statuses[stored_slot - 1] = (slot_status_t)status;
            }
        }
        fclose(file);
    }
    apply_slot_colors(statuses);
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

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --daemon\n"
            "  %s --open-slot 1-9\n"
            "  %s --test-key 1-9 [blue|green|amber|red|off]\n"
            "  %s --off\n",
            program, program, program, program);
}

int main(int argc, char **argv) {
    if (!initialize_paths()) {
        return 1;
    }
    bool input_monitoring_allowed = CGPreflightListenEventAccess();
    if (!input_monitoring_allowed) {
        input_monitoring_allowed = CGRequestListenEventAccess();
    }
    log_line("PERMISSION",
             input_monitoring_allowed
                 ? "Input Monitoring permission is active."
                 : "Input Monitoring permission is not active yet.");
    if (hid_init() != 0) {
        fputs("Unable to initialize HIDAPI.\n", stderr);
        return 1;
    }

    int result = 0;
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--daemon") == 0)) {
        result = run_daemon();
    } else if (argc == 3 && strcmp(argv[1], "--open-slot") == 0) {
        result = open_slot(atoi(argv[2]));
    } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "--test-key") == 0) {
        slot_status_t status = SLOT_COMPLETE;
        if (argc == 4) {
            if (strcmp(argv[3], "blue") == 0) status = SLOT_RUNNING;
            else if (strcmp(argv[3], "green") == 0) status = SLOT_COMPLETE;
            else if (strcmp(argv[3], "amber") == 0) status = SLOT_WAITING;
            else if (strcmp(argv[3], "red") == 0) status = SLOT_ERROR;
            else if (strcmp(argv[3], "off") == 0) status = SLOT_OFF;
            else {
                print_usage(argv[0]);
                result = 2;
                goto done;
            }
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
