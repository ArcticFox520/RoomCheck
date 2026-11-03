/*
 * week.c - week plans, groups, room assignments and week JSON view
 */
#include "common.h"

int week_get_active(int64_t account_id, week_t *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, account_id, week_start, week_end, iso_year, iso_week, created_at, "
        "updated_at, plan_type FROM week_plan WHERE account_id=?1 ORDER BY id DESC LIMIT 1";
    int found = 0;
    if (db_prepare(sql, &st) == 0) {
        db_bind_i64(st, 1, account_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->id = db_col_i64(st, 0);
            out->account_id = db_col_i64(st, 1);
            out->week_start = db_col_i64(st, 2);
            out->week_end = db_col_i64(st, 3);
            out->iso_year = (int)db_col_i64(st, 4);
            out->iso_week = (int)db_col_i64(st, 5);
            out->created_at = db_col_i64(st, 6);
            out->updated_at = db_col_i64(st, 7);
            snprintf(out->plan_type, sizeof(out->plan_type), "%s", db_col_text(st, 8));
            found = 1;
        }
        sqlite3_finalize(st);
    }
    return found;
}

void week_free(week_t *w)
{
    (void)w;
}

int64_t create_week_plan(int64_t account_id, int64_t week_start, const char *plan_type)
{
    int iso_year = 0, iso_week = 0;
    if (!rc_iso_week(week_start, &iso_year, &iso_week))
        return -1;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO week_plan(account_id, week_start, week_end, iso_year, iso_week, "
        "plan_type, created_at, updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?7)";
    if (db_prepare(sql, &st) != 0)
        return -1;
    int64_t now = rc_now();
    db_bind_i64(st, 1, account_id);
    db_bind_i64(st, 2, week_start);
    db_bind_i64(st, 3, week_start + 7 * 86400 - 1);
    db_bind_i64(st, 4, iso_year);
    db_bind_i64(st, 5, iso_week);
    db_bind_text(st, 6, plan_type ? plan_type : "initial");
    db_bind_i64(st, 7, now);
    int64_t id = -1;
    if (db_step_done(st) == 0)
        id = db_last_id();
    sqlite3_finalize(st);
    return id;
}

int ensure_groups(int64_t week_id)
{
    static const int days[6] = {0, 1, 2, 4, 5, 6};
    static const char *periods[2] = {"noon", "evening"};
    sqlite3_stmt *st = NULL;
    if (db_prepare("INSERT OR IGNORE INTO check_group(week_id, day_index, period, created_at) "
                   "VALUES(?1,?2,?3,?4)", &st) != 0)
        return -1;
    int64_t now = rc_now();
    for (int d = 0; d < 6; d++) {
        for (int p = 0; p < 2; p++) {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            db_bind_i64(st, 1, week_id);
            db_bind_i64(st, 2, days[d]);
            db_bind_text(st, 3, periods[p]);
            db_bind_i64(st, 4, now);
            if (db_step_done(st) != 0) {
                sqlite3_finalize(st);
                return -1;
            }
        }
    }
    sqlite3_finalize(st);
    return 0;
}

static int group_map(int64_t week_id, int64_t gid[7][2])
{
    for (int d = 0; d < 7; d++)
        for (int p = 0; p < 2; p++)
            gid[d][p] = 0;
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT id, day_index, period FROM check_group WHERE week_id=?1", &st) != 0)
        return -1;
    db_bind_i64(st, 1, week_id);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t id = db_col_i64(st, 0);
        int day = (int)db_col_i64(st, 1);
        const char *period = db_col_text(st, 2);
        int p = strcmp(period, "noon") == 0 ? 0 : 1;
        if (day >= 0 && day <= 6)
            gid[day][p] = id;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int write_assignments(int64_t week_id, int64_t account_id, const char *building_name,
                      const assign_t *assign, int count, int clear_checks)
{
    (void)building_name;
    if (ensure_groups(week_id) != 0)
        return -1;
    sqlite3_stmt *del = NULL;
    if (db_prepare("DELETE FROM room_check WHERE week_id=?1", &del) != 0)
        return -1;
    db_bind_i64(del, 1, week_id);
    if (db_step_done(del) != 0) {
        sqlite3_finalize(del);
        return -1;
    }
    sqlite3_finalize(del);

    int64_t gid[7][2];
    if (group_map(week_id, gid) != 0)
        return -1;
    int position[7][2] = {{0}};
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO room_check(week_id, group_id, account_id, room_key, room_number, "
        "position, checked, checked_at, created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)";
    if (db_prepare(sql, &st) != 0)
        return -1;
    int64_t now = rc_now();
    for (int i = 0; i < count; i++) {
        const assign_t *a = &assign[i];
        if (a->day < 0 || a->day > 6 || a->period < 0 || a->period > 1 ||
            !rc_day_is_check_day(a->day) || gid[a->day][a->period] == 0) {
            sqlite3_finalize(st);
            return -1;
        }
        int checked = clear_checks ? 0 : (a->checked ? 1 : 0);
        int64_t checked_at = checked ? (a->checked_at > 0 ? a->checked_at : now) : 0;
        position[a->day][a->period]++;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        db_bind_i64(st, 1, week_id);
        db_bind_i64(st, 2, gid[a->day][a->period]);
        db_bind_i64(st, 3, account_id);
        db_bind_text(st, 4, a->room_key);
        db_bind_text(st, 5, a->room_number);
        db_bind_i64(st, 6, position[a->day][a->period]);
        db_bind_i64(st, 7, checked);
        if (checked)
            db_bind_i64(st, 8, checked_at);
        else
            db_bind_null(st, 8);
        db_bind_i64(st, 9, now);
        if (db_step_done(st) != 0) {
            sqlite3_finalize(st);
            return -1;
        }
    }
    sqlite3_finalize(st);
    return 0;
}

static int load_old_assignments(int64_t week_id, assign_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT rc.room_key, rc.room_number, g.day_index, g.period, rc.checked, "
        "COALESCE(rc.checked_at,0) FROM room_check rc "
        "JOIN check_group g ON g.id=rc.group_id WHERE rc.week_id=?1 "
        "ORDER BY g.day_index, CASE WHEN g.period='noon' THEN 0 ELSE 1 END, "
        "rc.position, rc.id";
    if (db_prepare(sql, &st) != 0)
        return -1;
    db_bind_i64(st, 1, week_id);
    int cap = 16, n = 0;
    assign_t *arr = xcalloc((size_t)cap, sizeof(assign_t));
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = realloc(arr, (size_t)cap * sizeof(assign_t));
            if (!arr)
                abort();
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        snprintf(arr[n].room_key, sizeof(arr[n].room_key), "%s", db_col_text(st, 0));
        snprintf(arr[n].room_number, sizeof(arr[n].room_number), "%s", db_col_text(st, 1));
        arr[n].day = (int)db_col_i64(st, 2);
        const char *period = db_col_text(st, 3);
        arr[n].period = strcmp(period, "noon") == 0 ? 0 : 1;
        arr[n].checked = sqlite3_column_int(st, 4);
        arr[n].checked_at = db_col_i64(st, 5);
        n++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(arr);
        return -1;
    }
    *out = arr;
    *count = n;
    return 0;
}

int week_create_full(int64_t account_id, const room_config_t *cfg, const char *plan_type,
                     week_t *out, char *err, size_t errsz)
{
    char **numbers = NULL, **keys = NULL;
    int count = 0;
    if (build_room_list(cfg, &numbers, &keys, &count) != 0 || count <= 0) {
        snprintf(err, errsz, "无法生成有效寝室集合");
        return -1;
    }
    assign_t *assign = NULL;
    if (schedule_generate_initial(keys, numbers, count, &assign) != 0) {
        free_string_list(numbers, count);
        free_string_list(keys, count);
        snprintf(err, errsz, "排班生成失败");
        return -1;
    }
    char verr[256] = "";
    if (!schedule_validate(keys, count, assign, count, NULL, 0, verr, sizeof(verr))) {
        free(assign);
        free_string_list(numbers, count);
        free_string_list(keys, count);
        snprintf(err, errsz, "%s", verr);
        return -1;
    }
    int64_t week_start = rc_week_start(rc_now());
    int64_t week_id = create_week_plan(account_id, week_start, plan_type);
    if (week_id < 0 || ensure_groups(week_id) != 0 ||
        write_assignments(week_id, account_id, cfg->building_name, assign, count, 1) != 0) {
        free(assign);
        free_string_list(numbers, count);
        free_string_list(keys, count);
        snprintf(err, errsz, "保存排班失败");
        return -1;
    }
    int iso_year = 0, iso_week = 0;
    rc_iso_week(week_start, &iso_year, &iso_week);
    memset(out, 0, sizeof(*out));
    out->id = week_id;
    out->account_id = account_id;
    out->week_start = week_start;
    out->week_end = week_start + 7 * 86400 - 1;
    out->iso_year = iso_year;
    out->iso_week = iso_week;
    out->created_at = rc_now();
    out->updated_at = out->created_at;
    snprintf(out->plan_type, sizeof(out->plan_type), "%s", plan_type ? plan_type : "initial");
    free(assign);
    free_string_list(numbers, count);
    free_string_list(keys, count);
    return 0;
}

int week_rebuild(int64_t account_id, const char *plan_type, int incremental,
                 week_t *out, int *added, int *removed, int *retained,
                 char *err, size_t errsz)
{
    if (added) *added = 0;
    if (removed) *removed = 0;
    if (retained) *retained = 0;

    room_config_t cfg;
    if (!config_load(account_id, &cfg)) {
        snprintf(err, errsz, "尚未配置寝室范围");
        return -1;
    }
    char **numbers = NULL, **keys = NULL;
    int count = 0;
    if (build_room_list(&cfg, &numbers, &keys, &count) != 0 || count <= 0) {
        snprintf(err, errsz, "无法生成有效寝室集合");
        return -1;
    }

    week_t week;
    if (!week_get_active(account_id, &week)) {
        int rc = week_create_full(account_id, &cfg, "initial", out, err, errsz);
        if (rc == 0 && retained)
            *retained = count;
        free_string_list(numbers, count);
        free_string_list(keys, count);
        return rc;
    }

    assign_t *old = NULL;
    int old_count = 0;
    if (load_old_assignments(week.id, &old, &old_count) != 0) {
        free_string_list(numbers, count);
        free_string_list(keys, count);
        snprintf(err, errsz, "读取旧排班失败");
        return -1;
    }

    assign_t *assign = NULL;
    int added_n = 0, removed_n = 0, retained_n = 0;
    if (incremental && old_count > 0) {
        if (schedule_generate_incremental(old, old_count, keys, numbers, count, &assign,
                                          &retained_n, &added_n, &removed_n) != 0) {
            free(old);
            free_string_list(numbers, count);
            free_string_list(keys, count);
            snprintf(err, errsz, "增量排班失败");
            return -1;
        }
    } else {
        if (schedule_generate_initial(keys, numbers, count, &assign) != 0) {
            free(old);
            free_string_list(numbers, count);
            free_string_list(keys, count);
            snprintf(err, errsz, "排班生成失败");
            return -1;
        }
        retained_n = count;
    }

    char verr[256] = "";
    if (!schedule_validate(keys, count, assign, count,
                           (incremental && old_count > 0) ? old : NULL,
                           (incremental && old_count > 0) ? old_count : 0,
                           verr, sizeof(verr))) {
        free(assign);
        free(old);
        free_string_list(numbers, count);
        free_string_list(keys, count);
        snprintf(err, errsz, "%s", verr);
        return -1;
    }

    int rc = 0;
    if (write_assignments(week.id, account_id, cfg.building_name, assign, count,
                          incremental ? 0 : 1) != 0) {
        snprintf(err, errsz, "保存排班失败");
        rc = -1;
    } else {
        sqlite3_stmt *st = NULL;
        if (db_prepare("UPDATE week_plan SET plan_type=?1, updated_at=?2 WHERE id=?3", &st) == 0) {
            db_bind_text(st, 1, plan_type ? plan_type : (incremental ? "incremental" : "regenerate"));
            db_bind_i64(st, 2, rc_now());
            db_bind_i64(st, 3, week.id);
            if (db_step_done(st) != 0)
                rc = -1;
            sqlite3_finalize(st);
        }
        if (rc == 0) {
            week_get_active(account_id, out);
            if (added) *added = added_n;
            if (removed) *removed = removed_n;
            if (retained) *retained = retained_n;
            snprintf(out->plan_type, sizeof(out->plan_type), "%s",
                     plan_type ? plan_type : (incremental ? "incremental" : "regenerate"));
        }
    }
    free(assign);
    free(old);
    free_string_list(numbers, count);
    free_string_list(keys, count);
    return rc;
}

int week_progress(int64_t week_id, int *checked, int *total)
{
    *checked = 0;
    *total = 0;
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT COUNT(*), COALESCE(SUM(checked),0) FROM room_check WHERE week_id=?1",
                   &st) != 0)
        return -1;
    db_bind_i64(st, 1, week_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *total = (int)db_col_i64(st, 0);
        *checked = (int)db_col_i64(st, 1);
    }
    sqlite3_finalize(st);
    return 0;
}

typedef struct {
    int64_t id;
    int day;
    int period;
} group_head_t;

int week_build_json(request_t *r, int64_t account_id, jw_t *out, char *err, size_t errsz)
{
    (void)r;
    account_t acct;
    if (!auth_load_account(account_id, &acct)) {
        snprintf(err, errsz, "账户不存在");
        return -1;
    }
    week_t week;
    int has_week = week_get_active(account_id, &week);
    room_config_t cfg;
    int has_config = config_load(account_id, &cfg);

    jw_obj(out);
    jw_key(out, "account");
    account_json(out, &acct);

    jw_key(out, "config");
    if (has_config) {
        char **excluded = NULL;
        int excluded_count = 0;
        config_load_excluded(account_id, &excluded, &excluded_count);
        config_json(out, &cfg, excluded, excluded_count);
        free_string_list(excluded, excluded_count);
    } else {
        jw_null(out);
    }

    jw_key(out, "week");
    if (!has_week) {
        jw_null(out);
        jw_key(out, "progress");
        progress_json(out, 0, 0);
        jw_key(out, "days");
        jw_arr(out);
        jw_end(out);
        jw_key(out, "next_group_id");
        jw_null(out);
        jw_key(out, "next_room_id");
        jw_null(out);
        jw_end(out);
        return 0;
    }

    int checked = 0, total = 0;
    week_progress(week.id, &checked, &total);
    char datebuf[32];
    jw_obj(out);
    jw_key(out, "id");
    jw_i64(out, week.id);
    rc_fmt_date(week.week_start, datebuf, sizeof(datebuf));
    jw_key(out, "week_start");
    jw_str(out, datebuf);
    rc_fmt_date(week.week_end, datebuf, sizeof(datebuf));
    jw_key(out, "week_end");
    jw_str(out, datebuf);
    jw_key(out, "iso_year");
    jw_int(out, week.iso_year);
    jw_key(out, "iso_week");
    jw_int(out, week.iso_week);
    char label[64];
    snprintf(label, sizeof(label), "%d 年第 %d 周", week.iso_year, week.iso_week);
    jw_key(out, "label");
    jw_str(out, label);
    jw_key(out, "created_at");
    jw_i64(out, week.created_at);
    jw_key(out, "plan_type");
    jw_str(out, week.plan_type);
    jw_end(out);

    jw_key(out, "progress");
    progress_json(out, checked, total);

    group_head_t groups[12];
    int group_count = 0;
    sqlite3_stmt *gst = NULL;
    if (db_prepare("SELECT id, day_index, period FROM check_group WHERE week_id=?1 "
                   "ORDER BY day_index, CASE WHEN period='noon' THEN 0 ELSE 1 END",
                   &gst) == 0) {
        db_bind_i64(gst, 1, week.id);
        while (sqlite3_step(gst) == SQLITE_ROW && group_count < 12) {
            groups[group_count].id = db_col_i64(gst, 0);
            groups[group_count].day = (int)db_col_i64(gst, 1);
            groups[group_count].period = strcmp(db_col_text(gst, 2), "noon") == 0 ? 0 : 1;
            group_count++;
        }
        sqlite3_finalize(gst);
    }

    int64_t next_group = 0, next_room = 0;
    int day_is_today[7] = {0};
    int64_t now = rc_now();
    jw_key(out, "days");
    jw_arr(out);
    static const int days[6] = {0, 1, 2, 4, 5, 6};
    for (int di = 0; di < 6; di++) {
        int day = days[di];
        int64_t day_ts = week.week_start + (int64_t)day * 86400;
        day_is_today[day] = rc_same_day(day_ts, now);
        jw_obj(out);
        jw_key(out, "day_index");
        jw_int(out, day);
        rc_fmt_date(day_ts, datebuf, sizeof(datebuf));
        jw_key(out, "date");
        jw_str(out, datebuf);
        jw_key(out, "weekday");
        jw_str(out, rc_weekday_name(day));
        jw_key(out, "is_today");
        jw_bool(out, day_is_today[day]);
        jw_key(out, "groups");
        jw_arr(out);
        for (int p = 0; p < 2; p++) {
            group_head_t *g = NULL;
            for (int gi = 0; gi < group_count; gi++) {
                if (groups[gi].day == day && groups[gi].period == p) {
                    g = &groups[gi];
                    break;
                }
            }
            int gid = g ? (int)g->id : 0;
            int gtotal = 0, gchecked = 0;
            sqlite3_stmt *cst = NULL;
            if (gid > 0 &&
                db_prepare("SELECT COUNT(*), COALESCE(SUM(checked),0) FROM room_check "
                           "WHERE group_id=?1", &cst) == 0) {
                db_bind_i64(cst, 1, gid);
                if (sqlite3_step(cst) == SQLITE_ROW) {
                    gtotal = (int)db_col_i64(cst, 0);
                    gchecked = (int)db_col_i64(cst, 1);
                }
                sqlite3_finalize(cst);
            }
            jw_obj(out);
            jw_key(out, "id");
            jw_int(out, gid);
            jw_key(out, "period");
            jw_str(out, p == 0 ? "noon" : "evening");
            jw_key(out, "label");
            jw_str(out, p == 0 ? "中午" : "晚上");
            jw_key(out, "checked");
            jw_int(out, gchecked);
            jw_key(out, "total");
            jw_int(out, gtotal);
            jw_key(out, "rooms");
            jw_arr(out);
            if (gid > 0) {
                sqlite3_stmt *rst = NULL;
                const char *rsql =
                    "SELECT rc.id, rc.room_key, rc.room_number, rc.checked, "
                    "COALESCE(rc.checked_at,0), "
                    "(SELECT COUNT(*) FROM note n WHERE n.week_id=rc.week_id AND n.room_key=rc.room_key), "
                    "(SELECT COALESCE(SUM(length(n.content)),0) FROM note n WHERE n.week_id=rc.week_id AND n.room_key=rc.room_key), "
                    "(SELECT COUNT(*) FROM note_image i WHERE i.week_id=rc.week_id AND i.room_key=rc.room_key), "
                    "(SELECT n.content FROM note n WHERE n.week_id=rc.week_id AND n.room_key=rc.room_key ORDER BY n.id DESC LIMIT 1) "
                    "FROM room_check rc WHERE rc.group_id=?1 ORDER BY rc.position, rc.id";
                if (db_prepare(rsql, &rst) == 0) {
                    db_bind_i64(rst, 1, gid);
                    int room_index = 0;
                    while (sqlite3_step(rst) == SQLITE_ROW) {
                        int64_t room_id = db_col_i64(rst, 0);
                        const char *room_key = db_col_text(rst, 1);
                        const char *room_number = db_col_text(rst, 2);
                        int rchecked = sqlite3_column_int(rst, 3);
                        int64_t checked_at = db_col_i64(rst, 4);
                        int notes_count = (int)db_col_i64(rst, 5);
                        int notes_chars = (int)db_col_i64(rst, 6);
                        int images_count = (int)db_col_i64(rst, 7);
                        const char *preview = db_col_text(rst, 8);
                        jw_obj(out);
                        jw_key(out, "id");
                        jw_i64(out, room_id);
                        jw_key(out, "room_key");
                        jw_str(out, room_key);
                        jw_key(out, "room_number");
                        jw_str(out, room_number);
                        jw_key(out, "checked");
                        jw_bool(out, rchecked);
                        jw_key(out, "checked_at");
                        if (checked_at > 0)
                            jw_i64(out, checked_at);
                        else
                            jw_null(out);
                        jw_key(out, "notes_count");
                        jw_int(out, notes_count);
                        jw_key(out, "notes_chars");
                        jw_int(out, notes_chars);
                        jw_key(out, "images_count");
                        jw_int(out, images_count);
                        jw_key(out, "note_preview");
                        if (notes_count > 0 && preview[0])
                            jw_strn(out, preview, utf8_prefix_bytes(preview, 40));
                        else
                            jw_null(out);
                        jw_end(out);
                        if (next_group == 0 && !rchecked) {
                            next_group = gid;
                            next_room = room_id;
                        }
                        room_index++;
                    }
                    sqlite3_finalize(rst);
                }
            }
            jw_end(out);
            jw_end(out);
        }
        jw_end(out);
        jw_end(out);
    }
    jw_end(out);
    jw_key(out, "next_group_id");
    if (next_group > 0)
        jw_i64(out, next_group);
    else
        jw_null(out);
    jw_key(out, "next_room_id");
    if (next_room > 0)
        jw_i64(out, next_room);
    else
        jw_null(out);
    jw_end(out);
    return 0;
}
