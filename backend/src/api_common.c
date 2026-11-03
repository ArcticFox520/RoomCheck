/*
 * api_common.c - shared JSON builders and resource permission helpers
 */
#include "common.h"

void progress_json(jw_t *out, int checked, int total)
{
    int percent = total > 0 ? (checked * 100) / total : 0;
    jw_obj(out);
    jw_key(out, "checked");
    jw_int(out, checked);
    jw_key(out, "total");
    jw_int(out, total);
    jw_key(out, "percent");
    jw_int(out, percent);
    jw_key(out, "completed");
    jw_bool(out, total > 0 && checked >= total);
    jw_end(out);
}

void config_json(jw_t *out, const room_config_t *cfg, char **excluded, int excluded_count)
{
    jw_obj(out);
    jw_key(out, "building_name");
    jw_str(out, cfg->building_name);
    jw_key(out, "start_room");
    jw_str(out, cfg->start_room);
    jw_key(out, "end_room");
    jw_str(out, cfg->end_room);
    jw_key(out, "start_num");
    jw_i64(out, cfg->start_num);
    jw_key(out, "end_num");
    jw_i64(out, cfg->end_num);
    jw_key(out, "original_count");
    jw_i64(out, cfg->original_count);
    jw_key(out, "excluded_count");
    jw_int(out, excluded_count);
    jw_key(out, "N");
    jw_i64(out, cfg->original_count - excluded_count);
    jw_key(out, "excluded_rooms");
    jw_arr(out);
    for (int i = 0; i < excluded_count; i++)
        jw_str(out, excluded[i]);
    jw_end(out);
    jw_key(out, "updated_at");
    jw_i64(out, cfg->updated_at);
    jw_end(out);
}

int account_json(jw_t *out, const account_t *a)
{
    jw_obj(out);
    jw_key(out, "id");
    jw_i64(out, a->id);
    jw_key(out, "role");
    jw_str(out, a->role);
    jw_key(out, "username");
    jw_str(out, a->username);
    jw_key(out, "display_name");
    jw_str(out, a->display_name);
    jw_key(out, "status");
    jw_str(out, a->status);
    jw_key(out, "must_change_password");
    jw_bool(out, a->must_change_password);
    jw_key(out, "created_at");
    jw_i64(out, a->created_at);
    jw_key(out, "last_login_at");
    if (a->last_login_at > 0)
        jw_i64(out, a->last_login_at);
    else
        jw_null(out);
    jw_key(out, "deleted_at");
    if (a->deleted_at > 0)
        jw_i64(out, a->deleted_at);
    else
        jw_null(out);
    jw_key(out, "delete_at");
    if (a->delete_at > 0)
        jw_i64(out, a->delete_at);
    else
        jw_null(out);
    jw_end(out);
    return 0;
}

int is_admin_request(request_t *r)
{
    return r && r->authed && strcmp(r->role, "admin") == 0;
}

int room_permission(request_t *r, int64_t owner_account_id)
{
    if (!r || !r->authed)
        return 0;
    if (is_admin_request(r))
        return 1;
    return r->account_id == owner_account_id;
}

int require_manager_account(request_t *r, int64_t account_id)
{
    (void)r;
    account_t a;
    if (!auth_load_account(account_id, &a))
        return 0;
    return strcmp(a.role, "floor_manager") == 0;
}

int resolve_floor_manager(request_t *r, jval_t *body, int64_t *account_id,
                          char *code, size_t code_sz, char *msg, size_t msg_sz)
{
    int64_t target = r->account_id;
    if (is_admin_request(r)) {
        int64_t requested = json_get_i64(body, "account_id", 0);
        if (requested <= 0) {
            snprintf(code, code_sz, "account_required");
            snprintf(msg, msg_sz, "管理员操作必须指定楼层长账户");
            return 0;
        }
        target = requested;
        account_t a;
        if (!auth_load_account(target, &a)) {
            snprintf(code, code_sz, "not_found");
            snprintf(msg, msg_sz, "楼层长账户不存在");
            return 0;
        }
        if (strcmp(a.role, "floor_manager") != 0) {
            snprintf(code, code_sz, "invalid_target");
            snprintf(msg, msg_sz, "目标账户不是楼层长");
            return 0;
        }
    } else {
        int64_t requested = json_get_i64(body, "account_id", 0);
        if (requested > 0 && requested != r->account_id) {
            snprintf(code, code_sz, "forbidden");
            snprintf(msg, msg_sz, "不能操作其他楼层长的数据");
            return 0;
        }
    }
    *account_id = target;
    return 1;
}

int load_room(request_t *r, int64_t room_id, room_row_t *out, char *err, size_t errsz)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, account_id, week_id, group_id, room_key, room_number, checked, "
        "COALESCE(checked_at,0) FROM room_check WHERE id=?1";
    if (db_prepare(sql, &st) != 0) {
        snprintf(err, errsz, "数据库错误");
        return 500;
    }
    db_bind_i64(st, 1, room_id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        snprintf(err, errsz, "寝室记录不存在");
        return 404;
    }
    out->id = db_col_i64(st, 0);
    out->account_id = db_col_i64(st, 1);
    out->week_id = db_col_i64(st, 2);
    out->group_id = db_col_i64(st, 3);
    snprintf(out->room_key, sizeof(out->room_key), "%s", db_col_text(st, 4));
    snprintf(out->room_number, sizeof(out->room_number), "%s", db_col_text(st, 5));
    out->checked = sqlite3_column_int(st, 6);
    out->checked_at = db_col_i64(st, 7);
    sqlite3_finalize(st);
    if (!room_permission(r, out->account_id)) {
        snprintf(err, errsz, "无权访问该寝室");
        return 403;
    }
    return 200;
}

int load_note(request_t *r, int64_t note_id, room_row_t *room, int64_t *note_id_out,
              char *err, size_t errsz)
{
    memset(room, 0, sizeof(*room));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT n.account_id, n.week_id, n.room_key, n.room_number, n.id, "
        "(SELECT rc.id FROM room_check rc WHERE rc.week_id=n.week_id AND rc.room_key=n.room_key LIMIT 1) "
        "FROM note n WHERE n.id=?1";
    if (db_prepare(sql, &st) != 0) {
        snprintf(err, errsz, "数据库错误");
        return 500;
    }
    db_bind_i64(st, 1, note_id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        snprintf(err, errsz, "备注不存在");
        return 404;
    }
    room->account_id = db_col_i64(st, 0);
    room->week_id = db_col_i64(st, 1);
    snprintf(room->room_key, sizeof(room->room_key), "%s", db_col_text(st, 2));
    snprintf(room->room_number, sizeof(room->room_number), "%s", db_col_text(st, 3));
    *note_id_out = db_col_i64(st, 4);
    room->id = db_col_i64(st, 5);
    sqlite3_finalize(st);
    if (!room_permission(r, room->account_id)) {
        snprintf(err, errsz, "无权访问该备注");
        return 403;
    }
    return 200;
}

int load_image(request_t *r, int64_t image_id, room_row_t *room, char *rel_path,
               size_t rel_sz, char *mime, size_t mime_sz, char *original_name,
               size_t name_sz, char *err, size_t errsz)
{
    memset(room, 0, sizeof(*room));
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT i.account_id, i.week_id, i.room_key, i.room_number, i.rel_path, i.mime, "
        "i.original_name, i.size, (SELECT rc.id FROM room_check rc WHERE rc.week_id=i.week_id "
        "AND rc.room_key=i.room_key LIMIT 1) FROM note_image i WHERE i.id=?1";
    if (db_prepare(sql, &st) != 0) {
        snprintf(err, errsz, "数据库错误");
        return 500;
    }
    db_bind_i64(st, 1, image_id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        snprintf(err, errsz, "图片不存在");
        return 404;
    }
    room->account_id = db_col_i64(st, 0);
    room->week_id = db_col_i64(st, 1);
    snprintf(room->room_key, sizeof(room->room_key), "%s", db_col_text(st, 2));
    snprintf(room->room_number, sizeof(room->room_number), "%s", db_col_text(st, 3));
    snprintf(rel_path, rel_sz, "%s", db_col_text(st, 4));
    snprintf(mime, mime_sz, "%s", db_col_text(st, 5));
    snprintf(original_name, name_sz, "%s", db_col_text(st, 6));
    room->id = db_col_i64(st, 8);
    sqlite3_finalize(st);
    if (!room_permission(r, room->account_id)) {
        snprintf(err, errsz, "无权访问该图片");
        return 403;
    }
    return 200;
}

int note_total_chars(int64_t week_id, const char *room_key)
{
    sqlite3_stmt *st = NULL;
    int total = 0;
    if (db_prepare("SELECT COALESCE(SUM(length(content)),0) FROM note "
                   "WHERE week_id=?1 AND room_key=?2", &st) == 0) {
        db_bind_i64(st, 1, week_id);
        db_bind_text(st, 2, room_key);
        if (sqlite3_step(st) == SQLITE_ROW)
            total = (int)db_col_i64(st, 0);
        sqlite3_finalize(st);
    }
    return total;
}
