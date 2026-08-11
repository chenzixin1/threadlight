#define main threadlight_application_main
#include "../feker_codex_bridge.c"
#undef main

static void fail(const char *message) {
    fprintf(stderr, "thread mapping regression failed: %s\n", message);
    exit(1);
}

static void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

static void execute_sql(sqlite3 *database, const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(database, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "SQLite setup failed: %s\n",
                error != NULL ? error : "unknown error");
        sqlite3_free(error);
        exit(1);
    }
}

static void add_thread(sqlite3 *database, const char *thread_id,
                       const char *rollout_path, const char *title,
                       sqlite3_int64 recency, const char *thread_source) {
    static const char *insert_sql =
        "INSERT INTO threads "
        "(id, rollout_path, title, archived, preview, recency_at_ms, "
        " thread_source) VALUES (?, ?, ?, 0, 'visible', ?, ?)";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, insert_sql, -1,
                           &statement, NULL) != SQLITE_OK) {
        fail("unable to prepare fixture insert");
    }
    sqlite3_bind_text(statement, 1, thread_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, rollout_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, recency);
    if (thread_source == NULL) {
        sqlite3_bind_null(statement, 5);
    } else {
        sqlite3_bind_text(statement, 5, thread_source, -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(statement) != SQLITE_DONE) {
        sqlite3_finalize(statement);
        fail("unable to insert fixture thread");
    }
    sqlite3_finalize(statement);
}

static void write_event(const char *path, const char *event_type,
                        bool append) {
    FILE *file = fopen(path, append ? "a" : "w");
    if (file == NULL) {
        fail("unable to create rollout fixture");
    }
    fprintf(file,
            "{\"type\":\"event_msg\",\"payload\":{\"type\":\"%s\"}}\n",
            event_type);
    if (fclose(file) != 0) {
        fail("unable to close rollout fixture");
    }
}

static void fixture_path(char output[PATH_MAX], const char *directory,
                         const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s.jsonl", directory, name);
    if (length < 0 || length >= PATH_MAX) {
        fail("fixture path is too long");
    }
}

static void write_global_state(const char *path, const char *first,
                               const char *second, const char *third,
                               const char *unread_thread_id) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fail("unable to create global-state fixture");
    }
    fprintf(file,
            "{\"pinned-thread-ids\":[\"stale-pinned\","
            "\"%s\",\"%s\",\"%s\"],"
            "\"electron-persisted-atom-state\":{"
            "\"unread-thread-ids-by-host-v1\":{\"local\":[",
            first, second, third);
    if (unread_thread_id != NULL) {
        fprintf(file, "\"%s\"", unread_thread_id);
    }
    fputs("]}}}\n", file);
    if (fclose(file) != 0) {
        fail("unable to close global-state fixture");
    }
}

int main(void) {
    char fixture_directory[] = "/tmp/threadlight-mapping.XXXXXX";
    if (mkdtemp(fixture_directory) == NULL) {
        fail("unable to create fixture directory");
    }

    char first_path[PATH_MAX];
    char subagent_path[PATH_MAX];
    char main_path[PATH_MAX];
    char null_source_path[PATH_MAX];
    char empty_source_path[PATH_MAX];
    char stale_pinned_path[PATH_MAX];
    char filler_paths[6][PATH_MAX];
    char pinned_state_path[PATH_MAX];
    char missing_state_path[PATH_MAX];
    fixture_path(first_path, fixture_directory, "first-visible");
    fixture_path(subagent_path, fixture_directory, "completed-subagent");
    fixture_path(main_path, fixture_directory, "running-main");
    fixture_path(null_source_path, fixture_directory, "legacy-null-source");
    fixture_path(empty_source_path, fixture_directory, "legacy-empty-source");
    fixture_path(stale_pinned_path, fixture_directory, "stale-pinned");
    snprintf(pinned_state_path, sizeof(pinned_state_path), "%s/global-state.json",
             fixture_directory);
    snprintf(missing_state_path, sizeof(missing_state_path), "%s/missing.json",
             fixture_directory);

    write_event(first_path, "task_started", false);
    write_event(subagent_path, "task_started", false);
    write_event(subagent_path, "task_complete", true);
    write_event(main_path, "task_started", false);
    write_event(null_source_path, "task_started", false);
    write_event(empty_source_path, "task_started", false);
    write_event(stale_pinned_path, "task_started", false);
    for (size_t index = 0; index < 6; index++) {
        char name[32];
        snprintf(name, sizeof(name), "filler-%zu", index);
        fixture_path(filler_paths[index], fixture_directory, name);
        write_event(filler_paths[index], "task_started", false);
    }
    write_global_state(pinned_state_path, "first-visible",
                       "legacy-null-source", "running-main", "running-main");

    sqlite3 *database = NULL;
    if (sqlite3_open(":memory:", &database) != SQLITE_OK) {
        fail("unable to open in-memory database");
    }
    execute_sql(database,
        "CREATE TABLE threads ("
        "id TEXT PRIMARY KEY, rollout_path TEXT, title TEXT, "
        "archived INTEGER, preview TEXT, recency_at_ms INTEGER, "
        "thread_source TEXT)");

    add_thread(database, "first-visible", first_path, "First visible task",
               500, "user");
    add_thread(database, "completed-subagent", subagent_path,
               "Completed background subagent", 400, "subagent");
    add_thread(database, "running-main", main_path,
               "Deploy the new style to Cloudflare", 300, "user");
    add_thread(database, "legacy-null-source", null_source_path,
               "Legacy null-source task", 200, NULL);
    add_thread(database, "legacy-empty-source", empty_source_path,
               "Legacy empty-source task", 100, "");
    for (size_t index = 0; index < 6; index++) {
        char thread_id[32];
        snprintf(thread_id, sizeof(thread_id), "filler-%zu", index);
        add_thread(database, thread_id, filler_paths[index], "Filler task",
                   90 - (sqlite3_int64)index, "user");
    }
    add_thread(database, "stale-pinned", stale_pinned_path,
               "Stale pinned task", 1, "user");

    memset(watched, 0, sizeof(watched));
    watched_count = 0;
    task_lights_enabled = false;
    lighting_scope = LIGHTING_SCOPE_NUMBER_KEYS;
    snprintf(global_state_path, sizeof(global_state_path), "%s",
             pinned_state_path);
    discover_threads(database);

    expect(watched_count == RGB9_INDICATOR_COUNT,
           "subagent must not enter the visible watched-thread list");
    expect(find_thread("completed-subagent") == NULL,
           "completed subagent must be excluded even without agent_path");
    expect(find_thread("stale-pinned") == NULL,
           "stale pin outside the recent nine must not consume a key slot");

    watched_thread_t *first = find_thread("first-visible");
    watched_thread_t *main_task = find_thread("running-main");
    watched_thread_t *legacy_null = find_thread("legacy-null-source");
    watched_thread_t *legacy_empty = find_thread("legacy-empty-source");
    expect(first != NULL && first->slot == 1,
           "first eligible pinned task must occupy key 1");
    expect(legacy_null != NULL && legacy_null->slot == 2,
           "pinned order must override recency for key 2");
    expect(main_task != NULL && main_task->slot == 3,
           "running main task must follow the sidebar order on key 3");
    expect(legacy_empty != NULL && legacy_empty->slot == 4,
           "unpinned legacy task must follow pinned tasks");

    slot_status_t statuses[RGB9_INDICATOR_COUNT];
    collect_number_key_statuses(statuses);
    expect(statuses[2] == SLOT_WORKING,
           "key 3 must stay working despite stale unread state and a completed subagent");
    rgb_t working = base_color_for_status(COLOR_SCHEME_CODEX, statuses[2]);
    expect(working.r == 0x00 && working.g == 0xFF && working.b == 0x4C,
           "working main task must map to the Codex green color");

    write_global_state(pinned_state_path, "running-main", "first-visible",
                       "legacy-null-source", NULL);
    discover_threads(database);
    expect(main_task->slot == 1 && first->slot == 2 && legacy_null->slot == 3,
           "number-key slots must follow sidebar order changes");
    collect_number_key_statuses(statuses);
    expect(statuses[0] == SLOT_WORKING,
           "working status must move with the reordered task");

    snprintf(global_state_path, sizeof(global_state_path), "%s",
             missing_state_path);
    discover_threads(database);
    expect(first->slot == 1 && main_task->slot == 2,
           "missing global state must fall back to recency order");

    write_global_state(pinned_state_path, "first-visible",
                       "legacy-null-source", "running-main", NULL);
    snprintf(global_state_path, sizeof(global_state_path), "%s",
             pinned_state_path);
    discover_threads(database);
    expect(main_task->slot == 3,
           "restored sidebar order must move the running task back to key 3");

    write_event(main_path, "task_complete", true);
    process_appended_events(main_task);
    collect_number_key_statuses(statuses);
    expect(statuses[2] == SLOT_OFF,
           "a viewed or not-yet-unread completion must stay dark");

    write_global_state(pinned_state_path, "first-visible",
                       "legacy-null-source", "running-main", "running-main");
    discover_threads(database);
    collect_number_key_statuses(statuses);
    expect(statuses[2] == SLOT_COMPLETE,
           "key 3 must show a completed task while its result is unread");
    rgb_t complete = base_color_for_status(COLOR_SCHEME_CODEX, statuses[2]);
    expect(complete.r == 0x30 && complete.g == 0x4F && complete.b == 0xFE,
           "unread completed task must map to the Codex blue color");

    write_global_state(pinned_state_path, "first-visible",
                       "legacy-null-source", "running-main", NULL);
    discover_threads(database);
    collect_number_key_statuses(statuses);
    expect(statuses[2] == SLOT_OFF,
           "viewing the completed task must turn key 3 off");
    rgb_t idle = base_color_for_status(COLOR_SCHEME_CODEX, SLOT_IDLE);
    expect(idle.r == 0x00 && idle.g == 0x00 && idle.b == 0x00,
           "idle must map to black instead of white");
    slot_status_t parsed_status = SLOT_OFF;
    expect(parse_status("green", &parsed_status) &&
               parsed_status == SLOT_WORKING,
           "the green CLI alias must select working");
    expect(parse_status("blue", &parsed_status) &&
               parsed_status == SLOT_COMPLETE,
           "the blue CLI alias must select completed and unread");

    slot_status_t frame_statuses[RGB9_INDICATOR_COUNT];
    rgb_t frame_colors[RGB9_INDICATOR_COUNT];
    for (size_t index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        frame_statuses[index] = SLOT_OFF;
        frame_colors[index] = (rgb_t){0xFF, 0xFF, 0xFF};
    }
    uint16_t frame_mask =
        compose_number_key_frame(frame_statuses, 0.0, frame_colors);
    expect(frame_mask == RGB9_ALL_KEYS_MASK,
           "every reconciliation frame must address all nine keys");
    for (size_t index = 0; index < RGB9_INDICATOR_COUNT; index++) {
        expect(frame_colors[index].r == 0x00 &&
                   frame_colors[index].g == 0x00 &&
                   frame_colors[index].b == 0x00,
               "off keys must be explicitly written as black");
    }
    frame_statuses[4] = SLOT_WORKING;
    frame_mask = compose_number_key_frame(frame_statuses, 0.0, frame_colors);
    expect(frame_mask == RGB9_ALL_KEYS_MASK &&
               frame_colors[4].g > 0x00,
           "an active key must coexist with explicit black idle keys");
    expect(frame_colors[6].r == 0x00 && frame_colors[6].g == 0x00 &&
               frame_colors[6].b == 0x00,
           "a previously lit idle key must be cleared in the same frame");

    sqlite3_close(database);
    unlink(first_path);
    unlink(subagent_path);
    unlink(main_path);
    unlink(null_source_path);
    unlink(empty_source_path);
    unlink(stale_pinned_path);
    for (size_t index = 0; index < 6; index++) {
        unlink(filler_paths[index]);
    }
    unlink(pinned_state_path);
    rmdir(fixture_directory);

    puts("thread mapping regression passed: ordering and subagent filtering "
         "preserved; status colors and full-frame black clearing verified");
    return 0;
}
