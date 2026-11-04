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

/* ------------------------------------------------------------------ */
/* verified backup export (tar archive for local download)             */
/* ------------------------------------------------------------------ */
static int64_t path_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return (int64_t)st.st_mtime;
    return rc_now();
}

static void tar_octal(char *field, size_t width, int64_t value)
{
    size_t digits = width - 1;
    for (size_t i = digits; i > 0; i--) {
        field[i - 1] = (char)('0' + (value & 7));
        value >>= 3;
    }
    field[digits] = '\0';
}

static int tar_header(strbuf_t *out, const char *name, char type, int64_t size, int64_t mtime,
                      char *err, size_t errsz)
{
    if (!name || !*name || strlen(name) >= 100) {
        snprintf(err, errsz, "备份文件路径过长");
        return -1;
    }
    if (size < 0 || size > 077777777777LL) {
        snprintf(err, errsz, "备份文件过大，无法导出");
        return -1;
    }
    unsigned char hdr[512];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, name, strlen(name));
    tar_octal((char *)hdr + 100, 8, 0644);
    tar_octal((char *)hdr + 108, 8, 0);
    tar_octal((char *)hdr + 116, 8, 0);
    tar_octal((char *)hdr + 124, 12, size);
    tar_octal((char *)hdr + 136, 12, mtime);
    memset(hdr + 148, ' ', 8);
    hdr[156] = (unsigned char)type;
    memcpy(hdr + 257, "ustar", 6);
    memcpy(hdr + 263, "00", 2);
    memcpy(hdr + 265, "root", 4);
    memcpy(hdr + 297, "root", 4);
    unsigned int sum = 0;
    for (size_t i = 0; i < sizeof(hdr); i++)
        sum += hdr[i];
    char chk[8];
    snprintf(chk, sizeof(chk), "%06o", sum);
    memcpy(hdr + 148, chk, 6);
    hdr[154] = '\0';
    hdr[155] = ' ';
    sb_putn(out, (const char *)hdr, sizeof(hdr));
    return 0;
}

static int tar_add_file(strbuf_t *out, const char *archive_name, const char *fs_path,
                        char *err, size_t errsz)
{
    char *data = NULL;
    size_t len = 0;
    if (file_read(fs_path, &data, &len) != 0) {
        snprintf(err, errsz, "读取备份文件失败: %s", archive_name);
        return -1;
    }
    int rc = tar_header(out, archive_name, '0', (int64_t)len, path_mtime(fs_path), err, errsz);
    if (rc == 0) {
        sb_putn(out, data, len);
        size_t pad = (512 - (len % 512)) % 512;
        if (pad > 0) {
            static const char zeros[512] = {0};
            sb_putn(out, zeros, pad);
        }
    }
    free(data);
    return rc;
}

static int tar_add_tree(strbuf_t *out, const char *fs_path, const char *archive_path,
                        char *err, size_t errsz)
{
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        snprintf(err, errsz, "读取备份目录失败: %s", archive_path);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        char dir_name[256];
        int n = snprintf(dir_name, sizeof(dir_name), "%s/", archive_path);
        if (n < 0 || (size_t)n >= sizeof(dir_name) ||
            tar_header(out, dir_name, '5', 0, path_mtime(fs_path), err, errsz) != 0)
            return -1;
        char **names = NULL;
        int count = 0;
        if (dir_list(fs_path, &names, &count, -1) != 0) {
            snprintf(err, errsz, "读取备份目录失败: %s", archive_path);
            return -1;
        }
        int rc = 0;
        for (int i = 0; i < count; i++) {
            char *child_fs = path_join(fs_path, names[i]);
            char *child_archive = path_join(archive_path, names[i]);
            if (tar_add_tree(out, child_fs, child_archive, err, errsz) != 0)
                rc = -1;
            free(child_fs);
            free(child_archive);
            free(names[i]);
            if (rc != 0)
                break;
        }
        free(names);
        return rc;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errsz, "备份包含不支持的文件类型: %s", archive_path);
        return -1;
    }
    return tar_add_file(out, archive_path, fs_path, err, errsz);
}

static int verify_backup_database(const char *db_path, const char *uploads_path, char *err,
                                  size_t errsz)
{
    sqlite3 *verify = NULL;
    if (sqlite3_open_v2(db_path, &verify, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "无法打开备份数据库");
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
    if (!ok) {
        sqlite3_close(verify);
        snprintf(err, errsz, "备份数据库完整性校验失败");
        return -1;
    }
    if (sqlite3_prepare_v2(verify, "SELECT COUNT(*) FROM account", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) != SQLITE_ROW)
            ok = 0;
        sqlite3_finalize(st);
    } else {
        ok = 0;
    }
    if (ok) {
        if (sqlite3_prepare_v2(verify, "SELECT rel_path FROM note_image", -1, &st, NULL) ==
            SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *rel = (const char *)sqlite3_column_text(st, 0);
                if (!rel || !path_is_safe(rel)) {
                    ok = 0;
                    break;
                }
                char *image = path_join(uploads_path, rel);
                if (!file_exists(image))
                    ok = 0;
                free(image);
                if (!ok)
                    break;
            }
            sqlite3_finalize(st);
        } else {
            ok = 0;
        }
    }
    sqlite3_close(verify);
    if (!ok) {
        snprintf(err, errsz, "备份数据库或图片文件不完整");
        return -1;
    }
    return 0;
}

int backup_export_archive(int64_t backup_id, strbuf_t *out, char *file_name, size_t fname_sz,
                          char *err, size_t errsz)
{
    sqlite3_stmt *st = NULL;
    char dir_name[128] = "";
    if (db_prepare("SELECT dir_name FROM backup_record WHERE id=?1", &st) != 0) {
        snprintf(err, errsz, "读取备份记录失败");
        return -2;
    }
    db_bind_i64(st, 1, backup_id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        snprintf(err, errsz, "备份不存在");
        return -1;
    }
    snprintf(dir_name, sizeof(dir_name), "%s", db_col_text(st, 0));
    sqlite3_finalize(st);
    if (!dir_name[0] || !path_is_safe(dir_name) || strchr(dir_name, '/')) {
        snprintf(err, errsz, "备份名称非法");
        return -2;
    }
    for (const char *p = dir_name; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '-' || *p == '.')) {
            snprintf(err, errsz, "备份名称非法");
            return -2;
        }
    }

    char *dir_path = path_join(g_cfg.backup_dir, dir_name);
    char *db_path = path_join(dir_path, "roomcheck.db");
    char *uploads_path = path_join(dir_path, "uploads");
    char *manifest_path = path_join(dir_path, "manifest.json");
    int rc = -2;
    if (!file_exists(db_path) || !is_dir(uploads_path) || !file_exists(manifest_path)) {
        snprintf(err, errsz, "备份文件不完整");
        goto done;
    }
    if (verify_backup_database(db_path, uploads_path, err, errsz) != 0)
        goto done;

    char *manifest_text = NULL;
    size_t manifest_len = 0;
    if (file_read(manifest_path, &manifest_text, &manifest_len) != 0) {
        snprintf(err, errsz, "读取备份清单失败");
        goto done;
    }
    jval_t *manifest = json_parse(manifest_text, manifest_len);
    free(manifest_text);
    if (!manifest) {
        snprintf(err, errsz, "备份清单损坏");
        goto done;
    }
    const char *database_name = json_get_str(manifest, "database");
    const char *database_sha_field = json_get_str(manifest, "database_sha256");
    int image_files = json_get_int(manifest, "image_files", -1);
    char database_sha[80] = "";
    if (database_sha_field)
        snprintf(database_sha, sizeof(database_sha), "%s", database_sha_field);
    int manifest_ok = database_name && strcmp(database_name, "roomcheck.db") == 0 &&
                      database_sha[0] && strlen(database_sha) == 64 && image_files >= 0;
    json_free(manifest);
    if (!manifest_ok) {
        snprintf(err, errsz, "备份清单内容不完整");
        goto done;
    }

    char *db_data = NULL;
    size_t db_len = 0;
    if (file_read(db_path, &db_data, &db_len) != 0) {
        snprintf(err, errsz, "读取备份数据库失败");
        goto done;
    }
    char db_sha[65];
    rc_sha256_hex(db_data, db_len, db_sha);
    free(db_data);
    if (!str_ieq(db_sha, database_sha)) {
        snprintf(err, errsz, "备份数据库校验失败");
        goto done;
    }
    if (count_files(uploads_path) != image_files) {
        snprintf(err, errsz, "备份图片文件不完整");
        goto done;
    }

    int64_t total = tree_size(dir_path);
    if (total > 0)
        sb_reserve(out, (size_t)total + 4096);
    if (tar_add_file(out, "manifest.json", manifest_path, err, errsz) != 0)
        goto done;
    if (tar_add_file(out, "roomcheck.db", db_path, err, errsz) != 0)
        goto done;
    if (tar_add_tree(out, uploads_path, "uploads", err, errsz) != 0)
        goto done;
    static const char end_blocks[1024] = {0};
    sb_putn(out, end_blocks, sizeof(end_blocks));
    snprintf(file_name, fname_sz, "%s.tar", dir_name);
    rc = 0;

done:
    if (rc != 0)
        sb_reset(out);
    free(dir_path);
    free(db_path);
    free(uploads_path);
    free(manifest_path);
    return rc;
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
    if (ok && auth_ensure_super_admin() != 0) {
        ok = 0;
        snprintf(err, errsz, "恢复后校验超级管理员失败");
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
