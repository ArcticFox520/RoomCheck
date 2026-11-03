/*
 * main.c - RoomCheck entry point
 */
#include "common.h"

#include <sys/stat.h>
#include <unistd.h>

app_config_t g_cfg;
volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
    http_stop();
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (!end || *end || n <= 0 || n > 65535)
        return def;
    return (int)n;
}

static void env_str(const char *name, const char *def, char *out, size_t outsz)
{
    const char *v = getenv(name);
    if (v && *v)
        snprintf(out, outsz, "%s", v);
    else
        snprintf(out, outsz, "%s", def);
}

static void print_usage(const char *argv0)
{
    printf("RoomCheck %s - 寝室抽查管理系统\n", RC_VERSION);
    printf("用法: %s [--version] [--selftest] [--help]\n", argv0);
    printf("\n环境变量:\n");
    printf("  PORT / ROOMCHECK_PORT      监听端口（默认 8080）\n");
    printf("  ROOMCHECK_DATA_DIR         数据目录（默认 ./data）\n");
    printf("  ROOMCHECK_DB_PATH          数据库路径（默认 <DATA_DIR>/roomcheck.db）\n");
    printf("  ROOMCHECK_UPLOAD_DIR       图片目录（默认 <DATA_DIR>/uploads）\n");
    printf("  ROOMCHECK_BACKUP_DIR       备份目录（默认 <DATA_DIR>/backups）\n");
    printf("  ROOMCHECK_WEB_ROOT         前端目录（默认 ./frontend）\n");
    printf("  ADMIN_RESET=1              重置管理员密码并输出凭据\n");
    printf("  ADMIN_INIT_PASSWORD        指定初始管理员密码（默认随机生成）\n");
    printf("  ROOMCHECK_COOKIE_SECURE=1  Session Cookie 增加 Secure 标记\n");
    printf("  ROOMCHECK_DISABLE_BACKUP=1 关闭每日自动备份\n");
    printf("  ROOMCHECK_DISABLE_SCHEDULER=1 关闭后台调度（仅供测试）\n");
}

static void make_default_paths(void)
{
    env_str("ROOMCHECK_DATA_DIR", "./data", g_cfg.data_dir, sizeof(g_cfg.data_dir));
    char *db = path_join(g_cfg.data_dir, "roomcheck.db");
    char *uploads = path_join(g_cfg.data_dir, "uploads");
    char *backups = path_join(g_cfg.data_dir, "backups");
    env_str("ROOMCHECK_DB_PATH", db, g_cfg.db_path, sizeof(g_cfg.db_path));
    env_str("ROOMCHECK_UPLOAD_DIR", uploads, g_cfg.upload_dir, sizeof(g_cfg.upload_dir));
    env_str("ROOMCHECK_BACKUP_DIR", backups, g_cfg.backup_dir, sizeof(g_cfg.backup_dir));
    free(db);
    free(uploads);
    free(backups);
    env_str("ROOMCHECK_WEB_ROOT", "./frontend", g_cfg.web_root, sizeof(g_cfg.web_root));
    int port = env_int("PORT", 0);
    if (port == 0)
        port = env_int("ROOMCHECK_PORT", 8080);
    g_cfg.port = port;
    g_cfg.cookie_secure = getenv("ROOMCHECK_COOKIE_SECURE") &&
                                  strcmp(getenv("ROOMCHECK_COOKIE_SECURE"), "1") == 0;
    g_cfg.scheduler_disabled = getenv("ROOMCHECK_DISABLE_SCHEDULER") &&
                               strcmp(getenv("ROOMCHECK_DISABLE_SCHEDULER"), "1") == 0;
}

static int run_selftest(void)
{
    printf("RoomCheck %s selftest\n", RC_VERSION);
    printf("SQLite: %s\n", sqlite3_libversion());
    char hex[65];
    rc_sha256_hex("roomcheck", 9, hex);
    if (strlen(hex) != 64) {
        fprintf(stderr, "SHA-256 自检失败\n");
        return 1;
    }
    char *hash = password_hash("selftest");
    int ok = password_verify("selftest", hash) && !password_verify("wrong", hash);
    secure_wipe(hash, strlen(hash));
    free(hash);
    if (!ok) {
        fprintf(stderr, "密码哈希自检失败\n");
        return 1;
    }
    printf("selftest: ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("RoomCheck %s (SQLite %s)\n", RC_VERSION, sqlite3_libversion());
            return 0;
        }
        if (strcmp(argv[i], "--selftest") == 0)
            return run_selftest();
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        fprintf(stderr, "未知参数: %s\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    memset(&g_cfg, 0, sizeof(g_cfg));
    make_default_paths();

    if (mkdir_p(g_cfg.data_dir, 0750) != 0) {
        fprintf(stderr, "无法创建数据目录 %s: %s\n", g_cfg.data_dir, strerror(errno));
        return 1;
    }
    if (mkdir_p(g_cfg.upload_dir, 0750) != 0) {
        fprintf(stderr, "无法创建图片目录 %s: %s\n", g_cfg.upload_dir, strerror(errno));
        return 1;
    }
    if (mkdir_p(g_cfg.backup_dir, 0750) != 0) {
        fprintf(stderr, "无法创建备份目录 %s: %s\n", g_cfg.backup_dir, strerror(errno));
        return 1;
    }
    if (!is_dir(g_cfg.web_root))
        fprintf(stderr, "WARNING: 前端目录不存在: %s\n", g_cfg.web_root);

    printf("RoomCheck %s 启动中\n", RC_VERSION);
    printf("  数据目录: %s\n", g_cfg.data_dir);
    printf("  数据库:   %s\n", g_cfg.db_path);
    printf("  图片目录: %s\n", g_cfg.upload_dir);
    printf("  备份目录: %s\n", g_cfg.backup_dir);
    printf("  前端目录: %s\n", g_cfg.web_root);

    db_lock();
    if (db_init(g_cfg.db_path) != 0) {
        db_unlock();
        fprintf(stderr, "数据库初始化失败: %s\n", db_error());
        return 1;
    }
    if (auth_ensure_admin() != 0) {
        db_unlock();
        db_close_conn();
        return 1;
    }
    db_unlock();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!g_cfg.scheduler_disabled) {
        if (scheduler_start() != 0)
            fprintf(stderr, "WARNING: 后台调度线程启动失败\n");
    } else {
        printf("后台调度已禁用（测试模式）\n");
    }

    int rc = http_server_run(g_cfg.port);
    g_shutdown = 1;
    http_stop();
    if (g_cfg.scheduler_disabled == 0)
        scheduler_stop();
    http_wait_idle(5000);
    db_close_conn();
    printf("RoomCheck 已停止\n");
    return rc == 0 ? 0 : 1;
}
