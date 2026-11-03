/*
 * api_image.c - image list, multipart upload, original download, delete
 *
 * Upload security layers: extension blacklist, MIME blacklist, file signature
 * inspection. All three are required by the specification.
 */
#include "common.h"

#include <ctype.h>
#include <unistd.h>

static const char *k_blocked_ext[] = {
    "exe", "dll", "com", "scr", "sh", "bash", "bat", "cmd", "ps1", "elf", "bin",
    "msi", "deb", "rpm", "apk", "jar", "war", "class", "app", "dmg", "pkg", "py",
    "php", "pl", "rb", "html", "htm", "js", "mjs", "svg", "docm", "xlsm", "pptm",
    NULL};

static const char *k_blocked_mime[] = {
    "application/x-msdownload", "application/x-msdos-program",
    "application/x-executable", "application/x-elf", "application/x-sh",
    "application/x-shellscript", "application/x-php", "application/x-python",
    "application/x-httpd-php", "text/html", "text/javascript",
    "application/javascript", "application/x-javascript", "image/svg+xml",
    "application/java-archive", "application/vnd.android.package-archive",
    "application/x-apple-diskimage", "application/vnd.microsoft.portable-executable",
    "application/x-msi", "application/x-rpm", "application/x-deb", NULL};

typedef struct {
    const char *data;
    size_t len;
    char filename[256];
    char content_type[128];
    int64_t note_id;
    int has_file;
} upload_t;

static int str_in_list(const char *value, const char **list)
{
    for (const char **p = list; *p; p++)
        if (str_ieq(value, *p))
            return 1;
    return 0;
}

static void sanitize_mime(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (in) {
        for (const char *p = in; *p && *p != ';' && j < outsz - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7F && c != '\r' && c != '\n')
                out[j++] = (char)tolower(c);
        }
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
}

static void utf8_sanitize(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in)
        in = "";
    const unsigned char *p = (const unsigned char *)in;
    while (*p && j + 4 < outsz) {
        unsigned char c = *p;
        size_t need = 0;
        if (c < 0x80) {
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        } else {
            out[j++] = '_';
            p++;
            continue;
        }
        int valid = 1;
        for (size_t k = 1; k < need; k++) {
            if ((p[k] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            out[j++] = '_';
            p++;
            continue;
        }
        if (need == 1 && (c < 0x20 || c == 0x7F)) {
            out[j++] = '_';
            p++;
            continue;
        }
        for (size_t k = 0; k < need; k++)
            out[j++] = (char)p[k];
        p += need;
    }
    out[j] = '\0';
}

static int detect_image_mime(const unsigned char *d, size_t n, char *out, size_t outsz)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        snprintf(out, outsz, "image/jpeg");
        return 1;
    }
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        snprintf(out, outsz, "image/png");
        return 1;
    }
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        snprintf(out, outsz, "image/gif");
        return 1;
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        snprintf(out, outsz, "image/webp");
        return 1;
    }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') {
        snprintf(out, outsz, "image/bmp");
        return 1;
    }
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0) ||
                   (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 0x2A))) {
        snprintf(out, outsz, "image/tiff");
        return 1;
    }
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        if (memcmp(d + 8, "avif", 4) == 0) {
            snprintf(out, outsz, "image/avif");
            return 1;
        }
        if (memcmp(d + 8, "heic", 4) == 0 || memcmp(d + 8, "heix", 4) == 0 ||
            memcmp(d + 8, "mif1", 4) == 0 || memcmp(d + 8, "msf1", 4) == 0) {
            snprintf(out, outsz, "image/heic");
            return 1;
        }
    }
    if (n >= 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x01 && d[3] == 0x00) {
        snprintf(out, outsz, "image/x-icon");
        return 1;
    }
    return 0;
}

static int magic_forbidden(const unsigned char *d, size_t n)
{
    if (n >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')
        return 1;
    if (n >= 2 && d[0] == 'M' && d[1] == 'Z')
        return 1;
    if (n >= 4 && ((d[0] == 0xFE && d[1] == 0xED && d[2] == 0xFA && d[3] == 0xCE) ||
                   (d[0] == 0xFE && d[1] == 0xED && d[2] == 0xFA && d[3] == 0xCF) ||
                   (d[0] == 0xCE && d[1] == 0xFA && d[2] == 0xED && d[3] == 0xFE) ||
                   (d[0] == 0xCF && d[1] == 0xFA && d[2] == 0xED && d[3] == 0xFE)))
        return 1;
    if (n >= 4 && d[0] == 0xCA && d[1] == 0xFE && d[2] == 0xBA && d[3] == 0xBE)
        return 1; /* Java class / Mach-O fat */
    if (n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 0x03 && d[3] == 0x04)
        return 1; /* zip based packages */
    if (n >= 6 && memcmp(d, "7z\xBC\xAF\x27\x1C", 6) == 0)
        return 1;
    if (n >= 4 && memcmp(d, "Rar!", 4) == 0)
        return 1;
    if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B)
        return 1;
    if (n >= 2 && d[0] == '#' && d[1] == '!')
        return 1;
    size_t i = 0;
    while (i < n && i < 64 && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' ||
                               d[i] == '\n' || d[i] == 0xEF || d[i] == 0xBB ||
                               d[i] == 0xBF))
        i++;
    if (i + 5 <= n && strncasecmp((const char *)d + i, "<?php", 5) == 0)
        return 1;
    if (i + 5 <= n && strncasecmp((const char *)d + i, "<html", 5) == 0)
        return 1;
    if (i + 4 <= n && strncasecmp((const char *)d + i, "<svg", 4) == 0)
        return 1;
    if (i + 9 <= n && strncasecmp((const char *)d + i, "<!doctype", 9) == 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* multipart/form-data                                                 */
/* ------------------------------------------------------------------ */
static int find_boundary(const char *ct, char *out, size_t outsz)
{
    if (!ct)
        return 0;
    const char *p = strcasestr(ct, "boundary=");
    if (!p)
        return 0;
    p += 9;
    while (*p == ' ' || *p == '\t')
        p++;
    char quote = 0;
    if (*p == '"' || *p == '\'')
        quote = *p++;
    const char *end = p;
    while (*end && (quote ? *end != quote : *end != ';' && *end != ' ')) {
        if ((unsigned char)*end < 0x20)
            return 0;
        end++;
    }
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= outsz)
        return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int parse_part_headers(const char *headers, size_t hlen, char *field, size_t fieldsz,
                              char *filename, size_t filenamesz, char *ctype, size_t ctypesz)
{
    char *copy = xstrndup(headers, hlen);
    field[0] = '\0';
    filename[0] = '\0';
    ctype[0] = '\0';
    char *save = NULL;
    char *line = strtok_r(copy, "\r\n", &save);
    while (line) {
        if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
            char *n = strcasestr(line, "name=\"");
            if (n) {
                n += 6;
                char *e = strchr(n, '"');
                if (e && (size_t)(e - n) < fieldsz) {
                    memcpy(field, n, (size_t)(e - n));
                    field[e - n] = '\0';
                }
            }
            char *f = strcasestr(line, "filename=\"");
            if (f) {
                f += 10;
                char *e = strchr(f, '"');
                if (e && (size_t)(e - f) < filenamesz) {
                    memcpy(filename, f, (size_t)(e - f));
                    filename[e - f] = '\0';
                }
            }
        } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
            char *v = line + 13;
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(ctype, ctypesz, "%s", v);
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    free(copy);
    return 1;
}

static int parse_multipart(request_t *r, upload_t *up)
{
    memset(up, 0, sizeof(*up));
    const char *ct = req_header(r, "Content-Type");
    if (!ct || strncasecmp(ct, "multipart/form-data", 19) != 0)
        return -1;
    char boundary[128];
    if (!find_boundary(ct, boundary, sizeof(boundary)))
        return -1;
    char start[160], delim[160];
    snprintf(start, sizeof(start), "--%s", boundary);
    snprintf(delim, sizeof(delim), "\r\n--%s", boundary);
    size_t slen = strlen(start), dlen = strlen(delim);
    const char *body = r->body;
    size_t len = r->body_len;
    if (len < slen || memcmp(body, start, slen) != 0)
        return -1;
    size_t pos = slen;
    for (int guard = 0; guard < 32; guard++) {
        if (pos + 1 < len && body[pos] == '-' && body[pos + 1] == '-')
            break;
        if (pos + 1 < len && body[pos] == '\r' && body[pos + 1] == '\n')
            pos += 2;
        else
            return -1;
        const char *hdr_end = memmem(body + pos, len - pos, "\r\n\r\n", 4);
        if (!hdr_end)
            return -1;
        size_t hlen = (size_t)(hdr_end - (body + pos));
        char field[64], filename[256], ctype[128];
        parse_part_headers(body + pos, hlen, field, sizeof(field), filename,
                           sizeof(filename), ctype, sizeof(ctype));
        const char *data = hdr_end + 4;
        const char *next = memmem(data, len - (size_t)(data - body), delim, dlen);
        if (!next)
            return -1;
        size_t data_len = (size_t)(next - data);
        if (!up->has_file && (strcmp(field, "file") == 0 || strcmp(field, "image") == 0 ||
                              filename[0] != '\0')) {
            up->data = data;
            up->len = data_len;
            snprintf(up->filename, sizeof(up->filename), "%s", filename);
            snprintf(up->content_type, sizeof(up->content_type), "%s", ctype);
            up->has_file = 1;
        } else if (strcmp(field, "note_id") == 0 && data_len < 32) {
            char tmp[32];
            memcpy(tmp, data, data_len);
            tmp[data_len] = '\0';
            trim_inplace(tmp);
            if (is_all_digits(tmp))
                up->note_id = strtoll(tmp, NULL, 10);
        }
        pos = (size_t)(next - body) + dlen;
    }
    if (up->note_id < 0)
        up->note_id = 0;
    return up->has_file ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static int image_count(int64_t week_id, const char *room_key)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (db_prepare("SELECT COUNT(*) FROM note_image WHERE week_id=?1 AND room_key=?2",
                   &st) == 0) {
        db_bind_i64(st, 1, week_id);
        db_bind_text(st, 2, room_key);
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static void image_json(jw_t *w, int64_t id, const char *original_name, const char *mime,
                       int64_t size, int64_t created_at, int64_t note_id)
{
    char text[32];
    char *size_text = format_size(size);
    jw_obj(w);
    jw_key(w, "id");
    jw_i64(w, id);
    jw_key(w, "original_name");
    jw_str(w, original_name);
    jw_key(w, "mime");
    jw_str(w, mime);
    jw_key(w, "size");
    jw_i64(w, size);
    jw_key(w, "size_text");
    jw_str(w, size_text);
    free(size_text);
    jw_key(w, "created_at");
    jw_i64(w, created_at);
    jw_key(w, "created_at_text");
    rc_fmt_datetime(created_at, text, sizeof(text));
    jw_str(w, text);
    jw_key(w, "url");
    char url[64];
    snprintf(url, sizeof(url), "/api/images/%lld/file", (long long)id);
    jw_str(w, url);
    jw_key(w, "note_id");
    if (note_id > 0)
        jw_i64(w, note_id);
    else
        jw_null(w);
    jw_end(w);
}

static int load_room_or_error(request_t *r, int64_t room_id, room_row_t *room)
{
    char err[256] = "";
    int status = load_room(r, room_id, room, err, sizeof(err));
    if (status == 200)
        return 1;
    const char *code = status == 404 ? "not_found" : status == 403 ? "forbidden" : "server_error";
    resp_error(r, status, code, "%s", err[0] ? err : "寝室记录不可用");
    return 0;
}

void h_images_list(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id)) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    room_row_t room;
    if (!load_room_or_error(r, room_id, &room))
        return;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, original_name, mime, size, created_at, "
        "COALESCE(note_id,0) FROM note_image WHERE week_id=?1 AND room_key=?2 ORDER BY id";
    if (db_prepare(sql, &st) != 0) {
        resp_error(r, 500, "server_error", "读取图片失败");
        return;
    }
    db_bind_i64(st, 1, room.week_id);
    db_bind_text(st, 2, room.room_key);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "images");
    jw_arr(&w);
    while (sqlite3_step(st) == SQLITE_ROW) {
        image_json(&w, db_col_i64(st, 0), db_col_text(st, 1), db_col_text(st, 2),
                   db_col_i64(st, 3), db_col_i64(st, 4), db_col_i64(st, 5));
    }
    sqlite3_finalize(st);
    jw_end(&w);
    jw_key(&w, "limit");
    jw_int(&w, RC_MAX_IMAGES);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 200, json);
    free(json);
    jw_free(&w);
}

void h_image_upload(request_t *r)
{
    int64_t room_id = 0;
    if (!req_param_i64(r, 0, &room_id)) {
        resp_error(r, 400, "invalid_room", "寝室参数错误");
        return;
    }
    room_row_t room;
    if (!load_room_or_error(r, room_id, &room))
        return;
    if (image_count(room.week_id, room.room_key) >= RC_MAX_IMAGES) {
        resp_error(r, 422, "too_many_images", "每间寝室最多上传 %d 张图片", RC_MAX_IMAGES);
        return;
    }
    upload_t up;
    int parsed = parse_multipart(r, &up);
    if (parsed < 0) {
        resp_error(r, 400, "invalid_multipart", "上传格式错误");
        return;
    }
    if (parsed == 0) {
        resp_error(r, 400, "no_file", "请选择要上传的图片文件");
        return;
    }
    if (up.len == 0) {
        resp_error(r, 400, "no_file", "上传文件为空");
        return;
    }
    if (up.len > RC_MAX_IMAGE_BYTES) {
        resp_error(r, 413, "file_too_large", "单张图片不能超过 10MB");
        return;
    }

    char *orig = trim_dup(up.filename);
    /* display name: strip any path component and control characters */
    char display[256];
    const char *base = orig;
    const char *slash = strrchr(orig, '/');
    if (!slash)
        slash = strrchr(orig, '\\');
    if (slash)
        base = slash + 1;
    if (!*base)
        base = "image";
    utf8_sanitize(base, display, sizeof(display));
    if (!display[0])
        snprintf(display, sizeof(display), "image");
    free(orig);

    const char *ext = path_ext(display);
    char ext_low[16] = "";
    size_t ei = 0;
    for (size_t i = 0; ext[i] && ei < sizeof(ext_low) - 1; i++) {
        char c = ext[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            ext_low[ei++] = (char)tolower((unsigned char)c);
    }
    ext_low[ei] = '\0';
    if (ext_low[0] && str_in_list(ext_low, k_blocked_ext)) {
        resp_error(r, 422, "forbidden_file_type", "禁止上传该类型文件（扩展名黑名单）");
        return;
    }
    char client_mime[128];
    sanitize_mime(up.content_type, client_mime, sizeof(client_mime));
    if (client_mime[0] && str_in_list(client_mime, k_blocked_mime)) {
        resp_error(r, 422, "forbidden_file_type", "禁止上传该类型文件（MIME 黑名单）");
        return;
    }
    const unsigned char *data = (const unsigned char *)up.data;
    if (magic_forbidden(data, up.len)) {
        resp_error(r, 422, "forbidden_file_type", "禁止上传可执行文件或脚本文件");
        return;
    }
    char detected[64] = "";
    int has_image_magic = detect_image_mime(data, up.len, detected, sizeof(detected));
    if (!has_image_magic && !str_starts_with(client_mime, "image/")) {
        resp_error(r, 422, "forbidden_file_type", "仅支持上传图片文件");
        return;
    }
    char stored_mime[128];
    if (has_image_magic)
        snprintf(stored_mime, sizeof(stored_mime), "%s", detected);
    else
        snprintf(stored_mime, sizeof(stored_mime), "%s", client_mime);
    if (str_in_list(stored_mime, k_blocked_mime)) {
        resp_error(r, 422, "forbidden_file_type", "禁止上传该类型文件");
        return;
    }
    if (!ext_low[0]) {
        if (strcmp(stored_mime, "image/jpeg") == 0)
            snprintf(ext_low, sizeof(ext_low), "jpg");
        else if (strcmp(stored_mime, "image/png") == 0)
            snprintf(ext_low, sizeof(ext_low), "png");
        else if (strcmp(stored_mime, "image/gif") == 0)
            snprintf(ext_low, sizeof(ext_low), "gif");
        else if (strcmp(stored_mime, "image/webp") == 0)
            snprintf(ext_low, sizeof(ext_low), "webp");
        else
            snprintf(ext_low, sizeof(ext_low), "img");
    }

    int64_t note_id = 0;
    if (up.note_id > 0) {
        sqlite3_stmt *nst = NULL;
        if (db_prepare("SELECT week_id, room_key FROM note WHERE id=?1", &nst) == 0) {
            db_bind_i64(nst, 1, up.note_id);
            if (sqlite3_step(nst) == SQLITE_ROW) {
                if (db_col_i64(nst, 0) == room.week_id &&
                    strcmp(db_col_text(nst, 1), room.room_key) == 0)
                    note_id = up.note_id;
            }
            sqlite3_finalize(nst);
        }
        if (note_id == 0) {
            resp_error(r, 400, "invalid_note", "关联备注不存在");
            return;
        }
    }

    char idbuf[32], widbuf[32], dirc[1200], rel[1200];
    snprintf(idbuf, sizeof(idbuf), "%lld", (long long)room.account_id);
    snprintf(widbuf, sizeof(widbuf), "%lld", (long long)room.week_id);
    char *p1 = path_join(g_cfg.upload_dir, idbuf);
    char *p2 = path_join(p1, widbuf);
    free(p1);
    snprintf(dirc, sizeof(dirc), "%s", p2);
    free(p2);
    if (mkdir_p(dirc, 0750) != 0) {
        resp_error(r, 500, "storage_error", "创建图片目录失败");
        return;
    }
    char fname[80];
    random_hex(fname, 32);
    snprintf(rel, sizeof(rel), "%s/%s/%s.%s", idbuf, widbuf, fname, ext_low);
    char *full = path_join(g_cfg.upload_dir, rel);
    if (file_write(full, up.data, up.len) != 0) {
        free(full);
        resp_error(r, 500, "storage_error", "保存图片文件失败");
        return;
    }
    free(full);
    char sha[65];
    rc_sha256_hex(up.data, up.len, sha);

    int64_t now = rc_now();
    sqlite3_stmt *st = NULL;
    int ok = 0;
    int64_t image_id = 0;
    if (db_prepare("INSERT INTO note_image(note_id, account_id, week_id, room_key, room_number, "
                   "rel_path, original_name, mime, size, sha256, created_at) "
                   "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)", &st) == 0) {
        if (note_id > 0)
            db_bind_i64(st, 1, note_id);
        else
            db_bind_null(st, 1);
        db_bind_i64(st, 2, room.account_id);
        db_bind_i64(st, 3, room.week_id);
        db_bind_text(st, 4, room.room_key);
        db_bind_text(st, 5, room.room_number);
        db_bind_text(st, 6, rel);
        db_bind_text(st, 7, display);
        db_bind_text(st, 8, stored_mime);
        db_bind_i64(st, 9, (int64_t)up.len);
        db_bind_text(st, 10, sha);
        db_bind_i64(st, 11, now);
        ok = db_step_done(st) == 0;
        if (ok)
            image_id = db_last_id();
        sqlite3_finalize(st);
    }
    if (!ok) {
        char *f = path_join(g_cfg.upload_dir, rel);
        unlink(f);
        free(f);
        resp_error(r, 500, "storage_error", "保存图片记录失败");
        return;
    }
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "image");
    image_json(&w, image_id, display, stored_mime, (int64_t)up.len, now, note_id);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, 201, json);
    free(json);
    jw_free(&w);
    audit_req(r, "image_upload", "room", room.room_key, "success",
              "上传图片 %s（%lld 字节）", display, (long long)up.len);
}

void h_image_file(request_t *r)
{
    int64_t image_id = 0;
    if (!req_param_i64(r, 0, &image_id)) {
        resp_error(r, 400, "invalid_image", "图片参数错误");
        return;
    }
    room_row_t room;
    char rel[1200], mime[128], name[256], err[256] = "";
    int status = load_image(r, image_id, &room, rel, sizeof(rel), mime, sizeof(mime), name,
                            sizeof(name), err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }
    if (!path_is_safe(rel)) {
        resp_error(r, 500, "storage_error", "图片路径异常");
        return;
    }
    char *full = path_join(g_cfg.upload_dir, rel);
    char *data = NULL;
    size_t len = 0;
    if (file_read(full, &data, &len) != 0) {
        free(full);
        resp_error(r, 404, "not_found", "图片文件不存在");
        return;
    }
    free(full);
    char safe_mime[128];
    sanitize_mime(mime, safe_mime, sizeof(safe_mime));
    if (!safe_mime[0])
        snprintf(safe_mime, sizeof(safe_mime), "application/octet-stream");
    r->status = 200;
    r->no_store = 1;
    snprintf(r->content_type, sizeof(r->content_type), "%s", safe_mime);
    sb_reset(&r->out);
    sb_putn(&r->out, data, len);
    free(data);
    snprintf(r->extra_headers, sizeof(r->extra_headers),
             "Content-Disposition: inline\r\n");
    if (is_admin_request(r) && room.account_id != r->account_id) {
        audit_req(r, "image_view", "image", "", "success", "查看原图");
    }
}

void h_image_delete(request_t *r)
{
    int64_t image_id = 0;
    if (!req_param_i64(r, 0, &image_id)) {
        resp_error(r, 400, "invalid_image", "图片参数错误");
        return;
    }
    room_row_t room;
    char rel[1200], mime[128], name[256], err[256] = "";
    int status = load_image(r, image_id, &room, rel, sizeof(rel), mime, sizeof(mime), name,
                            sizeof(name), err, sizeof(err));
    if (status != 200) {
        resp_error(r, status, status == 404 ? "not_found" : "forbidden", "%s", err);
        return;
    }
    if (!path_is_safe(rel)) {
        resp_error(r, 500, "storage_error", "图片路径异常");
        return;
    }
    char *full = path_join(g_cfg.upload_dir, rel);
    if (unlink(full) != 0 && errno != ENOENT) {
        free(full);
        resp_error(r, 500, "image_delete_failed", "删除图片文件失败");
        return;
    }
    free(full);
    sqlite3_stmt *st = NULL;
    if (db_prepare("DELETE FROM note_image WHERE id=?1", &st) != 0) {
        resp_error(r, 500, "image_delete_failed", "删除图片记录失败");
        return;
    }
    db_bind_i64(st, 1, image_id);
    int rc = db_step_done(st);
    sqlite3_finalize(st);
    if (rc != 0) {
        resp_error(r, 500, "image_delete_failed", "删除图片记录失败");
        return;
    }
    audit_req(r, "image_delete", "room", room.room_key, "success", "删除图片 %s", name);
    resp_ok(r);
}
