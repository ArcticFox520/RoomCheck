/*
 * http.c - HTTP/1.1 server, routing, request parsing and responses
 */
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static int g_listen_fd = -1;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_active_connections = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static int header_name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *header_get(request_t *r, const char *name)
{
    for (int i = 0; i < r->header_count; i++)
        if (header_name_eq(r->headers[i].key, name))
            return r->headers[i].value;
    return NULL;
}

const char *req_header(request_t *r, const char *name)
{
    return header_get(r, name);
}

static int cookie_get(request_t *r, const char *name, char *out, size_t outsz)
{
    const char *cookie = header_get(r, "Cookie");
    if (!cookie)
        return 0;
    size_t nlen = strlen(name);
    const char *p = cookie;
    while (*p) {
        while (*p == ' ' || *p == ';')
            p++;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *end = strchr(v, ';');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= outsz)
                len = outsz - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        while (*p && *p != ';')
            p++;
    }
    return 0;
}

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

/* ------------------------------------------------------------------ */
/* responses                                                           */
/* ------------------------------------------------------------------ */
void resp_json(request_t *r, int status, const char *json)
{
    r->status = status;
    snprintf(r->content_type, sizeof(r->content_type), "application/json; charset=utf-8");
    sb_reset(&r->out);
    sb_puts(&r->out, json ? json : "{}");
    r->no_store = 1;
}

void resp_error(request_t *r, int status, const char *code, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    jw_t w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "error");
    jw_obj(&w);
    jw_key(&w, "code");
    jw_str(&w, code);
    jw_key(&w, "message");
    jw_str(&w, msg);
    jw_end(&w);
    jw_end(&w);
    char *json = sb_steal(&w.b);
    resp_json(r, status, json);
    free(json);
    jw_free(&w);
}

void resp_ok(request_t *r)
{
    resp_json(r, 200, "{\"ok\":true}");
}

int req_param_i64(request_t *r, int idx, int64_t *out)
{
    if (idx < 0 || idx >= r->param_count || !r->params[idx])
        return 0;
    return parse_i64(r->params[idx], out);
}

const char *req_query(request_t *r, const char *key)
{
    for (int i = 0; i < r->query_count; i++)
        if (strcmp(r->query_params[i].key, key) == 0)
            return r->query_params[i].value;
    return NULL;
}

jval_t *req_json(request_t *r)
{
    if (!r->body || r->body_len == 0) {
        resp_error(r, 400, "invalid_json", "请求体不能为空");
        return NULL;
    }
    jval_t *v = json_parse(r->body, r->body_len);
    if (!v) {
        resp_error(r, 400, "invalid_json", "请求体不是合法的 JSON");
        return NULL;
    }
    if (v->type != JOBJ) {
        json_free(v);
        resp_error(r, 400, "invalid_json", "请求体必须是 JSON 对象");
        return NULL;
    }
    return v;
}

void req_audit_ip(request_t *r, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", r->remote_ip);
}

/* ------------------------------------------------------------------ */
/* request parsing                                                     */
/* ------------------------------------------------------------------ */
static void free_request(request_t *r)
{
    free(r->target);
    free(r->path);
    free(r->query);
    free(r->body);
    for (int i = 0; i < r->query_count; i++) {
        free(r->query_params[i].key);
        free(r->query_params[i].value);
    }
    r->query_count = 0;
    for (int i = 0; i < r->header_count; i++) {
        free(r->headers[i].key);
        free(r->headers[i].value);
    }
    r->header_count = 0;
    for (int i = 0; i < r->param_count; i++) {
        free(r->params[i]);
        r->params[i] = NULL;
    }
    r->param_count = 0;
    sb_free(&r->out);
}

static int parse_query(request_t *r, const char *query)
{
    if (!query || !*query)
        return 0;
    char *copy = xstrdup(query);
    char *save = NULL;
    char *pair = strtok_r(copy, "&", &save);
    while (pair && r->query_count < 32) {
        char *eq = strchr(pair, '=');
        char *key, *val;
        if (eq) {
            *eq = '\0';
            key = pair;
            val = eq + 1;
        } else {
            key = pair;
            val = "";
        }
        for (char *p = val; *p; p++)
            if (*p == '+')
                *p = ' ';
        r->query_params[r->query_count].key = url_decode(key);
        r->query_params[r->query_count].value = url_decode(val);
        r->query_count++;
        pair = strtok_r(NULL, "&", &save);
    }
    free(copy);
    return 0;
}

static int parse_headers(request_t *r, char *header, size_t header_len)
{
    char *save = NULL;
    /* header buffer starts with request line */
    char *line = strtok_r(header, "\r\n", &save);
    if (!line)
        return -1;
    char method[16] = "";
    char target[4096] = "";
    char version[16] = "";
    if (sscanf(line, "%15s %4095s %15s", method, target, version) != 3)
        return -1;
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return -1;
    snprintf(r->method, sizeof(r->method), "%s", method);
    r->target = xstrdup(target);
    if (strlen(target) > 4000)
        return -1;
    char *q = strchr(r->target, '?');
    if (q) {
        *q = '\0';
        r->query = xstrdup(q + 1);
    } else {
        r->query = xstrdup("");
    }
    r->path = url_decode(r->target);
    if (strlen(r->path) == 0 || r->path[0] != '/') {
        char *fixed = xmalloc(strlen(r->path) + 2);
        fixed[0] = '/';
        strcpy(fixed + 1, r->path);
        free(r->path);
        r->path = fixed;
    }
    for (const char *p = r->path; *p; p++) {
        if (*p == '\0')
            return -1;
    }
    parse_query(r, r->query);
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (r->header_count >= RC_MAX_HEADERS)
            return -1;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        char *end = val + strlen(val);
        while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        r->headers[r->header_count].key = xstrdup(line);
        r->headers[r->header_count].value = xstrdup(val);
        r->header_count++;
    }
    (void)header_len;
    return 0;
}

static void fill_remote_addr(request_t *r, int fd)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) {
        snprintf(r->remote_ip, sizeof(r->remote_ip), "unknown");
        return;
    }
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &in->sin_addr, r->remote_ip, sizeof(r->remote_ip));
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        char tmp[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &in6->sin6_addr, tmp, sizeof(tmp));
        if (strncmp(tmp, "::ffff:", 7) == 0)
            snprintf(r->remote_ip, sizeof(r->remote_ip), "%s", tmp + 7);
        else
            snprintf(r->remote_ip, sizeof(r->remote_ip), "%s", tmp);
    } else {
        snprintf(r->remote_ip, sizeof(r->remote_ip), "unknown");
    }
}

static void send_early_error(int fd, int status, const char *msg)
{
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"error\":{\"code\":\"bad_request\",\"message\":\"%s\"}}", msg);
    char head[512];
    int hlen = snprintf(head, sizeof(head),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: application/json; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        status, status_text(status), n);
    send(fd, head, (size_t)hlen, MSG_NOSIGNAL);
    send(fd, body, (size_t)n, MSG_NOSIGNAL);
}

static int read_full(int fd, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

int http_parse_request(int fd, request_t *r)
{
    memset(r->method, 0, sizeof(r->method));
    size_t cap = 16384;
    char *buf = xmalloc(cap + 1);
    size_t used = 0;
    size_t header_end = 0;
    int found = 0;
    while (used < cap) {
        ssize_t n = recv(fd, buf + used, cap - used, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }
        if (n == 0) {
            free(buf);
            return -1;
        }
        used += (size_t)n;
        for (size_t i = 0; i + 3 < used; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                header_end = i + 4;
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }
    if (!found) {
        free(buf);
        send_early_error(fd, 400, "请求头不完整");
        return -1;
    }
    char *header_copy = xstrndup(buf, header_end);
    if (parse_headers(r, header_copy, header_end) != 0) {
        free(header_copy);
        free(buf);
        send_early_error(fd, 400, "请求格式错误");
        return -1;
    }
    free(header_copy);

    const char *cl = header_get(r, "Content-Length");
    int64_t content_len = 0;
    if (cl) {
        if (!parse_i64(cl, &content_len) || content_len < 0) {
            free(buf);
            send_early_error(fd, 400, "Content-Length 非法");
            return -1;
        }
    }
    if (content_len > (int64_t)RC_MAX_BODY) {
        free(buf);
        send_early_error(fd, 413, "请求体过大");
        return -1;
    }
    if (content_len > 0) {
        char *body = xmalloc((size_t)content_len + 1);
        size_t already = used - header_end;
        if (already > (size_t)content_len)
            already = (size_t)content_len;
        memcpy(body, buf + header_end, already);
        if (already < (size_t)content_len) {
            if (read_full(fd, body + already, (size_t)content_len - already) != 0) {
                free(body);
                free(buf);
                return -1;
            }
        }
        body[content_len] = '\0';
        r->body = body;
        r->body_len = (size_t)content_len;
    } else {
        r->body = xstrdup("");
        r->body_len = 0;
    }
    free(buf);
    r->status = 200;
    r->no_store = 1;
    fill_remote_addr(r, fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* routing                                                             */
/* ------------------------------------------------------------------ */
static int split_path(const char *path, char *segv[], int max)
{
    int n = 0;
    const char *p = path;
    while (*p && n < max) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        char *seg = xstrndup(start, len);
        segv[n++] = seg;
    }
    return n;
}

static const route_t *match_route(request_t *r, const char *method, const char *path)
{
    char *psegs[16];
    int pcount = split_path(path, psegs, 16);
    const route_t *found = NULL;
    for (const route_t *rt = g_routes; rt->pattern; rt++) {
        if (strcmp(rt->method, method) != 0)
            continue;
        char *tsegs[16];
        int tcount = split_path(rt->pattern, tsegs, 16);
        int ok = (tcount == pcount);
        if (ok) {
            for (int i = 0; i < tcount; i++) {
                if (tsegs[i][0] == ':') {
                    if (r->param_count < RC_MAX_PARAMS)
                        r->params[r->param_count++] = xstrdup(psegs[i]);
                } else if (strcmp(tsegs[i], psegs[i]) != 0) {
                    ok = 0;
                }
                if (!ok)
                    break;
            }
        }
        for (int i = 0; i < tcount; i++)
            free(tsegs[i]);
        if (ok) {
            found = rt;
            break;
        }
        for (int i = 0; i < r->param_count; i++) {
            free(r->params[i]);
            r->params[i] = NULL;
        }
        r->param_count = 0;
    }
    for (int i = 0; i < pcount; i++)
        free(psegs[i]);
    return found;
}

static const char *mime_type(const char *path)
{
    const char *ext = path_ext(path);
    if (str_ieq(ext, "html") || str_ieq(ext, "htm")) return "text/html; charset=utf-8";
    if (str_ieq(ext, "css")) return "text/css; charset=utf-8";
    if (str_ieq(ext, "js") || str_ieq(ext, "mjs")) return "application/javascript; charset=utf-8";
    if (str_ieq(ext, "json")) return "application/json; charset=utf-8";
    if (str_ieq(ext, "svg")) return "image/svg+xml";
    if (str_ieq(ext, "png")) return "image/png";
    if (str_ieq(ext, "jpg") || str_ieq(ext, "jpeg")) return "image/jpeg";
    if (str_ieq(ext, "gif")) return "image/gif";
    if (str_ieq(ext, "webp")) return "image/webp";
    if (str_ieq(ext, "ico")) return "image/x-icon";
    if (str_ieq(ext, "txt")) return "text/plain; charset=utf-8";
    if (str_ieq(ext, "woff2")) return "font/woff2";
    if (str_ieq(ext, "map")) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

static int is_spa_route(const char *path)
{
    for (const char **p = g_spa_routes; *p; p++) {
        if (strcmp(*p, path) == 0)
            return 1;
    }
    return 0;
}

static int serve_static(request_t *r)
{
    if (strcmp(r->method, "GET") != 0 && strcmp(r->method, "HEAD") != 0) {
        resp_error(r, 405, "method_not_allowed", "不支持的方法");
        return 1;
    }
    const char *rel = NULL;
    if (is_spa_route(r->path)) {
        rel = "index.html";
    } else if (str_starts_with(r->path, "/css/") || str_starts_with(r->path, "/js/") ||
               str_starts_with(r->path, "/assets/")) {
        rel = r->path + 1;
    }
    if (!rel) {
        resp_error(r, 404, "not_found", "资源不存在");
        return 1;
    }
    if (!path_is_safe(rel)) {
        resp_error(r, 404, "not_found", "资源不存在");
        return 1;
    }
    char *full = path_join(g_cfg.web_root, rel);
    char *data = NULL;
    size_t len = 0;
    if (file_read(full, &data, &len) != 0) {
        free(full);
        resp_error(r, 404, "not_found", "资源不存在");
        return 1;
    }
    free(full);
    r->status = 200;
    snprintf(r->content_type, sizeof(r->content_type), "%s", mime_type(rel));
    sb_reset(&r->out);
    sb_putn(&r->out, data, len);
    free(data);
    if (str_ends_with(rel, ".html")) {
        r->is_html = 1;
        r->no_store = 1;
    } else {
        r->no_store = 0;
        snprintf(r->extra_headers, sizeof(r->extra_headers),
                 "Cache-Control: public, max-age=300\r\n");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
static int method_is_mutating(const char *m)
{
    return strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 ||
           strcmp(m, "PATCH") == 0 || strcmp(m, "DELETE") == 0;
}

static int must_change_allowed(const request_t *r)
{
    if (strcmp(r->path, "/api/session") == 0)
        return 1;
    if (strcmp(r->path, "/api/password") == 0)
        return 1;
    if (strcmp(r->path, "/api/logout") == 0)
        return 1;
    if (strcmp(r->path, "/api/health") == 0)
        return 1;
    return 0;
}

static void authenticate(request_t *r)
{
    char token[256] = "";
    if (!cookie_get(r, RC_SESSION_COOKIE, token, sizeof(token)))
        return;
    account_t acct;
    char csrf[80] = "";
    int64_t expires = 0;
    char hash[80] = "";
    if (auth_validate_session(token, &acct, csrf, sizeof(csrf), &expires, hash)) {
        r->authed = 1;
        r->account_id = acct.id;
        snprintf(r->role, sizeof(r->role), "%s", acct.role);
        snprintf(r->username, sizeof(r->username), "%s", acct.username);
        snprintf(r->display_name, sizeof(r->display_name), "%s", acct.display_name);
        r->must_change_password = acct.must_change_password;
        snprintf(r->csrf_token, sizeof(r->csrf_token), "%s", csrf);
        snprintf(r->session_hash, sizeof(r->session_hash), "%s", hash);
        r->expires_at = expires;
    }
}

static void handle_client(request_t *r)
{
    const route_t *rt = match_route(r, r->method, r->path);
    int api_like = str_starts_with(r->path, "/api/");

    if (!rt) {
        if (api_like) {
            resp_error(r, 404, "not_found", "接口不存在");
            return;
        }
        serve_static(r);
        return;
    }

    if (!(rt->flags & RF_PUBLIC)) {
        authenticate(r);
        if (!r->authed) {
            auth_clear_session_cookie(r);
            resp_error(r, 401, "unauthenticated", "请先登录");
            return;
        }
        if (r->must_change_password && !must_change_allowed(r)) {
            resp_error(r, 403, "password_change_required", "首次登录请先修改密码");
            return;
        }
        if ((rt->flags & RF_ADMIN) && strcmp(r->role, "admin") != 0) {
            resp_error(r, 403, "forbidden", "需要管理员权限");
            return;
        }
        if (method_is_mutating(r->method)) {
            const char *csrf = header_get(r, "X-CSRF-Token");
            if (!csrf || !secure_eq(csrf, r->csrf_token)) {
                resp_error(r, 403, "csrf_invalid", "CSRF 校验失败，请刷新页面后重试");
                return;
            }
        }
    }

    db_lock();
    rt->handler(r);
    db_unlock();
}

/* ------------------------------------------------------------------ */
/* response write                                                      */
/* ------------------------------------------------------------------ */
void http_send_response(request_t *r)
{
    if (r->status <= 0)
        r->status = 200;
    if (r->content_type[0] == '\0')
        snprintf(r->content_type, sizeof(r->content_type), "application/json; charset=utf-8");

    strbuf_t head;
    sb_init(&head);
    sb_printf(&head, "HTTP/1.1 %d %s\r\n", r->status, status_text(r->status));
    sb_printf(&head, "Server: RoomCheck/%s\r\n", RC_VERSION);
    sb_printf(&head, "Content-Type: %s\r\n", r->content_type);
    sb_printf(&head, "Content-Length: %zu\r\n", r->out.len);
    sb_puts(&head, "Connection: close\r\n");
    sb_puts(&head, "X-Content-Type-Options: nosniff\r\n");
    sb_puts(&head, "X-Frame-Options: DENY\r\n");
    sb_puts(&head, "Referrer-Policy: no-referrer\r\n");
    sb_puts(&head, "Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n");
    if (r->no_store)
        sb_puts(&head, "Cache-Control: no-store\r\n");
    else
        sb_puts(&head, "Cache-Control: no-cache\r\n");
    if (r->is_html)
        sb_puts(&head,
                "Content-Security-Policy: default-src 'self'; img-src 'self' data: blob:; "
                "style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; "
                "font-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n");
    if (r->set_cookie[0])
        sb_printf(&head, "Set-Cookie: %s\r\n", r->set_cookie);
    if (r->extra_headers[0])
        sb_puts(&head, r->extra_headers);
    sb_puts(&head, "\r\n");

    size_t off = 0;
    while (off < head.len) {
        ssize_t n = send(r->fd, head.data + off, head.len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    int is_head = strcmp(r->method, "HEAD") == 0;
    if (!is_head) {
        off = 0;
        while (off < r->out.len) {
            ssize_t n = send(r->fd, r->out.data + off, r->out.len - off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)n;
        }
    }
    sb_free(&head);
}

/* ------------------------------------------------------------------ */
/* server loop                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int fd;
} client_arg_t;

static void *client_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    free(ca);

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    request_t r;
    memset(&r, 0, sizeof(r));
    r.fd = fd;
    r.status = 200;
    sb_init(&r.out);
    if (http_parse_request(fd, &r) == 0) {
        handle_client(&r);
        printf("%s %s%s%s -> %d (%s)\n", r.remote_ip, r.method, r.path,
               r.query && r.query[0] ? "?..." : "", r.status,
               r.authed ? r.username : "-");
        fflush(stdout);
        http_send_response(&r);
    }
    free_request(&r);
    close(fd);

    pthread_mutex_lock(&g_conn_mutex);
    g_active_connections--;
    pthread_mutex_unlock(&g_conn_mutex);
    return NULL;
}

int http_server_run(int port)
{
    g_listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    int v6 = 1;
    if (g_listen_fd < 0) {
        g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        v6 = 0;
    }
    if (g_listen_fd < 0) {
        fprintf(stderr, "无法创建监听 socket: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (v6) {
        int zero = 0;
        setsockopt(g_listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    }
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addrlen;
    if (v6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&addr;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_any;
        a->sin6_port = htons((uint16_t)port);
        addrlen = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&addr;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port = htons((uint16_t)port);
        addrlen = sizeof(*a);
    }
    if (bind(g_listen_fd, (struct sockaddr *)&addr, addrlen) != 0) {
        fprintf(stderr, "绑定端口 %d 失败: %s\n", port, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    if (listen(g_listen_fd, 128) != 0) {
        fprintf(stderr, "监听失败: %s\n", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    printf("RoomCheck HTTP 服务已启动: http://0.0.0.0:%d\n", port);
    fflush(stdout);

    while (!g_shutdown) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (g_shutdown)
                break;
            if (errno == EMFILE || errno == ENFILE) {
                fprintf(stderr, "连接数过多: %s\n", strerror(errno));
                usleep(100000);
            }
            continue;
        }
        pthread_mutex_lock(&g_conn_mutex);
        int active = g_active_connections;
        pthread_mutex_unlock(&g_conn_mutex);
        if (active >= 256) {
            send_early_error(fd, 503, "服务器繁忙，请稍后再试");
            close(fd);
            continue;
        }
        client_arg_t *ca = xmalloc(sizeof(*ca));
        ca->fd = fd;
        pthread_t th;
        pthread_mutex_lock(&g_conn_mutex);
        g_active_connections++;
        pthread_mutex_unlock(&g_conn_mutex);
        if (pthread_create(&th, NULL, client_thread, ca) != 0) {
            pthread_mutex_lock(&g_conn_mutex);
            g_active_connections--;
            pthread_mutex_unlock(&g_conn_mutex);
            free(ca);
            send_early_error(fd, 503, "服务器繁忙，请稍后再试");
            close(fd);
            continue;
        }
        pthread_detach(th);
    }
    return 0;
}

void http_wait_idle(int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        pthread_mutex_lock(&g_conn_mutex);
        int active = g_active_connections;
        pthread_mutex_unlock(&g_conn_mutex);
        if (active <= 0)
            return;
        usleep(50 * 1000);
        waited += 50;
    }
}

void http_stop(void)
{
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
