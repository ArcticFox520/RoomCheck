/*
 * RoomCheck - 寝室抽查管理系统
 * common.h - shared types, constants and helpers
 */
#ifndef RC_COMMON_H
#define RC_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sqlite3.h>

#define RC_VERSION "1.0.0"
#define RC_APP_NAME "RoomCheck"

#define RC_SESSION_COOKIE "rc_session"
#define RC_SESSION_TTL (7LL * 24 * 3600)
#define RC_DELETE_DAYS 72
#define RC_DELETE_SECONDS (RC_DELETE_DAYS * 24 * 3600)
#define RC_MAX_ROOMS 20000
#define RC_MAX_BODY (12u * 1024 * 1024)
#define RC_MAX_NOTE_CHARS 500
#define RC_MAX_IMAGES 50
#define RC_MAX_IMAGE_BYTES (10 * 1024 * 1024)
#define RC_MAX_HEADERS 48
#define RC_MAX_PARAMS 6

/* ------------------------------------------------------------------ */
/* string buffer                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

void sb_init(strbuf_t *b);
void sb_free(strbuf_t *b);
void sb_reset(strbuf_t *b);
void sb_reserve(strbuf_t *b, size_t extra);
void sb_putc(strbuf_t *b, char c);
void sb_putn(strbuf_t *b, const char *s, size_t n);
void sb_puts(strbuf_t *b, const char *s);
void sb_printf(strbuf_t *b, const char *fmt, ...);
char *sb_steal(strbuf_t *b);
const char *sb_cstr(strbuf_t *b);

/* ------------------------------------------------------------------ */
/* global configuration                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int port;
    char data_dir[1024];
    char db_path[1024];
    char upload_dir[1024];
    char backup_dir[1024];
    char web_root[1024];
    int cookie_secure;
    int scheduler_disabled;
    int workers;
} app_config_t;

extern app_config_t g_cfg;
extern volatile sig_atomic_t g_shutdown;

/* ------------------------------------------------------------------ */
/* utilities                                                           */
/* ------------------------------------------------------------------ */
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
char *trim_dup(const char *s);
void trim_inplace(char *s);
char *lower_dup(const char *s);
int str_ieq(const char *a, const char *b);
int str_starts_with(const char *s, const char *prefix);
int str_ends_with(const char *s, const char *suffix);
int parse_i64(const char *s, int64_t *out);
int is_all_digits(const char *s);
int utf8_count(const char *s);
size_t utf8_prefix_bytes(const char *s, int max_chars);
char *format_size(int64_t bytes);
char *duration_text(int64_t seconds);
char *json_escape(const char *s); /* malloc */
void secure_wipe(void *p, size_t n);
int secure_eq(const char *a, const char *b);

/* random */
void rc_random_bytes(void *buf, size_t n);
uint64_t rc_rand64(void);
int rc_rand_range(int n); /* 0..n-1 */
void rc_shuffle(void *base, size_t n, size_t width);
void random_hex(char *out, size_t hex_chars);
char *random_password(size_t len);

/* hash / password */
void rc_sha256(const void *data, size_t len, unsigned char out[32]);
void rc_sha256_hex(const void *data, size_t len, char out[65]);
char *password_hash(const char *password);
int password_verify(const char *password, const char *encoded);

/* time (Asia/Shanghai, UTC+8, no DST) */
int64_t rc_now(void);
int64_t rc_week_start(int64_t ts);
int64_t rc_week_end(int64_t ts);
int64_t rc_day_start(int64_t ts);
int64_t rc_date_start(const char *date); /* YYYY-MM-DD -> epoch, -1 on error */
int rc_iso_week(int64_t week_start, int *iso_year, int *iso_week);
void rc_fmt_datetime(int64_t ts, char *out, size_t outsz);
void rc_fmt_date(int64_t ts, char *out, size_t outsz);
void rc_fmt_time(int64_t ts, char *out, size_t outsz);
void rc_fmt_month_day(int64_t ts, char *out, size_t outsz);
void rc_fmt_week_range(int64_t week_start, int64_t week_end, char *out, size_t outsz);
int rc_weekday(int day_index); /* day 0..6 -> 0=Sun..6=Sat */
const char *rc_weekday_name(int day_index);
int rc_day_is_check_day(int day_index); /* 0=Mon .. 6=Sun, Thu=3 forbidden */
int rc_same_day(int64_t a, int64_t b);

/* url helpers */
char *url_decode(const char *s);
char *url_encode(const char *s);

/* filesystem */
int file_exists(const char *path);
int is_dir(const char *path);
int mkdir_p(const char *path, int mode);
int file_read(const char *path, char **data, size_t *len);
int file_write(const char *path, const void *data, size_t len);
int copy_file(const char *src, const char *dst);
int copy_tree(const char *src, const char *dst);
int remove_tree(const char *path);
int dir_list(const char *path, char ***names, int *count, int files_only);
char *path_join(const char *a, const char *b);
const char *path_basename(const char *path);
const char *path_ext(const char *path);
int path_is_safe(const char *path);

/* ------------------------------------------------------------------ */
/* minimal JSON parser                                                 */
/* ------------------------------------------------------------------ */
typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype_t;

typedef struct jval jval_t;
struct jval {
    jtype_t type;
    union {
        int b;
        double num;
        char *str;
        struct {
            jval_t **items;
            int count;
            int cap;
        } arr;
        struct {
            char **keys;
            jval_t **vals;
            int count;
            int cap;
        } obj;
    } u;
};

jval_t *json_parse(const char *s, size_t len);
void json_free(jval_t *v);
jval_t *json_obj_get(jval_t *obj, const char *key);
const char *json_get_str(jval_t *obj, const char *key);
int64_t json_get_i64(jval_t *obj, const char *key, int64_t def);
int json_get_int(jval_t *obj, const char *key, int def);
int json_get_bool(jval_t *obj, const char *key, int def);
int json_arr_count(jval_t *arr);
jval_t *json_arr_get(jval_t *arr, int idx);

/* ------------------------------------------------------------------ */
/* JSON writer                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    strbuf_t b;
    int depth;
    unsigned char is_obj[32];
    unsigned char first[32];
    int after_key;
} jw_t;

void jw_init(jw_t *w);
void jw_free(jw_t *w);
void jw_obj(jw_t *w);
void jw_arr(jw_t *w);
void jw_end(jw_t *w);
void jw_key(jw_t *w, const char *key);
void jw_str(jw_t *w, const char *s);
void jw_strn(jw_t *w, const char *s, size_t n);
void jw_i64(jw_t *w, int64_t v);
void jw_int(jw_t *w, int v);
void jw_bool(jw_t *w, int v);
void jw_null(jw_t *w);
void jw_raw(jw_t *w, const char *raw);

/* ------------------------------------------------------------------ */
/* HTTP request / response                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char *key;
    char *value;
} kv_t;

typedef struct request {
    int fd;
    char method[16];
    char *target;
    char *path;
    char *query;
    kv_t query_params[32];
    int query_count;
    kv_t headers[RC_MAX_HEADERS];
    int header_count;
    char *body;
    size_t body_len;
    char remote_ip[64];
    char *params[RC_MAX_PARAMS];
    int param_count;

    int authed;
    int64_t account_id;
    char role[16];
    char username[128];
    char display_name[128];
    int must_change_password;
    char session_hash[80];
    char csrf_token[80];
    int64_t expires_at;

    int status;
    strbuf_t out;
    char content_type[160];
    char set_cookie[320];
    char extra_headers[512];
    int no_store;
    int is_html;
} request_t;

#define RF_AUTH 0x01
#define RF_ADMIN 0x02
#define RF_PUBLIC 0x04
#define RF_SUPER_ADMIN 0x08

typedef void (*handler_fn)(request_t *r);

typedef struct {
    const char *method;
    const char *pattern;
    handler_fn handler;
    unsigned flags;
} route_t;

/* http.c */
int http_server_run(int port);
void http_stop(void);
void http_wait_idle(int timeout_ms);
int http_parse_request(int fd, request_t *r);
void http_send_response(request_t *r);
void resp_json(request_t *r, int status, const char *json);
void resp_error(request_t *r, int status, const char *code, const char *fmt, ...);
void resp_ok(request_t *r);
int req_param_i64(request_t *r, int idx, int64_t *out);
const char *req_query(request_t *r, const char *key);
const char *req_header(request_t *r, const char *name);
jval_t *req_json(request_t *r);
void req_audit_ip(request_t *r, char *out, size_t outsz);

extern const route_t g_routes[];
extern const char *g_spa_routes[];

/* db.c */
int db_init(const char *path);
void db_close_conn(void);
sqlite3 *db_handle(void);
void db_lock(void);
void db_unlock(void);
int db_exec(const char *sql);
int db_prepare(const char *sql, sqlite3_stmt **st);
int db_step_done(sqlite3_stmt *st);
int db_bind_text(sqlite3_stmt *st, int idx, const char *v);
int db_bind_text_nullable(sqlite3_stmt *st, int idx, const char *v);
int db_bind_i64(sqlite3_stmt *st, int idx, int64_t v);
int db_bind_null(sqlite3_stmt *st, int idx);
int64_t db_last_id(void);
int db_changes(void);
const char *db_error(void);
int db_begin(void);
int db_commit(void);
int db_rollback(void);
const char *db_col_text(sqlite3_stmt *st, int col);
int64_t db_col_i64(sqlite3_stmt *st, int col);

/* audit.c */
int audit_write(const char *actor_type, int64_t actor_id, const char *actor_name,
                const char *action, const char *target_type, const char *target_id,
                const char *result, const char *detail, const char *ip);
int audit_req(request_t *r, const char *action, const char *target_type,
              const char *target_id, const char *result, const char *fmt, ...);

/* auth.c */
typedef struct {
    int64_t id;
    char role[16];
    char username[128];
    char display_name[128];
    int must_change_password;
    int super_admin;
    char status[16];
    int64_t created_at;
    int64_t last_login_at;
    int64_t deleted_at;
    int64_t delete_at;
} account_t;

int auth_find_account(const char *username, account_t *out);
int auth_load_account(int64_t id, account_t *out);
int auth_create_session(int64_t account_id, const char *role, char *token_out,
                        char *csrf_out, size_t csrf_sz, int64_t *expires_out);
int auth_validate_session(const char *token, account_t *out, char *csrf_out,
                          size_t csrf_sz, int64_t *expires_out, char *session_hash);
void auth_revoke_session(const char *session_hash);
void auth_revoke_all_sessions(int64_t account_id, const char *except_hash);
int auth_login_allowed(const char *username, char *err_code, size_t code_sz,
                       char *err_msg, size_t msg_sz, int *retry_after);
int auth_record_login_failure(const char *username);
void auth_record_login_success(const char *username);
int auth_ensure_admin(void);
int auth_ensure_super_admin(void);
void auth_set_session_cookie(request_t *r, const char *token, int64_t max_age);
void auth_clear_session_cookie(request_t *r);
void hash_session_token(const char *token, char out[65]);
int auth_role_is_admin(request_t *r);

/* config.c */
typedef struct {
    int64_t account_id;
    char building_name[128];
    char start_room[32];
    char end_room[32];
    int64_t start_num;
    int64_t end_num;
    int64_t original_count;
    int64_t excluded_count;
    int64_t updated_at;
} room_config_t;

int config_load(int64_t account_id, room_config_t *out);
int config_load_excluded(int64_t account_id, char ***out, int *count);
int build_room_list(const room_config_t *cfg, char ***out_numbers, char ***out_keys, int *count);
void free_string_list(char **list, int count);
int room_number_canonical(const room_config_t *cfg, const char *input, char *out, size_t outsz);
int config_validate_rooms(const char *building, const char *start, const char *end,
                          char ***excluded, int excluded_count, room_config_t *out,
                          char *err_code, size_t code_sz, char *err_msg, size_t msg_sz);
int config_save(int64_t account_id, const room_config_t *cfg, char **excluded, int excluded_count);

/* schedule.c */
typedef struct {
    char room_key[192];
    char room_number[32];
    int day;
    int period; /* 0 noon, 1 evening */
    int checked;
    int64_t checked_at;
} assign_t;

int schedule_generate_initial(char **room_keys, char **room_numbers, int count, assign_t **out);
int schedule_generate_incremental(const assign_t *old, int old_count, char **new_keys,
                                  char **new_numbers, int new_count, assign_t **out,
                                  int *out_retained, int *out_added, int *out_removed);
int schedule_validate(char **expected_keys, int expected_count, const assign_t *assign,
                      int assign_count, const assign_t *old, int old_count, char *err, size_t errsz);
void assign_free(assign_t *a);

/* week.c */
typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t week_start;
    int64_t week_end;
    int iso_year;
    int iso_week;
    int64_t created_at;
    int64_t updated_at;
    char plan_type[24];
} week_t;

int week_get_active(int64_t account_id, week_t *out);
int week_create_full(int64_t account_id, const room_config_t *cfg, const char *plan_type,
                     week_t *out, char *err, size_t errsz);
int week_rebuild(int64_t account_id, const char *plan_type, int incremental,
                 week_t *out, int *added, int *removed, int *retained, char *err, size_t errsz);
int week_build_json(request_t *r, int64_t account_id, jw_t *out, char *err, size_t errsz);
int week_progress(int64_t week_id, int *checked, int *total);
void week_free(week_t *w);

/* backup.c */
int backup_run(const char *prefix, int keep, char *out_dir, size_t outsz, char *err, size_t errsz);
int backup_restore(int64_t backup_id, char *err, size_t errsz);
int backup_export_archive(int64_t backup_id, strbuf_t *out, char *file_name, size_t fname_sz,
                          char *err, size_t errsz);
int64_t backup_dir_size(const char *path);

/* scheduler.c */
int scheduler_start(void);
void scheduler_stop(void);

/* handlers */
void h_health(request_t *r);
void h_login(request_t *r);
void h_logout(request_t *r);
void h_session(request_t *r);
void h_password(request_t *r);
void h_profile(request_t *r);
void h_config_get(request_t *r);
void h_config_put(request_t *r);
void h_week_get(request_t *r);
void h_week_regenerate(request_t *r);
void h_week_reset(request_t *r);
void h_check(request_t *r);
void h_uncheck(request_t *r);
void h_notes_list(request_t *r);
void h_note_create(request_t *r);
void h_note_update(request_t *r);
void h_note_delete(request_t *r);
void h_images_list(request_t *r);
void h_image_upload(request_t *r);
void h_image_file(request_t *r);
void h_image_delete(request_t *r);
void h_admin_accounts(request_t *r);
void h_admin_account_create(request_t *r);
void h_admin_account_update(request_t *r);
void h_admin_account_enable(request_t *r);
void h_admin_account_disable(request_t *r);
void h_admin_account_delete(request_t *r);
void h_admin_account_restore(request_t *r);
void h_admin_account_week(request_t *r);
void h_admin_admins(request_t *r);
void h_admin_admin_create(request_t *r);
void h_admin_admin_update(request_t *r);
void h_admin_admin_enable(request_t *r);
void h_admin_admin_disable(request_t *r);
void h_admin_admin_delete(request_t *r);
void h_admin_admin_restore(request_t *r);
void h_admin_overview(request_t *r);
void h_admin_logs(request_t *r);
void h_admin_backups(request_t *r);
void h_admin_backup_create(request_t *r);
void h_admin_backup_restore(request_t *r);
void h_admin_backup_export(request_t *r);
void h_admin_orphans(request_t *r);

/* helpers shared by handlers (api_common.c) */
typedef struct {
    int64_t id;
    int64_t account_id;
    int64_t week_id;
    int64_t group_id;
    char room_key[192];
    char room_number[32];
    int checked;
    int64_t checked_at;
} room_row_t;

int load_room(request_t *r, int64_t room_id, room_row_t *out, char *err, size_t errsz);
int load_note(request_t *r, int64_t note_id, room_row_t *room, int64_t *note_id_out,
              char *err, size_t errsz);
int load_image(request_t *r, int64_t image_id, room_row_t *room, char *rel_path,
               size_t rel_sz, char *mime, size_t mime_sz, char *original_name,
               size_t name_sz, char *err, size_t errsz);
int note_total_chars(int64_t week_id, const char *room_key);
int64_t create_week_plan(int64_t account_id, int64_t week_start, const char *plan_type);
int ensure_groups(int64_t week_id);
int write_assignments(int64_t week_id, int64_t account_id, const char *building_name,
                      const assign_t *assign, int count, int clear_checks);
int resolve_floor_manager(request_t *r, jval_t *body, int64_t *account_id,
                          char *code, size_t code_sz, char *msg, size_t msg_sz);
void progress_json(jw_t *out, int checked, int total);
void config_json(jw_t *out, const room_config_t *cfg, char **excluded, int excluded_count);
int account_json(jw_t *out, const account_t *a);
int is_admin_request(request_t *r);
int require_manager_account(request_t *r, int64_t account_id);
int room_permission(request_t *r, int64_t owner_account_id);


#endif /* RC_COMMON_H */
