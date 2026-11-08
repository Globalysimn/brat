
/* `fileno`, `realpath` and `strdup` are POSIX, not ISO C: under `-std=c11` they
   are hidden unless the feature level is asked for, and the runtime then fails
   to compile for a user who tries strict ISO C. Ask before the first include;
   The floor is C99 plus POSIX: nothing in this runtime needs C11. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
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
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef LOAD_WITH_ALTERED_SEARCH_PATH
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008
#endif
#ifndef FILE_NAME_NORMALIZED
#define FILE_NAME_NORMALIZED 0x0
#endif
#else
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <netdb.h>
#endif
/* Baked memory contract: `brat build --mem N` compiles BRAT_MEM_BYTES in and
 * main() applies it as RLIMIT_AS on POSIX or a JobObject memory limit on
 * Windows before anything else allocates. 0 (the default) means no budget. */
#ifndef BRAT_MEM_BYTES
#define BRAT_MEM_BYTES 0
#endif
#ifndef BRAT_MAX_STR
#define BRAT_MAX_STR ((size_t)1 << 30)
#endif
#ifndef BRAT_MAX_FILE_BYTES
#define BRAT_MAX_FILE_BYTES ((size_t)64 << 20)
#endif
#ifndef BRAT_MAX_READ_BYTES
#define BRAT_MAX_READ_BYTES ((size_t)16 << 20)
#endif
#ifndef BRAT_MAX_BYTE_VEC
#define BRAT_MAX_BYTE_VEC ((size_t)16 << 20)
#endif
#define BRAT_CRT_LIB  0x001u
#define BRAT_CRT_RE   0x002u
#define BRAT_CRT_NET  0x004u
#define BRAT_CRT_TLS  0x008u
#define BRAT_CRT_SQL  0x010u
#define BRAT_CRT_PROC 0x020u
#define BRAT_CRT_CASE 0x040u
#define BRAT_CRT_CONC 0x080u
#define BRAT_CRT_MT   0x100u
#define BRAT_CRT_DYN  0x200u
#define BRAT_CRT_JSON 0x400u
#ifdef BRAT_HAS_VTAB_DISP
static const char *_brat_vtag_disp(const char *t);
#endif
#if defined(BRAT_RT_SHARED) || !defined(BRAT_CRT_MASK)
#undef BRAT_CRT_MASK
#define BRAT_CRT_MASK 0xFFFFu
#endif
#if !defined(BRAT_RT_SHARED) && !(BRAT_CRT_MASK & BRAT_CRT_MT)
#define BTLS
#elif defined(_MSC_VER)
#define BTLS __declspec(thread)
#else
#define BTLS _Thread_local
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BRAT_HAS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || __has_feature(thread_sanitizer)
#define BRAT_HAS_SANITIZER 1
#endif
#endif
#ifndef BRAT_HAS_SANITIZER
#define BRAT_HAS_SANITIZER 0
#endif
#if defined(BRAT_RT_SHARED)
#define BFN
#define BFN_HOT
#else
/* `crt.h` is header-only: every program compiles all ~250 runtime functions and
   calls a handful, which is 81 `-Wunused-function` warnings per program and a
   `-Wall` line nobody reads. The attribute is about this file, not about the
   program, so it lives on the linkage macro. */
#if defined(__GNUC__) || defined(__clang__)
#define BFN static __attribute__((unused))
#define BFN_HOT static inline __attribute__((unused))
#else
#define BFN static
#define BFN_HOT static inline
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

static inline const char *_bmemmem(const char *hay, size_t hlen, const char *ndl, size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return NULL;
    const unsigned char first = (unsigned char)ndl[0];
    size_t i = 0, limit = hlen - nlen;
    while (i <= limit) {
        const char *hit = (const char *)memchr(hay + i, first, limit - i + 1);
        if (!hit) return NULL;
        if (nlen == 1 || memcmp(hit + 1, ndl + 1, nlen - 1) == 0) return hit;
        i = (size_t)(hit - hay) + 1;
    }
    return NULL;
}


typedef enum { V_NIL, V_BOOL, V_INT, V_FLOAT, V_STR, V_VEC, V_STRUCT, V_TAG, V_MOVED } VTag;
typedef struct Val Val;
typedef struct Vec Vec;
typedef struct Map Map;
typedef struct BShape {
    const char *type;
    Val *keys;
    bint len;
    int ready;
} BShape;
#define BPIC_WAYS 4
typedef struct BFieldIC {
    const BShape *shapes[BPIC_WAYS];
    bint slots[BPIC_WAYS];
    unsigned char next;
} BFieldIC;
typedef struct BMapSlot {
    uint32_t tag;
    int32_t pos;
} BMapSlot;

/* `imm` occupies the 4-byte padding hole after `tag` (sizeof(Val) == 16):
 * non-zero marks immortal literals and arena-allocated values so vrc_inc /
 * vrc_dec / vrelease never dereference BHead (which may live in an overflow
 * arena chunk already unmapped by varena_leave). */
#if defined(__GNUC__) || defined(__clang__)
#define BRAT_BF __extension__
#else
#define BRAT_BF
#endif
/* 16-byte Val with inline Small-String Optimization (SSO <= 14 bytes + NUL):
 * - byte 0: tag (4 bits), sso (1 bit), imm (3 bits; non-zero skips RC/free).
 * - bytes 1..15: when sso == 1, holds 0..14 UTF-8 bytes + '\0', with
 *   byte 15 storing (14 - len) so len == 14 ends cleanly on '\0' at byte 15.
 * - bytes 8..15: 8-byte aligned payload union `as` when sso == 0. */
#define BRAT_SSO_MAX 14
struct Val {
    BRAT_BF uint8_t tag : 4;
    BRAT_BF uint8_t sso : 1;
    BRAT_BF uint8_t imm : 3;
    char _sso_pad[7];
    union { bint b; bint ival; double fval; char *str; Vec *vec; Map *map; Vec *tv; } as;
};
_Static_assert(sizeof(Val) == 16, "BRAT Val must be 16 bytes");
/* Inline 2-element Small-Buffer Optimization (`inl[2]`): `ok(x)`, `err(msg)`,
 * `some(x)`, 1..2-element vectors, and `closure(fn, env)` perform zero secondary
 * heap allocations while keeping `v->items` directly indexable everywhere. */
struct Vec { Val *items; bint len, cap; char *tag; Val inl[2]; };
#define BVEC_FREE_ITEMS(v_) do { if ((v_)->items && (v_)->items != (v_)->inl) free((v_)->items); } while (0)
/* Inline 2-entry Small-Buffer Optimization (`inl_k/v/h[2]`): `{}` empty maps,
 * 1..2-entry maps and structs use inline key/value/hash storage. Closure
 * environments use dense value slots, inline for up to two captures and one
 * values allocation for larger captures. */
struct Map {
    Val *keys; Val *vals; bint len, used, dead, cap; const char *type;
    BMapSlot *slots; uint64_t *hashes; const BShape *shape;
    bint idx_cap; bint idx_used; bint bad_key; int inline_vals;
    Val inl_k[2]; Val inl_v[2]; uint64_t inl_h[2];
};
#define BMAP_FREE_ARRS(m_) do { \
    if (!(m_)->shape && (m_)->keys && (m_)->keys != (m_)->inl_k) free((m_)->keys); \
    if (!(m_)->inline_vals && (m_)->vals && (m_)->vals != (m_)->inl_v) free((m_)->vals); \
    if ((m_)->hashes && (m_)->hashes != (m_)->inl_h) free((m_)->hashes); \
    if ((m_)->slots) free((m_)->slots); \
} while (0)

static const char _vclosure_env_type[] = "__brat_closure_env";
static int vclosure_env_map(const Map *m) { return m && m->type == _vclosure_env_type; }

static int BRAT_UNUSED g_pool_n = 0;
#if !defined(_WIN32)
typedef pthread_mutex_t bmutex_t;
#define BMUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define BMUTEX_LOCK(m) pthread_mutex_lock(m)
#define BMUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#else
typedef SRWLOCK bmutex_t;
#define BMUTEX_INIT SRWLOCK_INIT
#define BMUTEX_LOCK(m) AcquireSRWLockExclusive(m)
#define BMUTEX_UNLOCK(m) ReleaseSRWLockExclusive(m)
#endif
typedef union { bmutex_t mu; char _line[64]; } BStripeLock;
#define BSTRIPE_1_  { .mu = BMUTEX_INIT }
#define BSTRIPE_4_  BSTRIPE_1_, BSTRIPE_1_, BSTRIPE_1_, BSTRIPE_1_
#define BSTRIPE_16_ BSTRIPE_4_, BSTRIPE_4_, BSTRIPE_4_, BSTRIPE_4_
static BStripeLock BRAT_UNUSED _brat_stripes[64] = { BSTRIPE_16_, BSTRIPE_16_, BSTRIPE_16_, BSTRIPE_16_ };
#undef BSTRIPE_16_
#undef BSTRIPE_4_
#undef BSTRIPE_1_
#define BSTRIPE_IDX(p_) ((unsigned)((((uintptr_t)(p_) >> 6) * 11400714819323198485ULL) >> 58))
#if defined(BRAT_RT_SHARED)
#define _BRAT_MT() 1
#elif !(BRAT_CRT_MASK & BRAT_CRT_MT)
#define _BRAT_MT() 0
#elif defined(__GNUC__) || defined(__clang__)
#define _BRAT_MT() (__builtin_expect(__atomic_load_n(&g_pool_n, __ATOMIC_ACQUIRE) != 0, 0))
#else
#define _BRAT_MT() (g_pool_n != 0)
#endif
#if defined(__GNUC__) || defined(__clang__)
#define BRAT_LIKELY(x_) (__builtin_expect(!!(x_), 1))
#else
#define BRAT_LIKELY(x_) (x_)
#endif
#define BRAT_CTR_LOCK_OBJ(p_)   do { if (_BRAT_MT()) BMUTEX_LOCK(&_brat_stripes[BSTRIPE_IDX(p_)].mu); } while (0)
#define BRAT_CTR_UNLOCK_OBJ(p_) do { if (_BRAT_MT()) BMUTEX_UNLOCK(&_brat_stripes[BSTRIPE_IDX(p_)].mu); } while (0)
static bmutex_t BRAT_UNUSED _brat_plugin_lock = BMUTEX_INIT;
#define BRAT_PLUGIN_LOCK()   BMUTEX_LOCK(&_brat_plugin_lock)
#define BRAT_PLUGIN_UNLOCK() BMUTEX_UNLOCK(&_brat_plugin_lock)
static bmutex_t BRAT_UNUSED _brat_print_lock = BMUTEX_INIT;
#define BRAT_PRINT_LOCK() BMUTEX_LOCK(&_brat_print_lock)
#define BRAT_PRINT_UNLOCK() BMUTEX_UNLOCK(&_brat_print_lock)

typedef struct BHead {
    bint rc;
    bint cap;
    bint len;
    bint cplen;
    int ascii;
    uint64_t hash;
    struct BHead *arena_next;
} BHead;
BFN void vraise(const char *msg);
BFN Val vstr_of(Val v);
BFN void vraisef(const char *fmt, const char *arg);

/* Depth budget for the recursive value walkers (print, length, compare).

   `str()` and `print` are recursive over the object graph, so a program that
   builds a deep value -- `for i in 0..200000 { v = [v] }` is 200 000 cheap
   allocations -- walked the machine stack once per nesting level and died with
   SIGSEGV at around 100 000 levels. Past BRAT_MAX_VALUE_DEPTH the error channel gets
   "recursion too deep" and the walker stops, so the program exits 1 with an
   error instead of dying. The counter is thread-local because `spawn` runs the
   same helpers on several threads. */
#define BRAT_MAX_VALUE_DEPTH 2000
static BTLS bint bval_depth = 0;
static BTLS bint _btrace_cur_line = 0;
static BTLS bint _verr_line = 0;
BFN_HOT void btrace_line(bint ln) { _btrace_cur_line = ln; }
BFN int vtrace_enabled(void) {
    const char *ev = getenv("BRAT_TRACE");
#ifdef BRAT_TRACE_DEFAULT
    return (!ev || (ev[0] && ev[0] != '0'));
#else
    return (ev && ev[0] && ev[0] != '0');
#endif
}
BFN void vtrace_print_err_line(void) {
    if (_verr_line > 0 && vtrace_enabled()) {
        fprintf(stderr, " at line " BINT_FMT, _verr_line);
    }
    _verr_line = 0;
}
BFN void vtrace_print_cur_line(void) {
    if (_btrace_cur_line > 0 && vtrace_enabled()) {
        fprintf(stderr, " at line " BINT_FMT, _btrace_cur_line);
    }
}

/* Declared here because `vprint` pre-checks the nesting before it writes. */
BFN bint vsprint_len_q(Val v, bint q);
BFN_HOT int verr_pending(void);
BFN Val verr_take(void);
BFN void verr_clear(void);
BFN bint utf8_cplen(const char *s);
BFN void vjprint(Val v);

BFN bint veq(Val a, Val b);
BFN Val vat(Val a, Val i);
BFN Val vlen(Val a);
BFN Val vset_at(Val m, Val k, Val v);
BFN double vnum(Val v);
BFN_HOT Val vint(bint n);
BFN_HOT Val vfloat(double f);
BFN Val vstr(const char *s);
BFN Val vstrn(const char *s, bint n);
BFN Vec *vvec(void);
BFN Vec *vvec_cap(bint cap);
BFN_HOT Val vvecv(Vec *v);
BFN_HOT Val vclosure0(bint code);
BFN void vec_push(Vec *v, Val x);
typedef Val (*BMethFn)(Map *, Vec *);
typedef struct {
    const char *tns[BPIC_WAYS];
    BMethFn fns[BPIC_WAYS];
    unsigned char tags[BPIC_WAYS];
    unsigned char hits[BPIC_WAYS];
    unsigned char next;
} BMethIC;
typedef struct { BHead head; Vec vec; } BStackVec;
#define VSTACK_VEC_INIT(sv, buf, cap_) do { \
    (sv).head.rc = -1; (sv).head.cap = 0; (sv).head.len = 0; \
    (sv).head.cplen = 0; (sv).head.ascii = 0; (sv).head.hash = 0; (sv).head.arena_next = 0; \
    (sv).vec.items = (buf); (sv).vec.len = 0; (sv).vec.cap = (cap_); (sv).vec.tag = 0; \
} while (0)
BFN_HOT void vargs_drop(Vec *v);
BFN_HOT bint vub_div(bint a, bint b);
BFN_HOT bint vub_mod(bint a, bint b);
BFN_HOT double vub_fdiv(double a, double b);
BFN_HOT double vub_fmod(double a, double b);
BFN_HOT int vub_fcmp(double a, double b, int op);

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

BFN_HOT Val vnil(void) { Val v; v.tag = V_NIL; v.sso = 0; v.imm = 0; v.as.ival = 0; return v; }
BFN_HOT Val vbool(bint b) { Val v; v.tag = V_BOOL; v.sso = 0; v.imm = 0; v.as.b = b ? 1 : 0; return v; }
BFN_HOT Val vint(bint n) { Val v; v.tag = V_INT; v.sso = 0; v.imm = 0; v.as.ival = n; return v; }
BFN_HOT Val vfloat(double n) { Val v; v.tag = V_FLOAT; v.sso = 0; v.imm = 0; v.as.fval = n; return v; }

BFN_HOT Val vstr_sso(const char *s, size_t n) {
    Val v;
    memset(&v, 0, sizeof(v));
    v.tag = V_STR;
    v.sso = 1;
    v.imm = 1;
    char *dst = (char *)&v + 1;
    if (n) memcpy(dst, s, n);
    dst[n] = '\0';
    ((unsigned char *)&v)[15] = (unsigned char)(BRAT_SSO_MAX - n);
    return v;
}
BFN_HOT const char *vstr_c(const Val *v) {
    return v->sso ? ((const char *)v + 1) : v->as.str;
}
BFN_HOT size_t vstr_sso_len(const Val *v) {
    return (size_t)(BRAT_SSO_MAX - ((const unsigned char *)v)[15]);
}
BFN_HOT size_t vstr_len_fast(const Val *v) {
    if (v->sso) return vstr_sso_len(v);
    return v->as.str ? (size_t)((BHead *)v->as.str - 1)->len : 0;
}
BFN_HOT bint vstr_empty(Val v) {
    return v.sso ? (((const char *)&v)[1] == '\0') : (!v.as.str || v.as.str[0] == '\0');
}
BFN_HOT int vstr_has_nul(const Val *v) {
    return memchr(vstr_c(v), 0, vstr_len_fast(v)) != NULL;
}

/* Demand-paged, non-moving intern pool for pinned short strings (<= 14 bytes).
 * Slabs are never moved or evicted so raw pointers handed to C remain valid for
 * the process lifetime; zero heap or `.bss` memory is consumed until first use.
 * Total slab storage is capped by `BRAT_PIN_BUDGET_BYTES` (default 64 MiB =
 * 4,194,304 distinct short strings), raising a catchable BRAT error on overflow
 * instead of aborting the host process. */
#define BVPIN_SLAB_SHIFT 12u
#define BVPIN_SLAB_SLOTS (1u << BVPIN_SLAB_SHIFT)
#define BVPIN_EMPTY 0xFFFFFFFFu
#ifndef BRAT_DEFAULT_PIN_BUDGET_BYTES
#define BRAT_DEFAULT_PIN_BUDGET_BYTES (64ULL * 1024ULL * 1024ULL)
#endif
static char (**_bvpin_slabs)[16] = NULL;
static size_t _bvpin_nslabs = 0;
static size_t _bvpin_slab_cap = 0;
static uint32_t *_bvpin_ht = NULL;
static size_t _bvpin_ht_cap = 0;
static uint32_t _bvpin_count = 0;
static size_t _bvpin_budget_bytes = 0;
static int _bvpin_inited = 0;
static bmutex_t _bvpin_mu = BMUTEX_INIT;

static void vraise(const char *msg);

static uint64_t _bvpin_hash_bytes(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)n;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static size_t _bvpin_load_budget(void) {
    const char *env = getenv("BRAT_PIN_BUDGET_BYTES");
    if (env && *env) {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && *end == '\0' && v >= 64ULL) {
            if (v > (unsigned long long)(SIZE_MAX / 2))
                return SIZE_MAX / 2;
            return (size_t)v;
        }
    }
    return (size_t)BRAT_DEFAULT_PIN_BUDGET_BYTES;
}

static int _bvpin_rehash(size_t new_cap) {
    uint32_t *nht = (uint32_t *)malloc(new_cap * sizeof(uint32_t));
    if (!nht) return 0;
    memset(nht, 0xFF, new_cap * sizeof(uint32_t));
    for (uint32_t s = 0; s < _bvpin_count; s++) {
        const char *p = _bvpin_slabs[s >> BVPIN_SLAB_SHIFT][s & (BVPIN_SLAB_SLOTS - 1u)];
        size_t len = (size_t)(unsigned char)p[15];
        size_t idx = (size_t)_bvpin_hash_bytes(p, len) & (new_cap - 1u);
        while (nht[idx] != BVPIN_EMPTY) idx = (idx + 1u) & (new_cap - 1u);
        nht[idx] = s;
    }
    free(_bvpin_ht);
    _bvpin_ht = nht;
    _bvpin_ht_cap = new_cap;
    return 1;
}

BFN char *vstr_pin(Val v) {
    if (v.tag != V_STR) return NULL;
    if (!v.sso) return v.as.str;
    const char *src = vstr_c(&v);
    size_t n = vstr_sso_len(&v);
    uint64_t h = _bvpin_hash_bytes(src, n);
    BMUTEX_LOCK(&_bvpin_mu);
    if (!_bvpin_inited) {
        _bvpin_budget_bytes = _bvpin_load_budget();
        if (!_bvpin_rehash(8192u)) {
            BMUTEX_UNLOCK(&_bvpin_mu);
            vfatal_oom();
        }
        _bvpin_inited = 1;
    }
    size_t idx = (size_t)h & (_bvpin_ht_cap - 1u);
    while (_bvpin_ht[idx] != BVPIN_EMPTY) {
        uint32_t slot = _bvpin_ht[idx];
        char *p = _bvpin_slabs[slot >> BVPIN_SLAB_SHIFT][slot & (BVPIN_SLAB_SLOTS - 1u)];
        if ((unsigned char)p[15] == n && memcmp(p, src, n + 1) == 0) {
            BMUTEX_UNLOCK(&_bvpin_mu);
            return p;
        }
        idx = (idx + 1u) & (_bvpin_ht_cap - 1u);
    }
    size_t next_bytes = ((size_t)_bvpin_count + 1u) * 16u;
    if (_bvpin_count >= 0x7FFFFFFFu || next_bytes > _bvpin_budget_bytes) {
        BMUTEX_UNLOCK(&_bvpin_mu);
        vraise("string pin limit exceeded");
        return NULL;
    }
    if (((size_t)_bvpin_count + 1u) * 2u >= _bvpin_ht_cap) {
        if (!_bvpin_rehash(_bvpin_ht_cap * 2u)) {
            BMUTEX_UNLOCK(&_bvpin_mu);
            vraise("string pin limit exceeded");
            return NULL;
        }
        idx = (size_t)h & (_bvpin_ht_cap - 1u);
        while (_bvpin_ht[idx] != BVPIN_EMPTY) idx = (idx + 1u) & (_bvpin_ht_cap - 1u);
    }
    size_t slab_ix = (size_t)_bvpin_count >> BVPIN_SLAB_SHIFT;
    if (slab_ix >= _bvpin_nslabs) {
        if (slab_ix >= _bvpin_slab_cap) {
            size_t ncap = _bvpin_slab_cap ? _bvpin_slab_cap * 2u : 16u;
            char (**ns)[16] = (char (**)[16])realloc(_bvpin_slabs, ncap * sizeof(*_bvpin_slabs));
            if (!ns) {
                BMUTEX_UNLOCK(&_bvpin_mu);
                vraise("string pin limit exceeded");
                return NULL;
            }
            _bvpin_slabs = ns;
            _bvpin_slab_cap = ncap;
        }
        char (*slab)[16] = (char (*)[16])malloc(BVPIN_SLAB_SLOTS * 16u);
        if (!slab) {
            BMUTEX_UNLOCK(&_bvpin_mu);
            vraise("string pin limit exceeded");
            return NULL;
        }
        _bvpin_slabs[_bvpin_nslabs++] = slab;
    }
    uint32_t slot = _bvpin_count++;
    char *p = _bvpin_slabs[slab_ix][slot & (BVPIN_SLAB_SLOTS - 1u)];
    memcpy(p, src, n + 1);
    p[15] = (char)n;
    _bvpin_ht[idx] = slot;
    BMUTEX_UNLOCK(&_bvpin_mu);
    return p;
}

static void *balloc(size_t n);
static BTLS int barena_depth = 0;
static struct { const char *src; BHead *hdr; } _vlit_cache[512];
static int _vlit_n = 0;
static bmutex_t _vlit_mu = BMUTEX_INIT;
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
static Val vlit_slow(const char *s, size_t n) {
    int seen = _vlit_count();
    for (int i = 0; i < seen; i++) {
        if (_vlit_cache[i].src == s) {
            Val v; v.tag = V_STR; v.sso = 0; v.imm = 1; v.as.str = (char *)(_vlit_cache[i].hdr + 1);
            return v;
        }
    }
    int saved = barena_depth;
    barena_depth = 0;
    char *p = (char *)balloc(n + 1);
    barena_depth = saved;
    memcpy(p, s, n + 1);
    BHead *hdr = (BHead *)p - 1;
    hdr->rc = -1; /* immortal: never freed, never counted */
    hdr->len = (bint)n;
    BMUTEX_LOCK(&_vlit_mu);
    for (int i = 0; i < _vlit_n; i++) {
        if (_vlit_cache[i].src == s) {
            char *existing = (char *)(_vlit_cache[i].hdr + 1);
            BMUTEX_UNLOCK(&_vlit_mu);
            free(hdr);
            Val v; v.tag = V_STR; v.sso = 0; v.imm = 1; v.as.str = existing;
            return v;
        }
    }
    if (_vlit_n < 512) {
        _vlit_cache[_vlit_n].src = s;
        _vlit_cache[_vlit_n].hdr = hdr;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&_vlit_n, _vlit_n + 1, __ATOMIC_RELEASE);
#else
        _vlit_n++;
#endif
    }
    BMUTEX_UNLOCK(&_vlit_mu);
    Val v; v.tag = V_STR; v.sso = 0; v.imm = 1; v.as.str = p;
    return v;
}
BFN_HOT Val vlit(const char *s) {
    size_t n = strlen(s);
    if (n <= BRAT_SSO_MAX) return vstr_sso(s, n);
    return vlit_slow(s, n);
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
#define BRC_LOAD(h) (_BRAT_MT() ? __atomic_load_n(&(h)->rc, __ATOMIC_ACQUIRE) : (h)->rc)
#define BRC_ADD(h)  (_BRAT_MT() ? __atomic_add_fetch(&(h)->rc, 1, __ATOMIC_ACQ_REL) : (++(h)->rc))
#define BRC_SUB(h)  (_BRAT_MT() ? __atomic_sub_fetch(&(h)->rc, 1, __ATOMIC_ACQ_REL) : (--(h)->rc))
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
BFN int bsize_over_bint(size_t n) {
#if SIZE_MAX > INT64_MAX
    return n > (size_t)INT64_MAX;
#else
    (void)n;
    return 0;
#endif
}
BFN void *balloc(size_t n) {
    if (barena_depth > 0) {
        if (n > SIZE_MAX - sizeof(size_t) - sizeof(BHead) ||
            bsize_over_bint(n)) vfatal_oom();
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
    if (n > SIZE_MAX - sizeof(BHead) || bsize_over_bint(n)) vfatal_oom();
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
    if (n > SIZE_MAX - sizeof(BHead) || bsize_over_bint(n)) vfatal_oom();
    void *q = realloc(h, sizeof(BHead) + n);
    if (!q) { vfatal_oom(); }
    ((BHead *)q)->cap = (bint)n;
    return (BHead *)q + 1;
}
/* Container arrays follow their container: an arena container takes its arrays
 * from the arena too. Arena arrays are never freed one by one, and growing one
 * copies into fresh arena space; the old space is reclaimed by barena_leave. */
BFN size_t bmul_size(bint count, size_t elem) {
    if (count < 0) vfatal_oom();
    if (elem == 0) return 0;
    uint64_t u = (uint64_t)count;
    if (u > (uint64_t)(SIZE_MAX / elem) || u * (uint64_t)elem > (uint64_t)INT64_MAX)
        vfatal_oom();
    return (size_t)u * elem;
}
BFN bint bnext_cap(bint oldcap, size_t elem) {
    if (oldcap < 8) return 8;
    uint64_t max_items = elem ? (uint64_t)(SIZE_MAX / elem) : (uint64_t)INT64_MAX;
    if (max_items > (uint64_t)INT64_MAX) max_items = (uint64_t)INT64_MAX;
    if ((uint64_t)oldcap >= max_items) vfatal_oom();
    uint64_t dbl = (uint64_t)oldcap * 2ULL;
    return (bint)(dbl > max_items ? max_items : dbl);
}
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

BFN_HOT Val vrc_inc(Val v) {
    if (v.imm) return v;
    switch (v.tag) {
    case V_STR:   if (v.as.str)  { BHead *h = (BHead *)v.as.str - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); }  return v;
    case V_VEC:   if (v.as.vec)  { BHead *h = (BHead *)v.as.vec - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); }  return v;
    case V_STRUCT: if (v.as.map) { BHead *h = (BHead *)v.as.map - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); } return v;
    case V_TAG:   if (v.as.tv) { BHead *h = (BHead *)v.as.tv - 1; if (!BIS_IMMORTAL(h)) BRC_ADD(h); } return v;
    default: return v;
    }
}
BFN void vrelease1(Val v);
typedef struct { Val v; int post; } BVRelEnt;
#define BVREL_RAW_(val_, post_) do { \
    if (_sp == _cap) { \
        _cap *= 2; \
        BVRelEnt *_ns = (_stk == _sbuf) ? (BVRelEnt *)malloc(sizeof(BVRelEnt) * _cap) \
                                        : (BVRelEnt *)realloc(_stk, sizeof(BVRelEnt) * _cap); \
        if (!_ns) vfatal_oom(); \
        if (_stk == _sbuf) memcpy(_ns, _sbuf, sizeof(BVRelEnt) * _sp); \
        _stk = _ns; \
    } \
    _stk[_sp].v = (val_); \
    _stk[_sp].post = (post_); \
    _sp++; \
} while (0)
#define BVREL_PUSH_(el_) do { \
    Val _el = (el_); \
    if (!_el.imm && !(_pend && vhead_of(_el) == _uw_head)) { \
        if (_el.tag == V_STR) { \
            char *_es = _el.as.str; \
            if (_es) { \
                BHead *_eh = (BHead *)_es - 1; \
                if (!BIS_IMMORTAL(_eh) && !BIS_ARENA(_eh) && BRC_LOAD(_eh) > 0 && BRC_SUB(_eh) == 0) free(_eh); \
            } \
        } else if (_el.tag == V_VEC || _el.tag == V_STRUCT || _el.tag == V_TAG) { \
            BVREL_RAW_(_el, 0); \
        } \
    } \
} while (0)

#if !BRAT_HAS_SANITIZER
static BHead *_bmap_free = 0;
static int _bmap_free_n = 0;
static BHead *_bvec_free = 0;
static int _bvec_free_n = 0;
#endif
BFN_HOT Map *bmap_alloc(void) {
#if !BRAT_HAS_SANITIZER
    if (!_BRAT_MT() && !barena_depth && _bmap_free) {
        BHead *h = _bmap_free;
        _bmap_free = h->arena_next;
        _bmap_free_n--;
        h->rc = 1;
        h->cap = (bint)sizeof(Map);
        h->len = 0;
        h->cplen = -1;
        h->ascii = 0;
        h->hash = 0;
        h->arena_next = 0;
        return (Map *)(h + 1);
    }
#endif
    return (Map *)balloc(sizeof(Map));
}
BFN_HOT Vec *bvec_alloc(void) {
#if !BRAT_HAS_SANITIZER
    if (!_BRAT_MT() && !barena_depth && _bvec_free) {
        BHead *h = _bvec_free;
        _bvec_free = h->arena_next;
        _bvec_free_n--;
        h->rc = 1;
        h->cap = (bint)sizeof(Vec);
        h->len = 0;
        h->cplen = -1;
        h->ascii = 0;
        h->hash = 0;
        h->arena_next = 0;
        return (Vec *)(h + 1);
    }
#endif
    return (Vec *)balloc(sizeof(Vec));
}
BFN_HOT void bmap_recycle(BHead *h) {
#if !BRAT_HAS_SANITIZER
    if (!_BRAT_MT() && h->cap == (bint)sizeof(Map) && _bmap_free_n < 64) {
        h->arena_next = _bmap_free;
        _bmap_free = h;
        _bmap_free_n++;
        return;
    }
#endif
    free(h);
}
BFN_HOT void bvec_recycle(BHead *h) {
#if !BRAT_HAS_SANITIZER
    if (!_BRAT_MT() && h->cap == (bint)sizeof(Vec) && _bvec_free_n < 64) {
        h->arena_next = _bvec_free;
        _bvec_free = h;
        _bvec_free_n++;
        return;
    }
#endif
    free(h);
}
BFN void _btls_pool_drain(void) {
#if !BRAT_HAS_SANITIZER
    if (_BRAT_MT()) return;
    while (_bmap_free) {
        BHead *h = _bmap_free;
        _bmap_free = h->arena_next;
        free(h);
    }
    _bmap_free_n = 0;
    while (_bvec_free) {
        BHead *h = _bvec_free;
        _bvec_free = h->arena_next;
        free(h);
    }
    _bvec_free_n = 0;
#endif
}
#if (defined(__GNUC__) || defined(__clang__)) && !BRAT_HAS_SANITIZER && !defined(BRAT_RT_SHARED)
__attribute__((destructor)) static void _btls_pool_dtor(void) { _btls_pool_drain(); }
#endif

static void vrc_dec_slow(Val v) {
    if (v.imm) return;
    /* The error being carried belongs to the error channel until verr_take: a
     * frame that drops its locals on the way out must not free the message the
     * top level is about to print. */
    int _pend = verr_pending();
    void *_uw_head = _pend ? vhead_of(vunwind) : 0;
    if (_pend && vhead_of(v) == _uw_head) return;
    if (v.tag == V_STR) {
        char *s = v.as.str;
        if (s) {
            BHead *h = (BHead *)s - 1;
            if (!BIS_IMMORTAL(h) && !BIS_ARENA(h) && BRC_LOAD(h) > 0 && BRC_SUB(h) == 0) free(h);
        }
        return;
    }
    if (v.tag != V_VEC && v.tag != V_STRUCT && v.tag != V_TAG) return;
    BVRelEnt _sbuf[64];
    BVRelEnt *_stk = _sbuf;
    size_t _sp = 0, _cap = 64;
    if (v.tag == V_STRUCT) {
        Map *map = v.as.map;
        if (!map) return;
        BHead *h = (BHead *)map - 1;
        if (BIS_ARENA(h) || BRC_LOAD(h) <= 0 || BRC_SUB(h) != 0) return;
        if (vclosure_env_map(map)) {
            for (bint i = 0; i < map->len; i++)
                if (i != map->bad_key) BVREL_PUSH_(map->vals[i]);
        } else {
            for (bint i = 0; i < map->used; i++) {
                if (!map->shape) BVREL_PUSH_(map->keys[i]);
                BVREL_PUSH_(map->vals[i]);
            }
        }
        BMAP_FREE_ARRS(map);
        bmap_recycle(h);
        if (_sp == 0) return;
    } else if (v.tag == V_VEC) {
        Vec *vec = v.as.vec;
        if (!vec) return;
        BHead *h = (BHead *)vec - 1;
        if (BIS_ARENA(h) || BRC_LOAD(h) <= 0 || BRC_SUB(h) != 0) return;
        for (bint i = 0; i < vec->len; i++) BVREL_PUSH_(vec->items[i]);
        BVEC_FREE_ITEMS(vec);
        bvec_recycle(h);
        if (_sp == 0) return;
    } else {
        _stk[_sp].v = v; _stk[_sp].post = 0; _sp++;
    }
    while (_sp > 0) {
        BVRelEnt _ent = _stk[--_sp];
        Val cur = _ent.v;
        if (_ent.post) {
            if (cur.tag == V_VEC) {
                Vec *vec = cur.as.vec;
                BVEC_FREE_ITEMS(vec);
                bvec_recycle((BHead *)vec - 1);
            } else if (cur.tag == V_STRUCT) {
                Map *map = cur.as.map;
                BMAP_FREE_ARRS(map);
                bmap_recycle((BHead *)map - 1);
            } else if (cur.tag == V_TAG) {
                Vec *args = cur.as.tv;
                BVEC_FREE_ITEMS(args);
                bvec_recycle((BHead *)args - 1);
            }
            continue;
        }
        if (cur.imm) continue;
        if (_pend && vhead_of(cur) == _uw_head) continue;
        switch (cur.tag) {
        case V_VEC: {
            Vec *vec = cur.as.vec;
            if (vec) {
                BHead *h = (BHead *)vec - 1;
                if (!BIS_ARENA(h) && BRC_LOAD(h) > 0 && BRC_SUB(h) == 0) {
                    BVREL_RAW_(cur, 1);
                    for (bint i = 0; i < vec->len; i++) BVREL_PUSH_(vec->items[i]);
                }
            }
            break;
        }
        case V_STRUCT: {
            Map *map = cur.as.map;
            if (map) {
                BHead *h = (BHead *)map - 1;
                if (!BIS_ARENA(h) && BRC_LOAD(h) > 0 && BRC_SUB(h) == 0) {
                    BVREL_RAW_(cur, 1);
                    if (vclosure_env_map(map)) {
                        for (bint i = 0; i < map->len; i++)
                            if (i != map->bad_key) BVREL_PUSH_(map->vals[i]);
                    } else {
                        for (bint i = 0; i < map->used; i++) {
                            if (!map->shape) BVREL_PUSH_(map->keys[i]);
                            BVREL_PUSH_(map->vals[i]);
                        }
                    }
                }
            }
            break;
        }
        case V_TAG: {
            Vec *args = cur.as.tv;
            if (args) {
                BHead *h = (BHead *)args - 1;
                if (!BIS_ARENA(h) && BRC_LOAD(h) > 0 && BRC_SUB(h) == 0) {
                    BVREL_RAW_(cur, 1);
                    for (bint i = 0; i < args->len; i++) BVREL_PUSH_(args->items[i]);
                }
            }
            break;
        }
        default: break;
        }
    }
    if (_stk != _sbuf) free(_stk);
}
#undef BVREL_PUSH_
#undef BVREL_RAW_
BFN_HOT void vrc_dec(Val v) {
    if (v.imm) return;
    if (v.tag != V_STR && v.tag != V_VEC && v.tag != V_STRUCT && v.tag != V_TAG) return;
    vrc_dec_slow(v);
}
BFN void vmem_budget_apply(void) {
    if (BRAT_MEM_BYTES > 0) {
#if defined(_WIN32)
#if SIZE_MAX < UINT64_MAX
        if ((uint64_t)BRAT_MEM_BYTES > (uint64_t)SIZE_MAX) {
            fprintf(stderr, "mem budget: JobObject(%lld) refused\n", (long long)(bint)BRAT_MEM_BYTES);
            exit(1);
        }
#endif
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
        jeli.ProcessMemoryLimit = (SIZE_T)BRAT_MEM_BYTES;
        jeli.JobMemoryLimit = (SIZE_T)BRAT_MEM_BYTES;
        if (!job ||
            !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli) ||
            !AssignProcessToJobObject(job, GetCurrentProcess())) {
            fprintf(stderr, "mem budget: JobObject(%lld) refused\n", (long long)(bint)BRAT_MEM_BYTES);
            exit(1);
        }
#else
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)BRAT_MEM_BYTES;
        rl.rlim_max = (rlim_t)BRAT_MEM_BYTES;
        if (setrlimit(RLIMIT_AS, &rl) != 0) {
            fprintf(stderr, "mem budget: setrlimit(%lld) refused\n", (long long)
                    (bint)BRAT_MEM_BYTES);
            exit(1);
        }
#endif
    }
}
BFN Val vstr_buf(char *o, bint n) {
    if (n >= 0 && (size_t)n <= BRAT_SSO_MAX) {
        Val v = vstr_sso(o, (size_t)n);
        BHead *h = (BHead *)o - 1;
        if (!BIS_ARENA(h) && !BIS_IMMORTAL(h)) free(h);
        return v;
    }
    ((BHead *)o - 1)->len = n;
    Val v; v.tag = V_STR; v.sso = 0; v.imm = BIS_ARENA((BHead *)o - 1) ? 1u : 0u; v.as.str = o;
    return v;
}
BFN Val vstrn(const char *s, bint n) {
    if (n <= 0) return vstr_sso("", 0);
    if ((size_t)n <= BRAT_SSO_MAX) return vstr_sso(s, (size_t)n);
    char *o = (char *)balloc((size_t)n + 1); memcpy(o, s, (size_t)n); o[n] = 0; return vstr_buf(o, n);
}
BFN_HOT Vec *vvec(void) {
    Vec *v = bvec_alloc();
    v->len = 0; v->cap = 2; v->tag = 0;
    v->items = v->inl;
    return v;
}
BFN Vec *vvec_cap(bint cap) {
    if (cap < 0) vfatal_oom();
    size_t bytes = cap > 2 ? bmul_size(cap, sizeof(Val)) : 0;
    Vec *v = bvec_alloc();
    v->len = 0; v->tag = 0;
    if (cap <= 2) {
        v->cap = 2;
        v->items = v->inl;
    } else {
        int ina = BIS_ARENA((BHead *)v - 1);
        v->cap = cap;
        v->items = (Val *)barr_new(bytes, ina, 0);
    }
    return v;
}
BFN Val vstr(const char *s) {
    size_t n = strlen(s);
    if (n <= BRAT_SSO_MAX) return vstr_sso(s, n);
    char *o = (char *)balloc(n + 1);
    memcpy(o, s, n + 1);
    return vstr_buf(o, (bint)n);
}
BFN_HOT Val vvecv(Vec *v) { Val r; r.tag = V_VEC; r.sso = 0; r.imm = (v && (BIS_ARENA((BHead *)v - 1) || BIS_IMMORTAL((BHead *)v - 1))) ? 1u : 0u; r.as.vec = v; return r; }
BFN_HOT void vargs_drop(Vec *v) {
    BHead *h = (BHead *)v - 1;
    if (BIS_IMMORTAL(h)) {
        for (bint i = 0; i < v->len; i++) vrelease1(v->items[i]);
    } else {
        vrc_dec(vvecv(v));
    }
}
BFN_HOT bint vub_div(bint a, bint b) {
    if (!b) { vraise("div by zero"); return 0; }
    if (a == (-9223372036854775807LL - 1LL) && b == -1) return (-9223372036854775807LL - 1LL);
    bint q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}
BFN_HOT bint vub_mod(bint a, bint b) {
    if (!b) { vraise("div by zero"); return 0; }
    if (a == (-9223372036854775807LL - 1LL) && b == -1) return 0;
    bint r = a % b;
    if (r && ((r < 0) != (b < 0))) r += b;
    return r;
}
BFN_HOT double vub_fdiv(double a, double b) {
    if (b == 0.0) { vraise("div by zero"); return 0.0; }
    return a / b;
}
BFN_HOT double vub_fmod(double a, double b) {
    if (b == 0.0) { vraise("div by zero"); return 0.0; }
    double r = fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    return r;
}
BFN_HOT int vub_fcmp(double a, double b, int op) {
    return op == 0 ? (a < b) : op == 1 ? (a <= b) : op == 2 ? (a > b) : (a >= b);
}
/* Container scaffolding only: elements may be aliased or static, so dropping
   them here would be a UAF. */
BFN void vdrop(Val v) {
    if (v.imm) return;
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
            BVEC_FREE_ITEMS(vec);
            bvec_recycle(h);
        }
        return;
    }
    case V_STRUCT: {
        Map *map = v.as.map;
        if (map && !BIS_ARENA((BHead *)map - 1)) {
            BHead *h = (BHead *)map - 1;
            if (vclosure_env_map(map)) {
                for (bint i = 0; i < map->len; i++)
                    if (i != map->bad_key) vrelease1(map->vals[i]);
            } else {
                for (bint i = 0; i < map->used; i++) {
                    if (!map->shape) vrelease1(map->keys[i]);
                    vrelease1(map->vals[i]);
                }
            }
            BMAP_FREE_ARRS(map);
            bmap_recycle(h);
        }
        return;
    }
    case V_TAG: {
        Vec *args = v.as.tv;
        if (args && !BIS_ARENA((BHead *)args - 1)) {
            BHead *h = (BHead *)args - 1;
            for (bint i = 0; i < args->len; i++) vrelease1(args->items[i]);
            BVEC_FREE_ITEMS(args);
            bvec_recycle(h);
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
BFN_HOT void vrelease(Val v) {
    if (v.imm) return;
    switch (v.tag) {
    case V_STR:
    case V_VEC:
    case V_STRUCT:
    case V_TAG:
        vrc_dec_slow(v);
        return;
    default:
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
BFN_HOT void vdrop_temp(Val v) {
    if (v.imm) return;
    if (v.tag == V_VEC || v.tag == V_TAG || v.tag == V_STRUCT || v.tag == V_STR) vrc_dec_slow(v);
}
/* The receiver of a method call that nobody else names. A few methods hand the
 * receiver back as their result (`push`, `sort`), so it is released only when the
 * result is a different object: releasing it would free what the result reads. */
BFN void *vhead_of(Val v) {
    switch (v.tag) {
    case V_STR:   return (!v.sso && v.as.str) ? (void *)((BHead *)v.as.str - 1) : 0;
    case V_VEC:   return v.as.vec ? (void *)((BHead *)v.as.vec - 1) : 0;
    case V_STRUCT: return v.as.map ? (void *)((BHead *)v.as.map - 1) : 0;
    case V_TAG:   return v.as.tv ? (void *)((BHead *)v.as.tv - 1) : 0;
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
    else if (recv.tag == V_TAG) items = recv.as.tv;
    if (items) {
        for (bint i = 0; i < items->len; i++)
            if (vhead_of(items->items[i]) == h) { items->items[i] = vnil(); break; }
    } else if (recv.tag == V_STRUCT && recv.as.map) {
        Map *m = recv.as.map;
        for (bint i = 0; i < m->used; i++)
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
    if (r.tag == V_MOVED) vraise("use of moved value");
    src->tag = V_MOVED;
    return r;
}
/* Every element of a `vec[i64]` parameter, before a raw body reads one. The
 * Dynamic callers walk the same vector, so a non-int element is a legal
 * program there -- the wrapper's job is to send it down the general path, not to
 * assume what the annotation promised. */
BFN bint vvec_all_int(Vec *v) {
    for (bint i = 0; i < v->len; i++)
        if (v->items[i].tag != V_INT) return 0;
    return 1;
}

BFN_HOT Map *vmap(const char *type) {
    Map *m = bmap_alloc();
    m->len = 0; m->used = 0; m->dead = 0; m->cap = 2;
    m->keys = m->inl_k;
    m->vals = m->inl_v;
    m->hashes = 0;
    m->slots = 0;
    m->shape = 0;
    m->type = type;
    m->idx_cap = 0; m->idx_used = 0; m->bad_key = 0; m->inline_vals = 0;
    return m;
}
BFN Map *vclosure_env_new(bint n) {
    if (n < 0 || (uint64_t)n > (uint64_t)(SIZE_MAX / sizeof(Val))) {
        vraise("invalid closure environment size");
        return 0;
    }
    Map *m = vmap(_vclosure_env_type);
    m->type = _vclosure_env_type;
    m->keys = 0; m->hashes = 0; m->slots = 0; m->shape = 0;
    m->len = n; m->used = n; m->dead = 0; m->cap = n > 2 ? n : 2;
    m->idx_cap = 0; m->idx_used = 0; m->bad_key = -1;
    if (n <= 2) {
        m->vals = m->inl_v;
        m->inline_vals = 1;
    } else {
        m->vals = (Val *)barr_new(bmul_size(n, sizeof(Val)),
                                  BIS_ARENA((BHead *)m - 1), 0);
        m->inline_vals = 0;
    }
    for (bint i = 0; i < n; i++) m->vals[i] = vnil();
    return m;
}
static BTLS Val _vclosure_bad_slot;
BFN Val *vclosure_env_at(Map *m, bint slot) {
    if (!vclosure_env_map(m) || slot < 0 || slot >= m->len) {
        vraise("invalid closure capture slot");
        _vclosure_bad_slot = vnil();
        return &_vclosure_bad_slot;
    }
    return &m->vals[slot];
}
BFN void vclosure_env_set_weak(Map *m, bint slot, Val value) {
    Val *p = vclosure_env_at(m, slot);
    if (p && vclosure_env_map(m)) {
        *p = value;
        m->bad_key = slot;
    }
}
BFN void vclosure_env_set_weak_capture(Val closure, bint slot) {
    if (closure.tag != V_TAG || !closure.as.tv || closure.as.tv->len != 2 ||
        closure.as.tv->items[1].tag != V_STRUCT) {
        vraise("invalid recursive closure capture");
        return;
    }
    vclosure_env_set_weak(closure.as.tv->items[1].as.map, slot, closure);
}

static bmutex_t _vshp_mu = BMUTEX_INIT;
BFN_HOT Map *vmap_shaped(BShape *shp, const char *type, const char *const *fnames, bint n) {
    int rdy;
#if defined(__GNUC__) || defined(__clang__)
    rdy = __atomic_load_n(&shp->ready, __ATOMIC_ACQUIRE);
#else
    rdy = shp->ready;
#endif
    if (!rdy) {
        BMUTEX_LOCK(&_vshp_mu);
        if (!shp->ready) {
            Val *ks = (Val *)malloc(bmul_size(n > 0 ? n : 1, sizeof(Val)));
            if (!ks) vfatal_oom();
            for (bint i = 0; i < n; i++) ks[i] = vlit(fnames[i]);
            shp->keys = ks;
            shp->type = type;
            shp->len = n;
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&shp->ready, 1, __ATOMIC_RELEASE);
#else
            shp->ready = 1;
#endif
        }
        BMUTEX_UNLOCK(&_vshp_mu);
    }
    size_t vbytes = (n <= 2) ? 0 : bmul_size(n, sizeof(Val));
    if (vbytes > SIZE_MAX - sizeof(Map)) vfatal_oom();
    Map *m;
    if (n <= 2) {
        m = bmap_alloc();
        m->inl_v[0] = vnil();
        m->inl_v[1] = vnil();
        m->vals = m->inl_v;
    } else {
        m = (Map *)balloc(sizeof(Map) + vbytes);
        m->vals = (Val *)(m + 1);
    }
    m->keys = shp->keys;
    m->len = n;
    m->used = n;
    m->dead = 0;
    m->cap = n;
    m->type = type;
    m->slots = 0;
    m->hashes = 0;
    m->shape = shp;
    m->idx_cap = 0;
    m->idx_used = 0;
    m->bad_key = 0;
    m->inline_vals = 1;
    return m;
}

BFN_HOT uint64_t vcstr_hash(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)n;
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 1099511628211ULL;
        p += 8;
        n -= 8;
    }
    while (n > 0) {
        h = (h ^ (uint64_t)*p++) * 1099511628211ULL;
        n--;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h ? h : 1ULL;
}

BFN_HOT uint64_t vkey_hash(Val k) {
    uint64_t h = 1469598103934665603ULL;
    switch (k.tag) {
    case V_STR:
        if (!k.sso && k.as.str) {
            BHead *sh = (BHead *)k.as.str - 1;
#if defined(__GNUC__) || defined(__clang__)
            uint64_t ch = __atomic_load_n(&sh->hash, __ATOMIC_RELAXED);
#else
            uint64_t ch = sh->hash;
#endif
            if (ch) return ch;
            ch = vcstr_hash(k.as.str, (size_t)sh->len);
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&sh->hash, ch, __ATOMIC_RELAXED);
#else
            sh->hash = ch;
#endif
            return ch;
        }
        return vcstr_hash(vstr_c(&k), vstr_len_fast(&k));
    case V_INT:
        h ^= (uint64_t)k.as.ival; h *= 1099511628211ULL;
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; return h;
    case V_FLOAT: {
        double d = k.as.fval == 0.0 ? 0.0 : k.as.fval;
        /* One NaN is one key. IEEE says NaN != NaN, so hashing the bit pattern
           would make `m[nan] = 1` add a fresh entry on every store, and
           `m.has(nan)` answer false a line after the store that made it. Java
           normalises -0.0 and integral floats to the canonical key form. */
        if (d != d) { h ^= 0x7ff8deadbeefULL; h *= 1099511628211ULL; h ^= h >> 33; return h; }
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        h ^= bits; h *= 1099511628211ULL; h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; return h;
    }
    case V_BOOL:
        h ^= (uint64_t)(k.as.b ? 1 : 0); h *= 1099511628211ULL; h ^= h >> 33; return h;
    case V_NIL:
        h ^= 0x9e3779b97f4a7c15ULL; return h;
    case V_TAG: {
        const char *ts = k.as.tv->tag;
        h = vcstr_hash(ts, strlen(ts)) ^ ((uint64_t)k.as.tv->len * 0x9e3779b97f4a7c15ULL);
        h ^= h >> 33; return h;
    }
    default:
        h ^= (uint64_t)k.tag; h *= 1099511628211ULL; return h;
    }
}

BFN_HOT void map_slot_put_raw(Map *m, int32_t pos, uint64_t h) {
    uint32_t tag = (uint32_t)(h >> 32) | 1u;
    bint mask = m->idx_cap - 1;
    bint slot = (bint)(h & (uint64_t)mask);
    bint dist = 0;
    BMapSlot cur;
    cur.tag = tag;
    cur.pos = pos;
    for (;;) {
        BMapSlot ex = m->slots[slot];
        if (ex.tag == 0) {
            m->slots[slot] = cur;
            m->idx_used++;
            return;
        }
        bint ex_home = (bint)(m->hashes[ex.pos] & (uint64_t)mask);
        bint ex_dist = (slot - ex_home) & mask;
        if (ex_dist < dist) {
            m->slots[slot] = cur;
            cur = ex;
            dist = ex_dist;
        }
        slot = (slot + 1) & mask;
        dist++;
    }
}

BFN void map_idx_rebuild(Map *m) {
    bint want = m->idx_cap >= 16 ? m->idx_cap : 16;
    while ((uint64_t)(m->len + 1) * 4ULL >= (uint64_t)want * 3ULL) {
        want = bnext_cap(want, sizeof(BMapSlot));
    }
    int ina = BIS_ARENA((BHead *)m - 1);
    if (!m->hashes) {
        bint hcap = m->cap > m->used ? m->cap : (m->used > 4 ? m->used : 4);
        m->hashes = (uint64_t *)barr_new(bmul_size(hcap, sizeof(uint64_t)), ina, 0);
        for (bint i = 0; i < m->used; i++)
            if (m->keys[i].tag != V_MOVED) m->hashes[i] = vkey_hash(m->keys[i]);
    }
    if (want != m->idx_cap || m->slots == 0) {
        BMapSlot *ns = (BMapSlot *)barr_new(bmul_size(want, sizeof(BMapSlot)), ina, 1);
        if (m->slots && !ina) free(m->slots);
        m->slots = ns;
        m->idx_cap = want;
    } else {
        memset(m->slots, 0, bmul_size(m->idx_cap, sizeof(BMapSlot)));
    }
    m->idx_used = 0;
    for (bint i = 0; i < m->used; i++)
        if (m->keys[i].tag != V_MOVED) map_slot_put_raw(m, (int32_t)i, m->hashes[i]);
}

BFN void map_compact(Map *m) {
    if (!m || !m->dead) return;
    bint dst = 0;
    for (bint i = 0; i < m->used; i++) {
        if (m->keys[i].tag != V_MOVED) {
            if (dst != i) {
                m->keys[dst] = m->keys[i];
                m->vals[dst] = m->vals[i];
                if (m->hashes) m->hashes[dst] = m->hashes[i];
            }
            dst++;
        }
    }
    m->used = dst;
    m->len = dst;
    m->dead = 0;
    if (m->slots) map_idx_rebuild(m);
}

BFN void map_unshape(Map *m) {
    if (!m->shape && !m->inline_vals) return;
    int ina = BIS_ARENA((BHead *)m - 1);
    bint cap = m->len > 4 ? bnext_cap(m->len, sizeof(Val)) : 4;
    Val *nk = (Val *)barr_new(bmul_size(cap, sizeof(Val)), ina, 0);
    Val *nv = (Val *)barr_new(bmul_size(cap, sizeof(Val)), ina, 0);
    uint64_t *nh = (uint64_t *)barr_new(bmul_size(cap, sizeof(uint64_t)), ina, 0);
    for (bint i = 0; i < m->len; i++) {
        nk[i] = vrc_inc(m->keys[i]);
        nv[i] = m->vals[i];
        nh[i] = vkey_hash(nk[i]);
    }
    if (!m->shape && m->keys && m->keys != m->inl_k && !ina) free(m->keys);
    if (!m->inline_vals && m->vals && m->vals != m->inl_v && !ina) free(m->vals);
    if (m->hashes && m->hashes != m->inl_h && !ina) free(m->hashes);
    m->keys = nk;
    m->vals = nv;
    m->hashes = nh;
    m->used = m->len;
    m->dead = 0;
    m->cap = cap;
    m->shape = 0;
    m->inline_vals = 0;
    if (m->len > 4) map_idx_rebuild(m);
}

/* Equality as a map sees it: every NaN is the same key (see vkey_hash), and
   everything else is `veq`. `veq` itself keeps IEEE: `nan == nan` is false. */
BFN_HOT bint vmap_key_same(Val a, Val b) {
    if (a.tag != b.tag) return 0;
    if (a.sso && b.sso) return memcmp(&a, &b, sizeof(Val)) == 0;
    if (a.tag == V_INT) return a.as.ival == b.as.ival;
    if (a.tag == V_BOOL) return a.as.b == b.as.b;
    if (a.tag == V_FLOAT)
        return (a.as.fval != a.as.fval && b.as.fval != b.as.fval) || a.as.fval == b.as.fval;
    return veq(a, b);
}
BFN_HOT bint map_find_h(Map *m, Val k, uint64_t h) {
    if (m->shape || !m->slots) {
        if (m->hashes) {
            for (bint i = 0; i < m->used; i++)
                if (m->keys[i].tag != V_MOVED && m->hashes[i] == h && vmap_key_same(m->keys[i], k)) return i;
        } else {
            for (bint i = 0; i < m->used; i++)
                if (m->keys[i].tag != V_MOVED && vmap_key_same(m->keys[i], k)) return i;
        }
        return -1;
    }
    uint32_t tag = (uint32_t)(h >> 32) | 1u;
    bint mask = m->idx_cap - 1;
    bint slot = (bint)(h & (uint64_t)mask);
    for (bint dist = 0; dist < m->idx_cap; dist++) {
        BMapSlot ex = m->slots[slot];
        if (ex.tag == 0) return -1;
        bint ex_home = (bint)(m->hashes[ex.pos] & (uint64_t)mask);
        bint ex_dist = (slot - ex_home) & mask;
        if (ex_dist < dist) return -1;
        if (ex.tag == tag && m->hashes[ex.pos] == h && vmap_key_same(m->keys[ex.pos], k))
            return ex.pos;
        slot = (slot + 1) & mask;
    }
    return -1;
}
BFN_HOT bint map_find(Map *m, Val k) {
    if ((m->shape || !m->slots) && !m->hashes) {
        for (bint i = 0; i < m->used; i++)
            if (m->keys[i].tag != V_MOVED && vmap_key_same(m->keys[i], k)) return i;
        return -1;
    }
    return map_find_h(m, k, vkey_hash(k));
}
BFN_HOT bint map_find_cstr(Map *m, const char *k, size_t kl) {
    if (m->shape || !m->slots) {
        char c0 = k[0];
        for (bint i = 0; i < m->used; i++) {
            Val kv = m->keys[i];
            if (kv.tag == V_STR) {
                const char *ks = vstr_c(&kv);
                if (ks == k || (vstr_len_fast(&kv) == kl && ks[0] == c0 && memcmp(ks, k, kl) == 0))
                    return i;
            }
        }
        return -1;
    }
    uint64_t h = vcstr_hash(k, kl);
    uint32_t tag = (uint32_t)(h >> 32) | 1u;
    bint mask = m->idx_cap - 1;
    bint slot = (bint)(h & (uint64_t)mask);
    for (bint dist = 0; dist < m->idx_cap; dist++) {
        BMapSlot ex = m->slots[slot];
        if (ex.tag == 0) return -1;
        bint ex_home = (bint)(m->hashes[ex.pos] & (uint64_t)mask);
        bint ex_dist = (slot - ex_home) & mask;
        if (ex_dist < dist) return -1;
        if (ex.tag == tag && m->hashes[ex.pos] == h) {
            Val kv = m->keys[ex.pos];
            if (kv.tag == V_STR) {
                const char *ks = vstr_c(&kv);
                if (ks == k || (vstr_len_fast(&kv) == kl && memcmp(ks, k, kl) == 0))
                    return ex.pos;
            }
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}
BFN void map_idx_put(Map *m, bint pos, uint64_t h) {
    if (!m->slots) {
        if (m->len > 4) map_idx_rebuild(m);
        return;
    }
    if ((m->idx_used + 1) * 4 >= m->idx_cap * 3) {
        map_idx_rebuild(m);
        return;
    }
    map_slot_put_raw(m, (int32_t)pos, h);
}
BFN Val vstruct(Map *m) { Val r; r.tag = V_STRUCT; r.sso = 0; r.imm = (m && BIS_ARENA((BHead *)m - 1)) ? 1u : 0u; r.as.map = m; return r; }


BFN void vec_push(Vec *v, Val x) {
    BRAT_CTR_LOCK_OBJ(v);
    if (v->len >= v->cap) {
        bint oldcap = v->cap;
        int ina = BIS_ARENA((BHead *)v - 1);
        bint ncap = bnext_cap(oldcap, sizeof(Val));
        size_t nbytes = bmul_size(ncap, sizeof(Val));
        v->cap = ncap;
        if (v->items == v->inl) {
            Val *nb = (Val *)barr_new(nbytes, ina, 0);
            if (oldcap > 0) memcpy(nb, v->inl, bmul_size(oldcap, sizeof(Val)));
            v->items = nb;
        } else {
            v->items = (Val *)barr_grow(v->items, ina,
                                        bmul_size(oldcap, sizeof(Val)), nbytes);
        }
    }
    v->items[v->len++] = x;
    BRAT_CTR_UNLOCK_OBJ(v);
}
/* A tag's arguments outlive the block that built them when the tag is returned
 * (Ok/Err from an arena), so an arena-built argument list is moved to the heap.
 * Its items are copied shallowly; any arena values inside are the escape rules' job. */
BFN Vec *vtag_args_heap(Vec *a) {
    if (!a) {
        int saved = barena_depth;
        barena_depth = 0;
        Vec *d = (Vec *)balloc(sizeof(Vec));
        d->len = 0; d->cap = 0; d->items = 0; d->tag = 0;
        barena_depth = saved;
        return d;
    }
    if (!BIS_ARENA((BHead *)a - 1) && !a->tag) return a;
    int saved = barena_depth;
    barena_depth = 0;
    Vec *d = vvec();
    if (BIS_ARENA((BHead *)a - 1)) {
        for (bint i = 0; i < a->len; i++) vec_push(d, a->items[i]);
    } else {
        for (bint i = 0; i < a->len; i++) vec_push(d, vrc_inc(a->items[i]));
    }
    barena_depth = saved;
    return d;
}
static char *_vtag_intern(const char *tag);
BFN Val vtagv_lit(const char *tag, Vec *args) {
    Vec *tv = vtag_args_heap(args);
    tv->tag = _vtag_intern(tag);
    Val r; r.tag = V_TAG; r.sso = 0; r.imm = 0; r.as.tv = tv;
    return r;
}
/* Intern table for tag names (Ok, Err, user enum variants): immortal.
 * Two threads can build the same tag at the same time (`spawn` runs bodies
 * concurrently), so the table has one mutex and a length published with
 * release: a reader either misses a slot or sees it whole, and never reads past
 * the end of the array. */
static struct { const char *src; char *ptr; } _vtag_cache[512];
static int _vtag_n = 0;
static bmutex_t _vtag_mu = BMUTEX_INIT;
static BTLS struct { const char *lit; char *ptr; } _vtag_tls[64];
static int _vtag_count(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&_vtag_n, __ATOMIC_ACQUIRE);
#else
    return _vtag_n;
#endif
}
static char *_vtag_intern(const char *tag) {
    unsigned slot = (unsigned)(((uintptr_t)tag >> 3) & 63u);
    if (_vtag_tls[slot].lit == tag && _vtag_tls[slot].ptr) return _vtag_tls[slot].ptr;
    int n = _vtag_count();
    for (int i = 0; i < n; i++) {
        if (_vtag_cache[i].src == tag || strcmp(_vtag_cache[i].src, tag) == 0) {
            _vtag_tls[slot].lit = tag;
            _vtag_tls[slot].ptr = _vtag_cache[i].ptr;
            return _vtag_cache[i].ptr;
        }
    }
    BMUTEX_LOCK(&_vtag_mu);
    n = _vtag_n;                       /* another thread may have added it */
    for (int i = 0; i < n; i++) {
        if (strcmp(_vtag_cache[i].src, tag) == 0) {
            char *hit = _vtag_cache[i].ptr;
            BMUTEX_UNLOCK(&_vtag_mu);
            _vtag_tls[slot].lit = tag;
            _vtag_tls[slot].ptr = hit;
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
    BMUTEX_UNLOCK(&_vtag_mu);
    _vtag_tls[slot].lit = tag;
    _vtag_tls[slot].ptr = p;
    return p;
}
BFN Map *vmap_shallow_heap(Map *src) {
    if (!src || !BIS_ARENA((BHead *)src - 1)) return src;
    if (vclosure_env_map(src)) {
        int saved = barena_depth;
        barena_depth = 0;
        bint n = src->len;
        Map *dst = vclosure_env_new(n);
        dst->bad_key = src->bad_key;
        for (bint i = 0; i < n; i++) {
            dst->vals[i] = src->vals[i];
            src->vals[i] = vnil();
        }
        src->len = 0; src->used = 0;
        barena_depth = saved;
        return dst;
    }
    if (src->dead) map_compact(src);
    int saved = barena_depth;
    barena_depth = 0;
    if (src->shape) {
        bint n = src->len;
        size_t vbytes = (n <= 2) ? 0 : bmul_size(n, sizeof(Val));
        if (vbytes > SIZE_MAX - sizeof(Map)) vfatal_oom();
        Map *dst = (n <= 2)
            ? (Map *)balloc(sizeof(Map))
            : (Map *)balloc(sizeof(Map) + vbytes);
        dst->keys = src->keys;
        dst->vals = (n <= 2) ? dst->inl_v : (Val *)(dst + 1);
        dst->len = n; dst->used = n; dst->dead = 0; dst->cap = n;
        dst->type = src->type;
        dst->slots = 0; dst->hashes = 0; dst->shape = src->shape;
        dst->idx_cap = 0; dst->idx_used = 0; dst->bad_key = src->bad_key; dst->inline_vals = 1;
        for (bint i = 0; i < n; i++) {
            dst->vals[i] = src->vals[i];
            src->vals[i] = vnil();
        }
        src->len = 0; src->used = 0;
        barena_depth = saved;
        return dst;
    }
    Map *dst = (Map *)balloc(sizeof(Map));
    if (src->len <= 2 && !src->slots) {
        dst->len = src->len; dst->used = src->len; dst->dead = 0; dst->cap = 2;
        dst->keys = dst->inl_k;
        dst->vals = dst->inl_v;
        dst->hashes = dst->inl_h;
        dst->slots = 0;
        dst->shape = 0;
        dst->type = src->type;
        dst->idx_cap = 0; dst->idx_used = 0; dst->bad_key = src->bad_key; dst->inline_vals = 0;
        for (bint i = 0; i < src->len; i++) {
            dst->keys[i] = src->keys[i];
            dst->vals[i] = src->vals[i];
            dst->hashes[i] = src->hashes ? src->hashes[i] : vkey_hash(src->keys[i]);
            src->keys[i] = vnil();
            src->vals[i] = vnil();
        }
        src->len = 0; src->used = 0;
        barena_depth = saved;
        return dst;
    }
    bint cap = src->cap > 4 ? src->cap : 4;
    dst->len = src->len; dst->used = src->len; dst->dead = 0; dst->cap = cap;
    dst->keys = (Val *)barr_new(bmul_size(cap, sizeof(Val)), 0, 0);
    dst->vals = (Val *)barr_new(bmul_size(cap, sizeof(Val)), 0, 0);
    dst->hashes = (uint64_t *)barr_new(bmul_size(cap, sizeof(uint64_t)), 0, 0);
    dst->slots = src->slots ? (BMapSlot *)barr_new(bmul_size(src->idx_cap, sizeof(BMapSlot)), 0, 1) : 0;
    dst->shape = 0;
    dst->type = src->type;
    dst->idx_cap = src->idx_cap; dst->idx_used = src->idx_used; dst->bad_key = src->bad_key; dst->inline_vals = 0;
    for (bint i = 0; i < src->len; i++) {
        dst->keys[i] = src->keys[i];
        dst->vals[i] = src->vals[i];
        dst->hashes[i] = src->hashes ? src->hashes[i] : vkey_hash(src->keys[i]);
        src->keys[i] = vnil();
        src->vals[i] = vnil();
    }
    if (src->slots && src->idx_cap > 0)
        memcpy(dst->slots, src->slots, sizeof(BMapSlot) * (size_t)src->idx_cap);
    src->len = 0; src->used = 0;
    barena_depth = saved;
    return dst;
}
BFN Val vtagv(const char *tag, Vec *args) {
    Vec *tv = vtag_args_heap(args);
    if (strcmp(tag, "closure") == 0 && tv->len == 2 && tv->items[1].tag == V_STRUCT) {
        tv->items[1].as.map = vmap_shallow_heap(tv->items[1].as.map);
        tv->items[1].imm = 0;
    }
    tv->tag = _vtag_intern(tag);
    Val r; r.tag = V_TAG; r.sso = 0; r.imm = 0; r.as.tv = tv;
    return r;
}

#define BCLO0_STATIC_MAX 1024
static BStackVec _vclo0_tab[BCLO0_STATIC_MAX];
static unsigned char _vclo0_ready[BCLO0_STATIC_MAX];
static bmutex_t _vclo0_mu = BMUTEX_INIT;
BFN_HOT Val vclosure0(bint code) {
    if ((uint64_t)code < (uint64_t)BCLO0_STATIC_MAX) {
        BStackVec *slot = &_vclo0_tab[code];
        unsigned char rdy;
#if defined(__GNUC__) || defined(__clang__)
        rdy = __atomic_load_n(&_vclo0_ready[code], __ATOMIC_ACQUIRE);
#else
        rdy = _vclo0_ready[code];
#endif
        if (!rdy) {
            BMUTEX_LOCK(&_vclo0_mu);
            if (!_vclo0_ready[code]) {
                slot->head.rc = -1;
                slot->head.cap = 0;
                slot->head.len = 0;
                slot->head.cplen = 0;
                slot->head.ascii = 0;
                slot->head.hash = 0;
                slot->head.arena_next = 0;
                slot->vec.items = slot->vec.inl;
                slot->vec.len = 2;
                slot->vec.cap = 2;
                slot->vec.tag = (char *)"closure";
                slot->vec.inl[0] = vint(code);
                slot->vec.inl[1] = vnil();
#if defined(__GNUC__) || defined(__clang__)
                __atomic_store_n(&_vclo0_ready[code], 1u, __ATOMIC_RELEASE);
#else
                _vclo0_ready[code] = 1u;
#endif
            }
            BMUTEX_UNLOCK(&_vclo0_mu);
        }
        Val r;
        r.tag = V_TAG;
        r.sso = 0;
        r.imm = 1u;
        r.as.tv = &slot->vec;
        return r;
    }
    Vec *cb = vvec();
    vec_push(cb, vint(code));
    vec_push(cb, vnil());
    return vtagv("closure", cb);
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
   here instead of returning an error value. */
BFN bint vmap_bad_seen(Map *m) {
    if (m->bad_key) { vrc_dec(vstruct(m)); return 1; }
    return 0;
}
BFN Val vmap_bad_key_err(void) {
    Vec *e = vvec();
    vec_push(e, vstr("unhashable map key"));
    return vtagv("err", e);
}
BFN void map_set_v(Map *m, Val k, Val v) {
    if (vmap_key_bad(k)) { vrc_dec(k); vrc_dec(v); m->bad_key = 1; return; }  /* the caller answers */
    BRAT_CTR_LOCK_OBJ(m);
    if (!m->slots && !m->hashes && !m->shape && !m->inline_vals && m->used < m->cap) {
        for (bint i = 0; i < m->used; i++) {
            if (m->keys[i].tag != V_MOVED && vmap_key_same(m->keys[i], k)) {
                Val old = m->vals[i];
                m->vals[i] = v;
                BRAT_CTR_UNLOCK_OBJ(m);
                vrelease1(old);
                vrelease1(k);
                return;
            }
        }
        bint pos = m->used;
        m->keys[pos] = k;
        m->vals[pos] = v;
        m->used++;
        m->len++;
        BRAT_CTR_UNLOCK_OBJ(m);
        return;
    }
    uint64_t h = vkey_hash(k);
    bint at = map_find_h(m, k, h);
    if (at >= 0) {
        Val old = m->vals[at];
        m->vals[at] = v;
        BRAT_CTR_UNLOCK_OBJ(m);
        vrelease1(old);
        vrelease1(k);
        return;
    }
    if (m->shape || m->inline_vals) map_unshape(m);
    if (!m->hashes && m->keys == m->inl_k) {
        for (bint j = 0; j < m->used; j++)
            m->inl_h[j] = (m->keys[j].tag != V_MOVED) ? vkey_hash(m->keys[j]) : 0;
        m->hashes = m->inl_h;
    } else if (!m->hashes) {
        int inm0 = BIS_ARENA((BHead *)m - 1);
        m->hashes = (uint64_t *)barr_new(bmul_size(m->cap, sizeof(uint64_t)), inm0, 0);
        for (bint j = 0; j < m->used; j++)
            m->hashes[j] = (m->keys[j].tag != V_MOVED) ? vkey_hash(m->keys[j]) : 0;
    }
    if (m->used >= m->cap) {
        if (m->dead > 0 && m->len * 2 <= m->cap) {
            map_compact(m);
        } else {
            bint oldcap = m->cap;
            int inm = BIS_ARENA((BHead *)m - 1);
            bint ncap = bnext_cap(oldcap, sizeof(Val));
            m->cap = ncap;
            if (m->keys == m->inl_k) {
                Val *nk = (Val *)barr_new(bmul_size(ncap, sizeof(Val)), inm, 0);
                Val *nv = (Val *)barr_new(bmul_size(ncap, sizeof(Val)), inm, 0);
                uint64_t *nh = (uint64_t *)barr_new(bmul_size(ncap, sizeof(uint64_t)), inm, 0);
                if (oldcap > 0) {
                    memcpy(nk, m->inl_k, bmul_size(oldcap, sizeof(Val)));
                    memcpy(nv, m->inl_v, bmul_size(oldcap, sizeof(Val)));
                    memcpy(nh, m->inl_h, bmul_size(oldcap, sizeof(uint64_t)));
                }
                m->keys = nk;
                m->vals = nv;
                m->hashes = nh;
            } else {
                m->keys = (Val *)barr_grow(m->keys, inm, bmul_size(oldcap, sizeof(Val)), bmul_size(ncap, sizeof(Val)));
                m->vals = (Val *)barr_grow(m->vals, inm, bmul_size(oldcap, sizeof(Val)), bmul_size(ncap, sizeof(Val)));
                m->hashes = (uint64_t *)barr_grow(m->hashes, inm, bmul_size(oldcap, sizeof(uint64_t)), bmul_size(ncap, sizeof(uint64_t)));
            }
        }
    }
    bint pos = m->used;
    m->keys[pos] = k; m->vals[pos] = v; m->hashes[pos] = h;
    m->used++; m->len++;
    map_idx_put(m, pos, h);
    BRAT_CTR_UNLOCK_OBJ(m);
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
BFN_HOT Val *map_get(Map *m, const char *k) {
    bint at = map_find_cstr(m, k, strlen(k));
    return at >= 0 ? &m->vals[at] : 0;
}
BFN_HOT Val *map_get_ic(Map *m, const char *k, BFieldIC *ic) {
    if (ic) {
        const BShape *sh = m->shape;
        if (sh) {
            if (sh == ic->shapes[0]) { bint sl = ic->slots[0]; return sl >= 0 ? &m->vals[sl] : 0; }
            if (sh == ic->shapes[1]) { bint sl = ic->slots[1]; return sl >= 0 ? &m->vals[sl] : 0; }
            if (sh == ic->shapes[2]) { bint sl = ic->slots[2]; return sl >= 0 ? &m->vals[sl] : 0; }
            if (sh == ic->shapes[3]) { bint sl = ic->slots[3]; return sl >= 0 ? &m->vals[sl] : 0; }
        } else {
            bint sl = ic->slots[0];
            if (!ic->shapes[0] && sl >= 0 && sl < m->used) {
                Val kv = m->keys[sl];
                if (kv.tag == V_STR) {
                    const char *ks = vstr_c(&kv);
                    if (ks == k || (ks[0] == k[0] && strcmp(ks, k) == 0))
                        return &m->vals[sl];
                }
            }
        }
    }
    bint at = map_find_cstr(m, k, strlen(k));
    if (at < 0) {
        if (ic && m->shape) {
            unsigned w = (unsigned)(ic->next & 3u);
            ic->shapes[w] = m->shape;
            ic->slots[w] = -1;
            ic->next = (unsigned char)((w + 1u) & 3u);
        }
        return 0;
    }
    if (ic) {
        if (m->shape) {
            unsigned w = (unsigned)(ic->next & 3u);
            ic->shapes[w] = m->shape;
            ic->slots[w] = at;
            ic->next = (unsigned char)((w + 1u) & 3u);
        } else {
            ic->shapes[0] = 0;
            ic->slots[0] = at;
        }
    }
    return &m->vals[at];
}
BFN bint map_del_v(Map *m, Val k) {
    BRAT_CTR_LOCK_OBJ(m);
    uint64_t h = vkey_hash(k);
    bint at = map_find_h(m, k, h);
    if (at < 0) { BRAT_CTR_UNLOCK_OBJ(m); return 0; }
    if (m->shape || m->inline_vals) map_unshape(m);
    Val old_k = m->keys[at];
    Val old_v = m->vals[at];
    if (m->slots) {
        bint mask = m->idx_cap - 1;
        bint slot = (bint)(h & (uint64_t)mask);
        for (bint dist = 0; dist < m->idx_cap; dist++) {
            BMapSlot ex = m->slots[slot];
            if (ex.tag == 0) break;
            if (ex.pos == (int32_t)at) {
                for (;;) {
                    bint nx_slot = (slot + 1) & mask;
                    BMapSlot nx = m->slots[nx_slot];
                    if (nx.tag == 0) { m->slots[slot].tag = 0; m->slots[slot].pos = 0; break; }
                    bint nx_home = (bint)(m->hashes[nx.pos] & (uint64_t)mask);
                    if (((nx_slot - nx_home) & mask) == 0) { m->slots[slot].tag = 0; m->slots[slot].pos = 0; break; }
                    m->slots[slot] = nx;
                    slot = nx_slot;
                }
                m->idx_used--;
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    if (at == m->used - 1) {
        m->used--;
        m->len--;
        while (m->used > 0 && m->keys[m->used - 1].tag == V_MOVED) {
            m->used--;
            m->dead--;
        }
    } else {
        m->keys[at].tag = V_MOVED;
        m->keys[at].sso = 0;
        m->keys[at].imm = 0;
        m->keys[at].as.ival = 0;
        m->vals[at] = vnil();
        m->len--;
        m->dead++;
        if (m->dead > m->len && m->dead >= 8) map_compact(m);
    }
    BRAT_CTR_UNLOCK_OBJ(m);
    vrelease1(old_k);
    vrelease1(old_v);
    return 1;
}

BFN char *vkey(Val k) {
    if (k.tag == V_STR) return strdup(vstr_c(&k));
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
BFN_HOT Val vmap_get_ic(Val m, const char *k, BFieldIC *ic) {
    if (m.tag != V_STRUCT) { vraise("no method get"); return vunwind; }
    BRAT_CTR_LOCK_OBJ(m.as.map);
    Val *o = map_get_ic(m.as.map, k, ic);
    Val r = o ? *o : vnil();
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return r;
}
BFN Val vmap_has(Val m, Val k) {
    if (m.tag != V_STRUCT) { vraise("no method has"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    Val *o = map_get_v(m.as.map, k);
    return vbool(o != 0);
}
BFN_HOT Val vmap_has_ic(Val m, const char *k, BFieldIC *ic) {
    if (m.tag != V_STRUCT) { vraise("no method has"); return vunwind; }
    BRAT_CTR_LOCK_OBJ(m.as.map);
    Val *o = map_get_ic(m.as.map, k, ic);
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return vbool(o != 0);
}
BFN_HOT Val vstag_is(Val v, const char *t) {
    return vbool(v.tag == V_STRUCT && v.as.map && v.as.map->type && strcmp(v.as.map->type, t) == 0);
}
BFN_HOT Val vshas_ic(Val m, const char *k, BFieldIC *ic) {
    if (m.tag != V_STRUCT || !m.as.map) return vbool(0);
    BRAT_CTR_LOCK_OBJ(m.as.map);
    Val *o = map_get_ic(m.as.map, k, ic);
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return vbool(o != 0);
}
BFN Val vmap_keys(Val m) {
    if (m.tag != V_STRUCT) { vraise("no method keys"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < m.as.map->used; i++)
        if (m.as.map->keys[i].tag != V_MOVED) vec_push(o, vrc_inc(m.as.map->keys[i]));
    return vvecv(o);
}
BFN Val vmap_del(Val m, Val k) {
    if (m.tag != V_STRUCT) { vraise("no method del"); return vunwind; }
    if (vmapkey_reserved(k)) { return vmapkey_err(); }
    if (vmap_key_bad(k)) { return vmap_bad_key_err(); }
    return vbool(map_del_v(m.as.map, k));
}

BFN bint vmapkey_reserved(Val k) {
    return k.tag == V_STR && strcmp(vstr_c(&k), "$type") == 0;
}
BFN Val vmapkey_err(void) {
    Vec *e = vvec();
    vec_push(e, vstr("map key \"$type\" is reserved"));
    return vtagv("err", e);
}

BFN bint vmap_has_reserved(Map *m) {
    Val k = vstr_sso("$type", 5);
    if (map_find(m, k) >= 0) { vrc_dec(vstruct(m)); return 1; }
    return 0;
}

BFN Val vmap_setv(Val m, Val k, Val v) {
    if (m.tag != V_STRUCT) { vrc_dec(k); vrc_dec(v); vraise("no method set"); return vunwind; }
    if (vmapkey_reserved(k)) { vrc_dec(k); vrc_dec(v); return vmapkey_err(); }
    if (vmap_key_bad(k)) { vrc_dec(k); vrc_dec(v); return vmap_bad_key_err(); }
    map_set_v(m.as.map, k, v);
    return v;
}
BFN bint vlong(Val v);
/* The position an index names. A float counts when it is a whole number, so `v[2.0]` works;
   `v[1.5]`, NaN, infinity and a bool are not positions. A whole float beyond the i64 range
   names no element: it comes back as -1 and the caller's range check refuses it. */
BFN int vindex_int(Val i, bint *k) {
    if (i.tag == V_INT) { *k = i.as.ival; return 1; }
    if (i.tag == V_FLOAT && isfinite(i.as.fval) && floor(i.as.fval) == i.as.fval) {
        *k = (i.as.fval >= -9223372036854775808.0 && i.as.fval < 9223372036854775808.0)
             ? (bint)i.as.fval : -1;
        return 1;
    }
    vraise("index must be int");
    return 0;
}
BFN Val vset_at(Val m, Val k, Val v) {
    if (m.tag == V_STRUCT) {
        if (m.as.map->type && m.as.map->type[0]) {
            Val ks = vstr_of(k);
            vrc_dec(k);
            vrc_dec(v);
            if (verr_pending()) return vunwind;
            const char *ksc = vstr_c(&ks);
            size_t n = strlen(ksc) + 16;
            char *msg = (char *)malloc(n);
            if (!msg) vfatal_oom();
            snprintf(msg, n, "no field %s", ksc);
            vrc_dec(ks);
            vraise(msg);
            free(msg);
            return vunwind;
        }
        if (vmapkey_reserved(k) || vmap_key_bad(k)) {
            vrc_dec(k);
            vrc_dec(v);
            return vnil();
        }
        map_set_v(m.as.map, k, v);
        return v;
    }
    if (m.tag == V_VEC) {
        bint i = 0;
        const int whole = vindex_int(k, &i);
        vrc_dec(k);
        if (!whole) { vrc_dec(v); return vunwind; }
        if (i < 0 || i >= m.as.vec->len) { vrc_dec(v); vraise("index out of range"); return vunwind; }
        BRAT_CTR_LOCK_OBJ(m.as.vec);
        Val old = m.as.vec->items[i];
        m.as.vec->items[i] = v;
        BRAT_CTR_UNLOCK_OBJ(m.as.vec);
        /* Released outside the lock: `vrelease1` runs arbitrary destructors. */
        vrelease1(old);
        return v;
    }
    vrc_dec(k);
    vrc_dec(v);
    vraise("setidx of non-indexable");
    return vunwind;
}
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

static int vbase_ready;
BFN const char *vconfine(const char *p);

#if (BRAT_CRT_MASK & BRAT_CRT_LIB)
/* Plugins: open a shared object at run time and call a symbol with a declared
 * signature. The signature is a literal at the call site, so the cast is
 * checked at compile time and no variadic thunk is involved. */
typedef struct BPluginAbiEntry {
    const char *name;
    const char *sig;
} BPluginAbiEntry;
typedef struct BPluginHandle {
    bint id;
    bint active;
    int closing;
    void *native;
    const BPluginAbiEntry *abi;
    struct BPluginHandle *next;
} BPluginHandle;
typedef struct BPluginSymbol {
    bint id, owner;
    void *native;
    const char *abi_sig;
    char name[128];
    struct BPluginSymbol *next;
} BPluginSymbol;
static BPluginHandle *bplugin_handles = 0;
static BPluginSymbol *bplugin_symbols = 0;
static bint bplugin_seq = 0;

static bint bplugin_next_id(void) {
    if (bplugin_seq == INT64_MAX) return 0;
    return ++bplugin_seq;
}

static void *_blib_open_raw(const char *raw_path, char *errbuf, size_t errcap) {
#if defined(_WIN32)
    int has_sep = (strchr(raw_path, '/') != NULL || strchr(raw_path, '\\') != NULL);
#else
    int has_sep = (strchr(raw_path, '/') != NULL);
#endif
    const char *load_path = raw_path;
    if (has_sep && vbase_ready) {
        const char *cp = vconfine(raw_path);
        if (!cp) {
            snprintf(errbuf, errcap, "path outside the program directory");
            return 0;
        }
        load_path = cp;
    }
#if defined(_WIN32)
    UINT prev_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    /* Confine bare DLL names to System32 so CWD/PATH DLL planting cannot hijack
     * a plugin load; explicit paths have already passed `vconfine`. */
    DWORD flags = has_sep ? LOAD_WITH_ALTERED_SEARCH_PATH : LOAD_LIBRARY_SEARCH_SYSTEM32;
    HMODULE m = LoadLibraryExA(load_path, NULL, flags);
    DWORD werr = m ? 0 : GetLastError();
    SetErrorMode(prev_mode);
    if (!m) {
        DWORD got = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, werr, 0, errbuf, (DWORD)errcap, NULL);
        if (!got) snprintf(errbuf, errcap, "LoadLibraryEx failed (%u)", (unsigned)werr);
        else {
            while (got > 0 && (errbuf[got - 1] == '\r' || errbuf[got - 1] == '\n'))
                errbuf[--got] = 0;
        }
        return 0;
    }
    return (void *)m;
#else
    void *h = dlopen(load_path, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(errbuf, errcap, "%s", e ? e : "dlopen failed");
    }
    return h;
#endif
}

static void *_blib_sym_raw(void *native, const char *sym) {
#if defined(_WIN32)
    FARPROC fp = GetProcAddress((HMODULE)native, sym);
    void *out = 0;
    memcpy(&out, &fp, sizeof(out));
    return out;
#else
    dlerror();
    void *p = dlsym(native, sym);
    const char *err = dlerror();
    return err ? 0 : p;
#endif
}

static int _blib_close_raw(void *native, char *errbuf, size_t errcap) {
#if defined(_WIN32)
    if (FreeLibrary((HMODULE)native)) return 0;
    DWORD werr = GetLastError();
    snprintf(errbuf, errcap, "FreeLibrary failed (%u)", (unsigned)werr);
    return -1;
#else
    if (dlclose(native) == 0) return 0;
    const char *e = dlerror();
    snprintf(errbuf, errcap, "%s", e ? e : "dlclose failed");
    return -1;
#endif
}

BFN Val vlib_open(Val path) {
    void *h;
    BPluginHandle *entry;
    char err[512] = {0};
    if (path.tag != V_STR) { vraise("lib_open: want str"); return vunwind; }
    if (vstr_has_nul(&path)) { vraise("lib_open: embedded NUL in path"); return vunwind; }
    h = _blib_open_raw(vstr_c(&path), err, sizeof err);
    if (!h) {
        char b[560];
        snprintf(b, sizeof b, "lib_open: %s", err[0] ? err : "open failed");
        vraise(b);
        return vunwind;
    }
    entry = (BPluginHandle *)malloc(sizeof(BPluginHandle));
    if (!entry) { char ign[64]; _blib_close_raw(h, ign, sizeof ign); vfatal_oom(); }
    BRAT_PLUGIN_LOCK();
    entry->id = bplugin_next_id();
    if (!entry->id) {
        BRAT_PLUGIN_UNLOCK();
        free(entry);
        { char ign[64]; _blib_close_raw(h, ign, sizeof ign); }
        vraise("lib_open: handle id space exhausted");
        return vunwind;
    }
    entry->active = 0;
    entry->closing = 0;
    entry->native = h;
    entry->abi = (const BPluginAbiEntry *)_blib_sym_raw(h, "brat_plugin_abi_v1");
    entry->next = bplugin_handles;
    bplugin_handles = entry;
    BRAT_PLUGIN_UNLOCK();
    return vint(entry->id);
}

BFN Val vlib_sym(Val h, Val name) {
    void *p;
    BPluginHandle *handle;
    BPluginSymbol *symbol;
    if (name.tag != V_STR) { vraise("lib_sym: want str"); return vunwind; }
    if (vstr_has_nul(&name)) { vraise("lib_sym: embedded NUL in symbol name"); return vunwind; }
    if (h.tag != V_INT || h.as.ival <= 0) {
        vraise("lib_sym: invalid library handle");
        return vunwind;
    }
    BRAT_PLUGIN_LOCK();
    for (handle = bplugin_handles; handle && handle->id != h.as.ival; handle = handle->next) {}
    if (!handle || handle->closing) {
        BRAT_PLUGIN_UNLOCK();
        char b[128];
        snprintf(b, sizeof b, "lib_sym: no such handle %lld", (long long)h.as.ival);
        vraise(b);
        return vunwind;
    }
    p = _blib_sym_raw(handle->native, vstr_c(&name));
    if (!p) {
        BRAT_PLUGIN_UNLOCK();
        char b[512];
        snprintf(b, sizeof b, "lib_sym: no symbol %s", vstr_c(&name));
        vraise(b);
        return vunwind;
    }
    symbol = (BPluginSymbol *)malloc(sizeof(BPluginSymbol));
    if (!symbol) { BRAT_PLUGIN_UNLOCK(); vfatal_oom(); }
    symbol->id = bplugin_next_id();
    if (!symbol->id) {
        BRAT_PLUGIN_UNLOCK();
        free(symbol);
        vraise("lib_sym: handle id space exhausted");
        return vunwind;
    }
    symbol->owner = h.as.ival;
    symbol->native = p;
    symbol->abi_sig = 0;
    snprintf(symbol->name, sizeof symbol->name, "%s", vstr_c(&name));
    if (handle->abi) {
        const BPluginAbiEntry *ae;
        for (ae = handle->abi; ae->name; ae++) {
            if (strcmp(ae->name, vstr_c(&name)) == 0) {
                symbol->abi_sig = ae->sig;
                break;
            }
        }
    }
    symbol->next = bplugin_symbols;
    bplugin_symbols = symbol;
    BRAT_PLUGIN_UNLOCK();
    return vint(symbol->id);
}

BFN Val vlib_close(Val h) {
    BPluginHandle **link, *handle;
    BPluginSymbol **slink, *symbol;
    char err[512] = {0};
    if (h.tag != V_INT || h.as.ival <= 0) {
        vraise("lib_close: invalid library handle");
        return vunwind;
    }
    BRAT_PLUGIN_LOCK();
    for (link = &bplugin_handles; *link && (*link)->id != h.as.ival; link = &(*link)->next) {}
    handle = *link;
    if (!handle || (handle->closing && handle->active > 0)) {
        BRAT_PLUGIN_UNLOCK();
        char b[128];
        snprintf(b, sizeof b, "lib_close: no such handle %lld", (long long)h.as.ival);
        vraise(b);
        return vunwind;
    }
    if (handle->active) {
        handle->closing = 1;
    } else if (_blib_close_raw(handle->native, err, sizeof err) != 0) {
        char b[560];
        snprintf(b, sizeof b, "lib_close: %s", err[0] ? err : "close failed");
        BRAT_PLUGIN_UNLOCK();
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
    BRAT_PLUGIN_UNLOCK();
    return vint(0);
}

BFN void *vplugin_acquire_sig(Val fn, bint *owner, const char *call_sig) {
    BPluginSymbol *symbol;
    BPluginHandle *handle;
    void *native = 0;
    int saturated = 0;
    const char *bad_abi = 0;
    char bad_name[128] = {0};
    if (fn.tag != V_INT || fn.as.ival <= 0) {
        vraise("call: want function symbol");
        return 0;
    }
    BRAT_PLUGIN_LOCK();
    for (symbol = bplugin_symbols; symbol && symbol->id != fn.as.ival; symbol = symbol->next) {}
    if (symbol) {
        for (handle = bplugin_handles; handle && handle->id != symbol->owner;
             handle = handle->next) {}
        if (handle && !handle->closing) {
            if (call_sig && symbol->abi_sig && strcmp(symbol->abi_sig, call_sig) != 0) {
                bad_abi = symbol->abi_sig;
                snprintf(bad_name, sizeof bad_name, "%s", symbol->name);
            } else if (handle->active < INT64_MAX) {
                handle->active++;
                *owner = handle->id;
                native = symbol->native;
            } else {
                saturated = 1;
            }
        }
    }
    BRAT_PLUGIN_UNLOCK();
    if (bad_abi) {
        char b[512];
        snprintf(b, sizeof b,
                 "call: ABI mismatch on `%s`: plugin exports `%s`, call site wrote `%s`",
                 bad_name, bad_abi, call_sig);
        vraise(b);
        return 0;
    }
    if (!native) {
        char b[128];
        if (saturated) snprintf(b, sizeof b, "call: too many concurrent calls");
        else snprintf(b, sizeof b, "call: no such function %lld", (long long)fn.as.ival);
        vraise(b);
    }
    return native;
}

BFN void *vplugin_acquire(Val fn, bint *owner) {
    return vplugin_acquire_sig(fn, owner, 0);
}

BFN void vplugin_release(bint owner) {
    BPluginHandle **link, *handle;
    BPluginSymbol **slink, *symbol;
    char error[512] = {0};
    BRAT_PLUGIN_LOCK();
    for (link = &bplugin_handles; *link && (*link)->id != owner; link = &(*link)->next) {}
    handle = *link;
    if (handle && handle->active > 0) {
        handle->active--;
        if (handle->closing && handle->active == 0) {
            char cerr[480] = {0};
            if (_blib_close_raw(handle->native, cerr, sizeof cerr) == 0) {
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
                snprintf(error, sizeof error, "lib_close: %s",
                         cerr[0] ? cerr : "close failed");
            }
        }
    }
    BRAT_PLUGIN_UNLOCK();
    if (error[0]) vraise(error);
}
#endif /* BRAT_CRT_LIB */
