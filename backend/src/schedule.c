/*
 * schedule.c - random weekly schedule and incremental (range change) scheduling
 *
 * Rules:
 *  - 6 inspection days: Mon, Tue, Wed, Fri, Sat, Sun (Thursday never scheduled)
 *  - every day has noon + evening groups
 *  - initial generation: even distribution across days, remainder random; then
 *    even distribution between noon/evening, remainder random
 *  - incremental generation: retained rooms never move; added rooms fill group
 *    vacancies released by removed rooms (random match); leftover added rooms are
 *    balanced into the currently lightest day/period groups
 *  - every result is validated before it may be written
 */
#include "common.h"

static const int CHECK_DAYS[6] = {0, 1, 2, 4, 5, 6}; /* 3 = Thursday forbidden */

void assign_free(assign_t *a)
{
    free(a);
}

static void fill_assign(assign_t *a, const char *key, const char *number,
                        int day, int period)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->room_key, sizeof(a->room_key), "%s", key ? key : "");
    snprintf(a->room_number, sizeof(a->room_number), "%s", number ? number : "");
    a->day = day;
    a->period = period;
    a->checked = 0;
    a->checked_at = 0;
}

int schedule_generate_initial(char **room_keys, char **room_numbers, int count, assign_t **out)
{
    *out = NULL;
    if (count <= 0)
        return -1;
    assign_t *assign = xcalloc((size_t)count, sizeof(assign_t));
    int *order = xmalloc((size_t)count * sizeof(int));
    for (int i = 0; i < count; i++)
        order[i] = i;
    rc_shuffle(order, (size_t)count, sizeof(int));

    int base = count / 6;
    int rem = count % 6;
    int day_extra[6] = {0, 0, 0, 0, 0, 0};
    int day_idx[6] = {0, 1, 2, 3, 4, 5};
    rc_shuffle(day_idx, 6, sizeof(int));
    for (int i = 0; i < rem; i++)
        day_extra[day_idx[i]] = 1;

    int pos = 0;
    for (int d = 0; d < 6; d++) {
        int cnt = base + day_extra[d];
        int pbase = cnt / 2;
        int prem = cnt % 2;
        int noon = pbase + (prem ? rc_rand_range(2) : 0);
        for (int k = 0; k < cnt; k++) {
            int period = (k < noon) ? 0 : 1;
            int idx = order[pos++];
            fill_assign(&assign[idx], room_keys[idx], room_numbers[idx], CHECK_DAYS[d], period);
        }
    }
    free(order);
    if (pos != count) {
        free(assign);
        return -1;
    }
    *out = assign;
    return 0;
}

static int cmp_key_ptr(const void *a, const void *b)
{
    const char *ka = *(const char *const *)a;
    const char *kb = *(const char *const *)b;
    return strcmp(ka, kb);
}

static int cmp_assign_key(const void *a, const void *b)
{
    const assign_t *aa = *(const assign_t *const *)a;
    const assign_t *bb = *(const assign_t *const *)b;
    return strcmp(aa->room_key, bb->room_key);
}

static int key_in_sorted(char **sorted, int n, const char *key)
{
    if (!sorted || n <= 0)
        return 0;
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

static assign_t *find_assign_sorted(assign_t **sorted, int n, const char *key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(key, sorted[mid]->room_key);
        if (cmp == 0)
            return sorted[mid];
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

int schedule_generate_incremental(const assign_t *old, int old_count, char **new_keys,
                                  char **new_numbers, int new_count, assign_t **out,
                                  int *out_retained, int *out_added, int *out_removed)
{
    *out = NULL;
    if (out_retained) *out_retained = 0;
    if (out_added) *out_added = 0;
    if (out_removed) *out_removed = 0;
    if (new_count <= 0)
        return -1;

    char **new_sorted = xmalloc((size_t)new_count * sizeof(char *));
    for (int i = 0; i < new_count; i++)
        new_sorted[i] = new_keys[i];
    qsort(new_sorted, (size_t)new_count, sizeof(char *), cmp_key_ptr);

    char **old_sorted = NULL;
    if (old_count > 0) {
        old_sorted = xmalloc((size_t)old_count * sizeof(char *));
        for (int i = 0; i < old_count; i++)
            old_sorted[i] = (char *)old[i].room_key;
        qsort(old_sorted, (size_t)old_count, sizeof(char *), cmp_key_ptr);
    }

    assign_t *result = xcalloc((size_t)new_count, sizeof(assign_t));
    int retained = 0, removed = 0;

    /* 1. keep old rooms that are still in the new set, in their old order */
    for (int i = 0; i < old_count; i++) {
        if (key_in_sorted(new_sorted, new_count, old[i].room_key)) {
            result[retained] = old[i];
            retained++;
        } else {
            removed++;
        }
    }

    /* vacancies produced by removed rooms: {day, period} */
    int vacancy_count = 0;
    int *vac_day = xmalloc((size_t)(old_count > 0 ? old_count : 1) * sizeof(int));
    int *vac_period = xmalloc((size_t)(old_count > 0 ? old_count : 1) * sizeof(int));
    for (int i = 0; i < old_count; i++) {
        if (!key_in_sorted(new_sorted, new_count, old[i].room_key)) {
            vac_day[vacancy_count] = old[i].day;
            vac_period[vacancy_count] = old[i].period;
            vacancy_count++;
        }
    }

    int day_count[7] = {0, 0, 0, 0, 0, 0, 0};
    int period_count[7][2] = {{0}};
    for (int i = 0; i < retained; i++) {
        int d = result[i].day;
        int p = result[i].period;
        if (d >= 0 && d <= 6 && p >= 0 && p <= 1) {
            day_count[d]++;
            period_count[d][p]++;
        }
    }

    /* 2. added rooms fill vacancies first, randomly */
    int added = 0;
    for (int i = 0; i < new_count; i++) {
        if (key_in_sorted(old_sorted, old_count, new_keys[i]))
            continue;
        assign_t *a = &result[retained + added];
        fill_assign(a, new_keys[i], new_numbers[i], 0, 0);
        if (vacancy_count > 0) {
            int pick = rc_rand_range(vacancy_count);
            a->day = vac_day[pick];
            a->period = vac_period[pick];
            vac_day[pick] = vac_day[vacancy_count - 1];
            vac_period[pick] = vac_period[vacancy_count - 1];
            vacancy_count--;
        } else {
            int min_day = 0x7fffffff;
            for (int k = 0; k < 6; k++) {
                int c = day_count[CHECK_DAYS[k]];
                if (c < min_day)
                    min_day = c;
            }
            int candidates[6] = {0, 0, 0, 0, 0, 0};
            int nc = 0;
            for (int k = 0; k < 6; k++) {
                if (day_count[CHECK_DAYS[k]] == min_day)
                    candidates[nc++] = CHECK_DAYS[k];
            }
            int day = candidates[rc_rand_range(nc)];
            int min_p = 0x7fffffff;
            for (int p = 0; p < 2; p++) {
                if (period_count[day][p] < min_p)
                    min_p = period_count[day][p];
            }
            int periods[2] = {0, 0};
            int np = 0;
            for (int p = 0; p < 2; p++) {
                if (period_count[day][p] == min_p)
                    periods[np++] = p;
            }
            a->period = periods[rc_rand_range(np)];
            a->day = day;
        }
        day_count[a->day]++;
        period_count[a->day][a->period]++;
        added++;
    }

    free(vac_day);
    free(vac_period);
    free(new_sorted);
    free(old_sorted);

    if (retained + added != new_count) {
        free(result);
        return -1;
    }
    if (out_retained) *out_retained = retained;
    if (out_added) *out_added = added;
    if (out_removed) *out_removed = removed;
    *out = result;
    return 0;
}

int schedule_validate(char **expected_keys, int expected_count, const assign_t *assign,
                      int assign_count, const assign_t *old, int old_count,
                      char *err, size_t errsz)
{
    if (expected_count != assign_count) {
        snprintf(err, errsz, "排班数量 %d 与应查数 %d 不一致", assign_count, expected_count);
        return 0;
    }
    const assign_t **sorted = xmalloc((size_t)assign_count * sizeof(assign_t *));
    for (int i = 0; i < assign_count; i++)
        sorted[i] = &assign[i];
    qsort(sorted, (size_t)assign_count, sizeof(assign_t *), cmp_assign_key);

    for (int i = 0; i < assign_count; i++) {
        if (i > 0 && strcmp(sorted[i - 1]->room_key, sorted[i]->room_key) == 0) {
            snprintf(err, errsz, "排班存在重复寝室: %s", sorted[i]->room_key);
            free(sorted);
            return 0;
        }
        if (sorted[i]->day == 3) {
            snprintf(err, errsz, "排班包含周四寝室: %s", sorted[i]->room_key);
            free(sorted);
            return 0;
        }
        if (sorted[i]->day < 0 || sorted[i]->day > 6 || !rc_day_is_check_day(sorted[i]->day)) {
            snprintf(err, errsz, "排班日期非法: %s", sorted[i]->room_key);
            free(sorted);
            return 0;
        }
        if (sorted[i]->period != 0 && sorted[i]->period != 1) {
            snprintf(err, errsz, "排班时段非法: %s", sorted[i]->room_key);
            free(sorted);
            return 0;
        }
    }
    for (int i = 0; i < expected_count; i++) {
        if (!find_assign_sorted((assign_t **)sorted, assign_count, expected_keys[i])) {
            snprintf(err, errsz, "排班遗漏寝室: %s", expected_keys[i]);
            free(sorted);
            return 0;
        }
    }
    /* retained rooms must not move */
    for (int i = 0; i < old_count; i++) {
        assign_t *found = find_assign_sorted((assign_t **)sorted, assign_count, old[i].room_key);
        if (found && (old[i].day != found->day || old[i].period != found->period)) {
            snprintf(err, errsz, "保留寝室位置被改变: %s", old[i].room_key);
            free(sorted);
            return 0;
        }
    }
    free(sorted);
    return 1;
}
