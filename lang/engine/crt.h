#ifndef BRAT_RT_H
#define BRAT_RT_H

/* `fileno`, `realpath` and `strdup` are POSIX, not ISO C: under `-std=c11` they
   are hidden unless the feature level is asked for, and the runtime then fails
   to compile for a user who tries strict ISO C. Ask before the first include;
   The floor is C99 plus POSIX: nothing in this runtime needs C11. */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__) && !defined(_WIN32)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BRAT_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define BRAT_ASAN 1
#endif
#ifdef BRAT_ASAN
#include <sanitizer/asan_interface.h>
#define BARENA_POISON(p, n) __asan_poison_memory_region((p), (n))
#define BARENA_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define BARENA_POISON(p, n) ((void)0)
#define BARENA_UNPOISON(p, n) ((void)0)
#endif
#if defined(_MSC_VER)
#define BTLS __declspec(thread)
#else
#define BTLS _Thread_local
#endif
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
/* Baked memory contract: `brat build --mem N` compiles BRAT_MEM_BYTES in and
 * main() applies it as RLIMIT_AS before anything else allocates. 0 (the
 * default) means no budget. Windows has no RLIMIT_AS; the budget is silently
 * absent there rather than faked. */
#ifndef BRAT_MEM_BYTES
#define BRAT_MEM_BYTES 0
#endif
#if defined(BRAT_RT_SHARED)
#define BFN
#else
/* `crt.h` is header-only: every program compiles all ~250 runtime functions and
   calls a handful, which is 81 `-Wunused-function` warnings per program and a
   `-Wall` line nobody reads. The attribute is about this file, not about the
   program, so it lives on the linkage macro. */
#if defined(__GNUC__) || defined(__clang__)
#define BFN static __attribute__((unused))
#else
#define BFN static
#endif
#endif


/* A frame variable a program binds and never reads: the ownership checker
   already reported it as `unused var`, so the C compiler repeating it is noise
   and the only warning left in the emitted C. */
#if defined(__GNUC__) || defined(__clang__)
#define BRAT_UNUSED __attribute__((unused))
#else
#define BRAT_UNUSED
#endif

#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
typedef int64_t bint;
#define BINT_MIN INT64_MIN
_Static_assert(sizeof(bint) == 8, "BRAT i64 must be 64 bits");
_Static_assert(CHAR_BIT == 8, "BRAT assumes 8-bit bytes");

#define BINT_FMT "%" PRId64


typedef enum { V_NIL, V_BOOL, V_INT, V_FLOAT, V_STR, V_VEC, V_STRUCT, V_TAG, V_MOVED } VTag;
typedef struct Val Val;
typedef struct Vec Vec;
typedef struct Map Map;
struct Vec { Val *items; bint len, cap; };

struct Map { Val *keys; Val *vals; bint len, cap; const char *type;
             bint *idx; bint idx_cap; bint idx_used; bint bad_key; };
struct Val { VTag tag; union { bint b; bint ival; double fval; char *str; Vec *vec; Map *map; struct { char *tag; Vec *args; } tv; } as; };
#if !defined(_WIN32)
static pthread_mutex_t _brat_ctr_lock = PTHREAD_MUTEX_INITIALIZER;
#define BRAT_CTR_LOCK() pthread_mutex_lock(&_brat_ctr_lock)
#define BRAT_CTR_UNLOCK() pthread_mutex_unlock(&_brat_ctr_lock)
static pthread_mutex_t BRAT_UNUSED _brat_print_lock = PTHREAD_MUTEX_INITIALIZER;
#define BRAT_PRINT_LOCK() pthread_mutex_lock(&_brat_print_lock)
#define BRAT_PRINT_UNLOCK() pthread_mutex_unlock(&_brat_print_lock)
#else
#define BRAT_CTR_LOCK() ((void)0)
#define BRAT_CTR_UNLOCK() ((void)0)
static SRWLOCK BRAT_UNUSED _brat_print_lock = SRWLOCK_INIT;
#define BRAT_PRINT_LOCK() AcquireSRWLockExclusive(&_brat_print_lock)
#define BRAT_PRINT_UNLOCK() ReleaseSRWLockExclusive(&_brat_print_lock)
#endif

typedef struct BHead { bint rc; bint cap; bint len; struct BHead *arena_next; } BHead;
BFN void vraise(const char *msg);
BFN Val vstr_of(Val v);
BFN void vraisef(const char *fmt, const char *arg);

/* Depth budget for the recursive value walkers (print, length, compare).

   `str()` and `print` are recursive over the object graph, so a program that
   builds a deep value -- `for i in 0..200000 { v = [v] }` is 200 000 cheap
   allocations -- walked the machine stack once per nesting level and died with
   SIGSEGV at around 100 000 levels. The interpreter refuses the same program
   with "recursion too deep" because it has a frame budget of 2000, so the two
   backends disagreed twice over: one gave a clean error, the other a signal.

   Same budget here: past BRAT_MAX_VALUE_DEPTH the error channel gets
   "recursion too deep" and the walker stops, so the program exits 1 with an
   error instead of dying. The counter is thread-local because `spawn` runs the
   same helpers on several threads. */
#define BRAT_MAX_VALUE_DEPTH 2000
static BTLS bint bval_depth = 0;

/* Declared here because `vprint` pre-checks the nesting before it writes. */
BFN bint vsprint_len_q(Val v, bint q);
BFN int verr_pending(void);
BFN Val verr_take(void);
BFN void verr_clear(void);
BFN bint utf8_cplen(const char *s);
BFN void vjprint(Val v);

BFN bint veq(Val a, Val b);
BFN Val vat(Val a, Val i);
BFN Val vlen(Val a);
BFN Val vset_at(Val m, Val k, Val v);
BFN double vnum(Val v);
BFN Val vint(bint n);
BFN Val vfloat(double f);
BFN Val vstr(const char *s);
BFN Val vstrn(const char *s, bint n);
BFN Vec *vvec(void);
BFN Val vvecv(Vec *v);
BFN void vec_push(Vec *v, Val x);

BFN Val vrepeat_n(Val s, bint k);
BFN void *vhead_of(Val v);
#ifdef BRAT_RT_SHARED
BTLS Val vunwind;
#else
static BTLS Val vunwind;
#endif

static void vfatal_oom(void);

/* Check before overwriting: a failed realloc leaves the old block allocated. */

BFN void *bgrow(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { vfatal_oom(); }
    return q;
}

BFN bint vput(char *o, const char *s) {
    bint n = 0;
    while (s[n]) { o[n] = s[n]; n++; }
    return n;
}

BFN Val vnil(void) { Val v; v.tag = V_NIL; return v; }
BFN Val vbool(bint b) { Val v; v.tag = V_BOOL; v.as.b = b ? 1 : 0; return v; }
BFN Val vint(bint n) { Val v; v.tag = V_INT; v.as.ival = n; return v; }
BFN Val vfloat(double n) { Val v; v.tag = V_FLOAT; v.as.fval = n; return v; }

static void *balloc(size_t n);
static BTLS int barena_depth = 0;
/* Intern table for immortal string literals: avoids one allocation per loop.
 * Append-only, filled from any thread: an entry is written first and the count
 * is published last, so a reader either misses it or sees it whole. */
static struct { const char *src; char *ptr; } _vlit_cache[512];
static int _vlit_n = 0;
static pthread_mutex_t _vlit_mu = PTHREAD_MUTEX_INITIALIZER;
static int _vlit_count(void) {
/* The promises an `extern fn` makes about its C callee, as attributes the C
 * compiler can act on. `pure` on a call in a loop is the difference between one
 * call and one per iteration; `nothrow` lets the call be moved across a landing
 * pad. Both are only advice, so a compiler without attributes simply ignores
 * them and the program stays correct.
 */
#if defined(__GNUC__) || defined(__clang__)
#define BRAT_ATTR_CONST __attribute__((const))
#define BRAT_ATTR_PURE __attribute__((pure))
#define BRAT_ATTR_NOTHROW __attribute__((nothrow))
#else
#define BRAT_ATTR_CONST
#define BRAT_ATTR_PURE
#define BRAT_ATTR_NOTHROW
#endif

#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&_vlit_n, __ATOMIC_ACQUIRE);
#else
    return _vlit_n;
#endif
}
BFN Val vlit(const char *s) {
    int seen = _vlit_count();
    for (int i = 0; i < seen; i++) {
        if (_vlit_cache[i].src == s) {
            Val v; v.tag = V_STR; v.as.str = _vlit_cache[i].ptr;
            return v;
        }
    }
    size_t n = strlen(s);
    int saved = barena_depth;
    barena_depth = 0;
    char *p = (char *)balloc(n + 1);
    barena_depth = saved;
    memcpy(p, s, n + 1);
    ((BHead *)p - 1)->rc = -1; /* immortal: never freed, never counted */
    ((BHead *)p - 1)->len = (bint)n;
    pthread_mutex_lock(&_vlit_mu);
    if (_vlit_n < 512) {
        _vlit_cache[_vlit_n].src = s;
        _vlit_cache[_vlit_n].ptr = p;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&_vlit_n, _vlit_n + 1, __ATOMIC_RELEASE);
#else
        _vlit_n++;
#endif
    }
    pthread_mutex_unlock(&_vlit_mu);
    Val v; v.tag = V_STR; v.as.str = p;
    return v;
}

static void vfatal_oom(void) {
    fputs("brat: out of memory\n", stderr);
    abort();
}
/* Arena allocation. While an arena is entered, balloc bump-allocates from its
 * chunks; barena_leave frees all chunks added since the matching barena_enter.
 * Arena objects carry BARENA_FLAG in their refcount, so the decrement in
 * vrc_dec never reaches zero and vdrop returns early: they are never freed one
 * at a time. An arena block is laid out as [size_t usable][BHead][payload]. */
#define BARENA_FLAG ((bint)1 << 62)
/* Containers own what they hold: every store takes a reference and releasing
 * the container hands it back, so a value dies with its last holder. An arena
 * block is freed wholesale (barena_leave), so it carries a bit and is never
 * released element by element. */
/* The refcount is the one word two threads really do touch at the same time: a
 * value handed to `spawn` is held by the parent frame and by every task that
 * carries it, and tasks run on different OS threads. Every access to it is
 * therefore atomic, updates published acquire/release, so a thread that sees the
 * count also sees what the other thread wrote before dropping its reference.
 * Objects whose count carries a flag (arena) or is -1 (immortal) never change it,
 * but the probe still goes through the atomic, or two threads read and write the
 * same word with different tools and no tool can reason about either. */
#if defined(__GNUC__) || defined(__clang__)
#define BRC_LOAD(h) __atomic_load_n(&(h)->rc, __ATOMIC_ACQUIRE)
#define BRC_ADD(h) __atomic_add_fetch(&(h)->rc, 1, __ATOMIC_ACQ_REL)
#define BRC_SUB(h) __atomic_sub_fetch(&(h)->rc, 1, __ATOMIC_ACQ_REL)
#else
#define BRC_LOAD(h) ((h)->rc)
#define BRC_ADD(h) ((h)->rc += 1)
#define BRC_SUB(h) ((h)->rc -= 1)
#endif
#define BRC_VAL(h) (BRC_LOAD(h) & ~BARENA_FLAG)
#define BIS_IMMORTAL(h) (BRC_LOAD(h) == -1)
#define BARENA_CHUNK ((size_t)64 * 1024)
#define BARENA_HDR 32
#define BIS_ARENA(h) ((BRC_LOAD(h) & BARENA_FLAG) != 0)
#define BARENA_SIZE(h) (*(size_t *)((char *)(h) - sizeof(size_t)))
typedef struct BChunk { struct BChunk *prev; size_t used, cap; BHead *allocs; } BChunk;
typedef struct BMark { BChunk *top; size_t used; BHead *allocs; } BMark;
_Static_assert(sizeof(BChunk) <= BARENA_HDR, "arena chunk header is too small");
static BTLS BChunk *barena_top = 0;
/* Reset hands chunks back instead of munmap/malloc churn: a request loop
 * allocates and drops the same few chunks thousands of times. Bounded, so a
 * one-off huge chunk is not held for the life of the process. */
#define BARENA_POOL_CHUNKS 4
#define BARENA_POOL_BYTES ((size_t)256 * 1024)
static BTLS BChunk *barena_pool = 0;
static BTLS int barena_pool_chunks = 0;
static BTLS size_t barena_pool_bytes = 0;

static void barena_pool_put(BChunk *c) {
#ifdef BRAT_ASAN
    /* Keep ASan chunks available for deferred frame drops after arena reset. */
    c->used = 0;
    c->allocs = 0;
    c->prev = barena_pool;
    barena_pool = c;
    barena_pool_chunks++;
    barena_pool_bytes += c->cap;
#else
    if (barena_pool_chunks < BARENA_POOL_CHUNKS &&
        c->cap <= BARENA_POOL_BYTES - barena_pool_bytes) {
        c->used = 0;
        c->allocs = 0;
        c->prev = barena_pool;
        barena_pool = c;
        barena_pool_chunks++;
        barena_pool_bytes += c->cap;
        return;
    }
    free(c);
#endif
}

static BTLS bint barena_reuse_count = 0;

BFN bint barena_reuses(void) { return barena_reuse_count; }

static BChunk *barena_pool_get(size_t need) {
    BChunk **link = &barena_pool, *c;
    while (*link && (*link)->cap < need) link = &(*link)->prev;
    if (!*link) return 0;
    barena_reuse_count++;
    c = *link;
    *link = c->prev;
    barena_pool_chunks--;
    barena_pool_bytes -= c->cap;
    c->used = 0;
    c->allocs = 0;
    c->prev = 0;
    return c;
}

BFN BMark barena_enter(void) {
    BMark m;
    m.top = barena_top;
    m.used = barena_top ? barena_top->used : 0;
    m.allocs = barena_top ? barena_top->allocs : 0;
    barena_depth++;
    return m;
}
static void barena_poison_allocs(BHead *h, BHead *stop) {
    while (h && h != stop) {
        if (h->cap > 0) BARENA_POISON(h + 1, (size_t)h->cap);
        h = h->arena_next;
    }
}
BFN void barena_leave(BMark m) {
    while (barena_top && barena_top != m.top) {
        BChunk *c = barena_top;
        barena_top = c->prev;
        barena_poison_allocs(c->allocs, 0);
        barena_pool_put(c);
    }
    if (barena_top) {
        barena_poison_allocs(barena_top->allocs, m.allocs);
        barena_top->allocs = m.allocs;
        barena_top->used = m.used;
    }
    if (barena_depth > 0) barena_depth--;
}
static void *barena_alloc(size_t need) {
    if (need > SIZE_MAX - 15) vfatal_oom();
    need = (need + 15) & ~(size_t)15;
    BChunk *c = barena_top;
    if (!c || c->used > c->cap || need > c->cap - c->used) {
        size_t cap = need > BARENA_CHUNK ? need : BARENA_CHUNK;
        /* Grow: if we already have a chunk, double its size for the new one,
           so a sequence of medium allocations doesn't fragment into many
           fixed-size chunks. */
        if (c && c->cap > 0 && c->cap <= SIZE_MAX / 2 &&
            cap < c->cap * 2) cap = c->cap * 2;
        if (cap < need) cap = need;
        if (cap > SIZE_MAX - BARENA_HDR) vfatal_oom();
        BChunk *nc = barena_pool_get(cap);
        if (!nc) nc = (BChunk *)malloc(BARENA_HDR + cap);
        if (!nc) { fputs("brat: out of memory\n", stderr); abort(); }
        nc->prev = barena_top;
        nc->used = 0;
        nc->cap = cap;
        nc->allocs = 0;
        barena_top = nc;
        c = nc;
    }
    char *p = (char *)c + BARENA_HDR + c->used;
    c->used += need;
    BARENA_UNPOISON(p, need);
    return p;
}
BFN void *balloc(size_t n) {
    if (barena_depth > 0) {
        if (n > SIZE_MAX - sizeof(size_t) - sizeof(BHead) ||
            n > (size_t)INT64_MAX) vfatal_oom();
        char *base = (char *)barena_alloc(sizeof(size_t) + sizeof(BHead) + n);
        *(size_t *)base = n;
        BHead *ah = (BHead *)(base + sizeof(size_t));
        memset(ah, 0, sizeof(BHead) + n);
        ah->rc = 1 | BARENA_FLAG;
        ah->cap = (bint)n;
        ah->arena_next = barena_top->allocs;
        barena_top->allocs = ah;
        return ah + 1;
    }
    if (n > SIZE_MAX - sizeof(BHead) || n > (size_t)INT64_MAX) vfatal_oom();
    BHead *h = (BHead *)calloc(1, sizeof(BHead) + n);
    if (!h) vfatal_oom();
    h->rc = 1;
    h->cap = (bint)n;
    return h + 1;
}
BFN void *bgrow_rc(void *p, size_t n) {
    BHead *h = p ? (BHead *)p - 1 : 0;
    if (h && BIS_ARENA(h)) {
        size_t old = BARENA_SIZE(h);
        void *q = balloc(n);
        memcpy(q, p, old < n ? old : n);
        return q;
    }
    void *q = realloc(h, sizeof(BHead) + n);
    if (!q) { vfatal_oom(); }
    ((BHead *)q)->cap = (bint)n;
    return (BHead *)q + 1;
}
/* Container arrays follow their container: an arena container takes its arrays
 * from the arena too. Arena arrays are never freed one by one, and growing one
 * copies into fresh arena space; the old space is reclaimed by barena_leave. */
BFN void *barr_new(size_t bytes, int in_arena, int zero) {
    if (in_arena) return balloc(bytes);
    void *p = zero ? calloc(1, bytes) : malloc(bytes);
    if (!p) vfatal_oom();
    return p;
}
BFN void *barr_grow(void *old, int in_arena, size_t oldb, size_t newb) {
    if (!in_arena) return bgrow(old, newb);
    void *q = balloc(newb);
    memcpy(q, old, oldb < newb ? oldb : newb);
    return q;
}

#define BARENA_MAX_NEST 64
static BTLS BMark barena_stack[BARENA_MAX_NEST];
static BTLS int barena_nest = 0;
/* Language-level entry points used by the parser's `arena { }` desugaring. */
BFN Val varena_enter(void) {
    if (barena_nest >= BARENA_MAX_NEST) { vraise("arena nested too deep"); return vunwind; }
    barena_stack[barena_nest++] = barena_enter();
    return vint(0);
}
BFN Val varena_leave(void) {
    if (barena_nest <= 0) { vraise("arena leave without enter"); return vunwind; }
    barena_leave(barena_stack[--barena_nest]);
    return vint(0);
}

BFN Val vrc_inc(Val v) {
    switch (v.tag) {
    case V_STR:   if (v.as.str)  { BHead *h = (BHead *)v.as.str - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); }  return v;
    case V_VEC:   if (v.as.vec)  { BHead *h = (BHead *)v.as.vec - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); }  return v;
    case V_STRUCT: if (v.as.map) { BHead *h = (BHead *)v.as.map - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); } return v;
    case V_TAG:   if (v.as.tv.args) { BHead *h = (BHead *)v.as.tv.args - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); } return v;
    default: return v;
    }
}
BFN void vrelease1(Val v);
BFN void vrc_dec(Val v) {
    /* An immediate owns nothing, and immediates are most of what a scope drops:
     * the tag test has to come before the error-channel read, which is a
     * thread-local load plus a call per temporary. */
    if (v.tag != V_STR && v.tag != V_VEC && v.tag != V_STRUCT && v.tag != V_TAG) return;
    /* The error being carried belongs to the error channel until verr_take: a
     * frame that drops its locals on the way out must not free the message the
     * top level is about to print. */
    if (verr_pending() && vhead_of(v) == vhead_of(vunwind)) return;
    switch (v.tag) {
    case V_STR: {
        char *s = v.as.str;
        if (s) {
            BHead *h = (BHead *)s - 1;
            if (BIS_IMMORTAL(h) || BIS_ARENA(h)) return;
            if (BRC_SUB(h) == 0) free(h);
        }
        return;
    }
    case V_VEC: {
        Vec *vec = v.as.vec;
        if (vec) {
            BHead *h = (BHead *)vec - 1;
            if (BIS_ARENA(h)) return;
            if (BRC_SUB(h) == 0) {
                for (bint i = 0; i < vec->len; i++) vrelease1(vec->items[i]);
                free(vec->items);
                free(h);
            }
        }
        return;
    }
    case V_STRUCT: {
        Map *map = v.as.map;
        if (map) {
            BHead *h = (BHead *)map - 1;
            if (BIS_ARENA(h)) return;
            if (BRC_SUB(h) == 0) {
                for (bint i = 0; i < map->len; i++) {
                    vrelease1(map->keys[i]);
                    vrelease1(map->vals[i]);
                }
                free(map->keys);
                free(map->vals);
                free(map->idx);
                free(h);
            }
        }
        return;
    }
    case V_TAG: {
        /* A tag's arguments are the only thing it owns; the tag name is
         * interned and immortal. */
        Vec *args = v.as.tv.args;
        if (args) {
            BHead *h = (BHead *)args - 1;
            if (BIS_ARENA(h)) return;
            if (BRC_SUB(h) == 0) {
                for (bint i = 0; i < args->len; i++) vrelease1(args->items[i]);
                free(args->items);
                free(h);
            }
        }
        return;
    }
    default: return;
    }
}
BFN void vmem_budget_apply(void) {
#if !defined(_WIN32)
    if (BRAT_MEM_BYTES > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)BRAT_MEM_BYTES;
        rl.rlim_max = (rlim_t)BRAT_MEM_BYTES;
        if (setrlimit(RLIMIT_AS, &rl) != 0) {
            fprintf(stderr, "mem budget: setrlimit(%lld) refused\n", (long long)
                    (bint)BRAT_MEM_BYTES);
            exit(1);
        }
    }
#endif
}
BFN Val vstrn(const char *s, bint n) { Val v; v.tag = V_STR; v.as.str = (char *)balloc((size_t)n + 1); memcpy(v.as.str, s, n); v.as.str[n] = 0; ((BHead *)v.as.str - 1)->len = n; return v; }
BFN Vec *vvec(void) {
    Vec *v = (Vec *)balloc(sizeof(Vec));
    v->len = 0; v->cap = 4;
    v->items = (Val *)barr_new(sizeof(Val) * 4, BIS_ARENA((BHead *)v - 1), 0);
    return v;
}
BFN Val vstr(const char *s) {
    size_t n = strlen(s);
    Val v; v.tag = V_STR;
    v.as.str = (char *)balloc(n + 1);
    memcpy(v.as.str, s, n + 1);
    ((BHead *)v.as.str - 1)->len = (bint)n;
    return v;
}
BFN Val vvecv(Vec *v) { Val r; r.tag = V_VEC; r.as.vec = v; return r; }
/* Container scaffolding only: elements may be aliased or static, so dropping
   them here would be a UAF. */
BFN void vdrop(Val v) {
    switch (v.tag) {
    case V_NIL:
    case V_BOOL:
    case V_INT:
    case V_FLOAT:
    case V_STR:
    case V_MOVED:
        return;
    case V_VEC: {
        Vec *vec = v.as.vec;
        if (vec && !BIS_ARENA((BHead *)vec - 1)) {
            BHead *h = (BHead *)vec - 1;
            for (bint i = 0; i < vec->len; i++) vrelease1(vec->items[i]);
            free(vec->items);
            free(h);
        }
        return;
    }
    case V_STRUCT: {
        Map *map = v.as.map;
        if (map && !BIS_ARENA((BHead *)map - 1)) {
            BHead *h = (BHead *)map - 1;
            for (bint i = 0; i < map->len; i++) {
                vrelease1(map->keys[i]);
                vrelease1(map->vals[i]);
            }
            free(map->keys);
            free(map->vals);
            free(map->idx);
            free(h);
        }
        return;
    }
    case V_TAG: {
        Vec *args = v.as.tv.args;
        if (args && !BIS_ARENA((BHead *)args - 1)) {
            BHead *h = (BHead *)args - 1;
            for (bint i = 0; i < args->len; i++) vrelease1(args->items[i]);
            free(args->items);
            free(h);
        }
        return;
    }
    default:
        return;
    }
}
/* Release a binding without knowing what it holds. Strings and containers are
 * counted, so a value that reached the binding twice is only freed once; tags
 * and the rest go through vdrop, which leaves arena blocks to barena_leave.
 * vdrop alone cannot do this: it returns early for strings, whose count lives
 * with whoever aliases them. */
BFN void vrelease(Val v) {
    switch (v.tag) {
    case V_STR:
    case V_VEC:
    case V_STRUCT:
    case V_TAG:
        vrc_dec(v);
        return;
    default:
        vdrop(v);
        return;
    }
}
/* One element inside a container that owns its copies: same rule, named apart
 * so the recursion inside vrc_dec reads. */
BFN void vrelease1(Val v) {
    vrelease(v);
}
/* A statement temporary nobody owns afterwards. Every value the runtime builds
 * carries a header -- literal strings are immortal and arena values are skipped
 * inside vrc_dec -- so this is safe for the string tag too. */
/* A pointer that is about to be dereferenced has to be an address: 0 is a null
 * dereference, a negative value never is an address on any target we build for,
 * and C would fault on both. */
BFN int vptr_live(Val p) {
    if (p.as.ival > 0) return 1;
    vraise(p.as.ival == 0 ? "null pointer dereference" : "pointer is negative");
    return 0;
}
BFN void vdrop_temp(Val v) {
    if (v.tag == V_VEC || v.tag == V_TAG || v.tag == V_STRUCT || v.tag == V_STR) vrc_dec(v);
}
/* The receiver of a method call that nobody else names. A few methods hand the
 * receiver back as their result (`push`, `sort`), so it is released only when the
 * result is a different object: releasing it would free what the result reads. */
BFN void *vhead_of(Val v) {
    switch (v.tag) {
    case V_STR:   return v.as.str ? (void *)((BHead *)v.as.str - 1) : 0;
    case V_VEC:   return v.as.vec ? (void *)((BHead *)v.as.vec - 1) : 0;
    case V_STRUCT: return v.as.map ? (void *)((BHead *)v.as.map - 1) : 0;
    case V_TAG:   return v.as.tv.args ? (void *)((BHead *)v.as.tv.args - 1) : 0;
    default:      return 0;
    }
}
BFN void vdrop_recv(Val recv, Val result);
/* `first`, `min`, `pop`, `get` and `x[i]` answer with a part of the receiver, not
 * with a new value. The slot holding it is cleared before the receiver goes, so
 * the reference the container kept moves to the result instead of dying with it:
 * counting it again would leak, and releasing the container as it stands would
 * free the value the caller now holds. */
BFN void vdrop_recv_part(Val recv, Val result) {
    void *h = vhead_of(result);
    Vec *items = 0;
    if (!h) {
        vdrop_recv(recv, result);
        return;
    }
    if (recv.tag == V_VEC) items = recv.as.vec ? recv.as.vec : 0;
    else if (recv.tag == V_TAG) items = recv.as.tv.args;
    if (items) {
        for (bint i = 0; i < items->len; i++)
            if (vhead_of(items->items[i]) == h) { items->items[i] = vnil(); break; }
    } else if (recv.tag == V_STRUCT && recv.as.map) {
        Map *m = recv.as.map;
        for (bint i = 0; i < m->len; i++)
            if (vhead_of(m->vals[i]) == h) { m->vals[i] = vnil(); break; }
    }
    vdrop_recv(recv, result);
}
BFN void vdrop_recv(Val recv, Val result) {
    void *h = 0;
    switch (recv.tag) {
    case V_STR: case V_VEC: case V_STRUCT: case V_TAG: h = vhead_of(recv); break;
    default: return;
    }
    if (!h || h == vhead_of(result)) return;
    vrc_dec(recv);
}
BFN Val vmove(volatile Val *src) {
    /* The invalidation has to survive the optimizer. A plain store to an object
     * that is not read again is a dead store, and a move whose source still
     * reads is a double free waiting for a refcount -- the compiler cannot know
     * that the tag is a proof, not a value. Both accesses go through a volatile
     * lvalue, which is the only portable way to say "this write happens". */
    Val r;
    memcpy(&r, (const void *)src, sizeof r);
    src->tag = V_MOVED;
    return r;
}
/* Every element of a `vec[i64]` parameter, before a raw body reads one. The
 * interpreter walks the same vector dynamically, so a non-int element is a legal
 * program there -- the wrapper's job is to send it down the general path, not to
 * assume what the annotation promised. */
BFN bint vvec_all_int(Vec *v) {
    for (bint i = 0; i < v->len; i++)
        if (v->items[i].tag != V_INT) return 0;
    return 1;
}

BFN Map *vmap(const char *type) {
    Map *m = (Map *)balloc(sizeof(Map));
    m->len = 0; m->cap = 4;
    int ina = BIS_ARENA((BHead *)m - 1);
    m->keys = (Val *)barr_new(sizeof(Val) * 4, ina, 0);
    m->vals = (Val *)barr_new(sizeof(Val) * 4, ina, 0);
    m->idx = (bint *)barr_new(sizeof(bint) * 8, ina, 1);
    m->type = type;
    m->idx_cap = 8; m->idx_used = 0; m->bad_key = 0;
    return m;
}


BFN uint64_t vkey_hash(Val k) {
    uint64_t h = 1469598103934665603ULL;
    switch (k.tag) {
    case V_STR: {
        const unsigned char *p = (const unsigned char *)k.as.str;
        while (*p) { h ^= *p++; h *= 1099511628211ULL; }
        return h;
    }
    case V_INT:
        h ^= (uint64_t)k.as.ival; h *= 1099511628211ULL;
        h ^= h >> 29; return h;
    case V_FLOAT: {
        double d = k.as.fval == 0.0 ? 0.0 : k.as.fval;
        /* One NaN is one key. IEEE says NaN != NaN, so hashing the bit pattern
           would make `m[nan] = 1` add a fresh entry on every store, and
           `m.has(nan)` answer false a line after the store that made it. Java
           normalises the same way; the interpreter's `_mkey` does the same. */
        if (d != d) { h ^= 0x7ff8deadbeefULL; h *= 1099511628211ULL; return h; }
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        h ^= bits; h *= 1099511628211ULL; h ^= h >> 29; return h;
    }
    case V_BOOL:
        h ^= (uint64_t)(k.as.b ? 1 : 0); h *= 1099511628211ULL; return h;
    case V_NIL:
        h ^= 0x9e3779b97f4a7c15ULL; return h;
    case V_TAG: {
        const unsigned char *p = (const unsigned char *)k.as.tv.tag;
        while (*p) { h ^= *p++; h *= 1099511628211ULL; }
        h ^= (uint64_t)k.as.tv.args->len; h *= 1099511628211ULL;
        h ^= h >> 29; return h;
    }
    default:
        h ^= (uint64_t)k.tag; h *= 1099511628211ULL; return h;
    }
}

BFN void map_idx_rebuild(Map *m) {
    bint want = m->idx_cap;
    while (want < 8 || (m->len + 1) * 4 >= want * 3) want *= 2;
    if (want != m->idx_cap || m->idx == 0) {
        bint *ni = (bint *)barr_new(sizeof(bint) * (size_t)want, BIS_ARENA((BHead *)m - 1), 1);
        if (m->idx && !BIS_ARENA((BHead *)m - 1)) free(m->idx);
        m->idx = ni; m->idx_cap = want;
    } else {
        for (bint i = 0; i < m->idx_cap; i++) m->idx[i] = 0;
    }
    m->idx_used = 0;
    for (bint i = 0; i < m->len; i++) {
        uint64_t h = vkey_hash(m->keys[i]);
        bint mask = m->idx_cap - 1;
        bint slot = (bint)(h & (uint64_t)mask);
        while (m->idx[slot]) slot = (slot + 1) & mask;
        m->idx[slot] = i + 1;
        m->idx_used++;
    }
}

/* Equality as a map sees it: every NaN is the same key (see vkey_hash), and
   everything else is `veq`. `veq` itself keeps IEEE: `nan == nan` is false. */
BFN bint vmap_key_same(Val a, Val b) {
    if (a.tag == V_FLOAT && b.tag == V_FLOAT &&
        a.as.fval != a.as.fval && b.as.fval != b.as.fval) return 1;
    return veq(a, b);
}
BFN bint map_find(Map *m, Val k) {
    if (!m->idx || m->idx_cap == 0) map_idx_rebuild(m);
    bint mask = m->idx_cap - 1;
    bint slot = (bint)(vkey_hash(k) & (uint64_t)mask);
    bint first_tomb = -1;
    for (bint probes = 0; probes < m->idx_cap; probes++) {
        bint e = m->idx[slot];
        if (e == 0) return -1;
        if (e == -1) { if (first_tomb < 0) first_tomb = slot; }
        else if (vmap_key_same(m->keys[e - 1], k)) return e - 1;
        slot = (slot + 1) & mask;
    }
    return -1;
}
BFN void map_idx_put(Map *m, bint pos) {
    if (!m->idx) map_idx_rebuild(m);
    if (m->idx_used * 4 >= m->idx_cap * 3) map_idx_rebuild(m);
    bint mask = m->idx_cap - 1;
    bint slot = (bint)(vkey_hash(m->keys[pos]) & (uint64_t)mask);
    while (m->idx[slot] && m->idx[slot] != -1) slot = (slot + 1) & mask;
    m->idx[slot] = pos + 1;
    m->idx_used++;
}
BFN Val vstruct(Map *m) { Val r; r.tag = V_STRUCT; r.as.map = m; return r; }


BFN void vec_push(Vec *v, Val x) {
    BRAT_CTR_LOCK();
    if (v->len >= v->cap) {
        bint oldcap = v->cap;
        v->cap *= 2;
        v->items = (Val *)barr_grow(v->items, BIS_ARENA((BHead *)v - 1),
                                    sizeof(Val) * (size_t)oldcap, sizeof(Val) * (size_t)v->cap);
    }
    v->items[v->len++] = x;
    BRAT_CTR_UNLOCK();
}
/* A tag's arguments outlive the block that built them when the tag is returned
 * (Ok/Err from an arena), so an arena-built argument list is moved to the heap.
 * Its items are copied shallowly; any arena values inside are the escape rules' job. */
BFN Vec *vtag_args_heap(Vec *a) {
    if (!a || !BIS_ARENA((BHead *)a - 1)) return a;
    int saved = barena_depth;
    barena_depth = 0;
    Vec *d = vvec();
    /* The arena vector's references die with the arena, so the heap copy takes
     * them over instead of counting them again. */
    for (bint i = 0; i < a->len; i++) vec_push(d, a->items[i]);
    barena_depth = saved;
    return d;
}
static char *_vtag_intern(const char *tag);
BFN Val vtagv_lit(const char *tag, Vec *args) {
    Val r; r.tag = V_TAG;
    r.as.tv.tag = _vtag_intern(tag);
    r.as.tv.args = vtag_args_heap(args);
    return r;
}
/* Intern table for tag names (Ok, Err, user enum variants): immortal.
 * Two threads can build the same tag at the same time (`spawn` runs bodies
 * concurrently), so the table has one mutex and a length published with
 * release: a reader either misses a slot or sees it whole, and never reads past
 * the end of the array. */
static struct { const char *src; char *ptr; } _vtag_cache[512];
static int _vtag_n = 0;
static pthread_mutex_t _vtag_mu = PTHREAD_MUTEX_INITIALIZER;
static int _vtag_count(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&_vtag_n, __ATOMIC_ACQUIRE);
#else
    return _vtag_n;
#endif
}
static char *_vtag_intern(const char *tag) {
    int n = _vtag_count();
    for (int i = 0; i < n; i++) {
        if (strcmp(_vtag_cache[i].src, tag) == 0) return _vtag_cache[i].ptr;
    }
    pthread_mutex_lock(&_vtag_mu);
    n = _vtag_n;                       /* another thread may have added it */
    for (int i = 0; i < n; i++) {
        if (strcmp(_vtag_cache[i].src, tag) == 0) {
            char *hit = _vtag_cache[i].ptr;
            pthread_mutex_unlock(&_vtag_mu);
            return hit;
        }
    }
    char *p = strdup(tag);
    if (!p) vfatal_oom();
    if (_vtag_n < 512) {
        _vtag_cache[_vtag_n].src = p;
        _vtag_cache[_vtag_n].ptr = p;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&_vtag_n, _vtag_n + 1, __ATOMIC_RELEASE);
#else
        _vtag_n++;
#endif
    }
    pthread_mutex_unlock(&_vtag_mu);
    return p;
}
BFN Val vtagv(const char *tag, Vec *args) {
    Val r; r.tag = V_TAG;
    r.as.tv.tag = _vtag_intern(tag);
    r.as.tv.args = vtag_args_heap(args);
    return r;
}

BFN void vec_push_all(Vec *dst, Vec *src) {
    for (bint i = 0; i < src->len; i++) vec_push(dst, vrc_inc(src->items[i]));
}
/* A vector or a struct cannot be a map key: its hash would be over storage the
   program can reach and change. The refusal has to happen before any lock is
   taken, because `vraise` builds a `Val` (vvec, vec_push, vstr) and `vec_push`
   takes `_brat_ctr_lock` -- raising from inside the lock deadlocks on it. That
   was a real hang: `m[[1,2]] = 5` stopped forever in `vec_push`. */
BFN bint vmap_key_bad(Val k) {
    return k.tag == V_VEC || k.tag == V_STRUCT;
}
/* A key a map cannot hold answers like a reserved one did: with an err value,
   not by raising. `test_forge.py` pins that shape for `$type` -- a caught error
   is a value in this language -- and raising here was worse than useless
   anyway: `vraise` allocates, and the map held `_brat_ctr_lock`, so the
   program hung forever with no message at all. */
/* Set when a literal or a store handed a map a key it cannot hold. The emitter
   asks after building a literal, the way it asks `vmap_has_reserved`, so
   `{[1, 2]: 3}` is the err value in both backends instead of an empty map
   here and an err value in the interpreter. */
BFN bint vmap_bad_seen(Map *m) {
    return m->bad_key;
}
BFN Val vmap_bad_key_err(void) {
    Vec *e = vvec();
    vec_push(e, vstr("unhashable map key"));
    return vtagv("err", e);
}
BFN void map_set_v(Map *m, Val k, Val v) {
    if (vmap_key_bad(k)) { m->bad_key = 1; return; }  /* the caller answers */
    BRAT_CTR_LOCK();
    bint at = map_find(m, k);
    if (at >= 0) {
        Val old = m->vals[at];
        m->vals[at] = v;
        BRAT_CTR_UNLOCK();
        vrelease1(old);
        return;
    }
    if (m->len >= m->cap) {
        bint oldcap = m->cap;
        int inm = BIS_ARENA((BHead *)m - 1);
        m->cap *= 2;
        m->keys = (Val *)barr_grow(m->keys, inm, sizeof(Val) * (size_t)oldcap, sizeof(Val) * (size_t)m->cap);
        m->vals = (Val *)barr_grow(m->vals, inm, sizeof(Val) * (size_t)oldcap, sizeof(Val) * (size_t)m->cap);
    }
    m->keys[m->len] = k; m->vals[m->len] = v; m->len++;
    map_idx_put(m, m->len - 1);
    BRAT_CTR_UNLOCK();
}

BFN void map_set(Map *m, const char *k, Val v) {
    /* The key goes in as a value, so a map that owns its entries can release it:
     * a C literal has no header and vrc_dec would read past it. */
    map_set_v(m, vlit(k), v);
}
BFN Val *map_get_v(Map *m, Val k) {
    bint at = map_find(m, k);
    return at >= 0 ? &m->vals[at] : 0;
}
BFN Val *map_get(Map *m, const char *k) {
    Val key; key.tag = V_STR; key.as.str = (char *)k;
    return map_get_v(m, key);
}
BFN bint map_del_v(Map *m, Val k) {
    BRAT_CTR_LOCK();
    bint at = map_find(m, k);
    if (at < 0) { BRAT_CTR_UNLOCK(); return 0; }
    memmove(m->keys + at, m->keys + at + 1, sizeof(Val) * (size_t)(m->len - at - 1));
    memmove(m->vals + at, m->vals + at + 1, sizeof(Val) * (size_t)(m->len - at - 1));
    m->len--;
    
    map_idx_rebuild(m);
    BRAT_CTR_UNLOCK();
    return 1;
}

BFN char *vkey(Val k) {
    if (k.tag == V_STR) return strdup(k.as.str);
    if (k.tag == V_INT) { char b[32]; snprintf(b, sizeof b, BINT_FMT, (bint)k.as.ival); return strdup(b); }
    if (k.tag == V_FLOAT) { char b[64]; snprintf(b, sizeof b, "%.17g", k.as.fval); return strdup(b); }
    if (k.tag == V_BOOL) return strdup(k.as.b ? "true" : "false");
    vraise("bad map key"); return NULL;
}
/* Defined below the read methods that answer with them; the reserved key is
   refused by `get`, `has`, `del` and `get_or` as a value, never a raise. */
BFN bint vmapkey_reserved(Val k);
BFN Val vmapkey_err(void);
BFN Val vmap_getv(Val m, Val k) {
    if (m.tag != V_STRUCT) { vraise("no method get"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    Val *o = map_get_v(m.as.map, k);
    return o ? *o : vnil();
}
BFN Val vmap_has(Val m, Val k) {
    if (m.tag != V_STRUCT) { vraise("no method has"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    Val *o = map_get_v(m.as.map, k);
    return vbool(o != 0);
}
BFN Val vmap_keys(Val m) {
    if (m.tag != V_STRUCT) { vraise("no method keys"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < m.as.map->len; i++) vec_push(o, vrc_inc(m.as.map->keys[i]));
    return vvecv(o);
}
BFN Val vmap_del(Val m, Val k) {
    if (m.tag != V_STRUCT) { vraise("no method del"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    return vbool(map_del_v(m.as.map, k));
}

BFN bint vmapkey_reserved(Val k) {
    return k.tag == V_STR && k.as.str && strcmp(k.as.str, "$type") == 0;
}
BFN Val vmapkey_err(void) {
    Vec *e = vvec();
    vec_push(e, vstr("map key \"$type\" is reserved"));
    return vtagv("err", e);
}

BFN bint vmap_has_reserved(Map *m) {
    Val k; k.tag = V_STR; k.as.str = (char *)"$type";
    return map_find(m, k) >= 0;
}

BFN Val vmap_setv(Val m, Val k, Val v) {
    if (m.tag != V_STRUCT) { vraise("no method set"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    map_set_v(m.as.map, k, v);
    return v;
}
BFN bint vlong(Val v);
BFN Val vset_at(Val m, Val k, Val v) {
    
    if (m.tag == V_STRUCT) {
        if (m.as.map->type[0]) { char *kk = vkey(k); vraisef("no field %s\n", kk); return vunwind; }
        if (vmapkey_reserved(k)) { return vmapkey_err(); }
        if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
        map_set_v(m.as.map, k, v);
        return v;
    }
    if (m.tag == V_VEC) {
        bint i = vlong(k);
        if (i < 0 || i >= m.as.vec->len) { vraise("index out of range"); return vunwind; }
        BRAT_CTR_LOCK();
        Val old = m.as.vec->items[i];
        m.as.vec->items[i] = v;
        BRAT_CTR_UNLOCK();
        /* Released outside the lock: `vrelease1` runs arbitrary destructors. */
        vrelease1(old);
        return v;
    }
    vraise("setidx of non-indexable"); return vunwind;
}
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

/* Plugins: open a shared object at run time and call a symbol with a declared
 * signature. The signature is a literal at the call site, so the cast is
 * checked at compile time and no variadic thunk is involved. */
typedef struct BPluginHandle {
    bint id;
    bint active;
    int closing;
    void *native;
    struct BPluginHandle *next;
} BPluginHandle;
typedef struct BPluginSymbol {
    bint id, owner;
    void *native;
    struct BPluginSymbol *next;
} BPluginSymbol;
static BPluginHandle *bplugin_handles = 0;
static BPluginSymbol *bplugin_symbols = 0;
static bint bplugin_seq = 0;

static bint bplugin_next_id(void) {
    if (bplugin_seq == INT64_MAX) return 0;
    return ++bplugin_seq;
}

BFN Val vlib_open(Val path) {
#if defined(_WIN32)
    (void)path;
    vraise("lib_open: not supported on Windows");
    return vunwind;
#else
    void *h;
    BPluginHandle *entry;
    if (path.tag != V_STR) { vraise("lib_open: want str"); return vunwind; }
    h = dlopen(path.as.str, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        char b[512];
        const char *error = dlerror();
        snprintf(b, sizeof b, "lib_open: %s", error ? error : "dlopen failed");
        vraise(b);
        return vunwind;
    }
    entry = (BPluginHandle *)malloc(sizeof(BPluginHandle));
    if (!entry) { dlclose(h); vfatal_oom(); }
    BRAT_CTR_LOCK();
    entry->id = bplugin_next_id();
    if (!entry->id) {
        BRAT_CTR_UNLOCK();
        free(entry);
        dlclose(h);
        vraise("lib_open: handle id space exhausted");
        return vunwind;
    }
    entry->active = 0;
    entry->closing = 0;
    entry->native = h;
    entry->next = bplugin_handles;
    bplugin_handles = entry;
    BRAT_CTR_UNLOCK();
    return vint(entry->id);
#endif
}

BFN Val vlib_sym(Val h, Val name) {
#if defined(_WIN32)
    (void)h; (void)name;
    vraise("lib_sym: not supported on Windows");
    return vunwind;
#else
    void *p;
    const char *err;
    BPluginHandle *handle;
    BPluginSymbol *symbol;
    if (name.tag != V_STR) { vraise("lib_sym: want str"); return vunwind; }
    if (h.tag != V_INT || h.as.ival <= 0) {
        vraise("lib_sym: invalid library handle");
        return vunwind;
    }
    BRAT_CTR_LOCK();
    for (handle = bplugin_handles; handle && handle->id != h.as.ival; handle = handle->next) {}
    if (!handle || handle->closing) {
        BRAT_CTR_UNLOCK();
        char b[128];
        snprintf(b, sizeof b, "lib_sym: no such handle %lld", (long long)h.as.ival);
        vraise(b);
        return vunwind;
    }
    dlerror();
    p = dlsym(handle->native, name.as.str);
    err = dlerror();
    if (err || !p) {
        BRAT_CTR_UNLOCK();
        char b[512];
        snprintf(b, sizeof b, "lib_sym: no symbol %s", name.as.str);
        vraise(b);
        return vunwind;
    }
    symbol = (BPluginSymbol *)malloc(sizeof(BPluginSymbol));
    if (!symbol) vfatal_oom();
    symbol->id = bplugin_next_id();
    if (!symbol->id) {
        BRAT_CTR_UNLOCK();
        free(symbol);
        vraise("lib_sym: handle id space exhausted");
        return vunwind;
    }
    symbol->owner = h.as.ival;
    symbol->native = p;
    symbol->next = bplugin_symbols;
    bplugin_symbols = symbol;
    BRAT_CTR_UNLOCK();
    return vint(symbol->id);
#endif
}

BFN Val vlib_close(Val h) {
#if defined(_WIN32)
    (void)h;
    vraise("lib_close: not supported on Windows");
    return vunwind;
#else
    BPluginHandle **link, *handle;
    BPluginSymbol **slink, *symbol;
    if (h.tag != V_INT || h.as.ival <= 0) {
        vraise("lib_close: invalid library handle");
        return vunwind;
    }
    BRAT_CTR_LOCK();
    for (link = &bplugin_handles; *link && (*link)->id != h.as.ival; link = &(*link)->next) {}
    handle = *link;
    if (!handle || (handle->closing && handle->active > 0)) {
        BRAT_CTR_UNLOCK();
        char b[128];
        snprintf(b, sizeof b, "lib_close: no such handle %lld", (long long)h.as.ival);
        vraise(b);
        return vunwind;
    }
    if (handle->active) {
        handle->closing = 1;
    } else if (dlclose(handle->native) != 0) {
        const char *error = dlerror();
        char b[512];
        snprintf(b, sizeof b, "lib_close: %s", error ? error : "dlclose failed");
        BRAT_CTR_UNLOCK();
        vraise(b);
        return vunwind;
    } else {
        *link = handle->next;
        free(handle);
    }
    for (slink = &bplugin_symbols; *slink;) {
        symbol = *slink;
        if (symbol->owner == h.as.ival) {
            *slink = symbol->next;
            free(symbol);
        } else {
            slink = &symbol->next;
        }
    }
    BRAT_CTR_UNLOCK();
#endif
    return vint(0);
}

BFN void *vplugin_acquire(Val fn, bint *owner) {
#if defined(_WIN32)
    (void)fn; (void)owner;
    vraise("call: not supported on Windows");
    return 0;
#else
    BPluginSymbol *symbol;
    BPluginHandle *handle;
    void *native = 0;
    int saturated = 0;
    if (fn.tag != V_INT || fn.as.ival <= 0) {
        vraise("call: want function symbol");
        return 0;
    }
    BRAT_CTR_LOCK();
    for (symbol = bplugin_symbols; symbol && symbol->id != fn.as.ival; symbol = symbol->next) {}
    if (symbol) {
        for (handle = bplugin_handles; handle && handle->id != symbol->owner;
             handle = handle->next) {}
        if (handle && !handle->closing) {
            if (handle->active < INT64_MAX) {
                handle->active++;
                *owner = handle->id;
                native = symbol->native;
            } else {
                saturated = 1;
            }
        }
    }
    BRAT_CTR_UNLOCK();
    if (!native) {
        char b[128];
        if (saturated) snprintf(b, sizeof b, "call: too many concurrent calls");
        else snprintf(b, sizeof b, "call: no such function %lld", (long long)fn.as.ival);
        vraise(b);
    }
    return native;
#endif
}

BFN void vplugin_release(bint owner) {
#if !defined(_WIN32)
    BPluginHandle **link, *handle;
    BPluginSymbol **slink, *symbol;
    char error[512] = {0};
    BRAT_CTR_LOCK();
    for (link = &bplugin_handles; *link && (*link)->id != owner; link = &(*link)->next) {}
    handle = *link;
    if (handle && handle->active > 0) {
        handle->active--;
        if (handle->closing && handle->active == 0) {
            if (dlclose(handle->native) == 0) {
                *link = handle->next;
                free(handle);
                for (slink = &bplugin_symbols; *slink;) {
                    symbol = *slink;
                    if (symbol->owner == owner) {
                        *slink = symbol->next;
                        free(symbol);
                    } else {
                        slink = &symbol->next;
                    }
                }
            } else {
                const char *reason = dlerror();
                snprintf(error, sizeof error, "lib_close: %s",
                         reason ? reason : "dlclose failed");
            }
        }
    }
    BRAT_CTR_UNLOCK();
    if (error[0]) vraise(error);
#else
    (void)owner;
#endif
}

BFN Val vcall_str(const char *p) {
    if (!p) { vraise("call: NULL str result"); return vunwind; }
    return vstr(p);
}

/* String from an explicit (pointer, length) pair. C buffers carry their own
 * length; trusting strlen there is how a NUL in the middle silently truncates.
 * Rejects NUL and invalid UTF-8 so the byte string means the same thing in both
 * backends. */
BFN int utf8_valid(const char *s, bint n) {
    const unsigned char *p = (const unsigned char *)s;
    bint i = 0;
    while (i < n) {
        unsigned char c = p[i];
        bint k, need;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { need = 1; if (c < 0xC2) return 0; }
        else if ((c & 0xF0) == 0xE0) { need = 2; }
        else if ((c & 0xF8) == 0xF0) { need = 3; if (c > 0xF4) return 0; }
        else return 0;
        if (i + need >= n) return 0;
        for (k = 1; k <= need; k++) if ((p[i + k] & 0xC0) != 0x80) return 0;
        if (need == 2) {
            unsigned v = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        } else if (need == 3) {
            unsigned v = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) |
                         ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
            if (v < 0x10000 || v > 0x10FFFF) return 0;
        }
        i += need + 1;
    }
    return 1;
}

BFN Val vstr_from_ptr(Val p, Val n) {
    bint len = n.as.ival, i;
    const char *src;
    if (len < 0) { vraise("str_from_ptr: n must be >= 0"); return vunwind; }
    if (p.as.ival == 0) {
        if (len == 0) return vstr("");
        vraise("null pointer dereference");
        return vunwind;
    }
    src = (const char *)p.as.ival;
    for (i = 0; i < len; i++) {
        if (!src[i]) {
            char b[64];
            snprintf(b, sizeof b, "str_from_ptr: NUL byte at %" PRId64, (int64_t)i);
            vraise(b);
            return vunwind;
        }
    }
    if (!utf8_valid(src, len)) { vraise("str_from_ptr: invalid UTF-8"); return vunwind; }
    return vstrn(src, len);
}

/* A slice is [base, start, len, kind]: base is a vec or a raw address, kind is
 * 0 for a vec window, 1 for int64 elements, 2 for doubles, 3 for bytes, 4 for
 * int32, 5 for float32. Every access checks against len, so a window over C
 * memory is no less checked than a vec one. The 32-bit kinds exist because a
 * driver hands out blocks of registers and descriptor tables, and a u32 register
 * read as an i64 is the wrong value, not a wider one.
 *
 * Two more shapes carry a slice whose elements are not machine words:
 *
 *   [base, start, len, 6, esize]     a table of C structs, esize bytes apart.
 *                                    Walking records out of a buffer needs no
 *                                    compiler, only the stride: this is a
 *                                    driver's descriptor ring.
 *   [base, start, len, 7, esize, elems]
 *                                    an in-place window over `elems`, a vec of
 *                                    elements built at the point of the call.
 *                                    A table of structs is read-only; this is
 *                                    how a window of structs is *written*
 *                                    without copying as many as it holds, and
 *                                    the elements travel inside the window, so
 *                                    a store cannot outlive its targets.
 * A window whose kind is 7 indexes `elems`; every other kind indexes memory. */
BFN Val vslice_make(Val base, Val start, Val len, Val kind) {
    Vec *s = vvec();
    vec_push(s, vrc_inc(base));
    vec_push(s, vint(start.as.ival));
    vec_push(s, vint(len.as.ival));
    vec_push(s, vint(kind.as.ival));
    return vvecv(s);
}

BFN Val vslice_of(Val v, Val a, Val b) {
    bint n = vlen(v).as.ival;
    if (a.as.ival < 0 || b.as.ival < a.as.ival || b.as.ival > n) {
        char m[96];
        snprintf(m, sizeof m, "slice range %" PRId64 "..%" PRId64 " out of range 0..%" PRId64,
                 (int64_t)a.as.ival, (int64_t)b.as.ival, (int64_t)n);
        vraise(m);
        return vunwind;
    }
    return vslice_make(v, a, vint(b.as.ival - a.as.ival), vint(0));
}

BFN Val vslice_of_ptr(Val p, Val n, Val kind) {
    bint k = kind.as.ival;
    if (k < 1 || k > 5) {
        vraise("slice_of_ptr: kind must be i64, f64, u8, i32 or f32");
        return vunwind;
    }
    if (n.as.ival < 0) { vraise("slice_of_ptr: n must be >= 0"); return vunwind; }
    if (p.as.ival == 0 && n.as.ival > 0) { vraise("null pointer dereference"); return vunwind; }
    return vslice_make(p, vint(0), n, vint(k));
}

/* The only struct an element walker can build without a compiler is one whose
 * fields the emitter knows: a C array of `Val` literals. */
typedef struct { Val *items; bint len; } _Elems;

/* A window over elements built in BRAT itself: [base, 0, n, 7, esize, elems].
 * `esize` is unused by the accessors (elements are values, not bytes) but keeps
 * the slot count of a slice uniform. */
BFN Val vslice_elems(_Elems e) {
    Vec *v = vvec();
    for (bint i = 0; i < e.len; i++) vec_push(v, e.items[i]);
    Val elems = vvecv(v);
    /* The window is over the elements themselves: slot 0 and slot 5 both name
     * that vec, and the second reference is counted, so releasing the window
     * does not free storage it still points at. */
    Vec *out = vvec();
    vec_push(out, elems);
    vec_push(out, vint(0));
    vec_push(out, vint(e.len));
    vec_push(out, vint(7));
    vec_push(out, vint(0));
    vec_push(out, vrc_inc(elems));
    return vvecv(out);
}

/* A window over C memory laid out as `esize`-byte records. Reading is
 * `ptr_read_struct` at a computed address, so the layout stays the compiler's
 * business and only the stride is BRAT's. */
BFN Val vslice_of_struct(Val p, Val n, Val esize) {
    if (n.as.ival < 0) { vraise("slice_of_struct: n must be >= 0"); return vunwind; }
    if (p.as.ival == 0 && n.as.ival > 0) { vraise("null pointer dereference"); return vunwind; }
    if (esize.as.ival <= 0) { vraise("slice_of_struct: element size must be > 0"); return vunwind; }
    Vec *s = vvec();
    vec_push(s, vrc_inc(p));
    vec_push(s, vint(0));
    vec_push(s, vint(n.as.ival));
    vec_push(s, vint(6));
    vec_push(s, vint(esize.as.ival));
    return vvecv(s);
}

BFN Val vslice_len(Val s) { return vat(s, vint(2)); }

BFN Val vslice_at(Val s, Val i) {
    Val base = vat(s, vint(0));
    bint start = vat(s, vint(1)).as.ival, n = vat(s, vint(2)).as.ival, k, idx = i.as.ival;
    if (verr_pending()) return vunwind;
    if (idx < 0 || idx >= n) {
        char m[96];
        snprintf(m, sizeof m, "slice index %" PRId64 " out of range 0..%" PRId64,
                 (int64_t)idx, (int64_t)n);
        vraise(m);
        return vunwind;
    }
    if (base.tag == V_VEC) return vat(base, vint(start + idx));
    k = vat(s, vint(3)).as.ival;
    if (k == 7) {
        Val elems = vat(s, vint(5));
        return vat(elems, vint(start + idx));
    }
    if (k == 6) {
        /* The address of the record, not its bytes: reading it is
         * `ptr_read_struct`, which is where the layout is decided. */
        bint esize = vat(s, vint(4)).as.ival;
        return vint((bint)(intptr_t)((char *)(intptr_t)base.as.ival + (start + idx) * esize));
    }
    if (k == 2) return vfloat(((double *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 3) return vint((int64_t)((unsigned char *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 4) return vint((int64_t)((int32_t *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 5) return vfloat((double)((float *)(intptr_t)base.as.ival)[start + idx]);
    return vint(((int64_t *)(intptr_t)base.as.ival)[start + idx]);
}

BFN Val vslice_set(Val s, Val i, Val v) {
    Val base = vat(s, vint(0));
    bint start = vat(s, vint(1)).as.ival, n = vat(s, vint(2)).as.ival, k, idx = i.as.ival;
    if (verr_pending()) return vunwind;
    if (idx < 0 || idx >= n) {
        char m[96];
        snprintf(m, sizeof m, "slice index %" PRId64 " out of range 0..%" PRId64,
                 (int64_t)idx, (int64_t)n);
        vraise(m);
        return vunwind;
    }
    if (base.tag == V_VEC) return vset_at(base, vint(start + idx), v);
    k = vat(s, vint(3)).as.ival;
    if (k == 7) return vset_at(vat(s, vint(5)), vint(start + idx), v);
    if (k == 6) {
        /* A table of records is a view of foreign memory. Writing one from a
         * value is C's job (`ptr_write_struct` and the function that owns the
         * buffer); a silent store here would be a memcpy of a layout BRAT only
         * knows by size. */
        vraise("slice_set: a window of C structs is read-only");
        return vunwind;
    }
    if (k == 2) {
        if (v.tag != V_INT && v.tag != V_FLOAT) { vraise("slice_set: want f64, got other"); return vunwind; }
        ((double *)(intptr_t)base.as.ival)[start + idx] = vnum(v);
        return vint(0);
    }
    if (k == 4) {
        if (v.tag != V_INT) { vraise("slice_set: want i32, got other"); return vunwind; }
        if (v.as.ival < -2147483648LL || v.as.ival > 2147483647LL) {
            char m[96];
            snprintf(m, sizeof m, "slice_set: i32 value %" PRId64 " out of range", (int64_t)v.as.ival);
            vraise(m);
            return vunwind;
        }
        ((int32_t *)(intptr_t)base.as.ival)[start + idx] = (int32_t)v.as.ival;
        return vint(0);
    }
    if (k == 5) {
        if (v.tag != V_INT && v.tag != V_FLOAT) { vraise("slice_set: want f32, got other"); return vunwind; }
        ((float *)(intptr_t)base.as.ival)[start + idx] = (float)vnum(v);
        return vint(0);
    }
    if (v.tag != V_INT) { vraise("slice_set: want i64, got other"); return vunwind; }
    if (k == 3) {
        if (v.as.ival < 0 || v.as.ival > 255) {
            char m[96];
            snprintf(m, sizeof m, "slice_set: byte %" PRId64 " out of range 0..255",
                     (int64_t)v.as.ival);
            vraise(m);
            return vunwind;
        }
        ((unsigned char *)(intptr_t)base.as.ival)[start + idx] = (unsigned char)v.as.ival;
        return vint(0);
    }
    ((int64_t *)(intptr_t)base.as.ival)[start + idx] = v.as.ival;
    return vint(0);
}

/* A `for` over a map iterates a fresh vector of its keys; over a vec it walks
 * the vec itself. The loop releases only what it built. */
BFN void viter_done(Val src, Vec *it) {
    if (src.tag != V_VEC && it) vrc_dec(vvecv(it));
}
BFN Vec *viter_vec(Val v) {
    if (v.tag == V_VEC) return v.as.vec;
    if (v.tag == V_STRUCT) {
        Vec *o = vvec();
        for (bint i = 0; i < v.as.map->len; i++) vec_push(o, vrc_inc(v.as.map->keys[i]));
        return o;
    }
    vraise("not iterable"); return NULL;
}

/* `a[i]`, as a reference the caller owns.
 *
 * The two shapes disagree on ownership: a character of a string is a fresh
 * one-character string (nobody else has it) while an element of a vector is
 * still owned by that vector (an alias). Doing the counting here is what lets
 * the emitter treat an index as an owned value in both cases -- the alternative
 * was `vrc_inc(vat(...))`, which leaked every character a parser read, 13 KB in
 * csv.brat alone.
 */
BFN Val vat_owned(Val a, Val i) {
    if (a.tag == V_VEC && i.tag == V_INT) {
        bint k = i.as.ival;
        BRAT_CTR_LOCK();
        if (k < 0 || k >= a.as.vec->len) {
            BRAT_CTR_UNLOCK();
            vraise("index out of range");
            return vunwind;
        }
        Val r = vrc_inc(a.as.vec->items[k]);
        BRAT_CTR_UNLOCK();
        return r;
    }
    if (a.tag == V_TAG && i.tag == V_INT) {
        bint k = i.as.ival;
        BRAT_CTR_LOCK();
        if (k < 1 || k > a.as.tv.args->len) {
            BRAT_CTR_UNLOCK();
            vraise("index out of range");
            return vunwind;
        }
        Val r = vrc_inc(a.as.tv.args->items[k - 1]);
        BRAT_CTR_UNLOCK();
        return r;
    }
    Val r = vat(a, i);
    if (a.tag == V_STR) return r;
    /* A failed read hands back the error itself, which owns its own storage;
     * counting it here would keep the message alive after the catch releases
     * it -- one leaked Vec per caught out-of-range index. */
    if (verr_pending()) return r;
    return vrc_inc(r);
}

BFN double vnum(Val v) {
    if (v.tag == V_INT) return (double)v.as.ival;
    if (v.tag == V_FLOAT) return v.as.fval;
    if (v.tag == V_BOOL) return (double)v.as.b;
    return 0;
}
BFN int vis_true(Val v) {
    switch (v.tag) {
    case V_NIL: return 0;
    case V_BOOL: return v.as.b;
    case V_INT: return v.as.ival != 0;
    case V_FLOAT: return v.as.fval != 0;
    case V_STR: return v.as.str[0] != 0;
    case V_VEC: return v.as.vec->len != 0;
    default: return 1;
    }
}


BFN const char *vfmt_double(double x) {
    static BTLS char buf[8][48];
    static BTLS bint k = 0;
    char *b = buf[k++ & 7];
    int p;
    /* The shortest mantissa that reads back as x, in exponent form: `%.*g`
       cannot be used for this, because %g switches to fixed notation whenever
       the exponent is below its precision, so a 17-digit value at 1e16 came
       out as `12345678901234568.0` here and as `1.2345678901234568e+16` from
       the interpreter (Python's repr() decides by decimal exponent, not by
       precision). */
    for (p = 0; p <= 17; p++) {
        snprintf(b, 48, "%.*e", p, x);
        if (strtod(b, NULL) == x) break;
    }
    /* repr()'s fixed range, exactly: zero and [1e-4, 1e16). */
    if (x == 0.0 || (fabs(x) >= 1e-4 && fabs(x) < 1e16)) {
        for (p = 0; p <= 17; p++) {
            snprintf(b, 48, "%.*f", p, x);
            if (strtod(b, NULL) == x) break;
        }
    }
    if (!strpbrk(b, ".eEnN")) { strcat(b, ".0"); }
    return b;
}


/* Defined below (one predicate for the three printers); `print` comes first. */
BFN bint vtag_closure(Val v);

/* Matches the interpreter's repr quote choice. */
BFN char vquote(const char *s) {
    return (strchr(s, '\'') && !strchr(s, '"')) ? '"' : '\'';
}

BFN bint vrepr_slen(const char *s) {
    bint n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\'' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') n += 2;
        else n += 1;
    }
    return n;
}

BFN void vprint_f(Val v, bint q, FILE *f);
static void vprint_f_(Val v, bint q, FILE *f);

static void vprint_f_(Val v, bint q, FILE *f) {
    bint i;
    switch (v.tag) {
    case V_MOVED: vraise("use of moved value"); break;
    case V_NIL: fprintf(f, "None"); break;
    case V_BOOL: fprintf(f, v.as.b ? "True" : "False"); break;
    case V_INT: fprintf(f, BINT_FMT, (bint)v.as.ival); break;
    case V_FLOAT: fprintf(f, "%s", vfmt_double(v.as.fval)); break;
    case V_STR: {
        if (!q) { fprintf(f, "%s", v.as.str); break; }
        char quote = vquote(v.as.str);
        fputc(quote, f);
        for (const char *p = v.as.str; *p; p++) {
            if (*p == quote || *p == '\\') { fputc('\\', f); fputc(*p, f); }
            else if (*p == '\n') fprintf(f, "\\n");
            else if (*p == '\r') fprintf(f, "\\r");
            else if (*p == '\t') fprintf(f, "\\t");
            else fputc((unsigned char)*p, f);
        }
        fputc(quote, f);
        break;
    }
    case V_VEC:
        fprintf(f, "[");
        for (i = 0; i < v.as.vec->len; i++) { if (i) fprintf(f, ", "); vprint_f(v.as.vec->items[i], 1, f); }
        fprintf(f, "]");
        break;
    case V_STRUCT: {
        int ismap = !v.as.map->type[0];
        fprintf(f, "{");
        if (!ismap) fprintf(f, "'$type': '%s'", v.as.map->type);
        for (i = 0; i < v.as.map->len; i++) {
            if (i || !ismap) fprintf(f, ", ");
            vprint_f(v.as.map->keys[i], 1, f);
            fprintf(f, ": ");
            vprint_f(v.as.map->vals[i], 1, f);
        }
        fprintf(f, "}");
        break;
    }
    case V_TAG:
        if (vtag_closure(v)) { fprintf(f, "fn"); break; }
        fprintf(f, "('%s'", v.as.tv.tag);
        for (i = 0; i < v.as.tv.args->len; i++) { fprintf(f, ", "); vprint_f(v.as.tv.args->items[i], 1, f); }
        
        if (v.as.tv.args->len == 0) fprintf(f, ",");
        fprintf(f, ")");
        break;
    }
}

/* `print` has its own recursive walker, so it needs the same budget as
   `str()`; the body calls back into this wrapper at every level. */
BFN void vprint_f(Val v, bint q, FILE *f) {
    if (bval_depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return; }
    bval_depth++;
    vprint_f_(v, q, f);
    bval_depth--;
}
BFN void vprint(Val v) {
    /* Check the nesting before writing anything: `vprint_f` streams straight to
       the FILE, so a value past the budget would leave half a line on stdout and
       then fail. `vsprint_len_q` walks the same graph and raises "recursion too
       deep" without output, so the error path prints nothing -- which is what
       the interpreter does. The walk costs what `vtxt`/`vlogv` already pay. */
    if (bval_depth == 0) {
        (void)vsprint_len_q(v, 0);
        if (verr_pending()) return;
    }
    vprint_f(v, 0, stdout);
}
BFN void vprint_err(Val v) { vprint_f(v, 0, stderr); }
/* The newline belongs to a print that succeeded: `vprint` refuses a value past
   the depth budget without writing, and an unconditional `printf("\n")` would
   leave an empty line the interpreter never prints. */
BFN void vprint_nl(void) { if (!verr_pending()) printf("\n"); }
/* An error raised inside a callback cannot unwind through the C frames between
 * it and the BRAT caller, so the wrapper must stop here: returning would let C
 * carry on with a value that means nothing. Same words and exit code as an
 * uncaught error at the top level. */
BFN void vcb_check(Val r) {
    int pending = verr_pending();
    int err = r.tag == V_TAG && r.as.tv.tag && !strcmp(r.as.tv.tag, "err");
    if (!pending && !err) return;
    fprintf(stderr, "unhandled: ");
    if (pending) {
        vprint_err(verr_take());
    } else {
        vprint_err(r);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    exit(1);
}

BFN bint veq(Val a, Val b);

static bint veq_(Val a, Val b);

/* Same budget as the printers: comparing two deep values walks the graph too. */
BFN bint veq(Val a, Val b) {
    if (bval_depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return 0; }
    bval_depth++;
    bint r = veq_(a, b);
    bval_depth--;
    return r;
}

/* Numeric order, exact for integers. Both `veq_` and `vcmp_body` used to convert
   the two sides to `double` first, which answers `2^60 + 1 > 2^60` with "equal",
   and did so on both backends because both converted: the C side printed
   `False` where the program asked for the order of two distinct integers, and
   `==` on the same pair answered `True` in C and `False` in the interpreter.
   An integer is compared as an integer here, and against a float without
   rounding the integer, which is what Python does for `int` against `float`. */
static int vcmp_int_double(double d, bint i) {
    if (d != d) return 0;
    if (d >= 9223372036854775808.0) return 1;
    if (d < -9223372036854775808.0) return -1;
    double fl = floor(d);
    bint fi = (bint)fl;
    if (fi < i) return -1;
    if (fi > i) return 1;
    return d > fl ? -1 : 0;
}

static int vcmp_num(Val a, Val b) {
    if (a.tag == V_FLOAT || b.tag == V_FLOAT) {
        if (a.tag != V_FLOAT) {
            bint x = a.tag == V_INT ? a.as.ival : a.as.b;
            return -vcmp_int_double(b.as.fval, x);
        }
        if (b.tag != V_FLOAT) {
            bint y = b.tag == V_INT ? b.as.ival : b.as.b;
            return vcmp_int_double(a.as.fval, y);
        }
        return a.as.fval < b.as.fval ? -1 : a.as.fval > b.as.fval ? 1 : 0;
    }
    {
        bint x = a.tag == V_INT ? a.as.ival : a.as.b;
        bint y = b.tag == V_INT ? b.as.ival : b.as.b;
        return x < y ? -1 : x > y ? 1 : 0;
    }
}

static bint vcmp_is_num(Val v) {
    return v.tag == V_INT || v.tag == V_FLOAT || v.tag == V_BOOL;
}

/* `veq` keeps IEEE: a `nan` is equal to nothing, itself included. */
static bint veq_num(Val a, Val b) {
    if ((a.tag == V_FLOAT && a.as.fval != a.as.fval) ||
        (b.tag == V_FLOAT && b.as.fval != b.as.fval)) return 0;
    return vcmp_num(a, b) == 0;
}

static bint veq_(Val a, Val b) {
    bint i;
    if (vcmp_is_num(a) && vcmp_is_num(b)) return veq_num(a, b);
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case V_NIL: return 1;
    case V_STR: return !strcmp(a.as.str, b.as.str);
    case V_VEC:
        if (a.as.vec->len != b.as.vec->len) return 0;
        for (i = 0; i < a.as.vec->len; i++) if (!veq(a.as.vec->items[i], b.as.vec->items[i])) return 0;
        return 1;
    case V_STRUCT: {
        if (strcmp(a.as.map->type, b.as.map->type)) return 0;
        if (a.as.map->len != b.as.map->len) return 0;
        for (i = 0; i < a.as.map->len; i++) {
            Val *o = map_get_v(b.as.map, a.as.map->keys[i]);
            if (!o || !veq(a.as.map->vals[i], *o)) return 0;
        }
        return 1;
    }
    case V_TAG:
        if (strcmp(a.as.tv.tag, b.as.tv.tag)) return 0;
        if (a.as.tv.args->len != b.as.tv.args->len) return 0;
        for (i = 0; i < a.as.tv.args->len; i++) if (!veq(a.as.tv.args->items[i], b.as.tv.args->items[i])) return 0;
        return 1;
    default: return 0;
    }
}


BFN int vcmp(Val a, Val b);
BFN int vcmpt(Val a, Val b, const char *op);
BFN int vclass(Val v);
static int vcmp_(Val a, Val b, const char *op);
static int vcmp_body(Val a, Val b, const char *op);

/* One order, applied all the way down: values of different classes are not
   ordered. The top level always refused a `str` against an `i64`; inside a
   vector or a tag the comparison used to fall back to the tag enum, which
   answered `<` with a number the program never asked for, and the interpreter
   answered with a Python traceback. `op` is the operator the program wrote, so
   the message names it wherever the refusal happens. */
static int vcmp_body(Val a, Val b, const char *op) {
    if (a.tag == V_STR && b.tag == V_STR) {
        int r = strcmp(a.as.str, b.as.str);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (vcmp_is_num(a) && vcmp_is_num(b)) return vcmp_num(a, b);
    if (a.tag == V_NIL || b.tag == V_NIL) {
        if (a.tag == b.tag) return 0;
        return a.tag == V_NIL ? -1 : 1;
    }
    if (vclass(a) != vclass(b) || !vclass(a)) {
        vraisef("type error in %s", op);
        return 0;
    }
    if (a.tag == V_VEC) {
        Vec *x = a.as.vec, *y = b.as.vec;
        bint nn = x->len < y->len ? x->len : y->len;
        for (bint i = 0; i < nn; i++) {
            int c = vcmp_(x->items[i], y->items[i], op);
            if (c) return c;
        }
        return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
    }
    if (a.tag == V_TAG) {
        int c = strcmp(a.as.tv.tag, b.as.tv.tag);
        if (c) return c < 0 ? -1 : 1;
        Vec *x = a.as.tv.args, *y = b.as.tv.args;
        bint nn = x->len < y->len ? x->len : y->len;
        for (bint i = 0; i < nn; i++) {
            c = vcmp_(x->items[i], y->items[i], op);
            if (c) return c;
        }
        return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
    }
    if (a.tag == V_STRUCT) {
        int c = strcmp(a.as.map->type, b.as.map->type);
        if (c) return c < 0 ? -1 : 1;
        bint nn = a.as.map->len < b.as.map->len ? a.as.map->len : b.as.map->len;
        for (bint i = 0; i < nn; i++) {
            c = vcmp_(a.as.map->keys[i], b.as.map->keys[i], op);
            if (c) return c;
            c = vcmp_(a.as.map->vals[i], b.as.map->vals[i], op);
            if (c) return c;
        }
        return a.as.map->len < b.as.map->len ? -1 : a.as.map->len > b.as.map->len ? 1 : 0;
    }
    return 0;
}
/* Class of a value for ordering. `<` between `str` and `i64` used to answer
   with a type rank, a question the program never asked; the interpreter refuses
   it, so the native side does too and both print the same message. */
BFN int vclass(Val v) {
    switch (v.tag) {
    case V_INT: case V_FLOAT: case V_BOOL: return 1;
    case V_STR: return 2;
    case V_VEC: return 3;
    case V_STRUCT: return 4;
    case V_NIL: return 5;
    case V_TAG: return 6;
    default: return 0;
    }
}
static int vmixed(Vec *w) {
    int k = 0;
    for (bint i = 0; i < w->len; i++) {
        int c = vclass(w->items[i]);
        if (!c) return 1;
        if (!k) k = c;
        else if (k != c) return 1;
    }
    return 0;
}
/* The budget lives here, around the walk, so a comparison 2000 levels down
   stops the same way `veq` does instead of running the machine stack out. */
static int vcmp_(Val a, Val b, const char *op) {
    if (bval_depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return 0; }
    bval_depth++;
    int c = vcmp_body(a, b, op);
    bval_depth--;
    return c;
}

BFN int vcmp(Val a, Val b) {
    return vcmpt(a, b, "<");
}

BFN int vcmpt(Val a, Val b, const char *op) {
    /* Two integers are the hot case -- a loop condition -- and they cannot be
       a type error, so the answer needs no walk and no operator string. */
    if (a.tag == V_INT && b.tag == V_INT)
        return a.as.ival < b.as.ival ? -1 : a.as.ival > b.as.ival;
    return vcmp_(a, b, op);
}

BFN int vcontains(Val c, Val x) {
    bint i;
    if (c.tag == V_VEC) {
        for (i = 0; i < c.as.vec->len; i++) if (veq(c.as.vec->items[i], x)) return 1;
        return 0;
    }
    if (c.tag == V_STR && x.tag == V_STR) return strstr(c.as.str, x.as.str) != 0;
    if (c.tag == V_STRUCT) {
        Val *o = map_get_v(c.as.map, x);
        return o != 0;
    }
    vraise("in of non-container"); return 0;
}

BFN bint vsprint_len_q(Val v, bint q);
BFN bint vsprint_q(Val v, char *o, bint q);


BFN Val vnum0(Val v) {
    if (v.tag == V_BOOL) return vint(v.as.b ? 1 : 0);
    return v;
}

#define VTXT_SLOTS 8
static BTLS char vtxt_buf[VTXT_SLOTS][512];
static BTLS bint vtxt_next = 0;

BFN const char *vtxt(Val v) {
    char *b = vtxt_buf[vtxt_next++ % VTXT_SLOTS];
    if (v.tag == V_STR) { snprintf(b, 512, "%s", v.as.str); return b; }
    if (v.tag == V_INT) { snprintf(b, 512, BINT_FMT, (bint)v.as.ival); return b; }
    if (v.tag == V_FLOAT) { snprintf(b, 512, "%s", vfmt_double(v.as.fval)); return b; }
    bint nn = vsprint_len_q(v, 0);
    if (nn < 511) {
        vsprint_q(v, b, 0);
        b[nn] = 0;
        return b;
    }
    
    char *heap = (char *)malloc((size_t)nn + 1);
    if (!heap) vfatal_oom();
    vsprint_q(v, heap, 0);
    heap[nn] = 0;
    static BTLS char *keep[VTXT_SLOTS];
    bint k = vtxt_next % VTXT_SLOTS;
    free(keep[k]);
    keep[k] = heap;
    return heap;
}

static Val vadd_slow(Val a, Val b);
/* The int case of `+` is one tag test and an add; the slow path is a string
   concatenation and a vector copy. Split, so the arithmetic in a loop is a leaf
   the C compiler can inline and not a call into 200 lines it cannot. */
BFN Val vadd(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival + (uint64_t)b.as.ival));
    return vadd_slow(a, b);
}
static Val vadd_slow(Val a, Val b) {
    if (a.tag == V_STR || b.tag == V_STR) {
        const char *sa = (a.tag == V_STR) ? a.as.str : vtxt(a);
        const char *sb = (b.tag == V_STR) ? b.as.str : vtxt(b);
        size_t la = strlen(sa), lb = strlen(sb);

        char *out = (char *)balloc(la + lb + 1);
        Val r; r.tag = V_STR;
        r.as.str = out;
        memcpy(out, sa, la);
        memcpy(out + la, sb, lb);
        out[la + lb] = 0;
        ((BHead *)out - 1)->len = (bint)(la + lb);
        return r;
    }
    a = vnum0(a); b = vnum0(b);
    if (a.tag == V_INT && b.tag == V_INT) {

        return vint((bint)((uint64_t)a.as.ival + (uint64_t)b.as.ival));
    }
    if ((a.tag == V_INT || a.tag == V_FLOAT) && (b.tag == V_INT || b.tag == V_FLOAT))
        return vfloat(vnum(a) + vnum(b));
    
    if (a.tag == V_VEC && b.tag == V_VEC) {
        Vec *o = vvec();
        vec_push_all(o, a.as.vec);
        vec_push_all(o, b.as.vec);
        return vvecv(o);
    }
    vraise("type error in +"); return vunwind;
}
/* In-place string append for the `s = s + x` hot loop. Mutates the
   buffer only when it is uniquely owned (rc == 1); otherwise falls back to
   vadd. The rc is bumped to 2 on the in-place path so the caller's release of
   the old value leaves exactly one reference. */
BFN Val vadd_self(Val a, Val b) {
    if (a.tag == V_STR && a.as.str) {
        const char *sb = (b.tag == V_STR) ? b.as.str : vtxt(b);
        if (sb && a.as.str != sb) {
            BHead *h = (BHead *)a.as.str - 1;
            if (!BIS_ARENA(h)) {
                bint rc = BRC_VAL(h);
                if (rc == 1) {
                    size_t la = (size_t)h->len, lb = strlen(sb);
                    size_t need = la + lb + 1;
                    if ((size_t)h->cap < need) {
                        size_t nc = h->cap > 0 ? (size_t)h->cap : 16;
                        while (nc < need) nc *= 2;
                        BHead *nh = (BHead *)realloc(h, sizeof(BHead) + nc);
                        if (!nh) vfatal_oom();
                        nh->cap = (bint)nc;
                        a.as.str = (char *)nh + sizeof(BHead);
                        h = nh;
                    }
                    memcpy(a.as.str + la, sb, lb);
                    a.as.str[la + lb] = 0;
                    h->len = (bint)(la + lb);
                    /* rc stays 1: this function owns the release of the old value */
                    return a;
                }
            }
        }
    }
    Val _r = vadd(a, b);
    vrc_dec(a);
    return _r;
}


static Val vsub_slow(Val a, Val b);
BFN Val vsub(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival - (uint64_t)b.as.ival));
    return vsub_slow(a, b);
}
static Val vsub_slow(Val a, Val b) {
    a = vnum0(a); b = vnum0(b);
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival - (uint64_t)b.as.ival));
    if ((a.tag == V_INT || a.tag == V_FLOAT || a.tag == V_BOOL)
            && (b.tag == V_INT || b.tag == V_FLOAT || b.tag == V_BOOL))
        return vfloat(vnum(a) - vnum(b));
    vraise("type error in -"); return vunwind;
}
static Val vmul_slow(Val a, Val b);
BFN Val vmul(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival * (uint64_t)b.as.ival));
    return vmul_slow(a, b);
}
static Val vmul_slow(Val a, Val b) {
    a = vnum0(a); b = vnum0(b);
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival * (uint64_t)b.as.ival));
    if ((a.tag == V_INT || a.tag == V_FLOAT) && (b.tag == V_INT || b.tag == V_FLOAT))
        return vfloat(vnum(a) * vnum(b));
    
    if (a.tag == V_STR && b.tag == V_INT) return vrepeat_n(a, b.as.ival);
    if (a.tag == V_INT && b.tag == V_STR) return vrepeat_n(b, a.as.ival);
    vraise("type error in *"); return vunwind;
}
BFN Val vdiv(Val a, Val b) {
    a = vnum0(a); b = vnum0(b);
    if (a.tag == V_INT && b.tag == V_INT) {
        if (b.as.ival == 0) { vraise("div by zero"); return vunwind; }
        
        bint x = a.as.ival, y = b.as.ival;
        if (x == BINT_MIN && y == -1) { return vint(BINT_MIN); }
        bint q = x / y;
        if ((x % y) != 0 && ((x < 0) != (y < 0))) { q--; }
        return vint(q);
    }
    if ((a.tag == V_INT || a.tag == V_FLOAT) && (b.tag == V_INT || b.tag == V_FLOAT)) {
        double y = vnum(b);
        if (y == 0) { vraise("div by zero"); return vunwind; }
        return vfloat(vnum(a) / y);
    }
    vraise("type error in /"); return vunwind;
}
static Val vmod_slow(Val a, Val b);
/* Floor-mod with the sign of the divisor, the one definition both paths use. */
BFN bint vmod_int(bint x, bint y) {
    if (x == BINT_MIN && y == -1) return 0;
    bint r = x % y;
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return r;
}
BFN Val vmod(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT) {
        if (b.as.ival == 0) { vraise("div by zero"); return vunwind; }
        return vint(vmod_int(a.as.ival, b.as.ival));
    }
    return vmod_slow(a, b);
}
static Val vmod_slow(Val a, Val b) {
    a = vnum0(a); b = vnum0(b);
    if (a.tag == V_INT && b.tag == V_INT) {
        if (b.as.ival == 0) { vraise("div by zero"); return vunwind; }
        return vint(vmod_int(a.as.ival, b.as.ival));
    }
    
    if ((a.tag == V_INT || a.tag == V_FLOAT) && (b.tag == V_INT || b.tag == V_FLOAT)) {
        double x = vnum(a), y = vnum(b);
        if (y == 0) { vraise("div by zero"); return vunwind; }
        double r = fmod(x, y);
        if (r != 0 && ((r < 0) != (y < 0))) { r += y; }
        return vfloat(r);
    }
    vraise("type error in %"); return vunwind;
}
BFN Val vneg(Val a) {
    if (a.tag == V_INT) {
        if (a.as.ival == BINT_MIN) { return vint(BINT_MIN); }
        return vint((bint)(0ULL - (uint64_t)a.as.ival));
    }
    if (a.tag == V_FLOAT) return vfloat(-a.as.fval);
    if (a.tag == V_BOOL) return vint(a.as.b ? -1 : 0);
    vraise("type error in unary -"); return vunwind;
}
BFN Val vlen(Val a) {
    if (a.tag == V_STR) {
        bint n = 0;
        for (const char *q = a.as.str; *q; q += utf8_cplen(q)) n++;
        return vint(n);
    }
    if (a.tag == V_VEC) return vint(a.as.vec->len);
    if (a.tag == V_STRUCT) return vint(a.as.map->len);
    vraise("len of non-sized"); return vunwind;
}
BFN bint utf8_cplen(const char *s) {
    unsigned char c = (unsigned char)*s;
    bint need = 1;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    
    bint have = 1;
    while (have < need && s[have]) have++;
    return have;
}
BFN const char *utf8_at(const char *s, bint idx) {
    const char *p = s;
    while (idx > 0 && *p) {
        p += utf8_cplen(p);
        idx--;
    }
    return p;
}
/* Strict UTF-8, matching CPython's codec: read_file must refuse the same bytes. */
BFN int vutf8_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c < 0x80) { p++; continue; }
        bint n;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) n = 2;
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) n = 3;
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else return 0;
        if (p[1] < lo || p[1] > hi) return 0;
        for (bint i = 2; i <= n; i++) if (p[i] < 0x80 || p[i] > 0xBF) return 0;
        p += n + 1;
    }
    return 1;
}
BFN Val vat(Val a, Val i) {
    if (a.tag == V_STRUCT) {
        Val *o = map_get_v(a.as.map, i);
        if (!o) {
            /* Name the place the way the interpreter does: a map with a tag is
               a record, so a missing entry is a missing *field* (`no field zz`,
               the word `p.zz` already uses); an untagged map keeps `no key c`. */
            Val ks = vstr_of(i);
            size_t n = strlen(ks.as.str) + 16;
            char *m = (char *)malloc(n);
            if (!m) vfatal_oom();
            snprintf(m, n, a.as.map->type[0] ? "no field %s" : "no key %s", ks.as.str);
            vraise(m);
            free(m);
            return vunwind;
        }
        return *o;
    }
    bint k = (i.tag == V_INT || i.tag == V_FLOAT) ? (bint)vnum(i) : 0;
    if (i.tag != V_INT && i.tag != V_FLOAT) { vraise("index must be int"); return vunwind; }
    if (a.tag == V_STR) {
        const char *p = a.as.str;
        bint n = 0;
        for (const char *q = p; *q; q += utf8_cplen(q)) n++;
        if (k < 0 || k >= n) { vraise("index out of range"); return vunwind; }
        const char *at = utf8_at(p, k);
        return vstrn(at, utf8_cplen(at));
    }
    if (a.tag == V_VEC) {
        BRAT_CTR_LOCK();
        if (k < 0 || k >= a.as.vec->len) { BRAT_CTR_UNLOCK(); vraise("index out of range"); return vunwind; }
        Val r = a.as.vec->items[k];
        BRAT_CTR_UNLOCK();
        return r;
    }
    if (a.tag == V_TAG) {
        BRAT_CTR_LOCK();
        if (k < 1 || k > a.as.tv.args->len) { BRAT_CTR_UNLOCK(); vraise("index out of range"); return vunwind; }
        Val r = a.as.tv.args->items[k - 1];
        BRAT_CTR_UNLOCK();
        return r;
    }
    vraise("index of non-indexable"); return vunwind;
}

static BTLS int _verr_pending;
/* _verr_exc: 1 for a runtime error, 0 for an `err` value (`raise`) that a
   callee returns like a normal value. Only exceptions survive a frame. */
static BTLS int _verr_exc;

/* Raises set a thread-local pending flag; callers check verr_pending() instead
   of longjmp, which is UB through non-volatile frames at -O2. */

BFN int verr_pending(void) { return _verr_pending; }
BFN Val verr_take(void) {
    _verr_pending = 0;
    _verr_exc = 0;
    return vunwind;
}
/* Leave one frame: clear an `err` value, keep a runtime exception pending so
   the enclosing frame (or a try) still sees it. */
BFN Val verr_leave(void) {
    if (!_verr_exc) _verr_pending = 0;
    return vunwind;
}
BFN void verr_clear(void) { _verr_pending = 0; _verr_exc = 0; }
/* A catch binds the first argument of the error it took and keeps the tuple until
   the handler is done: releasing it at the bind would free the binding under the
   handler, never releasing it leaked one message per caught raise. */
BFN Val verr_catch_part(void) {
    Val e = verr_take();
    if (e.tag == V_TAG && e.as.tv.args && e.as.tv.args->len) return e.as.tv.args->items[0];
    return vnil();
}
BFN void verr_catch_end(void) {
    if (verr_pending()) return;  /* the handler raised: the channel owns that error now */
    vrelease(vunwind);
    vunwind = vnil();
}
BFN void vunwind_disarm(void) { /* no-op: pending channel has no jmp_buf */ }
BFN void vunwind_do(void) { _verr_pending = 1; _verr_exc = 1; }

/* The error owns its message and argument vector. A caught or stored error must not
   change when a later raise happens on the same thread, so nothing here is static. */
BFN void vraise(const char *msg) {
    if (_verr_pending) return;  /* first error wins: a poisoned follow-up must not replace it */
    Vec *a = vvec();
    vec_push(a, vstr(msg));
    vunwind = vtagv("err", a);
    _verr_pending = 1;
    _verr_exc = 1;
}
BFN void vraisef(const char *fmt, const char *arg) {
    char buf[256];
    snprintf(buf, sizeof buf, fmt, arg);
    bint n = (bint)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    vraise(buf);
}
BFN void vraisev(Val v) {
    Vec *a = vvec();
    vec_push(a, v);  /* the caller hands its reference over */
    vunwind = vtagv("err", a);
    _verr_pending = 1;
    _verr_exc = 0;
}
/* Language recursion budget: BRAT frames, not C frames (DESIGN-fast-fail A1).
   push returns nonzero once the budget is spent, without counting the frame. */
static BTLS bint _bdepth;
static bint _bbudget = 2000;
BFN void bdepth_init(void) {
    const char *e = getenv("BRAT_RECURSION_BUDGET");
    if (e && *e) {
        bint v = atol(e);
        if (v > 0) _bbudget = v;
    }
}
BFN int bdepth_push(void) {
    if (_bdepth >= _bbudget) return 1;
    _bdepth++;
    return 0;
}
BFN void bdepth_pop(void) { _bdepth--; }
static bint vargc_g;
static char **vargv_g;
static char vbase_g[4096];
static int vbase_ready;
BFN void vbase_set(const char *b) {
    if (!b || !*b) return;
    char res[4096];
    const char *rp = realpath(b, res);
    const char *use = rp ? rp : b;
    strncpy(vbase_g, use, sizeof vbase_g - 1);
    vbase_g[sizeof vbase_g - 1] = 0;
    vbase_ready = 1;
}
BFN void vargs_set(bint c, char **v) {
    vargc_g = c;
    vargv_g = v;
    
    if (!vbase_ready) {
        char tmp[4096];
        const char *src = (c > 0 && v && v[0]) ? v[0] : ".";
        const char *slash = NULL;
        for (const char *q = src; *q; q++) if (*q == '/') slash = q;
        if (slash) {
            size_t n = (size_t)(slash - src);
            if (n == 0) n = 1;
            if (n >= sizeof tmp) n = sizeof tmp - 1;
            memcpy(tmp, src, n);
            tmp[n] = 0;
        } else {
            tmp[0] = '.'; tmp[1] = 0;
        }
        char res[4096];
        const char *rp = realpath(tmp, res);
        if (rp) { strncpy(vbase_g, rp, sizeof vbase_g - 1); vbase_g[sizeof vbase_g - 1] = 0; }
        else { strncpy(vbase_g, tmp, sizeof vbase_g - 1); vbase_g[sizeof vbase_g - 1] = 0; }
        vbase_ready = 1;
    }
}

/* Returns 0 on success, -1 when a path has more components than the cap: a
   silently truncated path could point somewhere else entirely. */
BFN int vnorm(char *s) {
    int abs = (*s == '/');
    char *comps[256];
    int n = 0;
    char *cur = s;
    while (*cur) {
        while (*cur == '/') cur++;
        char *seg = cur;
        while (*cur && *cur != '/') cur++;
        size_t len = (size_t)(cur - seg);
        if (len == 0) continue;
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n > 0) n--;
            continue;
        }
        if (n == (int)(sizeof comps / sizeof *comps)) return -1;
        comps[n++] = seg;
    }
    char *o = s;
    if (abs) *o++ = '/';
    for (int i = 0; i < n; i++) {
        if (i) *o++ = '/';
        char *c = comps[i];
        while (*c && *c != '/') *o++ = *c++;
    }
    *o = 0;
    return 0;
}

/* Confine `p` to the program directory (vbase_g); callers must still re-verify
   the opened fd because realpath() and fopen() are not atomic (TOCTOU). */
/* `BRAT_FILES=1` is the runner's own opt-out, never the program's: a value of
   `0`, `no` or a typo must leave the sandbox on, or setting the variable for an
   unrelated reason would silently disarm it. */
static int _vfiles_free(void) {
    const char *v = getenv("BRAT_FILES");
    if (!v) return 0;
    while (isspace((unsigned char)*v)) v++;
    const char *values[] = {"1", "yes", "true", "on"};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        const char *p = v, *word = values[i];
        while (*p && *word && tolower((unsigned char)*p) == *word) {
            p++;
            word++;
        }
        while (isspace((unsigned char)*p)) p++;
        if (!*p && !*word) return 1;
    }
    return 0;
}

BFN const char *vconfine(const char *p) {
    if (_vfiles_free()) return p;
    if (!p) return NULL;
    if (!vbase_ready || !vbase_g[0]) return NULL; /* base must be set first */

    static BTLS char res[8192];
    size_t bl = strlen(vbase_g);
    if (p[0] == '/') snprintf(res, sizeof res, "%s", p);
    else snprintf(res, sizeof res, "%s/%s", vbase_g, p);

    char real[8192];
    if (realpath(res, real)) {
        snprintf(res, sizeof res, "%s", real);
    } else {
        if (vnorm(res) != 0) return NULL;
#if !defined(_WIN32)
        for (int hops = 0; hops < 16; hops++) {
            char dir[8192];
            snprintf(dir, sizeof dir, "%s", res);
            int resolved_parent = 0;
            for (char *slash = strrchr(dir, '/'); slash && slash != dir; slash = strrchr(dir, '/')) {
                size_t off = (size_t)(slash - dir);
                *slash = 0;
                if (realpath(dir, real)) {
                    char combined[8192];
                    snprintf(combined, sizeof combined, "%s%s", real, res + off);
                    snprintf(res, sizeof res, "%s", combined);
                    if (vnorm(res) != 0) return NULL;
                    resolved_parent = 1;
                    break;
                }
            }
            struct stat st;
            if (lstat(res, &st) == 0 && S_ISLNK(st.st_mode)) {
                char lbuf[4096];
                ssize_t ln = readlink(res, lbuf, sizeof lbuf - 1);
                if (ln < 0) return NULL;
                lbuf[ln] = 0;
                if (lbuf[0] == '/') {
                    snprintf(res, sizeof res, "%s", lbuf);
                } else {
                    char *sl = strrchr(res, '/');
                    if (!sl) return NULL;
                    char combined[8192 + 4096 + 2];
                    *sl = 0;
                    snprintf(combined, sizeof combined, "%s/%s", res, lbuf);
                    /* `res` cannot hold what `combined` may: an over-wide path
                       is refused rather than truncated into a wrong one. */
                    if (strlen(combined) >= sizeof res) return NULL;
                    memcpy(res, combined, strlen(combined) + 1);
                }
                if (vnorm(res) != 0) return NULL;
                if (realpath(res, real)) {
                    snprintf(res, sizeof res, "%s", real);
                    break;
                }
                continue;
            }
            (void)resolved_parent;
            break;
        }
#endif
    }
    if (strncmp(res, vbase_g, bl) == 0 && (res[bl] == '/' || res[bl] == 0)) return res;
    return NULL;
}

/* Re-check the opened file's resolved path against the program directory. */
static int _vfd_confined(int fd) {
#if defined(__linux__)
    char proc[64];
    char target[PATH_MAX];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, target, sizeof target - 1);
    if (n < 0) return 0;
    target[n] = 0;
#elif defined(__APPLE__)
    char target[PATH_MAX];
    if (fcntl(fd, F_GETPATH, target) != 0) return 0;
#else
    (void)fd;
    return 0;
#endif
    size_t bl = strlen(vbase_g);
    return strncmp(target, vbase_g, bl) == 0 && (target[bl] == '/' || target[bl] == 0);
}
BFN Val vargs(void) {
    Vec *o = vvec();
    for (bint i = 1; i < vargc_g; i++) vec_push(o, vstr(vargv_g[i]));
    return vvecv(o);
}
BFN Val vexit(Val v) {
    exit((int)vlong(v));
}
BFN void veprint(Vec *a) {
    for (bint i = 0; i < a->len; i++) {
        if (i) fprintf(stderr, " ");
        vprint_f(a->items[i], 0, stderr);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}
BFN Val vtime_ms(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return vint((bint)(t / 10000ULL));
#else
    struct timespec ts;
    /* `clock_gettime` rather than C11's `timespec_get`: the output is documented
       as C99, and `timespec_get`/`TIME_UTC` do not exist there. POSIX is already
       required by `fileno`, `realpath`, `strdup` and `pthread`, and the feature
       level is asked for at the top of this file. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { ts.tv_sec = 0; ts.tv_nsec = 0; }
    return vint((bint)((uint64_t)ts.tv_sec * 1000ULL
                       + (uint64_t)ts.tv_nsec / 1000000ULL));
#endif
}
BFN Val vsleep_ms(Val v) {
    bint ms = vlong(v);
    if (ms < 0) ms = 0;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    return vint(0);
}
BFN Val venv_get(Val v) {
    if (v.tag != V_STR) return vnil();
    const char *key = v.as.str;
    /* Mirror brat/runtime.py: a credential-shaped name is refused, never returned. */
    int blocked = 0;
    const char *allow = getenv("BRAT_ENV_ALLOW");
    if (allow && allow[0]) {
        int found = 0;
        const char *p = allow;
        size_t klen = strlen(key);
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ',') p++;
            const char *end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
            if ((size_t)(end - start) == klen && strncmp(start, key, klen) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) blocked = 1;
    }
    char *upper = (char *)malloc(strlen(key) + 1);
    if (!upper) vfatal_oom();
    size_t i = 0;
    for (; key[i]; i++) upper[i] = (char)toupper((unsigned char)key[i]);
    upper[i] = 0;
    static const char *deny[] = {
        "SECRET", "KEY", "TOKEN", "PASS", "PASSWORD", "CREDENTIAL", "PRIVATE",
        "DATABASE", "DB_", "AUTH", "DSN", "CONN", "COOKIE", "SESSION", "BEARER",
        "REDIS", "MONGO", "POSTGRES", "MYSQL", "AMQP", "AWS_", "GCP_", "AZURE_",
        "GITHUB_", "OPENAI_", "ANTHROPIC_", "STRIPE_", "WEBHOOK", "SIGNING",
        "CERT", "SALT", "SEED", 0
    };
    for (int b = 0; deny[b] && !blocked; b++) blocked = strstr(upper, deny[b]) != 0;
    free(upper);
    const char *r = blocked ? NULL : getenv(key);
    if (!blocked && r) {
        if ((strstr(r, "://") && strchr(r, '@')) || strstr(r, "-----BEGIN"))
            blocked = 1;
    }
    if (blocked) {
        char *o = (char *)malloc(strlen(key) * 2 + 32);
        if (!o) vfatal_oom();
        bint n = vput(o, "env_get refused: ");
        n += vsprint_q(v, o + n, 1);
        o[n] = 0;
        Vec *e = vvec();
        vec_push(e, vstrn(o, n));
        free(o);
        return vtagv("err", e);
    }
    return r ? vstr(r) : vnil();
}

typedef struct { char kind; char q; char ch; const char *body; } RTok;

static int rtok_match(RTok *t, char ch) {
    if (t->kind == 'l') return ch == t->ch;
    if (t->kind == 'd') return 1;
    if (t->kind == 's') {
        const char *body = t->body;
        int neg = 0, found = 0;
        if (*body == '^') { neg = 1; body++; }
        while (*body) {
            if (body[1] == '-' && body[2]) {
                if (ch >= body[0] && ch <= body[2]) { found = 1; break; }
                body += 3;
            } else {
                if (*body == ch) { found = 1; break; }
                body++;
            }
        }
        return neg ? !found : found;
    }
    return 0;
}


typedef struct { char op; char q; char ch; char owned; const char *body; bint x, y; } RInst;
typedef struct { RInst *p; bint n, cap; const char *pat; bint i, plen; int err; int empty_seq; } RProg;

static bint r_emit(RProg *g, char op, bint x, bint y) {
    if (g->n >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 64;
        g->p = (RInst *)bgrow(g->p, sizeof(RInst) * (size_t)g->cap);
    }
    RInst *in = &g->p[g->n];
    in->op = op; in->q = 0; in->ch = 0; in->owned = 0; in->body = NULL; in->x = x; in->y = y;
    return g->n++;
}

static bint r_alt(RProg *g);


static bint r_atom(RProg *g) {
    bint sp = r_emit(g, 's', 0, 0);
    bint body = -1;
    if (g->i >= g->plen) { g->err = 1; return -1; }
    char c = g->pat[g->i];
    if (c == '(') {
        g->i++;
        body = r_alt(g);
        if (g->err) return -1;
        if (g->i >= g->plen || g->pat[g->i] != ')') { g->err = 1; return -1; }
        g->i++;
    } else if (c == '[') {
        
        bint j = g->i + 1;
        bint m = j;
        while (m < g->plen && g->pat[m] != ']') m++;
        char *buf = (char *)malloc((size_t)(m - j + 2));
        if (!buf) vfatal_oom();
        int k = 0;
        if (j < g->plen && g->pat[j] == '^') buf[k++] = g->pat[j++];
        while (j < g->plen && g->pat[j] != ']') buf[k++] = g->pat[j++];
        buf[k] = 0;
        g->i = j + 1;
        body = r_emit(g, 'c', 0, 0);
        g->p[body].q = 's';
        g->p[body].owned = 1;
        g->p[body].body = buf;
    } else if (c == '.') {
        g->i++;
        body = r_emit(g, 'c', 0, 0);
        g->p[body].q = 'd';
    } else if (c == '^') {
        g->i++;
        body = r_emit(g, 'b', 0, 0);
    } else if (c == '$') {
        g->i++;
        body = r_emit(g, 'e', 0, 0);
    } else if (c == '\\') {
        char e = (g->i + 1 < g->plen) ? g->pat[g->i + 1] : '\\';
        g->i += 2;
        body = r_emit(g, 'c', 0, 0);
        if (e == 'd') { g->p[body].q = 's'; g->p[body].body = "0-9"; }
        else if (e == 'D') { g->p[body].q = 's'; g->p[body].body = "^0-9"; }
        else if (e == 'w') { g->p[body].q = 's'; g->p[body].body = "a-zA-Z0-9_"; }
        else if (e == 'W') { g->p[body].q = 's'; g->p[body].body = "^a-zA-Z0-9_"; }
        else if (e == 's') { g->p[body].q = 's'; g->p[body].body = " \t\n\r"; }
        else if (e == 'S') { g->p[body].q = 's'; g->p[body].body = "^ \t\n\r"; }
        else if (e == 'n') { g->p[body].q = 'l'; g->p[body].ch = '\n'; }
        else if (e == 't') { g->p[body].q = 'l'; g->p[body].ch = '\t'; }
        else if (e == 'r') { g->p[body].q = 'l'; g->p[body].ch = '\r'; }
        else { g->p[body].q = 'l'; g->p[body].ch = e; }
    } else {
        g->i++;
        body = r_emit(g, 'c', 0, 0);
        g->p[body].q = 'l';
        g->p[body].ch = c;
    }
    if (g->err) return -1;
    char q = 0;
    if (g->i < g->plen) {
        char cq = g->pat[g->i];
        if (cq == '*' || cq == '+' || cq == '?') { q = cq; g->i++; }
    }
    if (!q) {
        g->p[sp].op = 'j';
        
        g->p[sp].x = body;
        return sp;
    }
    if (q == '*') {
        bint jmp = r_emit(g, 'j', 0, 0);
        g->p[jmp].x = sp;
        g->p[sp].x = body;
        g->p[sp].y = g->n;
        return sp;
    }
    if (q == '+') {
        
        g->p[sp].op = 'j';
        g->p[sp].x = body;
        bint sp2 = r_emit(g, 's', 0, 0);
        g->p[sp2].x = body;
        g->p[sp2].y = g->n;
        return sp;
    }
    
    g->p[sp].x = body;
    g->p[sp].y = g->n;
    return sp;
}

static bint r_seq(RProg *g) {
    
    bint entry = r_emit(g, 'j', 0, 0);
    bint first_atom = -1;
    while (g->i < g->plen && !g->err) {
        char c = g->pat[g->i];
        if (c == '|' || c == ')') break;
        bint a = r_atom(g);
        if (g->err) return -1;
        if (first_atom < 0) first_atom = a;
    }
    if (g->err) return -1;
    g->p[entry].x = (first_atom < 0) ? entry : first_atom;
    g->empty_seq = (first_atom < 0);
    return entry;
}

static bint r_alt(RProg *g) {
    
    bint head = r_emit(g, 's', 0, 0);
    r_seq(g);
    if (g->err) return -1;
    if (!(g->i < g->plen && g->pat[g->i] == '|')) {
        g->p[head].op = 'j';
        g->p[head].x = head + 1;
        return head;
    }
    bint j1 = r_emit(g, 'j', 0, 0);
    bint cap = g->plen + 2;
    bint *sps = (bint *)malloc(sizeof(bint) * (size_t)cap);
    bint *jmps = (bint *)malloc(sizeof(bint) * (size_t)cap);
    if (!sps || !jmps) { free(sps); free(jmps); vfatal_oom(); }
    int ns = 0, nj = 0;
    while (g->i < g->plen && g->pat[g->i] == '|') {
        g->i++;
        bint sp = r_emit(g, 's', 0, 0);
        sps[ns++] = sp;
        bint s0 = r_seq(g);
        if (g->err) { free(sps); free(jmps); return -1; }
        bint j = r_emit(g, 'j', 0, 0);
        jmps[nj++] = j;
        if (g->empty_seq) g->p[s0].x = j;
    }
    bint fail = r_emit(g, 'f', 0, 0);
    bint exit = g->n;
    g->p[head].x = head + 1;
    g->p[head].y = (ns > 0) ? sps[0] : fail;
    g->p[j1].x = exit;
    for (int k = 0; k < ns; k++) {
        g->p[sps[k]].x = sps[k] + 1;
        g->p[sps[k]].y = (k + 1 < ns) ? sps[k + 1] : fail;
    }
    for (int k = 0; k < nj; k++) g->p[jmps[k]].x = exit;
    free(sps); free(jmps);
    return head;
}


/* A `[...]` class is the only part of the compiled program that the pattern
   parser mallocs itself; the rest of `body` points into the pattern or into a
   literal, so the flag is what says which ones this frees. */
static void r_free(RProg *g) {
    for (bint k = 0; k < g->n; k++) if (g->p[k].owned) free((void *)g->p[k].body);
    free(g->p);
    g->p = NULL;
    g->n = 0;
}

static void re_close(RInst *p, bint pc, const char *s, bint si, bint *out, bint *nout, bint *seen) {
    bint stack[512];
#define RSTACK_MAX 512
    int sp = 0;
    stack[sp++] = pc;
    while (sp > 0) {
        bint q = stack[--sp];
        if (q < 0 || seen[q]) continue;
        seen[q] = 1;
        RInst *in = &p[q];
        
        if (in->op == 'j') { if (sp < RSTACK_MAX) stack[sp++] = in->x; }
        else if (in->op == 's') { if (sp + 2 <= RSTACK_MAX) { stack[sp++] = in->x; stack[sp++] = in->y; } }
        else if (in->op == 'b') { if (si == 0 && sp < RSTACK_MAX) stack[sp++] = q + 1; }
        else if (in->op == 'e') { if (s[si] == 0 && sp < RSTACK_MAX) stack[sp++] = q + 1; }
        else if (in->op == 'f') {  }
        else out[(*nout)++] = q;   
    }
}

BFN int vre_match(Val sv, Val pv) {
    if (sv.tag != V_STR || pv.tag != V_STR) { vraise("re_match wants str"); return 0; }
    const char *s = sv.as.str, *pat = pv.as.str;
    RProg g;
    g.p = NULL; g.n = 0; g.cap = 0; g.pat = pat; g.i = 0; g.plen = (bint)strlen(pat); g.err = 0;
    g.empty_seq = 0;
    bint start = r_alt(&g);
    if (g.err || start < 0 || g.i != g.plen) { r_free(&g); vraise("bad regex pattern"); return 0; }
    r_emit(&g, 'm', 0, 0);
    for (bint k = 0; k < g.n; k++) {
        if (g.p[k].op == 'j' && g.p[k].x == k) g.p[k].x = g.n - 1;
    }
    bint nprog = g.n;
    bint *list = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    bint *next = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    bint *seen = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    if (!list || !next || !seen) { free(list); free(next); free(seen); vfatal_oom(); }
    bint slen = (bint)strlen(s);
    for (bint from = 0; from <= slen; from++) {
        bint nlist = 0;
        memset(seen, 0, sizeof(bint) * (size_t)(nprog + 1));
        re_close(g.p, start, s, from, list, &nlist, seen);
        for (bint si = from; ; si++) {
            
            for (bint k = 0; k < nlist; k++) {
                if (g.p[list[k]].op == 'm') { free(list); free(next); free(seen); r_free(&g); return 1; }
            }
            if (!s[si] || nlist == 0) break;
            bint nn = 0;
            memset(seen, 0, sizeof(bint) * (size_t)(nprog + 1));
            for (bint k = 0; k < nlist; k++) {
                RInst *in = &g.p[list[k]];
                if (in->op != 'c') continue;
                if (in->q == 'l') { if (s[si] != in->ch) continue; }
                else if (in->q == 'd') {  }
                else {
                    RTok t; t.kind = 's'; t.q = 0; t.ch = 0; t.body = in->body;
                    if (!rtok_match(&t, s[si])) continue;
                }
                re_close(g.p, list[k] + 1, s, si + 1, next, &nn, seen);
            }
            bint *tmp = list; list = next; next = tmp;
            nlist = nn;
        }
        if (nlist > 0) {
            for (bint k = 0; k < nlist; k++) {
                if (g.p[list[k]].op == 'm') { free(list); free(next); free(seen); r_free(&g); return 1; }
            }
        }
    }
    free(list); free(next); free(seen); r_free(&g);
    return 0;
}

BFN Val vtag_payload(Val v);
BFN Val vquest(Val v) {
    if (v.tag == V_TAG && !strcmp(v.as.tv.tag, "err")) {
        vunwind = v;
        vunwind_do();
    }
    return v;
}
BFN Val vquest_ok(Val v) {
    if (v.tag == V_TAG && (!strcmp(v.as.tv.tag, "ok") || !strcmp(v.as.tv.tag, "some")))
        return vtag_payload(v);
    /* Not an Ok: keep the value alive for the caller, who releases it. An
     * error is the exception: it belongs to the error channel and counting it
     * would outlive the catch. */
    if (verr_pending()) return v;
    return vrc_inc(v);
}

/* Wall clock, not CPU time: a log line has to be comparable with timestamps
 * printed by other processes, so seconds since the epoch, like `time.time()`. */
BFN double vtime(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)t / 10000000.0 - 11644473600.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

BFN bint vsprint_len(Val v);
BFN bint vsprint(Val v, char *o);
BFN bint vsprint_len_q(Val v, bint q);
BFN bint vsprint_q(Val v, char *o, bint q);

BFN Val vfield(Val v, const char *k) {
    if (v.tag == V_STRUCT) {
        Val *o = map_get(v.as.map, k);
        if (!o) { vraisef("no field %s\n", k); return vunwind; }
        return *o;
    }
    vraisef("no field %s\n", k); return vunwind;
}


BFN Val verr(const char *msg) {
    Vec *e = vvec();
    vec_push(e, vstr(msg));
    return vtagv("err", e);
}

BFN Val verr_os(const char *rp) {
    char m[9000];
    snprintf(m, sizeof m, "[Errno %d] %s: '%s'", errno, strerror(errno), rp);
    return verr(m);
}
BFN Val vok1(const char *s) {
    Vec *a = vvec();
    vec_push(a, vstr(s));
    return vtagv("ok", a);
}
BFN Val vreadfile(Val p) {
    if (p.tag != V_STR) return verr("read_file wants a string path");
    const char *rp = vconfine(p.as.str);
    if (!rp) return verr("path outside the program directory");
    errno = 0;
    FILE *f = fopen(rp, "rb");
    if (!f) return verr_os(rp);
    if (!_vfd_confined(fileno(f))) { fclose(f); return verr("path escaped during open"); }
    char buf[4096];
    size_t n;
    size_t total = 0;
    char *data = NULL;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        data = (char *)bgrow(data, total + n + 1);
        memcpy(data + total, buf, n);
        total += n;
    }
    
    int failed = ferror(f);
    int rerr = errno;
    fclose(f);
    if (failed) { errno = rerr; return verr_os(rp); }
    
    if (!data) return vok1("");
    data[total] = 0;
    if (!utf8_valid(data, (bint)total) || memchr(data, 0, total) != NULL) {
        free(data);
        return verr("invalid utf-8");
    }
    Vec *a = vvec();
    vec_push(a, vstrn(data, (bint)total));
    free(data);
    return vtagv("ok", a);
}

#if !defined(_WIN32)
#include <sys/time.h>
#endif

/* Socket slots are shared by all spawn threads, so the table has a lock. It is
 * never held across a blocking call: a slot is read under the lock, and the
 * call runs on the fd after the lock is released. */
#if defined(_WIN32)
static CRITICAL_SECTION _vsk_cs;
static int _vsk_cs_ready = 0;
static void _vsk_lock(void) {
    if (!_vsk_cs_ready) { InitializeCriticalSection(&_vsk_cs); _vsk_cs_ready = 1; }
    EnterCriticalSection(&_vsk_cs);
}
static void _vsk_unlock(void) { LeaveCriticalSection(&_vsk_cs); }
#else
static pthread_mutex_t _vsk_mu = PTHREAD_MUTEX_INITIALIZER;
static void _vsk_lock(void) { pthread_mutex_lock(&_vsk_mu); }
static void _vsk_unlock(void) { pthread_mutex_unlock(&_vsk_mu); }
#endif

#define _VSK_MAX 64
#define _VSK_TIMEOUT_S 30        /* connected sockets: a silent peer frees its thread */
#define _VRECV_MAX (1L << 20)    /* one tcp_recv returns at most 1 MiB */

static int _vsk[_VSK_MAX];
static int _vsk_ready = 0;

/* Callers hold the lock. */
static void _vsk_ensure(void) {
    if (_vsk_ready) return;
    for (int i = 0; i < _VSK_MAX; i++) _vsk[i] = -1;
    _vsk_ready = 1;
}
static int _vsk_put(int fd) {
    int slot = -1;
    _vsk_lock();
    _vsk_ensure();
    for (int i = 1; i < _VSK_MAX; i++) {
        if (_vsk[i] < 0) { _vsk[i] = fd; slot = i; break; }
    }
    _vsk_unlock();
    return slot;
}
static int _vsk_get(bint slot) {
    int fd = -1;
    _vsk_lock();
    _vsk_ensure();
    if (slot >= 1 && slot < _VSK_MAX) fd = _vsk[slot];
    _vsk_unlock();
    return fd;
}
/* Takes the fd out of its slot atomically; the caller closes it. */
static int _vsk_take(bint slot) {
    int fd = -1;
    _vsk_lock();
    _vsk_ensure();
    if (slot >= 1 && slot < _VSK_MAX && _vsk[slot] >= 0) {
        fd = _vsk[slot];
        _vsk[slot] = -1;
    }
    _vsk_unlock();
    return fd;
}
static int _vsk_slot_id(Val slot, bint *id) {
    if (slot.tag != V_INT) return 0;
    *id = slot.as.ival;
    return 1;
}

/* A write to a peer that has gone away must return EPIPE, not kill the process.
 * Linux and Windows take MSG_NOSIGNAL / have no SIGPIPE; macOS needs the socket option. */
static void _vsk_nosigpipe(int fd) {
#if defined(__APPLE__)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#else
    (void)fd;
#endif
}
#if defined(MSG_NOSIGNAL)
#define _VSEND_FLAGS MSG_NOSIGNAL
#else
#define _VSEND_FLAGS 0
#endif

/* Connected sockets only. The listening socket gets no timeout, or an idle server would die. */
static void _vsk_timeouts(int fd) {
#if defined(_WIN32)
    DWORD ms = _VSK_TIMEOUT_S * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv;
    tv.tv_sec = _VSK_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static Val _vok_int(bint n) { Vec *a = vvec(); vec_push(a, vint(n)); return vtagv("ok", a); }
static Val _verr_c(const char *m) { Vec *a = vvec(); vec_push(a, vstr(m)); return vtagv("err", a); }

/* "host:port". Returns 0 for anything malformed: a host that does not fit is
 * refused, not cut short; the port must be decimal digits giving 1..65535. */
static int _vhost_port(const char *c, char *host, size_t hcap, int *port) {
    const char *colon = strrchr(c, ':');
    if (!colon || colon == c) return 0;
    size_t hl = (size_t)(colon - c);
    if (hl >= hcap) return 0;
    memcpy(host, c, hl);
    host[hl] = 0;
    const char *p = colon + 1;
    if (!*p) return 0;
    bint v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 65535) return 0;
    }
    if (v < 1) return 0;
    *port = (int)v;
    return 1;
}

typedef struct { char *buf; bint len, cap; int used; } VSbuf;
static VSbuf _vsb[64];
static pthread_mutex_t _vsb_mu = PTHREAD_MUTEX_INITIALIZER;

static VSbuf *_vsb_get(bint h) {
    if (h < 1 || h >= 64) return NULL;
    if (!_vsb[h].used) return NULL;
    return &_vsb[h];
}
BFN Val vsbuf(void) {
    /* One table for the whole process: claiming a slot is a critical section,
     * or two threads build into the same buffer. */
    pthread_mutex_lock(&_vsb_mu);
    for (int i = 1; i < 64; i++) {
        if (!_vsb[i].used) {
            _vsb[i].used = 1;
            _vsb[i].len = 0;
            _vsb[i].cap = 16;
            _vsb[i].buf = (char *)malloc((size_t)_vsb[i].cap);
            if (!_vsb[i].buf) { _vsb[i].used = 0; pthread_mutex_unlock(&_vsb_mu); vfatal_oom(); }
            _vsb[i].buf[0] = 0;
            pthread_mutex_unlock(&_vsb_mu);
            return vint(i);
        }
    }
    pthread_mutex_unlock(&_vsb_mu);
    return _verr_c("too many builders");
}
/* One builder may be grown from two threads only if its owner shares it, which
 * the checker refuses; the lock here keeps the buffer itself coherent and the
 * copy out of it consistent with the length. */
BFN Val vsbuf_add(Val h, Val part) {
    VSbuf *b = _vsb_get((bint)vlong(h));
    if (!b) return _verr_c("bad builder");
    if (part.tag != V_STR) return _verr_c("sbuf_add wants a string");
    size_t pl = strlen(part.as.str);
    pthread_mutex_lock(&_vsb_mu);
    while (b->len + (bint)pl + 1 > b->cap) {
        b->cap *= 2;
        b->buf = (char *)bgrow(b->buf, (size_t)b->cap);
    }
    memcpy(b->buf + b->len, part.as.str, pl);
    b->len += (bint)pl;
    b->buf[b->len] = 0;
    bint len = b->len;
    pthread_mutex_unlock(&_vsb_mu);
    return vint(len);
}
BFN Val vsbuf_len(Val h) {
    VSbuf *b = _vsb_get((bint)vlong(h));
    if (!b) return _verr_c("bad builder");
    pthread_mutex_lock(&_vsb_mu);
    bint len = b->len;
    pthread_mutex_unlock(&_vsb_mu);
    return vint(len);
}
BFN Val vsbuf_str(Val h) {
    VSbuf *b = _vsb_get((bint)vlong(h));
    if (!b) return _verr_c("bad builder");
    pthread_mutex_lock(&_vsb_mu);
    Val text = vstrn(b->buf, b->len);
    pthread_mutex_unlock(&_vsb_mu);
    Vec *a = vvec();
    vec_push(a, text);
    return vtagv("ok", a);
}
BFN Val vtcp_listen(Val addr) {
    if (addr.tag != V_STR) return _verr_c("tcp_listen wants a string address");
    char host[256]; int port = 0;
    if (!_vhost_port(addr.as.str, host, sizeof host, &port))
        return _verr_c("address wants host:port with port 1..65535");
    uint64_t ip = inet_addr(host);
    if (ip == INADDR_NONE) return _verr_c("host must be an IPv4 address");
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return _verr_c("socket failed");
    _vsk_nosigpipe(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = (in_addr_t)ip;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return _verr_c("bind failed"); }
    if (listen(fd, 16) != 0) { close(fd); return _verr_c("listen failed"); }
    int slot = _vsk_put(fd);
    if (slot < 0) { close(fd); return _verr_c("too many sockets"); }
    return _vok_int(slot);
}
BFN Val vtcp_connect(Val addr) {
    if (addr.tag != V_STR) return _verr_c("tcp_connect wants a string address");
    char host[256]; int port = 0;
    if (!_vhost_port(addr.as.str, host, sizeof host, &port))
        return _verr_c("address wants host:port with port 1..65535");
    uint64_t ip = inet_addr(host);
    if (ip == INADDR_NONE) return _verr_c("host must be an IPv4 address");
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return _verr_c("socket failed");
    _vsk_nosigpipe(fd);
    _vsk_timeouts(fd);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = (in_addr_t)ip;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return _verr_c("connect failed"); }
    int slot = _vsk_put(fd);
    if (slot < 0) { close(fd); return _verr_c("too many sockets"); }
    return _vok_int(slot);
}
BFN Val vtcp_accept(Val slot) {
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    int lfd = _vsk_get(id);
    if (lfd < 0) return _verr_c("bad socket slot");
    int fd = (int)accept(lfd, NULL, NULL);
    if (fd < 0) return _verr_c("accept failed");
    _vsk_nosigpipe(fd);
    _vsk_timeouts(fd);
    int ns = _vsk_put(fd);
    if (ns < 0) { close(fd); return _verr_c("too many sockets"); }
    return _vok_int(ns);
}

BFN Val vtcp_recv(Val slot, Val n) {
    if (n.tag != V_INT) return _verr_c("recv wants 0 <= n <= 1048576");
    bint want = n.as.ival;
    if (want < 0 || want > _VRECV_MAX) return _verr_c("recv wants 0 <= n <= 1048576");
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    int fd = _vsk_get(id);
    if (fd < 0) return _verr_c("bad socket slot");
    char *buf = (char *)malloc((size_t)want + 1);
    if (!buf) vfatal_oom();
    bint got = (bint)recv(fd, buf, (size_t)want, 0);
    if (got < 0) { free(buf); return _verr_c("recv failed"); }
    buf[got] = 0;
    if (!utf8_valid(buf, got) || memchr(buf, 0, (size_t)got) != NULL) {
        free(buf);
        return verr("invalid utf-8");
    }
    Vec *a = vvec();
    vec_push(a, vstrn(buf, got));
    free(buf);
    return vtagv("ok", a);
}
BFN Val vtcp_send(Val slot, Val data) {
    if (data.tag != V_STR) return _verr_c("tcp_send wants string data");
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    int fd = _vsk_get(id);
    if (fd < 0) return _verr_c("bad socket slot");
    const char *p = data.as.str;
    size_t left = strlen(p);
    size_t done = 0;
    while (left > 0) {
#if defined(_WIN32)
        int k = send(fd, p + done, (int)left, 0);
#else
        ssize_t k = send(fd, p + done, left, _VSEND_FLAGS);
#endif
        if (k < 0) {
            if (errno == EINTR) continue;
            return _verr_c("send failed");
        }
        if (k == 0) return _verr_c("send made no progress");
        done += (size_t)k;
        left -= (size_t)k;
    }
    return _vok_int((bint)done);
}
BFN Val vtcp_close(Val slot) {
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    int fd = _vsk_take(id);
    if (fd < 0) return _verr_c("bad socket slot");
    close(fd);
    return _vok_int(0);
}
/* own(x): a deep copy that lives on the heap, so it survives the arena that
 * made x. Arena allocation is suspended for the copy. */
/* `own` copies a value out of an arena. Every container it builds owns the
 * copies it holds, so releasing it releases the whole tree; the flag is what
 * separates this from an ordinary container, whose elements alias bindings
 * elsewhere and must outlive it. */
static Val vown_copy(Val v) {
    switch (v.tag) {
    case V_STR:
        return v.as.str ? vstr(v.as.str) : v;
    case V_VEC: {
        if (!v.as.vec) return v;
        Vec *dst = vvec();
        for (bint i = 0; i < v.as.vec->len; i++) vec_push(dst, vown_copy(v.as.vec->items[i]));
        return vvecv(dst);
    }
    case V_STRUCT: {
        if (!v.as.map) return v;
        Map *src = v.as.map;
        Map *dst = vmap(src->type);
        for (bint i = 0; i < src->len; i++)
            map_set_v(dst, vown_copy(src->keys[i]), vown_copy(src->vals[i]));
        return vstruct(dst);
    }
    case V_TAG: {
        Vec *args = v.as.tv.args;
        if (!args) return vtagv(v.as.tv.tag, NULL);
        Vec *dst = vvec();
        for (bint i = 0; i < args->len; i++) vec_push(dst, vown_copy(args->items[i]));
        return vtagv(v.as.tv.tag, dst);
    }
    default:
        return v;
    }
}
BFN Val vown(Val v) {
    int saved = barena_depth;
    barena_depth = 0;
    Val r = vown_copy(v);
    barena_depth = saved;
    return r;
}

BFN Val vwritefile(Val p, Val t) {
    if (p.tag != V_STR || t.tag != V_STR) return verr("write_file wants string path and text");
    const char *rp = vconfine(p.as.str);
    if (!rp) return verr("path outside the program directory");
    errno = 0;
#if !defined(_WIN32)
    FILE *f = NULL;
    if (!_vfiles_free()) {
        int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(rp, flags, 0666);
        if (fd < 0) return verr_os(rp);
        if (!_vfd_confined(fd)) { close(fd); return verr("path escaped during open"); }
        if (ftruncate(fd, 0) != 0) {
            int e = errno;
            close(fd);
            errno = e;
            return verr_os(rp);
        }
        f = fdopen(fd, "wb");
        if (!f) {
            int e = errno;
            close(fd);
            errno = e;
            return verr_os(rp);
        }
    } else {
        f = fopen(rp, "wb");
        if (!f) return verr_os(rp);
    }
#else
    FILE *f = fopen(rp, "wb");
    if (!f) return verr_os(rp);
    if (!_vfd_confined(fileno(f))) { fclose(f); return verr("path escaped during open"); }
#endif
    size_t n = strlen(t.as.str);
    errno = 0;
    size_t w = n ? fwrite(t.as.str, 1, n, f) : 0;
    int werr = errno;
    int ferr = ferror(f);
    errno = 0;
    int cerr = fclose(f);
    int clerr = errno;
    if (w != n || ferr) { errno = werr ? werr : EIO; return verr_os(rp); }
    if (cerr != 0) { errno = clerr ? clerr : EIO; return verr_os(rp); }
    Vec *a = vvec();
    vec_push(a, vint((bint)n));
    return vtagv("ok", a);
}
/* Raise `fn: strerror(errno)`. Format before raising: vraise allocates and may clobber errno. */
BFN void vraise_errno(const char *fn) {
    char m[256];
    snprintf(m, sizeof m, "%s: %s", fn, strerror(errno));
    vraise(m);
}

BFN Val vstr_ffi(const char *s) {
    if (!s) { vraise("ffi: C returned NULL where str expected"); return vnil(); }
    return vstr(s);
}

BFN Val vstr_of(Val v) {
    if (v.tag == V_STR) return vstr(v.as.str);
    
    bint n = vsprint_len_q(v, 0);
    char *b = (char *)malloc(n + 1);
    if (!b) vfatal_oom();
    vsprint_q(v, b, 0);
    b[n] = 0;
    Val r = vstrn(b, n);
    free(b);
    return r;
}
BFN Val vsplit(Val s, Val sep) {
    Vec *out = vvec();
    if (s.tag != V_STR) { vraise("no method split"); return vunwind; }
    const char *p = s.as.str;
    if (sep.tag != V_STR) {
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (!*p) break;
            const char *q = p;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n') q++;
            vec_push(out, vstrn(p, q - p));
            p = q;
        }
        return vvecv(out);
    }
    size_t dl = strlen(sep.as.str);
    if (!dl) { vec_push(out, vstr(p)); return vvecv(out); }
    const char *q;
    while ((q = strstr(p, sep.as.str)) != 0) {
        vec_push(out, vstrn(p, q - p));
        p = q + dl;
    }
    vec_push(out, vstr(p));
    return vvecv(out);
}
BFN Val vtrim(Val s) {
    if (s.tag != V_STR) { vraise("no method trim"); return vunwind; }
    const char *p = s.as.str;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    bint n = strlen(p);
    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\n' || p[n-1] == '\r')) n--;
    return vstrn(p, n);
}

BFN bint vcp_next(const char *p, unsigned *out) {
    unsigned c = (unsigned char)p[0];
    bint n = 1;
    if (c >= 0xF0) { c &= 0x07; n = 4; }
    else if (c >= 0xE0) { c &= 0x0F; n = 3; }
    else if (c >= 0xC0) { c &= 0x1F; n = 2; }
    /* p[n-1] may be past the NUL when the sequence is truncated. */
    bint have = 1;
    while (have < n && p[have]) have++;
    n = have;
    for (bint i = 1; i < n; i++) c = (c << 6) | ((unsigned char)p[i] & 0x3F);
    *out = c;
    return n;
}


BFN bint vcp_put(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}


/* Simple case mapping, generated from Python's unicodedata (lang/tests/test_case_map.py
   checks every code point). A run {start, count, stride, delta} maps start + k*stride
   (k < count) to itself + delta. Runs may interleave, so lookup scans; ASCII is
   handled before the scan. Multi-character mappings (SS, i-dot) live in vcase. */
typedef struct { unsigned start, count, stride; int delta; } CaseRun;

#ifndef BRAT_CASE_MAP_VERSION
#define BRAT_CASE_MAP_VERSION 1500
#endif

static const CaseRun BCASE_UP[] = {
    {0x61, 26, 1, -32},
    {0xB5, 1, 1, 743},
    {0xE0, 23, 1, -32},
    {0xF8, 7, 1, -32},
    {0xFF, 1, 1, 121},
    {0x101, 24, 2, -1},
    {0x131, 1, 1, -232},
    {0x133, 3, 2, -1},
    {0x13A, 8, 2, -1},
    {0x14B, 23, 2, -1},
    {0x17A, 3, 2, -1},
    {0x17F, 1, 1, -300},
    {0x180, 1, 1, 195},
    {0x183, 2, 2, -1},
    {0x188, 1, 1, -1},
    {0x18C, 1, 1, -1},
    {0x192, 1, 1, -1},
    {0x195, 1, 1, 97},
    {0x199, 1, 1, -1},
    {0x19A, 1, 1, 163},
    {0x19E, 1, 1, 130},
    {0x1A1, 3, 2, -1},
    {0x1A8, 1, 1, -1},
    {0x1AD, 1, 1, -1},
    {0x1B0, 1, 1, -1},
    {0x1B4, 2, 2, -1},
    {0x1B9, 1, 1, -1},
    {0x1BD, 1, 1, -1},
    {0x1BF, 1, 1, 56},
    {0x1C5, 1, 1, -1},
    {0x1C6, 1, 1, -2},
    {0x1C8, 1, 1, -1},
    {0x1C9, 1, 1, -2},
    {0x1CB, 1, 1, -1},
    {0x1CC, 1, 1, -2},
    {0x1CE, 8, 2, -1},
    {0x1DD, 1, 1, -79},
    {0x1DF, 9, 2, -1},
    {0x1F2, 1, 1, -1},
    {0x1F3, 1, 1, -2},
    {0x1F5, 1, 1, -1},
    {0x1F9, 20, 2, -1},
    {0x223, 9, 2, -1},
    {0x23C, 1, 1, -1},
    {0x23F, 2, 1, 10815},
    {0x242, 1, 1, -1},
    {0x247, 5, 2, -1},
    {0x250, 1, 1, 10783},
    {0x251, 1, 1, 10780},
    {0x252, 1, 1, 10782},
    {0x253, 1, 1, -210},
    {0x254, 1, 1, -206},
    {0x256, 2, 1, -205},
    {0x259, 1, 1, -202},
    {0x25B, 1, 1, -203},
    {0x25C, 1, 1, 42319},
    {0x260, 1, 1, -205},
    {0x261, 1, 1, 42315},
    {0x263, 1, 1, -207},
    {0x265, 1, 1, 42280},
    {0x266, 1, 1, 42308},
    {0x268, 1, 1, -209},
    {0x269, 1, 1, -211},
    {0x26A, 1, 1, 42308},
    {0x26B, 1, 1, 10743},
    {0x26C, 1, 1, 42305},
    {0x26F, 1, 1, -211},
    {0x271, 1, 1, 10749},
    {0x272, 1, 1, -213},
    {0x275, 1, 1, -214},
    {0x27D, 1, 1, 10727},
    {0x280, 1, 1, -218},
    {0x282, 1, 1, 42307},
    {0x283, 1, 1, -218},
    {0x287, 1, 1, 42282},
    {0x288, 1, 1, -218},
    {0x289, 1, 1, -69},
    {0x28A, 2, 1, -217},
    {0x28C, 1, 1, -71},
    {0x292, 1, 1, -219},
    {0x29D, 1, 1, 42261},
    {0x29E, 1, 1, 42258},
    {0x345, 1, 1, 84},
    {0x371, 2, 2, -1},
    {0x377, 1, 1, -1},
    {0x37B, 3, 1, 130},
    {0x3AC, 1, 1, -38},
    {0x3AD, 3, 1, -37},
    {0x3B1, 17, 1, -32},
    {0x3C2, 1, 1, -31},
    {0x3C3, 9, 1, -32},
    {0x3CC, 1, 1, -64},
    {0x3CD, 2, 1, -63},
    {0x3D0, 1, 1, -62},
    {0x3D1, 1, 1, -57},
    {0x3D5, 1, 1, -47},
    {0x3D6, 1, 1, -54},
    {0x3D7, 1, 1, -8},
    {0x3D9, 12, 2, -1},
    {0x3F0, 1, 1, -86},
    {0x3F1, 1, 1, -80},
    {0x3F2, 1, 1, 7},
    {0x3F3, 1, 1, -116},
    {0x3F5, 1, 1, -96},
    {0x3F8, 1, 1, -1},
    {0x3FB, 1, 1, -1},
    {0x430, 32, 1, -32},
    {0x450, 16, 1, -80},
    {0x461, 17, 2, -1},
    {0x48B, 27, 2, -1},
    {0x4C2, 7, 2, -1},
    {0x4CF, 1, 1, -15},
    {0x4D1, 48, 2, -1},
    {0x561, 38, 1, -48},
    {0x10D0, 43, 1, 3008},
    {0x10FD, 3, 1, 3008},
    {0x13F8, 6, 1, -8},
    {0x1C80, 1, 1, -6254},
    {0x1C81, 1, 1, -6253},
    {0x1C82, 1, 1, -6244},
    {0x1C83, 2, 1, -6242},
    {0x1C85, 1, 1, -6243},
    {0x1C86, 1, 1, -6236},
    {0x1C87, 1, 1, -6181},
    {0x1C88, 1, 1, 35266},
    {0x1D79, 1, 1, 35332},
    {0x1D7D, 1, 1, 3814},
    {0x1D8E, 1, 1, 35384},
    {0x1E01, 75, 2, -1},
    {0x1E9B, 1, 1, -59},
    {0x1EA1, 48, 2, -1},
    {0x1F00, 8, 1, 8},
    {0x1F10, 6, 1, 8},
    {0x1F20, 8, 1, 8},
    {0x1F30, 8, 1, 8},
    {0x1F40, 6, 1, 8},
    {0x1F51, 4, 2, 8},
    {0x1F60, 8, 1, 8},
    {0x1F70, 2, 1, 74},
    {0x1F72, 4, 1, 86},
    {0x1F76, 2, 1, 100},
    {0x1F78, 2, 1, 128},
    {0x1F7A, 2, 1, 112},
    {0x1F7C, 2, 1, 126},
    {0x1FB0, 2, 1, 8},
    {0x1FBE, 1, 1, -7205},
    {0x1FD0, 2, 1, 8},
    {0x1FE0, 2, 1, 8},
    {0x1FE5, 1, 1, 7},
    {0x214E, 1, 1, -28},
    {0x2170, 16, 1, -16},
    {0x2184, 1, 1, -1},
    {0x24D0, 26, 1, -26},
    {0x2C30, 48, 1, -48},
    {0x2C61, 1, 1, -1},
    {0x2C65, 1, 1, -10795},
    {0x2C66, 1, 1, -10792},
    {0x2C68, 3, 2, -1},
    {0x2C73, 1, 1, -1},
    {0x2C76, 1, 1, -1},
    {0x2C81, 50, 2, -1},
    {0x2CEC, 2, 2, -1},
    {0x2CF3, 1, 1, -1},
    {0x2D00, 38, 1, -7264},
    {0x2D27, 1, 1, -7264},
    {0x2D2D, 1, 1, -7264},
    {0xA641, 23, 2, -1},
    {0xA681, 14, 2, -1},
    {0xA723, 7, 2, -1},
    {0xA733, 31, 2, -1},
    {0xA77A, 2, 2, -1},
    {0xA77F, 5, 2, -1},
    {0xA78C, 1, 1, -1},
    {0xA791, 2, 2, -1},
    {0xA794, 1, 1, 48},
    {0xA797, 10, 2, -1},
    {0xA7B5, 8, 2, -1},
    {0xA7C8, 2, 2, -1},
    {0xA7D1, 1, 1, -1},
    {0xA7D7, 2, 2, -1},
    {0xA7F6, 1, 1, -1},
    {0xAB53, 1, 1, -928},
    {0xAB70, 80, 1, -38864},
    {0xFF41, 26, 1, -32},
    {0x10428, 40, 1, -40},
    {0x104D8, 36, 1, -40},
    {0x10597, 11, 1, -39},
    {0x105A3, 15, 1, -39},
    {0x105B3, 7, 1, -39},
    {0x105BB, 2, 1, -39},
    {0x10CC0, 51, 1, -64},
    {0x118C0, 32, 1, -32},
    {0x16E60, 32, 1, -32},
    {0x1E922, 34, 1, -34},
};
#define BCASE_UP_N ((int)(sizeof BCASE_UP / sizeof BCASE_UP[0]))

static const CaseRun BCASE_LOW[] = {
    {0x41, 26, 1, 32},
    {0xC0, 23, 1, 32},
    {0xD8, 7, 1, 32},
    {0x100, 24, 2, 1},
    {0x132, 3, 2, 1},
    {0x139, 8, 2, 1},
    {0x14A, 23, 2, 1},
    {0x178, 1, 1, -121},
    {0x179, 3, 2, 1},
    {0x181, 1, 1, 210},
    {0x182, 2, 2, 1},
    {0x186, 1, 1, 206},
    {0x187, 1, 1, 1},
    {0x189, 2, 1, 205},
    {0x18B, 1, 1, 1},
    {0x18E, 1, 1, 79},
    {0x18F, 1, 1, 202},
    {0x190, 1, 1, 203},
    {0x191, 1, 1, 1},
    {0x193, 1, 1, 205},
    {0x194, 1, 1, 207},
    {0x196, 1, 1, 211},
    {0x197, 1, 1, 209},
    {0x198, 1, 1, 1},
    {0x19C, 1, 1, 211},
    {0x19D, 1, 1, 213},
    {0x19F, 1, 1, 214},
    {0x1A0, 3, 2, 1},
    {0x1A6, 1, 1, 218},
    {0x1A7, 1, 1, 1},
    {0x1A9, 1, 1, 218},
    {0x1AC, 1, 1, 1},
    {0x1AE, 1, 1, 218},
    {0x1AF, 1, 1, 1},
    {0x1B1, 2, 1, 217},
    {0x1B3, 2, 2, 1},
    {0x1B7, 1, 1, 219},
    {0x1B8, 1, 1, 1},
    {0x1BC, 1, 1, 1},
    {0x1C4, 1, 1, 2},
    {0x1C5, 1, 1, 1},
    {0x1C7, 1, 1, 2},
    {0x1C8, 1, 1, 1},
    {0x1CA, 1, 1, 2},
    {0x1CB, 9, 2, 1},
    {0x1DE, 9, 2, 1},
    {0x1F1, 1, 1, 2},
    {0x1F2, 2, 2, 1},
    {0x1F6, 1, 1, -97},
    {0x1F7, 1, 1, -56},
    {0x1F8, 20, 2, 1},
    {0x220, 1, 1, -130},
    {0x222, 9, 2, 1},
    {0x23A, 1, 1, 10795},
    {0x23B, 1, 1, 1},
    {0x23D, 1, 1, -163},
    {0x23E, 1, 1, 10792},
    {0x241, 1, 1, 1},
    {0x243, 1, 1, -195},
    {0x244, 1, 1, 69},
    {0x245, 1, 1, 71},
    {0x246, 5, 2, 1},
    {0x370, 2, 2, 1},
    {0x376, 1, 1, 1},
    {0x37F, 1, 1, 116},
    {0x386, 1, 1, 38},
    {0x388, 3, 1, 37},
    {0x38C, 1, 1, 64},
    {0x38E, 2, 1, 63},
    {0x391, 17, 1, 32},
    {0x3A3, 9, 1, 32},
    {0x3CF, 1, 1, 8},
    {0x3D8, 12, 2, 1},
    {0x3F4, 1, 1, -60},
    {0x3F7, 1, 1, 1},
    {0x3F9, 1, 1, -7},
    {0x3FA, 1, 1, 1},
    {0x3FD, 3, 1, -130},
    {0x400, 16, 1, 80},
    {0x410, 32, 1, 32},
    {0x460, 17, 2, 1},
    {0x48A, 27, 2, 1},
    {0x4C0, 1, 1, 15},
    {0x4C1, 7, 2, 1},
    {0x4D0, 48, 2, 1},
    {0x531, 38, 1, 48},
    {0x10A0, 38, 1, 7264},
    {0x10C7, 1, 1, 7264},
    {0x10CD, 1, 1, 7264},
    {0x13A0, 80, 1, 38864},
    {0x13F0, 6, 1, 8},
    {0x1C90, 43, 1, -3008},
    {0x1CBD, 3, 1, -3008},
    {0x1E00, 75, 2, 1},
    {0x1E9E, 1, 1, -7615},
    {0x1EA0, 48, 2, 1},
    {0x1F08, 8, 1, -8},
    {0x1F18, 6, 1, -8},
    {0x1F28, 8, 1, -8},
    {0x1F38, 8, 1, -8},
    {0x1F48, 6, 1, -8},
    {0x1F59, 4, 2, -8},
    {0x1F68, 8, 1, -8},
    {0x1F88, 8, 1, -8},
    {0x1F98, 8, 1, -8},
    {0x1FA8, 8, 1, -8},
    {0x1FB8, 2, 1, -8},
    {0x1FBA, 2, 1, -74},
    {0x1FBC, 1, 1, -9},
    {0x1FC8, 4, 1, -86},
    {0x1FCC, 1, 1, -9},
    {0x1FD8, 2, 1, -8},
    {0x1FDA, 2, 1, -100},
    {0x1FE8, 2, 1, -8},
    {0x1FEA, 2, 1, -112},
    {0x1FEC, 1, 1, -7},
    {0x1FF8, 2, 1, -128},
    {0x1FFA, 2, 1, -126},
    {0x1FFC, 1, 1, -9},
    {0x2126, 1, 1, -7517},
    {0x212A, 1, 1, -8383},
    {0x212B, 1, 1, -8262},
    {0x2132, 1, 1, 28},
    {0x2160, 16, 1, 16},
    {0x2183, 1, 1, 1},
    {0x24B6, 26, 1, 26},
    {0x2C00, 48, 1, 48},
    {0x2C60, 1, 1, 1},
    {0x2C62, 1, 1, -10743},
    {0x2C63, 1, 1, -3814},
    {0x2C64, 1, 1, -10727},
    {0x2C67, 3, 2, 1},
    {0x2C6D, 1, 1, -10780},
    {0x2C6E, 1, 1, -10749},
    {0x2C6F, 1, 1, -10783},
    {0x2C70, 1, 1, -10782},
    {0x2C72, 1, 1, 1},
    {0x2C75, 1, 1, 1},
    {0x2C7E, 2, 1, -10815},
    {0x2C80, 50, 2, 1},
    {0x2CEB, 2, 2, 1},
    {0x2CF2, 1, 1, 1},
    {0xA640, 23, 2, 1},
    {0xA680, 14, 2, 1},
    {0xA722, 7, 2, 1},
    {0xA732, 31, 2, 1},
    {0xA779, 2, 2, 1},
    {0xA77D, 1, 1, -35332},
    {0xA77E, 5, 2, 1},
    {0xA78B, 1, 1, 1},
    {0xA78D, 1, 1, -42280},
    {0xA790, 2, 2, 1},
    {0xA796, 10, 2, 1},
    {0xA7AA, 1, 1, -42308},
    {0xA7AB, 1, 1, -42319},
    {0xA7AC, 1, 1, -42315},
    {0xA7AD, 1, 1, -42305},
    {0xA7AE, 1, 1, -42308},
    {0xA7B0, 1, 1, -42258},
    {0xA7B1, 1, 1, -42282},
    {0xA7B2, 1, 1, -42261},
    {0xA7B3, 1, 1, 928},
    {0xA7B4, 8, 2, 1},
    {0xA7C4, 1, 1, -48},
    {0xA7C5, 1, 1, -42307},
    {0xA7C6, 1, 1, -35384},
    {0xA7C7, 2, 2, 1},
    {0xA7D0, 1, 1, 1},
    {0xA7D6, 2, 2, 1},
    {0xA7F5, 1, 1, 1},
    {0xFF21, 26, 1, 32},
    {0x10400, 40, 1, 40},
    {0x104B0, 36, 1, 40},
    {0x10570, 11, 1, 39},
    {0x1057C, 15, 1, 39},
    {0x1058C, 7, 1, 39},
    {0x10594, 2, 1, 39},
    {0x10C80, 51, 1, 64},
    {0x118A0, 32, 1, 32},
    {0x16E40, 32, 1, 32},
    {0x1E900, 34, 1, 34},
};
#define BCASE_LOW_N ((int)(sizeof BCASE_LOW / sizeof BCASE_LOW[0]))

#if BRAT_CASE_MAP_VERSION >= 1600
static const CaseRun BCASE_UP_16[] = {
    {0x19B, 1, 1, 42561},
    {0x264, 1, 1, 42343},
    {0x1C8A, 1, 1, -1},
    {0xA7CD, 1, 1, -1},
    {0xA7DB, 1, 1, -1},
    {0x10D70, 22, 1, -32},
};
#define BCASE_UP_16_N ((int)(sizeof BCASE_UP_16 / sizeof BCASE_UP_16[0]))

static const CaseRun BCASE_LOW_16[] = {
    {0x1C89, 1, 1, 1},
    {0xA7CB, 1, 1, -42343},
    {0xA7CC, 1, 1, 1},
    {0xA7DA, 1, 1, 1},
    {0xA7DC, 1, 1, -42561},
    {0x10D50, 22, 1, 32},
};
#define BCASE_LOW_16_N ((int)(sizeof BCASE_LOW_16 / sizeof BCASE_LOW_16[0]))
#endif

static unsigned case_map(const CaseRun *t, int n, unsigned c) {
    for (int i = 0; i < n; i++) {
        const CaseRun *r = &t[i];
        if (c < r->start) continue;
        unsigned off = c - r->start;
        if (off % r->stride) continue;
        if (off / r->stride >= r->count) continue;
        return (unsigned)((int)c + r->delta);
    }
    return 0;
}

BFN unsigned vcp_upper(unsigned c) {
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : 0;
    unsigned mapped = case_map(BCASE_UP, BCASE_UP_N, c);
#if BRAT_CASE_MAP_VERSION >= 1600
    if (!mapped) mapped = case_map(BCASE_UP_16, BCASE_UP_16_N, c);
#endif
    return mapped;
}

BFN unsigned vcp_lower(unsigned c) {
    if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : 0;
    unsigned mapped = case_map(BCASE_LOW, BCASE_LOW_N, c);
#if BRAT_CASE_MAP_VERSION >= 1600
    if (!mapped) mapped = case_map(BCASE_LOW_16, BCASE_LOW_16_N, c);
#endif
    return mapped;
}

BFN unsigned vcp_lower_ctx(unsigned c, unsigned prev, const char *rest) {
    unsigned out = vcp_lower(c);
    if (!out) return 0;
    if (out == 0x3C3) {
        
        int prev_cased = (prev >= 'A' && prev <= 'Z') || (prev >= 0x391 && prev <= 0x3A9)
                      || (prev >= 0x410 && prev <= 0x44F);
        unsigned next = 0;
        if (*rest) vcp_next(rest, &next);
        int next_cased = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')
                      || (next >= 0x3B1 && next <= 0x3C9) || (next >= 0x430 && next <= 0x44F);
        if (prev_cased && !next_cased) return 0x3C2;
    }
    return out;
}

BFN Val vcase(Val s, int up) {
    if (s.tag != V_STR) { vraise("no method case"); return vunwind; }
    size_t len = strlen(s.as.str) + 8;
    /* balloc, not malloc: the result is a BRAT string, and a BRAT string is
     * dropped by its holder. A raw buffer has no count to decrement. */
    char *o = (char *)balloc(len);
    if (!o) vfatal_oom();
    bint on = 0;
    unsigned prev = 0;
    for (const char *p = s.as.str; *p; ) {
        unsigned cp = 0;
        bint n = vcp_next(p, &cp);
        const char *rest = p + n;
        if (up && cp == 0xDF) {
            
            if (on + 4 >= (bint)len) { len = len * 2 + 8; o = (char *)bgrow_rc(o, len); }
            o[on++] = 'S'; o[on++] = 'S';
            p += n;
            prev = cp;
            continue;
        }
        if (!up && cp == 0x130) {
            /* U+0130 lowercases to two code points: i + combining dot above (U+0307). */
            if (on + 4 >= (bint)len) { len = len * 2 + 8; o = (char *)bgrow_rc(o, len); }
            o[on++] = 'i'; o[on++] = (char)0xCC; o[on++] = (char)0x87;
            p += n;
            prev = cp;
            continue;
        }
        unsigned m = up ? vcp_upper(cp) : vcp_lower_ctx(cp, prev, rest);
        if (!m) { m = cp; }
        if (m == cp) {
            if (on + 4 >= (bint)len) { len = len * 2 + 8; o = (char *)bgrow_rc(o, len); }
            memcpy(o + on, p, (size_t)n);
            on += n;
        } else {
            if (on + 5 >= (bint)len) { len = len * 2 + 8; o = (char *)bgrow_rc(o, len); }
            on += vcp_put(m, o + on);
        }
        prev = cp;
        p += n;
    }
    o[on] = 0;
    ((BHead *)o - 1)->len = on;
    Val r; r.tag = V_STR; r.as.str = o;
    return r;
}
BFN bint vaffix(Val s, Val p, int pre, int suf) {
    if (s.tag != V_STR || p.tag != V_STR) { vraise("no method affix"); return 0; }
    if (pre) return !strncmp(s.as.str, p.as.str, strlen(p.as.str));
    bint ls = strlen(s.as.str), lp = strlen(p.as.str);
    if (suf) return lp <= ls && !strcmp(s.as.str + ls - lp, p.as.str);
    return strstr(s.as.str, p.as.str) != 0;
}
BFN Val vreplace(Val s, Val a, Val b) {
    if (s.tag != V_STR || a.tag != V_STR || b.tag != V_STR) { vraise("no method replace"); return vunwind; }
    const char *p = s.as.str, *pat = a.as.str, *rep = b.as.str;
    size_t pl = strlen(pat), rl = strlen(rep);
    if (!pl) {
        /* Output is (sl+1)*rl + sl chars. Check the size before malloc:
           the old (sl+1)*(rl+1)+1 could overflow and under-allocate. */
        size_t sl = strlen(p);
        size_t cap = (size_t)1 << 30;   /* BRAT_MAX_STR */
        if (sl >= cap - 1 || rl > (cap - 1 - sl) / (sl + 1)) { vraise("out of memory"); return vunwind; }
        size_t olen = (sl + 1) * rl + sl;
        char *o = (char *)balloc(olen + 1);
        if (!o) vfatal_oom();
        char *w = o;
        strcpy(w, rep); w += rl;
        for (size_t i = 0; i < sl; i++) { *w++ = p[i]; memcpy(w, rep, rl); w += rl; }
        *w = 0;
        ((BHead *)o - 1)->len = (bint)olen;
        Val r; r.tag = V_STR; r.as.str = o;
        return r;
    }
    size_t total = strlen(p) + 1;
    char *o = (char *)balloc(total);
    if (!o) vfatal_oom();
    size_t n = 0;
    const char *q;
    while ((q = strstr(p, pat)) != 0) {
        size_t chunk = (size_t)(q - p);
        size_t need = n + chunk + rl + 1;
        if (need > total) { total = need * 2; o = (char *)bgrow_rc(o, total); }
        memcpy(o + n, p, chunk); n += chunk;
        memcpy(o + n, rep, rl); n += rl;
        p = q + pl;
    }
    size_t rest = strlen(p) + 1;
    if (n + rest > total) { total = n + rest; o = (char *)bgrow_rc(o, total); }
    memcpy(o + n, p, rest); n += rest - 1;
    ((BHead *)o - 1)->len = (bint)n;
    Val r; r.tag = V_STR; r.as.str = o;
    return r;
}
BFN Val vindex_of(Val s, Val sub) {
    if (s.tag != V_STR || sub.tag != V_STR) { vraise("no method index_of"); return vunwind; }
    char *p = strstr(s.as.str, sub.as.str);
    if (!p) return vnil();
    
    bint cps = 0;
    for (const char *q = s.as.str; q < p; ) {
        unsigned cp = 0;
        q += vcp_next(q, &cp);
        cps++;
    }
    return vint(cps);
}
/* Match brat/runtime.py MAX_STR: refuse absurd repeats before malloc. */
#ifndef BRAT_MAX_STR
#define BRAT_MAX_STR ((size_t)1 << 30)
#endif
BFN Val vrepeat_n(Val s, bint k) {
    if (s.tag != V_STR) { vraise("no method repeat"); return vunwind; }
    size_t sl = strlen(s.as.str);
    if (k <= 0 || !sl) return vstr("");
    if ((size_t)k > (size_t)-1 / sl) { vraise("out of memory"); return vunwind; }
    size_t total = sl * (size_t)k;
    if (total > BRAT_MAX_STR) { vraise("out of memory"); return vunwind; }
    char *o = (char *)balloc(total + 1);
    if (!o) { vraise("out of memory"); return vunwind; }
    for (bint i = 0; i < k; i++) memcpy(o + (size_t)i * sl, s.as.str, sl);
    o[total] = 0;
    ((BHead *)o - 1)->len = (bint)total;
    Val r; r.tag = V_STR; r.as.str = o;
    return r;
}
BFN Val vrepeat(Val s, Val n) { return vrepeat_n(s, vlong(n)); }

BFN Val vlines(Val s) {
    if (s.tag != V_STR) { vraise("no method lines"); return vunwind; }
    Vec *o = vvec();
    if (!s.as.str[0]) return vvecv(o);
    const char *p = s.as.str;
    const char *start = p;
    for (;;) {
        if (*p == '\n' || *p == '\r') {
            vec_push(o, vstrn(start, (bint)(p - start)));
            if (*p == '\r' && p[1] == '\n') p++;
            p++;
            start = p;
            if (!*start) break;
            continue;
        }
        if (*p == 0) {
            vec_push(o, vstrn(start, (bint)(p - start)));
            break;
        }
        p++;
    }
    return vvecv(o);
}

BFN Val vsplit_once(Val s, Val sep) {
    if (s.tag != V_STR || sep.tag != V_STR) { vraise("no method split_once"); return vunwind; }
    if (!sep.as.str[0]) {
        Vec *o = vvec();
        vec_push(o, vstr(""));
        vec_push(o, vstr(s.as.str));
        return vvecv(o);
    }
    char *p = strstr(s.as.str, sep.as.str);
    if (!p) return vnil();
    Vec *o = vvec();
    vec_push(o, vstrn(s.as.str, (bint)(p - s.as.str)));
    vec_push(o, vstr(p + strlen(sep.as.str)));
    return vvecv(o);
}
BFN Val vparse_int(Val s) {
    if (s.tag != V_STR) { vraise("no method parse_int"); return vunwind; }
    const char *p = s.as.str;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    if (!(*q >= '0' && *q <= '9')) { Vec *a = vvec(); vec_push(a, vstr("not a number")); return vtagv("err", a); }
    uint64_t acc = 0;
    while (*q >= '0' && *q <= '9') { acc = acc * 10ULL + (uint64_t)(*q - '0'); q++; }
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' || *q == '\f' || *q == '\v') q++;
    Vec *a = vvec();
    if (*q) { vec_push(a, vstr("not a number")); return vtagv("err", a); }
    
    bint v = (bint)acc;
    if (*p == '-') v = -v;
    vec_push(a, vint(v));
    return vtagv("ok", a);
}
BFN Val vjoin(Val v, Val sep) {
    if (v.tag != V_VEC || sep.tag != V_STR) { vraise("no method join"); return vunwind; }
    Vec *w = v.as.vec;
    size_t total = 1;
    bint i;
    for (i = 0; i < w->len; i++) total += (size_t)vsprint_len_q(w->items[i], 0);
    if (w->len > 1) total += strlen(sep.as.str) * (size_t)(w->len - 1);
    char *out = (char *)balloc(total + 1);
    if (!out) vfatal_oom();
    bint on = 0;
    for (i = 0; i < w->len; i++) {
        if (i) { size_t sl = strlen(sep.as.str); memcpy(out + on, sep.as.str, sl); on += (bint)sl; }
        on += vsprint_q(w->items[i], out + on, 0);
    }
    out[on] = 0;
    ((BHead *)out - 1)->len = on;
    Val r; r.tag = V_STR; r.as.str = out;
    return r;
}
/* `tagof` is asked for inside loops and its answer never changes, so the name is
 * cached as an immortal string instead of a fresh allocation per call. */
static struct { const char *name; Val v; } _vtagof_cache[512];
static int _vtagof_n = 0;
static pthread_mutex_t _vtagof_mu = PTHREAD_MUTEX_INITIALIZER;
static int _vtagof_count(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&_vtagof_n, __ATOMIC_ACQUIRE);
#else
    return _vtagof_n;
#endif
}
BFN Val vtagof(Val v) {
    /* `tagof` runs inside every worker thread, so the cache is a shared table:
     * a slot is written whole and published last, and a miss takes the lock. */
    const char *name = v.tag == V_NIL ? "none" : (v.tag == V_TAG ? v.as.tv.tag : "plain");
    int seen = _vtagof_count();
    for (int i = 0; i < seen; i++) {
        if (strcmp(_vtagof_cache[i].name, name) == 0) return _vtagof_cache[i].v;
    }
    Val s = vstr(name);
    ((BHead *)s.as.str - 1)->rc = -1;    /* immortal: cached forever */
    pthread_mutex_lock(&_vtagof_mu);
    if (_vtagof_n < 512) {
        for (int i = 0; i < _vtagof_n; i++) {
            if (strcmp(_vtagof_cache[i].name, name) == 0) {
                Val hit = _vtagof_cache[i].v;
                pthread_mutex_unlock(&_vtagof_mu);
                return hit;
            }
        }
        _vtagof_cache[_vtagof_n].name = name;
        _vtagof_cache[_vtagof_n].v = s;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&_vtagof_n, _vtagof_n + 1, __ATOMIC_RELEASE);
#else
        _vtagof_n++;
#endif
    }
    pthread_mutex_unlock(&_vtagof_mu);
    return s;
}
BFN bint vlong(Val v) {
    if (v.tag == V_INT) return v.as.ival;
    if (v.tag == V_FLOAT) {
        if (!(v.as.fval >= -9223372036854775808.0 &&
              v.as.fval < 9223372036854775808.0)) {
            vraise("integer out of range");
            return 0;
        }
        return (bint)v.as.fval;
    }
    vraise("not a number"); return 0;
}
/* A vec operation that got the wrong receiver raises and returns an empty vec:
   the generated code keeps running until the next error check and would
   dereference NULL there. `meth` names the method for the message, so the
   interpreter can say exactly the same thing. */
static Vec vpoison_vec_s;
BFN Vec *vvec_of_m(Val v, const char *meth) {
    if (v.tag != V_VEC) {
        vraisef("no method %s", meth);
        vpoison_vec_s.len = 0;
        return &vpoison_vec_s;
    }
    return v.as.vec;
}
BFN Vec *vvec_of(Val v) {
    if (v.tag != V_VEC) { vraise("not a vec"); return &vpoison_vec_s; }
    return v.as.vec;
}
BFN int vis_err(Val v) {
    return v.tag == V_TAG && !strcmp(v.as.tv.tag, "err");
}
BFN int vis_none(Val v) {
    return v.tag == V_NIL;
}

/* A closure is a runtime object, not a value a program can write down: the tag
   is "closure" and the payload is (code, captured frame). Printing that payload
   was an internal view, and the interpreter printed a *different* internal view
   (its AST), so `print(f)` was the last known spot where two backends disagreed
   about the bytes of a value. SPEC says printing a value is part of the
   contract, so both say `fn`. */
BFN bint vtag_closure(Val v) {
    return v.tag == V_TAG && v.as.tv.tag && !strcmp(v.as.tv.tag, "closure");
}
BFN const char *vtypename(Val v) {
    switch (v.tag) {
    case V_MOVED: return "moved";
    case V_NIL: return "none";
    case V_BOOL: return "bool";
    case V_INT: return "i64";
    case V_FLOAT: return "f64";
    case V_STR: return "str";
    case V_VEC: return "vec";
    case V_STRUCT: return v.as.map->type && v.as.map->type[0] ? v.as.map->type : "map";
    case V_TAG: return v.as.tv.tag ? v.as.tv.tag : "tag";
    }
    return "value";
}
/* The payload of a tag, as a reference the caller owns.
 *
 * `x.unwrap()` reads one item out of a tag and drops the tag. Whether the tag
 * itself may go is not a question the runtime can answer: a binding and a
 * temporary look the same here, and freeing a tag a binding still names is a
 * use-after-free. So this only takes the reference; the emitter releases the
 * tag when it knows the tag was built for this expression.
 */
BFN Val vtag_payload(Val v) {
    Vec *args = v.as.tv.args;
    if (!args || !args->len) return vnil();
    return vrc_inc(args->items[0]);
}

BFN Val vunwrap(Val v) {
    if (v.tag == V_TAG && (!strcmp(v.as.tv.tag, "ok") || !strcmp(v.as.tv.tag, "some")))
        return vtag_payload(v);
    if (v.tag == V_TAG && !strcmp(v.as.tv.tag, "err")) {
        vunwind = v;
        vunwind_do();
    }

    vraisef("unwrap of %s (want Ok/Err/Some)", vtypename(v)); return vunwind;
    return vnil();
}
BFN int vor_fb(Val v) {
    return v.tag == V_NIL;
}
BFN const char *vname(Val v) {
    if (v.tag != V_STR) { vraise("want str"); return NULL; }
    return v.as.str;
}
static bint vsprint_q_(Val v, char *o, bint q) {
    bint n = 0, i;
    char tmp[64];
    switch (v.tag) {
    case V_MOVED: vraise("use of moved value"); return 0;
    case V_NIL: memcpy(o, "None", 4); return 4;
    case V_BOOL: memcpy(o, v.as.b ? "True" : "False", v.as.b ? 4 : 5); return v.as.b ? 4 : 5;
    case V_INT: n = snprintf(tmp, sizeof tmp, BINT_FMT, (bint)v.as.ival); memcpy(o, tmp, n); return n;
    case V_FLOAT: { const char *s = vfmt_double(v.as.fval); n = (bint)strlen(s); memcpy(o, s, n); return n; }
    case V_STR: {
        if (!q) { n = strlen(v.as.str); memcpy(o, v.as.str, n); return n; }
        char quote = vquote(v.as.str);
        o[n++] = quote;
        for (const char *p = v.as.str; *p; p++) {
            if (*p == quote || *p == '\\') { o[n++] = '\\'; o[n++] = *p; }
            else if (*p == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
            else if (*p == '\r') { o[n++] = '\\'; o[n++] = 'r'; }
            else if (*p == '\t') { o[n++] = '\\'; o[n++] = 't'; }
            else o[n++] = *p;
        }
        o[n++] = quote;
        return n;
    }
    case V_VEC:
        o[n++] = '[';
        for (i = 0; i < v.as.vec->len; i++) {
            if (i) { o[n++] = ','; o[n++] = ' '; }
            n += vsprint_q(v.as.vec->items[i], o + n, 1);
        }
        o[n++] = ']';
        return n;
    case V_STRUCT: {
        int ismap = !v.as.map->type[0];
        o[n++] = '{';
        if (!ismap) {
            n += vput(o + n, "'$type': '");
            n += vput(o + n, v.as.map->type);
            o[n++] = '\'';
        }
        for (i = 0; i < v.as.map->len; i++) {
            if (i || !ismap) { o[n++] = ','; o[n++] = ' '; }
            n += vsprint_q(v.as.map->keys[i], o + n, 1);
            o[n++] = ':'; o[n++] = ' ';
            n += vsprint_q(v.as.map->vals[i], o + n, 1);
        }
        o[n++] = '}';
        return n;
    }
    case V_TAG: {
        if (vtag_closure(v)) { o[0] = 'f'; o[1] = 'n'; return 2; }
        o[0] = '('; o[1] = '\''; n = 2;
        n += vput(o + n, v.as.tv.tag);
        o[n++] = '\'';
        for (i = 0; i < v.as.tv.args->len; i++) {
            o[n++] = ','; o[n++] = ' ';
            n += vsprint_q(v.as.tv.args->items[i], o + n, 1);
        }
        if (v.as.tv.args->len == 0) o[n++] = ',';
        o[n++] = ')';
        return n;
    }
    }
    return 0;
}

BFN bint vsprint(Val v, char *o) { return vsprint_q(v, o, 0); }

static bint vsprint_len_q_(Val v, bint q) {
    bint n = 0, i;
    char tmp[64];
    switch (v.tag) {
    case V_MOVED: vraise("use of moved value"); return 0;
    case V_NIL: return 4;
    case V_BOOL: return v.as.b ? 4 : 5;
    case V_INT: return snprintf(tmp, sizeof tmp, BINT_FMT, (bint)v.as.ival);
    case V_FLOAT: return (bint)strlen(vfmt_double(v.as.fval));
    case V_STR: return q ? vrepr_slen(v.as.str) + 2 : (bint)strlen(v.as.str);
    case V_VEC:
        n = 2;
        for (i = 0; i < v.as.vec->len; i++) {
            if (i) n += 2;
            n += vsprint_len_q(v.as.vec->items[i], 1);
        }
        return n;
    case V_STRUCT: {
        int ismap = !v.as.map->type[0];
        n = 1;
        if (!ismap) n += 11 + (bint)strlen(v.as.map->type);
        for (i = 0; i < v.as.map->len; i++) {
            if (i || !ismap) n += 2;
            n += vsprint_len_q(v.as.map->keys[i], 1) + 2 + vsprint_len_q(v.as.map->vals[i], 1);
        }
        return n + 1;
    }
    case V_TAG: {
        if (vtag_closure(v)) return 2;
        n = 3 + (bint)strlen(v.as.tv.tag);
        for (i = 0; i < v.as.tv.args->len; i++) n += 2 + vsprint_len_q(v.as.tv.args->items[i], 1);
        if (v.as.tv.args->len == 0) n += 1;
        return n + 1;
    }
    }
    return 0;
}

/* The wrappers carry the depth budget; the bodies below stay recursive and call
   back into them, so every level is counted. */
BFN bint vsprint_q(Val v, char *o, bint q) {
    if (bval_depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return 0; }
    bval_depth++;
    bint n = vsprint_q_(v, o, q);
    bval_depth--;
    return n;
}

BFN bint vsprint(Val v, char *o);

BFN bint vsprint_len_q(Val v, bint q) {
    if (bval_depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return 0; }
    bval_depth++;
    bint n = vsprint_len_q_(v, q);
    bval_depth--;
    return n;
}

BFN bint vsprint_len(Val v) { return vsprint_len_q(v, 0); }
BFN void vlogv(const char *lvl, Vec *a) {
    bint total = 0, i;
    for (i = 0; i < a->len; i++) {
        if (i) total += 1;
        total += vsprint_len(a->items[i]);
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) vfatal_oom();
    bint n = 0;
    for (i = 0; i < a->len; i++) {
        if (i) buf[n++] = ' ';
        n += vsprint(a->items[i], buf + n);
    }
    buf[n] = 0;
    
    printf("{\"ts\": %.7f, \"lvl\": \"%s\", \"msg\": \"", vtime(), lvl);
    for (char *p = buf; *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p == '\n') printf("\\n");
        else if (*p == '\r') printf("\\r");
        else if (*p == '\t') printf("\\t");
        else if ((unsigned char)*p < 0x20) printf("\\u%04x", (unsigned char)*p);
        else putchar((unsigned char)*p);
    }
    printf("\"}\n");
    fflush(stdout);
    free(buf);
}
BFN Val vpush(Val v, Val x) {
    Vec *w = vvec_of_m(v, "push");
    vec_push(w, x);
    return vint(w->len);
}
BFN Val vpop(Val v) {
    Vec *w = vvec_of_m(v, "pop");
    if (!w->len) return vnil();
    return w->items[--w->len];
}
BFN Val vfirst(Val v) {
    Vec *w = vvec_of_m(v, "first");
    if (!w->len) return vnil();
    return w->items[0];
}
BFN Val vlast(Val v) {
    Vec *w = vvec_of_m(v, "last");
    if (!w->len) return vnil();
    return w->items[w->len - 1];
}
static int vcmp_q(const void *pa, const void *pb) {
    return vcmp(*(const Val *)pa, *(const Val *)pb);
}
BFN Val vsort(Val v) {
    Vec *w = vvec_of_m(v, "sort");
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (w->len > 1) qsort(w->items, (size_t)w->len, sizeof(Val), vcmp_q);
    return v;
}
BFN Val vrev(Val v) {
    Vec *w = vvec_of_m(v, "rev");
    Vec *o = vvec();
    for (bint i = w->len - 1; i >= 0; i--) vec_push(o, vrc_inc(w->items[i]));
    return vvecv(o);
}
BFN Val vins(Val v, Val i, Val x) {
    Vec *w = vvec_of_m(v, "ins");
    bint k = vlong(i);
    
    if (k < 0) k = (k < -w->len) ? 0 : k + w->len;
    if (k > w->len) k = w->len;
    vec_push(w, x);
    memmove(w->items + k + 1, w->items + k, sizeof(Val) * (size_t)(w->len - 1 - k));
    w->items[k] = x;
    return vint(w->len);
}
BFN Val vdel_at(Val v, Val i) {
    Vec *w = vvec_of_m(v, "del");
    bint k = vlong(i);
    
    if (k < 0) {
        if (k < -w->len) { vraise("index out of range"); return vunwind; }
        k += w->len;
    }
    if (k < 0 || k >= w->len) { vraise("index out of range"); return vunwind; }
    Val r = w->items[k];
    memmove(w->items + k, w->items + k + 1, sizeof(Val) * (size_t)(w->len - 1 - k));
    w->len--;
    return r;
}
BFN Val vslice(Val v, Val a, Val b) {
    Vec *w = vvec_of_m(v, "slice");
    bint n = w->len, x = vlong(a), y = vlong(b);
    if (x < 0) x = (x < -n) ? 0 : x + n;
    if (x < 0) x = 0;
    if (x > n) x = n;
    if (y < 0) y = (y < -n) ? 0 : y + n;
    if (y < 0) y = 0;
    if (y > n) y = n;
    Vec *o = vvec();
    for (bint i = x; i < y; i++) vec_push(o, vrc_inc(w->items[i]));
    return vvecv(o);
}
BFN Val vsum(Val v) {
    Vec *w = vvec_of_m(v, "sum");
    
    uint64_t acc = 0;
    double dl = 0;
    int isf = 0;
    for (bint i = 0; i < w->len; i++) {
        Val x = w->items[i];
        if (x.tag == V_INT || x.tag == V_BOOL) acc += (uint64_t)x.as.ival;
        else if (x.tag == V_FLOAT) { dl += x.as.fval; isf = 1; }
        else { vraise("sum of non-numeric"); return vunwind; }
    }
    if (isf) return vfloat(dl + (double)(bint)acc);
    return vint((bint)acc);
}
BFN Val vmin(Val v) {
    Vec *w = vvec_of_m(v, "min");
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (!w->len) return vnil();
    Val r = w->items[0];
    for (bint i = 1; i < w->len; i++) if (vcmp(w->items[i], r) < 0) r = w->items[i];
    return r;
}
BFN Val vmax(Val v) {
    Vec *w = vvec_of_m(v, "max");
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (!w->len) return vnil();
    Val r = w->items[0];
    for (bint i = 1; i < w->len; i++) if (vcmp(w->items[i], r) > 0) r = w->items[i];
    return r;
}
BFN Val venum(Val v) {
    Vec *w = vvec_of_m(v, "enumerate");
    Vec *o = vvec();
    for (bint i = 0; i < w->len; i++) {
        Vec *p = vvec();
        vec_push(p, vint(i));
        vec_push(p, vrc_inc(w->items[i]));
        vec_push(o, vvecv(p));
    }
    return vvecv(o);
}
BFN Val vmap_get_or(Val m, Val k, Val d) {
    if (m.tag != V_STRUCT) { vraise("get_or of non-map"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    Val *o = map_get_v(m.as.map, k);
    return o ? *o : d;
}
BFN Val vmap_items(Val m) {
    if (m.tag != V_STRUCT) { vraise("items of non-map"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < m.as.map->len; i++) {
        Vec *p = vvec();
        vec_push(p, vrc_inc(m.as.map->keys[i]));
        vec_push(p, vrc_inc(m.as.map->vals[i]));
        vec_push(o, vvecv(p));
    }
    return vvecv(o);
}
BFN Val vmap_vals(Val m) {
    if (m.tag != V_STRUCT) { vraise("vals of non-map"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < m.as.map->len; i++) vec_push(o, vrc_inc(m.as.map->vals[i]));
    return vvecv(o);
}
BFN Val vcopy(Val v) {
    if (v.tag == V_VEC) {
        Vec *o = vvec();
        for (bint i = 0; i < v.as.vec->len; i++) vec_push(o, vrc_inc(v.as.vec->items[i]));
        return vvecv(o);
    }
    if (v.tag == V_STRUCT) {
        Map *m = vmap(v.as.map->type);
        for (bint i = 0; i < v.as.map->len; i++)
            map_set_v(m, vrc_inc(v.as.map->keys[i]), vrc_inc(v.as.map->vals[i]));
        return vstruct(m);
    }
    if (v.tag == V_STR) return v;
    vraise("copy of non-container"); return vunwind;
}
BFN Val vchars(Val s) {
    if (s.tag != V_STR) { vraise("no method chars"); return vunwind; }
    Vec *o = vvec();
    for (const char *p = s.as.str; *p; p += utf8_cplen(p)) vec_push(o, vstrn(p, utf8_cplen(p)));
    return vvecv(o);
}
BFN Val vstr_slice(Val s, Val a, Val b) {
    if (s.tag != V_STR) { vraise("no method slice"); return vunwind; }
    bint n = 0;
    for (const char *q = s.as.str; *q; q += utf8_cplen(q)) n++;
    bint x = vlong(a), y = vlong(b);
    if (x < 0) x = (x < -n) ? 0 : x + n;
    if (x < 0) x = 0;
    if (x > n) x = n;
    if (y < 0) y = (y < -n) ? 0 : y + n;
    if (y < 0) y = 0;
    if (y > n) y = n;
    if (y <= x) return vstr("");
    const char *p = utf8_at(s.as.str, x);
    const char *q = utf8_at(s.as.str, y);
    return vstrn(p, (bint)(q - p));
}
BFN Val vparse_float(Val s) {
    if (s.tag != V_STR) { vraise("no method parse_int"); return vunwind; }
    char *e;
    const char *p = s.as.str;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    double v = strtod(p, &e);
    while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r' || *e == '\f' || *e == '\v') e++;
    Vec *a = vvec();
    
    if (e == p || *e || (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) { vec_push(a, vstr("not a float")); return vtagv("err", a); }
    vec_push(a, vfloat(v));
    return vtagv("ok", a);
}
BFN Val vzip(Val a, Val b) {
    Vec *x = vvec_of_m(a, "zip");
    Vec *y = vvec_of_m(b, "zip");
    bint n = x->len < y->len ? x->len : y->len;
    Vec *o = vvec();
    for (bint i = 0; i < n; i++) {
        Vec *p = vvec();
        vec_push(p, vrc_inc(x->items[i]));
        vec_push(p, vrc_inc(y->items[i]));
        vec_push(o, vvecv(p));
    }
    return vvecv(o);
}
BFN Val vchunks(Val v, Val n) {
    Vec *w = vvec_of_m(v, "chunks");
    bint k = vlong(n);
    if (k <= 0) { vraise("chunks wants n > 0"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < w->len; i += k) {
        Vec *p = vvec();
        for (bint j = i; j < i + k && j < w->len; j++) vec_push(p, vrc_inc(w->items[j]));
        vec_push(o, vvecv(p));
    }
    return vvecv(o);
}

BFN void vjprint(Val v) {
    bint i;
    switch (v.tag) {
    case V_NIL: printf("null"); break;
    case V_BOOL: printf(v.as.b ? "true" : "false"); break;
    case V_INT: printf(BINT_FMT, (bint)v.as.ival); break;
    case V_FLOAT: printf("%s", vfmt_double(v.as.fval)); break;
    case V_STR:
        printf("\"");
        for (const char *p = v.as.str; *p; p++) {
            if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
            else if (*p == '\n') printf("\\n");
            else if (*p == '\r') printf("\\r");
            else if (*p == '\t') printf("\\t");
            else if ((unsigned char)*p < 0x20) printf("\\u%04x", (unsigned char)*p);
            else putchar((unsigned char)*p);
        }
        printf("\"");
        break;
    case V_VEC:
        printf("[");
        for (i = 0; i < v.as.vec->len; i++) { if (i) printf(", "); vjprint(v.as.vec->items[i]); }
        printf("]");
        break;
    case V_STRUCT: {
        int ismap = !v.as.map->type[0];
        printf("{");
        if (!ismap) printf("\"$type\": \"%s\"", v.as.map->type);
        for (i = 0; i < v.as.map->len; i++) {
            if (i || !ismap) printf(", ");
            vjprint(v.as.map->keys[i]);
            printf(": ");
            vjprint(v.as.map->vals[i]);
        }
        printf("}");
        break;
    }
    case V_TAG: {
        
        printf("[");
        printf("\"%s\"", v.as.tv.tag);
        for (i = 0; i < v.as.tv.args->len; i++) { printf(", "); vjprint(v.as.tv.args->items[i]); }
        printf("]");
        break;
    }
    default:
        vprint(v);
        break;
    }
}

BFN void vspan_items(Val v) {
    if (v.tag == V_VEC) {
        bint i;
        for (i = 0; i < v.as.vec->len; i++) { if (i) printf(", "); vjprint(v.as.vec->items[i]); }
    } else {
        vjprint(v);
    }
}

BFN void vspan(Val v) {
    printf("{\"ts\": %.7f, \"span\": [", vtime());
    vspan_items(v);
    printf("]}\n");
    fflush(stdout);
}

BFN void vjson_str(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"') fputs("\\\"", stdout);
        else if (*p == '\\') fputs("\\\\", stdout);
        else if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\r') fputs("\\r", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else if (*p == '\b') fputs("\\b", stdout);
        else if (*p == '\f') fputs("\\f", stdout);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

BFN void vfieldset(Val s, const char *k, Val v) {
    Val *o;
    if (s.tag != V_STRUCT || !(o = map_get(s.as.map, k))) { vraise("bad field set"); return; }
    Val old = *o;
    *o = v;
    vrelease1(old);
}
BFN Val vcall_any(Val fn, Vec *a);
BFN Val vfold(Val obj, Val init, Val fn) {
    Vec *w = vvec_of_m(obj, "fold");
    Val acc = init;
    Vec *args = vvec();
    vec_push(args, init);
    vec_push(args, init);
    for (bint i = 0; i < w->len; i++) {
        args->items[0] = acc;
        args->items[1] = w->items[i];
        acc = vcall_any(fn, args);
    }
    return acc;
}
BFN Val vpred(Val obj, Val fn, const char *meth) {
    Vec *w = vvec_of_m(obj, meth);
    Vec *one = vvec();
    vec_push(one, vnil());
    for (bint i = 0; i < w->len; i++) {
        one->items[0] = w->items[i];
        Val r = vcall_any(fn, one);
        if (meth[0] == 'a' && meth[1] == 'n' && vis_true(r)) return vbool(1);
        if (meth[0] == 'a' && meth[1] == 'l' && !vis_true(r)) return vbool(0);
        if (meth[0] == 'f' && vis_true(r)) return w->items[i];
    }
    if (meth[0] == 'f') return vnil();
    return vbool(meth[1] == 'l' ? 1 : 0);
}
BFN Val vpos(Val v, Val x) {
    Vec *w = vvec_of_m(v, "pos");
    for (bint i = 0; i < w->len; i++) if (veq(w->items[i], x)) return vint(i);
    return vnil();
}

#define BRT_VDFR_MAX 1024
BTLS Vec **vdfr;
BTLS bint vdfsp;
BTLS bint vdfcap;
BFN void vdfpush(void) {
    if (vdfsp >= vdfcap) {
        bint nc = vdfcap ? vdfcap * 2 : BRT_VDFR_MAX;
        Vec **np = realloc(vdfr, (size_t)nc * sizeof(Vec *));
        if (!np) { vraise("out of memory"); return; }
        vdfr = np;
        vdfcap = nc;
    }
    vdfr[vdfsp++] = NULL;
}
BFN void vdfadd(Val clo) {
    if (vdfsp <= 0) { vraise("defer outside a function"); return; }
    if (!vdfr[vdfsp - 1]) {
        /* A defer registered inside an arena block still runs at function exit,
         * after the arena is gone: its frame lives on the heap. */
        int saved = barena_depth;
        barena_depth = 0;
        vdfr[vdfsp - 1] = vvec();
        barena_depth = saved;
    }
    vec_push(vdfr[vdfsp - 1], clo);
}

BFN void vdfreset(bint base) {
    while (vdfsp > base) {
        Vec *f = vdfr[--vdfsp];
        if (!f) continue;
        for (bint i = f->len - 1; i >= 0; --i) {
            /* Run the defer even while an error is unwinding, then restore it. */
            int sp = _verr_pending, se = _verr_exc;
            int saved = barena_depth;
            barena_depth = 0;
            Vec *noargs = vvec();
            barena_depth = saved;
            _verr_pending = 0;
            Val r = vcall_any(f->items[i], noargs);
            (void)r;
            vrc_dec(vvecv(noargs));
            _verr_pending = sp;
            _verr_exc = se;
        }
        /* The frame and its closures belong to this call: a function with a
         * defer runs it thousands of times and never owns the vector again.
         * Releasing the vector releases the closures it holds. */
        vdrop(vvecv(f));
    }
}
static BTLS Val vsortkey_fn;
static BTLS Vec *vsortby_buf;
static int vsortby_q(const void *pa, const void *pb) {
    if (!vsortby_buf) vsortby_buf = vvec();
    Vec *one = vsortby_buf;
    one->len = 0;
    vec_push(one, *(Val *)pa);
    Val ka = vcall_any(vsortkey_fn, one);
    one->len = 0;
    vec_push(one, *(Val *)pb);
    Val kb = vcall_any(vsortkey_fn, one);
    int r = vcmp(ka, kb);
    return r;
}
BFN Val vsort_by(Val v, Val fn) {
    Vec *w = vvec_of_m(v, "sort_by");
    vsortkey_fn = fn;
    if (w->len > 1) qsort(w->items, (size_t)w->len, sizeof(Val), vsortby_q);
    return v;
}
#define VARG(a, i) ((a)->len > (i) ? (a)->items[(i)] : vnil())
/* Channels
 * A bounded queue where a value has exactly one holder at a time: send takes
 * it over, recv hands it back. That is what makes `send` a move rather than a
 * copy, and it is the reason the checker refuses to send a reference.
 *
 * Slots are process-wide like sockets. A closed slot is reused once it has
 * drained; a handle to a reused slot then reads as None instead of reaching
 * into somebody else's queue. */
#define _VCH_MAX 64
typedef struct {
    Val *buf;
    bint cap, len, head, used, closed;
#if defined(_WIN32)
    CRITICAL_SECTION lk;
    CONDITION_VARIABLE rcv, snd;
#else
    pthread_mutex_t lk;
    pthread_cond_t rcv, snd;
#endif
} VChan;
static VChan _vch[_VCH_MAX];

#if defined(_WIN32)
static CRITICAL_SECTION _vch_tab;
static int _vch_tab_ready;
static void _vch_tab_lock(void) {
    if (!_vch_tab_ready) { InitializeCriticalSection(&_vch_tab); _vch_tab_ready = 1; }
    EnterCriticalSection(&_vch_tab);
}
static void _vch_tab_unlock(void) { LeaveCriticalSection(&_vch_tab); }
static void _vch_init(VChan *c) {
    InitializeCriticalSection(&c->lk);
    InitializeConditionVariable(&c->rcv);
    InitializeConditionVariable(&c->snd);
}
static void _vch_lk(VChan *c) { EnterCriticalSection(&c->lk); }
static void _vch_ul(VChan *c) { LeaveCriticalSection(&c->lk); }
static void _vch_wait_rcv(VChan *c) { SleepConditionVariableCS(&c->rcv, &c->lk, INFINITE); }
static void _vch_wait_snd(VChan *c) { SleepConditionVariableCS(&c->snd, &c->lk, INFINITE); }
static void _vch_wake_rcv(VChan *c) { WakeConditionVariable(&c->rcv); }
static void _vch_wake_snd(VChan *c) { WakeConditionVariable(&c->snd); }
static void _vch_wake_all(VChan *c) {
    WakeAllConditionVariable(&c->rcv);
    WakeAllConditionVariable(&c->snd);
}
#else
static pthread_mutex_t _vch_tab = PTHREAD_MUTEX_INITIALIZER;
static void _vch_tab_lock(void) { pthread_mutex_lock(&_vch_tab); }
static void _vch_tab_unlock(void) { pthread_mutex_unlock(&_vch_tab); }
static void _vch_init(VChan *c) {
    pthread_mutex_init(&c->lk, 0);
    pthread_cond_init(&c->rcv, 0);
    pthread_cond_init(&c->snd, 0);
}
static void _vch_lk(VChan *c) { pthread_mutex_lock(&c->lk); }
static void _vch_ul(VChan *c) { pthread_mutex_unlock(&c->lk); }
static void _vch_wait_rcv(VChan *c) { pthread_cond_wait(&c->rcv, &c->lk); }
static void _vch_wait_snd(VChan *c) { pthread_cond_wait(&c->snd, &c->lk); }
static void _vch_wake_rcv(VChan *c) { pthread_cond_signal(&c->rcv); }
static void _vch_wake_snd(VChan *c) { pthread_cond_signal(&c->snd); }
static void _vch_wake_all(VChan *c) {
    pthread_cond_broadcast(&c->rcv);
    pthread_cond_broadcast(&c->snd);
}
#endif

static VChan *_vch_of(Val h) {
    if (h.tag != V_INT) return NULL;
    bint i = h.as.ival;
    if (i < 1 || i >= _VCH_MAX) return NULL;
    _vch_tab_lock();
    VChan *c = _vch[i].used ? &_vch[i] : NULL;
    _vch_tab_unlock();
    return c;
}

/* A closed slot goes back to the table once it holds nothing. */
static void _vch_release(VChan *c) {
    _vch_tab_lock();
    if (c->used && c->closed && c->len == 0) {
        c->used = 0;
        free(c->buf);
        c->buf = NULL;
    }
    _vch_tab_unlock();
}

/* The queue is preallocated here and grown lazily in the interpreter, so a
   capacity of 100 000 000 was a 2.4 GB calloc in C (vfatal_oom, SIGABRT) and a
   working channel in Python. One cap for both backends: past it the call is an
   error rather than an allocation. 1 048 576 slots is far past any queue this
   language needs, and the whole table is bounded by _VCH_MAX channels anyway. */
#define BRAT_MAX_CHAN_CAP 1048576

BFN Val vchan_new(Val cap) {
    if (cap.tag != V_INT) return _verr_c("chan: capacity must be an integer");
    bint n = cap.as.ival;
    if (n < 1) return _verr_c("chan: capacity must be at least 1");
    if (n > BRAT_MAX_CHAN_CAP) return _verr_c("chan: capacity too large");
    _vch_tab_lock();
    for (int i = 1; i < _VCH_MAX; i++) {
        VChan *c = &_vch[i];
        if (c->used) continue;
        c->buf = (Val *)calloc((size_t)n, sizeof(Val));
        if (!c->buf) { _vch_tab_unlock(); vfatal_oom(); }
        c->cap = n; c->len = 0; c->head = 0; c->closed = 0;
        _vch_init(c);
        c->used = 1;
        _vch_tab_unlock();
        return vint(i);
    }
    _vch_tab_unlock();
    return _verr_c("too many channels");
}

BFN Val vchan_send(Val h, Val v) {
    VChan *c = _vch_of(h);
    /* A handle whose slot is gone names a closed channel: the slot was only
     * reused once the channel had closed and drained. */
    if (!c) return _verr_c("send on closed channel");
    _vch_lk(c);
    while (c->len == c->cap && !c->closed) _vch_wait_snd(c);
    if (c->closed) { _vch_ul(c); return _verr_c("send on closed channel"); }
    /* The channel keeps the value after the call returns, and the caller's
     * argument vector is released right after this call, so the buffer takes a
     * reference of its own. Without it a sent tag or string was freed under the
     * receiver as soon as the sender's statement ended. */
    c->buf[(c->head + c->len) % c->cap] = vrc_inc(v);
    c->len++;
    _vch_wake_rcv(c);
    _vch_ul(c);
    return vnil();
}

BFN Val vchan_recv(Val h) {
    VChan *c = _vch_of(h);
    if (!c) return vnil();  /* closed and drained, or never a channel */
    _vch_lk(c);
    while (c->len == 0 && !c->closed) _vch_wait_rcv(c);
    if (c->len == 0) {
        _vch_ul(c);
        _vch_release(c);
        return vnil();
    }
    Val v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap;
    c->len--;
    _vch_wake_snd(c);
    _vch_ul(c);
    _vch_release(c);
    return v;
}

BFN Val vchan_close(Val h) {
    VChan *c = _vch_of(h);
    if (!c) return vnil();  /* closing twice is not an error */
    _vch_lk(c);
    c->closed = 1;
    _vch_wake_all(c);
    _vch_ul(c);
    _vch_release(c);
    return vnil();
}

BFN Val vbuiltin(Val obj, const char *meth, Vec *a) {
    if (obj.tag == V_STR) {
        if (!strcmp(meth, "split")) { if (VARG(a, 0).tag == V_STR && !VARG(a, 0).as.str[0]) { vraise("empty separator"); return vunwind; } return vsplit(obj, VARG(a, 0)); }
        if (!strcmp(meth, "lines")) return vlines(obj);
        if (!strcmp(meth, "trim")) return vtrim(obj);
        if (!strcmp(meth, "lower")) return vcase(obj, 0);
        if (!strcmp(meth, "upper")) return vcase(obj, 1);
        if (!strcmp(meth, "starts_with")) return vbool(vaffix(obj, VARG(a, 0), 1, 0));
        if (!strcmp(meth, "ends_with")) return vbool(vaffix(obj, VARG(a, 0), 0, 1));
        if (!strcmp(meth, "contains")) return vbool(vaffix(obj, VARG(a, 0), 0, 0));
        if (!strcmp(meth, "parse_int")) return vparse_int(obj);
        if (!strcmp(meth, "replace")) return vreplace(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "index_of")) return vindex_of(obj, VARG(a, 0));
        if (!strcmp(meth, "repeat")) return vrepeat(obj, VARG(a, 0));
        if (!strcmp(meth, "split_once")) return vsplit_once(obj, VARG(a, 0));
        if (!strcmp(meth, "copy")) return obj;
        if (!strcmp(meth, "chars")) return vchars(obj);
        if (!strcmp(meth, "parse_float")) return vparse_float(obj);
        if (!strcmp(meth, "slice")) return vstr_slice(obj, VARG(a, 0), VARG(a, 1));
    }
    if (obj.tag == V_VEC) {
        if (!strcmp(meth, "join")) return vjoin(obj, VARG(a, 0));
        if (!strcmp(meth, "push")) return vpush(obj, VARG(a, 0));
        if (!strcmp(meth, "pop")) return vpop(obj);
        if (!strcmp(meth, "first")) return vfirst(obj);
        if (!strcmp(meth, "last")) return vlast(obj);
        if (!strcmp(meth, "sort")) return vsort(obj);
        if (!strcmp(meth, "rev")) return vrev(obj);
        if (!strcmp(meth, "ins")) return vins(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "del")) return vdel_at(obj, VARG(a, 0));
        if (!strcmp(meth, "slice")) return vslice(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "sum")) return vsum(obj);
        if (!strcmp(meth, "min")) return vmin(obj);
        if (!strcmp(meth, "max")) return vmax(obj);
        if (!strcmp(meth, "enumerate")) return venum(obj);
        if (!strcmp(meth, "copy")) return vcopy(obj);
        if (!strcmp(meth, "zip")) return vzip(obj, VARG(a, 0));
        if (!strcmp(meth, "chunks")) return vchunks(obj, VARG(a, 0));
        if (!strcmp(meth, "slice")) return vslice(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "pos")) return vpos(obj, VARG(a, 0));
    }
    if (obj.tag == V_INT) {
        if (!strcmp(meth, "send")) return vchan_send(obj, VARG(a, 0));
        if (!strcmp(meth, "recv")) return vchan_recv(obj);
        if (!strcmp(meth, "close")) return vchan_close(obj);
    }
    if (obj.tag == V_STRUCT) {
        if (!strcmp(meth, "get")) return vmap_getv(obj, VARG(a, 0));
        if (!strcmp(meth, "has")) return vmap_has(obj, VARG(a, 0));
        if (!strcmp(meth, "keys")) return vmap_keys(obj);
        if (!strcmp(meth, "del")) return vmap_del(obj, VARG(a, 0));
        if (!strcmp(meth, "set")) return vmap_setv(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "items")) return vmap_items(obj);
        if (!strcmp(meth, "vals")) return vmap_vals(obj);
        if (!strcmp(meth, "copy")) return vcopy(obj);
        if (!strcmp(meth, "get_or")) return vmap_get_or(obj, VARG(a, 0), VARG(a, 1));
    }
    vraisef("no method %s", meth); return vunwind;
}

typedef struct BTask BTask;
struct BTask {
    Val clo;
    Val result;
    Val arg;                /* `par for`: the element this task folds */
    Val *slot;              /* where the result goes, when the caller wants it */
    int has_arg;
    int err;
    BTask *qnext;           /* the run queue and the finished list */
    BTask *next;            /* the scope that owns it, when there is one */
    struct VScope *scope;
};
static BTask *g_done = 0;

/* Lazy bint-lived pool: one OS thread per spawn segfaults around 300. */
#define BPOOL_MAX 32
static int g_pool_n = 0;
#if defined(_WIN32)
static HANDLE g_pool[BPOOL_MAX];
#else
static pthread_t g_pool[BPOOL_MAX];
#endif
static BTask *g_q_head = 0;
static BTask *g_q_tail = 0;
static bint g_q_nleft = 0;
static bint g_parked = 0;
static BTLS int g_btask_self;

#if defined(_WIN32)
static CRITICAL_SECTION btlock;
static int btlock_init;
static void btask_lock(void) {
    if (!btlock_init) { InitializeCriticalSection(&btlock); btlock_init = 1; }
    EnterCriticalSection(&btlock);
}
static void btask_unlock(void) { LeaveCriticalSection(&btlock); }
#else
#include <sched.h>
static pthread_mutex_t btlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_q_idle = PTHREAD_COND_INITIALIZER;
static void btask_lock(void) { pthread_mutex_lock(&btlock); }
static void btask_unlock(void) { pthread_mutex_unlock(&btlock); }
#endif

/* A scope owns the tasks started inside it and joins them at its end. Nested
 * spawns inherit the scope through BTask.scope, so a child's children are
 * joined by the same block. */
typedef struct VScope {
    BTask *tasks;
    bint nleft;
    int err;
    Val errv;
    struct VScope *up;
} VScope;
static BTLS VScope *_vscope_top;

static void vscope_attach(VScope *sc, BTask *t) {
    t->scope = sc;
    btask_lock();
    t->result = vnil();
    t->next = sc->tasks;
    sc->tasks = t;
    sc->nleft++;
    btask_unlock();
}

static int bpool_ncpu(void) {
    /* BRAT_THREADS pins the pool size: CI and reproducible runs need a number
       a machine cannot argue with, and a result that depends on core count is
       not reproducible. 0 or unset means "as many as the machine has". */
    const char *want = getenv("BRAT_THREADS");
    if (want && *want) {
        int n = atoi(want);
        if (n >= 1) return n;
    }
#if defined(_WIN32)
    return 2;
#else
    bint n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n >= 1) ? (int)n : 1;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI bpool_worker(void *p);
#else
static void *bpool_worker(void *p);
#endif

static void bpool_start_locked(void) {
    if (g_pool_n) return;
    int n = bpool_ncpu();
    if (n > BPOOL_MAX) n = BPOOL_MAX;
    if (n < 1) n = 1;
    g_pool_n = n;
    for (int i = 0; i < n; i++) {
#if defined(_WIN32)
        g_pool[i] = CreateThread(NULL, 0, bpool_worker, NULL, 0, NULL);
        if (!g_pool[i]) { g_pool_n = i; return; }
#else
        if (pthread_create(&g_pool[i], NULL, bpool_worker, NULL) != 0) {
            g_pool_n = i;
            return;
        }
#endif
    }
}

BFN Val verr_own(Val v) {
    if (v.tag != V_TAG || strcmp(v.as.tv.tag, "err")) return v;
    Vec *a = vvec();
    for (bint i = 0; i < v.as.tv.args->len; i++) vec_push(a, vrc_inc(v.as.tv.args->items[i]));
    return vtagv("err", a);
}

static void bpool_wake(void) {
#if !defined(_WIN32)
    pthread_cond_signal(&g_q_cond);
#endif
}

static void bpool_idle_wake(void) {
#if !defined(_WIN32)
    pthread_cond_broadcast(&g_q_idle);
#endif
}

static void btask_run(BTask *t) {
    int outer = g_btask_self;
    VScope *outer_scope = _vscope_top;
    g_btask_self = 1;
    _vscope_top = t->scope;
    verr_clear();
    Vec *a = vvec();
    if (t->has_arg) {
        vec_push(a, t->arg);     /* the task owns this reference */
        t->arg = vnil();
    }
    t->result = vcall_any(t->clo, a);
    vrc_dec(vvecv(a));
    /* The task held the only reference to the closure it ran. */
    vrelease(t->clo);
    t->clo = vnil();
    if (verr_pending()) {
        t->err = 1;
        t->result = verr_own(verr_take());
    } else if (t->result.tag == V_TAG && !strcmp(t->result.as.tv.tag, "err")) {
        t->err = 1;
    }
    if (!t->err && t->slot) {
        *t->slot = t->result;    /* the caller folds these; the task owns none */
        t->result = vnil();
    }
    g_btask_self = outer;
    _vscope_top = outer_scope;

    btask_lock();
    g_q_nleft--;
    if (t->scope) {
        if (t->err && !t->scope->err) { t->scope->err = 1; t->scope->errv = t->result; }
        t->scope->nleft--;
    } else {
        t->qnext = g_done;
        g_done = t;
    }
    bpool_idle_wake();
    btask_unlock();
}

#if defined(_WIN32)
static DWORD WINAPI bpool_worker(void *p) {
#else
static void *bpool_worker(void *p) {
#endif
    (void)p;
    for (;;) {
        btask_lock();
        while (g_q_head == 0) {
#if defined(_WIN32)
            btask_unlock();
            Sleep(0);
            btask_lock();
#else
            pthread_cond_wait(&g_q_cond, &btlock);
#endif
        }
        BTask *t = g_q_head;
        g_q_head = t->qnext;
        if (!g_q_head) g_q_tail = 0;
        btask_unlock();
        btask_run(t);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

BFN void vspawn(Val clo) {
    if (clo.tag != V_TAG || strcmp(clo.as.tv.tag, "closure") || clo.as.tv.args->len != 2) {
        vraise("spawn wants closure"); return;
    }

    BTask *t = (BTask *)calloc(1, sizeof(BTask));
    if (!t) { vraise("spawn failed: out of memory"); return; }
    t->clo = clo;
    t->qnext = 0;
    t->next = 0;
    if (_vscope_top) vscope_attach(_vscope_top, t);
    btask_lock();
    bpool_start_locked();
    if (g_q_tail) g_q_tail->qnext = t;
    else g_q_head = t;
    g_q_tail = t;
    g_q_nleft++;
    bpool_wake();
    btask_unlock();
}

BFN void vjoin_all(void) {
    int self = g_btask_self;
    btask_lock();
    if (self) g_parked++;
    for (;;) {
        /* Wait until every task but the parked join_all callers is done, running
           queued tasks here: otherwise a nested join_all starves its children. */
        bint target = self ? g_parked : 0;
        if (g_q_head == 0 && g_q_nleft <= target) break;
        if (g_q_head) {
            BTask *t = g_q_head;
            g_q_head = t->qnext;
            if (!g_q_head) g_q_tail = 0;
            btask_unlock();
            btask_run(t);
            btask_lock();
            continue;
        }
#if defined(_WIN32)
        btask_unlock();
        Sleep(0);
        btask_lock();
#else
        pthread_cond_wait(&g_q_idle, &btlock);
#endif
    }
    BTask *list = g_done;
    g_done = 0;
    if (self) g_parked--;
    btask_unlock();
    int err = 0;
    Val errv = vnil();
    while (list) {
        BTask *nx = list->qnext;
        if (list->err && !err) { err = 1; errv = list->result; }
        free(list);
        list = nx;
    }
    if (err) { vunwind = errv; vunwind_do(); }
}

/* `par for`: run `clo(element)` for every element, then fold with `op` in index
 * order. The body is pure — the checker refuses anything else — so the tasks
 * commute and the only order that can be observed is the fold, which is fixed.
 * An error in any task propagates through vjoin_all like a `spawn` error. */
BFN Val vparfor(Val vec, Val clo, Val unit, Val op) {
    Vec *v = vvec_of_m(vec, "par for");
    if (!v) return vunwind;
    bint n = v->len;
    if (n == 0) {
        vrc_dec(clo);            /* vparfor consumes both closures */
        vrc_dec(op);
        return unit;
    }
    Val *slots = (Val *)calloc((size_t)n, sizeof(Val));
    if (!slots) { vraise("par for: out of memory"); return vunwind; }
    for (bint i = 0; i < n; i++) slots[i] = vnil();
    for (bint i = 0; i < n; i++) {
        BTask *t = (BTask *)calloc(1, sizeof(BTask));
        if (!t) { vraise("par for: out of memory"); free(slots); return vunwind; }
        t->clo = vrc_inc(clo);
        t->arg = vrc_inc(v->items[i]);
        t->has_arg = 1;
        t->slot = &slots[i];
        t->result = vnil();
        if (_vscope_top) vscope_attach(_vscope_top, t);
        btask_lock();
        bpool_start_locked();
        if (g_q_tail) g_q_tail->qnext = t;
        else g_q_head = t;
        g_q_tail = t;
        g_q_nleft++;
        bpool_wake();
        btask_unlock();
    }
    vjoin_all();
    Val acc = unit;
    for (bint i = 0; i < n; i++) {
        Vec *args = vvec();
        vec_push(args, acc);
        vec_push(args, slots[i]);
        acc = vcall_any(op, args);
        args->len = 0;               /* both items are borrowed, not owned */
        vrc_dec(vvecv(args));
        vrc_dec(slots[i]);
    }
    free(slots);
    vrc_dec(clo);                /* vparfor consumes both closures */
    vrc_dec(op);
    return acc;
}

BFN Val vscope_begin(void) {
    VScope *sc = (VScope *)calloc(1, sizeof(VScope));
    if (!sc) { vraise("scope: out of memory"); return vunwind; }
    sc->errv = vnil();
    sc->up = _vscope_top;
    _vscope_top = sc;
    return vint((bint)(intptr_t)sc);
}

/* Join every task this scope started. A queued task is run here rather than
 * waited for: a bounded pool that only blocked would deadlock a scope whose
 * children are all still in the queue. */
BFN void vscope_end(Val h) {
    VScope *sc = (VScope *)(intptr_t)h.as.ival;
    if (!sc) return;
    for (;;) {
        btask_lock();
        if (sc->nleft <= 0) { btask_unlock(); break; }
        if (g_q_head) {
            BTask *t = g_q_head;
            g_q_head = t->qnext;
            if (!g_q_head) g_q_tail = 0;
            btask_unlock();
            btask_run(t);
            continue;
        }
#if defined(_WIN32)
        btask_unlock();
        Sleep(0);
#else
        pthread_cond_wait(&g_q_idle, &btlock);
        btask_unlock();
#endif
    }
    if (_vscope_top == sc) _vscope_top = sc->up;
    BTask *list = sc->tasks;
    int err = sc->err;
    Val errv = sc->errv;
    while (list) {
        BTask *nx = list->next;
        free(list);
        list = nx;
    }
    free(sc);
    if (err) { vunwind = errv; vunwind_do(); }
}

#endif /* BRAT_RT_H */
