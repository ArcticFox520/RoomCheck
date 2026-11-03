/*
 * api_week.c - current week view, regenerate and weekly reset
 */
#include "common.h"

#include <unistd.h>
#include <sys/stat.h>

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int key_contains(char **sorted, int n, const char *key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(key, sorted[mid]);
        if (cmp == 0)
            return 1;
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

static int require_confirm(request_t *r, jval_t *body)
{
    if (!json_get_bool(body, "confirm", 0)) {
        resp_error(r, 400, "confirm_required", "该操作需要二次确认");
        return 0;
    }
    return 1;
}

static void send_week_view(request_t *r, int64_t account_id)
{
    jw_t w;
    jw_init(&w);
    char err[256] = "";
    if (week_build_json(r, account_id, &w, err, sizeof(err)) != 0) {
        jw_free(&w);
        resp_error(r, 500, "week_load_failed", "%s", err[0] ? err : "读取本周数据失败");
        return;
    }
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_week_get(request_t *r)
{
    send_week_view(r, r->account_id);
}

void h_week_regenerate(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!require_confirm(r, body)) {
        json_free(body);
        return;
    }
    int64_t account_id = 0;
    char code[64] = "", msg[256] = "";
    if (!resolve_floor_manager(r, body, &account_id, code, sizeof(code), msg, sizeof(msg))) {
        json_free(body);
        resp_error(r, strcmp(code, "not_found") == 0 ? 404 : 403, code, "%s", msg);
        return;
    }
    room_config_t cfg;
    if (!config_load(account_id, &cfg)) {
        json_free(body);
        resp_error(r, 422, "config_required", "请先完成寝室范围配置");
        return;
    }
    char err[256] = "";
    week_t week;
    int added = 0, removed = 0, retained = 0;
    int rc = -1;
    if (db_begin() == 0) {
        rc = week_rebuild(account_id, "regenerate", 0, &week, &added, &removed, &retained,
                          err, sizeof(err));
        if (rc == 0) {
            if (db_commit() != 0) {
                db_rollback();
                rc = -1;
                snprintf(err, sizeof(err), "提交事务失败");
            }
        } else {
            db_rollback();
        }
    }
    if (rc != 0) {
        json_free(body);
        resp_error(r, 500, "schedule_regenerate_failed", "%s",
                   err[0] ? err : "重新生成排班失败");
        return;
    }
    char target_id[32];
    snprintf(target_id, sizeof(target_id), "%lld", (long long)account_id);
    audit_req(r, "schedule_regenerate", "account", target_id, "success",
              "主动重新生成本周排班（随机），已清空本周勾选，总分配 %d 间", added + removed + retained);
    json_free(body);
    send_week_view(r, account_id);
}

/* ------------------------------------------------------------------ */
/* weekly reset: new plan, migrate notes/images, delete old week data  */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t old_id;
    int64_t new_id;
} note_map_t;

static void week_dir_path(int64_t account_id, int64_t week_id, char *dir_out, size_t dirsz,
                          char *rel_prefix, size_t relsz)
{
    char idbuf[32], widbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", (long long)account_id);
    snprintf(widbuf, sizeof(widbuf), "%lld", (long long)week_id);
    char *p1 = path_join(g_cfg.upload_dir, idbuf);
    char *p2 = path_join(p1, widbuf);
    free(p1);
    snprintf(dir_out, dirsz, "%s", p2);
    if (rel_prefix)
        snprintf(rel_prefix, relsz, "%s/%s", idbuf, widbuf);
    free(p2);
}

static void make_week_dirs(int64_t account_id, int64_t week_id, char *dir_out, size_t dirsz,
                           char *rel_prefix, size_t relsz)
{
    week_dir_path(account_id, week_id, dir_out, dirsz, rel_prefix, relsz);
    mkdir_p(dir_out, 0750);
}

static void safe_ext(const char *rel_path, char *out, size_t outsz)
{
    const char *ext = path_ext(rel_path);
    size_t j = 0;
    for (size_t i = 0; ext[i] && j < outsz - 1 && j < 10; i++) {
        char c = ext[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out[j++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[j] = '\0';
    if (j == 0)
        snprintf(out, outsz, "img");
}

static int copy_week_notes_images(int64_t account_id, int64_t old_week_id,
                                  int64_t new_week_id, char **new_keys, int new_count,
                                  int *notes_copied, int *images_copied,
                                  int *notes_total, int *images_total,
                                  char *err, size_t errsz)
{
    *notes_copied = *images_copied = *notes_total = *images_total = 0;
    if (new_week_id <= 0)
        return 0;

    char **sorted = xmalloc((size_t)new_count * sizeof(char *));
    for (int i = 0; i < new_count; i++)
        sorted[i] = new_keys[i];
    qsort(sorted, (size_t)new_count, sizeof(char *), cmp_str_ptr);

    char new_dir[1200], rel_prefix[600];
    make_week_dirs(account_id, new_week_id, new_dir, sizeof(new_dir), rel_prefix,
                   sizeof(rel_prefix));

    if (old_week_id > 0) {
        note_map_t *maps = NULL;
        int map_count = 0, map_cap = 0;
        sqlite3_stmt *st = NULL;
        if (db_prepare("SELECT id, room_key, room_number, content, created_at, updated_at "
                       "FROM note WHERE week_id=?1 ORDER BY id", &st) == 0) {
            db_bind_i64(st, 1, old_week_id);
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                (*notes_total)++;
                const char *room_key = db_col_text(st, 1);
                if (!key_contains(sorted, new_count, room_key))
                    continue;
                sqlite3_stmt *ins = NULL;
                if (db_prepare("INSERT INTO note(account_id, week_id, room_key, room_number, "
                               "content, created_at, updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7)",
                               &ins) != 0) {
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "复制备注失败");
                    return -1;
                }
                db_bind_i64(ins, 1, account_id);
                db_bind_i64(ins, 2, new_week_id);
                db_bind_text(ins, 3, room_key);
                db_bind_text(ins, 4, db_col_text(st, 2));
                db_bind_text(ins, 5, db_col_text(st, 3));
                db_bind_i64(ins, 6, db_col_i64(st, 4));
                db_bind_i64(ins, 7, db_col_i64(st, 5));
                int ok = db_step_done(ins) == 0;
                sqlite3_finalize(ins);
                if (!ok) {
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "复制备注失败");
                    return -1;
                }
                if (map_count == map_cap) {
                    map_cap = map_cap ? map_cap * 2 : 16;
                    maps = realloc(maps, (size_t)map_cap * sizeof(note_map_t));
                    if (!maps)
                        abort();
                }
                maps[map_count].old_id = db_col_i64(st, 0);
                maps[map_count].new_id = db_last_id();
                map_count++;
                (*notes_copied)++;
            }
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(st);
                free(maps);
                free(sorted);
                snprintf(err, errsz, "读取旧备注失败");
                return -1;
            }
            sqlite3_finalize(st);
        }

        if (db_prepare("SELECT id, note_id, room_key, room_number, rel_path, original_name, "
                       "mime, size, sha256, created_at FROM note_image WHERE week_id=?1 "
                       "ORDER BY id", &st) == 0) {
            db_bind_i64(st, 1, old_week_id);
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                (*images_total)++;
                const char *room_key = db_col_text(st, 2);
                if (!key_contains(sorted, new_count, room_key))
                    continue;
                int64_t old_note_id = 0;
                if (sqlite3_column_type(st, 1) != SQLITE_NULL)
                    old_note_id = db_col_i64(st, 1);
                int64_t new_note_id = 0;
                if (old_note_id > 0) {
                    for (int i = 0; i < map_count; i++) {
                        if (maps[i].old_id == old_note_id) {
                            new_note_id = maps[i].new_id;
                            break;
                        }
                    }
                    if (new_note_id == 0)
                        continue; /* note not migrated */
                }
                const char *old_rel = db_col_text(st, 4);
                if (!path_is_safe(old_rel)) {
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "图片路径非法");
                    return -1;
                }
                char *src = path_join(g_cfg.upload_dir, old_rel);
                char ext[16];
                safe_ext(old_rel, ext, sizeof(ext));
                char fname[64];
                random_hex(fname, 32);
                char *dst = path_join(new_dir, fname);
                char *dst_ext = xmalloc(strlen(dst) + strlen(ext) + 2);
                sprintf(dst_ext, "%s.%s", dst, ext);
                free(dst);
                dst = dst_ext;
                if (copy_file(src, dst) != 0) {
                    free(src);
                    free(dst);
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "复制图片文件失败");
                    return -1;
                }
                free(src);
                char rel[1200];
                snprintf(rel, sizeof(rel), "%s/%s.%s", rel_prefix, fname, ext);
                sqlite3_stmt *ins = NULL;
                if (db_prepare("INSERT INTO note_image(note_id, account_id, week_id, room_key, "
                               "room_number, rel_path, original_name, mime, size, sha256, "
                               "created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                               &ins) != 0) {
                    free(dst);
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "复制图片记录失败");
                    return -1;
                }
                if (new_note_id > 0)
                    db_bind_i64(ins, 1, new_note_id);
                else
                    db_bind_null(ins, 1);
                db_bind_i64(ins, 2, account_id);
                db_bind_i64(ins, 3, new_week_id);
                db_bind_text(ins, 4, room_key);
                db_bind_text(ins, 5, db_col_text(st, 3));
                db_bind_text(ins, 6, rel);
                db_bind_text(ins, 7, db_col_text(st, 5));
                db_bind_text(ins, 8, db_col_text(st, 6));
                db_bind_i64(ins, 9, db_col_i64(st, 7));
                db_bind_text(ins, 10, db_col_text(st, 8));
                db_bind_i64(ins, 11, db_col_i64(st, 9));
                int ok = db_step_done(ins) == 0;
                sqlite3_finalize(ins);
                free(dst);
                if (!ok) {
                    sqlite3_finalize(st);
                    free(maps);
                    free(sorted);
                    snprintf(err, errsz, "复制图片记录失败");
                    return -1;
                }
                (*images_copied)++;
            }
            sqlite3_finalize(st);
        }
        free(maps);
    }
    free(sorted);
    return 0;
}

void h_week_reset(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    if (!require_confirm(r, body)) {
        json_free(body);
        return;
    }
    int64_t account_id = 0;
    char code[64] = "", msg[256] = "";
    if (!resolve_floor_manager(r, body, &account_id, code, sizeof(code), msg, sizeof(msg))) {
        json_free(body);
        resp_error(r, strcmp(code, "not_found") == 0 ? 404 : 403, code, "%s", msg);
        return;
    }
    room_config_t cfg;
    if (!config_load(account_id, &cfg)) {
        json_free(body);
        resp_error(r, 422, "config_required", "请先完成寝室范围配置");
        return;
    }
    char **numbers = NULL, **keys = NULL;
    int count = 0;
    if (build_room_list(&cfg, &numbers, &keys, &count) != 0 || count <= 0) {
        json_free(body);
        resp_error(r, 422, "config_invalid", "无法生成有效寝室集合");
        return;
    }
    assign_t *assign = NULL;
    if (schedule_generate_initial(keys, numbers, count, &assign) != 0) {
        free_string_list(numbers, count);
        free_string_list(keys, count);
        json_free(body);
        resp_error(r, 500, "schedule_generate_failed", "新周排班生成失败");
        return;
    }
    char verr[256] = "";
    if (!schedule_validate(keys, count, assign, count, NULL, 0, verr, sizeof(verr))) {
        free(assign);
        free_string_list(numbers, count);
        free_string_list(keys, count);
        json_free(body);
        resp_error(r, 500, "schedule_validate_failed", "%s", verr);
        return;
    }

    week_t old_week;
    int has_old = week_get_active(account_id, &old_week);
    int64_t old_week_id = has_old ? old_week.id : 0;

    char err[256] = "";
    int notes_copied = 0, images_copied = 0, notes_total = 0, images_total = 0;
    int64_t new_week_id = 0;
    int ok = 0;
    if (db_begin() == 0) {
        new_week_id = create_week_plan(account_id, rc_week_start(rc_now()), "reset");
        if (new_week_id < 0 || ensure_groups(new_week_id) != 0 ||
            write_assignments(new_week_id, account_id, cfg.building_name, assign, count, 1) != 0) {
            snprintf(err, sizeof(err), "创建新周排班失败");
        } else if (copy_week_notes_images(account_id, old_week_id, new_week_id, keys, count,
                                          &notes_copied, &images_copied, &notes_total,
                                          &images_total, err, sizeof(err)) != 0) {
            /* already set */
        } else if (old_week_id > 0) {
            sqlite3_stmt *st = NULL;
            if (db_prepare("DELETE FROM week_plan WHERE id=?1", &st) != 0 ||
                (db_bind_i64(st, 1, old_week_id), db_step_done(st) != 0)) {
                if (st)
                    sqlite3_finalize(st);
                snprintf(err, sizeof(err), "清除旧周数据失败");
            } else {
                sqlite3_finalize(st);
                ok = 1;
            }
        } else {
            ok = 1;
        }
        if (ok && db_commit() != 0) {
            ok = 0;
            snprintf(err, sizeof(err), "提交事务失败");
        }
        if (!ok) {
            db_rollback();
        }
    } else {
        snprintf(err, sizeof(err), "数据库繁忙，请稍后重试");
    }

    if (!ok) {
        char new_dir[1200];
        if (new_week_id > 0) {
            week_dir_path(account_id, new_week_id, new_dir, sizeof(new_dir), NULL, 0);
            remove_tree(new_dir);
        }
        free(assign);
        free_string_list(numbers, count);
        free_string_list(keys, count);
        json_free(body);
        resp_error(r, 500, "week_reset_failed", "%s", err[0] ? err : "重置本周失败");
        return;
    }

    /* remove old week image files after successful migration */
    if (old_week_id > 0) {
        char old_dir[1200];
        week_dir_path(account_id, old_week_id, old_dir, sizeof(old_dir), NULL, 0);
        if (remove_tree(old_dir) != 0) {
            audit_req(r, "week_reset", "account", "", "warning",
                      "旧周图片目录清理失败，需要人工检查: %s", old_dir);
        }
    }
    char detail[512];
    snprintf(detail, sizeof(detail),
             "开始新一周（随机排班），应查 %d 间；迁移备注 %d/%d、图片 %d/%d，删除旧周排班与勾选",
             count, notes_copied, notes_total, images_copied, images_total);
    char target_id[32];
    snprintf(target_id, sizeof(target_id), "%lld", (long long)account_id);
    audit_req(r, "week_reset", "account", target_id, "success", "%s", detail);

    free(assign);
    free_string_list(numbers, count);
    free_string_list(keys, count);
    json_free(body);
    send_week_view(r, account_id);
}
