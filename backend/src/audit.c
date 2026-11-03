/*
 * audit.c - operation audit log
 */
#include "common.h"

int audit_write(const char *actor_type, int64_t actor_id, const char *actor_name,
                const char *action, const char *target_type, const char *target_id,
                const char *result, const char *detail, const char *ip)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO audit_log(actor_type, actor_id, actor_name, action, target_type, "
        "target_id, detail, result, ip, created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)";
    if (db_prepare(sql, &st) != 0) {
        fprintf(stderr, "AUDIT INSERT PREPARE FAILED: %s\n", db_error());
        return -1;
    }
    db_bind_text(st, 1, actor_type);
    if (actor_id > 0)
        db_bind_i64(st, 2, actor_id);
    else
        db_bind_null(st, 2);
    db_bind_text(st, 3, actor_name);
    db_bind_text(st, 4, action);
    db_bind_text(st, 5, target_type);
    db_bind_text(st, 6, target_id);
    db_bind_text(st, 7, detail);
    db_bind_text(st, 8, result);
    db_bind_text(st, 9, ip);
    db_bind_i64(st, 10, rc_now());
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        fprintf(stderr, "AUDIT INSERT FAILED: %s\n", db_error());
        return -1;
    }
    return 0;
}

int audit_req(request_t *r, const char *action, const char *target_type,
              const char *target_id, const char *result, const char *fmt, ...)
{
    char detail[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    const char *actor_type = "anonymous";
    int64_t actor_id = 0;
    const char *actor_name = "";
    const char *ip = "unknown";
    if (r) {
        ip = r->remote_ip;
        if (r->authed) {
            actor_type = r->role;
            actor_id = r->account_id;
            actor_name = r->username;
        }
    }
    return audit_write(actor_type, actor_id, actor_name, action, target_type, target_id,
                       result, detail, ip);
}
