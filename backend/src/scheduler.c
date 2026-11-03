/*
 * scheduler.c - background maintenance: 72-day account purge, daily backups,
 *               audit-log weekly retention and expired session cleanup.
 */
#include "common.h"

#include <unistd.h>

static pthread_t g_thread;
static volatile int g_stop = 0;
static volatile int g_started = 0;

static char *meta_get(const char *key)
{
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (db_prepare("SELECT value FROM meta WHERE key=?1", &st) == 0) {
        db_bind_text(st, 1, key);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = xstrdup(db_col_text(st, 0));
        sqlite3_finalize(st);
    }
    return out;
}

static void meta_set(const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (db_prepare("INSERT INTO meta(key, value) VALUES(?1,?2) "
                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value", &st) == 0) {
        db_bind_text(st, 1, key);
        db_bind_text(st, 2, value);
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

static void task_upsert(int64_t account_id, const char *status, const char *error)
{
    sqlite3_stmt *st = NULL;
    if (db_prepare("INSERT INTO deletion_task(account_id, status, attempts, last_error, "
                   "created_at, updated_at) VALUES(?1,?2,1,?3,?4,?4) "
                   "ON CONFLICT(account_id) DO UPDATE SET status=?2, "
                   "attempts=deletion_task.attempts+1, last_error=?3, updated_at=?4",
                   &st) == 0) {
        db_bind_i64(st, 1, account_id);
        db_bind_text(st, 2, status);
        db_bind_text(st, 3, error ? error : "");
        db_bind_i64(st, 4, rc_now());
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

static void task_delete(int64_t account_id)
{
    sqlite3_stmt *st = NULL;
    if (db_prepare("DELETE FROM deletion_task WHERE account_id=?1", &st) == 0) {
        db_bind_i64(st, 1, account_id);
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

static void mark_failed(int64_t account_id, const char *error)
{
    task_upsert(account_id, "failed", error);
    audit_write("system", 0, "system", "deletion_retry_failed", "account", "",
                "warning", error ? error : "永久删除失败，稍后重试", "scheduler");
}

static int process_deletion(int64_t account_id)
{
    account_t acct;
    if (!auth_load_account(account_id, &acct)) {
        task_delete(account_id);
        return 0;
    }
    int64_t now = rc_now();
    if (strcmp(acct.status, "deleted") != 0 || acct.delete_at > now) {
        task_delete(account_id);
        return 0;
    }
    task_upsert(account_id, "processing", "");

    /* remove image files first; missing files are treated as already deleted */
    sqlite3_stmt *st = NULL;
    int file_error = 0;
    if (db_prepare("SELECT rel_path FROM note_image WHERE account_id=?1", &st) == 0) {
        db_bind_i64(st, 1, account_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *rel = db_col_text(st, 0);
            if (!path_is_safe(rel)) {
                file_error = 1;
                break;
            }
            char *full = path_join(g_cfg.upload_dir, rel);
            if (unlink(full) != 0 && errno != ENOENT)
                file_error = 1;
            free(full);
            if (file_error)
                break;
        }
        sqlite3_finalize(st);
    } else {
        file_error = 1;
    }
    if (file_error) {
        mark_failed(account_id, "删除图片文件失败，已保留账户数据等待重试");
        return -1;
    }

    char detail[512];
    snprintf(detail, sizeof(detail),
             "72 天期满，永久删除楼层长 %s（账户 ID %lld）及其配置、排班、勾选、备注、图片",
             acct.username, (long long)account_id);
    int ok = 0;
    if (db_begin() == 0) {
        sqlite3_stmt *del = NULL;
        int deleted = 0;
        if (db_prepare("DELETE FROM account WHERE id=?1 AND role='floor_manager' "
                       "AND status='deleted' AND delete_at<=?2", &del) == 0) {
            db_bind_i64(del, 1, account_id);
            db_bind_i64(del, 2, rc_now());
            if (db_step_done(del) == 0)
                deleted = db_changes();
            sqlite3_finalize(del);
        }
        if (!deleted) {
            db_rollback();
            task_delete(account_id);
            return 0;
        }
        if (audit_write("system", 0, "system", "account_purge", "account", "",
                        "success", detail, "scheduler") != 0) {
            db_rollback();
            mark_failed(account_id, "永久删除审计日志写入失败，已回滚");
            return -1;
        }
        if (db_commit() != 0) {
            db_rollback();
            mark_failed(account_id, "永久删除事务提交失败");
            return -1;
        }
        ok = 1;
    } else {
        mark_failed(account_id, "数据库繁忙，永久删除任务稍后重试");
        return -1;
    }
    task_delete(account_id);
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", (long long)account_id);
    char *dir = path_join(g_cfg.upload_dir, idbuf);
    if (remove_tree(dir) != 0) {
        audit_write("system", 0, "system", "account_purge", "account", idbuf,
                    "warning", "账户已永久删除，但图片目录清理失败，需要人工检查", "scheduler");
    }
    free(dir);
    return ok ? 0 : -1;
}

static void scan_deletions(void)
{
    sqlite3_stmt *st = NULL;
    int64_t now = rc_now();
    int64_t retry_before = now - 60;
    const char *sql =
        "SELECT a.id FROM account a LEFT JOIN deletion_task t ON t.account_id=a.id "
        "WHERE a.status='deleted' AND a.delete_at<=?1 AND (t.id IS NULL OR "
        "t.status='pending' OR (t.status='failed' AND t.updated_at<=?2) OR "
        "(t.status='processing' AND t.updated_at<=?2))";
    if (db_prepare(sql, &st) != 0)
        return;
    db_bind_i64(st, 1, now);
    db_bind_i64(st, 2, retry_before);
    int64_t ids[64];
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < 64)
        ids[n++] = db_col_i64(st, 0);
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++)
        process_deletion(ids[i]);
}

static void cleanup_sessions(void)
{
    sqlite3_stmt *st = NULL;
    if (db_prepare("DELETE FROM session WHERE expires_at<=?1 OR revoked=1", &st) == 0) {
        db_bind_i64(st, 1, rc_now());
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

static void cleanup_audit_logs(void)
{
    int64_t week_start = rc_week_start(rc_now());
    char key[32];
    snprintf(key, sizeof(key), "%lld", (long long)week_start);
    char *last = meta_get("last_log_cleanup_week");
    if (last && strcmp(last, key) == 0) {
        free(last);
        return;
    }
    free(last);
    sqlite3_stmt *st = NULL;
    int removed = 0;
    if (db_prepare("DELETE FROM audit_log WHERE created_at<?1", &st) == 0) {
        db_bind_i64(st, 1, week_start);
        if (db_step_done(st) == 0)
            removed = db_changes();
        sqlite3_finalize(st);
    }
    meta_set("last_log_cleanup_week", key);
    audit_write("system", 0, "system", "log_cleanup", "audit_log", "", "success",
                removed > 0 ? "按自然周清理历史操作日志" : "本周日志清理完成（无过期记录）",
                "scheduler");
}

static int has_backup_today(int64_t day_start)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (db_prepare("SELECT COUNT(*) FROM backup_record WHERE created_at>=?1", &st) == 0) {
        db_bind_i64(st, 1, day_start);
        if (sqlite3_step(st) == SQLITE_ROW)
            found = sqlite3_column_int(st, 0) > 0;
        sqlite3_finalize(st);
    }
    return found;
}

static void daily_backup_check(void)
{
    int64_t now = rc_now();
    int64_t day_start = rc_day_start(now);
    int64_t backup_time = day_start + 3 * 3600;
    if (now < backup_time)
        return;
    if (has_backup_today(day_start))
        return;
    char *last = meta_get("last_backup_attempt");
    if (last) {
        int64_t ts = 0;
        parse_i64(last, &ts);
        free(last);
        if (ts > 0 && now - ts < 300)
            return;
    }
    char key[32];
    snprintf(key, sizeof(key), "%lld", (long long)now);
    meta_set("last_backup_attempt", key);

    char dir_name[128] = "", err[256] = "";
    if (backup_run("backup", 3, dir_name, sizeof(dir_name), err, sizeof(err)) != 0) {
        audit_write("system", 0, "system", "backup_failed", "backup", "", "warning",
                    err[0] ? err : "自动备份失败", "scheduler");
        fprintf(stderr, "WARNING: 自动备份失败: %s\n", err);
        return;
    }
    char *dir_path = path_join(g_cfg.backup_dir, dir_name);
    int64_t size = backup_dir_size(dir_path);
    free(dir_path);
    sqlite3_stmt *st = NULL;
    if (db_prepare("INSERT OR IGNORE INTO backup_record(dir_name, path, size, status, "
                   "created_at) VALUES(?1,'',?2,'success',?3)", &st) == 0) {
        db_bind_text(st, 1, dir_name);
        db_bind_i64(st, 2, size);
        db_bind_i64(st, 3, now);
        db_step_done(st);
        sqlite3_finalize(st);
    }
    audit_write("system", 0, "system", "backup_success", "backup", dir_name, "success",
                "每日自动备份完成", "scheduler");
}


static void cleanup_orphan_uploads(void)
{
    char **accounts = NULL;
    int account_count = 0;
    if (dir_list(g_cfg.upload_dir, &accounts, &account_count, 0) != 0)
        return;
    for (int i = 0; i < account_count; i++) {
        int64_t account_id = 0;
        if (!parse_i64(accounts[i], &account_id) || account_id <= 0) {
            free(accounts[i]);
            continue;
        }
        char *account_dir = path_join(g_cfg.upload_dir, accounts[i]);
        sqlite3_stmt *st = NULL;
        int account_exists = 0;
        if (db_prepare("SELECT COUNT(*) FROM account WHERE id=?1", &st) == 0) {
            db_bind_i64(st, 1, account_id);
            if (sqlite3_step(st) == SQLITE_ROW)
                account_exists = sqlite3_column_int(st, 0) > 0;
            sqlite3_finalize(st);
        }
        if (!account_exists) {
            remove_tree(account_dir);
            free(account_dir);
            free(accounts[i]);
            continue;
        }
        char **weeks = NULL;
        int week_count = 0;
        if (dir_list(account_dir, &weeks, &week_count, 0) == 0) {
            for (int j = 0; j < week_count; j++) {
                int64_t week_id = 0;
                if (!parse_i64(weeks[j], &week_id) || week_id <= 0) {
                    free(weeks[j]);
                    continue;
                }
                char *week_dir = path_join(account_dir, weeks[j]);
                int plan_exists = 0, images_exist = 0;
                if (db_prepare("SELECT COUNT(*) FROM week_plan WHERE id=?1 AND account_id=?2",
                               &st) == 0) {
                    db_bind_i64(st, 1, week_id);
                    db_bind_i64(st, 2, account_id);
                    if (sqlite3_step(st) == SQLITE_ROW)
                        plan_exists = sqlite3_column_int(st, 0) > 0;
                    sqlite3_finalize(st);
                }
                if (!plan_exists) {
                    if (db_prepare("SELECT COUNT(*) FROM note_image WHERE week_id=?1", &st) == 0) {
                        db_bind_i64(st, 1, week_id);
                        if (sqlite3_step(st) == SQLITE_ROW)
                            images_exist = sqlite3_column_int(st, 0) > 0;
                        sqlite3_finalize(st);
                    }
                    if (!images_exist) {
                        remove_tree(week_dir);
                        audit_write("system", 0, "system", "upload_cleanup", "week", "",
                                    "success", "清理不在任何周计划中的图片目录", "scheduler");
                    }
                }
                free(week_dir);
                free(weeks[j]);
            }
            free(weeks);
        }
        free(account_dir);
        free(accounts[i]);
    }
    free(accounts);
}

static void *scheduler_main(void *arg)
{
    (void)arg;
    int tick = 0;
    db_lock();
    scan_deletions();
    cleanup_orphan_uploads();
    cleanup_sessions();
    cleanup_audit_logs();
    if (!g_cfg.scheduler_disabled && getenv("ROOMCHECK_DISABLE_BACKUP") == NULL)
        daily_backup_check();
    db_unlock();
    while (!g_stop && !g_shutdown) {
        for (int i = 0; i < 10 && !g_stop && !g_shutdown; i++)
            usleep(200 * 1000);
        if (g_stop || g_shutdown)
            break;
        tick++;
        db_lock();
        scan_deletions();
        if (tick % 30 == 0) {
            cleanup_sessions();
            cleanup_audit_logs();
            if (!g_cfg.scheduler_disabled && getenv("ROOMCHECK_DISABLE_BACKUP") == NULL)
                daily_backup_check();
        }
        if (tick % 300 == 0)
            cleanup_orphan_uploads();
        db_unlock();
    }
    return NULL;
}

int scheduler_start(void)
{
    g_stop = 0;
    if (pthread_create(&g_thread, NULL, scheduler_main, NULL) != 0)
        return -1;
    g_started = 1;
    return 0;
}

void scheduler_stop(void)
{
    g_stop = 1;
    if (g_started) {
        pthread_join(g_thread, NULL);
        g_started = 0;
    }
}
