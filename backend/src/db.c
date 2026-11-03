/*
 * db.c - SQLite connection, schema and small helpers
 */
#include "common.h"

#include <unistd.h>
#include <sys/stat.h>

static sqlite3 *g_db = NULL;
static char g_db_error[512] = "";
static pthread_mutex_t g_db_mutex;
static pthread_once_t g_db_mutex_once = PTHREAD_ONCE_INIT;

static void db_mutex_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void db_mutex_ensure(void)
{
    pthread_once(&g_db_mutex_once, db_mutex_init);
}

static const char *k_schema =
    "CREATE TABLE IF NOT EXISTS account ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  role TEXT NOT NULL CHECK (role IN ('admin','floor_manager')),"
    "  username TEXT NOT NULL,"
    "  username_norm TEXT NOT NULL UNIQUE,"
    "  password_hash TEXT NOT NULL,"
    "  must_change_password INTEGER NOT NULL DEFAULT 0,"
    "  status TEXT NOT NULL DEFAULT 'enabled' CHECK (status IN ('enabled','disabled','deleted')),"
    "  deleted_at INTEGER,"
    "  delete_at INTEGER,"
    "  display_name TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  last_login_at INTEGER"
    ");"

    "CREATE TABLE IF NOT EXISTS floor_config ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL UNIQUE REFERENCES account(id) ON DELETE CASCADE,"
    "  building_name TEXT NOT NULL,"
    "  start_room TEXT NOT NULL,"
    "  end_room TEXT NOT NULL,"
    "  start_num INTEGER NOT NULL,"
    "  end_num INTEGER NOT NULL,"
    "  original_count INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS excluded_room ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  room_number TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(account_id, room_number)"
    ");"

    "CREATE TABLE IF NOT EXISTS week_plan ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  week_start INTEGER NOT NULL,"
    "  week_end INTEGER NOT NULL,"
    "  iso_year INTEGER NOT NULL,"
    "  iso_week INTEGER NOT NULL,"
    "  plan_type TEXT NOT NULL DEFAULT 'initial',"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS week_plan_account_idx ON week_plan(account_id);"

    "CREATE TABLE IF NOT EXISTS check_group ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  week_id INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,"
    "  day_index INTEGER NOT NULL CHECK (day_index BETWEEN 0 AND 6),"
    "  period TEXT NOT NULL CHECK (period IN ('noon','evening')),"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(week_id, day_index, period)"
    ");"

    "CREATE TABLE IF NOT EXISTS room_check ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  week_id INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,"
    "  group_id INTEGER NOT NULL REFERENCES check_group(id) ON DELETE CASCADE,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  room_key TEXT NOT NULL,"
    "  room_number TEXT NOT NULL,"
    "  position INTEGER NOT NULL DEFAULT 0,"
    "  checked INTEGER NOT NULL DEFAULT 0,"
    "  checked_at INTEGER,"
    "  created_at INTEGER NOT NULL,"
    "  UNIQUE(week_id, room_key)"
    ");"
    "CREATE INDEX IF NOT EXISTS room_check_group_idx ON room_check(group_id, position);"
    "CREATE INDEX IF NOT EXISTS room_check_week_idx ON room_check(week_id);"

    "CREATE TABLE IF NOT EXISTS note ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  week_id INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,"
    "  room_key TEXT NOT NULL,"
    "  room_number TEXT NOT NULL,"
    "  content TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS note_room_idx ON note(week_id, room_key);"

    "CREATE TABLE IF NOT EXISTS note_image ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  note_id INTEGER REFERENCES note(id) ON DELETE CASCADE,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  week_id INTEGER NOT NULL REFERENCES week_plan(id) ON DELETE CASCADE,"
    "  room_key TEXT NOT NULL,"
    "  room_number TEXT NOT NULL,"
    "  rel_path TEXT NOT NULL,"
    "  original_name TEXT NOT NULL,"
    "  mime TEXT NOT NULL,"
    "  size INTEGER NOT NULL,"
    "  sha256 TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS note_image_room_idx ON note_image(week_id, room_key);"
    "CREATE INDEX IF NOT EXISTS note_image_note_idx ON note_image(note_id);"

    "CREATE TABLE IF NOT EXISTS session ("
    "  token_hash TEXT PRIMARY KEY,"
    "  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,"
    "  role TEXT NOT NULL,"
    "  csrf_token TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL,"
    "  revoked INTEGER NOT NULL DEFAULT 0,"
    "  ip TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX IF NOT EXISTS session_account_idx ON session(account_id);"

    "CREATE TABLE IF NOT EXISTS audit_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  actor_type TEXT NOT NULL,"
    "  actor_id INTEGER,"
    "  actor_name TEXT NOT NULL DEFAULT '',"
    "  action TEXT NOT NULL,"
    "  target_type TEXT NOT NULL DEFAULT '',"
    "  target_id TEXT NOT NULL DEFAULT '',"
    "  detail TEXT NOT NULL DEFAULT '',"
    "  result TEXT NOT NULL DEFAULT 'success',"
    "  ip TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS audit_created_idx ON audit_log(created_at);"
    "CREATE INDEX IF NOT EXISTS audit_action_idx ON audit_log(action);"

    "CREATE TABLE IF NOT EXISTS deletion_task ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL UNIQUE,"
    "  status TEXT NOT NULL DEFAULT 'pending',"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  last_error TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS backup_record ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  dir_name TEXT NOT NULL UNIQUE,"
    "  path TEXT NOT NULL,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  status TEXT NOT NULL DEFAULT 'success',"
    "  created_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");";

int db_init(const char *path)
{
    db_mutex_ensure();
    pthread_mutex_lock(&g_db_mutex);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    int rc = sqlite3_open_v2(path, &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        snprintf(g_db_error, sizeof(g_db_error), "打开数据库失败: %s",
                 g_db ? sqlite3_errmsg(g_db) : "unknown");
        if (g_db) {
            sqlite3_close(g_db);
            g_db = NULL;
        }
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    sqlite3_busy_timeout(g_db, 5000);
    char *errmsg = NULL;
    const char *pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA temp_store=MEMORY;";
    if (sqlite3_exec(g_db, pragmas, NULL, NULL, &errmsg) != SQLITE_OK) {
        snprintf(g_db_error, sizeof(g_db_error), "数据库设置失败: %s",
                 errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    if (sqlite3_exec(g_db, k_schema, NULL, NULL, &errmsg) != SQLITE_OK) {
        snprintf(g_db_error, sizeof(g_db_error), "数据库初始化失败: %s",
                 errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = NULL;
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int version = 0;
    if (sqlite3_prepare_v2(g_db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            version = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (version == 0)
        sqlite3_exec(g_db, "PRAGMA user_version=1", NULL, NULL, NULL);
    snprintf(g_db_error, sizeof(g_db_error), "ok");
    pthread_mutex_unlock(&g_db_mutex);
    return 0;
}

void db_close_conn(void)
{
    db_mutex_ensure();
    pthread_mutex_lock(&g_db_mutex);
    if (g_db) {
        sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        sqlite3_close(g_db);
        g_db = NULL;
    }
    pthread_mutex_unlock(&g_db_mutex);
}

sqlite3 *db_handle(void)
{
    return g_db;
}

void db_lock(void)
{
    db_mutex_ensure();
    pthread_mutex_lock(&g_db_mutex);
}

void db_unlock(void)
{
    pthread_mutex_unlock(&g_db_mutex);
}

const char *db_error(void)
{
    return g_db_error;
}

int db_exec(const char *sql)
{
    if (!g_db) {
        snprintf(g_db_error, sizeof(g_db_error), "数据库未打开");
        return -1;
    }
    char *errmsg = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        snprintf(g_db_error, sizeof(g_db_error), "%s", errmsg ? errmsg : "SQL 执行失败");
        sqlite3_free(errmsg);
        return -1;
    }
    snprintf(g_db_error, sizeof(g_db_error), "ok");
    return 0;
}

int db_prepare(const char *sql, sqlite3_stmt **st)
{
    *st = NULL;
    if (!g_db) {
        snprintf(g_db_error, sizeof(g_db_error), "数据库未打开");
        return -1;
    }
    if (sqlite3_prepare_v2(g_db, sql, -1, st, NULL) != SQLITE_OK) {
        snprintf(g_db_error, sizeof(g_db_error), "%s", sqlite3_errmsg(g_db));
        *st = NULL;
        return -1;
    }
    return 0;
}

int db_step_done(sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        snprintf(g_db_error, sizeof(g_db_error), "%s", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

int db_bind_text(sqlite3_stmt *st, int idx, const char *v)
{
    return sqlite3_bind_text(st, idx, v ? v : "", -1, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int db_bind_text_nullable(sqlite3_stmt *st, int idx, const char *v)
{
    if (!v)
        return sqlite3_bind_null(st, idx) == SQLITE_OK ? 0 : -1;
    return db_bind_text(st, idx, v);
}

int db_bind_i64(sqlite3_stmt *st, int idx, int64_t v)
{
    return sqlite3_bind_int64(st, idx, v) == SQLITE_OK ? 0 : -1;
}

int db_bind_null(sqlite3_stmt *st, int idx)
{
    return sqlite3_bind_null(st, idx) == SQLITE_OK ? 0 : -1;
}

int64_t db_last_id(void)
{
    return sqlite3_last_insert_rowid(g_db);
}

int db_changes(void)
{
    return sqlite3_changes(g_db);
}

int db_begin(void)
{
    return db_exec("BEGIN IMMEDIATE");
}

int db_commit(void)
{
    return db_exec("COMMIT");
}

int db_rollback(void)
{
    return db_exec("ROLLBACK");
}

const char *db_col_text(sqlite3_stmt *st, int col)
{
    const unsigned char *p = sqlite3_column_text(st, col);
    return p ? (const char *)p : "";
}

int64_t db_col_i64(sqlite3_stmt *st, int col)
{
    return sqlite3_column_int64(st, col);
}
