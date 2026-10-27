/* Premium plants for gems.
 *
 * Offline, the 4.5.2 shop cannot sell the real-money premium plants ("service
 * unavailable"). The shop catalog is PACKAGES\MAGENTO.RTON inside the
 * Packages resource group. For every premium plant product this rewrites
 *     "Prices": [{"Price": <USD>}]   into   "PriceGems": "<n>", "Prices": [{"Price": 0}]
 * so the shop sells it for gems. Gems still have to be earned in the game.
 *
 * The user's OBB is changed in place, the same way the tested OBB mod was
 * built: the rebuilt Packages group is appended after the end of the file,
 * then three header records are repointed to it. The old group stays in the
 * file untouched. The original record bytes and file size are saved first, so
 * creating userdata/gem_shop_off.txt restores the original archive on the next
 * launch.
 *
 * Safety: the catalog is decoded and must re-encode to exactly the original
 * bytes before anything is changed. Any unexpected layout leaves the archive
 * untouched. */
#include "utils/gem_shop.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reimpl/miniz_zlib.h"

#ifdef GEM_SHOP_HOST_TEST
#define telemetry_log(tag, ...) (printf("[%s] ", tag), printf(__VA_ARGS__), printf("\n"))
#ifdef _WIN32
#include <io.h>
#endif
#else
#include "utils/telemetry.h"
#include <psp2/io/stat.h>
#endif

#define GROUP_NAME "Packages"
#define CATALOG_FILE "PACKAGES\\MAGENTO.RTON"
#define ALIGN 0x1000u

/* Gem price per plant, about 11 gems per dollar (the store's 110 gems = $9.99). */
static const struct { const char *item; unsigned gems; } k_prices[] = {
    {"imitater", 35}, {"jalapeno", 35}, {"peanut", 35}, {"powerlily", 35}, {"squash", 35},
    {"chomper", 45}, {"hypnoshroom", 45}, {"sapfling", 45}, {"snowpea", 45},
    {"starfruit", 45}, {"torchwood", 45}, {"hurrikale", 45},
    {"toadstool", 55}, {"strawburst", 55}, {"cactus", 55}, {"firepeashooter", 55},
    {"ghostpepper", 55}, {"homingthistle", 55}, {"sweetpotato", 55}, {"lavaguava", 55},
    {"electricblueberry", 75}, {"jackolantern", 75}, {"grapeshot", 75},
    {"coldsnapdragon", 75}, {"escaperoot", 75}, {"shrinkingviolet", 75}, {"bloominghearts", 75},
};

/* ---- small helpers ------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t align_up(uint32_t v) { return (v + ALIGN - 1u) & ~(ALIGN - 1u); }

static int read_at(FILE *f, uint32_t off, void *buf, uint32_t n) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}
static int write_at(FILE *f, uint32_t off, const void *buf, uint32_t n) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fwrite(buf, 1, n, f) == n;
}
static long file_size(FILE *f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

/* ---- arena -------------------------------------------------------------- */

typedef struct Chunk { struct Chunk *next; size_t used, cap; } Chunk;
typedef struct { Chunk *head; } Arena;

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 7u) & ~(size_t)7u;
    if (!a->head || a->head->cap - a->head->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        Chunk *c = (Chunk *)malloc(sizeof(Chunk) + cap);
        if (!c) return NULL;
        c->next = a->head; c->used = 0; c->cap = cap;
        a->head = c;
    }
    void *p = (uint8_t *)(a->head + 1) + a->head->used;
    a->head->used += n;
    return p;
}
static void arena_free(Arena *a) {
    while (a->head) { Chunk *n = a->head->next; free(a->head); a->head = n; }
}

/* ---- RTON tree ------------------------------------------------------------
 * Only the encoding subset our reference encoder produces is accepted; the
 * byte-exact round-trip check below proves nothing else is present. */

enum { N_FALSE, N_TRUE, N_NULL, N_INT, N_DBL, N_STR, N_RTID, N_ARR, N_OBJ };

typedef struct Node {
    uint8_t type;
    int64_t i;
    double d;
    const uint8_t *s; uint32_t n, chars;     /* STR; RTID first part */
    const uint8_t *s2; uint32_t n2, chars2;  /* RTID second part */
    struct Node **kids; uint32_t count;      /* ARR values; OBJ key,value pairs */
} Node;

typedef struct {
    const uint8_t *b; size_t n, p;
    int err, depth;
    Arena *arena;
    const uint8_t **cs; uint32_t *cn; uint32_t cc, ccap;  /* string cache */
} Dec;

static int dec_u8(Dec *d) {
    if (d->p >= d->n) { d->err = 1; return 0; }
    return d->b[d->p++];
}
static uint64_t dec_vu(Dec *d) {
    uint64_t r = 0;
    for (int sh = 0; sh < 64; sh += 7) {
        int c = dec_u8(d);
        if (d->err) return 0;
        r |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) return r;
    }
    d->err = 1;
    return 0;
}
static const uint8_t *dec_take(Dec *d, uint64_t n) {
    if (n > d->n - d->p) { d->err = 1; return NULL; }
    const uint8_t *p = d->b + d->p;
    d->p += (size_t)n;
    return p;
}
static Node *new_node(Dec *d, uint8_t type) {
    Node *x = (Node *)arena_alloc(d->arena, sizeof(Node));
    if (!x) { d->err = 1; return NULL; }
    memset(x, 0, sizeof(*x));
    x->type = type;
    return x;
}

static Node *dec_val(Dec *d, int t);

static Node *dec_obj(Dec *d) {
    if (++d->depth > 64) { d->err = 1; return NULL; }
    Node **tmp = NULL; uint32_t count = 0, cap = 0;
    for (;;) {
        int t = dec_u8(d);
        if (d->err) break;
        if (t == 0xff) break;
        Node *k = dec_val(d, t);
        Node *v = d->err ? NULL : dec_val(d, dec_u8(d));
        if (d->err) break;
        if (k->type != N_STR) { d->err = 1; break; }
        if (count + 2 > cap) {
            cap = cap ? cap * 2 : 16;
            Node **grown = (Node **)realloc(tmp, cap * sizeof(Node *));
            if (!grown) { d->err = 1; break; }
            tmp = grown;
        }
        tmp[count++] = k; tmp[count++] = v;
    }
    Node *o = d->err ? NULL : new_node(d, N_OBJ);
    if (o && count) {
        o->kids = (Node **)arena_alloc(d->arena, count * sizeof(Node *));
        if (!o->kids) d->err = 1;
        else memcpy(o->kids, tmp, count * sizeof(Node *));
        o->count = count;
    }
    free(tmp);
    --d->depth;
    return d->err ? NULL : o;
}

static Node *dec_val(Dec *d, int t) {
    if (d->err) return NULL;
    Node *x;
    switch (t) {
    case 0x00: return new_node(d, N_FALSE);
    case 0x01: return new_node(d, N_TRUE);
    case 0x02: return new_node(d, N_NULL);
    case 0x21: return new_node(d, N_INT);
    case 0x24:
        if ((x = new_node(d, N_INT))) x->i = (int64_t)dec_vu(d);
        return x;
    case 0x25:
        if ((x = new_node(d, N_INT))) {
            uint64_t v = dec_vu(d);
            x->i = (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
        }
        return x;
    case 0x42:
        if ((x = new_node(d, N_DBL))) {
            const uint8_t *p = dec_take(d, 8);
            if (p) memcpy(&x->d, p, 8);
        }
        return x;
    case 0x90:
        if ((x = new_node(d, N_STR))) {
            uint64_t n = dec_vu(d);
            x->s = dec_take(d, n); x->n = (uint32_t)n;
            if (d->err) return NULL;
            if (d->cc == d->ccap) {
                uint32_t cap = d->ccap ? d->ccap * 2 : 256;
                const uint8_t **cs = (const uint8_t **)realloc(d->cs, cap * sizeof(*cs));
                if (cs) d->cs = cs;
                uint32_t *cn = (uint32_t *)realloc(d->cn, cap * sizeof(*cn));
                if (cn) d->cn = cn;
                if (!cs || !cn) { d->err = 1; return NULL; }
                d->ccap = cap;
            }
            d->cs[d->cc] = x->s; d->cn[d->cc] = x->n; ++d->cc;
        }
        return x;
    case 0x91:
        if ((x = new_node(d, N_STR))) {
            uint64_t i = dec_vu(d);
            if (d->err || i >= d->cc) { d->err = 1; return NULL; }
            x->s = d->cs[i]; x->n = d->cn[i];
        }
        return x;
    case 0x83:
        if (dec_u8(d) != 3) { d->err = 1; return NULL; }
        if ((x = new_node(d, N_RTID))) {
            x->chars = (uint32_t)dec_vu(d); x->n = (uint32_t)dec_vu(d); x->s = dec_take(d, x->n);
            x->chars2 = (uint32_t)dec_vu(d); x->n2 = (uint32_t)dec_vu(d); x->s2 = dec_take(d, x->n2);
        }
        return d->err ? NULL : x;
    case 0x85:
        return dec_obj(d);
    case 0x86: {
        if (dec_u8(d) != 0xfd) { d->err = 1; return NULL; }
        uint64_t n = dec_vu(d);
        if (d->err || n > d->n - d->p || ++d->depth > 64) { d->err = 1; return NULL; }
        x = new_node(d, N_ARR);
        if (!x) return NULL;
        x->count = (uint32_t)n;
        x->kids = n ? (Node **)arena_alloc(d->arena, (size_t)n * sizeof(Node *)) : NULL;
        if (n && !x->kids) { d->err = 1; return NULL; }
        for (uint32_t i = 0; i < n && !d->err; ++i) x->kids[i] = dec_val(d, dec_u8(d));
        if (!d->err && dec_u8(d) != 0xfe) d->err = 1;
        --d->depth;
        return d->err ? NULL : x;
    }
    default:
        d->err = 1;
        return NULL;
    }
}

typedef struct { const uint8_t *s; uint32_t n, index; } CacheSlot;
typedef struct {
    uint8_t *b; size_t n, cap; int err;
    CacheSlot *slots; uint32_t scap, count;
} Enc;

static void enc_put(Enc *e, const void *p, size_t n) {
    if (e->err) return;
    if (e->n + n > e->cap) {
        size_t cap = e->cap ? e->cap : 65536;
        while (cap < e->n + n) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(e->b, cap);
        if (!b) { e->err = 1; return; }
        e->b = b; e->cap = cap;
    }
    memcpy(e->b + e->n, p, n);
    e->n += n;
}
static void enc_u8(Enc *e, uint8_t v) { enc_put(e, &v, 1); }
static void enc_vu(Enc *e, uint64_t v) {
    uint8_t buf[10]; int n = 0;
    do { uint8_t b = v & 0x7f; v >>= 7; buf[n++] = v ? (uint8_t)(b | 0x80) : b; } while (v);
    enc_put(e, buf, (size_t)n);
}
static uint32_t str_hash(const uint8_t *s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ s[i]) * 16777619u;
    return h;
}
/* Same caching rule as the reference encoder: first use writes the string,
 * later uses refer to its index. */
static void enc_str(Enc *e, const uint8_t *s, uint32_t n) {
    if (e->err) return;
    if ((e->count + 1) * 2 > e->scap) {
        uint32_t cap = e->scap ? e->scap * 2 : 1024;
        CacheSlot *slots = (CacheSlot *)calloc(cap, sizeof(CacheSlot));
        if (!slots) { e->err = 1; return; }
        for (uint32_t i = 0; i < e->scap; ++i) {
            if (!e->slots[i].s) continue;
            uint32_t h = str_hash(e->slots[i].s, e->slots[i].n) & (cap - 1);
            while (slots[h].s) h = (h + 1) & (cap - 1);
            slots[h] = e->slots[i];
        }
        free(e->slots);
        e->slots = slots; e->scap = cap;
    }
    static const uint8_t empty = 0;
    if (!s) s = &empty;
    uint32_t h = str_hash(s, n) & (e->scap - 1);
    while (e->slots[h].s) {
        if (e->slots[h].n == n && !memcmp(e->slots[h].s, s, n)) {
            enc_u8(e, 0x91); enc_vu(e, e->slots[h].index);
            return;
        }
        h = (h + 1) & (e->scap - 1);
    }
    e->slots[h].s = s; e->slots[h].n = n; e->slots[h].index = e->count++;
    enc_u8(e, 0x90); enc_vu(e, n); enc_put(e, s, n);
}
static void enc_val(Enc *e, const Node *x);
static void enc_body(Enc *e, const Node *o) {
    for (uint32_t i = 0; i + 1 < o->count; i += 2) {
        enc_str(e, o->kids[i]->s, o->kids[i]->n);
        enc_val(e, o->kids[i + 1]);
    }
    enc_u8(e, 0xff);
}
static void enc_val(Enc *e, const Node *x) {
    switch (x->type) {
    case N_FALSE: enc_u8(e, 0x00); break;
    case N_TRUE: enc_u8(e, 0x01); break;
    case N_NULL: enc_u8(e, 0x02); break;
    case N_INT:
        if (x->i == 0) enc_u8(e, 0x21);
        else if (x->i > 0) { enc_u8(e, 0x24); enc_vu(e, (uint64_t)x->i); }
        else { enc_u8(e, 0x25); enc_vu(e, ((uint64_t)(-x->i) << 1) - 1); }
        break;
    case N_DBL: enc_u8(e, 0x42); enc_put(e, &x->d, 8); break;
    case N_STR: enc_str(e, x->s, x->n); break;
    case N_RTID:
        enc_u8(e, 0x83); enc_u8(e, 0x03);
        enc_vu(e, x->chars); enc_vu(e, x->n); enc_put(e, x->s, x->n);
        enc_vu(e, x->chars2); enc_vu(e, x->n2); enc_put(e, x->s2, x->n2);
        break;
    case N_ARR:
        enc_u8(e, 0x86); enc_u8(e, 0xfd); enc_vu(e, x->count);
        for (uint32_t i = 0; i < x->count; ++i) enc_val(e, x->kids[i]);
        enc_u8(e, 0xfe);
        break;
    case N_OBJ: enc_u8(e, 0x85); enc_body(e, x); break;
    }
}
static uint8_t *encode_rton(const Node *root, size_t *out_n) {
    Enc e = {0};
    enc_put(&e, "RTON\x01\x00\x00\x00", 8);
    enc_body(&e, root);
    enc_put(&e, "DONE", 4);
    free(e.slots);
    if (e.err) { free(e.b); return NULL; }
    *out_n = e.n;
    return e.b;
}

/* ---- catalog change ------------------------------------------------------ */

static int str_is(const Node *x, const char *s) {
    size_t n = strlen(s);
    return x && x->type == N_STR && x->n == n && !memcmp(x->s, s, n);
}
static Node *obj_get(const Node *o, const char *key) {
    if (!o || o->type != N_OBJ) return NULL;
    for (uint32_t i = 0; i + 1 < o->count; i += 2)
        if (str_is(o->kids[i], key)) return o->kids[i + 1];
    return NULL;
}
static Node *mk(Arena *a, uint8_t type) {
    Node *x = (Node *)arena_alloc(a, sizeof(Node));
    if (x) { memset(x, 0, sizeof(*x)); x->type = type; }
    return x;
}
static Node *mk_str(Arena *a, const char *s) {
    Node *x = mk(a, N_STR);
    if (x) { x->s = (const uint8_t *)s; x->n = (uint32_t)strlen(s); }
    return x;
}

/* Returns the number of plant products rewritten, or -1 on allocation failure. */
static int convert_catalog(Arena *a, Node *root, char (*digits)[8]) {
    Node *objects = obj_get(root, "objects");
    if (!objects || objects->type != N_ARR) return 0;
    int converted = 0;
    for (uint32_t i = 0; i < objects->count; ++i) {
        Node *o = objects->kids[i];
        Node *data = obj_get(o, "objdata");
        if (!str_is(obj_get(o, "objclass"), "MagentoProductProps") ||
            !str_is(obj_get(data, "ObjectType"), "plant"))
            continue;
        Node *item = obj_get(data, "ObjectItem");
        int gems = -1;
        for (size_t k = 0; k < sizeof(k_prices) / sizeof(k_prices[0]); ++k)
            if (str_is(item, k_prices[k].item)) { gems = (int)k_prices[k].gems; break; }
        if (gems < 0) continue;

        Node **kids = (Node **)arena_alloc(a, (data->count + 2) * sizeof(Node *));
        if (!kids) return -1;
        uint32_t n = 0;
        for (uint32_t k = 0; k + 1 < data->count; k += 2) {
            Node *key = data->kids[k];
            if (str_is(key, "PriceGems")) continue;
            if (str_is(key, "Prices")) {
                Node *price = mk(a, N_OBJ), *zero = mk(a, N_INT), *list = mk(a, N_ARR);
                Node **pk = (Node **)arena_alloc(a, 2 * sizeof(Node *));
                Node **lk = (Node **)arena_alloc(a, sizeof(Node *));
                Node *gk = mk_str(a, "PriceGems"), *pkey = mk_str(a, "Price");
                Node *gv = mk_str(a, digits[gems]);
                if (!price || !zero || !list || !pk || !lk || !gk || !pkey || !gv) return -1;
                pk[0] = pkey; pk[1] = zero;
                price->kids = pk; price->count = 2;
                lk[0] = price;
                list->kids = lk; list->count = 1;
                kids[n++] = gk; kids[n++] = gv;
                kids[n++] = key; kids[n++] = list;
            } else {
                kids[n++] = key; kids[n++] = data->kids[k + 1];
            }
        }
        data->kids = kids; data->count = n;
        ++converted;
    }
    return converted;
}

/* ---- RSB / RSG layout ---------------------------------------------------- */

typedef struct { uint32_t off, len; uint8_t orig[16], next[16]; } Patch;
typedef struct {
    uint32_t magic, version, orig_size, new_size;
    Patch p[3];
} State;
#define STATE_MAGIC 0x31485347u /* "GSH1" */

/* Find a file in an RSG file-list trie. Returns the entry position or -1. */
static long find_in_list(const uint8_t *list, uint32_t len, const char *want,
                         uint32_t *flag, uint32_t *off, uint32_t *size) {
    char name[256]; uint32_t name_len = 0;
    struct { uint32_t len, pos; } stack[256]; uint32_t depth = 0;
    uint32_t pos = 0, visits = 0;
    while (pos + 4 <= len) {
        if (++visits > len / 4 + 1) return -1;
        uint8_t ch = list[pos];
        uint32_t branch = list[pos + 1] | (list[pos + 2] << 8) | ((uint32_t)list[pos + 3] << 16);
        if (ch == 0) {
            if (pos + 16 > len) return -1;
            uint32_t f = rd32(list + pos + 4);
            if (name_len && name_len == strlen(want) && !memcmp(name, want, name_len)) {
                *flag = f; *off = rd32(list + pos + 8); *size = rd32(list + pos + 12);
                return (long)pos;
            }
            pos += 16 + (f == 1 ? 20 : 0);
            if (!depth) {
                if (!name_len) break;
                name_len = 0;
                continue;
            }
            --depth;
            name_len = stack[depth].len;
            pos = stack[depth].pos;
        } else {
            if (branch) {
                if (depth == 256) return -1;
                stack[depth].len = name_len; stack[depth].pos = branch * 4; ++depth;
            }
            if (name_len == sizeof(name)) return -1;
            name[name_len++] = (char)ch;
            pos += 4;
        }
    }
    return -1;
}

static int read_state(const char *path, State *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(s, sizeof(*s), 1, f) == 1 && fgetc(f) == EOF &&
             s->magic == STATE_MAGIC && s->version == 1;
    fclose(f);
    for (int i = 0; ok && i < 3; ++i) ok = s->p[i].len && s->p[i].len <= 16;
    return ok;
}
static int write_state(const char *path, const State *s) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(s, sizeof(*s), 1, f) == 1;
    if (fclose(f) != 0) ok = 0;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    if (!ok) remove(tmp);
    return ok;
}

/* 1 = all patch ranges hold `next`, 0 = all hold `orig`, -1 = neither. */
static int header_matches(FILE *f, const State *s) {
    int all_next = 1, all_orig = 1;
    for (int i = 0; i < 3; ++i) {
        uint8_t cur[16];
        if (!read_at(f, s->p[i].off, cur, s->p[i].len)) return -1;
        if (memcmp(cur, s->p[i].next, s->p[i].len)) all_next = 0;
        if (memcmp(cur, s->p[i].orig, s->p[i].len)) all_orig = 0;
    }
    return all_next ? 1 : all_orig ? 0 : -1;
}
static int write_header(FILE *f, const State *s, int use_next) {
    for (int i = 0; i < 3; ++i)
        if (!write_at(f, s->p[i].off, use_next ? s->p[i].next : s->p[i].orig, s->p[i].len)) return 0;
    return fflush(f) == 0;
}
static int truncate_file(const char *path, uint32_t size) {
#ifdef GEM_SHOP_HOST_TEST
#ifdef _WIN32
    FILE *f = fopen(path, "r+b");
    if (!f) return 0;
    int ok = _chsize_s(_fileno(f), size) == 0;
    fclose(f);
    return ok;
#else
    extern int truncate(const char *, long);
    return truncate(path, (long)size) == 0;
#endif
#else
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    st.st_size = size;
    return sceIoChstat(path, &st, SCE_CST_SIZE) >= 0;
#endif
}

/* Undo or finish a previous run described by the state file. Returns a
 * GEM_SHOP_* result, or 0 when the state no longer describes this archive. */
static int resume_state(const char *obb_path, const char *state_path, int enabled) {
    State s;
    if (!read_state(state_path, &s)) return 0;
    FILE *f = fopen(obb_path, "r+b");
    if (!f) return GEM_SHOP_IO_ERROR;
    long size = file_size(f);
    int hdr = header_matches(f, &s);
    int result = 0;
    if (size == (long)s.new_size && hdr == 1) {
        if (enabled) result = GEM_SHOP_ALREADY;
        else if (write_header(f, &s, 0)) result = GEM_SHOP_RESTORED;
        else result = GEM_SHOP_IO_ERROR;
    } else if (size == (long)s.new_size && hdr == 0) {
        /* Interrupted after the append: finish or undo it. */
        if (enabled) result = write_header(f, &s, 1) ? GEM_SHOP_APPLIED : GEM_SHOP_IO_ERROR;
        else result = GEM_SHOP_RESTORED;
    }
    fclose(f);
    if (result == GEM_SHOP_RESTORED) {
        if (!truncate_file(obb_path, s.orig_size))
            telemetry_log("GEMSHOP", "restored header; could not trim %u appended bytes",
                          (unsigned)(s.new_size - s.orig_size));
        remove(state_path);
    } else if (!result) {
        /* A different archive was installed since: forget the old state. */
        remove(state_path);
    }
    return result;
}

int gem_shop_run(const char *obb_path, const char *state_path, int enabled) {
    int resumed = resume_state(obb_path, state_path, enabled);
    if (resumed) return resumed;
    if (!enabled) return GEM_SHOP_OFF;

    int result = GEM_SHOP_SKIPPED;
    uint8_t *group = NULL, *block = NULL, *catalog_out = NULL, *packed = NULL, *records = NULL;
    Arena arena = {0};
    Dec dec = {0};
    FILE *f = fopen(obb_path, "rb");
    if (!f) return GEM_SHOP_IO_ERROR;
#define SKIP(...) do { telemetry_log("GEMSHOP", "left unchanged: " __VA_ARGS__); goto done; } while (0)

    long obb_size = file_size(f);
    uint8_t head[0x70];
    if (obb_size <= 0 || obb_size > 0x7fff0000L || !read_at(f, 0, head, sizeof(head)) ||
        memcmp(head, "1bsr", 4))
        SKIP("not an RSB archive");
    uint32_t rsg_n = rd32(head + 0x28), rsg_begin = rd32(head + 0x2c), rsg_each = rd32(head + 0x30);
    uint32_t pool_begin = rd32(head + 0x4c), pool_each = rd32(head + 0x50);
    if (!rsg_n || rsg_n > 16384 || rsg_each < 0xa8 || pool_each < 0x84 ||
        (uint64_t)rsg_begin + (uint64_t)rsg_n * rsg_each > (uint64_t)obb_size)
        SKIP("unexpected group table");
    records = (uint8_t *)malloc((size_t)rsg_n * rsg_each);
    if (!records || !read_at(f, rsg_begin, records, rsg_n * rsg_each)) SKIP("cannot read group table");
    uint32_t rec_at = 0;
    const uint8_t *rec = NULL;
    for (uint32_t i = 0; i < rsg_n; ++i) {
        const uint8_t *r = records + (size_t)i * rsg_each;
        if (!memcmp(r, GROUP_NAME, sizeof(GROUP_NAME))) { rec = r; rec_at = rsg_begin + i * rsg_each; break; }
    }
    if (!rec) SKIP("no Packages group");
    uint32_t rsg_off = rd32(rec + 0x80), rsg_len = rd32(rec + 0x84), pool_idx = rd32(rec + 0x88);
    uint32_t pool_at = pool_begin + pool_idx * pool_each;
    if ((uint64_t)rsg_off + rsg_len > (uint64_t)obb_size || rsg_len < 0x60 || rsg_len > 64u << 20 ||
        (uint64_t)pool_at + 0x84 > (uint64_t)obb_size)
        SKIP("Packages group out of range");
    group = (uint8_t *)malloc(rsg_len);
    if (!group || !read_at(f, rsg_off, group, rsg_len)) SKIP("cannot read Packages group");
    uint32_t flags = rd32(group + 0x10), hdr_size = rd32(group + 0x14);
    uint32_t d0_off = rd32(group + 0x18), d0_csize = rd32(group + 0x1c), d0_size = rd32(group + 0x20);
    uint32_t list_len = rd32(group + 0x48), list_begin = rd32(group + 0x4c);
    if (memcmp(group, "pgsr", 4) || !(flags & 2) || d0_off != hdr_size || hdr_size > rsg_len ||
        d0_csize > rsg_len - hdr_size || !d0_size || d0_size > 64u << 20 ||
        (uint64_t)list_begin + list_len > hdr_size || rd32(group + 0x2c) || rd32(group + 0x30))
        SKIP("unexpected Packages group header");
    uint8_t pool_word[4];
    if (!read_at(f, pool_at + 0x80, pool_word, 4) || rd32(pool_word) != hdr_size + d0_size)
        SKIP("unexpected pool record");

    block = (uint8_t *)malloc(d0_size);
    mz_ulong unpacked = d0_size;
    if (!block || mz_uncompress(block, &unpacked, group + d0_off, d0_csize) != MZ_OK || unpacked != d0_size)
        SKIP("cannot unpack Packages group");

    uint32_t eflag = 0, m_off = 0, m_size = 0;
    long m_pos = find_in_list(group + list_begin, list_len, CATALOG_FILE, &eflag, &m_off, &m_size);
    if (m_pos < 0 || eflag != 0 || m_off > d0_size || m_size > d0_size - m_off || m_size < 12 ||
        memcmp(block + m_off, "RTON\x01\x00\x00\x00", 8))
        SKIP("shop catalog not found");

    dec.b = block + m_off; dec.n = m_size; dec.p = 8; dec.arena = &arena;
    Node *root = dec_obj(&dec);
    size_t check_n = 0;
    uint8_t *check = root ? encode_rton(root, &check_n) : NULL;
    int exact = check && check_n == m_size && !memcmp(check, block + m_off, m_size);
    free(check);
    if (!exact) SKIP("shop catalog uses an unexpected encoding");

    static char digits[100][8];
    for (int i = 0; i < 100; ++i) snprintf(digits[i], sizeof(digits[i]), "%d", i);
    int converted = convert_catalog(&arena, root, digits);
    if (converted < 0) SKIP("out of memory");
    size_t new_n = 0;
    catalog_out = encode_rton(root, &new_n);
    if (!catalog_out) SKIP("out of memory");
    if (!converted || (new_n == m_size && !memcmp(catalog_out, block + m_off, m_size))) {
        telemetry_log("GEMSHOP", "premium plants already cost gems in this archive");
        result = GEM_SHOP_NOT_NEEDED;
        goto done;
    }

    /* New block: old contents, then the new catalog on its own aligned page. */
    uint32_t cat_off = align_up(d0_size);
    uint32_t new_size = align_up(cat_off + (uint32_t)new_n);
    uint8_t *grown = (uint8_t *)realloc(block, new_size);
    if (!grown) SKIP("out of memory");
    block = grown;
    memset(block + d0_size, 0, new_size - d0_size);
    memcpy(block + cat_off, catalog_out, new_n);
    wr32(group + list_begin + (uint32_t)m_pos + 8, cat_off);
    wr32(group + list_begin + (uint32_t)m_pos + 12, (uint32_t)new_n);

    mz_ulong packed_n = mz_compressBound(new_size);
    packed = (uint8_t *)malloc(hdr_size + packed_n + ALIGN);
    if (!packed || mz_compress2(packed + hdr_size, &packed_n, block, new_size, 6) != MZ_OK)
        SKIP("cannot pack Packages group");
    uint32_t csize = align_up((uint32_t)packed_n);
    uint32_t group_len = hdr_size + csize;
    memcpy(packed, group, hdr_size);
    memset(packed + hdr_size + packed_n, 0, csize - packed_n);
    wr32(packed + 0x1c, csize);
    wr32(packed + 0x20, new_size);
    wr32(packed + 0x28, group_len);

    uint32_t new_rsg_off = align_up((uint32_t)obb_size);
    State s;
    memset(&s, 0, sizeof(s));
    s.magic = STATE_MAGIC; s.version = 1;
    s.orig_size = (uint32_t)obb_size; s.new_size = new_rsg_off + group_len;
    s.p[0].off = rec_at + 0x80; s.p[0].len = 8;
    s.p[1].off = rec_at + 0x98; s.p[1].len = 16;
    s.p[2].off = pool_at + 0x80; s.p[2].len = 4;
    memcpy(s.p[0].orig, rec + 0x80, 8);
    memcpy(s.p[1].orig, rec + 0x98, 16);
    memcpy(s.p[2].orig, pool_word, 4);
    wr32(s.p[0].next, new_rsg_off); wr32(s.p[0].next + 4, group_len);
    wr32(s.p[1].next, csize); wr32(s.p[1].next + 4, new_size);
    wr32(s.p[1].next + 8, new_size); wr32(s.p[1].next + 12, group_len);
    wr32(s.p[2].next, hdr_size + new_size);
    fclose(f);
    f = NULL;

    /* Append first, record how to undo it, then switch the header over. If
     * power is lost in between, the next launch finishes or undoes it. */
    FILE *w = fopen(obb_path, "r+b");
    if (!w) { result = GEM_SHOP_IO_ERROR; goto done; }
    static const uint8_t zeros[ALIGN];
    int ok = fseek(w, 0, SEEK_END) == 0 && ftell(w) == obb_size &&
             fwrite(zeros, 1, new_rsg_off - (uint32_t)obb_size, w) == new_rsg_off - (uint32_t)obb_size &&
             fwrite(packed, 1, group_len, w) == group_len && fflush(w) == 0;
    if (ok) ok = write_state(state_path, &s);
    if (ok) ok = write_header(w, &s, 1);
    if (ok) ok = header_matches(w, &s) == 1;
    if (fclose(w) != 0) ok = 0;
    if (!ok) {
        telemetry_log("GEMSHOP", "write failed; restoring the original archive");
        FILE *r = fopen(obb_path, "r+b");
        if (r) { write_header(r, &s, 0); fclose(r); }
        truncate_file(obb_path, s.orig_size);
        remove(state_path);
        result = GEM_SHOP_IO_ERROR;
        goto done;
    }
    telemetry_log("GEMSHOP", "%d premium plant products now cost gems (catalog %u -> %u bytes, +%u bytes appended)",
                  converted, (unsigned)m_size, (unsigned)new_n, (unsigned)(s.new_size - s.orig_size));
    result = GEM_SHOP_APPLIED;
#undef SKIP
done:
    if (f) fclose(f);
    free(dec.cs); free(dec.cn);
    arena_free(&arena);
    free(records); free(group); free(block); free(catalog_out); free(packed);
    return result;
}

#ifndef GEM_SHOP_HOST_TEST
#include "utils/boot_check.h"

void pvz2_gem_shop_boot(void) {
    FILE *off = fopen(DATA_PATH "gem_shop_off.txt", "rb");
    const int enabled = !off;
    if (off) fclose(off);
    static const char *const names[] = {"", "applied", "already applied", "not needed",
                                        "restored original archive", "off"};
    const int r = gem_shop_run(pvz2_obb_path(), DATA_PATH "gem_shop.state", enabled);
    if (r > 0 && r <= GEM_SHOP_OFF) telemetry_log("GEMSHOP", "premium plants for gems: %s", names[r]);
    else telemetry_log("GEMSHOP", "premium plants for gems: archive left unchanged (%d)", r);
}
#endif
