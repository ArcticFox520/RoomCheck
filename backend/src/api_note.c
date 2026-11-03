/*
 * api_note.c - multiple notes per room, 500 character limit
 */
#include "common.h"

#include <unistd.h>

static void note_json(jw_t *w, int64_t id, const char *content, int64_t created_at,
                      int64_t updated_at)
{
    char text[32];
    jw_obj(w);
    jw_key(w, "id");
    jw_i64(w, id);
    jw_key(w, "content");
    jw_str(w, content);
    jw_key(w, "created_at");
    jw_i64(w, created_at);
    jw_key(w, "updated_at");
    jw_i64(w, updated_at);
    jw_key(w, "created_at_text");
    rc_fmt_datetime(created_at, text, sizeof(text));
    jw_str(w, text);
    jw_key(w, "updated_at_text");
    rc_fmt_datetime(updated_at, text, sizeof(text));
    jw_str(w, text);
    jw_end(w);
}

static int validate_note_content(request_t *r, jval_t *body, char **content_out)
{
    *content_out = NULL;
    const char *content = json_get_str(body, "content");
    if (!content) {
        resp_error(r, 400, "invalid_note", "备注内容不能为空");
        return 0;
    }
    if (strlen(content) > 4096) {
        resp_error(r, 422, "note_limit", "单条备注过长");
        return 0;
    }
    int chars = utf8_count(content);
    if (chars == 0) {
        resp_error(r, 400, "invalid_note", "备注内容不能为空");
        return 0;
    }
    int only_space = 1;
    for (const unsigned char *p = (const unsigned char *)content; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            only_space = 0;
            break;
        }
    }
    if (only_space) {
        resp_error(r, 400, "invalid_note", "备注内容不能只有空白字符");
        return 0;
    }
    *content_out = xstrdup(content);
    return 1;
}

static void audit_note_target(request_t *r, const room_row_t *room, const char *action,
                              const char *detail)
{
    if (is_admin_request(r) && room->account_id != r->account_id) {
        account_t owner;
        const char *name = "";
        if (auth_load_account(room->account_id, &owner))
            name = owner.username;
        audit_req(r, action, "room", room->room_key, "success",
                  "管理员操作楼层长 %s 的寝室 %s：%s", name, room->room_key, detail);
    } else {
        audit_req(r, action, "room", room->room_key, "success", "%s", detail);
    }
}

void h_notes_list(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id)) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    room_row_t room;
    char err[256] = "";
    int status = load_room(r, room_id, &room, err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, content, created_at, updated_at FROM note WHERE week_id=?1 "
        "AND room_key=?2 ORDER BY id";
    if (db_prepare(sql, &st) != 0) {
        resp_error(r, 500, "server_error", "读取备注失败");
        return;
    }
    db_bind_i64(st, 1, room.week_id);
    db_bind_text(st, 2, room.room_key);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "notes");
    jw_arr(&w);
    int count = 0;
    int total = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t id = db_col_i64(st, 0);
        const char *content = db_col_text(st, 1);
        total += utf8_count(content);
        note_json(&w, id, content, db_col_i64(st, 2), db_col_i64(st, 3));
        count++;
    }
    sqlite3_finalize(st);
    jw_end(&w);
    jw_key(&w, "total_chars");
    jw_int(&w, total);
    jw_key(&w, "limit");
    jw_int(&w, RC_MAX_NOTE_CHARS);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);

    if (is_admin_request(r)) {
        account_t owner;
        if (auth_load_account(room.account_id, &owner)) {
            audit_req(r, "note_view", "room", room.room_key, "success",
                      "管理员查看楼层长 %s 的寝室 %s 备注（%d 条）", owner.username,
                      room.room_key, count);
        }
    }
}

void h_note_create(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id)) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    room_row_t room;
    char err[256] = "";
    int status = load_room(r, room_id, &room, err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }
    jval_t *body = req_json(r);
    if (!body)
        return;
    char *content = NULL;
    if (!validate_note_content(r, body, &content)) {
        json_free(body);
        return;
    }
    int chars = utf8_count(content);
    int total = note_total_chars(room.week_id, room.room_key);
    if (total + chars > RC_MAX_NOTE_CHARS) {
        resp_error(r, 422, "note_limit", "该寝室备注合计不能超过 %d 字（当前 %d 字，本次 %d 字）",
                   RC_MAX_NOTE_CHARS, total, chars);
        free(content);
        json_free(body);
        return;
    }
    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    int ok = 0;
    int64_t note_id = 0;
    if (db_prepare("INSERT INTO note(account_id, week_id, room_key, room_number, content, "
                   "created_at, updated_at) VALUES(?1,?2,?3,?4,?5,?6,?6)", &st) == 0) {
        db_bind_i64(st, 1, room.account_id);
        db_bind_i64(st, 2, room.week_id);
        db_bind_text(st, 3, room.room_key);
        db_bind_text(st, 4, room.room_number);
        db_bind_text(st, 5, content);
        db_bind_i64(st, 6, now);
        ok = db_step_done(st) == 0;
        if (ok)
            note_id = db_last_id();
        sqlite3_finalize(st);
    }
    if (!ok) {
        free(content);
        json_free(body);
        resp_error(r, 500, "note_create_failed", "保存备注失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "note");
    note_json(&w, note_id, content, now, now);
    jw_key(&w, "total_chars");
    jw_int(&w, total + chars);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 201, json);
    free(json);
    jw_free(&w);
    audit_note_target(r, &room, "note_create", "新增备注");
    free(content);
    json_free(body);
}

void h_note_update(request_t *r)
{
    int64_t note_id = 0;
    if (!req_param_i64(r, 0, &note_id)) {
        resp_error(r, 400, "invalid_note", "备注参数错误");
        return;
    }
    room_row_t room;
    int64_t real_note_id = 0;
    char err[256] = "";
    int status = load_note(r, note_id, &room, &real_note_id, err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }
    jval_t *body = req_json(r);
    if (!body)
        return;
    char *content = NULL;
    if (!validate_note_content(r, body, &content)) {
        json_free(body);
        return;
    }
    char old_content[4096] = "";
    int64_t created_at = rc_now();
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT content, created_at FROM note WHERE id=?1", &st) == 0) {
        db_bind_i64(st, 1, real_note_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            snprintf(old_content, sizeof(old_content), "%s", db_col_text(st, 0));
            created_at = db_col_i64(st, 1);
        }
        sqlite3_finalize(st);
    }
    int chars = utf8_count(content);
    int old_chars = utf8_count(old_content);
    int total = note_total_chars(room.week_id, room.room_key);
    if (total - old_chars + chars > RC_MAX_NOTE_CHARS) {
        resp_error(r, 422, "note_limit", "该寝室备注合计不能超过 %d 字", RC_MAX_NOTE_CHARS);
        free(content);
        json_free(body);
        return;
    }
    int64_t now = rc_now();
    if (db_prepare("UPDATE note SET content=?1, updated_at=?2 WHERE id=?3", &st) != 0) {
        resp_error(r, 500, "note_update_failed", "更新备注失败");
        free(content);
        json_free(body);
        return;
    }
    db_bind_text(st, 1, content);
    db_bind_i64(st, 2, now);
    db_bind_i64(st, 3, real_note_id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        resp_error(r, 500, "note_update_failed", "更新备注失败");
        free(content);
        json_free(body);
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "note");
    note_json(&w, real_note_id, content, created_at, now);
    jw_key(&w, "total_chars");
    jw_int(&w, total - old_chars + chars);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    audit_note_target(r, &room, "note_update", "修改备注");
    free(content);
    json_free(body);
}

void h_note_delete(request_t *r)
{
    int64_t note_id = 0;
    if (!req_param_i64(r, 0, &note_id)) {
        resp_error(r, 400, "invalid_note", "备注参数错误");
        return;
    }
    room_row_t room;
    int64_t real_note_id = 0;
    char err[256] = "";
    int status = load_note(r, note_id, &room, &real_note_id, err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }

    char **paths = NULL;
    int path_count = 0;
    sqlite3_stmt *st = NULL;
    if (db_prepare("SELECT rel_path FROM note_image WHERE note_id=?1", &st) == 0) {
        db_bind_i64(st, 1, real_note_id);
        int cap = 8;
        paths = xcalloc((size_t)cap, sizeof(char *));
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (path_count == cap) {
                cap *= 2;
                paths = realloc(paths, (size_t)cap * sizeof(char *));
                if (!paths)
                    abort();
            }
            paths[path_count++] = xstrdup(db_col_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    for (int i = 0; i < path_count; i++) {
        if (!path_is_safe(paths[i])) {
            free_string_list(paths, path_count);
            resp_error(r, 500, "note_delete_failed", "图片路径异常，已停止删除");
            return;
        }
        char *full = path_join(g_cfg.upload_dir, paths[i]);
        if (unlink(full) != 0 && errno != ENOENT) {
            free(full);
            free_string_list(paths, path_count);
            resp_error(r, 500, "note_delete_failed", "删除备注关联图片失败，未删除备注");
            return;
        }
        free(full);
    }
    free_string_list(paths, path_count);

    if (db_prepare("DELETE FROM note WHERE id=?1", &st) != 0) {
        resp_error(r, 500, "note_delete_failed", "删除备注失败");
        return;
    }
    db_bind_i64(st, 1, real_note_id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        resp_error(r, 500, "note_delete_failed", "删除备注失败");
        return;
    }
    int total = note_total_chars(room.week_id, room.room_key);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "ok");
    jw_bool(&w, 1);
    jw_key(&w, "deleted_images");
    jw_int(&w, path_count);
    jw_key(&w, "total_chars");
    jw_int(&w, total);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    audit_note_target(r, &room, "note_delete", "删除备注及其关联图片");
}
