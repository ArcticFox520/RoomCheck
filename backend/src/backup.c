/*
 * backup.c - daily/manual backups (SQLite + image volume), verification and
 *            administrator restore with a pre-restore snapshot.
 */
#include "common.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t tree_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return (int64_t)st.st_size;
    int64_t total = 0;
    char **names = NULL;
    int count = 0;
    if (dir_list(path, &names, &count, -1) != 0)
        return 0;
    for (int i = 0; i < count; i++) {
        char *full = path_join(path, names[i]);
        total += tree_size(full);
        free(full);
        free(names[i]);
    }
    free(names);
    return total;
}

int64_t backup_dir_size(const char *path)
{
    return tree_size(path);
}

static int count_files(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return 1;
    int total = 0;
    char **names = NULL;
    int count = 0;
    if (dir_list(path, &names, &count, -1) != 0)
        return 0;
    for (int i = 0; i < count; i++) {
        char *full = path_join(path, names[i]);
        total += count_files(full);
        free(full);
        free(names[i]);
    }
    free(names);
    return total;
}

static int copy_database(const char *dest_path, char *err, size_t errsz)
{
    sqlite3 *dest = NULL;
    int rc = sqlite3_open_v2(dest_path, &dest,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errsz, "无法创建备份数据库文件");
        if (dest)
            sqlite3_close(dest);
        return -1;
    }
    sqlite3_backup *bk = sqlite3_backup_init(dest, "main", db_handle(), "main");
    if (!bk) {
        snprintf(err, errsz, "数据库备份初始化失败: %s", sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return -1;
    }
    int steps = 0;
    do {
        rc = sqlite3_backup_step(bk, 128);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            sqlite3_sleep(20);
            steps++;
        }
    } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && steps < 500);
    int frc = sqlite3_backup_finish(bk);
    if (rc != SQLITE_DONE || frc != SQLITE_OK) {
        snprintf(err, errsz, "数据库备份失败: %s", sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return -1;
    }
    sqlite3_close(dest);

    sqlite3 *verify = NULL;
    if (sqlite3_open_v2(dest_path, &verify, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "备份数据库无法打开");
        if (verify)
            sqlite3_close(verify);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(verify, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW &&
            strcmp((const char *)sqlite3_column_text(st, 0), "ok") == 0)
            ok = 1;
        sqlite3_finalize(st);
    }
    sqlite3_close(verify);
    if (!ok) {
        snprintf(err, errsz, "备份数据库完整性校验失败");
        return -1;
    }
    return 0;
}

static void prune_backups(int keep)
{
    char **names = NULL;
    int count = 0;
    if (dir_list(g_cfg.backup_dir, &names, &count, 0) != 0)
        return;
    int kept = 0;
    int kept_snapshots = 0;
    /* names are sorted ascending; newest are at the end */
    for (int i = count - 1; i >= 0; i--) {
        const char *name = names[i];
        int is_backup = str_starts_with(name, "backup_");
        int is_snapshot = str_starts_with(name, "pre_restore_");
        if (!is_backup && !is_snapshot) {
            free(names[i]);
            continue;
        }
        int drop = 0;
        if (is_backup) {
            if (kept >= keep)
                drop = 1;
            else
                kept++;
        } else {
            if (kept_snapshots >= 2)
                drop = 1;
            else
                kept_snapshots++;
        }
        if (drop) {
            char *full = path_join(g_cfg.backup_dir, name);
            remove_tree(full);
            free(full);
            if (is_backup) {
                sqlite3_stmt *st = NULL;
                if (db_prepare("DELETE FROM backup_record WHERE dir_name=?1", &st) == 0) {
                    db_bind_text(st, 1, name);
                    db_step_done(st);
                    sqlite3_finalize(st);
                }
            }
        }
        free(names[i]);
    }
    free(names);
}

int backup_run(const char *prefix, int keep, char *out_dir, size_t outsz, char *err, size_t errsz)
{
    if (!db_handle()) {
        snprintf(err, errsz, "数据库未打开");
        return -1;
    }
    if (mkdir_p(g_cfg.backup_dir, 0750) != 0) {
        snprintf(err, errsz, "无法创建备份目录");
        return -1;
    }
    char ts[32];
    time_t t = (time_t)(rc_now() + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
    char dir_name[128];
    snprintf(dir_name, sizeof(dir_name), "%s_%s", prefix, ts);
    char *dir_path = path_join(g_cfg.backup_dir, dir_name);
    int suffix = 1;
    while (file_exists(dir_path)) {
        free(dir_path);
        snprintf(dir_name, sizeof(dir_name), "%s_%s_%d", prefix, ts, suffix++);
        dir_path = path_join(g_cfg.backup_dir, dir_name);
    }
    if (mkdir_p(dir_path, 0750) != 0) {
        free(dir_path);
        snprintf(err, errsz, "无法创建备份目录");
        return -1;
    }

    char *db_dest = path_join(dir_path, "roomcheck.db");
    if (copy_database(db_dest, err, errsz) != 0) {
        free(db_dest);
        remove_tree(dir_path);
        free(dir_path);
        return -1;
    }
    char db_sha[65] = "";
    {
        char *data = NULL;
        size_t len = 0;
        if (file_read(db_dest, &data, &len) == 0) {
            rc_sha256_hex(data, len, db_sha);
            free(data);
        }
    }
    int64_t db_size = tree_size(db_dest);

    char *uploads_dest = path_join(dir_path, "uploads");
    mkdir_p(uploads_dest, 0750);
    int copy_rc = copy_tree(g_cfg.upload_dir, uploads_dest);
    if (copy_rc < 0) {
        snprintf(err, errsz, "复制图片文件失败");
        free(db_dest);
        free(uploads_dest);
        remove_tree(dir_path);
        free(dir_path);
        return -1;
    }
    if (copy_rc == 1)
        mkdir_p(uploads_dest, 0750); /* no images yet */

    int image_files = count_files(uploads_dest);
    jw_t m;
    jw_init(&m);
    jw_obj(&m);
    jw_key(&m, "name");
    jw_str(&m, dir_name);
    jw_key(&m, "created_at");
    jw_i64(&m, rc_now());
    jw_key(&m, "version");
    jw_str(&m, RC_VERSION);
    jw_key(&m, "database");
    jw_str(&m, "roomcheck.db");
    jw_key(&m, "database_sha256");
    jw_str(&m, db_sha);
    jw_key(&m, "database_size");
    jw_i64(&m, db_size);
    jw_key(&m, "image_files");
    jw_int(&m, image_files);
    jw_end(&m);
    char *manifest = sb_steal(&m.b);
    char *manifest_path = path_join(dir_path, "manifest.json");
    int ok = file_write(manifest_path, manifest, strlen(manifest)) == 0;
    free(manifest);
    free(manifest_path);
    jw_free(&m);
    if (!ok) {
        snprintf(err, errsz, "写入备份清单失败");
        free(db_dest);
        free(uploads_dest);
        remove_tree(dir_path);
        free(dir_path);
        return -1;
    }
    free(db_dest);
    free(uploads_dest);
    if (out_dir && outsz)
        snprintf(out_dir, outsz, "%s", dir_name);
    if (keep > 0)
        prune_backups(keep);
    free(dir_path);
    return 0;
}

static int unlink_if_exists(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT)
        return -1;
    return 0;
}

static int restore_copy_snapshot(const char *snapshot_dir, char *err, size_t errsz)
{
    char *snap_db = path_join(snapshot_dir, "roomcheck.db");
    char *snap_uploads = path_join(snapshot_dir, "uploads");
    db_close_conn();
    char wal[1200], shm[1200];
    snprintf(wal, sizeof(wal), "%s-wal", g_cfg.db_path);
    snprintf(shm, sizeof(shm), "%s-shm", g_cfg.db_path);
    unlink_if_exists(wal);
    unlink_if_exists(shm);
    unlink_if_exists(g_cfg.db_path);
    int ok = copy_file(snap_db, g_cfg.db_path) == 0;
    if (ok) {
        remove_tree(g_cfg.upload_dir);
        mkdir_p(g_cfg.upload_dir, 0750);
        int crc = copy_tree(snap_uploads, g_cfg.upload_dir);
        if (crc < 0)
            ok = 0;
    }
    free(snap_db);
    free(snap_uploads);
    if (ok && db_init(g_cfg.db_path) != 0) {
        snprintf(err, errsz, "回滚后重新打开数据库失败");
        return -1;
    }
    if (!ok) {
        snprintf(err, errsz, "回滚快照恢复失败");
        return -1;
    }
    return 0;
}

int backup_restore(int64_t backup_id, char *err, size_t errsz)
{
    sqlite3_stmt *st = NULL;
    char dir_name[128] = "";
    int64_t created_at = 0;
    if (db_prepare("SELECT dir_name, created_at FROM backup_record WHERE id=?1", &st) != 0) {
        snprintf(err, errsz, "读取备份记录失败");
        return -1;
    }
    db_bind_i64(st, 1, backup_id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        snprintf(err, errsz, "备份不存在");
        return -1;
    }
    snprintf(dir_name, sizeof(dir_name), "%s", db_col_text(st, 0));
    created_at = db_col_i64(st, 1);
    sqlite3_finalize(st);
    (void)created_at;
    if (!dir_name[0] || !path_is_safe(dir_name) || strchr(dir_name, '/')) {
        snprintf(err, errsz, "备份名称非法");
        return -1;
    }
    char *dir_path = path_join(g_cfg.backup_dir, dir_name);
    char *backup_db = path_join(dir_path, "roomcheck.db");
    char *backup_uploads = path_join(dir_path, "uploads");
    if (!file_exists(backup_db) || !is_dir(backup_uploads)) {
        snprintf(err, errsz, "备份文件不完整");
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }
    /* verify the backup database before doing anything destructive */
    sqlite3 *verify = NULL;
    if (sqlite3_open_v2(backup_db, &verify, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "无法打开备份数据库");
        if (verify)
            sqlite3_close(verify);
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }
    int integrity_ok = 0;
    if (sqlite3_prepare_v2(verify, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW &&
            strcmp((const char *)sqlite3_column_text(st, 0), "ok") == 0)
            integrity_ok = 1;
        sqlite3_finalize(st);
    }
    if (!integrity_ok) {
        sqlite3_close(verify);
        snprintf(err, errsz, "备份数据库完整性校验失败，已拒绝恢复");
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }
    if (sqlite3_prepare_v2(verify, "SELECT COUNT(*) FROM account", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) != SQLITE_ROW)
            integrity_ok = 0;
        sqlite3_finalize(st);
    } else {
        integrity_ok = 0;
    }
    sqlite3_close(verify);
    if (!integrity_ok) {
        snprintf(err, errsz, "备份数据库结构不兼容");
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }

    /* snapshot current state so a failed restore can be rolled back */
    char snap_name[128] = "";
    char snap_err[256] = "";
    if (backup_run("pre_restore", 0, snap_name, sizeof(snap_name), snap_err,
                   sizeof(snap_err)) != 0) {
        snprintf(err, errsz, "创建恢复前快照失败：%s", snap_err);
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }
    char *snap_dir = path_join(g_cfg.backup_dir, snap_name);

    db_close_conn();
    char wal[1200], shm[1200];
    snprintf(wal, sizeof(wal), "%s-wal", g_cfg.db_path);
    snprintf(shm, sizeof(shm), "%s-shm", g_cfg.db_path);
    unlink_if_exists(wal);
    unlink_if_exists(shm);
    unlink_if_exists(g_cfg.db_path);

    int ok = copy_file(backup_db, g_cfg.db_path) == 0;
    if (!ok)
        snprintf(err, errsz, "复制备份数据库失败");
    if (ok) {
        if (remove_tree(g_cfg.upload_dir) != 0) {
            ok = 0;
            snprintf(err, errsz, "清空当前图片目录失败");
        } else if (mkdir_p(g_cfg.upload_dir, 0750) != 0) {
            ok = 0;
            snprintf(err, errsz, "重建图片目录失败");
        } else {
            int crc = copy_tree(backup_uploads, g_cfg.upload_dir);
            if (crc != 0) {
                ok = 0;
                snprintf(err, errsz, "恢复图片文件失败");
            }
        }
    }
    if (ok && db_init(g_cfg.db_path) != 0) {
        ok = 0;
        snprintf(err, errsz, "重新打开数据库失败");
    }
    if (ok) {
        sqlite3_stmt *del = NULL;
        if (db_prepare("DELETE FROM session", &del) != 0 ||
            db_step_done(del) != 0) {
            ok = 0;
            snprintf(err, errsz, "恢复后清理登录会话失败");
        }
        if (del)
            sqlite3_finalize(del);
    }
    if (!ok) {
        char rb_err[256] = "";
        if (restore_copy_snapshot(snap_dir, rb_err, sizeof(rb_err)) != 0)
            snprintf(err + strlen(err), errsz - strlen(err),
                     "；自动回滚失败：%s", rb_err);
        else
            snprintf(err + strlen(err), errsz - strlen(err), "；已自动回滚到恢复前状态");
        remove_tree(snap_dir);
        free(snap_dir);
        free(dir_path);
        free(backup_db);
        free(backup_uploads);
        return -1;
    }
    remove_tree(snap_dir);
    free(snap_dir);
    free(dir_path);
    free(backup_db);
    free(backup_uploads);
    return 0;
}
