/*
 * json.c - minimal JSON parser and writer
 */
#include "common.h"

#include <ctype.h>
#include <math.h>

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    int error;
} jp_t;

static jval_t *parse_value(jp_t *p, int depth);

static void jp_skip_ws(jp_t *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static jval_t *jnew(jtype_t type)
{
    jval_t *v = xcalloc(1, sizeof(jval_t));
    v->type = type;
    return v;
}

static int jp_hex4(jp_t *p, unsigned *out)
{
    if (p->pos + 4 > p->len)
        return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->s[p->pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void utf8_encode(strbuf_t *b, unsigned cp)
{
    if (cp <= 0x7F) {
        sb_putc(b, (char)cp);
    } else if (cp <= 0x7FF) {
        sb_putc(b, (char)(0xC0 | (cp >> 6)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        sb_putc(b, (char)(0xE0 | (cp >> 12)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (char)(0xF0 | (cp >> 18)));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *parse_string_raw(jp_t *p)
{
    if (p->pos >= p->len || p->s[p->pos] != '"') {
        p->error = 1;
        return NULL;
    }
    p->pos++;
    strbuf_t b;
    sb_init(&b);
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"') {
            if (!b.data)
                return xstrdup("");
            return sb_steal(&b);
        }
        if (c < 0x20) {
            p->error = 1;
            break;
        }
        if (c != '\\') {
            sb_putc(&b, (char)c);
            continue;
        }
        if (p->pos >= p->len) {
            p->error = 1;
            break;
        }
        char esc = p->s[p->pos++];
        switch (esc) {
        case '"': sb_putc(&b, '"'); break;
        case '\\': sb_putc(&b, '\\'); break;
        case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, '\b'); break;
        case 'f': sb_putc(&b, '\f'); break;
        case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 't': sb_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!jp_hex4(p, &cp)) {
                p->error = 1;
                break;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                p->pos += 2;
                unsigned low;
                if (!jp_hex4(p, &low)) {
                    p->error = 1;
                    break;
                }
                if (low >= 0xDC00 && low <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                else
                    cp = 0xFFFD;
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            utf8_encode(&b, cp);
            break;
        }
        default:
            p->error = 1;
            break;
        }
        if (p->error)
            break;
    }
    p->error = 1;
    sb_free(&b);
    return NULL;
}

static void obj_add(jval_t *v, char *key, jval_t *val)
{
    if (v->u.obj.count == v->u.obj.cap) {
        int cap = v->u.obj.cap ? v->u.obj.cap * 2 : 8;
        v->u.obj.keys = realloc(v->u.obj.keys, (size_t)cap * sizeof(char *));
        v->u.obj.vals = realloc(v->u.obj.vals, (size_t)cap * sizeof(jval_t *));
        if (!v->u.obj.keys || !v->u.obj.vals) {
            fprintf(stderr, "fatal: out of memory\n");
            abort();
        }
        v->u.obj.cap = cap;
    }
    v->u.obj.keys[v->u.obj.count] = key;
    v->u.obj.vals[v->u.obj.count] = val;
    v->u.obj.count++;
}

static void arr_add(jval_t *v, jval_t *val)
{
    if (v->u.arr.count == v->u.arr.cap) {
        int cap = v->u.arr.cap ? v->u.arr.cap * 2 : 8;
        v->u.arr.items = realloc(v->u.arr.items, (size_t)cap * sizeof(jval_t *));
        if (!v->u.arr.items) {
            fprintf(stderr, "fatal: out of memory\n");
            abort();
        }
        v->u.arr.cap = cap;
    }
    v->u.arr.items[v->u.arr.count++] = val;
}

static jval_t *parse_object(jp_t *p, int depth)
{
    p->pos++; /* { */
    jval_t *v = jnew(JOBJ);
    jp_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        jp_skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key)
            goto fail;
        jp_skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            free(key);
            goto fail;
        }
        p->pos++;
        jval_t *val = parse_value(p, depth + 1);
        if (!val) {
            free(key);
            goto fail;
        }
        obj_add(v, key, val);
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            return v;
        }
        goto fail;
    }
fail:
    p->error = 1;
    json_free(v);
    return NULL;
}

static jval_t *parse_array(jp_t *p, int depth)
{
    p->pos++; /* [ */
    jval_t *v = jnew(JARR);
    jp_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        jval_t *val = parse_value(p, depth + 1);
        if (!val)
            goto fail;
        arr_add(v, val);
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
            return v;
        }
        goto fail;
    }
fail:
    p->error = 1;
    json_free(v);
    return NULL;
}

static jval_t *parse_value(jp_t *p, int depth)
{
    if (depth > 24) {
        p->error = 1;
        return NULL;
    }
    jp_skip_ws(p);
    if (p->pos >= p->len) {
        p->error = 1;
        return NULL;
    }
    char c = p->s[p->pos];
    if (c == '{')
        return parse_object(p, depth);
    if (c == '[')
        return parse_array(p, depth);
    if (c == '"') {
        char *s = parse_string_raw(p);
        if (!s)
            return NULL;
        jval_t *v = jnew(JSTR);
        v->u.str = s;
        return v;
    }
    if (c == 't' && p->pos + 4 <= p->len && strncmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4;
        jval_t *v = jnew(JBOOL);
        v->u.b = 1;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5;
        jval_t *v = jnew(JBOOL);
        v->u.b = 0;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->s + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return jnew(JNULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = p->pos;
        if (p->s[p->pos] == '-')
            p->pos++;
        int digits = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) {
            p->pos++;
            digits++;
        }
        if (p->pos < p->len && p->s[p->pos] == '.') {
            p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos]))
                p->pos++;
        }
        if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
            p->pos++;
            if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
                p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos]))
                p->pos++;
        }
        if (digits == 0) {
            p->error = 1;
            return NULL;
        }
        char *tmp = xstrndup(p->s + start, p->pos - start);
        jval_t *v = jnew(JNUM);
        v->u.num = strtod(tmp, NULL);
        free(tmp);
        return v;
    }
    p->error = 1;
    return NULL;
}

jval_t *json_parse(const char *s, size_t len)
{
    jp_t p = {s, len, 0, 0};
    jval_t *v = parse_value(&p, 0);
    if (!v || p.error) {
        json_free(v);
        return NULL;
    }
    jp_skip_ws(&p);
    if (p.pos != p.len) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(jval_t *v)
{
    if (!v)
        return;
    switch (v->type) {
    case JSTR:
        free(v->u.str);
        break;
    case JARR:
        for (int i = 0; i < v->u.arr.count; i++)
            json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case JOBJ:
        for (int i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.keys[i]);
            json_free(v->u.obj.vals[i]);
        }
        free(v->u.obj.keys);
        free(v->u.obj.vals);
        break;
    default:
        break;
    }
    free(v);
}

jval_t *json_obj_get(jval_t *obj, const char *key)
{
    if (!obj || obj->type != JOBJ || !key)
        return NULL;
    for (int i = 0; i < obj->u.obj.count; i++)
        if (strcmp(obj->u.obj.keys[i], key) == 0)
            return obj->u.obj.vals[i];
    return NULL;
}

const char *json_get_str(jval_t *obj, const char *key)
{
    jval_t *v = json_obj_get(obj, key);
    if (!v || v->type != JSTR)
        return NULL;
    return v->u.str;
}

int64_t json_get_i64(jval_t *obj, const char *key, int64_t def)
{
    jval_t *v = json_obj_get(obj, key);
    if (!v || v->type != JNUM)
        return def;
    return (int64_t)v->u.num;
}

int json_get_int(jval_t *obj, const char *key, int def)
{
    return (int)json_get_i64(obj, key, def);
}

int json_get_bool(jval_t *obj, const char *key, int def)
{
    jval_t *v = json_obj_get(obj, key);
    if (!v || v->type != JBOOL)
        return def;
    return v->u.b;
}

int json_arr_count(jval_t *arr)
{
    return (arr && arr->type == JARR) ? arr->u.arr.count : 0;
}

jval_t *json_arr_get(jval_t *arr, int idx)
{
    if (!arr || arr->type != JARR || idx < 0 || idx >= arr->u.arr.count)
        return NULL;
    return arr->u.arr.items[idx];
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */
void jw_init(jw_t *w)
{
    memset(w, 0, sizeof(*w));
    sb_init(&w->b);
}

void jw_free(jw_t *w)
{
    sb_free(&w->b);
}

static void jw_value_start(jw_t *w)
{
    if (w->after_key) {
        w->after_key = 0;
        return;
    }
    int d = w->depth;
    if (d > 0) {
        if (!w->first[d])
            sb_putc(&w->b, ',');
        w->first[d] = 0;
    }
}

static void jw_string_raw(jw_t *w, const char *s)
{
    sb_putc(&w->b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            unsigned char c = *p;
            if (c < 0x80) {
                switch (c) {
                case '"': sb_puts(&w->b, "\\\""); break;
                case '\\': sb_puts(&w->b, "\\\\"); break;
                case '\b': sb_puts(&w->b, "\\b"); break;
                case '\f': sb_puts(&w->b, "\\f"); break;
                case '\n': sb_puts(&w->b, "\\n"); break;
                case '\r': sb_puts(&w->b, "\\r"); break;
                case '\t': sb_puts(&w->b, "\\t"); break;
                default:
                    if (c < 0x20)
                        sb_printf(&w->b, "\\u%04x", c);
                    else
                        sb_putc(&w->b, (char)c);
                }
                continue;
            }
            size_t need = 0;
            if ((c & 0xE0) == 0xC0)
                need = 2;
            else if ((c & 0xF0) == 0xE0)
                need = 3;
            else if ((c & 0xF8) == 0xF0)
                need = 4;
            int valid = need > 0;
            for (size_t k = 1; valid && k < need; k++) {
                if (!p[k] || (p[k] & 0xC0) != 0x80)
                    valid = 0;
            }
            if (!valid) {
                sb_puts(&w->b, "\\ufffd");
                continue;
            }
            sb_putn(&w->b, (const char *)p, need);
            p += need - 1;
        }
    }
    sb_putc(&w->b, '"');
}

void jw_obj(jw_t *w)
{
    jw_value_start(w);
    sb_putc(&w->b, '{');
    if (w->depth < 31) {
        w->depth++;
        w->is_obj[w->depth] = 1;
        w->first[w->depth] = 1;
    }
}

void jw_arr(jw_t *w)
{
    jw_value_start(w);
    sb_putc(&w->b, '[');
    if (w->depth < 31) {
        w->depth++;
        w->is_obj[w->depth] = 0;
        w->first[w->depth] = 1;
    }
}

void jw_end(jw_t *w)
{
    if (w->depth <= 0)
        return;
    int d = w->depth;
    sb_putc(&w->b, w->is_obj[d] ? '}' : ']');
    w->depth--;
}

void jw_key(jw_t *w, const char *key)
{
    int d = w->depth;
    if (d <= 0 || !w->is_obj[d])
        return;
    if (!w->first[d])
        sb_putc(&w->b, ',');
    w->first[d] = 0;
    jw_string_raw(w, key);
    sb_putc(&w->b, ':');
    w->after_key = 1;
}

void jw_str(jw_t *w, const char *s)
{
    jw_value_start(w);
    if (!s) {
        sb_puts(&w->b, "null");
        return;
    }
    jw_string_raw(w, s);
}

void jw_strn(jw_t *w, const char *s, size_t n)
{
    char *tmp = xstrndup(s ? s : "", n);
    jw_str(w, tmp);
    free(tmp);
}

void jw_i64(jw_t *w, int64_t v)
{
    jw_value_start(w);
    sb_printf(&w->b, "%lld", (long long)v);
}

void jw_int(jw_t *w, int v)
{
    jw_value_start(w);
    sb_printf(&w->b, "%d", v);
}

void jw_bool(jw_t *w, int v)
{
    jw_value_start(w);
    sb_puts(&w->b, v ? "true" : "false");
}

void jw_null(jw_t *w)
{
    jw_value_start(w);
    sb_puts(&w->b, "null");
}

void jw_raw(jw_t *w, const char *raw)
{
    jw_value_start(w);
    sb_puts(&w->b, raw ? raw : "null");
}
