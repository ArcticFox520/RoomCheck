/*
 * util.c - string, time, crypto, filesystem helpers
 */
#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* string buffer                                                       */
/* ------------------------------------------------------------------ */
void sb_init(strbuf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(strbuf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_reset(strbuf_t *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

void sb_reserve(strbuf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    b->data = p;
    b->cap = cap;
}

void sb_putc(strbuf_t *b, char c)
{
    sb_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void sb_putn(strbuf_t *b, const char *s, size_t n)
{
    if (!s || n == 0)
        return;
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(strbuf_t *b, const char *s)
{
    if (s)
        sb_putn(b, s, strlen(s));
}

void sb_printf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *sb_steal(strbuf_t *b)
{
    char *p = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

const char *sb_cstr(strbuf_t *b)
{
    return b->data ? b->data : "";
}

/* ------------------------------------------------------------------ */
/* basic helpers                                                       */
/* ------------------------------------------------------------------ */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    return p;
}

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    if (!s)
        return NULL;
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void trim_inplace(char *s)
{
    if (!s)
        return;
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

char *trim_dup(const char *s)
{
    if (!s)
        return xstrdup("");
    char *p = xstrdup(s);
    trim_inplace(p);
    return p;
}

char *lower_dup(const char *s)
{
    if (!s)
        return xstrdup("");
    char *p = xstrdup(s);
    for (char *q = p; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    return p;
}

int str_ieq(const char *a, const char *b)
{
    return a && b && strcasecmp(a, b) == 0;
}

int str_starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return 0;
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

int parse_i64(const char *s, int64_t *out)
{
    if (!s || !*s)
        return 0;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return 0;
    *out = (int64_t)v;
    return 1;
}

int is_all_digits(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

int utf8_count(const char *s)
{
    int n = 0;
    if (!s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}

size_t utf8_prefix_bytes(const char *s, int max_chars)
{
    if (!s || max_chars <= 0)
        return 0;
    size_t i = 0;
    int chars = 0;
    while (s[i] && chars < max_chars) {
        unsigned char c = (unsigned char)s[i];
        size_t need = 1;
        if ((c & 0xE0) == 0xC0)
            need = 2;
        else if ((c & 0xF0) == 0xE0)
            need = 3;
        else if ((c & 0xF8) == 0xF0)
            need = 4;
        for (size_t k = 1; k < need; k++) {
            if (!s[i + k] || ((unsigned char)s[i + k] & 0xC0) != 0x80) {
                need = 1;
                break;
            }
        }
        i += need;
        chars++;
    }
    return i;
}

char *format_size(int64_t bytes)
{
    char buf[64];
    if (bytes < 1024)
        snprintf(buf, sizeof(buf), "%" PRId64 " B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return xstrdup(buf);
}

char *duration_text(int64_t seconds)
{
    if (seconds < 0)
        seconds = 0;
    int64_t days = seconds / 86400;
    int64_t hours = (seconds % 86400) / 3600;
    int64_t mins = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;
    char buf[128];
    if (days > 0)
        snprintf(buf, sizeof(buf), "%" PRId64 "天%" PRId64 "小时", days, hours);
    else if (hours > 0)
        snprintf(buf, sizeof(buf), "%" PRId64 "小时%" PRId64 "分", hours, mins);
    else if (mins > 0)
        snprintf(buf, sizeof(buf), "%" PRId64 "分%" PRId64 "秒", mins, secs);
    else
        snprintf(buf, sizeof(buf), "%" PRId64 "秒", secs);
    return xstrdup(buf);
}

char *json_escape(const char *s)
{
    strbuf_t b;
    sb_init(&b);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"': sb_puts(&b, "\\\""); break;
            case '\\': sb_puts(&b, "\\\\"); break;
            case '\b': sb_puts(&b, "\\b"); break;
            case '\f': sb_puts(&b, "\\f"); break;
            case '\n': sb_puts(&b, "\\n"); break;
            case '\r': sb_puts(&b, "\\r"); break;
            case '\t': sb_puts(&b, "\\t"); break;
            default:
                if (*p < 0x20)
                    sb_printf(&b, "\\u%04x", *p);
                else
                    sb_putc(&b, (char)*p);
            }
        }
    }
    if (!b.data)
        return xstrdup("");
    return sb_steal(&b);
}

void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

int secure_eq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb)
        return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* ------------------------------------------------------------------ */
/* random                                                              */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_rand_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_rand_fd = -1;
static uint64_t g_prng_state = 0;

static uint64_t prng_next(void)
{
    if (g_prng_state == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        g_prng_state = ((uint64_t)tv.tv_sec << 32) ^ (uint64_t)tv.tv_usec ^
                       ((uint64_t)getpid() << 16) ^ (uint64_t)(uintptr_t)&tv;
        if (g_prng_state == 0)
            g_prng_state = 0x9E3779B97F4A7C15ULL;
    }
    uint64_t x = g_prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_prng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

void rc_random_bytes(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    pthread_mutex_lock(&g_rand_mutex);
    if (g_rand_fd < 0)
        g_rand_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    if (g_rand_fd >= 0) {
        while (got < n) {
            ssize_t r = read(g_rand_fd, p + got, n - got);
            if (r <= 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            got += (size_t)r;
        }
    }
    pthread_mutex_unlock(&g_rand_mutex);
    while (got < n) {
        uint64_t r = prng_next();
        size_t chunk = n - got < sizeof(r) ? n - got : sizeof(r);
        memcpy(p + got, &r, chunk);
        got += chunk;
    }
}

uint64_t rc_rand64(void)
{
    uint64_t v = 0;
    rc_random_bytes(&v, sizeof(v));
    return v;
}

int rc_rand_range(int n)
{
    if (n <= 1)
        return 0;
    uint64_t limit = UINT64_MAX - (UINT64_MAX % (uint64_t)n);
    uint64_t v;
    do {
        v = rc_rand64();
    } while (v >= limit);
    return (int)(v % (uint64_t)n);
}

void rc_shuffle(void *base, size_t n, size_t width)
{
    unsigned char *p = (unsigned char *)base;
    unsigned char tmp[512];
    if (width > sizeof(tmp))
        return;
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)rc_rand_range((int)i);
        if (i - 1 == j)
            continue;
        memcpy(tmp, p + (i - 1) * width, width);
        memcpy(p + (i - 1) * width, p + j * width, width);
        memcpy(p + j * width, tmp, width);
    }
}

void random_hex(char *out, size_t hex_chars)
{
    size_t bytes = (hex_chars + 1) / 2;
    unsigned char *buf = xmalloc(bytes);
    rc_random_bytes(buf, bytes);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < hex_chars; i++) {
        unsigned char b = buf[i / 2];
        out[i] = (i % 2 == 0) ? hex[b >> 4] : hex[b & 0x0F];
    }
    out[hex_chars] = '\0';
    secure_wipe(buf, bytes);
    free(buf);
}

char *random_password(size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    char *p = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++)
        p[i] = alphabet[rc_rand_range((int)sizeof(alphabet) - 1)];
    p[len] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* SHA-256 / HMAC / PBKDF2                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t state[8];
    uint64_t bits;
    unsigned char buf[64];
    size_t buflen;
} sha256_ctx;

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *ctx, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bits = 0;
    ctx->buflen = 0;
}

static void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    ctx->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t space = 64 - ctx->buflen;
        size_t take = len < space ? len : space;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, unsigned char out[32])
{
    uint64_t bits = ctx->bits;
    unsigned char pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    size_t padlen = (ctx->buflen < 56) ? (56 - ctx->buflen) : (120 - ctx->buflen);
    sha256_update(ctx, pad, padlen);
    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(ctx, lenbuf, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
    }
}

void rc_sha256(const void *data, size_t len, unsigned char out[32])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void rc_sha256_hex(const void *data, size_t len, char out[65])
{
    unsigned char digest[32];
    rc_sha256(data, len, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

static void hmac_sha256(const unsigned char *key, size_t keylen,
                        const unsigned char *msg, size_t msglen,
                        unsigned char out[32])
{
    unsigned char k[64];
    memset(k, 0, sizeof(k));
    if (keylen > 64) {
        sha256_ctx t;
        sha256_init(&t);
        sha256_update(&t, key, keylen);
        sha256_final(&t, k);
    } else {
        memcpy(k, key, keylen);
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_ctx ctx;
    unsigned char inner[32];
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, msg, msglen);
    sha256_final(&ctx, inner);
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
    secure_wipe(k, sizeof(k));
    secure_wipe(ipad, sizeof(ipad));
    secure_wipe(opad, sizeof(opad));
    secure_wipe(inner, sizeof(inner));
}

#define PBKDF2_ITERATIONS 120000
#define PBKDF2_SALT_LEN 16
#define PBKDF2_DKLEN 32

char *password_hash(const char *password)
{
    unsigned char salt[PBKDF2_SALT_LEN];
    rc_random_bytes(salt, sizeof(salt));
    unsigned char dk[PBKDF2_DKLEN];
    unsigned char block[32];
    unsigned char U[32];
    unsigned char input[PBKDF2_SALT_LEN + 4];
    memcpy(input, salt, PBKDF2_SALT_LEN);
    input[PBKDF2_SALT_LEN] = 0; input[PBKDF2_SALT_LEN + 1] = 0;
    input[PBKDF2_SALT_LEN + 2] = 0; input[PBKDF2_SALT_LEN + 3] = 1;
    hmac_sha256((const unsigned char *)password, strlen(password), input,
                sizeof(input), U);
    memcpy(block, U, 32);
    for (int iter = 1; iter < PBKDF2_ITERATIONS; iter++) {
        hmac_sha256((const unsigned char *)password, strlen(password), U, 32, U);
        for (int i = 0; i < 32; i++)
            block[i] ^= U[i];
    }
    memcpy(dk, block, PBKDF2_DKLEN);

    static const char hex[] = "0123456789abcdef";
    char salt_hex[PBKDF2_SALT_LEN * 2 + 1];
    char hash_hex[PBKDF2_DKLEN * 2 + 1];
    for (int i = 0; i < PBKDF2_SALT_LEN; i++) {
        salt_hex[i * 2] = hex[salt[i] >> 4];
        salt_hex[i * 2 + 1] = hex[salt[i] & 0x0F];
    }
    salt_hex[PBKDF2_SALT_LEN * 2] = '\0';
    for (int i = 0; i < PBKDF2_DKLEN; i++) {
        hash_hex[i * 2] = hex[dk[i] >> 4];
        hash_hex[i * 2 + 1] = hex[dk[i] & 0x0F];
    }
    hash_hex[PBKDF2_DKLEN * 2] = '\0';
    char *out = NULL;
    if (asprintf(&out, "pbkdf2$sha256$%d$%s$%s", PBKDF2_ITERATIONS, salt_hex,
                 hash_hex) < 0)
        out = xstrdup("pbkdf2$sha256$0$$");
    secure_wipe(salt, sizeof(salt));
    secure_wipe(dk, sizeof(dk));
    secure_wipe(block, sizeof(block));
    secure_wipe(U, sizeof(U));
    return out;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *s, unsigned char *out, size_t outlen)
{
    if (strlen(s) != outlen * 2)
        return 0;
    for (size_t i = 0; i < outlen; i++) {
        int hi = hex_val(s[i * 2]), lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

int password_verify(const char *password, const char *encoded)
{
    if (!password || !encoded)
        return 0;
    char *copy = xstrdup(encoded);
    char *save = NULL;
    char *part = strtok_r(copy, "$", &save);
    char *algo = strtok_r(NULL, "$", &save);
    char *iter_s = strtok_r(NULL, "$", &save);
    char *salt_s = strtok_r(NULL, "$", &save);
    char *hash_s = strtok_r(NULL, "$", &save);
    int ok = 0;
    if (part && algo && iter_s && salt_s && hash_s && strcmp(part, "pbkdf2") == 0 &&
        strcmp(algo, "sha256") == 0) {
        long iter = strtol(iter_s, NULL, 10);
        unsigned char salt[64];
        unsigned char want[PBKDF2_DKLEN];
        size_t saltlen = strlen(salt_s) / 2;
        if (iter > 0 && saltlen <= sizeof(salt) && hex_decode(salt_s, salt, saltlen) &&
            hex_decode(hash_s, want, PBKDF2_DKLEN)) {
            unsigned char dk[32], block[32], U[32];
            size_t input_len = saltlen + 4;
            unsigned char *input = xmalloc(input_len);
            memcpy(input, salt, saltlen);
            input[saltlen] = 0; input[saltlen + 1] = 0;
            input[saltlen + 2] = 0; input[saltlen + 3] = 1;
            hmac_sha256((const unsigned char *)password, strlen(password), input,
                        input_len, U);
            memcpy(block, U, 32);
            for (long i = 1; i < iter; i++) {
                hmac_sha256((const unsigned char *)password, strlen(password), U, 32, U);
                for (int j = 0; j < 32; j++)
                    block[j] ^= U[j];
            }
            memcpy(dk, block, 32);
            unsigned char diff = 0;
            for (int i = 0; i < 32; i++)
                diff |= dk[i] ^ want[i];
            ok = diff == 0;
            secure_wipe(dk, sizeof(dk));
            secure_wipe(block, sizeof(block));
            secure_wipe(U, sizeof(U));
            free(input);
        }
    }
    secure_wipe(copy, strlen(copy));
    free(copy);
    return ok;
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */
int64_t rc_now(void)
{
    return (int64_t)time(NULL);
}

int64_t rc_week_start(int64_t ts)
{
    time_t t = (time_t)(ts + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    int wday = tm.tm_wday; /* 0=Sunday */
    int days_from_monday = (wday == 0) ? 6 : wday - 1;
    int64_t local_midnight = ts - (int64_t)tm.tm_hour * 3600 -
                             (int64_t)tm.tm_min * 60 - (int64_t)tm.tm_sec;
    return local_midnight - (int64_t)days_from_monday * 86400;
}

int64_t rc_week_end(int64_t ts)
{
    return rc_week_start(ts) + 7 * 86400 - 1;
}

int64_t rc_day_start(int64_t ts)
{
    time_t t = (time_t)(ts + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    return ts - (int64_t)tm.tm_hour * 3600 - (int64_t)tm.tm_min * 60 -
           (int64_t)tm.tm_sec;
}

int64_t rc_date_start(const char *date)
{
    if (!date || strlen(date) != 10)
        return -1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(date, "%Y-%m-%d", &tm))
        return -1;
    int64_t epoch = (int64_t)timegm(&tm);
    return epoch - 8 * 3600;
}

int rc_iso_week(int64_t week_start, int *iso_year, int *iso_week)
{
    time_t t = (time_t)(week_start + 8 * 3600 + 12 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    if (!strftime(buf, sizeof(buf), "%G %V", &tm))
        return 0;
    int y = 0, w = 0;
    if (sscanf(buf, "%d %d", &y, &w) != 2)
        return 0;
    if (iso_year) *iso_year = y;
    if (iso_week) *iso_week = w;
    return 1;
}

void rc_fmt_datetime(int64_t ts, char *out, size_t outsz)
{
    time_t t = (time_t)(ts + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, outsz, "%Y-%m-%d %H:%M:%S", &tm);
}

void rc_fmt_date(int64_t ts, char *out, size_t outsz)
{
    time_t t = (time_t)(ts + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, outsz, "%Y-%m-%d", &tm);
}

void rc_fmt_time(int64_t ts, char *out, size_t outsz)
{
    time_t t = (time_t)(ts + 8 * 3600);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, outsz, "%H:%M:%S", &tm);
}

int rc_weekday(int day_index)
{
    /* day 0=Mon .. 6=Sun -> tm_wday 1..6,0 */
    return (day_index + 1) % 7;
}

const char *rc_weekday_name(int day_index)
{
    static const char *names[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    if (day_index < 0 || day_index > 6)
        return "";
    return names[day_index];
}

int rc_day_is_check_day(int day_index)
{
    return day_index >= 0 && day_index <= 6 && day_index != 3;
}

int rc_same_day(int64_t a, int64_t b)
{
    return rc_day_start(a) == rc_day_start(b);
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *url_decode(const char *s)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = hex_digit(s[i + 1]), lo = hex_digit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

char *url_encode(const char *s)
{
    if (!s)
        return xstrdup("");
    static const char hex[] = "0123456789ABCDEF";
    strbuf_t b;
    sb_init(&b);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            sb_putc(&b, (char)*p);
        } else {
            sb_putc(&b, '%');
            sb_putc(&b, hex[*p >> 4]);
            sb_putc(&b, hex[*p & 0x0F]);
        }
    }
    if (!b.data)
        return xstrdup("");
    return sb_steal(&b);
}

/* ------------------------------------------------------------------ */
/* filesystem                                                          */
/* ------------------------------------------------------------------ */
int file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

int is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mkdir_p(const char *path, int mode)
{
    if (!path || !*path)
        return 0;
    char *copy = xstrdup(path);
    size_t len = strlen(copy);
    if (len == 0) {
        free(copy);
        return 0;
    }
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int file_read(const char *path, char **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    char *buf = xmalloc(size + 1);
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            free(buf);
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    *data = buf;
    *len = got;
    return 0;
}

int file_write(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0)
        return -1;
    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int copy_file(const char *src, const char *dst)
{
    char *data;
    size_t len;
    if (file_read(src, &data, &len) != 0)
        return -1;
    int rc = file_write(dst, data, len);
    free(data);
    return rc;
}

int dir_list(const char *path, char ***names, int *count, int files_only)
{
    *names = NULL;
    *count = 0;
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int cap = 16, n = 0;
    char **arr = xcalloc((size_t)cap, sizeof(char *));
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char *full = path_join(path, de->d_name);
        int ok = 1;
        if (files_only == 1)
            ok = !is_dir(full);
        else if (files_only == 0)
            ok = is_dir(full);
        free(full);
        if (!ok)
            continue;
        if (n == cap) {
            cap *= 2;
            arr = realloc(arr, (size_t)cap * sizeof(char *));
            if (!arr)
                abort();
        }
        arr[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    /* insertion sort by name (ascending) */
    for (int i = 1; i < n; i++) {
        char *key = arr[i];
        int j = i - 1;
        while (j >= 0 && strcmp(arr[j], key) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    *names = arr;
    *count = n;
    return 0;
}

int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char *full = path_join(path, de->d_name);
            if (remove_tree(full) != 0)
                rc = -1;
            free(full);
        }
        closedir(d);
        if (rmdir(path) != 0)
            rc = -1;
        return rc;
    }
    return unlink(path) == 0 ? 0 : (errno == ENOENT ? 0 : -1);
}

int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0)
        return errno == ENOENT ? 1 : -1;
    if (!S_ISDIR(st.st_mode))
        return -1;
    if (mkdir_p(dst, 0750) != 0)
        return -1;
    char **names;
    int count;
    if (dir_list(src, &names, &count, -1) != 0)
        return -1;
    int rc = 0;
    for (int i = 0; i < count; i++) {
        char *s = path_join(src, names[i]);
        char *d = path_join(dst, names[i]);
        struct stat es;
        if (stat(s, &es) == 0) {
            if (S_ISDIR(es.st_mode)) {
                if (copy_tree(s, d) != 0)
                    rc = -1;
            } else if (S_ISREG(es.st_mode)) {
                if (copy_file(s, d) != 0)
                    rc = -1;
            }
        }
        free(s);
        free(d);
        free(names[i]);
    }
    free(names);
    return rc;
}

char *path_join(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && a[la - 1] == '/')
        la--;
    size_t need = la + 1 + lb + 1;
    char *p = xmalloc(need);
    memcpy(p, a, la);
    if (lb > 0) {
        if (la > 0)
            p[la++] = '/';
        memcpy(p + la, b, lb);
        la += lb;
    }
    p[la] = '\0';
    return p;
}

const char *path_basename(const char *path)
{
    if (!path)
        return "";
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

const char *path_ext(const char *path)
{
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

int path_is_safe(const char *path)
{
    if (!path || path[0] == '/' || path[0] == '\0')
        return 0;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg == 0 || seg > 255)
            return 0;
        if (seg == 1 && p[0] == '.')
            return 0;
        if (seg == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        if (!slash)
            break;
        p = slash + 1;
    }
    for (const char *q = path; *q; q++)
        if ((unsigned char)*q < 0x20 || *q == '\\')
            return 0;
    return 1;
}
