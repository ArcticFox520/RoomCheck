/*
 * api_config.c - GET/PUT room range configuration
 */
#include "common.h"

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int same_key_set(char **a, int na, char **b, int nb)
{
    if (na != nb)
        return 0;
    if (na == 0)
        return 1;
    char **ca = xmalloc((size_t)na * sizeof(char *));
    char **cb = xmalloc((size_t)nb * sizeof(char *));
    memcpy(ca, a, (size_t)na * sizeof(char *));
    memcpy(cb, b, (size_t)nb * sizeof(char *));
    qsort(ca, (size_t)na, sizeof(char *), cmp_str_ptr);
    qsort(cb, (size_t)nb, sizeof(char *), cmp_str_ptr);
    int same = 1;
    for (int i = 0; i < na; i++) {
        if (strcmp(ca[i], cb[i]) != 0) {
            same = 0;
            break;
        }
    }
    free(ca);
    free(cb);
    return same;
}

static void config_response(request_t *r, int64_t account_id, const room_config_t *cfg,
                            char **excluded, int excluded_count, int added, int removed,
                            int retained, int week_updated, const char *plan_type)
{
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "config");
    config_json(&w, cfg, excluded, excluded_count);
    jw_key(&w, "schedule");
    jw_obj(&w);
    jw_key(&w, "added");
    jw_int(&w, added);
    jw_key(&w, "removed");
    jw_int(&w, removed);
    jw_key(&w, "retained");
    jw_int(&w, retained);
    jw_key(&w, "N");
    jw_i64(&w, cfg->original_count - excluded_count);
    jw_key(&w, "week_updated");
    jw_bool(&w, week_updated);
    jw_key(&w, "plan_type");
    jw_str(&w, plan_type ? plan_type : "");
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    (void)account_id;
}

void h_config_get(request_t *r)
{
    int64_t account_id = r->account_id;
    if (is_admin_request(r)) {
        const char *qid = req_query(r, "account_id");
        if (!qid) {
            resp_error(r, 400, "account_required", "管理员查询必须指定楼层长账户");
            return;
        }
        if (!parse_i64(qid, &account_id) ||
            !require_manager_account(r, account_id)) {
            resp_error(r, 404, "not_found", "楼层长账户不存在");
            return;
        }
    }
    room_config_t cfg;
    if (!config_load(account_id, &cfg)) {
        resp_json(r, 200, "{\"config\":null}");
        return;
    }
    char **excluded = NULL;
    int excluded_count = 0;
    config_load_excluded(account_id, &excluded, &excluded_count);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "config");
    config_json(&w, &cfg, excluded, excluded_count);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
    free_string_list(excluded, excluded_count);
}

void h_config_put(request_t *r)
{
    jval_t *body = req_json(r);
    if (!body)
        return;
    int64_t account_id = 0;
    char code[64] = "", msg[256] = "";
    if (!resolve_floor_manager(r, body, &account_id, code, sizeof(code), msg, sizeof(msg))) {
        json_free(body);
        resp_error(r, strcmp(code, "not_found") == 0 ? 404 : 403, code, "%s", msg);
        return;
    }
    const char *building = json_get_str(body, "building_name");
    const char *start = json_get_str(body, "start_room");
    const char *end = json_get_str(body, "end_room");
    if (!building || !start || !end) {
        json_free(body);
        resp_error(r, 400, "invalid_range", "请填写楼栋名称、起始和结束寝室号");
        return;
    }
    jval_t *arr = json_obj_get(body, "excluded_rooms");
    if (arr && arr->type != JARR && arr->type != JNULL) {
        json_free(body);
        resp_error(r, 400, "invalid_range", "无需查寝列表格式错误");
        return;
    }
    int arr_count = json_arr_count(arr);
    if (arr_count > RC_MAX_ROOMS) {
        json_free(body);
        resp_error(r, 422, "invalid_range", "无需查寝寝室数量过多");
        return;
    }
    char **excluded = xcalloc((size_t)(arr_count > 0 ? arr_count : 1), sizeof(char *));
    int excluded_count = 0;
    for (int i = 0; i < arr_count; i++) {
        jval_t *item = json_arr_get(arr, i);
        if (!item || item->type != JSTR) {
            free_string_list(excluded, excluded_count);
            json_free(body);
            resp_error(r, 400, "invalid_range", "无需查寝列表必须为字符串数组");
            return;
        }
        excluded[excluded_count++] = xstrdup(item->u.str);
    }

    room_config_t cfg;
    char verr_code[64] = "", verr_msg[256] = "";
    if (!config_validate_rooms(building, start, end, &excluded, excluded_count, &cfg,
                               verr_code, sizeof(verr_code), verr_msg, sizeof(verr_msg))) {
        free_string_list(excluded, excluded_count);
        json_free(body);
        resp_error(r, 422, verr_code, "%s", verr_msg);
        return;
    }
    excluded_count = (int)cfg.excluded_count;
    cfg.account_id = account_id;

    account_t target;
    if (!auth_load_account(account_id, &target)) {
        free_string_list(excluded, excluded_count);
        json_free(body);
        resp_error(r, 404, "not_found", "楼层长账户不存在");
        return;
    }
    if (strcmp(target.status, "deleted") == 0) {
        free_string_list(excluded, excluded_count);
        json_free(body);
        resp_error(r, 409, "account_deleted", "已删除账户不能修改配置");
        return;
    }

    room_config_t old_cfg;
    int had_old = config_load(account_id, &old_cfg);
    char **old_keys = NULL;
    int old_count = 0;
    if (had_old)
        build_room_list(&old_cfg, NULL, &old_keys, &old_count);

    int week_updated = 0, added = 0, removed = 0, retained = 0;
    char plan_type[24] = "";
    char serr[256] = "";
    int ok = 0;
    int http_status = 500;
    char fail_msg[256] = "保存失败";

    if (db_begin() != 0) {
        fail_msg[0] = '\0';
        snprintf(fail_msg, sizeof(fail_msg), "数据库繁忙，请稍后重试");
    } else {
        int failed = 0;
        if (config_save(account_id, &cfg, excluded, excluded_count) != 0) {
            failed = 1;
            snprintf(fail_msg, sizeof(fail_msg), "保存配置失败");
        }
        char **new_keys = NULL;
        int new_count = 0;
        if (!failed) {
            cfg.updated_at = rc_now();
            if (build_room_list(&cfg, NULL, &new_keys, &new_count) != 0) {
                failed = 1;
                snprintf(fail_msg, sizeof(fail_msg), "生成寝室集合失败");
            }
        }
        if (!failed) {
            week_t existing_week;
            int has_plan = week_get_active(account_id, &existing_week);
            int changed = !had_old || !has_plan ||
                          !same_key_set(old_keys, old_count, new_keys, new_count);
            if (changed) {
                week_t week;
                if (!had_old) {
                    if (week_create_full(account_id, &cfg, "initial", &week, serr,
                                         sizeof(serr)) != 0) {
                        failed = 1;
                        snprintf(fail_msg, sizeof(fail_msg), "%s", serr);
                    } else {
                        week_updated = 1;
                        added = new_count;
                        retained = 0;
                        snprintf(plan_type, sizeof(plan_type), "initial");
                    }
                } else {
                    if (week_rebuild(account_id, "incremental", 1, &week, &added, &removed,
                                     &retained, serr, sizeof(serr)) != 0) {
                        failed = 1;
                        snprintf(fail_msg, sizeof(fail_msg), "%s", serr);
                    } else {
                        week_updated = 1;
                        snprintf(plan_type, sizeof(plan_type), "incremental");
                    }
                }
            } else {
                week_t week;
                if (week_get_active(account_id, &week))
                    snprintf(plan_type, sizeof(plan_type), "%s", week.plan_type);
                retained = new_count;
            }
        }
        free_string_list(new_keys, new_count);
        if (failed) {
            db_rollback();
            http_status = 422;
        } else if (db_commit() != 0) {
            failed = 1;
            db_rollback();
            snprintf(fail_msg, sizeof(fail_msg), "提交事务失败");
            http_status = 500;
        }
        ok = !failed;
    }
    free_string_list(old_keys, old_count);

    if (!ok) {
        free_string_list(excluded, excluded_count);
        json_free(body);
        resp_error(r, http_status, "config_save_failed", "%s", fail_msg);
        return;
    }

    char detail[512];
    if (is_admin_request(r) && account_id != r->account_id) {
        snprintf(detail, sizeof(detail),
                 "管理员修改楼层长 %s 的寝室范围：%s %s-%s，N=%lld，新增 %d 移出 %d 保留 %d%s",
                 target.username, cfg.building_name, cfg.start_room, cfg.end_room,
                 (long long)(cfg.original_count - excluded_count), added, removed, retained,
                 week_updated ? "，已增量更新本周排班（随机）" : "");
    } else {
        snprintf(detail, sizeof(detail),
                 "修改寝室范围：%s %s-%s，N=%lld，新增 %d 移出 %d 保留 %d%s",
                 cfg.building_name, cfg.start_room, cfg.end_room,
                 (long long)(cfg.original_count - excluded_count), added, removed, retained,
                 week_updated ? "，已增量更新本周排班（随机）" : "");
    }
    char target_id[32];
    snprintf(target_id, sizeof(target_id), "%lld", (long long)account_id);
    audit_req(r, "config_update", "account", target_id, "success", "%s", detail);

    config_response(r, account_id, &cfg, excluded, excluded_count, added, removed, retained,
                    week_updated, plan_type[0] ? plan_type : "unchanged");
    free_string_list(excluded, excluded_count);
    json_free(body);
}
