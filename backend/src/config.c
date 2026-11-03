/*
 * config.c - room range, excluded rooms and N calculation
 */
#include "common.h"

#include <ctype.h>

int config_load(int64_t account_id, room_config_t *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT c.account_id, c.building_name, c.start_room, c.end_room, c.start_num, "
        "c.end_num, c.original_count, c.updated_at, "
        "(SELECT COUNT(*) FROM excluded_room e WHERE e.account_id=c.account_id) "
        "FROM floor_config c WHERE c.account_id=?1";
    int found = 0;
    if (db_prepare(sql, &st) == 0) {
        db_bind_i64(st, 1, account_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->account_id = db_col_i64(st, 0);
            snprintf(out->building_name, sizeof(out->building_name), "%s", db_col_text(st, 1));
            snprintf(out->start_room, sizeof(out->start_room), "%s", db_col_text(st, 2));
            snprintf(out->end_room, sizeof(out->end_room), "%s", db_col_text(st, 3));
            out->start_num = db_col_i64(st, 4);
            out->end_num = db_col_i64(st, 5);
            out->original_count = db_col_i64(st, 6);
            out->updated_at = db_col_i64(st, 7);
            out->excluded_count = db_col_i64(st, 8);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    return found;
}

int config_load_excluded(int64_t account_id, char ***out, int *count)
{
    *out = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT room_number FROM excluded_room WHERE account_id=?1 "
                   "ORDER BY CAST(room_number AS INTEGER), room_number", &st) != 0)
        return -1;
    db_bind_i64(st, 1, account_id);
    int cap = 8, n = 0;
    char **arr = xcalloc((size_t)cap, sizeof(char *));
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            arr = realloc(arr, (size_t)cap * sizeof(char *));
            if (!arr)
                abort();
        }
        arr[n++] = xstrdup(db_col_text(st, 0));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free_string_list(arr, n);
        return -1;
    }
    *out = arr;
    *count = n;
    return 0;
}

void free_string_list(char **list, int count)
{
    if (!list)
        return;
    for (int i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

static int room_width(const room_config_t *cfg)
{
    size_t ls = strlen(cfg->start_room), le = strlen(cfg->end_room);
    if (ls > 1 && cfg->start_room[0] == '0' && ls == le)
        return (int)ls;
    return 0;
}

static void format_room_number(int64_t num, int width, char *out, size_t outsz)
{
    if (width > 0)
        snprintf(out, outsz, "%0*lld", width, (long long)num);
    else
        snprintf(out, outsz, "%lld", (long long)num);
}

int room_number_canonical(const room_config_t *cfg, const char *input, char *out, size_t outsz)
{
    if (!input)
        return 0;
    char *tmp = trim_dup(input);
    int ok = 0;
    int64_t num;
    if (is_all_digits(tmp) && parse_i64(tmp, &num)) {
        if (num >= cfg->start_num && num <= cfg->end_num) {
            format_room_number(num, room_width(cfg), out, outsz);
            ok = 1;
        }
    }
    free(tmp);
    return ok;
}

int build_room_list(const room_config_t *cfg, char ***out_numbers, char ***out_keys, int *count)
{
    if (out_numbers)
        *out_numbers = NULL;
    if (out_keys)
        *out_keys = NULL;
    if (count)
        *count = 0;
    int64_t total = cfg->end_num - cfg->start_num + 1;
    if (total <= 0 || total > RC_MAX_ROOMS)
        return -1;

    char **excluded = NULL;
    int excluded_count = 0;
    if (config_load_excluded(cfg->account_id, &excluded, &excluded_count) != 0)
        return -1;

    int width = room_width(cfg);
    char **numbers = xcalloc((size_t)total, sizeof(char *));
    char **keys = xcalloc((size_t)total, sizeof(char *));
    int n = 0;
    for (int64_t num = cfg->start_num; num <= cfg->end_num; num++) {
        char numbuf[32];
        format_room_number(num, width, numbuf, sizeof(numbuf));
        int skip = 0;
        for (int i = 0; i < excluded_count; i++) {
            if (strcmp(excluded[i], numbuf) == 0) {
                skip = 1;
                break;
            }
        }
        if (skip)
            continue;
        size_t klen = strlen(cfg->building_name) + strlen(numbuf) + 2;
        char *key = xmalloc(klen);
        snprintf(key, klen, "%s#%s", cfg->building_name, numbuf);
        numbers[n] = xstrdup(numbuf);
        keys[n] = key;
        n++;
    }
    free_string_list(excluded, excluded_count);
    if (n == 0) {
        free(numbers);
        free(keys);
        return -1;
    }
    if (out_numbers)
        *out_numbers = numbers;
    else
        free_string_list(numbers, n);
    if (out_keys)
        *out_keys = keys;
    else
        free_string_list(keys, n);
    if (count)
        *count = n;
    return 0;
}

static int invalid(char *code, size_t code_sz, char *msg, size_t msg_sz,
                   const char *c, const char *m)
{
    snprintf(code, code_sz, "%s", c);
    snprintf(msg, msg_sz, "%s", m);
    return 0;
}

int config_validate_rooms(const char *building, const char *start, const char *end,
                          char ***excluded, int excluded_count, room_config_t *out,
                          char *err_code, size_t code_sz, char *err_msg, size_t msg_sz)
{
    memset(out, 0, sizeof(*out));
    char *b = trim_dup(building ? building : "");
    char *s = trim_dup(start ? start : "");
    char *e = trim_dup(end ? end : "");

    if (strlen(b) == 0 || utf8_count(b) > 40 || strlen(b) > 120) {
        free(b); free(s); free(e);
        return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                       "楼栋名称长度须为 1-40 个字符");
    }
    if (strchr(b, '#') || strchr(b, '/') || strchr(b, '\\')) {
        free(b); free(s); free(e);
        return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                       "楼栋名称不能包含 # / \\ 字符");
    }
    for (const unsigned char *p = (const unsigned char *)b; *p; p++) {
        if (*p < 0x20) {
            free(b); free(s); free(e);
            return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                           "楼栋名称包含非法字符");
        }
    }
    if (!is_all_digits(s) || !is_all_digits(e) || strlen(s) > 9 || strlen(e) > 9) {
        free(b); free(s); free(e);
        return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                       "起始和结束寝室号必须为数字");
    }
    int64_t sn, en;
    parse_i64(s, &sn);
    parse_i64(e, &en);
    if (sn > en) {
        free(b); free(s); free(e);
        return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                       "起始寝室号不能大于结束寝室号");
    }
    int64_t total = en - sn + 1;
    if (total > RC_MAX_ROOMS) {
        free(b); free(s); free(e);
        return invalid(err_code, code_sz, err_msg, msg_sz, "invalid_range",
                       "寝室范围过大，单次最多 20000 间");
    }

    snprintf(out->building_name, sizeof(out->building_name), "%s", b);
    snprintf(out->start_room, sizeof(out->start_room), "%s", s);
    snprintf(out->end_room, sizeof(out->end_room), "%s", e);
    out->start_num = sn;
    out->end_num = en;
    out->original_count = total;

    char **canon = xcalloc((size_t)(excluded_count > 0 ? excluded_count : 1), sizeof(char *));
    int canon_count = 0;
    for (int i = 0; i < excluded_count; i++) {
        char buf[32];
        if (!room_number_canonical(out, (*excluded)[i], buf, sizeof(buf))) {
            free(b); free(s); free(e);
            free_string_list(canon, canon_count);
            return invalid(err_code, code_sz, err_msg, msg_sz, "room_out_of_range",
                           "无需查寝寝室不在当前范围内");
        }
        int dup = 0;
        for (int j = 0; j < canon_count; j++) {
            if (strcmp(canon[j], buf) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            free(b); free(s); free(e);
            free_string_list(canon, canon_count);
            return invalid(err_code, code_sz, err_msg, msg_sz, "duplicate_excluded",
                           "无需查寝寝室不允许重复");
        }
        canon[canon_count++] = xstrdup(buf);
    }
    out->excluded_count = canon_count;
    if (total - canon_count <= 0) {
        free(b); free(s); free(e);
        free_string_list(canon, canon_count);
        return invalid(err_code, code_sz, err_msg, msg_sz, "N_zero",
                       "实际应查数 N 必须大于 0");
    }
    free(b);
    free(s);
    free(e);
    if (excluded) {
        free_string_list(*excluded, excluded_count);
        *excluded = canon;
    } else {
        free_string_list(canon, canon_count);
    }
    return 1;
}

int config_save(int64_t account_id, const room_config_t *cfg, char **excluded, int excluded_count)
{
    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO floor_config(account_id, building_name, start_room, end_room, "
        "start_num, end_num, original_count, created_at, updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?8) "
        "ON CONFLICT(account_id) DO UPDATE SET building_name=excluded.building_name, "
        "start_room=excluded.start_room, end_room=excluded.end_room, "
        "start_num=excluded.start_num, end_num=excluded.end_num, "
        "original_count=excluded.original_count, updated_at=excluded.updated_at";
    if (db_prepare(sql, &st) != 0)
        return -1;
    db_bind_i64(st, 1, account_id);
    db_bind_text(st, 2, cfg->building_name);
    db_bind_text(st, 3, cfg->start_room);
    db_bind_text(st, 4, cfg->end_room);
    db_bind_i64(st, 5, cfg->start_num);
    db_bind_i64(st, 6, cfg->end_num);
    db_bind_i64(st, 7, cfg->original_count);
    db_bind_i64(st, 8, now);
    if (db_step_done(st) != 0) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);

    if (db_prepare("DELETE FROM excluded_room WHERE account_id=?1", &st) != 0)
        return -1;
    db_bind_i64(st, 1, account_id);
    if (db_step_done(st) != 0) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);

    if (excluded_count > 0) {
        if (db_prepare("INSERT INTO excluded_room(account_id, room_number, created_at) "
                       "VALUES(?1,?2,?3)", &st) != 0)
            return -1;
        for (int i = 0; i < excluded_count; i++) {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            db_bind_i64(st, 1, account_id);
            db_bind_text(st, 2, excluded[i]);
            db_bind_i64(st, 3, now);
            if (db_step_done(st) != 0) {
                sqlite3_finalize(st);
                return -1;
            }
        }
        sqlite3_finalize(st);
    }
    return 0;
}
