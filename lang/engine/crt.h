#ifndef BRAT_RT_H
#define BRAT_RT_H

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
#if defined(_MSC_VER)
#define BTLS __declspec(thread)
#else
#define BTLS _Thread_local
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

static int g_pool_n = 0;
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
static BStripeLock _brat_stripes[64] = { BSTRIPE_16_, BSTRIPE_16_, BSTRIPE_16_, BSTRIPE_16_ };
#undef BSTRIPE_16_
#undef BSTRIPE_4_
#undef BSTRIPE_1_
#define BSTRIPE_IDX(p_) ((unsigned)((((uintptr_t)(p_) >> 6) * 11400714819323198485ULL) >> 58))
#if defined(BRAT_RT_SHARED)
#define _BRAT_MT() 1
#elif defined(__GNUC__) || defined(__clang__)
#define _BRAT_MT() (__builtin_expect(__atomic_load_n(&g_pool_n, __ATOMIC_ACQUIRE) != 0, 0))
#else
#define _BRAT_MT() (g_pool_n != 0)
#endif
#define BRAT_CTR_LOCK_OBJ(p_)   do { if (_BRAT_MT()) BMUTEX_LOCK(&_brat_stripes[BSTRIPE_IDX(p_)].mu); } while (0)
#define BRAT_CTR_UNLOCK_OBJ(p_) do { if (_BRAT_MT()) BMUTEX_UNLOCK(&_brat_stripes[BSTRIPE_IDX(p_)].mu); } while (0)
static bmutex_t _brat_plugin_lock = BMUTEX_INIT;
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
   SIGSEGV at around 100 000 levels. The interpreter refuses the same program
   with "recursion too deep" because it has a frame budget of 2000, so the two
   backends disagreed twice over: one gave a clean error, the other a signal.

   Same budget here: past BRAT_MAX_VALUE_DEPTH the error channel gets
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

#define BVPIN_SLOTS 32768u
#define BVPIN_HT_CAP 65536u
#define BVPIN_EMPTY 0xFFFFu
static char _bvpin_slots[BVPIN_SLOTS][16];
static uint16_t _bvpin_ht[BVPIN_HT_CAP];
static uint32_t _bvpin_count = 0;
static int _bvpin_inited = 0;
static bmutex_t _bvpin_mu = BMUTEX_INIT;

static uint64_t _bvpin_hash_bytes(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)n;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Returned addresses remain valid for the process lifetime. Do not evict:
 * callers can hold raw pointers independently of subsequent pin operations. */
BFN char *vstr_pin(Val v) {
    if (v.tag != V_STR) return NULL;
    if (!v.sso) return v.as.str;
    const char *src = vstr_c(&v);
    size_t n = vstr_sso_len(&v);
    size_t idx = (size_t)_bvpin_hash_bytes(src, n) & (BVPIN_HT_CAP - 1u);
    BMUTEX_LOCK(&_bvpin_mu);
    if (!_bvpin_inited) {
        memset(_bvpin_ht, 0xFF, sizeof _bvpin_ht);
        _bvpin_inited = 1;
    }
    while (_bvpin_ht[idx] != BVPIN_EMPTY) {
        char *p = _bvpin_slots[_bvpin_ht[idx]];
        if ((unsigned char)p[15] == n && memcmp(p, src, n + 1) == 0) {
            BMUTEX_UNLOCK(&_bvpin_mu);
            return p;
        }
        idx = (idx + 1u) & (BVPIN_HT_CAP - 1u);
    }
    if (_bvpin_count == BVPIN_SLOTS) {
        BMUTEX_UNLOCK(&_bvpin_mu);
        fputs("brat: string pin limit exceeded\n", stderr);
        abort();
    }
    uint16_t slot = (uint16_t)_bvpin_count++;
    char *p = _bvpin_slots[slot];
    memcpy(p, src, n + 1);
    p[15] = (char)n;
    _bvpin_ht[idx] = slot;
    BMUTEX_UNLOCK(&_bvpin_mu);
    return p;
}

static void *balloc(size_t n);
static BTLS int barena_depth = 0;
static struct { const char *src; char *ptr; } _vlit_cache[512];
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
BFN Val vlit(const char *s) {
    size_t n = strlen(s);
    if (n <= BRAT_SSO_MAX) return vstr_sso(s, n);
    int seen = _vlit_count();
    for (int i = 0; i < seen; i++) {
        if (_vlit_cache[i].src == s) {
            Val v; v.tag = V_STR; v.sso = 0; v.imm = 1; v.as.str = _vlit_cache[i].ptr;
            return v;
        }
    }
    int saved = barena_depth;
    barena_depth = 0;
    char *p = (char *)balloc(n + 1);
    barena_depth = saved;
    memcpy(p, s, n + 1);
    ((BHead *)p - 1)->rc = -1; /* immortal: never freed, never counted */
    ((BHead *)p - 1)->len = (bint)n;
    BMUTEX_LOCK(&_vlit_mu);
    if (_vlit_n < 512) {
        _vlit_cache[_vlit_n].src = s;
        _vlit_cache[_vlit_n].ptr = p;
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
    _stk[_sp].v = v; _stk[_sp].post = 0; _sp++;
    while (_sp > 0) {
        BVRelEnt _ent = _stk[--_sp];
        Val cur = _ent.v;
        if (_ent.post) {
            if (cur.tag == V_VEC) {
                Vec *vec = cur.as.vec;
                BVEC_FREE_ITEMS(vec);
                free((BHead *)vec - 1);
            } else if (cur.tag == V_STRUCT) {
                Map *map = cur.as.map;
                BMAP_FREE_ARRS(map);
                free((BHead *)map - 1);
            } else if (cur.tag == V_TAG) {
                Vec *args = cur.as.tv;
                BVEC_FREE_ITEMS(args);
                free((BHead *)args - 1);
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
        if ((uint64_t)BRAT_MEM_BYTES > (uint64_t)SIZE_MAX) {
            fprintf(stderr, "mem budget: JobObject(%lld) refused\n", (long long)(bint)BRAT_MEM_BYTES);
            exit(1);
        }
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
BFN Vec *vvec(void) {
    Vec *v = (Vec *)balloc(sizeof(Vec));
    v->len = 0; v->cap = 2; v->tag = 0;
    v->items = v->inl;
    return v;
}
BFN Vec *vvec_cap(bint cap) {
    if (cap < 0) vfatal_oom();
    size_t bytes = cap > 2 ? bmul_size(cap, sizeof(Val)) : 0;
    Vec *v = (Vec *)balloc(sizeof(Vec));
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
            free(h);
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
            free(h);
        }
        return;
    }
    case V_TAG: {
        Vec *args = v.as.tv;
        if (args && !BIS_ARENA((BHead *)args - 1)) {
            BHead *h = (BHead *)args - 1;
            for (bint i = 0; i < args->len; i++) vrelease1(args->items[i]);
            BVEC_FREE_ITEMS(args);
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
    m->len = 0; m->used = 0; m->dead = 0; m->cap = 2;
    m->keys = m->inl_k;
    m->vals = m->inl_v;
    m->hashes = m->inl_h;
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
    Map *m = (n <= 2)
        ? (Map *)balloc(sizeof(Map))
        : (Map *)balloc(sizeof(Map) + vbytes);
    m->keys = shp->keys;
    m->vals = (n <= 2) ? m->inl_v : (Val *)(m + 1);
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
           normalises the same way; the interpreter's `_mkey` does the same. */
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
    if (a.tag == V_FLOAT && b.tag == V_FLOAT &&
        a.as.fval != a.as.fval && b.as.fval != b.as.fval) return 1;
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
   here and an err value in the interpreter. */
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
            if (sh == ic->shapes[0]) return &m->vals[ic->slots[0]];
            if (sh == ic->shapes[1]) return &m->vals[ic->slots[1]];
            if (sh == ic->shapes[2]) return &m->vals[ic->slots[2]];
            if (sh == ic->shapes[3]) return &m->vals[ic->slots[3]];
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
    if (at < 0) return 0;
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
static int vbase_ready;
BFN const char *vconfine(const char *p);

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

/* A NUL-terminated C string becomes a BRAT string only after the cap and
 * UTF-8 checks. The scan stops one byte past the cap, so a string with no
 * terminator in range is refused instead of copied without bound. */
static Val vstr_c_checked(const char *s, const char *null_msg, const char *bad_msg) {
    size_t n = 0;
    if (!s) { vraise(null_msg); return vnil(); }
    while (n <= BRAT_MAX_STR && s[n]) n++;
    if (n > BRAT_MAX_STR) { vraise("out of memory"); return vnil(); }
    if (!utf8_valid(s, (bint)n)) { vraise(bad_msg); return vnil(); }
    return vstrn(s, (bint)n);
}

static const char *_verror_text(const char *msg) {
    size_t n = 0;
    if (!msg) return "";
    while (n <= BRAT_MAX_STR && msg[n]) n++;
    if (n > BRAT_MAX_STR || !utf8_valid(msg, (bint)n))
        return "invalid utf-8 in error message";
    return msg;
}

BFN Val vcall_str(const char *p) {
    return vstr_c_checked(p, "call: NULL str result", "call: invalid UTF-8 str result");
}

BFN Val vstr_ffi(const char *s) {
    return vstr_c_checked(s, "ffi: C returned NULL where str expected",
                          "ffi: C returned invalid UTF-8 where str expected");
}

BFN Val vstr_ffi_arg(const char *s) {
    return vstr_c_checked(s, "ffi: C passed NULL where str expected",
                          "ffi: C passed invalid UTF-8 where str expected");
}

BFN Val vstr_ffi_field(const char *s) {
    if (!s) return vnil();
    return vstr_c_checked(s, "ffi: C string field is NULL",
                          "ffi: C string field has invalid UTF-8");
}

BFN Val vstr_ptr_read(const char *s) {
    return vstr_c_checked(s, "null pointer dereference", "ptr_read_str: invalid UTF-8");
}

BFN Val vstr_from_ptr(Val p, Val n) {
    bint len = n.as.ival, i;
    const char *src;
    if (len < 0) { vraise("str_from_ptr: n must be >= 0"); return vunwind; }
    if ((uint64_t)len > (uint64_t)BRAT_MAX_STR) vfatal_oom();
    if (p.as.ival < 0) { vraise("pointer is negative"); return vunwind; }
    if (p.as.ival == 0) {
        if (len == 0) return vstr("");
        vraise("null pointer dereference");
        return vunwind;
    }
    src = (const char *)(intptr_t)p.as.ival;
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
    if (p.as.ival < 0) { vraise("pointer is negative"); return vunwind; }
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
    if (p.as.ival < 0) { vraise("pointer is negative"); return vunwind; }
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
        if (vclosure_env_map(v.as.map)) { vraise("not iterable"); return NULL; }
        Vec *o = vvec();
        for (bint i = 0; i < v.as.map->used; i++)
            if (v.as.map->keys[i].tag != V_MOVED) vec_push(o, vrc_inc(v.as.map->keys[i]));
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
        BRAT_CTR_LOCK_OBJ(a.as.vec);
        if (k < 0 || k >= a.as.vec->len) {
            BRAT_CTR_UNLOCK_OBJ(a.as.vec);
            vraise("index out of range");
            return vunwind;
        }
        Val r = vrc_inc(a.as.vec->items[k]);
        BRAT_CTR_UNLOCK_OBJ(a.as.vec);
        return r;
    }
    if (a.tag == V_TAG && i.tag == V_INT) {
        bint k = i.as.ival;
        BRAT_CTR_LOCK_OBJ(a.as.tv);
        if (k < 1 || k > a.as.tv->len) {
            BRAT_CTR_UNLOCK_OBJ(a.as.tv);
            vraise("index out of range");
            return vunwind;
        }
        Val r = vrc_inc(a.as.tv->items[k - 1]);
        BRAT_CTR_UNLOCK_OBJ(a.as.tv);
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
BFN_HOT int vis_true(Val v) {
    switch (v.tag) {
    case V_NIL: return 0;
    case V_BOOL: return v.as.b;
    case V_INT: return v.as.ival != 0;
    case V_FLOAT: return v.as.fval != 0;
    case V_STR: return !vstr_empty(v);
    case V_VEC: return v.as.vec->len != 0;
    case V_STRUCT: return (v.as.map->type && v.as.map->type[0]) || v.as.map->len != 0;
    default: return 1;
    }
}


BFN const char *vfmt_double(double x) {
    static BTLS char buf[8][48];
    static BTLS bint k = 0;
    char *b = buf[k++ & 7];
    int p;
    if (x != x) return "nan";
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
    char quote = vquote(s);
    for (const char *p = s; *p; p++) {
        if (*p == quote || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') n += 2;
        else n += 1;
    }
    return n;
}

BFN void vprint_f(Val v, bint q, FILE *f);

typedef struct {
    Val v;
    bint idx;
    int phase;
} BFmtFrame;

static void vfmt_emit_(const char *s, bint len, char **outp, FILE *fp, bint *np) {
    if (len <= 0) return;
    if (outp && *outp) {
        memcpy(*outp, s, (size_t)len);
        *outp += len;
    } else if (fp) {
        fwrite(s, 1, (size_t)len, fp);
    }
    *np += len;
}

static bint vfmt_walk_(Val cur, bint q, char *out, FILE *fp) {
    bint n = 0;
    bint base_depth = bval_depth;
    BFmtFrame sbuf[64];
    BFmtFrame *stk = sbuf;
    size_t sp = 0, cap = 64;
    char tmp[64];
#define BFMT_PUSH_(fv, fidx, fph) do { \
    if (sp == cap) { \
        cap *= 2; \
        BFmtFrame *_ns = (stk == sbuf) ? (BFmtFrame *)malloc(sizeof(BFmtFrame) * cap) \
                                       : (BFmtFrame *)realloc(stk, sizeof(BFmtFrame) * cap); \
        if (!_ns) vfatal_oom(); \
        if (stk == sbuf) memcpy(_ns, sbuf, sizeof(BFmtFrame) * sp); \
        stk = _ns; \
    } \
    stk[sp].v = (fv); stk[sp].idx = (fidx); stk[sp].phase = (fph); sp++; \
} while (0)

descend:
    if (base_depth + (bint)sp >= BRAT_MAX_VALUE_DEPTH) {
        vraise("recursion too deep");
        goto done;
    }
    switch (cur.tag) {
    case V_MOVED:
        vraise("use of moved value");
        goto done;
    case V_NIL:
        vfmt_emit_("None", 4, &out, fp, &n);
        break;
    case V_BOOL:
        vfmt_emit_(cur.as.b ? "True" : "False", cur.as.b ? 4 : 5, &out, fp, &n);
        break;
    case V_INT: {
        bint ln = snprintf(tmp, sizeof tmp, BINT_FMT, (bint)cur.as.ival);
        vfmt_emit_(tmp, ln, &out, fp, &n);
        break;
    }
    case V_FLOAT: {
        const char *fs = vfmt_double(cur.as.fval);
        vfmt_emit_(fs, (bint)strlen(fs), &out, fp, &n);
        break;
    }
    case V_STR: {
        const char *sv = vstr_c(&cur);
        if (!q) {
            vfmt_emit_(sv, (bint)vstr_len_fast(&cur), &out, fp, &n);
            break;
        }
        if (!out && !fp) {
            n += vrepr_slen(sv) + 2;
            break;
        }
        char quote = vquote(sv);
        vfmt_emit_(&quote, 1, &out, fp, &n);
        for (const char *p = sv; *p; p++) {
            if (*p == quote || *p == '\\') { char esc[2]; esc[0] = '\\'; esc[1] = *p; vfmt_emit_(esc, 2, &out, fp, &n); }
            else if (*p == '\n') vfmt_emit_("\\n", 2, &out, fp, &n);
            else if (*p == '\r') vfmt_emit_("\\r", 2, &out, fp, &n);
            else if (*p == '\t') vfmt_emit_("\\t", 2, &out, fp, &n);
            else vfmt_emit_(p, 1, &out, fp, &n);
        }
        vfmt_emit_(&quote, 1, &out, fp, &n);
        break;
    }
    case V_VEC:
        vfmt_emit_("[", 1, &out, fp, &n);
        if (cur.as.vec->len == 0) {
            vfmt_emit_("]", 1, &out, fp, &n);
            break;
        }
        BFMT_PUSH_(cur, 0, 0);
        cur = cur.as.vec->items[0];
        q = 1;
        goto descend;
    case V_STRUCT: {
        if (vclosure_env_map(cur.as.map)) {
            vfmt_emit_("{'$type': '__brat_closure_env'}", 31, &out, fp, &n);
            break;
        }
        int ismap = !cur.as.map->type[0];
        vfmt_emit_("{", 1, &out, fp, &n);
        if (!ismap) {
            vfmt_emit_("'$type': '", 10, &out, fp, &n);
            vfmt_emit_(cur.as.map->type, (bint)strlen(cur.as.map->type), &out, fp, &n);
            vfmt_emit_("'", 1, &out, fp, &n);
        }
        bint k0 = 0;
        while (k0 < cur.as.map->used && cur.as.map->keys[k0].tag == V_MOVED) k0++;
        if (k0 >= cur.as.map->used) {
            vfmt_emit_("}", 1, &out, fp, &n);
            break;
        }
        if (!ismap) vfmt_emit_(", ", 2, &out, fp, &n);
        BFMT_PUSH_(cur, k0, 0);
        cur = cur.as.map->keys[k0];
        q = 1;
        goto descend;
    }
    case V_TAG:
        if (vtag_closure(cur)) {
            vfmt_emit_("fn", 2, &out, fp, &n);
            break;
        }
        vfmt_emit_("('", 2, &out, fp, &n);
        vfmt_emit_(cur.as.tv->tag, (bint)strlen(cur.as.tv->tag), &out, fp, &n);
        vfmt_emit_("'", 1, &out, fp, &n);
        if (cur.as.tv->len == 0) {
            vfmt_emit_(",)", 2, &out, fp, &n);
            break;
        }
        vfmt_emit_(", ", 2, &out, fp, &n);
        BFMT_PUSH_(cur, 0, 0);
        cur = cur.as.tv->items[0];
        q = 1;
        goto descend;
    }

    while (sp > 0) {
        BFmtFrame *top = &stk[sp - 1];
        if (top->v.tag == V_VEC) {
            bint nx = ++top->idx;
            if (nx < top->v.as.vec->len) {
                vfmt_emit_(", ", 2, &out, fp, &n);
                cur = top->v.as.vec->items[nx];
                q = 1;
                goto descend;
            }
            vfmt_emit_("]", 1, &out, fp, &n);
            sp--;
        } else if (top->v.tag == V_TAG) {
            bint nx = ++top->idx;
            if (nx < top->v.as.tv->len) {
                vfmt_emit_(", ", 2, &out, fp, &n);
                cur = top->v.as.tv->items[nx];
                q = 1;
                goto descend;
            }
            vfmt_emit_(")", 1, &out, fp, &n);
            sp--;
        } else {
            if (top->phase == 0) {
                top->phase = 1;
                vfmt_emit_(": ", 2, &out, fp, &n);
                cur = top->v.as.map->vals[top->idx];
                q = 1;
                goto descend;
            }
            bint nx = ++top->idx;
            while (nx < top->v.as.map->used && top->v.as.map->keys[nx].tag == V_MOVED)
                nx = ++top->idx;
            top->phase = 0;
            if (nx < top->v.as.map->used) {
                vfmt_emit_(", ", 2, &out, fp, &n);
                cur = top->v.as.map->keys[nx];
                q = 1;
                goto descend;
            }
            vfmt_emit_("}", 1, &out, fp, &n);
            sp--;
        }
    }
#undef BFMT_PUSH_
done:
    if (stk != sbuf) free(stk);
    return n;
}

/* `print` shares the iterative heap-stack walker with `vsprint_len_q` and
   `vsprint_q`, keeping the same `BRAT_MAX_VALUE_DEPTH` budget with O(1) C stack. */
BFN void vprint_f(Val v, bint q, FILE *f) {
    (void)vfmt_walk_(v, q, NULL, f);
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
BFN void vprint_err(Val v) {
    vprint_f(v, 0, stderr);
    vtrace_print_err_line();
    vrc_dec(v);
}
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
    int err = r.tag == V_TAG && r.as.tv && r.as.tv->tag && !strcmp(r.as.tv->tag, "err");
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

/* Numeric order, exact for integers. Both `veq` and `vcmp_` used to convert
   the two sides to `double` first, which answers `2^60 + 1 > 2^60` with "equal",
   and did so on both backends because both converted: the C side printed
   `False` where the program asked for the order of two distinct integers, and
   `==` on the same pair answered `True` in C and `False` in the interpreter.
   An integer is compared as an integer here, and against a float without
   rounding the integer, which is what Python does for `int` against `float`. */
static int vcmp_int_double(double d, bint i) {
    if (d != d) return 1;
    if (d >= 9223372036854775808.0) return 1;
    if (d < -9223372036854775808.0) return -1;
    double fl = floor(d);
    bint fi = (bint)fl;
    if (fi < i) return -1;
    if (fi > i) return 1;
    return d > fl ? 1 : 0;
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
        if (a.as.fval != a.as.fval || b.as.fval != b.as.fval)
            return (a.as.fval != a.as.fval) - (b.as.fval != b.as.fval);
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

typedef struct {
    Val a;
    Val b;
    bint idx;
    bint idx2;
    int phase;
} BPairFrame;

BFN bint veq(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT) return a.as.ival == b.as.ival;
    if (a.tag == V_FLOAT && b.tag == V_FLOAT) return a.as.fval == b.as.fval;
    if (a.tag == V_BOOL && b.tag == V_BOOL) return a.as.b == b.as.b;
    if (a.tag == V_NIL && b.tag == V_NIL) return 1;
    bint base_depth = bval_depth;
    BPairFrame sbuf[64];
    BPairFrame *stk = sbuf;
    size_t sp = 0, cap = 64;
    bint res = 1;
#define BPAIR_PUSH_(fa, fb, fidx, fph) do { \
    if (sp == cap) { \
        cap *= 2; \
        BPairFrame *_ns = (stk == sbuf) ? (BPairFrame *)malloc(sizeof(BPairFrame) * cap) \
                                        : (BPairFrame *)realloc(stk, sizeof(BPairFrame) * cap); \
        if (!_ns) vfatal_oom(); \
        if (stk == sbuf) memcpy(_ns, sbuf, sizeof(BPairFrame) * sp); \
        stk = _ns; \
    } \
    stk[sp].a = (fa); stk[sp].b = (fb); stk[sp].idx = (fidx); stk[sp].idx2 = 0; stk[sp].phase = (fph); sp++; \
} while (0)

descend:
    if (base_depth + (bint)sp >= BRAT_MAX_VALUE_DEPTH) {
        vraise("recursion too deep");
        res = 0;
        goto done;
    }
    if (vcmp_is_num(a) && vcmp_is_num(b)) {
        if (!veq_num(a, b)) { res = 0; goto done; }
    } else if (a.tag != b.tag) {
        res = 0;
        goto done;
    } else {
        switch (a.tag) {
        case V_NIL:
            break;
        case V_STR: {
            if (a.sso && b.sso) {
                if (memcmp(&a, &b, sizeof(Val)) != 0) { res = 0; goto done; }
                break;
            }
            size_t la = vstr_len_fast(&a), lb = vstr_len_fast(&b);
            if (la != lb) { res = 0; goto done; }
            const char *sa = vstr_c(&a), *sb = vstr_c(&b);
            if (sa != sb && memcmp(sa, sb, la) != 0) { res = 0; goto done; }
            break;
        }
        case V_VEC:
            if (a.as.vec->len != b.as.vec->len) { res = 0; goto done; }
            if (a.as.vec->len == 0) break;
            BPAIR_PUSH_(a, b, 0, 0);
            a = a.as.vec->items[0];
            b = b.as.vec->items[0];
            goto descend;
        case V_STRUCT: {
            if (vclosure_env_map(a.as.map) || vclosure_env_map(b.as.map)) {
                if (!vclosure_env_map(a.as.map) || !vclosure_env_map(b.as.map) ||
                    a.as.map->len != b.as.map->len || a.as.map->bad_key != b.as.map->bad_key) {
                    res = 0;
                    goto done;
                }
                bint k = 0;
                while (k < a.as.map->len && k == a.as.map->bad_key) k++;
                if (k >= a.as.map->len) break;
                BPAIR_PUSH_(a, b, k, 2);
                a = a.as.map->vals[k];
                b = b.as.map->vals[k];
                goto descend;
            }
            if (a.as.map->shape && a.as.map->shape == b.as.map->shape) {
                if (a.as.map->len == 0) break;
                BPAIR_PUSH_(a, b, 0, 1);
                a = a.as.map->vals[0];
                b = b.as.map->vals[0];
                goto descend;
            }
            if (strcmp(a.as.map->type, b.as.map->type) || a.as.map->len != b.as.map->len) {
                res = 0;
                goto done;
            }
            bint k0 = 0;
            while (k0 < a.as.map->used && a.as.map->keys[k0].tag == V_MOVED) k0++;
            if (k0 >= a.as.map->used) break;
            bval_depth = base_depth + (bint)sp + 1;
            Val *o = map_get_v(b.as.map, a.as.map->keys[k0]);
            bval_depth = base_depth;
            if (verr_pending() || !o) { res = 0; goto done; }
            BPAIR_PUSH_(a, b, k0, 0);
            a = a.as.map->vals[k0];
            b = *o;
            goto descend;
        }
        case V_TAG:
            if (strcmp(a.as.tv->tag, b.as.tv->tag) || a.as.tv->len != b.as.tv->len) {
                res = 0;
                goto done;
            }
            if (a.as.tv->len == 0) break;
            BPAIR_PUSH_(a, b, 0, 0);
            a = a.as.tv->items[0];
            b = b.as.tv->items[0];
            goto descend;
        default:
            res = 0;
            goto done;
        }
    }

    while (sp > 0) {
        BPairFrame *top = &stk[sp - 1];
        bint nx = ++top->idx;
        if (top->a.tag == V_VEC) {
            if (nx < top->a.as.vec->len) {
                a = top->a.as.vec->items[nx];
                b = top->b.as.vec->items[nx];
                goto descend;
            }
            sp--;
        } else if (top->a.tag == V_TAG) {
            if (nx < top->a.as.tv->len) {
                a = top->a.as.tv->items[nx];
                b = top->b.as.tv->items[nx];
                goto descend;
            }
            sp--;
        } else {
            if (top->phase == 2) {
                while (nx < top->a.as.map->len && nx == top->a.as.map->bad_key)
                    nx = ++top->idx;
                if (nx < top->a.as.map->len) {
                    a = top->a.as.map->vals[nx];
                    b = top->b.as.map->vals[nx];
                    goto descend;
                }
                sp--;
                continue;
            }
            if (top->phase == 1) {
                if (nx < top->a.as.map->len) {
                    a = top->a.as.map->vals[nx];
                    b = top->b.as.map->vals[nx];
                    goto descend;
                }
                sp--;
                continue;
            }
            while (nx < top->a.as.map->used && top->a.as.map->keys[nx].tag == V_MOVED)
                nx = ++top->idx;
            if (nx < top->a.as.map->used) {
                bval_depth = base_depth + (bint)sp;
                Val *o = map_get_v(top->b.as.map, top->a.as.map->keys[nx]);
                bval_depth = base_depth;
                if (verr_pending() || !o) { res = 0; goto done; }
                a = top->a.as.map->vals[nx];
                b = *o;
                goto descend;
            }
            sp--;
        }
    }
#undef BPAIR_PUSH_
done:
    bval_depth = base_depth;
    if (stk != sbuf) free(stk);
    return res;
}


BFN int vcmp(Val a, Val b);
BFN_HOT int vcmpt(Val a, Val b, const char *op);
BFN int vclass(Val v);
static int vcmp_(Val a, Val b, const char *op);

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

/* Iterative explicit heap-stack three-way ordering with `BRAT_MAX_VALUE_DEPTH`. */
static int vcmp_(Val a, Val b, const char *op) {
    bint base_depth = bval_depth;
    BPairFrame sbuf[64];
    BPairFrame *stk = sbuf;
    size_t sp = 0, cap = 64;
    int res = 0;
#define BCMP_PUSH2_(fa, fb, fidx, fidx2, fph) do { \
    if (sp == cap) { \
        cap *= 2; \
        BPairFrame *_ns = (stk == sbuf) ? (BPairFrame *)malloc(sizeof(BPairFrame) * cap) \
                                        : (BPairFrame *)realloc(stk, sizeof(BPairFrame) * cap); \
        if (!_ns) vfatal_oom(); \
        if (stk == sbuf) memcpy(_ns, sbuf, sizeof(BPairFrame) * sp); \
        stk = _ns; \
    } \
    stk[sp].a = (fa); stk[sp].b = (fb); stk[sp].idx = (fidx); stk[sp].idx2 = (fidx2); stk[sp].phase = (fph); sp++; \
} while (0)
#define BCMP_PUSH_(fa, fb, fidx, fph) BCMP_PUSH2_(fa, fb, fidx, 0, fph)

descend:
    if (base_depth + (bint)sp >= BRAT_MAX_VALUE_DEPTH) {
        vraise("recursion too deep");
        res = 0;
        goto done;
    }
    if (a.tag == V_STR && b.tag == V_STR) {
        int r = strcmp(vstr_c(&a), vstr_c(&b));
        if (r != 0) { res = r < 0 ? -1 : 1; goto done; }
    } else if (vcmp_is_num(a) && vcmp_is_num(b)) {
        int r = vcmp_num(a, b);
        if (r != 0) { res = r; goto done; }
    } else if (a.tag == V_NIL || b.tag == V_NIL) {
        if (a.tag != b.tag) { res = a.tag == V_NIL ? -1 : 1; goto done; }
    } else if (vclass(a) != vclass(b) || !vclass(a)) {
        vraisef("type error in %s", op);
        res = 0;
        goto done;
    } else if (a.tag == V_VEC) {
        Vec *x = a.as.vec, *y = b.as.vec;
        bint nn = x->len < y->len ? x->len : y->len;
        if (nn == 0) {
            if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
        } else {
            BCMP_PUSH_(a, b, 0, 0);
            a = x->items[0];
            b = y->items[0];
            goto descend;
        }
    } else if (a.tag == V_TAG) {
        int c = strcmp(a.as.tv->tag, b.as.tv->tag);
        if (c) { res = c < 0 ? -1 : 1; goto done; }
        Vec *x = a.as.tv, *y = b.as.tv;
        bint nn = x->len < y->len ? x->len : y->len;
        if (nn == 0) {
            if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
        } else {
            BCMP_PUSH_(a, b, 0, 0);
            a = x->items[0];
            b = y->items[0];
            goto descend;
        }
    } else if (a.tag == V_STRUCT) {
        int c = strcmp(a.as.map->type, b.as.map->type);
        if (c) { res = c < 0 ? -1 : 1; goto done; }
        if (vclosure_env_map(a.as.map) || vclosure_env_map(b.as.map)) {
            if (a.as.map->bad_key != b.as.map->bad_key) {
                res = a.as.map->bad_key < b.as.map->bad_key ? -1 : 1;
                goto done;
            }
            bint nn = a.as.map->len < b.as.map->len ? a.as.map->len : b.as.map->len;
            bint k = 0;
            while (k < nn && k == a.as.map->bad_key) k++;
            if (k >= nn) {
                if (a.as.map->len != b.as.map->len) { res = a.as.map->len < b.as.map->len ? -1 : 1; goto done; }
            } else {
                BCMP_PUSH_(a, b, k, 2);
                a = a.as.map->vals[k];
                b = b.as.map->vals[k];
                goto descend;
            }
        } else {
            bint nn = a.as.map->len < b.as.map->len ? a.as.map->len : b.as.map->len;
            if (nn == 0) {
                if (a.as.map->len != b.as.map->len) { res = a.as.map->len < b.as.map->len ? -1 : 1; goto done; }
            } else {
                bint ka = 0, kb = 0;
                while (ka < a.as.map->used && a.as.map->keys[ka].tag == V_MOVED) ka++;
                while (kb < b.as.map->used && b.as.map->keys[kb].tag == V_MOVED) kb++;
                if (ka >= a.as.map->used || kb >= b.as.map->used) {
                    if (a.as.map->len != b.as.map->len) { res = a.as.map->len < b.as.map->len ? -1 : 1; goto done; }
                } else {
                    BCMP_PUSH2_(a, b, ka, kb, 0);
                    a = a.as.map->keys[ka];
                    b = b.as.map->keys[kb];
                    goto descend;
                }
            }
        }
    }

    while (sp > 0) {
        BPairFrame *top = &stk[sp - 1];
        if (top->a.tag == V_VEC) {
            Vec *x = top->a.as.vec, *y = top->b.as.vec;
            bint nn = x->len < y->len ? x->len : y->len;
            bint nx = ++top->idx;
            if (nx < nn) {
                a = x->items[nx];
                b = y->items[nx];
                goto descend;
            }
            if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
            sp--;
        } else if (top->a.tag == V_TAG) {
            Vec *x = top->a.as.tv, *y = top->b.as.tv;
            bint nn = x->len < y->len ? x->len : y->len;
            bint nx = ++top->idx;
            if (nx < nn) {
                a = x->items[nx];
                b = y->items[nx];
                goto descend;
            }
            if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
            sp--;
        } else {
            Map *x = top->a.as.map, *y = top->b.as.map;
            if (top->phase == 2) {
                bint nn = x->len < y->len ? x->len : y->len;
                bint nx = ++top->idx;
                while (nx < nn && nx == x->bad_key) nx = ++top->idx;
                if (nx < nn) {
                    a = x->vals[nx];
                    b = y->vals[nx];
                    goto descend;
                }
                if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
                sp--;
                continue;
            }
            if (top->phase == 0) {
                top->phase = 1;
                a = x->vals[top->idx];
                b = y->vals[top->idx2];
                goto descend;
            }
            bint nxa = ++top->idx;
            bint nxb = ++top->idx2;
            while (nxa < x->used && x->keys[nxa].tag == V_MOVED) nxa = ++top->idx;
            while (nxb < y->used && y->keys[nxb].tag == V_MOVED) nxb = ++top->idx2;
            top->phase = 0;
            if (nxa < x->used && nxb < y->used) {
                a = x->keys[nxa];
                b = y->keys[nxb];
                goto descend;
            }
            if (x->len != y->len) { res = x->len < y->len ? -1 : 1; goto done; }
            sp--;
        }
    }
#undef BCMP_PUSH_
#undef BCMP_PUSH2_
done:
    if (stk != sbuf) free(stk);
    return res;
}

BFN int vcmp(Val a, Val b) {
    return vcmp_(a, b, "<");
}

BFN_HOT int vcmpt(Val a, Val b, const char *op) {
    /* Two integers are the hot case -- a loop condition -- and they cannot be
       a type error, so the answer needs no walk and no operator string. */
    if (a.tag == V_INT && b.tag == V_INT)
        return a.as.ival < b.as.ival ? -1 : a.as.ival > b.as.ival;
    if (vcmp_is_num(a) && vcmp_is_num(b)) {
        if ((a.tag == V_FLOAT && a.as.fval != a.as.fval) ||
            (b.tag == V_FLOAT && b.as.fval != b.as.fval))
            return (op && op[0] == '<') ? 1 : -1;
        return vcmp_num(a, b);
    }
    return vcmp_(a, b, op);
}

BFN int vcontains(Val c, Val x) {
    bint i;
    if (c.tag == V_VEC) {
        for (i = 0; i < c.as.vec->len; i++) if (veq(c.as.vec->items[i], x)) return 1;
        return 0;
    }
    if (c.tag == V_STR && x.tag == V_STR) return strstr(vstr_c(&c), vstr_c(&x)) != 0;
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
    if (v.tag == V_STR && !v.sso) return v.as.str ? v.as.str : "";
    char *b = vtxt_buf[vtxt_next++ % VTXT_SLOTS];
    if (v.tag == V_STR) {
        size_t sl = vstr_sso_len(&v);
        memcpy(b, vstr_c(&v), sl + 1);
        return b;
    }
    if (v.tag == V_INT) { snprintf(b, 512, BINT_FMT, (bint)v.as.ival); return b; }
    if (v.tag == V_FLOAT) { snprintf(b, 512, "%s", vfmt_double(v.as.fval)); return b; }
    bint nn = vsprint_len_q(v, 0);
    if (verr_pending()) return "";
    if (nn < 511) {
        bint wrote = vsprint_q(v, b, 0);
        b[wrote] = 0;
        return b;
    }

    char *heap = (char *)malloc((size_t)nn + 1);
    if (!heap) vfatal_oom();
    bint wrote = vsprint_q(v, heap, 0);
    heap[wrote] = 0;
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
BFN_HOT Val vadd(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival + (uint64_t)b.as.ival));
    if (a.tag == V_FLOAT && b.tag == V_FLOAT)
        return vfloat(a.as.fval + b.as.fval);
    return vadd_slow(a, b);
}
static Val vadd_slow(Val a, Val b) {
    if (a.tag == V_STR || b.tag == V_STR) {
        const char *sa = (a.tag == V_STR) ? vstr_c(&a) : vtxt(a);
        if (verr_pending()) return vunwind;
        const char *sb = (b.tag == V_STR) ? vstr_c(&b) : vtxt(b);
        if (verr_pending()) return vunwind;
        size_t la = (a.tag == V_STR) ? vstr_len_fast(&a) : strlen(sa);
        size_t lb = (b.tag == V_STR) ? vstr_len_fast(&b) : strlen(sb);
        if (la + lb <= BRAT_SSO_MAX) {
            Val r;
            memset(&r, 0, sizeof(r));
            r.tag = V_STR; r.sso = 1; r.imm = 1;
            char *dst = (char *)&r + 1;
            if (la) memcpy(dst, sa, la);
            if (lb) memcpy(dst + la, sb, lb);
            dst[la + lb] = '\0';
            ((unsigned char *)&r)[15] = (unsigned char)(BRAT_SSO_MAX - (la + lb));
            return r;
        }
        if (la > BRAT_MAX_STR || lb > BRAT_MAX_STR - la) { vraise("out of memory"); return vunwind; }

        char *out = (char *)balloc(la + lb + 1);
        memcpy(out, sa, la);
        memcpy(out + la, sb, lb);
        out[la + lb] = 0;
        return vstr_buf(out, (bint)(la + lb));
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
   vadd. */
static Val vadd_self_slow(Val a, Val b) {
    if (a.tag == V_STR && a.sso) {
        const char *sb = (b.tag == V_STR) ? vstr_c(&b) : vtxt(b);
        if (verr_pending()) return vunwind;
        size_t la = vstr_sso_len(&a);
        size_t lb = (b.tag == V_STR) ? vstr_len_fast(&b) : strlen(sb);
        if (la + lb <= BRAT_SSO_MAX) {
            char *dst = (char *)&a + 1;
            if (lb) memmove(dst + la, sb, lb);
            dst[la + lb] = '\0';
            ((unsigned char *)&a)[15] = (unsigned char)(BRAT_SSO_MAX - (la + lb));
            return a;
        }
        if (la > BRAT_MAX_STR || lb > BRAT_MAX_STR - la) { vraise("out of memory"); return vunwind; }
        size_t need = la + lb + 1;
        size_t cap = need < 32 ? 32 : need * 2;
        if (cap < need) cap = need;
        char *out = (char *)balloc(cap);
        if (la) memcpy(out, vstr_c(&a), la);
        if (lb) memcpy(out + la, sb, lb);
        out[la + lb] = '\0';
        return vstr_buf(out, (bint)(la + lb));
    } else if (a.tag == V_STR && a.as.str) {
        const char *sb = (b.tag == V_STR) ? vstr_c(&b) : vtxt(b);
        if (verr_pending()) return vunwind;
        if (sb) {
            BHead *h = (BHead *)a.as.str - 1;
            if (!BIS_ARENA(h)) {
                bint rc = BRC_VAL(h);
                if (rc == 1) {
                    size_t la = (size_t)h->len;
                    size_t lb = (b.tag == V_STR) ? vstr_len_fast(&b) : strlen(sb);
                    int self_alias = (sb >= a.as.str && sb <= a.as.str + la);
                    size_t sb_off = self_alias ? (size_t)(sb - a.as.str) : 0;
                    if (la > BRAT_MAX_STR || lb > BRAT_MAX_STR - la) {
                        vraise("out of memory");
                        return vunwind;
                    }
                    size_t need = la + lb + 1;
                    if ((size_t)h->cap < need) {
                        size_t nc = h->cap > 0 ? (size_t)h->cap : 32;
                        while (nc < need) nc *= 2;
                        BHead *nh = (BHead *)realloc(h, sizeof(BHead) + nc);
                        if (!nh) vfatal_oom();
                        nh->cap = (bint)nc;
                        a.as.str = (char *)nh + sizeof(BHead);
                        h = nh;
                    }
                    if (self_alias) sb = a.as.str + sb_off;
                    if (lb) memmove(a.as.str + la, sb, lb);
                    a.as.str[la + lb] = 0;
                    h->len = (bint)(la + lb);
                    h->cplen = 0;
                    h->ascii = 0;
                    h->hash = 0;
                    /* rc stays 1: this function owns the release of the old value */
                    return a;
                }
            }
        }
    }
    Val _r = vadd_slow(a, b);
    if (!verr_pending()) vrc_dec(a);
    return _r;
}
BFN_HOT Val vadd_self(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival + (uint64_t)b.as.ival));
    if (a.tag == V_FLOAT && b.tag == V_FLOAT)
        return vfloat(a.as.fval + b.as.fval);
    return vadd_self_slow(a, b);
}


static Val vsub_slow(Val a, Val b);
BFN_HOT Val vsub(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival - (uint64_t)b.as.ival));
    if (a.tag == V_FLOAT && b.tag == V_FLOAT)
        return vfloat(a.as.fval - b.as.fval);
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
BFN_HOT Val vmul(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival * (uint64_t)b.as.ival));
    if (a.tag == V_FLOAT && b.tag == V_FLOAT)
        return vfloat(a.as.fval * b.as.fval);
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
BFN_HOT bint vmod_int(bint x, bint y) {
    if (x == BINT_MIN && y == -1) return 0;
    bint r = x % y;
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return r;
}
BFN_HOT Val vmod(Val a, Val b) {
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
/* SWAR-verified cached UTF-8 codepoint length + 7-bit ASCII flag (`O(1)` after first probe). */
BFN_HOT void vstr_meta(const Val *a, bint *out_cplen, int *out_ascii) {
    if (a->sso) {
        size_t sl = vstr_sso_len(a);
        const unsigned char *u = (const unsigned char *)a + 1;
        unsigned char hi = 0;
        for (size_t i = 0; i < sl; i++) hi |= u[i];
        if ((hi & 0x80u) == 0) {
            *out_cplen = (bint)sl;
            *out_ascii = 1;
            return;
        }
        bint n = 0;
        for (const char *q = (const char *)u; *q; q += utf8_cplen(q)) n++;
        *out_cplen = n;
        *out_ascii = 0;
        return;
    }
    if (!a->as.str) { *out_cplen = 0; *out_ascii = 1; return; }
    BHead *h = (BHead *)a->as.str - 1;
    int asc;
#if defined(__GNUC__) || defined(__clang__)
    asc = __atomic_load_n(&h->ascii, __ATOMIC_ACQUIRE);
#else
    asc = h->ascii;
#endif
    if (asc != 0) {
#if defined(__GNUC__) || defined(__clang__)
        *out_cplen = __atomic_load_n(&h->cplen, __ATOMIC_RELAXED);
#else
        *out_cplen = h->cplen;
#endif
        *out_ascii = (asc > 0) ? 1 : 0;
        return;
    }
    size_t sl = (size_t)h->len;
    const unsigned char *u = (const unsigned char *)a->as.str;
    size_t i = 0;
    bint n = 0;
    while (i + 8 <= sl) {
        uint64_t w;
        memcpy(&w, u + i, 8);
        if ((w & 0x8080808080808080ULL) == 0) {
            n += 8;
            i += 8;
        } else {
            break;
        }
    }
    while (i < sl && (u[i] & 0x80u) == 0) {
        n++;
        i++;
    }
    int is_asc = (i == sl) ? 1 : -1;
    if (i < sl) {
        for (const char *q = (const char *)(u + i); *q; q += utf8_cplen(q)) n++;
    }
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&h->cplen, n, __ATOMIC_RELAXED);
    __atomic_store_n(&h->ascii, is_asc, __ATOMIC_RELEASE);
#else
    h->cplen = n;
    h->ascii = is_asc;
#endif
    *out_cplen = n;
    *out_ascii = (is_asc > 0) ? 1 : 0;
}

/* Compare an indexed codepoint with one ASCII byte without constructing a
 * temporary Val. The caller falls back to vat()/vcmpt() for non-ASCII strings
 * and non-integer indices, preserving the language's Unicode semantics. */
BFN_HOT int vstr_ascii_cmp(Val s, Val idx, unsigned char byte, int *fast) {
    *fast = 0;
    if (s.tag != V_STR || idx.tag != V_INT) return 0;
    bint n = 0;
    int ascii = 0;
    vstr_meta(&s, &n, &ascii);
    if (!ascii) return 0;
    const bint i = idx.as.ival;
    if (i < 0 || i >= n) {
        vraise("index out of range");
        return 0;
    }
    *fast = 1;
    return (int)(unsigned char)vstr_c(&s)[i] - (int)byte;
}

BFN_HOT Val vlen(Val a) {
    if (a.tag == V_STR) {
        bint n = 0;
        int asc = 0;
        vstr_meta(&a, &n, &asc);
        (void)asc;
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
            if (verr_pending()) return vunwind;
            const char *ksc = vstr_c(&ks);
            size_t n = strlen(ksc) + 16;
            char *m = (char *)malloc(n);
            if (!m) vfatal_oom();
            snprintf(m, n, a.as.map->type && a.as.map->type[0] ? "no field %s" : "no key %s", ksc);
            vrc_dec(ks);
            vraise(m);
            free(m);
            return vunwind;
        }
        return *o;
    }
    bint k = 0;
    if (!vindex_int(i, &k)) return vunwind;
    if (a.tag == V_STR) {
        const char *p = vstr_c(&a);
        bint n = 0;
        int asc = 0;
        vstr_meta(&a, &n, &asc);
        if (k < 0 || k >= n) { vraise("index out of range"); return vunwind; }
        if (asc) return vstr_sso(p + k, 1);
        const char *at = utf8_at(p, k);
        return vstrn(at, utf8_cplen(at));
    }
    if (a.tag == V_VEC) {
        BRAT_CTR_LOCK_OBJ(a.as.vec);
        if (k < 0 || k >= a.as.vec->len) { BRAT_CTR_UNLOCK_OBJ(a.as.vec); vraise("index out of range"); return vunwind; }
        Val r = a.as.vec->items[k];
        BRAT_CTR_UNLOCK_OBJ(a.as.vec);
        return r;
    }
    if (a.tag == V_TAG) {
        BRAT_CTR_LOCK_OBJ(a.as.tv);
        if (k < 1 || k > a.as.tv->len) { BRAT_CTR_UNLOCK_OBJ(a.as.tv); vraise("index out of range"); return vunwind; }
        Val r = a.as.tv->items[k - 1];
        BRAT_CTR_UNLOCK_OBJ(a.as.tv);
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

BFN_HOT int verr_pending(void) { return _verr_pending; }
BFN Val verr_take(void) {
    Val e = vunwind;
    vunwind = vnil();
    _verr_pending = 0;
    _verr_exc = 0;
    return e;
}
/* Leave one frame: clear an `err` value, keep a runtime exception pending so
   the enclosing frame (or a try) still sees it. */
BFN Val verr_leave(void) {
    if (!_verr_exc) {
        Val e = vunwind;
        vunwind = vnil();
        _verr_pending = 0;
        return e;
    }
    return vunwind;
}
BFN void verr_clear(void) {
    Val prev = _verr_pending ? vunwind : vnil();
    vunwind = vnil();
    _verr_pending = 0;
    _verr_exc = 0;
    _verr_line = 0;
    vrelease(prev);
}
/* A catch binds the first argument of the error it took and releases the
   envelope immediately while returning an owned reference to the payload. */
BFN Val verr_catch_part(void) {
    Val e = verr_take();
    _verr_line = 0;
    Val out = vnil();
    if (e.tag == V_TAG && e.as.tv && e.as.tv->len) out = vrc_inc(e.as.tv->items[0]);
    vrelease(e);
    return out;
}
BFN void verr_catch_discard(void) {
    Val e = verr_take();
    _verr_line = 0;
    vrelease(e);
}
BFN void verr_catch_end(void) {
    if (verr_pending()) return;
}
BFN void vunwind_disarm(void) { /* no-op: pending channel has no jmp_buf */ }
BFN void vunwind_do(void) { _verr_pending = 1; _verr_exc = 1; }

/* The error owns its message and argument vector. A caught or stored error must not
   change when a later raise happens on the same thread, so nothing here is static. */
BFN void vraise(const char *msg) {
    if (_verr_pending) return;  /* first error wins: a poisoned follow-up must not replace it */
    int saved = barena_depth;
    barena_depth = 0;
    Vec *a = vvec();
    vec_push(a, vstr(_verror_text(msg)));
    vunwind = vtagv("err", a);
    barena_depth = saved;
    _verr_line = _btrace_cur_line;
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
static Val vown_copy(Val v);
BFN void vraisev(Val v) {
    int saved = barena_depth;
    barena_depth = 0;
    if (saved) {
        Val hv = vown_copy(v);
        vrc_dec(v);
        if (_verr_pending) { barena_depth = saved; return; }
        v = hv;
    }
    int was_pending = _verr_pending;
    Val prev = was_pending ? vunwind : vnil();
    _verr_pending = 0;
    vunwind = vnil();
    vrelease(prev);
    Vec *a = vvec();
    vec_push(a, v);  /* error channel outlives any active arena */
    vunwind = vtagv("err", a);
    barena_depth = saved;
    if (!was_pending) _verr_line = _btrace_cur_line;
    _verr_pending = 1;
    _verr_exc = 0;
}
/* Language recursion budget: BRAT frames, not C frames. push returns nonzero, without counting
   the frame, once the budget is spent or, on Linux, once this thread's stack is within
   BSTACK_MARGIN of its end: a budget of a million frames on an 8 MiB stack would otherwise end
   in SIGSEGV instead of this error. The margin is what the code that raises the error and
   unwinds needs, with room for a runtime call that recurses on its own. */
static BTLS bint _bdepth;
static bint _bbudget = 2000;
#if defined(__linux__)
#define BSTACK_MARGIN ((size_t)256 * 1024)
/* A GNU extension that glibc and musl both export; the runtime asks for POSIX only, so no header
   declares it. */
extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
static BTLS char *_bstack_floor;
static BTLS int _bstack_probed;
BFN void bstack_probe(void) {
    pthread_attr_t at;
    void *lo = NULL;
    size_t size = 0;
    _bstack_probed = 1;
    if (pthread_getattr_np(pthread_self(), &at) != 0) return;
    if (pthread_attr_getstack(&at, &lo, &size) == 0 && size > 2 * BSTACK_MARGIN)
        _bstack_floor = (char *)lo + BSTACK_MARGIN;
    pthread_attr_destroy(&at);
}
#endif
BFN void bdepth_init(void) {
    const char *e = getenv("BRAT_RECURSION_BUDGET");
    if (e && *e) {
        char *end = NULL;
        errno = 0;
        long long v = strtoll(e, &end, 10);
        if (errno == 0 && end && *end == '\0' && v > 0 && v <= 1000000)
            _bbudget = (bint)v;
    }
}
BFN int bdepth_push(void) {
    if (_bdepth >= _bbudget) return 1;
#if defined(__linux__)
    if (!_bstack_probed) bstack_probe();
    if (_bstack_floor && (char *)__builtin_frame_address(0) < _bstack_floor) return 1;
#endif
    _bdepth++;
    return 0;
}
BFN void bdepth_pop(void) { _bdepth--; }
static bint vargc_g;
static char **vargv_g;
static char vbase_g[4096];
static int vbase_ready;

static void _vslash_norm(char *s) {
#if defined(_WIN32)
    for (; *s; s++) if (*s == '\\') *s = '/';
#else
    (void)s;
#endif
}
static int _vis_abs_path(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '/') return 1;
#if defined(_WIN32)
    if (p[0] == '\\') return 1;
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\' || p[2] == 0))
        return 1;
#endif
    return 0;
}
static char *_vrealpath(const char *path, char *resolved) {
#if defined(_WIN32)
    if (!path || !*path) return NULL;
    wchar_t wpath[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 4096) <= 0)
        return NULL;
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) return NULL;
    HANDLE h = CreateFileW(wpath, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    wchar_t wfin[4096];
    DWORD wn = GetFinalPathNameByHandleW(h, wfin, 4096, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (wn == 0 || wn >= 4096) return NULL;
    const wchar_t *ws = wfin;
    wchar_t unc_buf[4096];
    if (wcsncmp(ws, L"\\\\?\\UNC\\", 8) == 0) {
        _snwprintf(unc_buf, 4095, L"\\\\%ls", ws + 8);
        unc_buf[4095] = 0;
        ws = unc_buf;
    } else if (wcsncmp(ws, L"\\\\?\\", 4) == 0) {
        ws += 4;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, resolved, 4096, NULL, NULL) <= 0)
        return NULL;
    _vslash_norm(resolved);
    return resolved;
#else
    return realpath(path, resolved);
#endif
}

BFN void vbase_set(const char *b) {
    if (!b || !*b) return;
    char res[4096];
    const char *rp = _vrealpath(b, res);
    const char *use = rp ? rp : b;
    strncpy(vbase_g, use, sizeof vbase_g - 1);
    vbase_g[sizeof vbase_g - 1] = 0;
    _vslash_norm(vbase_g);
    vbase_ready = 1;
}
BFN void vargs_set(bint c, char **v) {
    vargc_g = c;
    vargv_g = v;
    
    if (!vbase_ready) {
        char tmp[4096];
        const char *src = (c > 0 && v && v[0]) ? v[0] : ".";
        const char *slash = NULL;
#if defined(_WIN32)
        for (const char *q = src; *q; q++) if (*q == '/' || *q == '\\') slash = q;
#else
        for (const char *q = src; *q; q++) if (*q == '/') slash = q;
#endif
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
        const char *rp = _vrealpath(tmp, res);
        if (rp) { strncpy(vbase_g, rp, sizeof vbase_g - 1); vbase_g[sizeof vbase_g - 1] = 0; }
        else { strncpy(vbase_g, tmp, sizeof vbase_g - 1); vbase_g[sizeof vbase_g - 1] = 0; }
        _vslash_norm(vbase_g);
        vbase_ready = 1;
    }
}

/* Returns 0 on success, -1 when a path has more components than the cap: a
   silently truncated path could point somewhere else entirely. */
BFN int vnorm(char *s) {
    _vslash_norm(s);
    char drive[4] = {0};
    char *body = s;
#if defined(_WIN32)
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        drive[0] = s[0]; drive[1] = ':'; drive[2] = 0;
        body = s + 2;
    }
#endif
    int abs = (*body == '/') || (drive[0] != 0);
    char *comps[256];
    int n = 0;
    char *cur = body;
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
    if (drive[0]) { *o++ = drive[0]; *o++ = ':'; }
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
    if (_vis_abs_path(p)) snprintf(res, sizeof res, "%s", p);
    else snprintf(res, sizeof res, "%s/%s", vbase_g, p);
    _vslash_norm(res);

    char real[8192];
    if (_vrealpath(res, real)) {
        snprintf(res, sizeof res, "%s", real);
    } else {
        if (vnorm(res) != 0) return NULL;
#if defined(_WIN32)
        char dir[8192];
        snprintf(dir, sizeof dir, "%s", res);
        for (char *slash = strrchr(dir, '/'); slash && slash > dir + 2; slash = strrchr(dir, '/')) {
            size_t off = (size_t)(slash - dir);
            *slash = 0;
            if (_vrealpath(dir, real)) {
                char combined[8192];
                int cn = snprintf(combined, sizeof combined, "%s%s", real, res + off);
                if (cn < 0 || (size_t)cn >= sizeof res) return NULL;
                memcpy(res, combined, (size_t)cn + 1);
                if (vnorm(res) != 0) return NULL;
                break;
            }
        }
        wchar_t wchk[4096];
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, res, -1, wchk, 4096) <= 0)
            return NULL;
        DWORD lattr = GetFileAttributesW(wchk);
        if (lattr != INVALID_FILE_ATTRIBUTES && (lattr & FILE_ATTRIBUTE_REPARSE_POINT))
            return NULL;
#else
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
#if defined(_WIN32)
    if (_strnicmp(res, vbase_g, bl) == 0 && (res[bl] == '/' || res[bl] == 0)) return res;
#else
    if (strncmp(res, vbase_g, bl) == 0 && (res[bl] == '/' || res[bl] == 0)) return res;
#endif
    return NULL;
}

/* Re-check the opened file's resolved path against the program directory, and
 * prevent child processes from inheriting open file descriptors/handles. */
#if defined(_WIN32)
static int _vhandle_confined(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h) return 0;
    SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    if (_vfiles_free()) return 1;
    wchar_t wfin[4096];
    DWORD wn = GetFinalPathNameByHandleW(h, wfin, 4096, FILE_NAME_NORMALIZED);
    if (wn == 0 || wn >= 4096) return 0;
    const wchar_t *ws = wfin;
    wchar_t unc_buf[4096];
    if (wcsncmp(ws, L"\\\\?\\UNC\\", 8) == 0) {
        _snwprintf(unc_buf, 4095, L"\\\\%ls", ws + 8);
        unc_buf[4095] = 0;
        ws = unc_buf;
    } else if (wcsncmp(ws, L"\\\\?\\", 4) == 0) {
        ws += 4;
    }
    char target[4096];
    if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, target, sizeof target, NULL, NULL) <= 0)
        return 0;
    _vslash_norm(target);
    size_t bl = strlen(vbase_g);
    return _strnicmp(target, vbase_g, bl) == 0 && (target[bl] == '/' || target[bl] == 0);
}
#endif

static int _vfd_confined(int fd) {
    if (fd < 0) return 0;
#if defined(_WIN32)
    intptr_t osfh = _get_osfhandle(fd);
    if (osfh == -1) return 0;
    return _vhandle_confined((HANDLE)osfh);
#else
    {
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    if (_vfiles_free()) return 1;
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
#endif
}

#if !defined(_WIN32)
/* Walk from `vbase_g` to the parent directory of canonical `rp` using `openat`
 * with `O_NOFOLLOW | O_DIRECTORY` so a concurrent directory-to-symlink swap
 * between `vconfine()` and the leaf syscall fails closed with `ELOOP`. */
static int _vopen_parent_confined(const char *rp, char *leaf_out, size_t leaf_cap, int *escaped) {
    if (escaped) *escaped = 0;
    size_t bl = strlen(vbase_g);
    if (strncmp(rp, vbase_g, bl) != 0 || rp[bl] != '/' || rp[bl + 1] == 0) {
        errno = (rp[bl] == 0) ? EISDIR : EACCES;
        return -1;
    }
    int dfd = open(vbase_g, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        if (errno == ELOOP && escaped) *escaped = 1;
        return -1;
    }
    if (!_vfd_confined(dfd)) {
        close(dfd);
        if (escaped) *escaped = 1;
        errno = EACCES;
        return -1;
    }
    const char *cur = rp + bl + 1;
    for (;;) {
        while (*cur == '/') cur++;
        const char *sl = strchr(cur, '/');
        if (!sl) {
            size_t llen = strlen(cur);
            if (llen == 0 || llen >= leaf_cap || strcmp(cur, ".") == 0 || strcmp(cur, "..") == 0) {
                close(dfd);
                errno = EISDIR;
                return -1;
            }
            memcpy(leaf_out, cur, llen + 1);
            return dfd;
        }
        size_t slen = (size_t)(sl - cur);
        if (slen == 0 || slen >= 1024 || (slen == 1 && cur[0] == '.') ||
            (slen == 2 && cur[0] == '.' && cur[1] == '.')) {
            close(dfd);
            if (escaped) *escaped = 1;
            errno = EACCES;
            return -1;
        }
        char seg[1024];
        memcpy(seg, cur, slen);
        seg[slen] = 0;
        int nfd = openat(dfd, seg, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int e = errno;
        close(dfd);
        if (nfd < 0) {
            if (e == ELOOP && escaped) *escaped = 1;
            errno = e;
            return -1;
        }
        dfd = nfd;
        cur = sl + 1;
    }
}
#endif

static FILE *_vopen_confined_stream(const char *rp, int write_mode, size_t max_bytes,
                                    int *escaped, int *too_large) {
    if (escaped) *escaped = 0;
    if (too_large) *too_large = 0;
    errno = 0;
#if defined(_WIN32)
    wchar_t wpath[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rp, -1, wpath, 4096) <= 0) {
        errno = EINVAL;
        return NULL;
    }
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = FALSE;
    DWORD access = write_mode ? GENERIC_WRITE : GENERIC_READ;
    DWORD disp = write_mode ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (!_vfiles_free()) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(wpath, access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           &sa, disp, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
                ? ENOENT : EACCES;
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &fi) ||
        (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        errno = EISDIR;
        return NULL;
    }
    if (!_vfiles_free() &&
        ((fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !_vhandle_confined(h))) {
        CloseHandle(h);
        if (escaped) *escaped = 1;
        errno = EACCES;
        return NULL;
    }
    if (!write_mode && max_bytes > 0) {
        uint64_t sz = ((uint64_t)fi.nFileSizeHigh << 32) | (uint64_t)fi.nFileSizeLow;
        if (sz > (uint64_t)max_bytes) {
            CloseHandle(h);
            if (too_large) *too_large = 1;
            return NULL;
        }
    }
    if (write_mode) {
        LARGE_INTEGER z;
        z.QuadPart = 0;
        if (!SetFilePointerEx(h, z, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h);
            errno = EIO;
            return NULL;
        }
    }
    int fd = _open_osfhandle((intptr_t)h, (write_mode ? _O_WRONLY : _O_RDONLY) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { CloseHandle(h); errno = EIO; return NULL; }
    FILE *f = _fdopen(fd, write_mode ? "wb" : "rb");
    if (!f) { _close(fd); errno = EIO; return NULL; }
    return f;
#else
    int fd = -1;
    if (_vfiles_free()) {
        int fl = (write_mode ? (O_WRONLY | O_CREAT) : O_RDONLY) | O_CLOEXEC;
        fd = open(rp, fl, 0666);
        if (fd < 0) return NULL;
    } else {
        char leaf[1024];
        int dfd = _vopen_parent_confined(rp, leaf, sizeof leaf, escaped);
        if (dfd < 0) return NULL;
        int fl = (write_mode ? (O_WRONLY | O_CREAT) : O_RDONLY) | O_CLOEXEC;
#ifdef O_NOFOLLOW
        fl |= O_NOFOLLOW;
#endif
        fd = openat(dfd, leaf, fl, 0666);
        int e = errno;
        close(dfd);
        if (fd < 0) {
            if (e == ELOOP && escaped) *escaped = 1;
            errno = e;
            return NULL;
        }
        if (!_vfd_confined(fd)) {
            close(fd);
            if (escaped) *escaped = 1;
            errno = EACCES;
            return NULL;
        }
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); errno = e; return NULL; }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return NULL;
    }
    if (!write_mode && max_bytes > 0 && st.st_size > 0 && (uint64_t)st.st_size > (uint64_t)max_bytes) {
        close(fd);
        if (too_large) *too_large = 1;
        return NULL;
    }
    if (write_mode && ftruncate(fd, 0) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    FILE *f = fdopen(fd, write_mode ? "wb" : "rb");
    if (!f) { int e = errno; close(fd); errno = e; return NULL; }
    return f;
#endif
}
BFN Val vargs(void) {
    for (bint i = 1; i < vargc_g; i++) {
        const char *a = vargv_g[i] ? vargv_g[i] : "";
        if (!utf8_valid(a, (bint)strlen(a))) {
            char m[96];
            snprintf(m, sizeof m, "invalid utf-8 in argument %lld; use args_bytes()",
                     (long long)i);
            vraise(m);
            return vunwind;
        }
    }
    Vec *o = vvec();
    for (bint i = 1; i < vargc_g; i++) vec_push(o, vstr(vargv_g[i] ? vargv_g[i] : ""));
    return vvecv(o);
}
BFN Val vargs_bytes(void) {
    Vec *o = vvec();
    for (bint i = 1; i < vargc_g; i++) {
        const unsigned char *a = (const unsigned char *)(vargv_g[i] ? vargv_g[i] : "");
        size_t n = strlen((const char *)a);
        if (n > BRAT_MAX_BYTE_VEC) {
            vrc_dec(vvecv(o));
            vraise("args_bytes: argument exceeds 16 MiB byte vector limit");
            return vunwind;
        }
        Vec *b = vvec_cap((bint)n);
        for (size_t j = 0; j < n; j++) vec_push(b, vint((bint)a[j]));
        vec_push(o, vvecv(b));
    }
    return vvecv(o);
}
#define BEXIT_ROOTS_MAX 128
static Val *_bexit_roots[BEXIT_ROOTS_MAX];
static int _bexit_roots_n = 0;
static Vec *_bexit_main_args = 0;
BFN void bexit_track_args(Vec *a) { _bexit_main_args = a; }
BFN void bexit_track_val(Val *vp) {
    if (_bexit_roots_n < BEXIT_ROOTS_MAX) _bexit_roots[_bexit_roots_n++] = vp;
}
BFN void bexit_untrack_all(void) { _bexit_roots_n = 0; _bexit_main_args = 0; }
BFN Val vexit(Val v) {
    int code = (int)vlong(v);
    for (int i = _bexit_roots_n - 1; i >= 0; i--) {
        Val *vp = _bexit_roots[i];
        if (vp && vp->tag != V_MOVED) {
            Val tmp = *vp;
            *vp = vnil();
            vrc_dec(tmp);
        }
    }
    _bexit_roots_n = 0;
    if (_bexit_main_args) {
        Vec *ma = _bexit_main_args;
        _bexit_main_args = 0;
        vrc_dec(vvecv(ma));
    }
    exit(code);
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
BFN Val vmono_ns(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return vint((bint)(t * 100ULL));
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { ts.tv_sec = 0; ts.tv_nsec = 0; }
    return vint((bint)((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec));
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
static int _venv_val_secret(const char *r) {
    if (!r || !*r) return 0;
    if ((strstr(r, "://") && strchr(r, '@')) || strstr(r, "-----BEGIN")) return 1;
    static const char *pfx[] = {
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_",
        "sk-", "sk_live_", "sk_test_", "rk_live_", "rk_test_",
        "xoxb-", "xoxp-", "xoxa-", "xoxr-", "xoxs-", "AIza", 0
    };
    for (int i = 0; pfx[i]; i++) {
        const char *hit = strstr(r, pfx[i]);
        if (hit && strlen(hit) >= strlen(pfx[i]) + 12) return 1;
    }
    for (const char *p = r; *p; p++) {
        if ((strncmp(p, "AKIA", 4) == 0 || strncmp(p, "ASIA", 4) == 0) &&
            (p == r || !isalnum((unsigned char)p[-1]))) {
            int ok = 1;
            for (int k = 4; k < 20; k++) {
                if (!isupper((unsigned char)p[k]) && !isdigit((unsigned char)p[k])) { ok = 0; break; }
            }
            if (ok && !isalnum((unsigned char)p[20])) return 1;
        }
    }
    const char *jwt = strstr(r, "eyJ");
    if (jwt && (jwt == r || !isalnum((unsigned char)jwt[-1]))) {
        const char *d1 = strchr(jwt + 3, '.');
        if (d1 && d1 > jwt + 6 && strncmp(d1 + 1, "eyJ", 3) == 0) {
            const char *d2 = strchr(d1 + 4, '.');
            if (d2 && d2 > d1 + 6 && isalnum((unsigned char)d2[1])) return 1;
        }
    }
    return 0;
}

static Val _venv_err(Val v, const char *prefix) {
    const char *key = vstr_c(&v);
    char *o = (char *)malloc(strlen(key) * 2 + strlen(prefix) + 32);
    if (!o) vfatal_oom();
    bint n = vput(o, prefix);
    n += vsprint_q(v, o + n, 1);
    o[n] = 0;
    Vec *e = vvec();
    vec_push(e, vstrn(o, n));
    free(o);
    return vtagv("err", e);
}

static Val _venv_lookup(Val v, int want_bytes) {
    if (v.tag != V_STR || vstr_has_nul(&v)) return vnil();
    const char *key = vstr_c(&v);
    if (strchr(key, '=')) return vnil();
    int blocked = 0;
    const char *deny_all = getenv("BRAT_ENV_DENY_ALL");
    if (deny_all && (strcmp(deny_all, "1") == 0 || strcmp(deny_all, "true") == 0))
        blocked = 1;
    const char *allow = getenv("BRAT_ENV_ALLOW");
    if (allow != NULL && !blocked) {
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
        "GITHUB_", "GITLAB_", "OPENAI_", "ANTHROPIC_", "STRIPE_", "SLACK_",
        "CLOUDFLARE_", "VAULT_", "KUBECONFIG", "WEBHOOK", "SIGNING",
        "CERT", "SALT", "SEED", 0
    };
    for (int b = 0; deny[b] && !blocked; b++) blocked = strstr(upper, deny[b]) != 0;
    free(upper);
    const char *r = blocked ? NULL : getenv(key);
    if (!blocked && _venv_val_secret(r)) blocked = 1;
    if (blocked) return _venv_err(v, "env_get refused: ");
    if (!r) return vnil();
    if (want_bytes) {
        size_t n = strlen(r);
        if (n > BRAT_MAX_BYTE_VEC) {
            vraise("env_get_bytes: value exceeds 16 MiB byte vector limit");
            return vunwind;
        }
        Vec *o = vvec_cap((bint)n);
        for (size_t j = 0; j < n; j++) vec_push(o, vint((bint)(unsigned char)r[j]));
        return vvecv(o);
    }
    if (!utf8_valid(r, (bint)strlen(r))) return _venv_err(v, "env_get invalid utf-8: ");
    return vstr(r);
}

BFN Val venv_get(Val v) { return _venv_lookup(v, 0); }
BFN Val venv_get_bytes(Val v) { return _venv_lookup(v, 1); }

typedef struct { char kind; char q; char ch; const char *body; } RTok;

static int rtok_match(RTok *t, char ch) {
    unsigned char uch = (unsigned char)ch;
    if (t->kind == 'l') return uch == (unsigned char)t->ch;
    if (t->kind == 'd') return 1;
    if (t->kind == 's') {
        const char *body = t->body;
        int neg = 0, found = 0;
        if (*body == '^') { neg = 1; body++; }
        while (*body) {
            if (body[1] == '-' && body[2]) {
                if (uch >= (unsigned char)body[0] && uch <= (unsigned char)body[2]) { found = 1; break; }
                body += 3;
            } else {
                if ((unsigned char)*body == uch) { found = 1; break; }
                body++;
            }
        }
        return neg ? !found : found;
    }
    return 0;
}


typedef struct { char op; char q; char ch; const char *body; bint x, y; } RInst;
typedef struct {
    RInst *p;
    bint n, cap;
    char **bodies;
    bint nbodies, bodies_cap;
    const char *pat;
    bint i, plen;
    bint ngroups;
    int err;
    int empty_seq;
} RProg;

#define BRAT_RE_MAX_INST 65536
#define BRAT_RE_MAX_REP 1024
#define BRAT_RE_MAX_GROUPS 64

static bint r_emit(RProg *g, char op, bint x, bint y) {
    if (g->n >= BRAT_RE_MAX_INST) { g->err = 1; return 0; }
    if (g->n >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 64;
        g->p = (RInst *)bgrow(g->p, sizeof(RInst) * (size_t)g->cap);
    }
    RInst *in = &g->p[g->n];
    in->op = op; in->q = 0; in->ch = 0; in->body = NULL; in->x = x; in->y = y;
    return g->n++;
}

static const char *r_body_copy(RProg *g, const char *src, bint len) {
    if (g->nbodies >= g->bodies_cap) {
        g->bodies_cap = g->bodies_cap ? g->bodies_cap * 2 : 16;
        g->bodies = (char **)bgrow(g->bodies, sizeof(char *) * (size_t)g->bodies_cap);
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) vfatal_oom();
    memcpy(buf, src, (size_t)len);
    buf[len] = 0;
    g->bodies[g->nbodies++] = buf;
    return buf;
}

static void r_truncate(RProg *g, bint to) {
    g->n = to;
}

static bint r_emit_tpl(RProg *g, const RInst *tpl, bint tpl_len, bint body, bint body_end) {
    bint dst = g->n, delta = dst - body;
    for (bint k = 0; k < tpl_len; k++) {
        bint idx = r_emit(g, tpl[k].op, tpl[k].x, tpl[k].y);
        if (g->err) return -1;
        g->p[idx].q = tpl[k].q;
        g->p[idx].ch = tpl[k].ch;
        g->p[idx].body = tpl[k].body;
        if (tpl[k].op == 'j' && tpl[k].x >= body && tpl[k].x <= body_end)
            g->p[idx].x = tpl[k].x + delta;
        else if (tpl[k].op == 's') {
            if (tpl[k].x >= body && tpl[k].x <= body_end) g->p[idx].x = tpl[k].x + delta;
            if (tpl[k].y >= body && tpl[k].y <= body_end) g->p[idx].y = tpl[k].y + delta;
        }
    }
    return dst;
}

static int r_parse_bounds(RProg *g, bint *lo_out, bint *hi_out) {
    /* Caller verified g->pat[g->i] == '{'; advances past '}'.
     * Sets *hi_out = -1 for unbounded `{n,}`. */
    g->i++;
    if (g->i >= g->plen || !isdigit((unsigned char)g->pat[g->i])) { g->err = 1; return 0; }
    bint lo = 0;
    while (g->i < g->plen && isdigit((unsigned char)g->pat[g->i])) {
        lo = lo * 10 + (g->pat[g->i++] - '0');
        if (lo > BRAT_RE_MAX_REP) { g->err = 1; return 0; }
    }
    if (g->i >= g->plen) { g->err = 1; return 0; }
    if (g->pat[g->i] == '}') {
        g->i++;
        *lo_out = lo;
        *hi_out = lo;
        return 1;
    }
    if (g->pat[g->i] != ',') { g->err = 1; return 0; }
    g->i++;
    if (g->i >= g->plen) { g->err = 1; return 0; }
    if (g->pat[g->i] == '}') {
        g->i++;
        *lo_out = lo;
        *hi_out = -1;
        return 1;
    }
    if (!isdigit((unsigned char)g->pat[g->i])) { g->err = 1; return 0; }
    bint hi = 0;
    while (g->i < g->plen && isdigit((unsigned char)g->pat[g->i])) {
        hi = hi * 10 + (g->pat[g->i++] - '0');
        if (hi > BRAT_RE_MAX_REP) { g->err = 1; return 0; }
    }
    if (g->i >= g->plen || g->pat[g->i] != '}' || hi < lo) { g->err = 1; return 0; }
    g->i++;
    *lo_out = lo;
    *hi_out = hi;
    return 1;
}

static bint r_alt(RProg *g);

static bint r_atom(RProg *g) {
    bint sp = r_emit(g, 's', 0, 0);
    bint body = -1;
    if (g->err || g->i >= g->plen) { g->err = 1; return -1; }
    char c = g->pat[g->i];
    if (c == '*' || c == '+' || c == '?') { g->err = 1; return -1; }
    if (c == '(') {
        g->i++;
        bint gid = ++g->ngroups;
        bint sv0 = (gid <= BRAT_RE_MAX_GROUPS) ? r_emit(g, 'v', 2 * gid, 0) : -1;
        bint inner = r_alt(g);
        if (g->err) return -1;
        if (g->i >= g->plen || g->pat[g->i] != ')') { g->err = 1; return -1; }
        g->i++;
        if (gid <= BRAT_RE_MAX_GROUPS) r_emit(g, 'v', 2 * gid + 1, 0);
        body = (sv0 >= 0) ? sv0 : inner;
    } else if (c == '[') {
        bint j = g->i + 1;
        bint m = j;
        while (m < g->plen && g->pat[m] != ']') m++;
        if (m >= g->plen) { g->err = 1; return -1; }
        const char *buf = r_body_copy(g, g->pat + j, m - j);
        g->i = m + 1;
        body = r_emit(g, 'c', 0, 0);
        g->p[body].q = 's';
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
    bint body_end = g->n;
    if (g->i < g->plen && g->pat[g->i] == '{') {
        bint lo = 0, hi = 0;
        if (!r_parse_bounds(g, &lo, &hi)) return -1;
        if (hi == 0) {
            r_truncate(g, sp);
            bint nop = r_emit(g, 'j', 0, 0);
            g->p[nop].x = g->n;
            return nop;
        }
        /* Keep a clean template of [body .. body_end) before emitting copies. */
        bint tpl_len = body_end - body;
        RInst *tpl = (RInst *)malloc(sizeof(RInst) * (size_t)(tpl_len > 0 ? tpl_len : 1));
        if (!tpl) vfatal_oom();
        if (tpl_len > 0) memcpy(tpl, &g->p[body], sizeof(RInst) * (size_t)tpl_len);
        r_truncate(g, sp);
        bint entry = r_emit(g, 'j', 0, 0);
        g->p[entry].x = g->n;
        for (bint rep = 0; rep < lo; rep++) {
            r_emit_tpl(g, tpl, tpl_len, body, body_end);
            if (g->err) break;
        }
        if (!g->err && hi < 0) {
            bint star_sp = r_emit(g, 's', 0, 0);
            bint dst = r_emit_tpl(g, tpl, tpl_len, body, body_end);
            r_emit(g, 'j', star_sp, 0);
            if (!g->err) {
                g->p[star_sp].x = dst;
                g->p[star_sp].y = g->n;
            }
        } else if (!g->err && hi > lo) {
            for (bint opt = 0; opt < hi - lo; opt++) {
                bint opt_sp = r_emit(g, 's', 0, 0);
                bint dst = r_emit_tpl(g, tpl, tpl_len, body, body_end);
                if (g->err) break;
                g->p[opt_sp].x = dst;
                g->p[opt_sp].y = g->n;
            }
        }
        free(tpl);
        return g->err ? -1 : entry;
    }
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
        if (g->err) return -1;
        g->p[jmp].x = sp;
        g->p[sp].x = body;
        g->p[sp].y = g->n;
        return sp;
    }
    if (q == '+') {
        g->p[sp].op = 'j';
        g->p[sp].x = body;
        bint sp2 = r_emit(g, 's', 0, 0);
        if (g->err) return -1;
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

static void r_free(RProg *g) {
    for (bint k = 0; k < g->nbodies; k++) free(g->bodies[k]);
    free(g->bodies);
    g->bodies = NULL;
    g->nbodies = 0;
    g->bodies_cap = 0;
    free(g->p);
    g->p = NULL;
    g->n = 0;
    g->cap = 0;
}

typedef struct { bint pc; bint slot; bint prev; } REpsItem;

static int r_compile_prog(RProg *g, const char *pat, bint *start_out) {
    g->p = NULL; g->n = 0; g->cap = 0;
    g->bodies = NULL; g->nbodies = 0; g->bodies_cap = 0;
    g->pat = pat; g->i = 0; g->plen = (bint)strlen(pat);
    g->ngroups = 0; g->err = 0; g->empty_seq = 0;
    bint sv0 = r_emit(g, 'v', 0, 0);
    bint body = r_alt(g);
    if (g->err || body < 0 || g->i != g->plen) { r_free(g); return 0; }
    r_emit(g, 'v', 1, 0);
    r_emit(g, 'm', 0, 0);
    if (g->err) { r_free(g); return 0; }
    for (bint k = 0; k < g->n; k++) {
        if (g->p[k].op == 'j' && g->p[k].x == k) g->p[k].x = g->n - 2;
    }
    *start_out = sv0;
    return 1;
}

static void re_pike_add(RInst *p, bint pc, const char *s, bint si,
                        bint *out_pcs, bint *out_slots, bint *nout,
                        bint *seen, bint *work_slots, bint nslots,
                        REpsItem *stk, bint stk_cap) {
    bint sp = 0;
    stk[sp].pc = pc; stk[sp].slot = 0; stk[sp].prev = 0; sp++;
    while (sp > 0) {
        REpsItem it = stk[--sp];
        if (it.pc == -1) {
            if (it.slot >= 0 && it.slot < nslots) work_slots[it.slot] = it.prev;
            continue;
        }
        bint q = it.pc;
        if (q < 0 || seen[q]) continue;
        seen[q] = 1;
        RInst *in = &p[q];
        if (in->op == 'j') {
            if (sp < stk_cap) { stk[sp].pc = in->x; stk[sp].slot = 0; stk[sp].prev = 0; sp++; }
        } else if (in->op == 's') {
            /* Push low-priority `y` first, high-priority `x` second (LIFO). */
            if (sp + 2 <= stk_cap) {
                stk[sp].pc = in->y; stk[sp].slot = 0; stk[sp].prev = 0; sp++;
                stk[sp].pc = in->x; stk[sp].slot = 0; stk[sp].prev = 0; sp++;
            }
        } else if (in->op == 'b') {
            if (si == 0 && sp < stk_cap) { stk[sp].pc = q + 1; stk[sp].slot = 0; stk[sp].prev = 0; sp++; }
        } else if (in->op == 'e') {
            if (s[si] == 0 && sp < stk_cap) { stk[sp].pc = q + 1; stk[sp].slot = 0; stk[sp].prev = 0; sp++; }
        } else if (in->op == 'v') {
            if (sp + 2 <= stk_cap) {
                bint sl = in->x;
                if (sl >= 0 && sl < nslots) {
                    stk[sp].pc = -1; stk[sp].slot = sl; stk[sp].prev = work_slots[sl]; sp++;
                    work_slots[sl] = si;
                }
                stk[sp].pc = q + 1; stk[sp].slot = 0; stk[sp].prev = 0; sp++;
            }
        } else if (in->op == 'f') {
            /* dead branch */
        } else {
            bint idx = (*nout)++;
            out_pcs[idx] = q;
            if (nslots > 0) memcpy(out_slots + idx * nslots, work_slots, sizeof(bint) * (size_t)nslots);
        }
    }
}

/* Runs Pike VM starting at offset >= min_from; returns 1 on match and fills
 * `best_slots` (length `nslots = 2 * (capped_groups + 1)`). */
static int re_pike_search(RProg *g, bint start, const char *s, bint slen,
                          bint min_from, bint *best_slots, bint nslots) {
    bint nprog = g->n;
    bint stk_cap = nprog * 4 + 32;
    bint *list_pcs = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    bint *next_pcs = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    bint *seen = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    bint *next_seen = (bint *)malloc(sizeof(bint) * (size_t)(nprog + 1));
    size_t slot_elems = (size_t)(nprog + 1) * (size_t)(nslots > 0 ? nslots : 1);
    bint *list_slots = (bint *)malloc(sizeof(bint) * slot_elems);
    bint *next_slots = (bint *)malloc(sizeof(bint) * slot_elems);
    bint *work_slots = (bint *)malloc(sizeof(bint) * (size_t)(nslots > 0 ? nslots : 1));
    REpsItem *stk = (REpsItem *)malloc(sizeof(REpsItem) * (size_t)stk_cap);
    if (!list_pcs || !next_pcs || !seen || !next_seen || !list_slots || !next_slots || !work_slots || !stk) {
        free(list_pcs); free(next_pcs); free(seen); free(next_seen);
        free(list_slots); free(next_slots); free(work_slots); free(stk);
        vfatal_oom();
    }
    int matched = 0;
    bint nlist = 0;
    memset(seen, 0, sizeof(bint) * (size_t)(nprog + 1));
    for (bint si = min_from; si <= slen; si++) {
        if (!matched) {
            for (bint z = 0; z < nslots; z++) work_slots[z] = -1;
            re_pike_add(g->p, start, s, si, list_pcs, list_slots, &nlist,
                        seen, work_slots, nslots, stk, stk_cap);
        }
        bint nn = 0;
        memset(next_seen, 0, sizeof(bint) * (size_t)(nprog + 1));
        for (bint k = 0; k < nlist; k++) {
            bint pc = list_pcs[k];
            RInst *in = &g->p[pc];
            if (in->op == 'm') {
                matched = 1;
                if (nslots > 0 && best_slots)
                    memcpy(best_slots, list_slots + k * nslots, sizeof(bint) * (size_t)nslots);
                break;
            }
            if (si >= slen || in->op != 'c') continue;
            if (in->q == 'l') { if (s[si] != in->ch) continue; }
            else if (in->q == 'd') { }
            else {
                RTok t; t.kind = 's'; t.q = 0; t.ch = 0; t.body = in->body;
                if (!rtok_match(&t, s[si])) continue;
            }
            if (nslots > 0)
                memcpy(work_slots, list_slots + k * nslots, sizeof(bint) * (size_t)nslots);
            re_pike_add(g->p, pc + 1, s, si + 1, next_pcs, next_slots, &nn,
                        next_seen, work_slots, nslots, stk, stk_cap);
        }
        if (si >= slen) break;
        bint *tp = list_pcs; list_pcs = next_pcs; next_pcs = tp;
        bint *ts = list_slots; list_slots = next_slots; next_slots = ts;
        bint *tv = seen; seen = next_seen; next_seen = tv;
        nlist = nn;
        if (matched && nlist == 0) break;
    }
    free(list_pcs); free(next_pcs); free(seen); free(next_seen);
    free(list_slots); free(next_slots); free(work_slots); free(stk);
    return matched;
}

BFN int vre_match(Val sv, Val pv) {
    if (sv.tag != V_STR || pv.tag != V_STR) { vraise("re_match wants str"); return 0; }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog(&g, pat, &start)) { vraise("bad regex pattern"); return 0; }
    bint slots[2] = {-1, -1};
    int ok = re_pike_search(&g, start, s, (bint)strlen(s), 0, slots, 2);
    r_free(&g);
    return ok;
}

BFN Val vre_find(Val sv, Val pv) {
    if (sv.tag != V_STR || pv.tag != V_STR) { vraise("re_find wants str"); return vnil(); }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog(&g, pat, &start)) { vraise("bad regex pattern"); return vnil(); }
    bint slots[2] = {-1, -1};
    int ok = re_pike_search(&g, start, s, (bint)strlen(s), 0, slots, 2);
    r_free(&g);
    if (!ok || slots[0] < 0 || slots[1] < slots[0]) return vnil();
    Vec *a = vvec();
    vec_push(a, vstrn(s + slots[0], slots[1] - slots[0]));
    return vtagv("some", a);
}

BFN Val vre_captures(Val sv, Val pv) {
    if (sv.tag != V_STR || pv.tag != V_STR) { vraise("re_captures wants str"); return vnil(); }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog(&g, pat, &start)) { vraise("bad regex pattern"); return vnil(); }
    bint ng = g.ngroups > BRAT_RE_MAX_GROUPS ? BRAT_RE_MAX_GROUPS : g.ngroups;
    bint nslots = 2 * (ng + 1);
    bint *slots = (bint *)malloc(sizeof(bint) * (size_t)nslots);
    if (!slots) { r_free(&g); vfatal_oom(); }
    for (bint z = 0; z < nslots; z++) slots[z] = -1;
    int ok = re_pike_search(&g, start, s, (bint)strlen(s), 0, slots, nslots);
    r_free(&g);
    if (!ok || slots[0] < 0 || slots[1] < slots[0]) { free(slots); return vnil(); }
    Vec *groups = vvec();
    for (bint k = 0; k <= ng; k++) {
        bint a0 = slots[2 * k], a1 = slots[2 * k + 1];
        if (a0 >= 0 && a1 >= a0) vec_push(groups, vstrn(s + a0, a1 - a0));
        else vec_push(groups, vstr(""));
    }
    free(slots);
    Vec *wrap = vvec();
    vec_push(wrap, vvecv(groups));
    return vtagv("some", wrap);
}

static void _vre_buf_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (n == 0) return;
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) vfatal_oom();
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = 0;
}

BFN Val vre_replace(Val sv, Val pv, Val rv) {
    if (sv.tag != V_STR || pv.tag != V_STR || rv.tag != V_STR) {
        vraise("re_replace wants str");
        return vnil();
    }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv), *repl = vstr_c(&rv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog(&g, pat, &start)) { vraise("bad regex pattern"); return vnil(); }
    bint slen = (bint)strlen(s), rlen = (bint)strlen(repl);
    bint ng = g.ngroups > BRAT_RE_MAX_GROUPS ? BRAT_RE_MAX_GROUPS : g.ngroups;
    bint nslots = 2 * (ng + 1);
    bint *slots = (bint *)malloc(sizeof(bint) * (size_t)nslots);
    if (!slots) { r_free(&g); vfatal_oom(); }
    char *out = NULL;
    size_t olen = 0, ocap = 0;
    bint cur = 0, search_from = 0;
    while (search_from <= slen) {
        for (bint z = 0; z < nslots; z++) slots[z] = -1;
        if (!re_pike_search(&g, start, s, slen, search_from, slots, nslots)) break;
        bint m0 = slots[0], m1 = slots[1];
        if (m0 > cur) _vre_buf_append(&out, &olen, &ocap, s + cur, (size_t)(m0 - cur));
        for (bint ri = 0; ri < rlen; ri++) {
            if (repl[ri] == '$' && ri + 1 < rlen) {
                char nxt = repl[ri + 1];
                if (nxt == '$') {
                    _vre_buf_append(&out, &olen, &ocap, "$", 1);
                    ri++;
                    continue;
                }
                if (nxt >= '0' && nxt <= '9') {
                    bint gi = nxt - '0';
                    if (gi <= ng && slots[2 * gi] >= 0 && slots[2 * gi + 1] >= slots[2 * gi]) {
                        _vre_buf_append(&out, &olen, &ocap, s + slots[2 * gi],
                                        (size_t)(slots[2 * gi + 1] - slots[2 * gi]));
                    }
                    ri++;
                    continue;
                }
            }
            if (repl[ri] == '\\' && ri + 1 < rlen && repl[ri + 1] >= '1' && repl[ri + 1] <= '9') {
                bint gi = repl[ri + 1] - '0';
                if (gi <= ng && slots[2 * gi] >= 0 && slots[2 * gi + 1] >= slots[2 * gi]) {
                    _vre_buf_append(&out, &olen, &ocap, s + slots[2 * gi],
                                    (size_t)(slots[2 * gi + 1] - slots[2 * gi]));
                }
                ri++;
                continue;
            }
            _vre_buf_append(&out, &olen, &ocap, repl + ri, 1);
        }
        cur = m1;
        if (m1 == m0) {
            if (m1 >= slen) break;
            /* Advance one full UTF-8 codepoint so split UTF-8 bytes are never emitted. */
            unsigned char lead = (unsigned char)s[m1];
            bint step = (lead < 0x80) ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : 4));
            if (m1 + step > slen) step = 1;
            _vre_buf_append(&out, &olen, &ocap, s + m1, (size_t)step);
            cur = m1 + step;
            search_from = cur;
        } else {
            search_from = m1;
        }
    }
    if (cur < slen) _vre_buf_append(&out, &olen, &ocap, s + cur, (size_t)(slen - cur));
    free(slots);
    r_free(&g);
    if (!out) return vstr("");
    Val res = vstrn(out, (bint)olen);
    free(out);
    return res;
}

static void vraise_err_tag(Val v) {
    if (_verr_pending) return;
    int saved = barena_depth;
    barena_depth = 0;
    Vec *src = v.as.tv;
    bint n = src ? src->len : 0;
    Vec *a = vvec();
    for (bint i = 0; i < n; i++) {
        Val el = saved ? vown_copy(src->items[i]) : vrc_inc(src->items[i]);
        if (saved && _verr_pending) {
            vrc_dec(vvecv(a));
            barena_depth = saved;
            return;
        }
        vec_push(a, el);
    }
    vunwind = vtagv("err", a);
    barena_depth = saved;
    _verr_line = _btrace_cur_line;
    vunwind_do();
}

BFN int vassert1(Val cond) {
    if (_verr_pending) {
        vrc_dec(cond);
        return 0;
    }
    int ok = vis_true(cond);
    vrc_dec(cond);
    if (ok) return 1;
    vraise("assert failed");
    return 0;
}

BFN int vassert2(Val cond, Val msg) {
    if (_verr_pending) {
        vrc_dec(cond);
        vrc_dec(msg);
        return 0;
    }
    int ok = vis_true(cond);
    vrc_dec(cond);
    if (ok) {
        vrc_dec(msg);
        return 1;
    }
    int saved = barena_depth;
    barena_depth = 0;
    if (saved) {
        Val hv = vown_copy(msg);
        vrc_dec(msg);
        if (_verr_pending) {
            barena_depth = saved;
            return 0;
        }
        msg = hv;
    }
    Vec *a = vvec();
    vec_push(a, msg);
    vunwind = vtagv("err", a);
    barena_depth = saved;
    _verr_line = _btrace_cur_line;
    vunwind_do();
    return 0;
}

BFN Val vtag_payload(Val v);
BFN Val vquest(Val v) {
    if (v.tag == V_TAG && !strcmp(v.as.tv->tag, "err")) {
        vraise_err_tag(v);
    }
    return v;
}
BFN Val vquest_ok(Val v) {
    if (v.tag == V_TAG && (!strcmp(v.as.tv->tag, "ok") || !strcmp(v.as.tv->tag, "some")))
        return vtag_payload(v);
    if (verr_pending()) return vnil();
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

BFN_HOT Val vfield(Val v, const char *k) {
    if (v.tag == V_STRUCT) {
        Val *o = map_get(v.as.map, k);
        if (!o) { vraisef("no field %s\n", k); return vunwind; }
        return *o;
    }
    vraisef("no field %s\n", k); return vunwind;
}

BFN_HOT Val vfield_ic(Val v, const char *k, BFieldIC *ic) {
    if (v.tag == V_STRUCT) {
        Map *m = v.as.map;
        if (ic) {
            const BShape *sh = m->shape;
            if (sh) {
                if (sh == ic->shapes[0]) return m->vals[ic->slots[0]];
                if (sh == ic->shapes[1]) return m->vals[ic->slots[1]];
                if (sh == ic->shapes[2]) return m->vals[ic->slots[2]];
                if (sh == ic->shapes[3]) return m->vals[ic->slots[3]];
            } else {
                bint s = ic->slots[0];
                if (!ic->shapes[0] && s >= 0 && s < m->used) {
                    Val kv = m->keys[s];
                    if (kv.tag == V_STR) {
                        const char *ks = vstr_c(&kv);
                        if (ks == k || (ks[0] == k[0] && strcmp(ks, k) == 0))
                            return m->vals[s];
                    }
                }
            }
        }
        bint at = map_find_cstr(m, k, strlen(k));
        if (at < 0) { vraisef("no field %s\n", k); return vunwind; }
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
        return m->vals[at];
    }
    vraisef("no field %s\n", k); return vunwind;
}


BFN Val verr(const char *msg) {
    Vec *e = vvec();
    vec_push(e, vstr(_verror_text(msg)));
    return vtagv("err", e);
}

BFN Val verr_os(const char *rp) {
    char m[9000];
    int e = errno;
    snprintf(m, sizeof m, "[Errno %d] %s: '%s'", e, strerror(e), rp ? rp : "");
    if (!utf8_valid(m, (bint)strlen(m))) snprintf(m, sizeof m, "[Errno %d] os error", e);
    return verr(m);
}
BFN Val vok1(const char *s) {
    Vec *a = vvec();
    vec_push(a, vstr(s));
    return vtagv("ok", a);
}
BFN const char *vconfine_val(const Val *p) {
    if (!p || p->tag != V_STR || vstr_has_nul(p)) return NULL;
    return vconfine(vstr_c(p));
}
static int _vpath_bytes(const Val *p, char *buf, size_t cap, const char **out) {
    if (p->tag == V_STR) {
        if (vstr_has_nul(p)) return 2;
        *out = vstr_c(p);
        return 0;
    }
    if (p->tag != V_VEC) return 1;
    Vec *v = p->as.vec;
    if (!v || v->len < 0) return 1;
    if ((size_t)v->len >= cap) return 3;
    for (bint i = 0; i < v->len; i++) {
        Val el = v->items[i];
        if (el.tag != V_INT || el.as.ival < 0 || el.as.ival > 255) return 4;
        if (el.as.ival == 0) return 2;
        buf[i] = (char)el.as.ival;
    }
    buf[v->len] = 0;
    *out = buf;
    return 0;
}
static const char *_vconfine_path(const Val *p, char *buf, size_t cap, int *bad) {
    const char *raw = NULL;
    *bad = _vpath_bytes(p, buf, cap, &raw);
    if (*bad) return NULL;
    return vconfine(raw);
}
BFN Val vreadfile(Val p) {
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("read_file wants a string path");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("read_file byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    int escaped = 0, too_large = 0;
    FILE *f = _vopen_confined_stream(rp, 0, BRAT_MAX_FILE_BYTES, &escaped, &too_large);
    if (!f) {
        if (escaped) return verr("path escaped during open");
        if (too_large) return verr("file exceeds 64 MiB limit");
        return verr_os(rp);
    }
    char buf[4096];
    size_t n;
    size_t total = 0, cap = 0;
    char *data = NULL;
    int over_cap = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (n > BRAT_MAX_FILE_BYTES || total > BRAT_MAX_FILE_BYTES - n) {
            over_cap = 1;
            break;
        }
        if (total + n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            while (ncap < total + n + 1) {
                if (ncap > BRAT_MAX_FILE_BYTES) { ncap = BRAT_MAX_FILE_BYTES + 1; break; }
                ncap *= 2;
            }
            data = (char *)bgrow(data, ncap);
            cap = ncap;
        }
        memcpy(data + total, buf, n);
        total += n;
    }
    int failed = ferror(f);
    int rerr = errno;
    fclose(f);
    if (over_cap) { free(data); return verr("file exceeds 64 MiB limit"); }
    if (failed) { free(data); errno = rerr; return verr_os(rp); }
    if (!data) return vok1("");
    data[total] = 0;
    if (bsize_over_bint(total) || !utf8_valid(data, (bint)total) || strlen(data) != total) {
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
typedef SOCKET bsock_t;
#define BSOCK_INVALID INVALID_SOCKET
#define bsock_close(s) closesocket(s)
typedef uint32_t bin_addr_t;
#else
typedef int bsock_t;
#define BSOCK_INVALID (-1)
#define bsock_close(s) close(s)
typedef in_addr_t bin_addr_t;
#endif

static bmutex_t _vsk_mu = BMUTEX_INIT;
static void _vsk_lock(void) { BMUTEX_LOCK(&_vsk_mu); }
static void _vsk_unlock(void) { BMUTEX_UNLOCK(&_vsk_mu); }

#ifndef _VSK_TIMEOUT_S
#define _VSK_TIMEOUT_S 30        /* connected sockets have a fixed lifetime */
#endif
#define _VRECV_MAX (1L << 20)    /* one tcp_recv returns at most 1 MiB */

typedef struct {
    bsock_t fd;
    bint id;
    bint free_next;
    bint refs;
    bint closing;
    uint64_t deadline_ms;
} VSock;
static VSock *_vsk;
static bint _vsk_cap;
static bint _vsk_len = 1;
static bint _vsk_free;
static bint _vsk_next_id = 1;
static int _vsk_ready = 0;

static uint64_t _vsk_now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* Callers hold the lock. */
static int _vsk_ensure(void) {
    if (_vsk_ready) return 1;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    _vsk_ready = 1;
    return 1;
}
static int _vsk_init_net(void) {
    _vsk_lock();
    int ok = _vsk_ensure();
    _vsk_unlock();
    return ok;
}
/* Descriptor confinement: raw OS socket descriptors/SOCKET handles never leave
 * `_vsk[]` as BRAT values. Programs see fresh, monotonically increasing IDs,
 * so a copied stale handle cannot alias the next socket in a reused table
 * slot. Descriptors are marked non-inheritable so child processes spawned by
 * `proc_run` cannot touch them. */
static void _vsk_confine(bsock_t fd) {
#if defined(_WIN32)
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
#endif
}
static bint _vsk_find_locked(bint id) {
    if (id < 1) return -1;
    for (bint i = 1; i < _vsk_len; i++) {
        if (_vsk[i].fd != BSOCK_INVALID && !_vsk[i].closing && _vsk[i].id == id) return i;
    }
    return -1;
}
static bint _vsk_put(bsock_t fd) {
    bint id = -1;
    _vsk_lock();
    if (_vsk_ensure() && _vsk_next_id >= 1) {
        bint slot = 0;
        if (_vsk_free) {
            slot = _vsk_free;
            _vsk_free = _vsk[slot].free_next;
        } else {
            if (_vsk_len >= _vsk_cap) {
                bint nc = _vsk_cap ? _vsk_cap * 2 : 64;
                VSock *nt = NULL;
                if (nc >= _vsk_cap && (uint64_t)nc <= (uint64_t)SIZE_MAX / sizeof(VSock))
                    nt = (VSock *)realloc(_vsk, (size_t)nc * sizeof(VSock));
                if (nt) {
                    for (bint i = _vsk_cap; i < nc; i++) {
                        nt[i].fd = BSOCK_INVALID;
                        nt[i].id = 0;
                        nt[i].free_next = 0;
                        nt[i].refs = 0;
                        nt[i].closing = 0;
                        nt[i].deadline_ms = 0;
                    }
                    _vsk = nt;
                    _vsk_cap = nc;
                }
            }
            if (_vsk_len < _vsk_cap) slot = _vsk_len++;
        }
        if (slot) {
            _vsk[slot].fd = fd;
            _vsk[slot].id = _vsk_next_id;
            _vsk[slot].free_next = 0;
            _vsk[slot].refs = 0;
            _vsk[slot].closing = 0;
            _vsk[slot].deadline_ms = _vsk_now_ms() + (uint64_t)_VSK_TIMEOUT_S * 1000;
            id = _vsk_next_id;
            _vsk_next_id = (_vsk_next_id == INT64_MAX) ? 0 : _vsk_next_id + 1;
        }
    }
    _vsk_unlock();
    return id;
}
static void _vsk_free_locked(bint slot) {
    _vsk[slot].fd = BSOCK_INVALID;
    _vsk[slot].id = 0;
    _vsk[slot].refs = 0;
    _vsk[slot].closing = 0;
    _vsk[slot].deadline_ms = 0;
    _vsk[slot].free_next = _vsk_free;
    _vsk_free = slot;
}
static void _vsk_shutdown(bsock_t fd) {
#if defined(_WIN32)
    shutdown(fd, SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}
static bsock_t _vsk_acquire(bint id, bint *slot_out, uint64_t *deadline_out) {
    bsock_t fd = BSOCK_INVALID;
    *slot_out = -1;
    if (deadline_out) *deadline_out = 0;
    _vsk_lock();
    if (_vsk_ensure()) {
        bint slot = _vsk_find_locked(id);
        if (slot >= 0) {
            fd = _vsk[slot].fd;
            _vsk[slot].refs++;
            *slot_out = slot;
            if (deadline_out) *deadline_out = _vsk[slot].deadline_ms;
        }
    }
    _vsk_unlock();
    return fd;
}
static int _vsk_release(bint slot) {
    if (slot < 0) return 0;
    bsock_t close_fd = BSOCK_INVALID;
    int closing = 0;
    _vsk_lock();
    if (slot < _vsk_len && _vsk[slot].refs > 0) {
        closing = _vsk[slot].closing != 0;
        _vsk[slot].refs--;
        if (closing && _vsk[slot].refs == 0) {
            close_fd = _vsk[slot].fd;
            _vsk_free_locked(slot);
        }
    }
    _vsk_unlock();
    if (close_fd != BSOCK_INVALID) bsock_close(close_fd);
    return closing;
}
static int _vsk_close_id(bint id) {
    bsock_t close_fd = BSOCK_INVALID;
    bsock_t shutdown_fd = BSOCK_INVALID;
    int found = 0;
    _vsk_lock();
    if (_vsk_ensure()) {
        bint slot = _vsk_find_locked(id);
        if (slot >= 0) {
            found = 1;
            if (_vsk[slot].refs == 0) {
                close_fd = _vsk[slot].fd;
                _vsk_free_locked(slot);
            } else {
                _vsk[slot].closing = 1;
                shutdown_fd = _vsk[slot].fd;
            }
        }
    }
    _vsk_unlock();
    if (shutdown_fd != BSOCK_INVALID) _vsk_shutdown(shutdown_fd);
    if (close_fd != BSOCK_INVALID) bsock_close(close_fd);
    return found;
}
static int _vsk_slot_id(Val slot, bint *id) {
    if (slot.tag != V_INT) return 0;
    *id = slot.as.ival;
    return 1;
}

/* A write to a peer that has gone away must return EPIPE, not kill the process.
 * Linux and Windows take MSG_NOSIGNAL / have no SIGPIPE; macOS needs the socket option. */
static void _vsk_nosigpipe(bsock_t fd) {
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
static void _vsk_timeouts(bsock_t fd) {
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
static int _vsk_apply_deadline(bsock_t fd, uint64_t deadline, int receive) {
    uint64_t now = _vsk_now_ms();
    if (!deadline || now >= deadline) return 0;
    uint64_t left = deadline - now;
#if defined(_WIN32)
    DWORD ms = left > 0xffffffffu ? 0xffffffffu : (DWORD)left;
    if (ms == 0) ms = 1;
    int opt = receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    return setsockopt(fd, SOL_SOCKET, opt, (const char *)&ms, sizeof ms) == 0;
#else
    struct timeval tv;
    tv.tv_sec = (time_t)(left / 1000);
    tv.tv_usec = (suseconds_t)((left % 1000) * 1000);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1000;
    int opt = receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    return setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv) == 0;
#endif
}
static int _vsk_timed_out(void) {
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
static int _vsk_connect_timeout(bsock_t fd, const struct sockaddr *addr,
#if defined(_WIN32)
                                int addrlen) {
#else
                                socklen_t addrlen) {
#endif
    uint64_t deadline = _vsk_now_ms() + (uint64_t)_VSK_TIMEOUT_S * 1000;
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) == SOCKET_ERROR) return SOCKET_ERROR;
    int result = connect(fd, addr, (int)addrlen);
    if (result == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS && e != WSAEALREADY) {
            nonblocking = 0;
            ioctlsocket(fd, FIONBIO, &nonblocking);
            WSASetLastError(e);
            return SOCKET_ERROR;
        }
        for (;;) {
            uint64_t now = _vsk_now_ms();
            if (now >= deadline) { WSASetLastError(WSAETIMEDOUT); break; }
            uint64_t left = deadline - now;
            struct timeval tv;
            tv.tv_sec = (int32_t)(left / 1000);
            tv.tv_usec = (int32_t)((left % 1000) * 1000);
            fd_set writefds, errorfds;
            FD_ZERO(&writefds); FD_SET(fd, &writefds);
            FD_ZERO(&errorfds); FD_SET(fd, &errorfds);
            int ready = select(0, NULL, &writefds, &errorfds, &tv);
            if (ready == 0) { WSASetLastError(WSAETIMEDOUT); break; }
            if (ready == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEINTR) continue;
                break;
            }
            int so_error = 0;
            int so_len = sizeof so_error;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_len) == SOCKET_ERROR) {
                result = SOCKET_ERROR;
            } else if (so_error != 0) {
                WSASetLastError(so_error);
                result = SOCKET_ERROR;
            } else {
                result = 0;
            }
            break;
        }
    }
    nonblocking = 0;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) == SOCKET_ERROR && result == 0)
        result = SOCKET_ERROR;
    return result;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    int result = connect(fd, addr, addrlen);
    if (result < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EINTR)) {
        for (;;) {
            uint64_t now = _vsk_now_ms();
            if (now >= deadline) { errno = ETIMEDOUT; break; }
            uint64_t left = deadline - now;
            int timeout = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int ready = poll(&pfd, 1, timeout);
            if (ready == 0) { errno = ETIMEDOUT; break; }
            if (ready < 0) {
                if (errno == EINTR) continue;
                result = -1;
                break;
            }
            int so_error = 0;
            socklen_t so_len = sizeof so_error;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0) {
                result = -1;
            } else if (so_error != 0) {
                errno = so_error;
                result = -1;
            } else {
                result = 0;
            }
            break;
        }
    }
    int saved_errno = errno;
    if (fcntl(fd, F_SETFL, flags) < 0 && result == 0) result = -1;
    errno = saved_errno;
    return result;
#endif
}

static Val _vok_int(bint n) { Vec *a = vvec(); vec_push(a, vint(n)); return vtagv("ok", a); }
static Val _verr_c(const char *m) { Vec *a = vvec(); vec_push(a, vstr(_verror_text(m))); return vtagv("err", a); }

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

static const char _vsbuf_type[] = "__brat_sbuf";

static Map *_vsb_map(Val h) {
    if (h.tag != V_STRUCT || !h.as.map || h.as.map->type != _vsbuf_type || h.as.map->len < 1)
        return NULL;
    return h.as.map;
}
BFN Val vsbuf(void) {
    /* Refcounted heap builder: automatically released by `vrc_dec`/`vrelease`
     * when the owning `Val` leaves scope, with no fixed process-lifetime slot table. */
    Map *m = vmap(_vsbuf_type);
    map_set_v(m, vlit("b"), vstr_sso("", 0));
    return vstruct(m);
}
BFN Val vsbuf_add(Val h, Val part) {
    Map *m = _vsb_map(h);
    if (!m) return _verr_c("bad builder");
    if (part.tag != V_STR) return _verr_c("sbuf_add wants a string");
    const char *ps = vstr_c(&part);
    size_t pl = vstr_len_fast(&part);
    bint cp_add = vlen(part).as.ival;
    BRAT_CTR_LOCK_OBJ(m);
    Val cur = m->vals[0];
    size_t cl = vstr_len_fast(&cur);
    if (pl > BRAT_MAX_STR || cl > BRAT_MAX_STR - pl) {
        BRAT_CTR_UNLOCK_OBJ(m);
        return _verr_c("sbuf exceeds maximum string size");
    }
    bint old_cp = vlen(cur).as.ival;
    size_t nlen = cl + pl;
    if (nlen <= BRAT_SSO_MAX && cur.sso) {
        char tmp[BRAT_SSO_MAX + 1];
        if (cl) memcpy(tmp, vstr_c(&cur), cl);
        if (pl) memcpy(tmp + cl, ps, pl);
        m->vals[0] = vstr_sso(tmp, nlen);
        BRAT_CTR_UNLOCK_OBJ(m);
        return vint(old_cp + cp_add);
    }
    int ina = BIS_ARENA((BHead *)m - 1);
    int saved_a = barena_depth;
    if (!ina) barena_depth = 0;
    char *buf = NULL;
    if (!cur.sso && cur.as.str) {
        BHead *bh = (BHead *)cur.as.str - 1;
        if (!BIS_IMMORTAL(bh) && (BIS_ARENA(bh) ? ina : (BRC_LOAD(bh) == 1))) {
            size_t cap = (size_t)bh->cap;
            if (nlen + 1 > cap) {
                size_t ncap = cap >= 32 ? cap : 32;
                while (ncap < nlen + 1) {
                    if (ncap > BRAT_MAX_STR / 2 + 1) { ncap = nlen + 1; break; }
                    ncap *= 2;
                }
                buf = (char *)bgrow_rc(cur.as.str, ncap);
            } else {
                buf = cur.as.str;
            }
            if (pl) memcpy(buf + cl, ps, pl);
            buf[nlen] = 0;
            BHead *nh = (BHead *)buf - 1;
            nh->len = (bint)nlen;
            nh->cplen = old_cp + cp_add;
            nh->ascii = (old_cp + cp_add == (bint)nlen) ? 1 : -1;
            nh->hash = 0;
            Val nv; nv.tag = V_STR; nv.sso = 0; nv.imm = BIS_ARENA(nh) ? 1u : 0u; nv.as.str = buf;
            m->vals[0] = nv;
            barena_depth = saved_a;
            BRAT_CTR_UNLOCK_OBJ(m);
            return vint(old_cp + cp_add);
        }
    }
    size_t icap = 32;
    while (icap < nlen + 1) {
        if (icap > BRAT_MAX_STR / 2 + 1) { icap = nlen + 1; break; }
        icap *= 2;
    }
    buf = (char *)balloc(icap);
    if (cl) memcpy(buf, vstr_c(&cur), cl);
    if (pl) memcpy(buf + cl, ps, pl);
    buf[nlen] = 0;
    BHead *nh = (BHead *)buf - 1;
    nh->len = (bint)nlen;
    nh->cplen = old_cp + cp_add;
    nh->ascii = (old_cp + cp_add == (bint)nlen) ? 1 : -1;
    Val nv; nv.tag = V_STR; nv.sso = 0; nv.imm = BIS_ARENA(nh) ? 1u : 0u; nv.as.str = buf;
    m->vals[0] = nv;
    barena_depth = saved_a;
    BRAT_CTR_UNLOCK_OBJ(m);
    vrelease1(cur);
    return vint(old_cp + cp_add);
}
BFN Val vsbuf_len(Val h) {
    Map *m = _vsb_map(h);
    if (!m) return _verr_c("bad builder");
    BRAT_CTR_LOCK_OBJ(m);
    bint len = vlen(m->vals[0]).as.ival;
    BRAT_CTR_UNLOCK_OBJ(m);
    return vint(len);
}
BFN Val vsbuf_str(Val h) {
    Map *m = _vsb_map(h);
    if (!m) return _verr_c("bad builder");
    BRAT_CTR_LOCK_OBJ(m);
    Val cur = m->vals[0];
    Val text = vstrn(vstr_c(&cur), (bint)vstr_len_fast(&cur));
    BRAT_CTR_UNLOCK_OBJ(m);
    Vec *a = vvec();
    vec_push(a, text);
    return vtagv("ok", a);
}
BFN Val vtcp_listen(Val addr) {
    if (addr.tag != V_STR) return _verr_c("tcp_listen wants a string address");
    char host[256]; int port = 0;
    if (!_vhost_port(vstr_c(&addr), host, sizeof host, &port))
        return _verr_c("address wants host:port with port 1..65535");
    if (!_vsk_init_net()) return _verr_c("socket init failed");
    uint64_t ip = inet_addr(host);
    if (ip == INADDR_NONE) return _verr_c("host must be an IPv4 address");
    bsock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BSOCK_INVALID) return _verr_c("socket failed");
    _vsk_confine(fd);
    _vsk_nosigpipe(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = (bin_addr_t)ip;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { bsock_close(fd); return _verr_c("bind failed"); }
    if (listen(fd, 16) != 0) { bsock_close(fd); return _verr_c("listen failed"); }
    bint slot = _vsk_put(fd);
    if (slot < 0) { bsock_close(fd); return _verr_c("too many sockets"); }
    return _vok_int(slot);
}
BFN Val vtcp_connect(Val addr) {
    if (addr.tag != V_STR) return _verr_c("tcp_connect wants a string address");
    char host[256]; int port = 0;
    if (!_vhost_port(vstr_c(&addr), host, sizeof host, &port))
        return _verr_c("address wants host:port with port 1..65535");
    if (!_vsk_init_net()) return _verr_c("socket init failed");
    uint64_t ip = inet_addr(host);
    if (ip == INADDR_NONE) return _verr_c("host must be an IPv4 address");
    bsock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BSOCK_INVALID) return _verr_c("socket failed");
    _vsk_confine(fd);
    _vsk_nosigpipe(fd);
    _vsk_timeouts(fd);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = (bin_addr_t)ip;
    if (_vsk_connect_timeout(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        bsock_close(fd);
        return _verr_c("connect failed");
    }
    bint slot = _vsk_put(fd);
    if (slot < 0) { bsock_close(fd); return _verr_c("too many sockets"); }
    return _vok_int(slot);
}
BFN Val vtcp_accept(Val slot) {
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    bint at;
    bsock_t lfd = _vsk_acquire(id, &at, NULL);
    if (lfd == BSOCK_INVALID) return _verr_c("bad socket slot");
    bsock_t fd = accept(lfd, NULL, NULL);
    _vsk_release(at);
    if (fd == BSOCK_INVALID) return _verr_c("accept failed");
    _vsk_confine(fd);
    _vsk_nosigpipe(fd);
    _vsk_timeouts(fd);
    bint ns = _vsk_put(fd);
    if (ns < 0) { bsock_close(fd); return _verr_c("too many sockets"); }
    return _vok_int(ns);
}

BFN Val vtcp_recv(Val slot, Val n) {
    if (n.tag != V_INT) return _verr_c("recv wants 0 <= n <= 1048576");
    bint want = n.as.ival;
    if (want < 0 || want > _VRECV_MAX) return _verr_c("recv wants 0 <= n <= 1048576");
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    bint at;
    uint64_t deadline;
    bsock_t fd = _vsk_acquire(id, &at, &deadline);
    if (fd == BSOCK_INVALID) return _verr_c("bad socket slot");
    if (!_vsk_apply_deadline(fd, deadline, 1)) {
        _vsk_release(at);
        return _verr_c("socket deadline exceeded");
    }
    char *buf = (char *)malloc((size_t)want + 1);
    if (!buf) vfatal_oom();
#if defined(_WIN32)
    bint got = (bint)recv(fd, buf, (int)want, 0);
#else
    bint got = (bint)recv(fd, buf, (size_t)want, 0);
#endif
    int timed_out = got < 0 ? _vsk_timed_out() : 0;
    int closing = _vsk_release(at);
    if (got < 0) {
        free(buf);
        return _verr_c(timed_out ? "socket deadline exceeded" : "recv failed");
    }
    if (got == 0 && closing) {
        free(buf);
        return _verr_c("recv failed");
    }
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
    bint at;
    uint64_t deadline;
    bsock_t fd = _vsk_acquire(id, &at, &deadline);
    if (fd == BSOCK_INVALID) return _verr_c("bad socket slot");
    const char *p = vstr_c(&data);
    size_t left = vstr_len_fast(&data);
    size_t done = 0;
    while (left > 0) {
        if (!_vsk_apply_deadline(fd, deadline, 0)) {
            _vsk_release(at);
            return _verr_c("socket deadline exceeded");
        }
#if defined(_WIN32)
        int chunk = left > 0x7fffffff ? 0x7fffffff : (int)left;
        int k = send(fd, p + done, chunk, 0);
        if (k == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            int timed_out = _vsk_timed_out();
            _vsk_release(at);
            return _verr_c(timed_out ? "socket deadline exceeded" : "send failed");
        }
#else
        ssize_t k = send(fd, p + done, left, _VSEND_FLAGS);
        if (k < 0) {
            if (errno == EINTR) continue;
            int timed_out = _vsk_timed_out();
            _vsk_release(at);
            return _verr_c(timed_out ? "socket deadline exceeded" : "send failed");
        }
#endif
        if (k == 0) {
            _vsk_release(at);
            return _verr_c("send made no progress");
        }
        done += (size_t)k;
        left -= (size_t)k;
    }
    _vsk_release(at);
    return _vok_int((bint)done);
}
BFN Val vtcp_close(Val slot) {
    bint id;
    if (!_vsk_slot_id(slot, &id)) return _verr_c("bad socket slot");
    if (!_vsk_close_id(id)) return _verr_c("bad socket slot");
    return _vok_int(0);
}
/* own(x): a deep copy that lives on the heap, so it survives the arena that
 * made x. Arena allocation is suspended for the copy. */
/* `own` copies a value out of an arena. Every container it builds owns the
 * copies it holds, so releasing it releases the whole tree; the flag is what
 * separates this from an ordinary container, whose elements alias bindings
 * elsewhere and must outlive it. */
static Val vown_copy_d(Val v, bint depth) {
    switch (v.tag) {
    case V_STR:
        return (v.sso || !v.as.str) ? v : vstrn(v.as.str, ((BHead *)v.as.str - 1)->len);
    case V_VEC: {
        if (!v.as.vec) return v;
        if (depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return vunwind; }
        Vec *dst = vvec();
        for (bint i = 0; i < v.as.vec->len; i++) {
            Val el = vown_copy_d(v.as.vec->items[i], depth + 1);
            if (_verr_pending) { vrc_dec(vvecv(dst)); return vunwind; }
            vec_push(dst, el);
        }
        return vvecv(dst);
    }
    case V_STRUCT: {
        if (!v.as.map) return v;
        if (depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return vunwind; }
        Map *src = v.as.map;
        if (vclosure_env_map(src)) {
            Map *dst = vclosure_env_new(src->len);
            dst->bad_key = src->bad_key;
            for (bint i = 0; i < src->len; i++) {
                if (i == src->bad_key) { dst->vals[i] = vnil(); continue; }
                Val ev = vown_copy_d(src->vals[i], depth + 1);
                if (_verr_pending) {
                    for (bint j = i; j < src->len; j++) dst->vals[j] = vnil();
                    vrc_dec(vstruct(dst));
                    return vunwind;
                }
                dst->vals[i] = ev;
            }
            return vstruct(dst);
        }
        Map *dst = vmap(src->type);
        for (bint i = 0; i < src->used; i++) {
            if (src->keys[i].tag == V_MOVED) continue;
            Val kv = vown_copy_d(src->keys[i], depth + 1);
            if (_verr_pending) { vrc_dec(vstruct(dst)); return vunwind; }
            Val vv = vown_copy_d(src->vals[i], depth + 1);
            if (_verr_pending) { vrc_dec(kv); vrc_dec(vstruct(dst)); return vunwind; }
            map_set_v(dst, kv, vv);
        }
        return vstruct(dst);
    }
    case V_TAG: {
        Vec *args = v.as.tv;
        if (!args) return v;
        if (v.imm) return v;
        if (depth >= BRAT_MAX_VALUE_DEPTH) { vraise("recursion too deep"); return vunwind; }
        Vec *dst = vvec();
        for (bint i = 0; i < args->len; i++) {
            Val el = vown_copy_d(args->items[i], depth + 1);
            if (_verr_pending) { vrc_dec(vvecv(dst)); return vunwind; }
            vec_push(dst, el);
        }
        Val owned = vtagv(args->tag, dst);
        if (vtag_closure(owned) && owned.as.tv->len == 2 &&
            owned.as.tv->items[1].tag == V_STRUCT &&
            vclosure_env_map(owned.as.tv->items[1].as.map)) {
            Map *env = owned.as.tv->items[1].as.map;
            if (env->bad_key >= 0 && env->bad_key < env->len)
                vclosure_env_set_weak(env, env->bad_key, owned);
        }
        return owned;
    }
    default:
        return v;
    }
}
static Val vown_copy(Val v) { return vown_copy_d(v, 0); }
BFN Val vown(Val v) {
    int saved = barena_depth;
    barena_depth = 0;
    Val r = vown_copy(v);
    barena_depth = saved;
    return r;
}

BFN Val vwritefile(Val p, Val t) {
    if (t.tag != V_STR) return verr("write_file wants string path and text");
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("write_file wants string path and text");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("write_file byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    int escaped = 0;
    FILE *f = _vopen_confined_stream(rp, 1, 0, &escaped, NULL);
    if (!f) {
        if (escaped) return verr("path escaped during open");
        return verr_os(rp);
    }
    const char *ts = vstr_c(&t);
    size_t n = vstr_len_fast(&t);
    errno = 0;
    size_t w = n ? fwrite(ts, 1, n, f) : 0;
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

BFN Val vreadbytes(Val p) {
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("read_bytes wants a string path");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("read_bytes byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    int escaped = 0, too_large = 0;
    FILE *f = _vopen_confined_stream(rp, 0, BRAT_MAX_READ_BYTES, &escaped, &too_large);
    if (!f) {
        if (escaped) return verr("path escaped during open");
        if (too_large) return verr("file exceeds 16 MiB byte vector limit");
        return verr_os(rp);
    }
    unsigned char buf[4096];
    size_t n, total = 0;
    int over_cap = 0;
    Vec *bytes = vvec();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (n > BRAT_MAX_READ_BYTES || total > BRAT_MAX_READ_BYTES - n) {
            over_cap = 1;
            break;
        }
        total += n;
        for (size_t i = 0; i < n; i++) vec_push(bytes, vint((bint)buf[i]));
    }
    int failed = ferror(f);
    int rerr = errno;
    fclose(f);
    if (over_cap) {
        vrc_dec(vvecv(bytes));
        return verr("file exceeds 16 MiB byte vector limit");
    }
    if (failed) {
        vrc_dec(vvecv(bytes));
        errno = rerr;
        return verr_os(rp);
    }
    Vec *a = vvec();
    vec_push(a, vvecv(bytes));
    return vtagv("ok", a);
}

BFN Val vutf8_bytes(Val s) {
    if (s.tag != V_STR) { vraise("utf8_bytes wants str"); return vunwind; }
    const char *p = vstr_c(&s);
    size_t n = vstr_len_fast(&s);
    if (n > BRAT_MAX_BYTE_VEC) {
        vraise("utf8_bytes: string exceeds 16 MiB byte vector limit");
        return vunwind;
    }
    Vec *out = vvec_cap((bint)n);
    for (size_t i = 0; i < n; i++) vec_push(out, vint((bint)(unsigned char)p[i]));
    return vvecv(out);
}

BFN Val vutf8_str(Val b) {
    if (b.tag != V_VEC) return verr("utf8_str wants a byte vec");
    Vec *bv = b.as.vec;
    bint n = bv->len;
    if (n <= 0) return vok1("");
    size_t un = (size_t)n;
    if (un > (size_t)0x3fffffff) vfatal_oom();
    char *raw = (char *)malloc(un + 1);
    if (!raw) vfatal_oom();
    for (size_t i = 0; i < un; i++) {
        Val el = bv->items[i];
        if (el.tag != V_INT || el.as.ival < 0 || el.as.ival > 255) {
            free(raw);
            return verr("utf8_str byte out of range 0..255");
        }
        if (el.as.ival == 0) {
            free(raw);
            return verr("invalid utf-8");
        }
        raw[i] = (char)(unsigned char)el.as.ival;
    }
    raw[un] = 0;
    if (!utf8_valid(raw, un)) {
        free(raw);
        return verr("invalid utf-8");
    }
    Vec *a = vvec_cap(1);
    vec_push(a, vstrn(raw, n));
    free(raw);
    return vtagv("ok", a);
}

BFN Val vfs_rename(Val src, Val dst) {
    char pbuf1[8192], pbuf2[8192];
    const char *raw1 = NULL, *raw2 = NULL;
    int pbad1 = _vpath_bytes(&src, pbuf1, sizeof pbuf1, &raw1);
    int pbad2 = _vpath_bytes(&dst, pbuf2, sizeof pbuf2, &raw2);
    if (pbad1 == 1 || pbad2 == 1) return verr("fs_rename wants string paths");
    if (pbad1 == 2 || pbad2 == 2) return verr("embedded NUL in file path");
    if (pbad1 == 4 || pbad2 == 4) return verr("fs_rename byte out of range 0..255");
    if (pbad1 || pbad2) return verr("path outside the program directory");
    const char *rp1 = vconfine(raw1);
    if (!rp1) return verr("path outside the program directory");
    char sbuf[8192];
    snprintf(sbuf, sizeof sbuf, "%s", rp1);
    const char *rp2 = vconfine(raw2);
    if (!rp2) return verr("path outside the program directory");
    char dbuf[8192];
    snprintf(dbuf, sizeof dbuf, "%s", rp2);
    errno = 0;
#if defined(_WIN32)
    wchar_t ws[4096], wd[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, sbuf, -1, ws, 4096) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dbuf, -1, wd, 4096) <= 0)
        return verr("fs_rename failed");
    if (!_vfiles_free()) {
        DWORD sa = GetFileAttributesW(ws), da = GetFileAttributesW(wd);
        if ((sa != INVALID_FILE_ATTRIBUTES && (sa & FILE_ATTRIBUTE_REPARSE_POINT)) ||
            (da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_REPARSE_POINT)))
            return verr("path escaped during open");
    }
    if (!MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING)) {
        return verr("fs_rename failed");
    }
#else
    if (_vfiles_free()) {
        if (rename(sbuf, dbuf) != 0) return verr_os(sbuf);
    } else {
        char sleaf[1024], dleaf[1024];
        int esc1 = 0, esc2 = 0;
        int sfd = _vopen_parent_confined(sbuf, sleaf, sizeof sleaf, &esc1);
        if (sfd < 0) return esc1 ? verr("path escaped during open") : verr_os(sbuf);
        int dfd = _vopen_parent_confined(dbuf, dleaf, sizeof dleaf, &esc2);
        if (dfd < 0) {
            int e = errno;
            close(sfd);
            errno = e;
            return esc2 ? verr("path escaped during open") : verr_os(dbuf);
        }
        int rc = renameat(sfd, sleaf, dfd, dleaf);
        int e = errno;
        close(sfd);
        close(dfd);
        if (rc != 0) { errno = e; return verr_os(sbuf); }
    }
#endif
    Vec *a = vvec();
    vec_push(a, vint(0));
    return vtagv("ok", a);
}

BFN Val vfs_remove(Val p) {
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("fs_remove wants a string path");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("fs_remove byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    errno = 0;
#if defined(_WIN32)
    wchar_t wp[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rp, -1, wp, 4096) <= 0)
        return verr_os(rp);
    if (!_vfiles_free()) {
        DWORD attr = GetFileAttributesW(wp);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return verr("path escaped during open");
    }
    if (!DeleteFileW(wp)) { errno = ENOENT; return verr_os(rp); }
#else
    if (_vfiles_free()) {
        if (unlink(rp) != 0) return verr_os(rp);
    } else {
        char leaf[1024];
        int esc = 0;
        int dfd = _vopen_parent_confined(rp, leaf, sizeof leaf, &esc);
        if (dfd < 0) return esc ? verr("path escaped during open") : verr_os(rp);
        int rc = unlinkat(dfd, leaf, 0);
        int e = errno;
        close(dfd);
        if (rc != 0) { errno = e; return verr_os(rp); }
    }
#endif
    Vec *a = vvec();
    vec_push(a, vint(0));
    return vtagv("ok", a);
}

/* FIPS 180-4 SHA-256, CSPRNG, lazily loaded TLS and SQLite3 */
static void _bsha256_block(uint32_t st[8], const unsigned char blk[64]) {
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16)
             | ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        uint32_t s0 = ((x >> 7) | (x << 25)) ^ ((x >> 18) | (x << 14)) ^ (x >> 3);
        uint32_t s1 = ((y >> 17) | (y << 15)) ^ ((y >> 19) | (y << 13)) ^ (y >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void _bsha256_raw(const unsigned char *msg, size_t len, unsigned char out[32]) {
    uint32_t st[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    size_t off = 0;
    while (len - off >= 64) {
        _bsha256_block(st, msg + off);
        off += 64;
    }
    unsigned char tail[128];
    memset(tail, 0, sizeof tail);
    size_t rem = len - off;
    if (rem > 0) memcpy(tail, msg + off, rem);
    tail[rem] = 0x80u;
    size_t pad_len = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; i++) {
        tail[pad_len - 1 - i] = (unsigned char)((bits >> (i * 8)) & 0xffu);
    }
    _bsha256_block(st, tail);
    if (pad_len == 128) _bsha256_block(st, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(st[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(st[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(st[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(st[i]);
    }
}

BFN Val vsha256_bytes(Val d) {
    unsigned char dig[32];
    if (d.tag == V_STR) {
        const char *s = vstr_c(&d);
        _bsha256_raw((const unsigned char *)s, vstr_len_fast(&d), dig);
    } else if (d.tag == V_VEC) {
        Vec *bv = d.as.vec;
        bint n = bv->len;
        unsigned char *raw = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
        if (n > 0 && !raw) vfatal_oom();
        for (bint i = 0; i < n; i++) {
            Val el = bv->items[i];
            if (el.tag != V_INT || el.as.ival < 0 || el.as.ival > 255) {
                free(raw);
                return verr("sha256_bytes byte out of range 0..255");
            }
            raw[i] = (unsigned char)el.as.ival;
        }
        _bsha256_raw(raw ? raw : (const unsigned char *)"", (size_t)n, dig);
        free(raw);
    } else {
        return verr("sha256_bytes wants str or byte vec");
    }
    Vec *out = vvec();
    for (int i = 0; i < 32; i++) vec_push(out, vint((bint)dig[i]));
    Vec *a = vvec();
    vec_push(a, vvecv(out));
    return vtagv("ok", a);
}

BFN Val vrandom_bytes(Val nv) {
    if (nv.tag != V_INT || nv.as.ival < 0 || nv.as.ival > 1048576)
        return verr("random_bytes wants 0 <= n <= 1048576");
    bint n = nv.as.ival;
    if (n == 0) {
        Vec *out = vvec();
        Vec *a = vvec();
        vec_push(a, vvecv(out));
        return vtagv("ok", a);
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) vfatal_oom();
#if defined(_WIN32)
    /* Dynamically resolve SystemFunction036 (RtlGenRandom) from advapi32.dll so
     * standalone syntax probes need no extra import libraries while still using
     * the kernel CSPRNG and failing closed if unavailable. */
    typedef unsigned char (WINAPI *_BRtlGenRandomFn)(void *, DWORD);
    static _BRtlGenRandomFn _brtl_gen = NULL;
    if (!_brtl_gen) {
        HMODULE adv = LoadLibraryExA("advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (adv) {
            union { FARPROC fp; _BRtlGenRandomFn fn; } u;
            u.fp = GetProcAddress(adv, "SystemFunction036");
            _brtl_gen = u.fn;
        }
    }
    if (!_brtl_gen || !_brtl_gen(buf, (DWORD)n)) {
        free(buf);
        return verr("OS CSPRNG unavailable");
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(buf); return verr("open /dev/urandom failed"); }
    size_t got = 0;
    while (got < (size_t)n) {
        ssize_t r = read(fd, buf + got, (size_t)n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            free(buf);
            return verr("read /dev/urandom failed");
        }
        if (r == 0) { close(fd); free(buf); return verr("short read from /dev/urandom"); }
        got += (size_t)r;
    }
    close(fd);
#endif
    Vec *out = vvec();
    for (bint i = 0; i < n; i++) vec_push(out, vint((bint)buf[i]));
    free(buf);
    Vec *a = vvec();
    vec_push(a, vvecv(out));
    return vtagv("ok", a);
}

#if !defined(_WIN32)
typedef struct _BOsslApi {
    int ready;
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    void (*SSL_CTX_free)(void *);
    int (*SSL_CTX_set_default_verify_paths)(void *);
    int (*SSL_CTX_load_verify_locations)(void *, const char *, const char *);
    intptr_t (*SSL_CTX_ctrl)(void *, int, intptr_t, void *);
    intptr_t (*SSL_CTX_set_options)(void *, intptr_t);
    void (*SSL_CTX_set_verify)(void *, int, void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    int (*SSL_set_fd)(void *, int);
    intptr_t (*SSL_ctrl)(void *, int, intptr_t, void *);
    int (*SSL_set1_host)(void *, const char *);
    void *(*SSL_get0_param)(void *);
    int (*SSL_connect)(void *);
    intptr_t (*SSL_get_verify_result)(const void *);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_get_error)(const void *, int);
    int (*SSL_shutdown)(void *);
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void *, const char *);
} BOsslApi;
static BOsslApi _bossl;
static bmutex_t _bossl_mu = BMUTEX_INIT;

static int _bossl_init(void) {
    BMUTEX_LOCK(&_bossl_mu);
    if (_bossl.ready != 0) {
        int r = _bossl.ready;
        BMUTEX_UNLOCK(&_bossl_mu);
        return r > 0;
    }
    const char *ssl_names[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so", "libssl.3.dylib", "libssl.dylib", NULL};
    const char *cry_names[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so", "libcrypto.3.dylib", "libcrypto.dylib", NULL};
    void *hssl = NULL, *hcry = NULL;
    for (int i = 0; ssl_names[i] && !hssl; i++) hssl = dlopen(ssl_names[i], RTLD_LAZY | RTLD_GLOBAL);
    for (int i = 0; cry_names[i] && !hcry; i++) hcry = dlopen(cry_names[i], RTLD_LAZY | RTLD_GLOBAL);
    if (!hssl || !hcry) {
        _bossl.ready = -1;
        BMUTEX_UNLOCK(&_bossl_mu);
        return 0;
    }
    *(void **)&_bossl.TLS_client_method = dlsym(hssl, "TLS_client_method");
    *(void **)&_bossl.SSL_CTX_new = dlsym(hssl, "SSL_CTX_new");
    *(void **)&_bossl.SSL_CTX_free = dlsym(hssl, "SSL_CTX_free");
    *(void **)&_bossl.SSL_CTX_set_default_verify_paths = dlsym(hssl, "SSL_CTX_set_default_verify_paths");
    *(void **)&_bossl.SSL_CTX_load_verify_locations = dlsym(hssl, "SSL_CTX_load_verify_locations");
    *(void **)&_bossl.SSL_CTX_ctrl = dlsym(hssl, "SSL_CTX_ctrl");
    *(void **)&_bossl.SSL_CTX_set_options = dlsym(hssl, "SSL_CTX_set_options");
    *(void **)&_bossl.SSL_CTX_set_verify = dlsym(hssl, "SSL_CTX_set_verify");
    *(void **)&_bossl.SSL_new = dlsym(hssl, "SSL_new");
    *(void **)&_bossl.SSL_free = dlsym(hssl, "SSL_free");
    *(void **)&_bossl.SSL_set_fd = dlsym(hssl, "SSL_set_fd");
    *(void **)&_bossl.SSL_ctrl = dlsym(hssl, "SSL_ctrl");
    *(void **)&_bossl.SSL_set1_host = dlsym(hssl, "SSL_set1_host");
    *(void **)&_bossl.SSL_get0_param = dlsym(hssl, "SSL_get0_param");
    *(void **)&_bossl.SSL_connect = dlsym(hssl, "SSL_connect");
    *(void **)&_bossl.SSL_get_verify_result = dlsym(hssl, "SSL_get_verify_result");
    *(void **)&_bossl.SSL_write = dlsym(hssl, "SSL_write");
    *(void **)&_bossl.SSL_read = dlsym(hssl, "SSL_read");
    *(void **)&_bossl.SSL_get_error = dlsym(hssl, "SSL_get_error");
    *(void **)&_bossl.SSL_shutdown = dlsym(hssl, "SSL_shutdown");
    *(void **)&_bossl.X509_VERIFY_PARAM_set1_ip_asc = dlsym(hcry, "X509_VERIFY_PARAM_set1_ip_asc");
    if (!_bossl.TLS_client_method || !_bossl.SSL_CTX_new || !_bossl.SSL_CTX_free ||
        !_bossl.SSL_CTX_set_default_verify_paths || !_bossl.SSL_CTX_load_verify_locations ||
        (!_bossl.SSL_CTX_set_options && !_bossl.SSL_CTX_ctrl) ||
        !_bossl.SSL_CTX_set_verify || !_bossl.SSL_new || !_bossl.SSL_free ||
        !_bossl.SSL_set_fd || !_bossl.SSL_set1_host || !_bossl.SSL_connect ||
        !_bossl.SSL_get_verify_result || !_bossl.SSL_write || !_bossl.SSL_read ||
        !_bossl.SSL_get_error || !_bossl.SSL_shutdown) {
        _bossl.ready = -1;
        BMUTEX_UNLOCK(&_bossl_mu);
        return 0;
    }
    _bossl.ready = 1;
    BMUTEX_UNLOCK(&_bossl_mu);
    return 1;
}
#endif

BFN Val vhttps_request_raw(Val addr, Val server_name, Val wire, Val ca_file) {
    if (addr.tag != V_STR || server_name.tag != V_STR || wire.tag != V_STR || ca_file.tag != V_STR)
        return _verr_c("https_request_raw wants string arguments");
    if (vstr_has_nul(&addr) || vstr_has_nul(&server_name) || vstr_has_nul(&ca_file))
        return _verr_c("https_request_raw embedded NUL in argument");
#if defined(_WIN32)
    return _verr_c("https_request_raw unavailable on this target");
#else
    char host[256]; int port = 0;
    if (!_vhost_port(vstr_c(&addr), host, sizeof host, &port))
        return _verr_c("address wants host:port with port 1..65535");
    size_t hlen = strlen(host);
    if (hlen >= 2 && host[0] == '[' && host[hlen - 1] == ']') {
        memmove(host, host + 1, hlen - 2);
        host[hlen - 2] = 0;
    }
    const char *ca_raw = vstr_c(&ca_file);
    char ca_conf[8192];
    ca_conf[0] = 0;
    if (ca_raw[0] != 0) {
        const char *rp = vconfine_val(&ca_file);
        if (!rp) return _verr_c("path outside the program directory");
        snprintf(ca_conf, sizeof ca_conf, "%s", rp);
    }
    if (!_bossl_init()) return _verr_c("OpenSSL libssl unavailable");
    char port_s[16];
    snprintf(port_s, sizeof port_s, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res)
        return _verr_c("DNS resolution failed");
    bsock_t fd = BSOCK_INVALID;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        bsock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BSOCK_INVALID) continue;
        _vsk_confine(s);
        _vsk_nosigpipe(s);
        _vsk_timeouts(s);
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) { fd = s; break; }
        bsock_close(s);
    }
    freeaddrinfo(res);
    if (fd == BSOCK_INVALID) return _verr_c("TLS TCP connect failed");
    void *ctx = _bossl.SSL_CTX_new(_bossl.TLS_client_method());
    if (!ctx) { bsock_close(fd); return _verr_c("SSL_CTX_new failed"); }
    if (_bossl.SSL_CTX_set_options)
        _bossl.SSL_CTX_set_options(ctx, 0x80 /* SSL_OP_IGNORE_UNEXPECTED_EOF */);
    else
        _bossl.SSL_CTX_ctrl(ctx, 32 /* SSL_CTRL_OPTIONS */, 0x80, NULL);
    if (ca_conf[0] != 0) {
        struct stat ca_st;
        if (lstat(ca_conf, &ca_st) != 0 || !S_ISREG(ca_st.st_mode) ||
            _bossl.SSL_CTX_load_verify_locations(ctx, ca_conf, NULL) != 1) {
            _bossl.SSL_CTX_free(ctx); bsock_close(fd);
            return _verr_c("failed to load CA file");
        }
    } else if (_bossl.SSL_CTX_set_default_verify_paths) {
        _bossl.SSL_CTX_set_default_verify_paths(ctx);
    }
    if (_bossl.SSL_CTX_set_verify) _bossl.SSL_CTX_set_verify(ctx, 0x01 /* SSL_VERIFY_PEER */, NULL);
    void *ssl = _bossl.SSL_new(ctx);
    if (!ssl) { _bossl.SSL_CTX_free(ctx); bsock_close(fd); return _verr_c("SSL_new failed"); }
    _bossl.SSL_set_fd(ssl, fd);
    const char *sn_raw = vstr_c(&server_name);
    if (sn_raw[0] == 0) sn_raw = host;
    char sn_buf[256];
    const char *sn = sn_raw;
    size_t slen = strlen(sn_raw);
    if (slen >= 2 && slen < sizeof sn_buf && sn_raw[0] == '[' && sn_raw[slen - 1] == ']') {
        memcpy(sn_buf, sn_raw + 1, slen - 2);
        sn_buf[slen - 2] = 0;
        sn = sn_buf;
    }
    if (sn[0] == 0) {
        _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
        return _verr_c("TLS server name is required for certificate verification");
    }
    struct in_addr dummy4;
    struct in6_addr dummy6;
    int is_ip = (inet_pton(AF_INET, sn, &dummy4) == 1) ||
                (inet_pton(AF_INET6, sn, &dummy6) == 1);
    if (!is_ip && _bossl.SSL_ctrl) {
        _bossl.SSL_ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0, (void *)sn);
    }
    int host_ok = 0;
    if (is_ip) {
        if (_bossl.SSL_get0_param && _bossl.X509_VERIFY_PARAM_set1_ip_asc) {
            void *vp = _bossl.SSL_get0_param(ssl);
            if (vp && _bossl.X509_VERIFY_PARAM_set1_ip_asc(vp, sn) == 1) host_ok = 1;
        }
    } else {
        if (_bossl.SSL_set1_host(ssl, sn) == 1) host_ok = 1;
    }
    if (!host_ok) {
        _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
        return _verr_c("TLS peer identity verification setup failed");
    }
    if (_bossl.SSL_connect(ssl) != 1 || _bossl.SSL_get_verify_result(ssl) != 0) {
        _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
        return _verr_c("TLS handshake or certificate verification failed");
    }
    const char *wp = vstr_c(&wire);
    size_t wleft = vstr_len_fast(&wire), wdone = 0;
    while (wleft > 0) {
        int chunk = wleft > 0x4000 ? 0x4000 : (int)wleft;
        int nw = _bossl.SSL_write(ssl, wp + wdone, chunk);
        if (nw <= 0) {
            _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
            return _verr_c("TLS write failed");
        }
        wdone += (size_t)nw;
        wleft -= (size_t)nw;
    }
    size_t cap = 4096, len = 0;
    char *rbuf = (char *)malloc(cap + 1);
    if (!rbuf) vfatal_oom();
    for (;;) {
        if (len + 4096 > cap) {
            if (cap >= (16u << 20)) {
                free(rbuf); _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
                return _verr_c("HTTPS response exceeds 16 MB");
            }
            cap *= 2;
            char *nb = (char *)realloc(rbuf, cap + 1);
            if (!nb) vfatal_oom();
            rbuf = nb;
        }
        errno = 0;
        int nr = _bossl.SSL_read(ssl, rbuf + len, 4096);
        if (nr > 0) { len += (size_t)nr; continue; }
        int read_errno = errno;
        int ssl_err = _bossl.SSL_get_error(ssl, nr);
        if ((ssl_err == 2 || ssl_err == 3) && read_errno == EINTR) continue;
        if (read_errno == EAGAIN || read_errno == EWOULDBLOCK || read_errno == ETIMEDOUT) {
            free(rbuf); _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
            return _verr_c("TLS read timed out");
        }
        /* Only clean close_notify (6) or a ragged transport EOF (5, ret 0)
         * ends the response. Record/protocol errors must discard partial data. */
        if (ssl_err == 6 || (ssl_err == 5 && nr == 0)) break;
        free(rbuf); _bossl.SSL_free(ssl); _bossl.SSL_CTX_free(ctx); bsock_close(fd);
        return _verr_c("TLS read/write failed");
    }
    _bossl.SSL_shutdown(ssl);
    _bossl.SSL_free(ssl);
    _bossl.SSL_CTX_free(ctx);
    bsock_close(fd);
    rbuf[len] = 0;
    if (!utf8_valid(rbuf, (bint)len) || memchr(rbuf, 0, len) != NULL) {
        free(rbuf);
        return verr("invalid utf-8");
    }
    Vec *a = vvec();
    vec_push(a, vstrn(rbuf, (bint)len));
    free(rbuf);
    return vtagv("ok", a);
#endif
}

#if !defined(_WIN32)
typedef struct _BSqlApi {
    int ready;
    int (*open)(const char *, void **);
    int (*busy_timeout)(void *, int);
    int (*close_v2)(void *);
    int (*prepare_v2)(void *, const char *, int, void **, const char **);
    int (*bind_int64)(void *, int, int64_t);
    int (*bind_double)(void *, int, double);
    int (*bind_text)(void *, int, const char *, int, void (*)(void *));
    int (*bind_null)(void *, int);
    int (*step)(void *);
    int (*column_count)(void *);
    const char *(*column_name)(void *, int);
    int (*column_type)(void *, int);
    int64_t (*column_int64)(void *, int);
    double (*column_double)(void *, int);
    const unsigned char *(*column_text)(void *, int);
    const void *(*column_blob)(void *, int);
    int (*column_bytes)(void *, int);
    int (*finalize)(void *);
    int (*exec)(void *, const char *, void *, void *, char **);
    void (*free_mem)(void *);
    const char *(*errmsg)(void *);
    int (*changes)(void *);
} BSqlApi;
static BSqlApi _bsql;
static bmutex_t _bsql_mu = BMUTEX_INIT;
#define _VSQL_MAX 128
#ifndef BRAT_MAX_SQL_ROWS
#define BRAT_MAX_SQL_ROWS 100000
#endif
#ifndef BRAT_MAX_SQL_COLS
#define BRAT_MAX_SQL_COLS 1024
#endif
#ifndef BRAT_MAX_SQL_BLOB
#define BRAT_MAX_SQL_BLOB (4 * 1024 * 1024)
#endif
#ifndef BRAT_MAX_SQL_RESULT_BYTES
#define BRAT_MAX_SQL_RESULT_BYTES (64 * 1024 * 1024)
#endif
typedef struct {
    void *db;
    uint64_t gen;
    int active;
    int closing;
    int mu_inited;
    bmutex_t op_mu;
} BSqlSlot;
static BSqlSlot _vsql_slots[_VSQL_MAX];
static uint64_t _vsql_next_gen = 1;

static BSqlSlot *_vsql_acquire(bint handle) {
    if (handle <= 0) return NULL;
    int slot = (int)((uint64_t)handle & 0xFFu);
    uint64_t gen = (uint64_t)handle >> 8;
    if (slot <= 0 || slot >= _VSQL_MAX || gen == 0) return NULL;
    BMUTEX_LOCK(&_bsql_mu);
    BSqlSlot *s = &_vsql_slots[slot];
    if (!s->db || s->closing || s->gen != gen) {
        BMUTEX_UNLOCK(&_bsql_mu);
        return NULL;
    }
    s->active++;
    BMUTEX_UNLOCK(&_bsql_mu);
    BMUTEX_LOCK(&s->op_mu);
    return s;
}

static void _vsql_release(BSqlSlot *s) {
    BMUTEX_UNLOCK(&s->op_mu);
    void *to_close = NULL;
    BMUTEX_LOCK(&_bsql_mu);
    s->active--;
    if (s->closing && s->active == 0) {
        to_close = s->db;
        s->db = NULL;
        s->closing = 0;
        s->gen = 0;
    }
    BMUTEX_UNLOCK(&_bsql_mu);
    if (to_close && _bsql.close_v2) _bsql.close_v2(to_close);
}

static int _bsql_init(void) {
    BMUTEX_LOCK(&_bsql_mu);
    if (_bsql.ready != 0) {
        int r = _bsql.ready;
        BMUTEX_UNLOCK(&_bsql_mu);
        return r > 0;
    }
    const char *names[] = {"libsqlite3.so.0", "libsqlite3.so", "libsqlite3.0.dylib", "libsqlite3.dylib", NULL};
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        _bsql.ready = -1;
        BMUTEX_UNLOCK(&_bsql_mu);
        return 0;
    }
    *(void **)&_bsql.open = dlsym(h, "sqlite3_open");
    *(void **)&_bsql.busy_timeout = dlsym(h, "sqlite3_busy_timeout");
    *(void **)&_bsql.close_v2 = dlsym(h, "sqlite3_close_v2");
    *(void **)&_bsql.prepare_v2 = dlsym(h, "sqlite3_prepare_v2");
    *(void **)&_bsql.bind_int64 = dlsym(h, "sqlite3_bind_int64");
    *(void **)&_bsql.bind_double = dlsym(h, "sqlite3_bind_double");
    *(void **)&_bsql.bind_text = dlsym(h, "sqlite3_bind_text");
    *(void **)&_bsql.bind_null = dlsym(h, "sqlite3_bind_null");
    *(void **)&_bsql.step = dlsym(h, "sqlite3_step");
    *(void **)&_bsql.column_count = dlsym(h, "sqlite3_column_count");
    *(void **)&_bsql.column_name = dlsym(h, "sqlite3_column_name");
    *(void **)&_bsql.column_type = dlsym(h, "sqlite3_column_type");
    *(void **)&_bsql.column_int64 = dlsym(h, "sqlite3_column_int64");
    *(void **)&_bsql.column_double = dlsym(h, "sqlite3_column_double");
    *(void **)&_bsql.column_text = dlsym(h, "sqlite3_column_text");
    *(void **)&_bsql.column_blob = dlsym(h, "sqlite3_column_blob");
    *(void **)&_bsql.column_bytes = dlsym(h, "sqlite3_column_bytes");
    *(void **)&_bsql.finalize = dlsym(h, "sqlite3_finalize");
    *(void **)&_bsql.exec = dlsym(h, "sqlite3_exec");
    *(void **)&_bsql.free_mem = dlsym(h, "sqlite3_free");
    *(void **)&_bsql.errmsg = dlsym(h, "sqlite3_errmsg");
    *(void **)&_bsql.changes = dlsym(h, "sqlite3_changes");
    if (!_bsql.open || !_bsql.busy_timeout || !_bsql.close_v2 || !_bsql.prepare_v2 ||
        !_bsql.bind_int64 || !_bsql.bind_double || !_bsql.bind_text || !_bsql.bind_null ||
        !_bsql.step || !_bsql.column_count || !_bsql.column_name || !_bsql.column_type ||
        !_bsql.column_int64 || !_bsql.column_double || !_bsql.column_text ||
        !_bsql.column_blob || !_bsql.column_bytes || !_bsql.finalize || !_bsql.exec ||
        !_bsql.free_mem || !_bsql.errmsg || !_bsql.changes) {
        _bsql.ready = -1;
        BMUTEX_UNLOCK(&_bsql_mu);
        return 0;
    }
    _bsql.ready = 1;
    BMUTEX_UNLOCK(&_bsql_mu);
    return 1;
}
static int _bsql_tail_empty(const char *p) {
    if (!p) return 1;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == ';') {
            p++;
            continue;
        }
        if (c == '-' && p[1] == '-') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        return 0;
    }
    return 1;
}
#endif

BFN Val vsql_open(Val pv) {
    char pbuf[8192];
    const char *raw = NULL;
    int pbad = _vpath_bytes(&pv, pbuf, sizeof pbuf, &raw);
    if (pbad == 1) return verr("sql_open wants string path");
#if defined(_WIN32)
    return verr("sqlite3 unavailable on this target");
#else
    if (pbad == 2) return verr("embedded NUL in database path");
    if (pbad == 4) return verr("sql_open byte out of range 0..255");
    if (pbad) return verr("path outside the program directory");
    const char *use_path = ":memory:";
    char conf_buf[8192];
    if (strcmp(raw, ":memory:") != 0) {
        const char *rp = vconfine(raw);
        if (!rp) return verr("path outside the program directory");
        snprintf(conf_buf, sizeof conf_buf, "%s", rp);
        if (!_vfiles_free()) {
            char leaf[1024];
            int esc = 0;
            int dfd = _vopen_parent_confined(conf_buf, leaf, sizeof leaf, &esc);
            if (dfd < 0) return esc ? verr("path escaped during open") : verr_os(conf_buf);
            static const char *sfx[] = {"", "-wal", "-journal", "-shm"};
            for (int k = 0; k < 4; k++) {
                char sc[1040];
                snprintf(sc, sizeof sc, "%s%s", leaf, sfx[k]);
                struct stat st;
                if (fstatat(dfd, sc, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode)) {
                    close(dfd);
                    return verr("path escaped during open");
                }
            }
            int fl = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
            fl |= O_NOFOLLOW;
#endif
            int tfd = openat(dfd, leaf, fl, 0666);
            int te = errno;
            close(dfd);
            if (tfd < 0) {
                if (te == ELOOP) return verr("path escaped during open");
                errno = te;
                return verr_os(conf_buf);
            }
            struct stat tst;
            int ok_fd = _vfd_confined(tfd) && (fstat(tfd, &tst) == 0 && S_ISREG(tst.st_mode));
            close(tfd);
            if (!ok_fd) return verr("path escaped during open");
        }
        use_path = conf_buf;
    }
    if (!_bsql_init()) return verr("libsqlite3 unavailable");
    void *db = NULL;
    if (_bsql.open(use_path, &db) != 0 || !db) {
        const char *em = db ? _bsql.errmsg(db) : "sqlite3_open failed";
        Val r = verr(em);
        if (db) _bsql.close_v2(db);
        return r;
    }
    if (_bsql.busy_timeout(db, 5000) != 0) {
        Val r = verr(_bsql.errmsg(db));
        _bsql.close_v2(db);
        return r;
    }
    if (strcmp(use_path, ":memory:") != 0 && !_vfiles_free()) {
        struct stat pst;
        if (lstat(use_path, &pst) == 0 && !S_ISREG(pst.st_mode)) {
            _bsql.close_v2(db);
            return verr("path escaped during open");
        }
    }
    BMUTEX_LOCK(&_bsql_mu);
    int slot = -1;
    uint64_t gen = 0;
    for (int i = 1; i < _VSQL_MAX; i++) {
        BSqlSlot *s = &_vsql_slots[i];
        if (!s->db && !s->closing && s->active == 0) {
            if (!s->mu_inited) {
                pthread_mutex_init(&s->op_mu, NULL);
                s->mu_inited = 1;
            }
            if (_vsql_next_gen >= (1ULL << 54)) _vsql_next_gen = 1;
            gen = _vsql_next_gen++;
            s->db = db;
            s->gen = gen;
            s->active = 0;
            s->closing = 0;
            slot = i;
            break;
        }
    }
    BMUTEX_UNLOCK(&_bsql_mu);
    if (slot < 0) {
        _bsql.close_v2(db);
        return verr("too many open sqlite databases");
    }
    return _vok_int((bint)((gen << 8) | (uint64_t)slot));
#endif
}

BFN Val vsql_close(Val sv) {
    if (sv.tag != V_INT) return verr("bad sqlite handle");
#if defined(_WIN32)
    return verr("bad sqlite handle");
#else
    bint handle = sv.as.ival;
    if (handle <= 0) return verr("bad sqlite handle");
    int slot = (int)((uint64_t)handle & 0xFFu);
    uint64_t gen = (uint64_t)handle >> 8;
    if (slot <= 0 || slot >= _VSQL_MAX || gen == 0) return verr("bad sqlite handle");
    void *to_close = NULL;
    BMUTEX_LOCK(&_bsql_mu);
    BSqlSlot *s = &_vsql_slots[slot];
    if (!s->db || s->closing || s->gen != gen) {
        BMUTEX_UNLOCK(&_bsql_mu);
        return verr("bad sqlite handle");
    }
    if (s->active == 0) {
        to_close = s->db;
        s->db = NULL;
        s->gen = 0;
        s->closing = 0;
    } else {
        s->closing = 1;
    }
    BMUTEX_UNLOCK(&_bsql_mu);
    if (to_close) _bsql.close_v2(to_close);
    return _vok_int(0);
#endif
}

BFN Val vsql_exec(Val sv, Val sqlv) {
    if (sv.tag != V_INT || sqlv.tag != V_STR) return verr("sql_exec wants handle and sql string");
    if (vstr_has_nul(&sqlv)) return verr("embedded NUL in sql string");
#if defined(_WIN32)
    return verr("bad sqlite handle");
#else
    BSqlSlot *slot = _vsql_acquire(sv.as.ival);
    if (!slot) return verr("bad sqlite handle");
    void *db = slot->db;
    char *errmsg = NULL;
    int rc = _bsql.exec(db, vstr_c(&sqlv), NULL, NULL, &errmsg);
    if (rc != 0) {
        Val errv = verr(errmsg ? errmsg : _bsql.errmsg(db));
        if (errmsg && _bsql.free_mem) _bsql.free_mem(errmsg);
        _vsql_release(slot);
        return errv;
    }
    int ch = _bsql.changes ? _bsql.changes(db) : 0;
    _vsql_release(slot);
    return _vok_int((bint)ch);
#endif
}

BFN Val vsql_query(Val sv, Val sqlv, Val paramv) {
    if (sv.tag != V_INT || sqlv.tag != V_STR || paramv.tag != V_VEC)
        return verr("sql_query wants (handle, sql, params_vec)");
    if (vstr_has_nul(&sqlv)) return verr("embedded NUL in sql string");
#if defined(_WIN32)
    return verr("bad sqlite handle");
#else
    size_t sql_len = vstr_len_fast(&sqlv);
    if (sql_len > 0x3fffffffu) return verr("sql string too large");
    BSqlSlot *slot = _vsql_acquire(sv.as.ival);
    if (!slot) return verr("bad sqlite handle");
    void *db = slot->db;
    void *stmt = NULL;
    const char *tail = NULL;
    if (_bsql.prepare_v2(db, vstr_c(&sqlv), (int)sql_len, &stmt, &tail) != 0 || !stmt) {
        Val errv = verr(_bsql.errmsg(db));
        _vsql_release(slot);
        return errv;
    }
    if (!_bsql_tail_empty(tail)) {
        _bsql.finalize(stmt);
        _vsql_release(slot);
        return verr("sql_query wants one statement");
    }
    Vec *pv = paramv.as.vec;
    for (bint i = 0; i < pv->len; i++) {
        Val el = pv->items[i];
        int idx = (int)(i + 1);
        int brc = 0;
        if (el.tag == V_INT) brc = _bsql.bind_int64(stmt, idx, el.as.ival);
        else if (el.tag == V_BOOL) brc = _bsql.bind_int64(stmt, idx, el.as.ival ? 1 : 0);
        else if (el.tag == V_FLOAT) brc = _bsql.bind_double(stmt, idx, el.as.fval);
        else if (el.tag == V_STR) {
            const char *sp = vstr_c(&el);
            size_t slen = vstr_len_fast(&el);
            if (slen > 0x3fffffffu) {
                _bsql.finalize(stmt);
                _vsql_release(slot);
                return verr("sqlite text parameter too large");
            }
            brc = _bsql.bind_text(stmt, idx, sp, (int)slen, (void (*)(void *))(intptr_t)-1);
        } else if (el.tag == V_NIL) brc = _bsql.bind_null(stmt, idx);
        else {
            _bsql.finalize(stmt);
            _vsql_release(slot);
            return verr("unsupported sqlite parameter type");
        }
        if (brc != 0) {
            Val errv = verr(_bsql.errmsg(db));
            _bsql.finalize(stmt);
            _vsql_release(slot);
            return errv;
        }
    }
    Vec *rows = vvec();
    size_t nrows = 0;
    size_t total_bytes = 0;
    for (;;) {
        int s = _bsql.step(stmt);
        if (s == 100 /* SQLITE_ROW */) {
            if (++nrows > (size_t)BRAT_MAX_SQL_ROWS) {
                _bsql.finalize(stmt);
                _vsql_release(slot);
                vrc_dec(vvecv(rows));
                return verr("sql_query exceeded maximum row limit");
            }
            int ncols = _bsql.column_count(stmt);
            if (ncols < 0 || ncols > BRAT_MAX_SQL_COLS) {
                _bsql.finalize(stmt);
                _vsql_release(slot);
                vrc_dec(vvecv(rows));
                return verr("sql_query exceeded maximum column limit");
            }
            Map *row = vmap("");
            for (int c = 0; c < ncols; c++) {
                const char *cname = _bsql.column_name(stmt, c);
                size_t cnlen = cname ? strlen(cname) : 0;
                int cty = _bsql.column_type(stmt, c);
                Val cv = vnil();
                size_t cell_cost = cnlen + 32;
                if (cty == 1) cv = vint(_bsql.column_int64(stmt, c));
                else if (cty == 2) cv = vfloat(_bsql.column_double(stmt, c));
                else if (cty == 3) {
                    const unsigned char *txt = _bsql.column_text(stmt, c);
                    int nb = _bsql.column_bytes(stmt, c);
                    if (nb < 0 || (size_t)nb > (size_t)BRAT_MAX_SQL_RESULT_BYTES) {
                        vrc_dec(vstruct(row));
                        _bsql.finalize(stmt);
                        _vsql_release(slot);
                        vrc_dec(vvecv(rows));
                        return verr("sql_query exceeded result size limit");
                    }
                    if (txt && nb > 0 &&
                        (memchr(txt, 0, (size_t)nb) != NULL ||
                         !utf8_valid((const char *)txt, (bint)nb))) {
                        vrc_dec(vstruct(row));
                        _bsql.finalize(stmt);
                        _vsql_release(slot);
                        vrc_dec(vvecv(rows));
                        return verr("invalid utf-8 or embedded NUL in sqlite text result");
                    }
                    cell_cost += (size_t)nb;
                    cv = txt ? vstrn((const char *)txt, (bint)nb) : vlit("");
                } else if (cty == 4) {
                    const unsigned char *bl = (const unsigned char *)_bsql.column_blob(stmt, c);
                    int nb = _bsql.column_bytes(stmt, c);
                    if (nb < 0 || nb > BRAT_MAX_SQL_BLOB) {
                        vrc_dec(vstruct(row));
                        _bsql.finalize(stmt);
                        _vsql_release(slot);
                        vrc_dec(vvecv(rows));
                        return verr("sql_query blob exceeds 4 MiB limit");
                    }
                    cell_cost += (size_t)nb * sizeof(Val);
                    if (total_bytes + cell_cost > (size_t)BRAT_MAX_SQL_RESULT_BYTES) {
                        vrc_dec(vstruct(row));
                        _bsql.finalize(stmt);
                        _vsql_release(slot);
                        vrc_dec(vvecv(rows));
                        return verr("sql_query exceeded result size limit");
                    }
                    Vec *bv = vvec_cap((bint)nb);
                    for (int k = 0; k < nb; k++) vec_push(bv, vint((bint)(bl ? bl[k] : 0)));
                    cv = vvecv(bv);
                }
                if (total_bytes + cell_cost > (size_t)BRAT_MAX_SQL_RESULT_BYTES) {
                    vrc_dec(cv);
                    vrc_dec(vstruct(row));
                    _bsql.finalize(stmt);
                    _vsql_release(slot);
                    vrc_dec(vvecv(rows));
                    return verr("sql_query exceeded result size limit");
                }
                total_bytes += cell_cost;
                map_set(row, cname ? cname : "", cv);
            }
            vec_push(rows, vstruct(row));
        } else if (s == 101 /* SQLITE_DONE */) {
            break;
        } else {
            Val errv = verr(_bsql.errmsg(db));
            _bsql.finalize(stmt);
            _vsql_release(slot);
            vrc_dec(vvecv(rows));
            return errv;
        }
    }
    _bsql.finalize(stmt);
    _vsql_release(slot);
    Vec *a = vvec();
    vec_push(a, vvecv(rows));
    return vtagv("ok", a);
#endif
}

BFN Val vwritebytes(Val p, Val d) {
    if (d.tag != V_VEC) return verr("write_bytes wants string path and byte vec");
    char pbuf[8192];
    const char *praw = NULL;
    int pbad = _vpath_bytes(&p, pbuf, sizeof pbuf, &praw);
    if (pbad == 1) return verr("write_bytes wants string path and byte vec");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("write_bytes byte out of range 0..255");
    if (pbad) return verr("path outside the program directory");
    Vec *bv = d.as.vec;
    bint n = bv->len;
    unsigned char *raw = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
    if (n > 0 && !raw) { vraise("out of memory"); return vunwind; }
    for (bint i = 0; i < n; i++) {
        Val el = bv->items[i];
        if (el.tag != V_INT || el.as.ival < 0 || el.as.ival > 255) {
            free(raw);
            return verr("write_bytes byte out of range 0..255");
        }
        raw[i] = (unsigned char)el.as.ival;
    }
    const char *rp = vconfine(praw);
    if (!rp) { free(raw); return verr("path outside the program directory"); }
    int escaped = 0;
    FILE *f = _vopen_confined_stream(rp, 1, 0, &escaped, NULL);
    if (!f) {
        int e = errno;
        free(raw);
        errno = e;
        return escaped ? verr("path escaped during open") : verr_os(rp);
    }
    errno = 0;
    size_t w = n > 0 ? fwrite(raw, 1, (size_t)n, f) : 0;
    int werr = errno;
    int ferr = ferror(f);
    errno = 0;
    int cerr = fclose(f);
    int clerr = errno;
    free(raw);
    if (w != (size_t)n || ferr) { errno = werr ? werr : EIO; return verr_os(rp); }
    if (cerr != 0) { errno = clerr ? clerr : EIO; return verr_os(rp); }
    Vec *a = vvec();
    vec_push(a, vint(n));
    return vtagv("ok", a);
}

BFN int _vexec_allowed(const char *cmd) {
    const char *v = getenv("BRAT_EXEC");
    if (v) {
        while (*v == ' ' || *v == '\t') v++;
        size_t n = strlen(v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
        if ((n == 1 && v[0] == '1') ||
            (n == 2 && tolower((unsigned char)v[0]) == 'o' && tolower((unsigned char)v[1]) == 'n') ||
            (n == 3 && tolower((unsigned char)v[0]) == 'y' && tolower((unsigned char)v[1]) == 'e' && tolower((unsigned char)v[2]) == 's') ||
            (n == 4 && tolower((unsigned char)v[0]) == 't' && tolower((unsigned char)v[1]) == 'r' && tolower((unsigned char)v[2]) == 'u' && tolower((unsigned char)v[3]) == 'e'))
            return 1;
    }
    const char *al = getenv("BRAT_EXEC_ALLOW");
    if (!al || !*al) return 0;
    int has_sep = (strchr(cmd, '/') != NULL || strchr(cmd, '\\') != NULL);
    size_t clen = strlen(cmd);
    const char *p = al;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != ',') q++;
        const char *end = q;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t tlen = (size_t)(end - p);
        if (tlen > 0) {
            int tok_sep = (memchr(p, '/', tlen) != NULL || memchr(p, '\\', tlen) != NULL);
            if (tok_sep) {
                if (tlen == clen && memcmp(p, cmd, clen) == 0) return 1;
            } else if (!has_sep && tlen == clen && memcmp(p, cmd, clen) == 0) {
                return 1;
            }
        }
        p = q;
    }
    return 0;
}

static bint _vproc_default_timeout_ms(void) {
    const char *ev = getenv("BRAT_PROC_TIMEOUT_MS");
    if (ev && *ev) {
        char *end = NULL;
        long long v = strtoll(ev, &end, 10);
        if (end != ev && *end == 0 && v > 0 && v <= 3600000LL) return (bint)v;
    }
    return 60000;
}

static uint64_t _vproc_mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

static char *_vbytes_to_cstr(Vec *v) {
    char *s = (char *)malloc((size_t)v->len + 1);
    if (!s) return NULL;
    for (bint i = 0; i < v->len; i++) s[i] = (char)v->items[i].as.ival;
    s[v->len] = 0;
    return s;
}

BFN Val vproc_run_to(Val cmd, Val argv, Val tmo_v) {
    if (cmd.tag != V_STR || vstr_empty(cmd)) return verr("proc_run wants non-empty command string");
    if (vstr_has_nul(&cmd)) return verr("embedded NUL in command string");
    if (argv.tag != V_VEC) return verr("proc_run wants vec[str] args");
    bint timeout_ms = 0;
    if (tmo_v.tag == V_INT) timeout_ms = tmo_v.as.ival;
    else return verr("proc_run timeout must be an integer (ms)");
    if (timeout_ms <= 0) timeout_ms = _vproc_default_timeout_ms();
    if (timeout_ms > 3600000) timeout_ms = 3600000;
    Vec *av = argv.as.vec;
    for (bint i = 0; i < av->len; i++) {
        Val it = av->items[i];
        if (it.tag == V_STR) {
            if (vstr_has_nul(&it)) return verr("embedded NUL in command argument");
        } else if (it.tag == V_VEC) {
            Vec *bv = it.as.vec;
            if (!bv) return verr("proc_run wants vec[str] args");
            for (bint j = 0; j < bv->len; j++) {
                Val el = bv->items[j];
                if (el.tag != V_INT || el.as.ival < 0 || el.as.ival > 255)
                    return verr("proc_run byte out of range 0..255");
                if (el.as.ival == 0) return verr("embedded NUL in command argument");
            }
        } else {
            return verr("proc_run wants vec[str] args");
        }
    }
    /* Parent enforces the capability policy (`BRAT_EXEC` / `BRAT_EXEC_ALLOW`)
     * before forking; the child then invokes `execve` directly, skipping empty
     * or relative `PATH` entries (CWE-426) and avoiding POSIX `execvp`'s
     * implicit `/bin/sh` fallback on `ENOEXEC`. */
    if (!_vexec_allowed(vstr_c(&cmd))) return verr("proc_run refused: set BRAT_EXEC=1 or BRAT_EXEC_ALLOW");
#if defined(_WIN32)
    return verr("proc_run unsupported on this platform");
#else
    int out_p[2] = {-1, -1}, err_p[2] = {-1, -1}, ex_p[2] = {-1, -1};
    if (pipe(out_p) != 0) return verr_os("pipe");
    if (pipe(err_p) != 0) { close(out_p[0]); close(out_p[1]); return verr_os("pipe"); }
    if (pipe(ex_p) != 0) {
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        return verr_os("pipe");
    }
    fcntl(out_p[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_p[0], F_SETFD, FD_CLOEXEC);
    fcntl(ex_p[0], F_SETFD, FD_CLOEXEC);
    fcntl(ex_p[1], F_SETFD, FD_CLOEXEC);
    char **cargv = (char **)calloc((size_t)av->len + 2, sizeof(char *));
    if (!cargv) {
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        close(ex_p[0]); close(ex_p[1]);
        vraise("out of memory");
        return vunwind;
    }
    char **cowned = (char **)calloc((size_t)av->len + 1, sizeof(char *));
    if (!cowned) {
        free(cargv);
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        close(ex_p[0]); close(ex_p[1]);
        vraise("out of memory");
        return vunwind;
    }
    cargv[0] = (char *)vstr_c(&cmd);
    int cargv_oom = 0;
    for (bint i = 0; i < av->len; i++) {
        if (av->items[i].tag == V_STR) {
            cargv[i + 1] = (char *)vstr_c(&av->items[i]);
        } else {
            cowned[i] = _vbytes_to_cstr(av->items[i].as.vec);
            if (!cowned[i]) { cargv_oom = 1; break; }
            cargv[i + 1] = cowned[i];
        }
    }
    if (cargv_oom) {
        for (bint i = 0; i < av->len; i++) free(cowned[i]);
        free(cowned);
        free(cargv);
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        close(ex_p[0]); close(ex_p[1]);
        vraise("out of memory");
        return vunwind;
    }
    cargv[av->len + 1] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        for (bint i = 0; i < av->len; i++) free(cowned[i]);
        free(cowned);
        free(cargv);
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        close(ex_p[0]); close(ex_p[1]);
        errno = e;
        return verr_os("fork");
    }
    if (pid == 0) {
        extern char **environ;
        setpgid(0, 0);
        close(out_p[0]);
        close(err_p[0]);
        close(ex_p[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); if (devnull > 2) close(devnull); }
        dup2(out_p[1], STDOUT_FILENO);
        dup2(err_p[1], STDERR_FILENO);
        if (out_p[1] > 2) close(out_p[1]);
        if (err_p[1] > 2) close(err_p[1]);
        const char *prog = cargv[0];
        if (strchr(prog, '/') != NULL) {
            execve(prog, cargv, environ);
        } else {
            const char *path_env = getenv("PATH");
            if (!path_env || !*path_env) path_env = "/usr/bin:/bin";
            int sticky_err = ENOENT;
            const char *p = path_env;
            char full[4096];
            size_t plen = strlen(prog);
            while (*p) {
                const char *col = strchr(p, ':');
                size_t dlen = col ? (size_t)(col - p) : strlen(p);
                if (dlen > 0 && p[0] == '/' && dlen + 1 + plen < sizeof full) {
                    memcpy(full, p, dlen);
                    full[dlen] = '/';
                    memcpy(full + dlen + 1, prog, plen + 1);
                    execve(full, cargv, environ);
                    if (errno == EACCES || errno == EPERM || errno == ENOEXEC)
                        sticky_err = errno;
                }
                if (!col) break;
                p = col + 1;
            }
            errno = sticky_err;
        }
        int e = errno;
        ssize_t _w = write(ex_p[1], &e, sizeof e);
        (void)_w;
        close(ex_p[1]);
        _exit(127);
    }
    setpgid(pid, pid);
    for (bint i = 0; i < av->len; i++) free(cowned[i]);
    free(cowned);
    free(cargv);
    close(out_p[1]);
    close(err_p[1]);
    close(ex_p[1]);

    int child_err = 0;
    ssize_t er = 0;
    while ((er = read(ex_p[0], &child_err, sizeof child_err)) < 0 && errno == EINTR) {}
    close(ex_p[0]);
    if (er == (ssize_t)sizeof child_err && child_err != 0) {
        close(out_p[0]);
        close(err_p[0]);
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        errno = child_err;
        return verr_os(vstr_c(&cmd));
    }

    const size_t max_total = 16u * 1024u * 1024u;
    uint64_t deadline = _vproc_mono_ms() + (uint64_t)timeout_ms;
    uint64_t post_exit_deadline = 0;
    int status = 0, reaped = 0, timed_out = 0;
    char *obuf = NULL, *ebuf = NULL;
    size_t olen = 0, elen = 0, ocap = 0, ecap = 0;
    int o_open = 1, e_open = 1, over = 0;
    char tmp[4096];
    while (o_open || e_open) {
        uint64_t now = _vproc_mono_ms();
        if (now >= deadline) { timed_out = 1; break; }
        if (!reaped) {
            pid_t wr = waitpid(pid, &status, WNOHANG);
            if (wr == pid) {
                reaped = 1;
                post_exit_deadline = now + 50ULL;
            }
        }
        if (reaped && now >= post_exit_deadline) break;
        uint64_t eff_dl = (reaped && post_exit_deadline < deadline) ? post_exit_deadline : deadline;
        uint64_t rem = eff_dl > now ? (eff_dl - now) : 0;
        int wait_ms = rem > 50ULL ? 50 : (int)rem;
        struct pollfd fds[2];
        nfds_t nf = 0;
        int oi = -1, ei = -1;
        if (o_open) { oi = (int)nf; fds[nf].fd = out_p[0]; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++; }
        if (e_open) { ei = (int)nf; fds[nf].fd = err_p[0]; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++; }
        int pr = poll(fds, nf, wait_ms);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(out_p[0], tmp, sizeof tmp);
            if (k > 0) {
                if (olen + elen + (size_t)k > max_total) { over = 1; break; }
                if (olen + (size_t)k + 1 > ocap) {
                    size_t ncap = ocap ? ocap * 2 : 4096;
                    while (ncap < olen + (size_t)k + 1) ncap *= 2;
                    obuf = (char *)bgrow(obuf, ncap);
                    ocap = ncap;
                }
                memcpy(obuf + olen, tmp, (size_t)k);
                olen += (size_t)k;
            } else if (k == 0 || errno != EINTR) {
                o_open = 0;
            }
        }
        if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(err_p[0], tmp, sizeof tmp);
            if (k > 0) {
                if (olen + elen + (size_t)k > max_total) { over = 1; break; }
                if (elen + (size_t)k + 1 > ecap) {
                    size_t ncap = ecap ? ecap * 2 : 4096;
                    while (ncap < elen + (size_t)k + 1) ncap *= 2;
                    ebuf = (char *)bgrow(ebuf, ncap);
                    ecap = ncap;
                }
                memcpy(ebuf + elen, tmp, (size_t)k);
                elen += (size_t)k;
            } else if (k == 0 || errno != EINTR) {
                e_open = 0;
            }
        }
    }
    close(out_p[0]);
    close(err_p[0]);
    if (!reaped && !over && !timed_out) {
        for (;;) {
            pid_t wr = waitpid(pid, &status, WNOHANG);
            if (wr == pid) { reaped = 1; break; }
            if (wr < 0) {
                if (errno == EINTR) continue;
                reaped = 1;
                break;
            }
            uint64_t now = _vproc_mono_ms();
            if (now >= deadline) { timed_out = 1; break; }
            uint64_t rem = deadline - now;
            poll(NULL, 0, rem > 50ULL ? 50 : (int)rem);
        }
    }
    if (over || timed_out) {
        if (kill(-pid, SIGKILL) != 0 && !reaped) kill(pid, SIGKILL);
    }
    if (!reaped) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (timed_out) {
        free(obuf);
        free(ebuf);
        return verr("proc_run timed out");
    }
    if (over) {
        free(obuf);
        free(ebuf);
        return verr("proc_run output exceeded 16 MiB limit");
    }
    if ((olen > 0 && (!utf8_valid(obuf, (bint)olen) || memchr(obuf, 0, olen) != NULL)) ||
        (elen > 0 && (!utf8_valid(ebuf, (bint)elen) || memchr(ebuf, 0, elen) != NULL))) {
        free(obuf);
        free(ebuf);
        return verr("invalid utf-8");
    }
    bint code = WIFEXITED(status) ? (bint)WEXITSTATUS(status)
              : (WIFSIGNALED(status) ? -(bint)WTERMSIG(status) : -1);
    Map *m = vmap("");
    map_set(m, "code", vint(code));
    map_set(m, "out", olen > 0 ? vstrn(obuf, (bint)olen) : vlit(""));
    map_set(m, "err", elen > 0 ? vstrn(ebuf, (bint)elen) : vlit(""));
    free(obuf);
    free(ebuf);
    Vec *a = vvec();
    vec_push(a, vstruct(m));
    return vtagv("ok", a);
#endif
}

BFN Val vproc_run(Val cmd, Val argv) {
    return vproc_run_to(cmd, argv, vint(0));
}
/* Raise `fn: strerror(errno)`. Format before raising: vraise allocates and may clobber errno. */
BFN void vraise_errno(const char *fn) {
    char m[256];
    snprintf(m, sizeof m, "%s: %s", fn, strerror(errno));
    vraise(m);
}

BFN Val vstr_of(Val v) {
    if (v.tag == V_STR) return v.sso ? v : vstr(v.as.str);
    bint n = vsprint_len_q(v, 0);
    if (verr_pending()) return vunwind;
    char *b = (char *)malloc(n + 1);
    if (!b) vfatal_oom();
    bint wrote = vsprint_q(v, b, 0);
    b[wrote] = 0;
    Val r = vstrn(b, wrote);
    free(b);
    return r;
}
/* The whitespace `trim`, `split()` and the `parse_*` methods share: the six ASCII
   characters. The interpreter implements the same set (`_WS` in runtime.py). */
BFN_HOT int vis_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
BFN Val vsplit_ws(Val s) {
    if (s.tag != V_STR) { vraise("no method split"); return vunwind; }
    Vec *out = vvec();
    const char *p = vstr_c(&s);
    while (*p) {
        while (vis_space(*p)) p++;
        if (!*p) break;
        const char *q = p;
        while (*q && !vis_space(*q)) q++;
        vec_push(out, vstrn(p, q - p));
        p = q;
    }
    return vvecv(out);
}
BFN Val vsplit(Val s, Val sep) {
    if (sep.tag != V_STR) return vsplit_ws(s);
    if (s.tag != V_STR) { vraise("no method split"); return vunwind; }
    Vec *out = vvec();
    const char *p = vstr_c(&s);
    const char *seps = vstr_c(&sep);
    size_t dl = vstr_len_fast(&sep);
    if (!dl) { vec_push(out, vstr(p)); return vvecv(out); }
    const char *q;
    while ((q = strstr(p, seps)) != 0) {
        vec_push(out, vstrn(p, q - p));
        p = q + dl;
    }
    vec_push(out, vstr(p));
    return vvecv(out);
}
BFN Val vtrim(Val s) {
    if (s.tag != V_STR) { vraise("no method trim"); return vunwind; }
    const char *p = vstr_c(&s);
    while (vis_space(*p)) p++;
    bint n = strlen(p);
    while (n > 0 && vis_space(p[n-1])) n--;
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
    const char *sc = vstr_c(&s);
    size_t len = strlen(sc) + 8;
    /* balloc, not malloc: the result is a BRAT string, and a BRAT string is
     * dropped by its holder. A raw buffer has no count to decrement. */
    char *o = (char *)balloc(len);
    if (!o) vfatal_oom();
    bint on = 0;
    unsigned prev = 0;
    for (const char *p = sc; *p; ) {
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
    return vstr_buf(o, on);
}
BFN bint vaffix(Val s, Val p, int pre, int suf) {
    if (s.tag != V_STR || p.tag != V_STR) { vraise("no method affix"); return 0; }
    const char *sc = vstr_c(&s), *pc = vstr_c(&p);
    size_t lp = vstr_len_fast(&p);
    if (pre) return !strncmp(sc, pc, lp);
    size_t ls = vstr_len_fast(&s);
    if (suf) return lp <= ls && !strcmp(sc + ls - lp, pc);
    return strstr(sc, pc) != 0;
}
BFN Val vreplace(Val s, Val a, Val b) {
    if (s.tag != V_STR || a.tag != V_STR || b.tag != V_STR) { vraise("no method replace"); return vunwind; }
    const char *p = vstr_c(&s), *pat = vstr_c(&a), *rep = vstr_c(&b);
    size_t pl = strlen(pat), rl = strlen(rep);
    if (!pl) {
        /* Output is (sl+1)*rl + sl chars. Check the size before malloc:
           the old (sl+1)*(rl+1)+1 could overflow and under-allocate. */
        size_t sl = strlen(p);
        size_t gaps = 1;   /* one before every code point, and one at the end */
        for (size_t i = 0; i < sl; i++) if (((unsigned char)p[i] & 0xC0) != 0x80) gaps++;
        size_t cap = (size_t)1 << 30;   /* BRAT_MAX_STR */
        if (sl >= cap - 1 || rl > (cap - 1 - sl) / gaps) { vraise("out of memory"); return vunwind; }
        size_t olen = gaps * rl + sl;
        char *o = (char *)balloc(olen + 1);
        if (!o) vfatal_oom();
        char *w = o;
        memcpy(w, rep, rl); w += rl;
        for (size_t i = 0; i < sl;) {
            size_t n = (size_t)utf8_cplen(p + i);
            memcpy(w, p + i, n); w += n; i += n;
            memcpy(w, rep, rl); w += rl;
        }
        *w = 0;
        return vstr_buf(o, (bint)olen);
    }
    size_t total = strlen(p) + 1;
    char *o = (char *)balloc(total);
    if (!o) vfatal_oom();
    size_t n = 0;
    const char *q;
    while ((q = strstr(p, pat)) != 0) {
        size_t chunk = (size_t)(q - p);
        if (chunk > BRAT_MAX_STR - n || rl > BRAT_MAX_STR - n - chunk) {
            vrc_dec((Val){ .tag = V_STR, .as = { .str = o } });
            vraise("out of memory");
            return vunwind;
        }
        size_t need = n + chunk + rl + 1;
        if (need > total) {
            total = need > BRAT_MAX_STR / 2 ? need : need * 2;
            o = (char *)bgrow_rc(o, total);
        }
        memcpy(o + n, p, chunk); n += chunk;
        memcpy(o + n, rep, rl); n += rl;
        p = q + pl;
    }
    size_t rest_len = strlen(p);
    if (rest_len > BRAT_MAX_STR - n) {
        vrc_dec((Val){ .tag = V_STR, .as = { .str = o } });
        vraise("out of memory");
        return vunwind;
    }
    size_t rest = rest_len + 1;
    if (n + rest > total) { total = n + rest; o = (char *)bgrow_rc(o, total); }
    memcpy(o + n, p, rest); n += rest - 1;
    return vstr_buf(o, (bint)n);
}
BFN Val vindex_of(Val s, Val sub) {
    if (s.tag != V_STR || sub.tag != V_STR) { vraise("no method index_of"); return vunwind; }
    const char *sc = vstr_c(&s);
    char *p = strstr(sc, vstr_c(&sub));
    if (!p) return vnil();
    
    bint cps = 0;
    for (const char *q = sc; q < p; ) {
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
    const char *sc = vstr_c(&s);
    size_t sl = vstr_len_fast(&s);
    if (k <= 0 || !sl) return vstr("");
    if ((size_t)k > (size_t)-1 / sl) { vraise("out of memory"); return vunwind; }
    size_t total = sl * (size_t)k;
    if (total <= BRAT_SSO_MAX) {
        Val r;
        memset(&r, 0, sizeof(r));
        r.tag = V_STR; r.sso = 1; r.imm = 1;
        char *dst = (char *)&r + 1;
        for (bint i = 0; i < k; i++) memcpy(dst + (size_t)i * sl, sc, sl);
        dst[total] = '\0';
        ((unsigned char *)&r)[15] = (unsigned char)(BRAT_SSO_MAX - total);
        return r;
    }
    if (total > BRAT_MAX_STR) { vraise("out of memory"); return vunwind; }
    char *o = (char *)balloc(total + 1);
    if (!o) { vraise("out of memory"); return vunwind; }
    for (bint i = 0; i < k; i++) memcpy(o + (size_t)i * sl, sc, sl);
    o[total] = 0;
    return vstr_buf(o, (bint)total);
}
BFN Val vrepeat(Val s, Val n) {
    bint k = vlong(n);
    if (verr_pending()) return vunwind;
    return vrepeat_n(s, k);
}

BFN Val vlines(Val s) {
    if (s.tag != V_STR) { vraise("no method lines"); return vunwind; }
    Vec *o = vvec();
    const char *p = vstr_c(&s);
    if (!p[0]) return vvecv(o);
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
    const char *sc = vstr_c(&s), *sepc = vstr_c(&sep);
    if (!sepc[0]) { vraise("empty separator"); return vunwind; }
    char *p = strstr(sc, sepc);
    if (!p) return vnil();
    Vec *o = vvec();
    vec_push(o, vstrn(sc, (bint)(p - sc)));
    vec_push(o, vstr(p + vstr_len_fast(&sep)));
    return vvecv(o);
}
BFN Val vparse_int(Val s) {
    if (s.tag != V_STR) { vraise("no method parse_int"); return vunwind; }
    const char *p = vstr_c(&s);
    while (vis_space(*p)) p++;
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    if (!(*q >= '0' && *q <= '9')) { Vec *a = vvec(); vec_push(a, vstr("not a number")); return vtagv("err", a); }
    uint64_t acc = 0;
    int over = 0;
    while (*q >= '0' && *q <= '9') {
        unsigned d = (unsigned)(*q - '0');
        if (acc > (UINT64_MAX - d) / 10ULL) over = 1;
        else acc = acc * 10ULL + d;
        q++;
    }
    while (vis_space(*q)) q++;
    Vec *a = vvec();
    if (*q) { vec_push(a, vstr("not a number")); return vtagv("err", a); }
    /* -2^63 is the one magnitude a negative number may have and a positive one may not. */
    if (over || acc > ((*p == '-') ? (1ULL << 63) : (1ULL << 63) - 1)) {
        vec_push(a, vstr("number out of range"));
        return vtagv("err", a);
    }
    uint64_t u = (*p == '-') ? (0ULL - acc) : acc;
    bint v;
    memcpy(&v, &u, sizeof v);
    vec_push(a, vint(v));
    return vtagv("ok", a);
}
BFN Val vjoin(Val v, Val sep) {
    if (v.tag != V_VEC || sep.tag != V_STR) { vraise("no method join"); return vunwind; }
    Vec *w = v.as.vec;
    size_t total = 0;
    bint i;
    for (i = 0; i < w->len; i++) {
        size_t el = (size_t)vsprint_len_q(w->items[i], 0);
        if (verr_pending()) return vunwind;
        if (el > BRAT_MAX_STR - total) { vraise("out of memory"); return vunwind; }
        total += el;
    }
    const char *sepc = vstr_c(&sep);
    size_t sepl = vstr_len_fast(&sep);
    if (w->len > 1 && sepl > 0) {
        size_t gaps = (size_t)(w->len - 1);
        if (sepl > (BRAT_MAX_STR - total) / gaps) { vraise("out of memory"); return vunwind; }
        total += sepl * gaps;
    }
    char *out = (char *)balloc(total + 1);
    if (!out) vfatal_oom();
    bint on = 0;
    for (i = 0; i < w->len; i++) {
        if (i) { memcpy(out + on, sepc, sepl); on += (bint)sepl; }
        on += vsprint_q(w->items[i], out + on, 0);
    }
    out[on] = 0;
    return vstr_buf(out, on);
}
/* `tagof` is asked for inside loops and its answer never changes, so the name is
 * cached as an immortal string instead of a fresh allocation per call. */
static struct { const char *name; Val v; } _vtagof_cache[512];
static int _vtagof_n = 0;
static bmutex_t _vtagof_mu = BMUTEX_INIT;
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
    const char *name = v.tag == V_NIL ? "none" : (v.tag == V_TAG ? v.as.tv->tag : "plain");
    int seen = _vtagof_count();
    for (int i = 0; i < seen; i++) {
        if (strcmp(_vtagof_cache[i].name, name) == 0) return _vtagof_cache[i].v;
    }
    Val s = vstr(name);
    if (!s.sso && s.as.str) { ((BHead *)s.as.str - 1)->rc = -1; s.imm = 1; }
    BMUTEX_LOCK(&_vtagof_mu);
    if (_vtagof_n < 512) {
        for (int i = 0; i < _vtagof_n; i++) {
            if (strcmp(_vtagof_cache[i].name, name) == 0) {
                Val hit = _vtagof_cache[i].v;
                BMUTEX_UNLOCK(&_vtagof_mu);
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
    BMUTEX_UNLOCK(&_vtagof_mu);
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
static struct { BHead h; Vec v; } _vpoison_vec_box;
#define vpoison_vec_s (_vpoison_vec_box.v)
BFN Vec *vvec_of_m(Val v, const char *meth) {
    if (v.tag != V_VEC) {
        vraisef("no method %s", meth);
        vpoison_vec_s.len = 0;
        return &vpoison_vec_s;
    }
    return v.as.vec;
}
BFN Vec *vvec_of(Val v) {
    if (v.tag != V_VEC) { vraise("not a vec"); vpoison_vec_s.len = 0; return &vpoison_vec_s; }
    return v.as.vec;
}
BFN int vis_err(Val v) {
    return v.tag == V_TAG && !strcmp(v.as.tv->tag, "err");
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
    return v.tag == V_TAG && v.as.tv && v.as.tv->tag && !strcmp(v.as.tv->tag, "closure");
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
    case V_TAG: return v.as.tv && v.as.tv->tag ? v.as.tv->tag : "tag";
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
    Vec *args = v.as.tv;
    if (!args || !args->len) return vnil();
    return vrc_inc(args->items[0]);
}

BFN Val vunwrap(Val v) {
    if (v.tag == V_TAG && (!strcmp(v.as.tv->tag, "ok") || !strcmp(v.as.tv->tag, "some")))
        return vtag_payload(v);
    if (v.tag == V_TAG && !strcmp(v.as.tv->tag, "err")) {
        vraise_err_tag(v);
        return vnil();
    }

    vraisef("unwrap of %s (want Ok/Err/Some)", vtypename(v));
    return vnil();
}
BFN int vor_fb(Val v) {
    return v.tag == V_NIL;
}
BFN const char *vname(Val v) {
    if (v.tag != V_STR) { vraise("want str"); return NULL; }
    return vstr_pin(v);
}
/* Iterative cursor formatting via `vfmt_walk_` with `BRAT_MAX_VALUE_DEPTH`. */
BFN bint vsprint_q(Val v, char *o, bint q) {
    return vfmt_walk_(v, q, o, NULL);
}

BFN bint vsprint(Val v, char *o) { return vsprint_q(v, o, 0); }

BFN bint vsprint_len_q(Val v, bint q) {
    return vfmt_walk_(v, q, NULL, NULL);
}

BFN bint vsprint_len(Val v) { return vsprint_len_q(v, 0); }
BFN void vlogv(const char *lvl, Vec *a) {
    bint total = 0, i;
    for (i = 0; i < a->len; i++) {
        if (i) total += 1;
        total += vsprint_len(a->items[i]);
        if (verr_pending()) return;
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
    if (verr_pending()) { vrc_dec(x); return vunwind; }
    vec_push(w, x);
    return vint(w->len);
}
BFN Val vpop(Val v) {
    Vec *w = vvec_of_m(v, "pop");
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (!w->len) { BRAT_CTR_UNLOCK_OBJ(w); return vnil(); }
    Val r = w->items[--w->len];
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
BFN Val vfirst(Val v) {
    Vec *w = vvec_of_m(v, "first");
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (!w->len) { BRAT_CTR_UNLOCK_OBJ(w); return vnil(); }
    Val r = w->items[0];
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
BFN Val vlast(Val v) {
    Vec *w = vvec_of_m(v, "last");
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (!w->len) { BRAT_CTR_UNLOCK_OBJ(w); return vnil(); }
    Val r = w->items[w->len - 1];
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
static int vcmp_q(const void *pa, const void *pb) {
    if (verr_pending()) return 0;
    return vcmp(*(const Val *)pa, *(const Val *)pb);
}
BFN Val vsort(Val v) {
    Vec *w = vvec_of_m(v, "sort");
    if (verr_pending()) return vunwind;
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (w->len > 1) {
        qsort(w->items, (size_t)w->len, sizeof(Val), vcmp_q);
        if (verr_pending()) return vunwind;
    }
    return v;
}
BFN Val vrev(Val v) {
    Vec *w = vvec_of_m(v, "rev");
    if (verr_pending()) return vunwind;
    Vec *o = vvec();
    for (bint i = w->len - 1; i >= 0; i--) vec_push(o, vrc_inc(w->items[i]));
    return vvecv(o);
}
BFN Val vins(Val v, Val i, Val x) {
    Vec *w = vvec_of_m(v, "ins");
    if (verr_pending()) { vrc_dec(x); return vunwind; }
    bint k = vlong(i);
    if (verr_pending()) { vrc_dec(x); return vunwind; }
    BRAT_CTR_LOCK_OBJ(w);
    if (k < 0) k = (k < -w->len) ? 0 : k + w->len;
    if (k > w->len) k = w->len;
    if (w->len >= w->cap) {
        bint oldcap = w->cap;
        int ina = BIS_ARENA((BHead *)w - 1);
        bint ncap = bnext_cap(oldcap, sizeof(Val));
        size_t nbytes = bmul_size(ncap, sizeof(Val));
        w->cap = ncap;
        if (w->items == w->inl) {
            Val *nb = (Val *)barr_new(nbytes, ina, 0);
            if (oldcap > 0) memcpy(nb, w->inl, bmul_size(oldcap, sizeof(Val)));
            w->items = nb;
        } else {
            w->items = (Val *)barr_grow(w->items, ina,
                                        bmul_size(oldcap, sizeof(Val)), nbytes);
        }
    }
    w->items[w->len++] = x;
    memmove(w->items + k + 1, w->items + k, sizeof(Val) * (size_t)(w->len - 1 - k));
    w->items[k] = x;
    bint nlen = w->len;
    BRAT_CTR_UNLOCK_OBJ(w);
    return vint(nlen);
}
BFN Val vdel_at(Val v, Val i) {
    Vec *w = vvec_of_m(v, "del");
    if (verr_pending()) return vunwind;
    bint k = vlong(i);
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (k < 0) {
        if (k < -w->len) { BRAT_CTR_UNLOCK_OBJ(w); vraise("index out of range"); return vunwind; }
        k += w->len;
    }
    if (k < 0 || k >= w->len) { BRAT_CTR_UNLOCK_OBJ(w); vraise("index out of range"); return vunwind; }
    Val r = w->items[k];
    memmove(w->items + k, w->items + k + 1, sizeof(Val) * (size_t)(w->len - 1 - k));
    w->len--;
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
BFN Val vslice(Val v, Val a, Val b) {
    Vec *w = vvec_of_m(v, "slice");
    if (verr_pending()) return vunwind;
    bint n = w->len, x = vlong(a);
    if (verr_pending()) return vunwind;
    bint y = vlong(b);
    if (verr_pending()) return vunwind;
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
    if (verr_pending()) return vunwind;
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
    if (verr_pending()) return vunwind;
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (!w->len) return vnil();
    Val r = w->items[0];
    for (bint i = 1; i < w->len; i++) {
        if (vcmp(w->items[i], r) < 0) r = w->items[i];
        if (verr_pending()) return vunwind;
    }
    return r;
}
BFN Val vmax(Val v) {
    Vec *w = vvec_of_m(v, "max");
    if (verr_pending()) return vunwind;
    if (vmixed(w)) { vraise("type error in <"); return vunwind; }
    if (!w->len) return vnil();
    Val r = w->items[0];
    for (bint i = 1; i < w->len; i++) {
        if (vcmp(w->items[i], r) > 0) r = w->items[i];
        if (verr_pending()) return vunwind;
    }
    return r;
}
BFN Val venum(Val v) {
    Vec *w = vvec_of_m(v, "enumerate");
    if (verr_pending()) return vunwind;
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
    for (bint i = 0; i < m.as.map->used; i++) {
        if (m.as.map->keys[i].tag == V_MOVED) continue;
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
    for (bint i = 0; i < m.as.map->used; i++)
        if (m.as.map->keys[i].tag != V_MOVED) vec_push(o, vrc_inc(m.as.map->vals[i]));
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
        for (bint i = 0; i < v.as.map->used; i++)
            if (v.as.map->keys[i].tag != V_MOVED)
                map_set_v(m, vrc_inc(v.as.map->keys[i]), vrc_inc(v.as.map->vals[i]));
        return vstruct(m);
    }
    if (v.tag == V_STR) return vrc_inc(v);
    vraise("copy of non-container"); return vunwind;
}
BFN Val vchars(Val s) {
    if (s.tag != V_STR) { vraise("no method chars"); return vunwind; }
    Vec *o = vvec();
    for (const char *p = vstr_c(&s); *p; p += utf8_cplen(p)) vec_push(o, vstrn(p, utf8_cplen(p)));
    return vvecv(o);
}
BFN_HOT Val vstr_slice(Val s, Val a, Val b) {
    if (s.tag != V_STR) { vraise("no method slice"); return vunwind; }
    const char *sc = vstr_c(&s);
    bint n = 0;
    int asc = 0;
    vstr_meta(&s, &n, &asc);
    bint x = vlong(a);
    if (verr_pending()) return vunwind;
    bint y = vlong(b);
    if (verr_pending()) return vunwind;
    if (x < 0) x = (x < -n) ? 0 : x + n;
    if (x < 0) x = 0;
    if (x > n) x = n;
    if (y < 0) y = (y < -n) ? 0 : y + n;
    if (y < 0) y = 0;
    if (y > n) y = n;
    if (y <= x) return vstr_sso("", 0);
    if (asc) return vstrn(sc + x, y - x);
    const char *p = utf8_at(sc, x);
    const char *q = utf8_at(sc, y);
    return vstrn(p, (bint)(q - p));
}
/* `[+-]?(digits[.digits*] | .digits)([eE][+-]?digits)?` or `[+-]?(inf|infinity|nan)`,
   any case. strtod alone also takes hex floats, `nan(...)` and, under another
   locale, a decimal comma. Returns the length of the accepted prefix. */
BFN size_t vfloat_prefix(const char *p) {
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    if ((*q | 0x20) >= 'a' && (*q | 0x20) <= 'z') {
        static const char *const words[] = {"infinity", "inf", "nan"};
        for (int i = 0; i < 3; i++) {
            size_t n = 0;
            while (words[i][n] && (q[n] | 0x20) == words[i][n]) n++;
            if (!words[i][n]) return (size_t)(q - p) + n;
        }
        return 0;
    }
    const char *d = q;
    while (*q >= '0' && *q <= '9') q++;
    size_t whole = (size_t)(q - d);
    size_t frac = 0;
    if (*q == '.') {
        const char *f = ++q;
        while (*q >= '0' && *q <= '9') q++;
        frac = (size_t)(q - f);
    }
    if (whole == 0 && frac == 0) return 0;
    if (*q == 'e' || *q == 'E') {
        const char *x = q + 1;
        if (*x == '+' || *x == '-') x++;
        if (*x >= '0' && *x <= '9') {
            while (*x >= '0' && *x <= '9') x++;
            q = x;
        }
    }
    return (size_t)(q - p);
}
BFN Val vparse_float(Val s) {
    if (s.tag != V_STR) { vraise("no method parse_float"); return vunwind; }
    char *e;
    const char *p = vstr_c(&s);
    while (vis_space(*p)) p++;
    size_t n = vfloat_prefix(p);
    const char *tail = p + n;
    while (vis_space(*tail)) tail++;
    Vec *a = vvec();
    if (n == 0 || *tail) { vec_push(a, vstr("not a float")); return vtagv("err", a); }
    double v = strtod(p, &e);
    vec_push(a, vfloat(v));
    return vtagv("ok", a);
}
BFN Val vzip(Val a, Val b) {
    Vec *x = vvec_of_m(a, "zip");
    if (verr_pending()) return vunwind;
    Vec *y = vvec_of_m(b, "zip");
    if (verr_pending()) return vunwind;
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
    if (verr_pending()) return vunwind;
    bint k = vlong(n);
    if (verr_pending()) return vunwind;
    if (k <= 0) { vraise("chunks wants n > 0"); return vunwind; }
    Vec *o = vvec();
    for (bint i = 0; i < w->len; ) {
        bint end = (w->len - i > k) ? (i + k) : w->len;
        Vec *p = vvec();
        for (bint j = i; j < end; j++) vec_push(p, vrc_inc(w->items[j]));
        vec_push(o, vvecv(p));
        i = end;
    }
    return vvecv(o);
}

BFN void vjprint_d(Val v, int d) {
    if (d > BRAT_MAX_VALUE_DEPTH) { printf("\"...\""); return; }
    bint i;
    switch (v.tag) {
    case V_NIL: printf("null"); break;
    case V_BOOL: printf(v.as.b ? "true" : "false"); break;
    case V_INT: printf(BINT_FMT, (bint)v.as.ival); break;
    case V_FLOAT: printf("%s", vfmt_double(v.as.fval)); break;
    case V_STR:
        printf("\"");
        for (const char *p = vstr_c(&v); *p; p++) {
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
        for (i = 0; i < v.as.vec->len; i++) { if (i) printf(", "); vjprint_d(v.as.vec->items[i], d + 1); }
        printf("]");
        break;
    case V_STRUCT: {
        if (vclosure_env_map(v.as.map)) {
            printf("{\"$type\": \"__brat_closure_env\"}");
            break;
        }
        int ismap = !v.as.map->type[0];
        int need_sep = !ismap;
        printf("{");
        if (!ismap) printf("\"$type\": \"%s\"", v.as.map->type);
        for (i = 0; i < v.as.map->used; i++) {
            if (v.as.map->keys[i].tag == V_MOVED) continue;
            if (need_sep) printf(", ");
            need_sep = 1;
            vjprint_d(v.as.map->keys[i], d + 1);
            printf(": ");
            vjprint_d(v.as.map->vals[i], d + 1);
        }
        printf("}");
        break;
    }
    case V_TAG: {
        if (vtag_closure(v)) {
            printf("\"fn\"");
            break;
        }
        printf("[");
        printf("\"%s\"", v.as.tv->tag);
        for (i = 0; i < v.as.tv->len; i++) { printf(", "); vjprint_d(v.as.tv->items[i], d + 1); }
        printf("]");
        break;
    }
    default:
        vprint(v);
        break;
    }
}

BFN void vjprint(Val v) {
    vjprint_d(v, 0);
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
    BRAT_CTR_LOCK_OBJ(s.as.map);
    Val old = *o;
    *o = v;
    BRAT_CTR_UNLOCK_OBJ(s.as.map);
    vrelease1(old);
}
BFN_HOT void vfieldset_ic(Val s, const char *k, Val v, BFieldIC *ic) {
    if (s.tag != V_STRUCT) { vrelease1(v); vraise("bad field set"); return; }
    Map *m = s.as.map;
    if (ic) {
        const BShape *sh = m->shape;
        if (sh) {
            bint hit_sl = -1;
            if (sh == ic->shapes[0]) hit_sl = ic->slots[0];
            else if (sh == ic->shapes[1]) hit_sl = ic->slots[1];
            else if (sh == ic->shapes[2]) hit_sl = ic->slots[2];
            else if (sh == ic->shapes[3]) hit_sl = ic->slots[3];
            if (hit_sl >= 0) {
                BRAT_CTR_LOCK_OBJ(m);
                Val old = m->vals[hit_sl];
                m->vals[hit_sl] = v;
                BRAT_CTR_UNLOCK_OBJ(m);
                vrelease1(old);
                return;
            }
        } else {
            bint sl = ic->slots[0];
            if (!ic->shapes[0] && sl >= 0 && sl < m->used) {
                Val kv = m->keys[sl];
                if (kv.tag == V_STR) {
                    const char *ks = vstr_c(&kv);
                    if (ks == k || (ks[0] == k[0] && strcmp(ks, k) == 0)) {
                        BRAT_CTR_LOCK_OBJ(m);
                        Val old = m->vals[sl];
                        m->vals[sl] = v;
                        BRAT_CTR_UNLOCK_OBJ(m);
                        vrelease1(old);
                        return;
                    }
                }
            }
        }
    }
    bint at = map_find_cstr(m, k, strlen(k));
    if (at < 0) { vrelease1(v); vraise("bad field set"); return; }
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
    BRAT_CTR_LOCK_OBJ(m);
    Val old = m->vals[at];
    m->vals[at] = v;
    BRAT_CTR_UNLOCK_OBJ(m);
    vrelease1(old);
}
BFN Val vcall_any(Val fn, Vec *a);
BFN Val vfold(Val obj, Val init, Val fn) {
    Vec *w = vvec_of_m(obj, "fold");
    if (verr_pending()) { vrc_dec(init); vrc_dec(fn); return vunwind; }
    Val acc = init;
    Val ab[2];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 2);
    sv.vec.len = 2;
    for (bint i = 0; i < w->len; i++) {
        ab[0] = acc;
        ab[1] = w->items[i];
        Val next = vcall_any(fn, &sv.vec);
        if (verr_pending()) {
            vrc_dec(acc);
            vrc_dec(fn);
            return vunwind;
        }
        vrc_dec(acc);
        acc = next;
    }
    vrc_dec(fn);
    return acc;
}
BFN Val vpred(Val obj, Val fn, const char *meth) {
    Vec *w = vvec_of_m(obj, meth);
    if (verr_pending()) { vrc_dec(fn); return vunwind; }
    Val ab[1];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 1);
    sv.vec.len = 1;
    for (bint i = 0; i < w->len; i++) {
        ab[0] = w->items[i];
        Val r = vcall_any(fn, &sv.vec);
        if (verr_pending()) {
            vrc_dec(fn);
            return vunwind;
        }
        int ok = vis_true(r);
        vrc_dec(r);
        if (meth[0] == 'a' && meth[1] == 'n' && ok) {
            vrc_dec(fn); return vbool(1);
        }
        if (meth[0] == 'a' && meth[1] == 'l' && !ok) {
            vrc_dec(fn); return vbool(0);
        }
        if (meth[0] == 'f' && ok) {
            Val found = w->items[i];
            vrc_dec(fn); return found;
        }
    }
    vrc_dec(fn);
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
            /* Run the defer even while an error is unwinding, then restore it
             * unless the defer itself raised an uncaught error. */
            int sp = _verr_pending, se = _verr_exc;
            bint sline = _verr_line;
            Val sv_unw = sp ? vunwind : vnil();
            Val ab[1];
            BStackVec sv;
            VSTACK_VEC_INIT(sv, ab, 1);
            _verr_pending = 0;
            _verr_exc = 0;
            vunwind = vnil();
            Val r = vcall_any(f->items[i], &sv.vec);
            vrc_dec(r);
            if (_verr_pending) {
                if (sp) vrelease(sv_unw);
                break;
            }
            _verr_pending = sp;
            _verr_exc = se;
            _verr_line = sline;
            vunwind = sv_unw;
        }
        /* The frame and its closures belong to this call: a function with a
         * defer runs it thousands of times and never owns the vector again.
         * Releasing the vector releases the closures it holds. */
        vdrop(vvecv(f));
    }
}
typedef struct { Val item; Val key; bint idx; } _VSortEntry;
static int vsortby_q(const void *pa, const void *pb) {
    if (verr_pending()) return 0;
    const _VSortEntry *ea = (const _VSortEntry *)pa;
    const _VSortEntry *eb = (const _VSortEntry *)pb;
    int r = vcmp(ea->key, eb->key);
    if (r != 0) return r;
    return ea->idx < eb->idx ? -1 : (ea->idx > eb->idx ? 1 : 0);
}
BFN Val vsort_by(Val v, Val fn) {
    Vec *w = vvec_of_m(v, "sort_by");
    if (verr_pending()) { vrc_dec(fn); return vunwind; }
    bint n = w->len;
    Val ab[1];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 1);
    Vec *one = &sv.vec;
    one->len = 1;
    if (n <= 1) {
        if (n == 1) {
            ab[0] = w->items[0];
            Val k0 = vcall_any(fn, one);
            if (verr_pending()) { vrc_dec(fn); return vunwind; }
            vrc_dec(k0);
        }
        vrc_dec(fn);
        return v;
    }
    _VSortEntry *ents = (_VSortEntry *)malloc((size_t)n * sizeof(_VSortEntry));
    if (!ents) vfatal_oom();
    for (bint i = 0; i < n; i++) {
        ab[0] = w->items[i];
        Val k = vcall_any(fn, one);
        if (verr_pending()) {
            for (bint j = 0; j < i; j++) vrc_dec(ents[j].key);
            free(ents);
            vrc_dec(fn);
            return vunwind;
        }
        ents[i].item = w->items[i];
        ents[i].key = k;
        ents[i].idx = i;
    }
    vrc_dec(fn);
    qsort(ents, (size_t)n, sizeof(_VSortEntry), vsortby_q);
    if (verr_pending()) {
        for (bint i = 0; i < n; i++) vrc_dec(ents[i].key);
        free(ents);
        return vunwind;
    }
    for (bint i = 0; i < n; i++) {
        w->items[i] = ents[i].item;
        vrc_dec(ents[i].key);
    }
    free(ents);
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
typedef struct {
    Val *buf;
    bint cap, len, head, used, closed, inited, waiters;
    bint slot, free_next;
    uint32_t gen;
#if defined(_WIN32)
    SRWLOCK lk;
    CONDITION_VARIABLE rcv, snd;
#else
    pthread_mutex_t lk;
    pthread_cond_t rcv, snd;
#endif
} VChan;
static VChan **_vch;
static bint _vch_cap;
static bint _vch_len = 1;
static bint _vch_free;

#if defined(_WIN32)
static SRWLOCK _vch_tab = SRWLOCK_INIT;
static void _vch_tab_lock(void) { AcquireSRWLockExclusive(&_vch_tab); }
static void _vch_tab_unlock(void) { ReleaseSRWLockExclusive(&_vch_tab); }
static void _vch_init(VChan *c) {
    InitializeSRWLock(&c->lk);
    InitializeConditionVariable(&c->rcv);
    InitializeConditionVariable(&c->snd);
}
static void _vch_lk(VChan *c) { AcquireSRWLockExclusive(&c->lk); }
static void _vch_ul(VChan *c) { ReleaseSRWLockExclusive(&c->lk); }
static void _vch_wait_rcv(VChan *c) { SleepConditionVariableSRW(&c->rcv, &c->lk, INFINITE, 0); }
static void _vch_wait_snd(VChan *c) { SleepConditionVariableSRW(&c->snd, &c->lk, INFINITE, 0); }
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

static VChan *_vch_acquire(Val h) {
    if (h.tag != V_INT || h.as.ival <= 0) return NULL;
    bint i = (bint)(uint32_t)h.as.ival;
    uint32_t g = (uint32_t)((uint64_t)h.as.ival >> 32);
    if (i < 1 || g == 0) return NULL;
    _vch_tab_lock();
    VChan *c = NULL;
    if (i < _vch_len) {
        c = _vch[i];
        if (!c || !c->used || c->gen != g) c = NULL;
    }
    if (c) c->waiters++;
    _vch_tab_unlock();
    return c;
}

#ifndef BRAT_MAX_CHAN_CAP
#define BRAT_MAX_CHAN_CAP 1048576
#endif
#ifndef BRAT_MAX_TOTAL_CHAN_SLOTS
#define BRAT_MAX_TOTAL_CHAN_SLOTS 4194304
#endif
static bint _vch_total_slots = 0;

/* A closed slot goes back to the table once it holds nothing and has no active callers. */
static void _vch_put(VChan *c) {
    _vch_tab_lock();
    c->waiters--;
    if (c->waiters == 0 && c->used && c->closed && c->len == 0) {
        if (_vch_total_slots >= c->cap) _vch_total_slots -= c->cap;
        else _vch_total_slots = 0;
        c->cap = 0;
        c->used = 0;
        free(c->buf);
        c->buf = NULL;
        c->free_next = _vch_free;
        _vch_free = c->slot;
    }
    _vch_tab_unlock();
}

BFN Val vchan_new(Val cap) {
    if (cap.tag != V_INT) return _verr_c("chan: capacity must be an integer");
    bint n = cap.as.ival;
    if (n < 1) return _verr_c("chan: capacity must be at least 1");
    if (n > BRAT_MAX_CHAN_CAP) return _verr_c("chan: capacity too large");
    _vch_tab_lock();
    if (n > BRAT_MAX_TOTAL_CHAN_SLOTS - _vch_total_slots) {
        _vch_tab_unlock();
        return _verr_c("chan: total channel capacity budget exceeded");
    }
    bint i = 0;
    VChan *c = NULL;
    if (_vch_free) {
        i = _vch_free;
        c = _vch[i];
        _vch_free = c->free_next;
        c->free_next = 0;
    } else {
        if (_vch_len >= _vch_cap) {
            bint nc = _vch_cap ? _vch_cap * 2 : 64;
            if (nc < _vch_cap || (uint64_t)nc > (uint64_t)SIZE_MAX / sizeof(VChan *)) {
                _vch_tab_unlock();
                vfatal_oom();
            }
            VChan **nt = (VChan **)realloc(_vch, (size_t)nc * sizeof(VChan *));
            if (!nt) { _vch_tab_unlock(); vfatal_oom(); }
            for (bint j = _vch_cap; j < nc; j++) nt[j] = NULL;
            _vch = nt;
            _vch_cap = nc;
        }
        i = _vch_len++;
        c = (VChan *)calloc(1, sizeof(VChan));
        if (!c) { _vch_tab_unlock(); vfatal_oom(); }
        c->slot = i;
        _vch[i] = c;
        _vch_init(c);
        c->inited = 1;
    }
    c->buf = (Val *)calloc((size_t)n, sizeof(Val));
    if (!c->buf) { _vch_tab_unlock(); vfatal_oom(); }
    c->gen = (c->gen >= 0x7fffffffU) ? 1U : (c->gen + 1U);
    c->cap = n; c->len = 0; c->head = 0; c->closed = 0;
    c->used = 1;
    _vch_total_slots += n;
    bint handle = ((bint)c->gen << 32) | (bint)(uint32_t)i;
    _vch_tab_unlock();
    return vint(handle);
}

BFN Val vchan_send(Val h, Val v) {
    VChan *c = _vch_acquire(h);
    if (!c) return _verr_c("send on closed channel");
    _vch_lk(c);
    while (c->len == c->cap && !c->closed) _vch_wait_snd(c);
    if (c->closed) { _vch_ul(c); _vch_put(c); return _verr_c("send on closed channel"); }
    Val stored = barena_depth ? vown(v) : vrc_inc(v);
    if (barena_depth && _verr_pending) { _vch_ul(c); _vch_put(c); return vunwind; }
    c->buf[(c->head + c->len) % c->cap] = stored;
    c->len++;
    _vch_wake_rcv(c);
    _vch_ul(c);
    _vch_put(c);
    return vnil();
}

BFN Val vchan_recv(Val h) {
    VChan *c = _vch_acquire(h);
    if (!c) return vnil();  /* closed and drained, or never a channel */
    _vch_lk(c);
    while (c->len == 0 && !c->closed) _vch_wait_rcv(c);
    if (c->len == 0) {
        _vch_ul(c);
        _vch_put(c);
        return vnil();
    }
    Val v = c->buf[c->head];
    c->head = (c->head + 1) % c->cap;
    c->len--;
    _vch_wake_snd(c);
    _vch_ul(c);
    _vch_put(c);
    return v;
}

BFN Val vchan_close(Val h) {
    VChan *c = _vch_acquire(h);
    if (!c) return vnil();  /* closing twice is not an error */
    _vch_lk(c);
    c->closed = 1;
    _vch_wake_all(c);
    _vch_ul(c);
    _vch_put(c);
    return vnil();
}

BFN Val vbuiltin(Val obj, const char *meth, Vec *a) {
    if (obj.tag == V_STR) {
        if (!strcmp(meth, "split")) { if (VARG(a, 0).tag == V_STR && vstr_empty(VARG(a, 0))) { vraise("empty separator"); return vunwind; } return vsplit(obj, VARG(a, 0)); }
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
        if (!strcmp(meth, "copy")) return vrc_inc(obj);
        if (!strcmp(meth, "chars")) return vchars(obj);
        if (!strcmp(meth, "parse_float")) return vparse_float(obj);
        if (!strcmp(meth, "slice")) return vstr_slice(obj, VARG(a, 0), VARG(a, 1));
    }
    if (obj.tag == V_VEC) {
        if (!strcmp(meth, "join")) return vjoin(obj, VARG(a, 0));
        if (!strcmp(meth, "push")) return vpush(obj, vrc_inc(VARG(a, 0)));
        if (!strcmp(meth, "pop")) return vpop(obj);
        if (!strcmp(meth, "first")) return vfirst(obj);
        if (!strcmp(meth, "last")) return vlast(obj);
        if (!strcmp(meth, "sort")) return vsort(obj);
        if (!strcmp(meth, "rev")) return vrev(obj);
        if (!strcmp(meth, "ins")) return vins(obj, VARG(a, 0), vrc_inc(VARG(a, 1)));
        if (!strcmp(meth, "del")) return vdel_at(obj, VARG(a, 0));
        if (!strcmp(meth, "slice")) return vslice(obj, VARG(a, 0), VARG(a, 1));
        if (!strcmp(meth, "sum")) return vsum(obj);
        if (!strcmp(meth, "min")) return vmin(obj);
        if (!strcmp(meth, "max")) return vmax(obj);
        if (!strcmp(meth, "enumerate")) return venum(obj);
        if (!strcmp(meth, "copy")) return vcopy(obj);
        if (!strcmp(meth, "zip")) return vzip(obj, VARG(a, 0));
        if (!strcmp(meth, "chunks")) return vchunks(obj, VARG(a, 0));
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
        if (!strcmp(meth, "set")) return vmap_setv(obj, vrc_inc(VARG(a, 0)), vrc_inc(VARG(a, 1)));
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
static SRWLOCK btlock = SRWLOCK_INIT;
static void btask_lock(void) { AcquireSRWLockExclusive(&btlock); }
static void btask_unlock(void) { ReleaseSRWLockExclusive(&btlock); }
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

static int bpool_start_locked(void) {
    if (g_pool_n) return 1;
    int n = bpool_ncpu();
    if (n > BPOOL_MAX) n = BPOOL_MAX;
    if (n < 1) n = 1;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&g_pool_n, n, __ATOMIC_RELEASE);
#else
    g_pool_n = n;
#endif
    for (int i = 0; i < n; i++) {
#if defined(_WIN32)
        g_pool[i] = CreateThread(NULL, 0, bpool_worker, NULL, 0, NULL);
        if (!g_pool[i]) { g_pool_n = i; return i > 0; }
#else
        if (pthread_create(&g_pool[i], NULL, bpool_worker, NULL) != 0) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&g_pool_n, i, __ATOMIC_RELEASE);
#else
            g_pool_n = i;
#endif
            return i > 0;
        }
#endif
    }
    return 1;
}

BFN Val verr_own(Val v) {
    if (v.tag != V_TAG || strcmp(v.as.tv->tag, "err")) return v;
    int saved = barena_depth;
    barena_depth = 0;
    Vec *a = vvec();
    for (bint i = 0; i < v.as.tv->len; i++) {
        Val el = vown_copy(v.as.tv->items[i]);
        if (_verr_pending) {
            vrc_dec(vvecv(a));
            vrc_dec(v);
            barena_depth = saved;
            return verr_take();
        }
        vec_push(a, el);
    }
    vrc_dec(v);
    Val out = vtagv("err", a);
    barena_depth = saved;
    return out;
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
    int outer_arena = barena_depth;
    VScope *outer_scope = _vscope_top;
    int outer_pending = _verr_pending;
    int outer_exc = _verr_exc;
    bint outer_line = _verr_line;
    bint outer_trace_line = _btrace_cur_line;
    Val outer_unwind = outer_pending ? vunwind : vnil();
    g_btask_self = 1;
    barena_depth = 0;
    _vscope_top = t->scope;
    _verr_pending = 0;
    _verr_exc = 0;
    _verr_line = 0;
    vunwind = vnil();
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
        vrc_dec(t->result);
        t->err = 1;
        t->result = verr_own(verr_take());
    } else if (t->result.tag == V_TAG && !strcmp(t->result.as.tv->tag, "err")) {
        t->err = 1;
        t->result = verr_own(t->result);
    }
    if (!t->err) {
        if (t->slot) {
            *t->slot = t->result;    /* the caller folds these; the task owns none */
        } else {
            vrc_dec(t->result);
        }
        t->result = vnil();
    }
    vunwind = outer_unwind;
    _verr_pending = outer_pending;
    _verr_exc = outer_exc;
    _verr_line = outer_line;
    _btrace_cur_line = outer_trace_line;
    g_btask_self = outer;
    barena_depth = outer_arena;
    _vscope_top = outer_scope;

    btask_lock();
    g_q_nleft--;
    if (t->scope) {
        if (t->err) {
            if (!t->scope->err) { t->scope->err = 1; t->scope->errv = t->result; t->result = vnil(); }
            else { vrc_dec(t->result); t->result = vnil(); }
        }
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
    if (clo.tag != V_TAG || strcmp(clo.as.tv->tag, "closure") || clo.as.tv->len != 2) {
        vrc_dec(clo);
        vraise("spawn wants closure"); return;
    }

    BTask *t = (BTask *)calloc(1, sizeof(BTask));
    if (!t) { vrc_dec(clo); vraise("spawn failed: out of memory"); return; }
    t->clo = clo;
    t->qnext = 0;
    t->next = 0;
    btask_lock();
    int started = bpool_start_locked();
    btask_unlock();
    if (!started) {
        free(t);
        vrc_dec(clo);
        vraise("spawn failed: could not start worker thread");
        return;
    }
    if (_vscope_top) vscope_attach(_vscope_top, t);
    btask_lock();
    if (g_q_tail) g_q_tail->qnext = t;
    else g_q_head = t;
    g_q_tail = t;
    g_q_nleft++;
    bpool_wake();
    bpool_idle_wake();
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
        if (list->err) {
            if (!err) { err = 1; errv = list->result; }
            else vrc_dec(list->result);
        } else {
            vrc_dec(list->result);
        }
        free(list);
        list = nx;
    }
    if (err) {
        if (_verr_pending) vrc_dec(errv);
        else { vunwind = errv; vunwind_do(); }
    }
}

static void _vscope_drain(VScope *sc) {
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
}

/* `par for`: run `clo(element)` for every element, then fold with `op` in index
 * order. The body is pure — the checker refuses anything else — so the tasks
 * commute and the only order that can be observed is the fold, which is fixed.
 * An error in any task propagates through a dedicated local scope so enclosing
 * `scope { ... }` blocks and sibling `spawn` tasks stay isolated. */
BFN Val vparfor(Val vec, Val clo, Val unit, Val op) {
    Vec *v = vvec_of_m(vec, "par for");
    if (!v || verr_pending()) { vrc_dec(clo); vrc_dec(op); vrc_dec(unit); return vunwind; }
    bint n = v->len;
    if (n == 0) {
        vrc_dec(clo);            /* vparfor consumes both closures */
        vrc_dec(op);
        return unit;
    }
    Val *slots = (Val *)calloc((size_t)n, sizeof(Val));
    if (!slots) { vrc_dec(clo); vrc_dec(op); vrc_dec(unit); vraise("par for: out of memory"); return vunwind; }
    for (bint i = 0; i < n; i++) slots[i] = vnil();
    VScope pf_sc;
    memset(&pf_sc, 0, sizeof pf_sc);
    pf_sc.errv = vnil();
    for (bint i = 0; i < n; i++) {
        BTask *t = (BTask *)calloc(1, sizeof(BTask));
        if (!t) {
            _vscope_drain(&pf_sc);
            BTask *tl = pf_sc.tasks;
            while (tl) { BTask *nx = tl->next; free(tl); tl = nx; }
            if (pf_sc.err) vrc_dec(pf_sc.errv);
            for (bint j = 0; j < n; j++) vrc_dec(slots[j]);
            free(slots);
            vrc_dec(clo); vrc_dec(op); vrc_dec(unit);
            vraise("par for: out of memory");
            return vunwind;
        }
        t->clo = vrc_inc(clo);
        t->arg = vrc_inc(v->items[i]);
        t->has_arg = 1;
        t->slot = &slots[i];
        t->result = vnil();
        vscope_attach(&pf_sc, t);
        btask_lock();
        bpool_start_locked();
        if (g_q_tail) g_q_tail->qnext = t;
        else g_q_head = t;
        g_q_tail = t;
        g_q_nleft++;
        bpool_wake();
        bpool_idle_wake();
        btask_unlock();
    }
    _vscope_drain(&pf_sc);
    BTask *tl = pf_sc.tasks;
    while (tl) { BTask *nx = tl->next; free(tl); tl = nx; }
    if (pf_sc.err) {
        for (bint i = 0; i < n; i++) vrc_dec(slots[i]);
        free(slots);
        vrc_dec(clo);
        vrc_dec(op);
        vrc_dec(unit);
        if (_verr_pending) vrc_dec(pf_sc.errv);
        else { vunwind = pf_sc.errv; vunwind_do(); }
        return vunwind;
    }
    Val acc = unit;
    for (bint i = 0; i < n; i++) {
        Vec *args = vvec();
        vec_push(args, acc);
        vec_push(args, slots[i]);
        Val next_acc = vcall_any(op, args);
        args->len = 0;               /* both items are borrowed, not owned */
        vrc_dec(vvecv(args));
        vrc_dec(acc);
        vrc_dec(slots[i]);
        acc = next_acc;
        if (_verr_pending) {
            vrc_dec(acc);
            for (bint j = i + 1; j < n; j++) vrc_dec(slots[j]);
            free(slots);
            vrc_dec(clo);
            vrc_dec(op);
            return vunwind;
        }
    }
    free(slots);
    vrc_dec(clo);                /* vparfor consumes both closures */
    vrc_dec(op);
    return acc;
}

BFN Val vscope_begin(void) {
    VScope *sc = (VScope *)calloc(1, sizeof(VScope));
    if (!sc) { vraise("scope: out of memory"); return vint(0); }
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
    _vscope_drain(sc);
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
    if (err) {
        if (_verr_pending) vrc_dec(errv);
        else { vunwind = errv; vunwind_do(); }
    }
}

#endif /* BRAT_RT_H */
