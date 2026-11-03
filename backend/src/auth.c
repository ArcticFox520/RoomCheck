/*
 * auth.c - accounts, password verification, sessions, login lockout
 */
#include "common.h"

#include <ctype.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* account queries                                                     */
/* ------------------------------------------------------------------ */
static void account_from_row(sqlite3_stmt *st, account_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = db_col_i64(st, 0);
    snprintf(out->role, sizeof(out->role), "%s", db_col_text(st, 1));
    snprintf(out->username, sizeof(out->username), "%s", db_col_text(st, 2));
    snprintf(out->display_name, sizeof(out->display_name), "%s", db_col_text(st, 3));
    out->must_change_password = sqlite3_column_int(st, 4);
    snprintf(out->status, sizeof(out->status), "%s", db_col_text(st, 5));
    out->created_at = db_col_i64(st, 6);
    out->last_login_at = db_col_i64(st, 7);
    out->deleted_at = db_col_i64(st, 8);
    out->delete_at = db_col_i64(st, 9);
}

static const char *k_account_cols =
    "id, role, username, display_name, must_change_password, status, created_at, "
    "last_login_at, deleted_at, delete_at";

int auth_find_account(const char *username, account_t *out)
{
    if (!username || !*username)
        return 0;
    char *norm = lower_dup(username);
    trim_inplace(norm);
    sqlite3_stmt *st = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT %s FROM account WHERE username_norm=?1", k_account_cols);
    int found = 0;
    if (db_prepare(sql, &st) == 0) {
        db_bind_text(st, 1, norm);
        if (sqlite3_step(st) == SQLITE_ROW) {
            account_from_row(st, out);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    free(norm);
    return found;
}

int auth_load_account(int64_t id, account_t *out)
{
    sqlite3_stmt *st = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT %s FROM account WHERE id=?1", k_account_cols);
    int found = 0;
    if (db_prepare(sql, &st) == 0) {
        db_bind_i64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            account_from_row(st, out);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    return found;
}

int auth_role_is_admin(request_t *r)
{
    return r && r->authed && strcmp(r->role, "admin") == 0;
}

/* ------------------------------------------------------------------ */
/* session                                                             */
/* ------------------------------------------------------------------ */
void hash_session_token(const char *token, char out[65])
{
    rc_sha256_hex(token ? token : "", strlen(token ? token : ""), out);
}

int auth_create_session(int64_t account_id, const char *role, char *token_out,
                        char *csrf_out, size_t csrf_sz, int64_t *expires_out)
{
    char token[65], csrf[65], hash[65];
    random_hex(token, 64);
    random_hex(csrf, 64);
    hash_session_token(token, hash);
    int64_t now = rc_now();
    int64_t expires = now + RC_SESSION_TTL;
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (db_prepare("INSERT INTO session(token_hash, account_id, role, csrf_token, "
                   "created_at, expires_at, revoked, ip) VALUES(?1,?2,?3,?4,?5,?6,0,'')",
                   &st) == 0) {
        db_bind_text(st, 1, hash);
        db_bind_i64(st, 2, account_id);
        db_bind_text(st, 3, role);
        db_bind_text(st, 4, csrf);
        db_bind_i64(st, 5, now);
        db_bind_i64(st, 6, expires);
        ok = db_step_done(st) == 0;
        sqlite3_finalize(st);
    }
    if (ok) {
        snprintf(token_out, 65, "%s", token);
        snprintf(csrf_out, csrf_sz, "%s", csrf);
        if (expires_out)
            *expires_out = expires;
    }
    return ok;
}

int auth_validate_session(const char *token, account_t *out, char *csrf_out,
                          size_t csrf_sz, int64_t *expires_out, char *session_hash)
{
    if (!token || !*token)
        return 0;
    char hash[65];
    hash_session_token(token, hash);
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT s.account_id, s.role, s.csrf_token, s.expires_at, s.revoked, "
        "a.id, a.role, a.username, a.display_name, a.must_change_password, a.status, "
        "a.created_at, a.last_login_at, a.deleted_at, a.delete_at "
        "FROM session s JOIN account a ON a.id=s.account_id WHERE s.token_hash=?1";
    int ok = 0;
    if (db_prepare(sql, &st) == 0) {
        db_bind_text(st, 1, hash);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int64_t account_id = db_col_i64(st, 0);
            const char *sess_role = db_col_text(st, 1);
            const char *csrf = db_col_text(st, 2);
            int64_t expires = db_col_i64(st, 3);
            int revoked = sqlite3_column_int(st, 4);
            const char *acct_role = db_col_text(st, 6);
            const char *username = db_col_text(st, 7);
            const char *display_name = db_col_text(st, 8);
            int must_change = sqlite3_column_int(st, 9);
            const char *status = db_col_text(st, 10);
            int64_t now = rc_now();
            if (!revoked && expires > now && strcmp(status, "enabled") == 0 &&
                strcmp(sess_role, acct_role) == 0) {
                memset(out, 0, sizeof(*out));
                out->id = account_id;
                snprintf(out->role, sizeof(out->role), "%s", acct_role);
                snprintf(out->username, sizeof(out->username), "%s", username);
                snprintf(out->display_name, sizeof(out->display_name), "%s", display_name);
                out->must_change_password = must_change;
                snprintf(out->status, sizeof(out->status), "%s", status);
                out->created_at = db_col_i64(st, 11);
                out->last_login_at = db_col_i64(st, 12);
                out->deleted_at = db_col_i64(st, 13);
                out->delete_at = db_col_i64(st, 14);
                snprintf(csrf_out, csrf_sz, "%s", csrf);
                if (expires_out)
                    *expires_out = expires;
                if (session_hash)
                    snprintf(session_hash, 80, "%s", hash);
                ok = 1;
            }
        }
        sqlite3_finalize(st);
    }
    return ok;
}

void auth_revoke_session(const char *session_hash)
{
    if (!session_hash)
        return;
    sqlite3_stmt *st = NULL;
    if (db_prepare("DELETE FROM session WHERE token_hash=?1", &st) == 0) {
        db_bind_text(st, 1, session_hash);
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

void auth_revoke_all_sessions(int64_t account_id, const char *except_hash)
{
    sqlite3_stmt *st = NULL;
    const char *sql = except_hash
                          ? "DELETE FROM session WHERE account_id=?1 AND token_hash<>?2"
                          : "DELETE FROM session WHERE account_id=?1";
    if (db_prepare(sql, &st) == 0) {
        db_bind_i64(st, 1, account_id);
        if (except_hash)
            db_bind_text(st, 2, except_hash);
        db_step_done(st);
        sqlite3_finalize(st);
    }
}

void auth_set_session_cookie(request_t *r, const char *token, int64_t max_age)
{
    snprintf(r->set_cookie, sizeof(r->set_cookie),
             RC_SESSION_COOKIE "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%lld%s",
             token, (long long)max_age, g_cfg.cookie_secure ? "; Secure" : "");
}

void auth_clear_session_cookie(request_t *r)
{
    snprintf(r->set_cookie, sizeof(r->set_cookie),
             RC_SESSION_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s",
             g_cfg.cookie_secure ? "; Secure" : "");
}

/* ------------------------------------------------------------------ */
/* login lockout (memory only)                                         */
/* ------------------------------------------------------------------ */
#define LOCK_MAX_FAILURES 5
#define LOCK_SECONDS (10 * 60)
#define LOCK_SLOTS 256

typedef struct {
    char key[300];
    int failures;
    int64_t lock_until;
    int64_t last_seen;
} lock_entry_t;

static lock_entry_t g_locks[LOCK_SLOTS];
static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static void lock_key(const char *username, char *out, size_t outsz)
{
    char *norm = lower_dup(username ? username : "");
    trim_inplace(norm);
    snprintf(out, outsz, "acct:%s", norm);
    free(norm);
}

static lock_entry_t *lock_find(const char *key, int create)
{
    int free_slot = -1;
    int oldest_slot = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < LOCK_SLOTS; i++) {
        if (g_locks[i].key[0] && strcmp(g_locks[i].key, key) == 0)
            return &g_locks[i];
        if (!g_locks[i].key[0] && free_slot < 0)
            free_slot = i;
        if (g_locks[i].last_seen < oldest) {
            oldest = g_locks[i].last_seen;
            oldest_slot = i;
        }
    }
    if (!create)
        return NULL;
    int slot = free_slot >= 0 ? free_slot : oldest_slot;
    memset(&g_locks[slot], 0, sizeof(g_locks[slot]));
    snprintf(g_locks[slot].key, sizeof(g_locks[slot].key), "%s", key);
    return &g_locks[slot];
}

int auth_login_allowed(const char *username, char *err_code, size_t code_sz,
                       char *err_msg, size_t msg_sz, int *retry_after)
{
    char key[300];
    lock_key(username, key, sizeof(key));
    int64_t now = rc_now();
    pthread_mutex_lock(&g_lock_mutex);
    lock_entry_t *e = lock_find(key, 0);
    int allowed = 1;
    if (e && e->lock_until > now) {
        int64_t remain = e->lock_until - now;
        if (err_code)
            snprintf(err_code, code_sz, "account_locked");
        if (err_msg)
            snprintf(err_msg, msg_sz, "登录失败次数过多，账户已锁定，请 %lld 分钟后重试",
                     (long long)((remain + 59) / 60));
        if (retry_after)
            *retry_after = (int)remain;
        allowed = 0;
    }
    pthread_mutex_unlock(&g_lock_mutex);
    return allowed;
}

int auth_record_login_failure(const char *username)
{
    char key[300];
    lock_key(username, key, sizeof(key));
    int64_t now = rc_now();
    int locked_now = 0;
    pthread_mutex_lock(&g_lock_mutex);
    lock_entry_t *e = lock_find(key, 1);
    if (e) {
        e->last_seen = now;
        if (e->lock_until <= now) {
            e->failures++;
            if (e->failures >= LOCK_MAX_FAILURES) {
                e->lock_until = now + LOCK_SECONDS;
                e->failures = LOCK_MAX_FAILURES;
                locked_now = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_lock_mutex);
    return locked_now;
}

void auth_record_login_success(const char *username)
{
    char key[300];
    lock_key(username, key, sizeof(key));
    pthread_mutex_lock(&g_lock_mutex);
    lock_entry_t *e = lock_find(key, 0);
    if (e)
        memset(e, 0, sizeof(*e));
    pthread_mutex_unlock(&g_lock_mutex);
}

/* ------------------------------------------------------------------ */
/* admin bootstrap                                                     */
/* ------------------------------------------------------------------ */
static int count_admins(void)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (db_prepare("SELECT COUNT(*) FROM account WHERE role='admin'", &st) == 0) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

int auth_ensure_admin(void)
{
    const char *reset_env = getenv("ADMIN_RESET");
    int do_reset = reset_env && strcmp(reset_env, "1") == 0;
    int admins = count_admins();
    if (admins > 0 && !do_reset)
        return 0;

    account_t existing;
    memset(&existing, 0, sizeof(existing));
    int has_existing = 0;
    if (admins > 0) {
        sqlite3_stmt *st = NULL;
        if (db_prepare("SELECT username FROM account WHERE role='admin' ORDER BY id LIMIT 1",
                       &st) == 0) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                snprintf(existing.username, sizeof(existing.username), "%s",
                         db_col_text(st, 0));
                has_existing = 1;
            }
            sqlite3_finalize(st);
        }
    }

    char username[128];
    if (has_existing)
        snprintf(username, sizeof(username), "%s", existing.username);
    else
        snprintf(username, sizeof(username), "admin");

    const char *env_pw = getenv("ADMIN_INIT_PASSWORD");
    char *password = NULL;
    int generated = 0;
    if (env_pw && strlen(env_pw) >= 4) {
        password = xstrdup(env_pw);
    } else {
        password = random_password(14);
        generated = 1;
    }
    char *hash = password_hash(password);
    char *norm = lower_dup(username);
    int64_t now = rc_now();
    int ok = 0;

    if (has_existing) {
        sqlite3_stmt *st = NULL;
        if (db_prepare("UPDATE account SET password_hash=?1, must_change_password=1, "
                       "status='enabled', deleted_at=NULL, delete_at=NULL, updated_at=?2 "
                       "WHERE role='admin' AND username_norm=?3",
                       &st) == 0) {
            db_bind_text(st, 1, hash);
            db_bind_i64(st, 2, now);
            db_bind_text(st, 3, norm);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
        if (ok) {
            sqlite3_stmt *q = NULL;
            if (db_prepare("SELECT id FROM account WHERE role='admin' LIMIT 1", &q) == 0) {
                if (sqlite3_step(q) == SQLITE_ROW)
                    auth_revoke_all_sessions(db_col_i64(q, 0), NULL);
                sqlite3_finalize(q);
            }
        }
    } else {
        sqlite3_stmt *st = NULL;
        if (db_prepare("INSERT INTO account(role, username, username_norm, password_hash, "
                       "must_change_password, status, display_name, created_at, updated_at) "
                       "VALUES('admin',?1,?2,?3,1,'enabled','系统管理员',?4,?4)",
                       &st) == 0) {
            db_bind_text(st, 1, username);
            db_bind_text(st, 2, norm);
            db_bind_text(st, 3, hash);
            db_bind_i64(st, 4, now);
            ok = db_step_done(st) == 0;
            sqlite3_finalize(st);
        }
    }
    free(norm);
    free(hash);

    if (!ok) {
        fprintf(stderr, "初始化管理员账户失败: %s\n", db_error());
        secure_wipe(password, strlen(password));
        free(password);
        return -1;
    }

    printf("\n");
    printf("============================================================\n");
    printf("  RoomCheck 管理员账户已%s\n", has_existing ? "重置" : "初始化");
    printf("  账户名: %s\n", username);
    printf("  密码:   %s\n", password);
    printf("  首次登录必须修改密码。此凭据仅在本次输出中显示。\n");
    if (generated)
        printf("  密码由系统随机生成，请立即妥善保存。\n");
    printf("============================================================\n\n");
    fflush(stdout);

    secure_wipe(password, strlen(password));
    free(password);
    return 0;
}
