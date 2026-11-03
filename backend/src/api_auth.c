/*
 * api_auth.c - login, logout, session, password and profile endpoints
 */
#include "common.h"

#include <ctype.h>

static void send_session_payload(request_t *r, account_t *a, const char *csrf,
                                 int64_t expires)
{
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "user");
    account_json(&w, a);
    jw_key(&w, "csrf_token");
    jw_str(&w, csrf);
    jw_key(&w, "expires_at");
    jw_i64(&w, expires);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_health(request_t *r)
{
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "ok");
    jw_bool(&w, 1);
    jw_key(&w, "version");
    jw_str(&w, RC_VERSION);
    jw_key(&w, "time");
    jw_i64(&w, rc_now());
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_login(request_t *r)
{
    if (r->body_len > 16 * 1024) {
        resp_error(r, 400, "invalid_request", "请求体过大");
        return;
    }
    jval_t *body = req_json(r);
    if (!body)
        return;
    const char *username_raw = json_get_str(body, "username");
    const char *password = json_get_str(body, "password");
    if (!username_raw || !password || !*username_raw) {
        json_free(body);
        resp_error(r, 400, "invalid_credentials_format", "请输入账户名和密码");
        return;
    }
    char *username = trim_dup(username_raw);
    if (!*username || strlen(username) > 64 || strlen(password) > 256) {
        free(username);
        json_free(body);
        resp_error(r, 400, "invalid_credentials_format", "账户名或密码格式错误");
        return;
    }
    char err_code[64] = "";
    char err_msg[256] = "";
    int retry_after = 0;
    if (!auth_login_allowed(username, err_code, sizeof(err_code), err_msg, sizeof(err_msg),
                            &retry_after)) {
        audit_write("anonymous", 0, username, "login_failed", "account", "", "failure",
                    "账户处于登录锁定期间", r->remote_ip);
        json_free(body);
        jw_t w;
        jw_init(&w);
        jw_obj(&w);
        jw_key(&w, "error");
        jw_obj(&w);
        jw_key(&w, "code");
        jw_str(&w, err_code);
        jw_key(&w, "message");
        jw_str(&w, err_msg);
        jw_key(&w, "retry_after");
        jw_int(&w, retry_after);
        jw_end(&w);
        jw_end(&w);
        char *json = sb_steal(&w.b);
        resp_json(r, 423, json);
        free(json);
        jw_free(&w);
        free(username);
        return;
    }

    account_t acct;
    int found = auth_find_account(username, &acct);
    if (!found) {
        auth_record_login_failure(username);
        audit_write("anonymous", 0, username, "login_failed", "account", "", "failure",
                    "账户不存在", r->remote_ip);
        free(username);
        json_free(body);
        resp_error(r, 401, "invalid_credentials", "账户名或密码错误");
        return;
    }
    if (strcmp(acct.status, "disabled") == 0) {
        audit_write(acct.role, acct.id, acct.username, "login_failed", "account",
                    acct.username, "failure", "账户已被禁用", r->remote_ip);
        free(username);
        json_free(body);
        resp_error(r, 403, "account_disabled", "账户已被禁用，请联系管理员");
        return;
    }
    if (strcmp(acct.status, "deleted") == 0) {
        audit_write(acct.role, acct.id, acct.username, "login_failed", "account",
                    acct.username, "failure", "账户已删除", r->remote_ip);
        free(username);
        json_free(body);
        resp_error(r, 403, "account_deleted", "账户已删除，无法登录");
        return;
    }

    char stored_hash[512] = "";
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT password_hash FROM account WHERE id=?1", &st) == 0) {
        db_bind_i64(st, 1, acct.id);
        if (sqlite3_step(st) == SQLITE_ROW)
            snprintf(stored_hash, sizeof(stored_hash), "%s", db_col_text(st, 0));
        sqlite3_finalize(st);
    }
    if (!password_verify(password, stored_hash)) {
        int locked_now = auth_record_login_failure(username);
        audit_write(acct.role, acct.id, acct.username, "login_failed", "account",
                    acct.username, "failure", locked_now ? "密码错误，触发账户锁定" : "密码错误",
                    r->remote_ip);
        if (locked_now) {
            audit_write(acct.role, acct.id, acct.username, "login_locked", "account",
                        acct.username, "warning", "连续 5 次登录失败，锁定 10 分钟",
                        r->remote_ip);
            free(username);
            json_free(body);
            jw_t w;
            jw_init(&w);
            jw_obj(&w);
            jw_key(&w, "error");
            jw_obj(&w);
            jw_key(&w, "code");
            jw_str(&w, "account_locked");
            jw_key(&w, "message");
            jw_str(&w, "登录失败次数过多，账户已锁定 10 分钟");
            jw_key(&w, "retry_after");
            jw_int(&w, 600);
            jw_end(&w);
            jw_end(&w);
            char *json = sb_steal(&w.b);
            resp_json(r, 423, json);
            free(json);
            jw_free(&w);
            return;
        }
        free(username);
        json_free(body);
        resp_error(r, 401, "invalid_credentials", "账户名或密码错误");
        return;
    }

    auth_record_login_success(username);
    char token[65] = "", csrf[65] = "";
    int64_t expires = 0;
    if (!auth_create_session(acct.id, acct.role, token, csrf, sizeof(csrf), &expires)) {
        free(username);
        json_free(body);
        resp_error(r, 500, "session_error", "创建会话失败");
        return;
    }
    auth_set_session_cookie(r, token, RC_SESSION_TTL);

    int64_t now = rc_now();
    sqlite3_stmt *up = NULL;
    if (db_prepare("UPDATE account SET last_login_at=?1 WHERE id=?2", &up) == 0) {
        db_bind_i64(up, 1, now);
        db_bind_i64(up, 2, acct.id);
        db_step_done(up);
        sqlite3_finalize(up);
    }
    sqlite3_stmt *ip = NULL;
    if (db_prepare("UPDATE session SET ip=?1 WHERE token_hash=("
                   "SELECT token_hash FROM session WHERE account_id=?2 ORDER BY created_at DESC LIMIT 1)",
                   &ip) == 0) {
        db_bind_text(ip, 1, r->remote_ip);
        db_bind_i64(ip, 2, acct.id);
        db_step_done(ip);
        sqlite3_finalize(ip);
    }
    acct.last_login_at = now;
    audit_write(acct.role, acct.id, acct.username, "login_success", "session", "",
                "success", "登录成功", r->remote_ip);
    send_session_payload(r, &acct, csrf, expires);
    secure_wipe(token, sizeof(token));
    free(username);
    json_free(body);
}

void h_logout(request_t *r)
{
    if (r->authed) {
        audit_req(r, "logout", "session", "", "success", "退出登录");
        auth_revoke_session(r->session_hash);
    }
    auth_clear_session_cookie(r);
    resp_ok(r);
}

void h_session(request_t *r)
{
    account_t acct;
    if (!auth_load_account(r->account_id, &acct) ||
        strcmp(acct.status, "enabled") != 0) {
        auth_clear_session_cookie(r);
        resp_error(r, 401, "unauthenticated", "会话已失效，请重新登录");
        return;
    }
    send_session_payload(r, &acct, r->csrf_token, r->expires_at);
}

void h_password(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    const char *old_pw = json_get_str(body, "old_password");
    const char *new_pw = json_get_str(body, "new_password");
    if (!old_pw || !new_pw || !*new_pw || strlen(new_pw) > 256) {
        json_free(body);
        resp_error(r, 400, "invalid_password_format", "请输入原密码和新密码");
        return;
    }
    char stored[512] = "";
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT password_hash FROM account WHERE id=?1", &st) == 0) {
        db_bind_i64(st, 1, r->account_id);
        if (sqlite3_step(st) == SQLITE_ROW)
            snprintf(stored, sizeof(stored), "%s", db_col_text(st, 0));
        sqlite3_finalize(st);
    }
    if (!password_verify(old_pw, stored)) {
        json_free(body);
        resp_error(r, 422, "invalid_password", "原密码错误");
        return;
    }
    char *hash = password_hash(new_pw);
    int ok = 0;
    if (db_prepare("UPDATE account SET password_hash=?1, must_change_password=0, updated_at=?2 "
                   "WHERE id=?3", &st) == 0) {
        db_bind_text(st, 1, hash);
        db_bind_i64(st, 2, rc_now());
        db_bind_i64(st, 3, r->account_id);
        ok = db_step_done(st) == 0 && db_changes() > 0;
        sqlite3_finalize(st);
    }
    secure_wipe(hash, strlen(hash));
    free(hash);
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "password_update_failed", "密码修改失败");
        return;
    }
    char new_csrf[65];
    random_hex(new_csrf, 64);
    if (db_prepare("UPDATE session SET csrf_token=?1 WHERE token_hash=?2", &st) == 0) {
        db_bind_text(st, 1, new_csrf);
        db_bind_text(st, 2, r->session_hash);
        db_step_done(st);
        sqlite3_finalize(st);
    }
    auth_revoke_all_sessions(r->account_id, r->session_hash);
    audit_req(r, "password_change", "account", "", "success", "修改登录密码");
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "ok");
    jw_bool(&w, 1);
    jw_key(&w, "csrf_token");
    jw_str(&w, new_csrf);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    json_free(body);
}

void h_profile(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    const char *new_username = json_get_str(body, "username");
    const char *new_display = json_get_str(body, "display_name");
    if (!new_username && !new_display) {
        json_free(body);
        resp_error(r, 400, "invalid_request", "没有要修改的字段");
        return;
    }
    account_t acct;
    if (!auth_load_account(r->account_id, &acct)) {
        json_free(body);
        resp_error(r, 404, "not_found", "账户不存在");
        return;
    }
    char old_username[128];
    snprintf(old_username, sizeof(old_username), "%s", acct.username);
    char new_name_buf[128] = "";
    if (new_username) {
        char *tmp = trim_dup(new_username);
        int valid = *tmp && strlen(tmp) <= 64 && utf8_count(tmp) >= 1 && utf8_count(tmp) <= 32;
        for (const unsigned char *p = (const unsigned char *)tmp; *p; p++)
            if (isspace(*p) || *p < 0x20)
                valid = 0;
        if (!valid) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_username", "账户名不能为空、不能包含空白，最多 32 个字符");
            return;
        }
        snprintf(new_name_buf, sizeof(new_name_buf), "%s", tmp);
        free(tmp);
        if (strcmp(new_name_buf, acct.username) != 0) {
            char *norm = lower_dup(new_name_buf);
            sqlite3_stmt *st = NULL;
            int exists = 0;
            if (db_prepare("SELECT id FROM account WHERE username_norm=?1 AND id<>?2", &st) == 0) {
                db_bind_text(st, 1, norm);
                db_bind_i64(st, 2, r->account_id);
                exists = sqlite3_step(st) == SQLITE_ROW;
                sqlite3_finalize(st);
            }
            free(norm);
            if (exists) {
                json_free(body);
                resp_error(r, 409, "username_taken", "账户名已被使用");
                return;
            }
        }
    }
    char display_buf[128] = "";
    if (new_display) {
        char *tmp = trim_dup(new_display);
        if (utf8_count(tmp) > 40 || strlen(tmp) > 120) {
            free(tmp);
            json_free(body);
            resp_error(r, 422, "invalid_display_name", "显示名最多 40 个字符");
            return;
        }
        snprintf(display_buf, sizeof(display_buf), "%s", tmp);
        free(tmp);
    }
    int updating_name = new_username && strcmp(new_name_buf, acct.username) != 0;
    int updating_display = new_display != NULL;
    if (!updating_name && !updating_display) {
        json_free(body);
        send_session_payload(r, &acct, r->csrf_token, r->expires_at);
        return;
    }
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (db_prepare("UPDATE account SET username=?1, username_norm=?2, display_name=?3, "
                   "updated_at=?4 WHERE id=?5", &st) == 0) {
        const char *final_name = updating_name ? new_name_buf : acct.username;
        const char *final_display = updating_display ? display_buf : acct.display_name;
        char *norm = lower_dup(final_name);
        db_bind_text(st, 1, final_name);
        db_bind_text(st, 2, norm);
        db_bind_text(st, 3, final_display);
        db_bind_i64(st, 4, rc_now());
        db_bind_i64(st, 5, r->account_id);
        ok = db_step_done(st) == 0;
        free(norm);
        sqlite3_finalize(st);
    }
    if (!ok) {
        json_free(body);
        resp_error(r, 500, "profile_update_failed", "账户信息修改失败");
        return;
    }
    if (updating_name) {
        audit_req(r, "username_change", "account", "", "success", "账户名 %s -> %s",
                  old_username, new_name_buf);
        snprintf(r->username, sizeof(r->username), "%s", new_name_buf);
    }
    auth_load_account(r->account_id, &acct);
    send_session_payload(r, &acct, r->csrf_token, r->expires_at);
    json_free(body);
}
