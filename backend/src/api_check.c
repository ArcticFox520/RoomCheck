/*
 * api_check.c - check / uncheck rooms and progress
 */
#include "common.h"

static int next_unchecked_room(int64_t week_id)
{
    sqlite3_stmt *st = NULL;
    int64_t id = 0;
    const char *sql =
        "SELECT rc.id FROM room_check rc JOIN check_group g ON g.id=rc.group_id "
        "WHERE rc.week_id=?1 AND rc.checked=0 "
        "ORDER BY g.day_index, CASE WHEN g.period='noon' THEN 0 ELSE 1 END, "
        "rc.position, rc.id LIMIT 1";
    if (db_prepare(sql, &st) == 0) {
        db_bind_i64(st, 1, week_id);
        if (sqlite3_step(st) == SQLITE_ROW)
            id = db_col_i64(st, 0);
        sqlite3_finalize(st);
    }
    return (int)id;
}

static void room_state_response(request_t *r, const room_row_t *room)
{
    int checked = 0, total = 0;
    week_progress(room->week_id, &checked, &total);
    int next = next_unchecked_room(room->week_id);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "room");
    jw_obj(&w);
    jw_key(&w, "id");
    jw_i64(&w, room->id);
    jw_key(&w, "room_key");
    jw_str(&w, room->room_key);
    jw_key(&w, "room_number");
    jw_str(&w, room->room_number);
    jw_key(&w, "checked");
    jw_bool(&w, room->checked);
    jw_key(&w, "checked_at");
    if (room->checked_at > 0)
        jw_i64(&w, room->checked_at);
    else
        jw_null(&w);
    jw_end(&w);
    jw_key(&w, "progress");
    progress_json(&w, checked, total);
    jw_key(&w, "next_room_id");
    if (next > 0)
        jw_int(&w, next);
    else
        jw_null(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

static void audit_check(request_t *r, const room_row_t *room, const char *verb)
{
    if (is_admin_request(r) && room->account_id != r->account_id) {
        account_t owner;
        const char *name = "";
        if (auth_load_account(room->account_id, &owner))
            name = owner.username;
        audit_req(r, "check_toggle", "room", room->room_key, "success",
                  "管理员%s楼层长 %s 的寝室 %s", verb, name, room->room_key);
    } else {
        audit_req(r, "check_toggle", "room", room->room_key, "success", "%s寝室 %s", verb,
                  room->room_key);
    }
}

static int load_room_or_error(request_t *r, int64_t room_id, room_row_t *room)
{
    char err[256] = "";
    int status = load_room(r, room_id, room, err, sizeof(err));
    if (status == 200)
        return 1;
    const char *code = status == 404 ? "not_found" : status == 403 ? "forbidden" : "server_error";
    resp_error(r, status, code, "%s", err[0] ? err : "寝室记录不可用");
    return 0;
}

void h_check(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id) || room_id <= 0) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    room_row_t room;
    if (!load_room_or_error(r, room_id, &room))
        return;
    if (!room.checked) {
        sqlite3_stmt *st = NULL;
        if (db_prepare("UPDATE room_check SET checked=1, checked_at=?1 WHERE id=?2 AND checked=0",
                       &st) != 0) {
            resp_error(r, 500, "check_failed", "保存勾选状态失败");
            return;
        }
        db_bind_i64(st, 1, rc_now());
        db_bind_i64(st, 2, room.id);
        int rc = db_step_done(st);
        sqlite3_finalize(st);
        if (rc != 0) {
            resp_error(r, 500, "check_failed", "保存勾选状态失败");
            return;
        }
        room.checked = 1;
        room.checked_at = rc_now();
        audit_check(r, &room, "勾选");
    }
    room_state_response(r, &room);
}

void h_uncheck(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id) || room_id <= 0) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!json_get_bool(body, "confirm", 0)) {
        json_free(body);
        resp_error(r, 400, "confirm_required", "撤销勾选需要二次确认");
        return;
    }
    json_free(body);
    room_row_t room;
    if (!load_room_or_error(r, room_id, &room))
        return;
    if (room.checked) {
        sqlite3_stmt *st = NULL;
        if (db_prepare("UPDATE room_check SET checked=0, checked_at=NULL WHERE id=?1",
                       &st) != 0) {
            resp_error(r, 500, "uncheck_failed", "保存撤销状态失败");
            return;
        }
        db_bind_i64(st, 1, room.id);
        int rc = db_step_done(st);
        sqlite3_finalize(st);
        if (rc != 0) {
            resp_error(r, 500, "uncheck_failed", "保存撤销状态失败");
            return;
        }
        room.checked = 0;
        room.checked_at = 0;
        audit_check(r, &room, "撤销勾选");
    }
    room_state_response(r, &room);
}
