/*
 * api_admin.c - administrator account management, overview, logs, backups,
 *                restore and orphaned note/image cleanup endpoints
 */
#include "common.h"

#include <ctype.h>

static void week_label(int64_t week_start, int64_t week_end, char *out, size_t outsz)
{
    rc_fmt_week_range(week_start, week_end, out, outsz);
}

static void base_account_fields(jw_t *w, const account_t *a)
{
    jw_key(w, "id");
    jw_i64(w, a->id);
    jw_key(w, "role");
    jw_str(w, a->role);
    jw_key(w, "username");
    jw_str(w, a->username);
    jw_key(w, "display_name");
    jw_str(w, a->display_name);
    jw_key(w, "status");
    jw_str(w, a->status);
    jw_key(w, "created_at");
    jw_i64(w, a->created_at);
    jw_key(w, "last_login_at");
    if (a->last_login_at > 0)
        jw_i64(w, a->last_login_at);
    else
        jw_null(w);
    jw_key(w, "deleted_at");
    if (a->deleted_at > 0)
        jw_i64(w, a->deleted_at);
    else
        jw_null(w);
    jw_key(w, "delete_at");
    if (a->delete_at > 0)
        jw_i64(w, a->delete_at);
    else
        jw_null(w);
    int64_t remaining = 0;
    if (strcmp(a->status, "deleted") == 0 && a->delete_at > 0) {
        remaining = a->delete_at - rc_now();
        if (remaining < 0)
            remaining = 0;
        jw_key(w, "remaining_seconds");
        jw_i64(w, remaining);
        jw_key(w, "remaining_text");
        char *text = duration_text(remaining);
        jw_str(w, text);
        free(text);
    } else {
        jw_key(w, "remaining_seconds");
        jw_null(w);
        jw_key(w, "remaining_text");
        jw_null(w);
    }
}

static void brief_account_json(jw_t *w, const account_t *a)
{
    jw_obj(w);
    base_account_fields(w, a);
    jw_end(w);
}

static int progress_for_day(int64_t week_id, int64_t day_start, int *checked, int *total)
{
    *checked = 0;
    *total = 0;
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT COUNT(*), COALESCE(SUM(rc.checked),0) FROM room_check rc "
                   "JOIN check_group g ON g.id=rc.group_id "
                   "WHERE rc.week_id=?1 AND g.day_index=?2", &st) != 0)
        return -1;
    db_bind_i64(st, 1, week_id);
    db_bind_i64(st, 2, day_start);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *total = (int)db_col_i64(st, 0);
        *checked = (int)db_col_i64(st, 1);
    }
    sqlite3_finalize(st);
    return 0;
}

static void account_summary_json(jw_t *w, const account_t *a, int64_t filter_date)
{
    jw_obj(w);
    base_account_fields(w, a);
    room_config_t cfg;
    int has_cfg = config_load(a->id, &cfg);
    jw_key(w, "config");
    if (has_cfg) {
        char **excluded = NULL;
        int excluded_count = 0;
        config_load_excluded(a->id, &excluded, &excluded_count);
        config_json(w, &cfg, excluded, excluded_count);
        free_string_list(excluded, excluded_count);
    } else {
        jw_null(w);
    }
    week_t week;
    int has_week = week_get_active(a->id, &week);
    jw_key(w, "week");
    if (has_week) {
        char text[64];
        jw_obj(w);
        jw_key(w, "week_start");
        rc_fmt_date(week.week_start, text, sizeof(text));
        jw_str(w, text);
        jw_key(w, "week_end");
        rc_fmt_date(week.week_end, text, sizeof(text));
        jw_str(w, text);
        jw_key(w, "label");
        week_label(week.week_start, week.week_end, text, sizeof(text));
        jw_str(w, text);
        jw_key(w, "plan_type");
        jw_str(w, week.plan_type);
        jw_end(w);
    } else {
        jw_null(w);
    }
    jw_key(w, "progress");
    if (has_week) {
        int checked = 0, total = 0;
        if (filter_date > 0) {
            int64_t day_index = (filter_date - week.week_start) / 86400;
            if (day_index >= 0 && day_index <= 6 && rc_day_is_check_day((int)day_index))
                progress_for_day(week.id, (int)day_index, &checked, &total);
        } else {
            week_progress(week.id, &checked, &total);
        }
        progress_json(w, checked, total);
    } else {
        jw_null(w);
    }
    jw_end(w);
}

static int target_account(request_t *r, account_t *out, char *err_code, size_t ec_sz,
                          char *err_msg, size_t em_sz)
{
    int64_t id = 0;
    if (!req_param_i64(r, 0, &id) || id <= 0 ||
        !auth_load_account(id, out)) {
        snprintf(err_code, ec_sz, "not_found");
        snprintf(err_msg, em_sz, "账户不存在");
        return 0;
    }
    if (strcmp(out->role, "floor_manager") != 0) {
        snprintf(err_code, ec_sz, "invalid_target");
        snprintf(err_msg, em_sz, "目标账户不是楼层长");
        return 0;
    }
    return 1;
}

static int load_manager_target(request_t *r, account_t *out, char *err_msg, size_t em_sz)
{
    char code[64];
    return target_account(r, out, code, sizeof(code), err_msg, em_sz);
}

static int load_admin_target(request_t *r, account_t *out, char *err_msg, size_t em_sz)
{
    int64_t id = 0;
    if (!req_param_i64(r, 0, &id) || id <= 0 || !auth_load_account(id, out)) {
        snprintf(err_msg, em_sz, "账户不存在");
        return 0;
    }
    if (strcmp(out->role, "admin") != 0 && strcmp(out->role, "super_admin") != 0) {
        snprintf(err_msg, em_sz, "目标账户不是管理员");
        return 0;
    }
    return 1;
}

static int other_enabled_super_admins(int64_t exclude_id)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (db_prepare("SELECT COUNT(*) FROM account WHERE role='admin' AND super_admin=1 "
                   "AND status='enabled' AND id<>?1",
                   &st) == 0) {
        db_bind_i64(st, 1, exclude_id);
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static int validate_new_username(request_t *r, const char *raw, int64_t exclude_id,
                                 char *out, size_t outsz, char *err, size_t errsz)
{
    char *tmp = trim_dup(raw ? raw : "");
    int chars = utf8_count(tmp);
    int valid = *tmp && chars >= 1 && chars <= 32 && strlen(tmp) <= 64;
    for (const unsigned char *p = (const unsigned char *)tmp; *p; p++)
        if (isspace(*p) || *p < 0x20)
            valid = 0;
    if (!valid) {
        free(tmp);
        snprintf(err, errsz, "账户名不能为空、不能包含空白，最多 32 个字符");
        return 0;
    }
    char *norm = lower_dup(tmp);
    sqlite3_stmt *st = NULL;
    int exists = 0;
    if (db_prepare("SELECT id FROM account WHERE username_norm=?1 AND id<>?2", &st) == 0) {
        db_bind_text(st, 1, norm);
        db_bind_i64(st, 2, exclude_id);
        exists = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    free(norm);
    if (exists) {
        free(tmp);
        snprintf(err, errsz, "账户名已被使用");
        return -1;
    }
    snprintf(out, outsz, "%s", tmp);
    free(tmp);
    (void)r;
    return 1;
}

void h_admin_accounts(request_t *r)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, role, username, display_name, must_change_password, status, created_at, "
        "last_login_at, deleted_at, delete_at FROM account WHERE role='floor_manager' "
        "ORDER BY id";
    if (db_prepare(sql, &st) != 0) {
        resp_error(r, 500, "server_error", "读取账户列表失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "accounts");
    jw_arr(&w);
    while (sqlite3_step(st) == SQLITE_ROW) {
        account_t a;
        memset(&a, 0, sizeof(a));
        a.id = db_col_i64(st, 0);
        snprintf(a.role, sizeof(a.role), "%s", db_col_text(st, 1));
        snprintf(a.username, sizeof(a.username), "%s", db_col_text(st, 2));
        snprintf(a.display_name, sizeof(a.display_name), "%s", db_col_text(st, 3));
        a.must_change_password = sqlite3_column_int(st, 4);
        snprintf(a.status, sizeof(a.status), "%s", db_col_text(st, 5));
        a.created_at = db_col_i64(st, 6);
        a.last_login_at = db_col_i64(st, 7);
        a.deleted_at = db_col_i64(st, 8);
        a.delete_at = db_col_i64(st, 9);
        account_summary_json(&w, &a, 0);
    }
    sqlite3_finalize(st);
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_admin_account_create(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    const char *username = json_get_str(body, "username");
    const char *password = json_get_str(body, "password");
    const char *display_name = json_get_str(body, "display_name");
    if (!username || !password || !*password || strlen(password) > 256) {
        json_free(body);
        resp_error(r, 400, "invalid_request", "请填写账户名和密码");
        return;
    }
    char name[128], err[256] = "";
    int v = validate_new_username(r, username, 0, name, sizeof(name), err, sizeof(err));
    if (v != 1) {
        json_free(body);
        resp_error(r, v == -1 ? 409 : 422, v == -1 ? "username_taken" : "invalid_username",
                   "%s", err);
        return;
    }
    char display[128] = "";
    if (display_name) {
        char *tmp = trim_dup(display_name);
        if (utf8_count(tmp) > 40) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_display_name", "显示名最多 40 个字符");
            return;
        }
        snprintf(display, sizeof(display), "%s", tmp);
        free(tmp);
    }
    char *norm = lower_dup(name);
    char *hash = password_hash(password);
    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    int ok = 0;
    int64_t id = 0;
    if (db_prepare("INSERT INTO account(role, username, username_norm, password_hash, "
                   "must_change_password, status, display_name, created_at, updated_at) "
                   "VALUES('floor_manager',?1,?2,?3,0,'enabled',?4,?5,?5)", &st) == 0) {
        db_bind_text(st, 1, name);
        db_bind_text(st, 2, norm);
        db_bind_text(st, 3, hash);
        db_bind_text(st, 4, display);
        db_bind_i64(st, 5, now);
        ok = db_step_done(st) == 0;
        if (ok)
            id = db_last_id();
        sqlite3_finalize(st);
    }
    free(norm);
    secure_wipe(hash, strlen(hash));
    free(hash);
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "account_create_failed", "创建楼层长失败");
        return;
    }
    account_t a;
    auth_load_account(id, &a);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "account");
    brief_account_json(&w, &a);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 201, json);
    free(json);
    jw_free(&w);
    audit_req(r, "account_create", "account", "", "success", "创建楼层长 %s", name);
    json_free(body);
}

void h_admin_account_update(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_deleted", "已删除账户不能修改");
        return;
    }
    const char *new_username = json_get_str(body, "username");
    const char *new_password = json_get_str(body, "password");
    const char *new_display = json_get_str(body, "display_name");
    if (!new_username && !new_password && !new_display) {
        json_free(body);
        resp_error(r, 400, "invalid_request", "没有要修改的字段");
        return;
    }
    char name[128];
    snprintf(name, sizeof(name), "%s", target.username);
    if (new_username) {
        int v = validate_new_username(r, new_username, target.id, name, sizeof(name), err,
                                      sizeof(err));
        if (v != 1) {
            json_free(body);
            resp_error(r, v == -1 ? 409 : 422, v == -1 ? "username_taken" : "invalid_username",
                       "%s", err);
            return;
        }
    }
    char display[128];
    snprintf(display, sizeof(display), "%s", target.display_name);
    if (new_display) {
        char *tmp = trim_dup(new_display);
        if (utf8_count(tmp) > 40) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_display_name", "显示名最多 40 个字符");
            return;
        }
        snprintf(display, sizeof(display), "%s", tmp);
        free(tmp);
    }
    if (new_password && (!*new_password || strlen(new_password) > 256)) {
        json_free(body);
        resp_error(r, 422, "invalid_password", "密码不能为空且最多 256 字节");
        return;
    }
    char *norm = lower_dup(name);
    char *hash = new_password ? password_hash(new_password) : NULL;
    int64_t now = rc_now();
    int ok = 0;
    sqlite3_stmt *st = NULL;
    if (hash) {
        if (db_prepare("UPDATE account SET username=?1, username_norm=?2, display_name=?3, "
                       "password_hash=?4, must_change_password=0, updated_at=?5 WHERE id=?6",
                       &st) == 0) {
            db_bind_text(st, 1, name);
            db_bind_text(st, 2, norm);
            db_bind_text(st, 3, display);
            db_bind_text(st, 4, hash);
            db_bind_i64(st, 5, now);
            db_bind_i64(st, 6, target.id);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
    } else {
        if (db_prepare("UPDATE account SET username=?1, username_norm=?2, display_name=?3, "
                       "updated_at=?4 WHERE id=?5", &st) == 0) {
            db_bind_text(st, 1, name);
            db_bind_text(st, 2, norm);
            db_bind_text(st, 3, display);
            db_bind_i64(st, 4, now);
            db_bind_i64(st, 5, target.id);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
    }
    free(norm);
    if (hash) {
        secure_wipe(hash, strlen(hash));
        free(hash);
    }
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "account_update_failed", "修改账户失败");
        return;
    }
    if (new_password)
        auth_revoke_all_sessions(target.id, NULL);
    account_t updated;
    auth_load_account(target.id, &updated);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "account");
    brief_account_json(&w, &updated);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    audit_req(r, "account_update", "account", "", "success",
              "修改楼层长账户 %s（用户名=%s，显示名=%s，重置密码=%s）", target.username,
              name, display, new_password ? "是" : "否");
    json_free(body);
}

static void status_change_response(request_t *r, int64_t account_id, int status_code)
{
    account_t a;
    if (!auth_load_account(account_id, &a)) {
        resp_error(r, 500, "server_error", "账户状态异常");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "account");
    brief_account_json(&w, &a);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, status_code, json);
    free(json);
    jw_free(&w);
}

void h_admin_account_enable(request_t *r)
{
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        resp_error(r, 409, "account_deleted", "已删除账户请使用撤销删除恢复");
        return;
    }
    if (strcmp(target.status, "enabled") == 0) {
        status_change_response(r, target.id, 200);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='enabled', updated_at=?1 WHERE id=?2", &st) != 0) {
        resp_error(r, 500, "account_enable_failed", "启用账户失败");
        return;
    }
    db_bind_i64(st, 1, rc_now());
    db_bind_i64(st, 2, target.id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        resp_error(r, 500, "account_enable_failed", "启用账户失败");
        return;
    }
    auth_record_login_success(target.username);
    audit_req(r, "account_enable", "account", "", "success", "启用楼层长 %s", target.username);
    status_change_response(r, target.id, 200);
}

void h_admin_account_disable(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "禁用账户需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_deleted", "已删除账户不能禁用");
        return;
    }
    if (strcmp(target.status, "disabled") == 0) {
        json_free(body);
        status_change_response(r, target.id, 200);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='disabled', updated_at=?1 WHERE id=?2", &st) != 0) {
        json_free(body);
        resp_error(r, 500, "account_disable_failed", "禁用账户失败");
        return;
    }
    db_bind_i64(st, 1, rc_now());
    db_bind_i64(st, 2, target.id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        json_free(body);
        resp_error(r, 500, "account_disable_failed", "禁用账户失败");
        return;
    }
    auth_revoke_all_sessions(target.id, NULL);
    audit_req(r, "account_disable", "account", "", "success",
              "禁用楼层长 %s，保留全部业务数据", target.username);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_account_delete(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "删除账户需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_already_deleted", "账户已处于删除状态");
        return;
    }
    int64_t now = rc_now();
    int64_t delete_at = now + RC_DELETE_SECONDS;
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='deleted', deleted_at=?1, delete_at=?2, "
                   "updated_at=?1 WHERE id=?3 AND status IN ('enabled','disabled')",
                   &st) != 0) {
        json_free(body);
        resp_error(r, 500, "account_delete_failed", "删除账户失败");
        return;
    }
    db_bind_i64(st, 1, now);
    db_bind_i64(st, 2, delete_at);
    db_bind_i64(st, 3, target.id);
    int rc = db_step_done(st);
    int changed = db_changes();
    sqlite3_finalize(st);
    if (rc != 0 || changed == 0) {
        json_free(body);
        resp_error(r, 500, "account_delete_failed", "删除账户失败");
        return;
    }
    auth_revoke_all_sessions(target.id, NULL);
    audit_req(r, "account_delete", "account", "", "success",
              "删除楼层长 %s，立即禁止登录，数据保留 72 天（至 %lld）", target.username,
              (long long)delete_at);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_account_restore(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "撤销删除需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") != 0) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "账户未处于删除状态");
        return;
    }
    int64_t now = rc_now();
    if (target.delete_at <= now) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "已到永久删除时间，无法撤销删除");
        return;
    }
    sqlite3_stmt *task = NULL;
    if (db_prepare("SELECT status FROM deletion_task WHERE account_id=?1", &task) == 0) {
        db_bind_i64(task, 1, target.id);
        if (sqlite3_step(task) == SQLITE_ROW) {
            const char *ts = db_col_text(task, 0);
            if (strcmp(ts, "processing") == 0 || strcmp(ts, "done") == 0) {
                sqlite3_finalize(task);
                json_free(body);
                resp_error(r, 409, "restore_not_allowed", "永久删除已开始，无法撤销");
                return;
            }
        }
        sqlite3_finalize(task);
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='enabled', deleted_at=NULL, delete_at=NULL, "
                   "updated_at=?1 WHERE id=?2 AND status='deleted' AND delete_at>?3", &st) != 0) {
        json_free(body);
        resp_error(r, 500, "restore_failed", "撤销删除失败");
        return;
    }
    db_bind_i64(st, 1, now);
    db_bind_i64(st, 2, target.id);
    db_bind_i64(st, 3, now);
    int rc = db_step_done(st);
    int changed = db_changes();
    sqlite3_finalize(st);
    if (rc != 0 || changed == 0) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "撤销删除失败，账户状态已变化");
        return;
    }
    if (db_prepare("DELETE FROM deletion_task WHERE account_id=?1", &st) == 0) {
        db_bind_i64(st, 1, target.id);
        db_step_done(st);
        sqlite3_finalize(st);
    }
    auth_record_login_success(target.username);
    audit_req(r, "account_restore", "account", "", "success",
              "撤销删除楼层长 %s（原删除时间 %lld），恢复为启用状态", target.username,
              (long long)target.deleted_at);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_account_week(request_t *r)
{
    account_t target;
    char err[256] = "";
    if (!load_manager_target(r, &target, err, sizeof(err))) {
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    jw_t w;
    jw_init(&w);
    if (week_build_json(r, target.id, &w, err, sizeof(err)) != 0) {
        jw_free(&w);
        resp_error(r, 500, "week_load_failed", "%s", err);
        return;
    }
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_admin_admins(request_t *r)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, role, username, display_name, must_change_password, status, created_at, "
        "last_login_at, deleted_at, delete_at, super_admin FROM account WHERE role='admin' "
        "ORDER BY super_admin DESC, id";
    if (db_prepare(sql, &st) != 0) {
        resp_error(r, 500, "server_error", "读取管理员账户列表失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "accounts");
    jw_arr(&w);
    while (sqlite3_step(st) == SQLITE_ROW) {
        account_t a;
        memset(&a, 0, sizeof(a));
        a.id = db_col_i64(st, 0);
        snprintf(a.role, sizeof(a.role), "%s", db_col_text(st, 1));
        snprintf(a.username, sizeof(a.username), "%s", db_col_text(st, 2));
        snprintf(a.display_name, sizeof(a.display_name), "%s", db_col_text(st, 3));
        a.must_change_password = sqlite3_column_int(st, 4);
        snprintf(a.status, sizeof(a.status), "%s", db_col_text(st, 5));
        a.created_at = db_col_i64(st, 6);
        a.last_login_at = db_col_i64(st, 7);
        a.deleted_at = db_col_i64(st, 8);
        a.delete_at = db_col_i64(st, 9);
        a.super_admin = sqlite3_column_int(st, 10);
        if (a.super_admin && strcmp(a.role, "admin") == 0)
            snprintf(a.role, sizeof(a.role), "%s", "super_admin");
        brief_account_json(&w, &a);
    }
    sqlite3_finalize(st);
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_admin_admin_create(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    const char *username = json_get_str(body, "username");
    const char *password = json_get_str(body, "password");
    const char *display_name = json_get_str(body, "display_name");
    if (!username || !password || !*password || strlen(password) > 256) {
        json_free(body);
        resp_error(r, 400, "invalid_request", "请填写账户名和密码");
        return;
    }
    char name[128], err[256] = "";
    int v = validate_new_username(r, username, 0, name, sizeof(name), err, sizeof(err));
    if (v != 1) {
        json_free(body);
        resp_error(r, v == -1 ? 409 : 422, v == -1 ? "username_taken" : "invalid_username",
                   "%s", err);
        return;
    }
    char display[128] = "";
    if (display_name) {
        char *tmp = trim_dup(display_name);
        if (utf8_count(tmp) > 40) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_display_name", "显示名最多 40 个字符");
            return;
        }
        snprintf(display, sizeof(display), "%s", tmp);
        free(tmp);
    }
    char *norm = lower_dup(name);
    char *hash = password_hash(password);
    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    int ok = 0;
    int64_t id = 0;
    if (db_prepare("INSERT INTO account(role, username, username_norm, password_hash, "
                   "must_change_password, status, display_name, created_at, updated_at, "
                   "super_admin) VALUES('admin',?1,?2,?3,0,'enabled',?4,?5,?5,0)",
                   &st) == 0) {
        db_bind_text(st, 1, name);
        db_bind_text(st, 2, norm);
        db_bind_text(st, 3, hash);
        db_bind_text(st, 4, display);
        db_bind_i64(st, 5, now);
        ok = db_step_done(st) == 0;
        if (ok)
            id = db_last_id();
        sqlite3_finalize(st);
    }
    free(norm);
    secure_wipe(hash, strlen(hash));
    free(hash);
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "account_create_failed", "创建管理员失败");
        return;
    }
    account_t a;
    auth_load_account(id, &a);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "account");
    brief_account_json(&w, &a);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 201, json);
    free(json);
    jw_free(&w);
    audit_req(r, "account_create", "account", "", "success", "创建管理员 %s", name);
    json_free(body);
}

void h_admin_admin_update(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    account_t target;
    char err[256] = "";
    if (!load_admin_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_deleted", "已删除账户不能修改");
        return;
    }
    const char *new_username = json_get_str(body, "username");
    const char *new_password = json_get_str(body, "password");
    const char *new_display = json_get_str(body, "display_name");
    if (!new_username && !new_password && !new_display) {
        json_free(body);
        resp_error(r, 400, "invalid_request", "没有要修改的字段");
        return;
    }
    char name[128];
    snprintf(name, sizeof(name), "%s", target.username);
    if (new_username) {
        int v = validate_new_username(r, new_username, target.id, name, sizeof(name), err,
                                      sizeof(err));
        if (v != 1) {
            json_free(body);
            resp_error(r, v == -1 ? 409 : 422, v == -1 ? "username_taken" : "invalid_username",
                       "%s", err);
            return;
        }
    }
    char display[128];
    snprintf(display, sizeof(display), "%s", target.display_name);
    if (new_display) {
        char *tmp = trim_dup(new_display);
        if (utf8_count(tmp) > 40) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_display_name", "显示名最多 40 个字符");
            return;
        }
        snprintf(display, sizeof(display), "%s", tmp);
        free(tmp);
    }
    if (new_password && (!*new_password || strlen(new_password) > 256)) {
        json_free(body);
        resp_error(r, 422, "invalid_password", "密码不能为空且最多 256 字节");
        return;
    }
    char *norm = lower_dup(name);
    char *hash = new_password ? password_hash(new_password) : NULL;
    int64_t now = rc_now();
    int ok = 0;
    sqlite3_stmt *st = NULL;
    if (hash) {
        if (db_prepare("UPDATE account SET username=?1, username_norm=?2, display_name=?3, "
                       "password_hash=?4, must_change_password=0, updated_at=?5 WHERE id=?6",
                       &st) == 0) {
            db_bind_text(st, 1, name);
            db_bind_text(st, 2, norm);
            db_bind_text(st, 3, display);
            db_bind_text(st, 4, hash);
            db_bind_i64(st, 5, now);
            db_bind_i64(st, 6, target.id);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
    } else {
        if (db_prepare("UPDATE account SET username=?1, username_norm=?2, display_name=?3, "
                       "updated_at=?4 WHERE id=?5",
                       &st) == 0) {
            db_bind_text(st, 1, name);
            db_bind_text(st, 2, norm);
            db_bind_text(st, 3, display);
            db_bind_i64(st, 4, now);
            db_bind_i64(st, 5, target.id);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
    }
    free(norm);
    if (hash) {
        secure_wipe(hash, strlen(hash));
        free(hash);
    }
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "account_update_failed", "修改账户失败");
        return;
    }
    if (new_password)
        auth_revoke_all_sessions(target.id, NULL);
    account_t updated;
    auth_load_account(target.id, &updated);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "account");
    brief_account_json(&w, &updated);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    audit_req(r, "account_update", "account", "", "success",
              "修改管理员账户 %s（用户名=%s，显示名=%s，重置密码=%s）", target.username,
              name, display, new_password ? "是" : "否");
    json_free(body);
}

void h_admin_admin_enable(request_t *r)
{
    account_t target;
    char err[256] = "";
    if (!load_admin_target(r, &target, err, sizeof(err))) {
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        resp_error(r, 409, "account_deleted", "已删除账户请使用撤销删除恢复");
        return;
    }
    if (strcmp(target.status, "enabled") == 0) {
        status_change_response(r, target.id, 200);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='enabled', updated_at=?1 WHERE id=?2", &st) != 0) {
        resp_error(r, 500, "account_enable_failed", "启用账户失败");
        return;
    }
    db_bind_i64(st, 1, rc_now());
    db_bind_i64(st, 2, target.id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        resp_error(r, 500, "account_enable_failed", "启用账户失败");
        return;
    }
    auth_record_login_success(target.username);
    audit_req(r, "account_enable", "account", "", "success", "启用管理员 %s", target.username);
    status_change_response(r, target.id, 200);
}

void h_admin_admin_disable(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "禁用账户需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_admin_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_deleted", "已删除账户不能禁用");
        return;
    }
    if (strcmp(target.status, "disabled") == 0) {
        json_free(body);
        status_change_response(r, target.id, 200);
        return;
    }
    if (strcmp(target.role, "super_admin") == 0 &&
        other_enabled_super_admins(target.id) == 0) {
        json_free(body);
        resp_error(r, 409, "last_super_admin", "不能禁用最后一个可用超级管理员");
        return;
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='disabled', updated_at=?1 WHERE id=?2", &st) != 0) {
        json_free(body);
        resp_error(r, 500, "account_disable_failed", "禁用账户失败");
        return;
    }
    db_bind_i64(st, 1, rc_now());
    db_bind_i64(st, 2, target.id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        json_free(body);
        resp_error(r, 500, "account_disable_failed", "禁用账户失败");
        return;
    }
    auth_revoke_all_sessions(target.id, NULL);
    audit_req(r, "account_disable", "account", "", "success",
              "禁用管理员 %s，保留全部业务数据", target.username);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_admin_delete(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "删除账户需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_admin_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        json_free(body);
        resp_error(r, 409, "account_already_deleted", "账户已处于删除状态");
        return;
    }
    if (strcmp(target.role, "super_admin") == 0 &&
        other_enabled_super_admins(target.id) == 0) {
        json_free(body);
        resp_error(r, 409, "last_super_admin", "不能删除最后一个可用超级管理员");
        return;
    }
    int64_t now = rc_now();
    int64_t delete_at = now + RC_DELETE_SECONDS;
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='deleted', deleted_at=?1, delete_at=?2, "
                   "updated_at=?1 WHERE id=?3 AND status IN ('enabled','disabled')",
                   &st) != 0) {
        json_free(body);
        resp_error(r, 500, "account_delete_failed", "删除账户失败");
        return;
    }
    db_bind_i64(st, 1, now);
    db_bind_i64(st, 2, delete_at);
    db_bind_i64(st, 3, target.id);
    int rc = db_step_done(st);
    int changed = db_changes();
    sqlite3_finalize(st);
    if (rc != 0 || changed == 0) {
        json_free(body);
        resp_error(r, 500, "account_delete_failed", "删除账户失败");
        return;
    }
    auth_revoke_all_sessions(target.id, NULL);
    audit_req(r, "account_delete", "account", "", "success",
              "删除管理员 %s，立即禁止登录，数据保留 72 天（至 %lld）", target.username,
              (long long)delete_at);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_admin_restore(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "撤销删除需要二次确认");
        return;
    }
    account_t target;
    char err[256] = "";
    if (!load_admin_target(r, &target, err, sizeof(err))) {
        json_free(body);
        resp_error(r, 404, "not_found", "%s", err);
        return;
    }
    if (strcmp(target.status, "deleted") != 0) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "账户未处于删除状态");
        return;
    }
    int64_t now = rc_now();
    if (target.delete_at <= now) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "已到永久删除时间，无法撤销删除");
        return;
    }
    sqlite3_stmt *task = NULL;
    if (db_prepare("SELECT status FROM deletion_task WHERE account_id=?1", &task) == 0) {
        db_bind_i64(task, 1, target.id);
        if (sqlite3_step(task) == SQLITE_ROW) {
            const char *ts = db_col_text(task, 0);
            if (strcmp(ts, "processing") == 0 || strcmp(ts, "done") == 0) {
                sqlite3_finalize(task);
                json_free(body);
                resp_error(r, 409, "restore_not_allowed", "永久删除已开始，无法撤销");
                return;
            }
        }
        sqlite3_finalize(task);
    }
    sqlite3_stmt *st = NULL;
    if (db_prepare("UPDATE account SET status='enabled', deleted_at=NULL, delete_at=NULL, "
                   "updated_at=?1 WHERE id=?2 AND status='deleted' AND delete_at>?3",
                   &st) != 0) {
        json_free(body);
        resp_error(r, 500, "restore_failed", "撤销删除失败");
        return;
    }
    db_bind_i64(st, 1, now);
    db_bind_i64(st, 2, target.id);
    db_bind_i64(st, 3, now);
    int rc = db_step_done(st);
    int changed = db_changes();
    sqlite3_finalize(st);
    if (rc != 0 || changed == 0) {
        json_free(body);
        resp_error(r, 409, "restore_not_allowed", "撤销删除失败，账户状态已变化");
        return;
    }
    if (db_prepare("DELETE FROM deletion_task WHERE account_id=?1", &st) == 0) {
        db_bind_i64(st, 1, target.id);
        db_step_done(st);
        sqlite3_finalize(st);
    }
    auth_record_login_success(target.username);
    audit_req(r, "account_restore", "account", "", "success",
              "撤销删除管理员 %s（原删除时间 %lld），恢复为启用状态", target.username,
              (long long)target.deleted_at);
    json_free(body);
    status_change_response(r, target.id, 200);
}

void h_admin_overview(request_t *r)
{
    int64_t filter_date = 0;
    char date_text[32] = "";
    const char *date_q = req_query(r, "date");
    if (date_q && *date_q) {
        filter_date = rc_date_start(date_q);
        if (filter_date < 0) {
            resp_error(r, 400, "invalid_date", "日期格式应为 YYYY-MM-DD");
            return;
        }
        time_t t = (time_t)(filter_date + 20 * 3600);
        struct tm tm;
        gmtime_r(&t, &tm);
        if (tm.tm_wday == 4) {
            resp_error(r, 422, "thursday_hidden", "周四为隐藏查寝日，不参与统计");
            return;
        }
        snprintf(date_text, sizeof(date_text), "%s", date_q);
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, role, username, display_name, must_change_password, status, created_at, "
        "last_login_at, deleted_at, delete_at FROM account WHERE role='floor_manager' "
        "ORDER BY id";
    if (db_prepare(sql, &st) != 0) {
        resp_error(r, 500, "server_error", "读取概览失败");
        return;
    }
    int total_accounts = 0;
    int total_checked = 0, total_rooms = 0;
    /* collect rows first so totals and list stay consistent */
    account_t *accounts = NULL;
    int account_count = 0, account_cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (account_count == account_cap) {
            account_cap = account_cap ? account_cap * 2 : 8;
            accounts = realloc(accounts, (size_t)account_cap * sizeof(account_t));
            if (!accounts)
                abort();
        }
        account_t *a = &accounts[account_count++];
        memset(a, 0, sizeof(*a));
        a->id = db_col_i64(st, 0);
        snprintf(a->role, sizeof(a->role), "%s", db_col_text(st, 1));
        snprintf(a->username, sizeof(a->username), "%s", db_col_text(st, 2));
        snprintf(a->display_name, sizeof(a->display_name), "%s", db_col_text(st, 3));
        a->must_change_password = sqlite3_column_int(st, 4);
        snprintf(a->status, sizeof(a->status), "%s", db_col_text(st, 5));
        a->created_at = db_col_i64(st, 6);
        a->last_login_at = db_col_i64(st, 7);
        a->deleted_at = db_col_i64(st, 8);
        a->delete_at = db_col_i64(st, 9);
    }
    sqlite3_finalize(st);

    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "date");
    if (date_text[0])
        jw_str(&w, date_text);
    else
        jw_null(&w);
    jw_key(&w, "accounts");
    jw_arr(&w);
    for (int i = 0; i < account_count; i++) {
        account_t *a = &accounts[i];
        if (strcmp(a->status, "deleted") != 0) {
            total_accounts++;
            week_t week;
            if (week_get_active(a->id, &week)) {
                int checked = 0, total = 0;
                if (filter_date > 0) {
                    int64_t day_index = (filter_date - week.week_start) / 86400;
                    if (day_index >= 0 && day_index <= 6 &&
                        rc_day_is_check_day((int)day_index))
                        progress_for_day(week.id, (int)day_index, &checked, &total);
                } else {
                    week_progress(week.id, &checked, &total);
                }
                total_checked += checked;
                total_rooms += total;
            }
        }
        account_summary_json(&w, a, filter_date);
    }
    jw_end(&w);
    jw_key(&w, "totals");
    jw_obj(&w);
    jw_key(&w, "accounts");
    jw_int(&w, total_accounts);
    jw_key(&w, "checked");
    jw_int(&w, total_checked);
    jw_key(&w, "total");
    jw_int(&w, total_rooms);
    jw_key(&w, "percent");
    jw_int(&w, total_rooms > 0 ? total_checked * 100 / total_rooms : 0);
    jw_end(&w);
    jw_end(&w);
    free(accounts);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

/* ------------------------------------------------------------------ */
/* logs                                                                */
/* ------------------------------------------------------------------ */
void h_admin_logs(request_t *r)
{
    int page = 1, page_size = 50;
    const char *p = req_query(r, "page");
    const char *ps = req_query(r, "page_size");
    if (p) {
        int64_t v;
        if (parse_i64(p, &v) && v > 0)
            page = (int)v;
    }
    if (ps) {
        int64_t v;
        if (parse_i64(ps, &v) && v > 0)
            page_size = (int)v;
    }
    if (page_size > 200)
        page_size = 200;
    const char *action = req_query(r, "action");
    const char *account_id_s = req_query(r, "account_id");
    const char *from = req_query(r, "from");
    const char *to = req_query(r, "to");
    int64_t account_id = 0;
    if (account_id_s && *account_id_s && !parse_i64(account_id_s, &account_id)) {
        resp_error(r, 400, "invalid_account", "账户参数错误");
        return;
    }
    strbuf_t where;
    sb_init(&where);
    sb_puts(&where, " WHERE 1=1");
    int binds = 0;
    int64_t bind_vals[5] = {0, 0, 0, 0, 0};
    int64_t bind_types[5] = {0, 0, 0, 0, 0}; /* 1=id, 2=text */
    char *bind_texts[5] = {NULL, NULL, NULL, NULL, NULL};
    if (account_id > 0) {
        binds++;
        sb_printf(&where, " AND actor_id=%d", binds);
        bind_vals[binds - 1] = account_id;
        bind_types[binds - 1] = 1;
    }
    if (action && *action) {
        binds++;
        sb_printf(&where, " AND action=?%d", binds);
        bind_texts[binds - 1] = xstrdup(action);
        bind_types[binds - 1] = 2;
    }
    if (from && *from) {
        int64_t ts = rc_date_start(from);
        if (ts < 0) {
            for (int i = 0; i < 5; i++)
                free(bind_texts[i]);
            sb_free(&where);
            resp_error(r, 400, "invalid_date", "起始日期格式错误");
            return;
        }
        binds++;
        sb_printf(&where, " AND created_at>=?%d", binds);
        bind_vals[binds - 1] = ts;
        bind_types[binds - 1] = 1;
    }
    if (to && *to) {
        int64_t ts = rc_date_start(to);
        if (ts < 0) {
            for (int i = 0; i < 5; i++)
                free(bind_texts[i]);
            sb_free(&where);
            resp_error(r, 400, "invalid_date", "结束日期格式错误");
            return;
        }
        binds++;
        sb_printf(&where, " AND created_at<?%d", binds);
        bind_vals[binds - 1] = ts + 86400;
        bind_types[binds - 1] = 1;
    }

    char count_sql[1024];
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM audit_log%s", sb_cstr(&where));
    sqlite3_stmt *st = NULL;
    int total = 0;
    if (db_prepare(count_sql, &st) == 0) {
        for (int i = 0; i < binds; i++) {
            if (bind_types[i] == 1)
                db_bind_i64(st, i + 1, bind_vals[i]);
            else
                db_bind_text(st, i + 1, bind_texts[i]);
        }
        if (sqlite3_step(st) == SQLITE_ROW)
            total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    char list_sql[1200];
    snprintf(list_sql, sizeof(list_sql),
             "SELECT id, actor_type, COALESCE(actor_id,0), actor_name, action, target_type, "
             "target_id, detail, result, ip, created_at FROM audit_log%s "
             "ORDER BY id DESC LIMIT %d OFFSET %d", sb_cstr(&where), page_size,
             (page - 1) * page_size);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "total");
    jw_int(&w, total);
    jw_key(&w, "page");
    jw_int(&w, page);
    jw_key(&w, "page_size");
    jw_int(&w, page_size);
    jw_key(&w, "items");
    jw_arr(&w);
    if (db_prepare(list_sql, &st) == 0) {
        for (int i = 0; i < binds; i++) {
            if (bind_types[i] == 1)
                db_bind_i64(st, i + 1, bind_vals[i]);
            else
                db_bind_text(st, i + 1, bind_texts[i]);
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            char text[32];
            jw_obj(&w);
            jw_key(&w, "id");
            jw_i64(&w, db_col_i64(st, 0));
            jw_key(&w, "actor_type");
            jw_str(&w, db_col_text(st, 1));
            jw_key(&w, "actor_id");
            jw_i64(&w, db_col_i64(st, 2));
            jw_key(&w, "actor_name");
            jw_str(&w, db_col_text(st, 3));
            jw_key(&w, "action");
            jw_str(&w, db_col_text(st, 4));
            jw_key(&w, "target_type");
            jw_str(&w, db_col_text(st, 5));
            jw_key(&w, "target_id");
            jw_str(&w, db_col_text(st, 6));
            jw_key(&w, "detail");
            jw_str(&w, db_col_text(st, 7));
            jw_key(&w, "result");
            jw_str(&w, db_col_text(st, 8));
            jw_key(&w, "ip");
            jw_str(&w, db_col_text(st, 9));
            jw_key(&w, "created_at");
            jw_i64(&w, db_col_i64(st, 10));
            jw_key(&w, "created_at_text");
            rc_fmt_datetime(db_col_i64(st, 10), text, sizeof(text));
            jw_str(&w, text);
            jw_end(&w);
        }
        sqlite3_finalize(st);
    }
    jw_end(&w);
    jw_key(&w, "actions");
    jw_arr(&w);
    if (db_prepare("SELECT DISTINCT action FROM audit_log ORDER BY action", &st) == 0) {
        while (sqlite3_step(st) == SQLITE_ROW)
            jw_str(&w, db_col_text(st, 0));
        sqlite3_finalize(st);
    }
    jw_end(&w);
    jw_end(&w);
    for (int i = 0; i < 5; i++)
        free(bind_texts[i]);
    sb_free(&where);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

/* ------------------------------------------------------------------ */
/* backups                                                             */
/* ------------------------------------------------------------------ */
static void backup_json(jw_t *w, int64_t id, const char *dir_name, int64_t size,
                        int64_t created_at)
{
    char text[32];
    char *size_text = format_size(size);
    jw_obj(w);
    jw_key(w, "id");
    jw_i64(w, id);
    jw_key(w, "dir_name");
    jw_str(w, dir_name);
    jw_key(w, "size");
    jw_i64(w, size);
    jw_key(w, "size_text");
    jw_str(w, size_text);
    free(size_text);
    jw_key(w, "created_at");
    jw_i64(w, created_at);
    jw_key(w, "created_at_text");
    rc_fmt_datetime(created_at, text, sizeof(text));
    jw_str(w, text);
    jw_key(w, "status");
    jw_str(w, "success");
    jw_end(w);
}

void h_admin_backups(request_t *r)
{
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT id, dir_name, size, created_at FROM backup_record "
                   "ORDER BY created_at DESC, id DESC LIMIT 50", &st) != 0) {
        resp_error(r, 500, "server_error", "读取备份列表失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "backups");
    jw_arr(&w);
    while (sqlite3_step(st) == SQLITE_ROW)
        backup_json(&w, db_col_i64(st, 0), db_col_text(st, 1), db_col_i64(st, 2),
                    db_col_i64(st, 3));
    sqlite3_finalize(st);
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_admin_backup_create(request_t *r)
{
    char dir_name[128] = "", err[256] = "";
    if (backup_run("backup", 3, dir_name, sizeof(dir_name), err, sizeof(err)) != 0) {
        audit_req(r, "backup_failed", "backup", "", "warning", "手动备份失败：%s", err);
        resp_error(r, 500, "backup_failed", "%s", err);
        return;
    }
    char *dir_path = path_join(g_cfg.backup_dir, dir_name);
    int64_t size = backup_dir_size(dir_path);
    free(dir_path);
    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    if (db_prepare("INSERT INTO backup_record(dir_name, path, size, status, created_at) "
                   "VALUES(?1,?2,?3,'success',?4)", &st) == 0) {
        char *p = path_join(g_cfg.backup_dir, dir_name);
        db_bind_text(st, 1, dir_name);
        db_bind_text(st, 2, p);
        db_bind_i64(st, 3, size);
        db_bind_i64(st, 4, now);
        if (db_step_done(st) == 0)
            id = db_last_id();
        sqlite3_finalize(st);
        free(p);
    }
    if (id == 0) {
        resp_error(r, 500, "backup_record_failed", "备份已完成但记录失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "backup");
    backup_json(&w, id, dir_name, size, now);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 201, json);
    free(json);
    jw_free(&w);
    audit_req(r, "backup_success", "backup", dir_name, "success", "手动备份完成：%s", dir_name);
}

void h_admin_backup_restore(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "恢复备份需要二次确认");
        return;
    }
    int64_t id = 0;
    if (!req_param_i64(r, 0, &id)) {
        json_free(body);
        resp_error(r, 400, "invalid_backup", "备份参数错误");
        return;
    }
    char err[512] = "";
    if (backup_restore(id, err, sizeof(err)) != 0) {
        audit_req(r, "backup_restore", "backup", "", "failure", "恢复备份失败：%s", err);
        resp_error(r, 500, "restore_failed", "%s", err);
        json_free(body);
        return;
    }
    audit_req(r, "backup_restore", "backup", "", "success",
              "管理员恢复备份，数据库与图片已回滚，全部会话失效");
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "ok");
    jw_bool(&w, 1);
    jw_key(&w, "message");
    jw_str(&w, "恢复完成，请重新登录");
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    json_free(body);
}

void h_admin_backup_export(request_t *r)
{
    const char *csrf = req_header(r, "X-CSRF-Token");
    if (!csrf || !secure_eq(csrf, r->csrf_token)) {
        resp_error(r, 403, "csrf_invalid", "CSRF 校验失败，请刷新页面后重试");
        return;
    }
    int64_t backup_id = 0;
    if (!req_param_i64(r, 0, &backup_id) || backup_id <= 0) {
        resp_error(r, 400, "invalid_backup", "备份参数错误");
        return;
    }
    strbuf_t archive;
    sb_init(&archive);
    char file_name[160] = "", err[512] = "";
    int rc = backup_export_archive(backup_id, &archive, file_name, sizeof(file_name), err,
                                   sizeof(err));
    if (rc != 0) {
        audit_req(r, "backup_export", "backup", "", "failure", "导出备份失败：%s", err);
        sb_free(&archive);
        resp_error(r, rc == -1 ? 404 : 500, rc == -1 ? "not_found" : "backup_export_failed",
                   "%s", err);
        return;
    }
    audit_req(r, "backup_export", "backup", file_name, "success", "导出备份文件 %s",
              file_name);
    sb_free(&r->out);
    r->out = archive;
    r->status = 200;
    r->no_store = 1;
    snprintf(r->content_type, sizeof(r->content_type), "application/x-tar");
    snprintf(r->extra_headers, sizeof(r->extra_headers),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", file_name);
}

/* ------------------------------------------------------------------ */
/* orphan notes / images (rooms removed from the current range)        */
/* ------------------------------------------------------------------ */
static void orphan_note_json(jw_t *w, sqlite3_stmt *st)
{
    char text[32];
    jw_obj(w);
    jw_key(w, "id");
    jw_i64(w, db_col_i64(st, 0));
    jw_key(w, "content");
    jw_str(w, db_col_text(st, 1));
    jw_key(w, "created_at");
    jw_i64(w, db_col_i64(st, 2));
    jw_key(w, "created_at_text");
    rc_fmt_datetime(db_col_i64(st, 2), text, sizeof(text));
    jw_str(w, text);
    jw_key(w, "updated_at");
    jw_i64(w, db_col_i64(st, 3));
    jw_end(w);
}

static void orphan_image_json(jw_t *w, sqlite3_stmt *st)
{
    char url[64];
    char *size_text = format_size(db_col_i64(st, 3));
    jw_obj(w);
    jw_key(w, "id");
    jw_i64(w, db_col_i64(st, 0));
    jw_key(w, "original_name");
    jw_str(w, db_col_text(st, 1));
    jw_key(w, "mime");
    jw_str(w, db_col_text(st, 2));
    jw_key(w, "size");
    jw_i64(w, db_col_i64(st, 3));
    jw_key(w, "size_text");
    jw_str(w, size_text);
    free(size_text);
    jw_key(w, "url");
    snprintf(url, sizeof(url), "/api/images/%lld/file", (long long)db_col_i64(st, 0));
    jw_str(w, url);
    jw_end(w);
}

void h_admin_orphans(request_t *r)
{
    const char *qid = req_query(r, "account_id");
    int64_t account_id = 0;
    if (!qid || !parse_i64(qid, &account_id) || account_id <= 0) {
        resp_error(r, 400, "account_required", "请指定楼层长账户");
        return;
    }
    account_t target;
    if (!auth_load_account(account_id, &target) ||
        strcmp(target.role, "floor_manager") != 0) {
        resp_error(r, 404, "not_found", "楼层长账户不存在");
        return;
    }
    week_t week;
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "items");
    jw_arr(&w);
    int item_count = 0;
    if (week_get_active(account_id, &week)) {
        sqlite3_stmt *st = NULL;
        const char *keys_sql =
            "SELECT DISTINCT room_key FROM ("
            " SELECT room_key FROM note WHERE week_id=?1 AND account_id=?2"
            " UNION SELECT room_key FROM note_image WHERE week_id=?1 AND account_id=?2) "
            "WHERE room_key NOT IN (SELECT room_key FROM room_check WHERE week_id=?1) "
            "ORDER BY room_key";
        if (db_prepare(keys_sql, &st) == 0) {
            db_bind_i64(st, 1, week.id);
            db_bind_i64(st, 2, account_id);
            char **keys = NULL;
            int key_count = 0, key_cap = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (key_count == key_cap) {
                    key_cap = key_cap ? key_cap * 2 : 8;
                    keys = realloc(keys, (size_t)key_cap * sizeof(char *));
                    if (!keys)
                        abort();
                }
                keys[key_count++] = xstrdup(db_col_text(st, 0));
            }
            sqlite3_finalize(st);
            for (int i = 0; i < key_count; i++) {
                jw_obj(&w);
                jw_key(&w, "room_key");
                jw_str(&w, keys[i]);
                const char *hash = strrchr(keys[i], '#');
                jw_key(&w, "room_number");
                jw_str(&w, hash ? hash + 1 : keys[i]);
                jw_key(&w, "notes");
                jw_arr(&w);
                if (db_prepare("SELECT id, content, created_at, updated_at FROM note "
                               "WHERE week_id=?1 AND room_key=?2 ORDER BY id", &st) == 0) {
                    db_bind_i64(st, 1, week.id);
                    db_bind_text(st, 2, keys[i]);
                    while (sqlite3_step(st) == SQLITE_ROW)
                        orphan_note_json(&w, st);
                    sqlite3_finalize(st);
                }
                jw_end(&w);
                jw_key(&w, "images");
                jw_arr(&w);
                if (db_prepare("SELECT id, original_name, mime, size FROM note_image "
                               "WHERE week_id=?1 AND room_key=?2 ORDER BY id", &st) == 0) {
                    db_bind_i64(st, 1, week.id);
                    db_bind_text(st, 2, keys[i]);
                    while (sqlite3_step(st) == SQLITE_ROW)
                        orphan_image_json(&w, st);
                    sqlite3_finalize(st);
                }
                jw_end(&w);
                jw_end(&w);
                item_count++;
                free(keys[i]);
            }
            free(keys);
        }
    }
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    if (item_count > 0) {
        audit_req(r, "note_view", "account", "", "success",
                  "管理员查看楼层长 %s 被移出范围的备注/图片（%d 间寝室）", target.username,
                  item_count);
    }
}
