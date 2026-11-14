BFN int vmeth_int(Val v, const char *meth, bint *out) {
    if (v.tag == V_INT) { *out = v.as.ival; return 1; }
    vraisef("method `%s`: bad argument", meth);
    return 0;
}
BFN bint vaffix(Val s, Val p, int pre, int suf) {
    const char *mname = pre ? "starts_with" : (suf ? "ends_with" : "contains");
    if (s.tag != V_STR) { vraisef("no method %s", mname); return 0; }
    if (p.tag != V_STR) { vraisef("method `%s`: bad argument", mname); return 0; }
    const char *sc = vstr_c(&s), *pc = vstr_c(&p);
    size_t ls = vstr_len_fast(&s), lp = vstr_len_fast(&p);
    if (pre) return lp <= ls && memcmp(sc, pc, lp) == 0;
    if (suf) return lp <= ls && memcmp(sc + ls - lp, pc, lp) == 0;
    return _bmemmem(sc, ls, pc, lp) != NULL;
}
BFN Val vreplace(Val s, Val a, Val b) {
    if (s.tag != V_STR) { vraise("no method replace"); return vunwind; }
    if (a.tag != V_STR || b.tag != V_STR) { vraise("method `replace`: bad argument"); return vunwind; }
    const char *p = vstr_c(&s), *pat = vstr_c(&a), *rep = vstr_c(&b);
    size_t sl = vstr_len_fast(&s), pl = vstr_len_fast(&a), rl = vstr_len_fast(&b);
    if (!pl) {
        /* Output is (gaps)*rl + sl bytes. Check bounds before allocation. */
        size_t gaps = 1;
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
            if (i + n > sl) n = 1;
            memcpy(w, p + i, n); w += n; i += n;
            memcpy(w, rep, rl); w += rl;
        }
        *w = 0;
        return vstr_buf(o, (bint)olen);
    }
    const char *end = p + sl;
    size_t total = sl + 1;
    char *o = (char *)balloc(total);
    if (!o) vfatal_oom();
    size_t n = 0;
    const char *q;
    while ((q = _bmemmem(p, (size_t)(end - p), pat, pl)) != NULL) {
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
    size_t rest_len = (size_t)(end - p);
    if (rest_len > BRAT_MAX_STR - n) {
        vrc_dec((Val){ .tag = V_STR, .as = { .str = o } });
        vraise("out of memory");
        return vunwind;
    }
    size_t rest = rest_len + 1;
    if (n + rest > total) { total = n + rest; o = (char *)bgrow_rc(o, total); }
    memcpy(o + n, p, rest_len);
    n += rest_len;
    o[n] = 0;
    return vstr_buf(o, (bint)n);
}
BFN Val vindex_of(Val s, Val sub) {
    if (s.tag != V_STR) { vraise("no method index_of"); return vunwind; }
    if (sub.tag != V_STR) { vraise("method `index_of`: bad argument"); return vunwind; }
    const char *sc = vstr_c(&s);
    const char *p = _bmemmem(sc, vstr_len_fast(&s), vstr_c(&sub), vstr_len_fast(&sub));
    if (!p) return vnil();

    bint cps = 0;
    for (const char *q = sc; q < p; ) {
        unsigned cp = 0;
        q += vcp_next(q, &cp);
        cps++;
    }
    return vint(cps);
}
/* Refuse oversized string repeats before allocation. */
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
    if (s.tag != V_STR) { vraise("no method repeat"); return vunwind; }
    bint k = 0;
    if (!vmeth_int(n, "repeat", &k)) return vunwind;
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
    if (s.tag != V_STR) { vraise("no method split_once"); return vunwind; }
    if (sep.tag != V_STR) { vraise("method `split_once`: bad argument"); return vunwind; }
    const char *sc = vstr_c(&s), *sepc = vstr_c(&sep);
    size_t sl = vstr_len_fast(&s), dl = vstr_len_fast(&sep);
    if (!dl) { vraise("empty separator"); return vunwind; }
    const char *p = _bmemmem(sc, sl, sepc, dl);
    if (!p) return vnil();
    Vec *o = vvec();
    vec_push(o, vstrn(sc, (bint)(p - sc)));
    vec_push(o, vstrn(p + dl, (bint)((sc + sl) - (p + dl))));
    return vvecv(o);
}
BFN Val vparse_int(Val s) {
    if (s.tag != V_STR) { vraise("no method parse_int"); return vunwind; }
    const char *p = vstr_c(&s);
    const char *end = p + vstr_len_fast(&s);
    while (p < end && vis_space(*p)) p++;
    const char *q = p;
    if (q < end && (*q == '+' || *q == '-')) q++;
    if (!(q < end && *q >= '0' && *q <= '9')) {
        Vec *a = vvec();
        vec_push(a, vstr("not a number"));
        return vtagv("err", a);
    }
    uint64_t acc = 0;
    int over = 0;
    while (q < end && *q >= '0' && *q <= '9') {
        unsigned d = (unsigned)(*q - '0');
        if (acc > (UINT64_MAX - d) / 10ULL) over = 1;
        else acc = acc * 10ULL + d;
        q++;
    }
    while (q < end && vis_space(*q)) q++;
    Vec *a = vvec();
    if (q != end) { vec_push(a, vstr("not a number")); return vtagv("err", a); }
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
    if (v.tag != V_VEC) { vraise("no method join"); return vunwind; }
    if (sep.tag != V_STR) { vraise("method `join`: bad argument"); return vunwind; }
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
    BMUTEX_LOCK(&_vtagof_mu);
    for (int i = 0; i < _vtagof_n; i++) {
        if (strcmp(_vtagof_cache[i].name, name) == 0) {
            Val hit = _vtagof_cache[i].v;
            BMUTEX_UNLOCK(&_vtagof_mu);
            return hit;
        }
    }
    Val s = vstr(name);
    if (_vtagof_n < 512) {
        if (!s.sso && s.as.str) { ((BHead *)s.as.str - 1)->rc = -1; s.imm = 1; }
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
/* `i64(x)` and `f64(x)`: the same number in the other type. A float loses its
 * fraction toward zero; a value that is not a number raises. */
BFN Val vto_i64(Val v) {
    bint n = vlong(v);
    return verr_pending() ? vunwind : vint(n);
}
BFN Val vto_f64(Val v) {
    if (v.tag == V_FLOAT) return v;
    if (v.tag == V_INT) return vfloat((double)v.as.ival);
    vraise("not a number"); return vunwind;
}
/* `chr(n)` and `ord(s)`: a code point as a one-character string, and back. */
BFN Val vchr(Val v) {
    if (v.tag != V_INT || v.as.ival < 0 || v.as.ival > 0x10FFFF ||
        (v.as.ival >= 0xD800 && v.as.ival <= 0xDFFF)) {
        vraise("not a code point"); return vunwind;
    }
    char b[4];
    return vstrn(b, vcp_put((unsigned)v.as.ival, b));
}
BFN Val vord(Val v) {
    unsigned cp = 0;
    if (v.tag != V_STR || vstr_len_fast(&v) == 0 ||
        (size_t)vcp_next(vstr_c(&v), &cp) != vstr_len_fast(&v)) {
        vraise("ord wants one character"); return vunwind;
    }
    return vint((bint)cp);
}
/* A vec operation that got the wrong receiver raises and returns an empty vec:
   the generated code keeps running until the next error check and would
   dereference NULL there. `meth` names the method for the message, so the
   error diagnostics stay uniform across platforms. */
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
BFN int vis_ok(Val v) {
    return v.tag == V_TAG && !strcmp(v.as.tv->tag, "ok");
}
BFN int vis_err(Val v) {
    return v.tag == V_TAG && !strcmp(v.as.tv->tag, "err");
}
BFN int vis_some(Val v) {
    return v.tag == V_TAG && !strcmp(v.as.tv->tag, "some");
}
BFN int vis_none(Val v) {
    return v.tag == V_NIL || (v.tag == V_TAG && !strcmp(v.as.tv->tag, "none"));
}

/* Closures carry an internal ("closure", (code, env)) tag; format them as `fn`
   rather than exposing the internal representation. */
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
BFN int vopt_res_state(Val v) {
    if (v.tag == V_TAG && (!strcmp(v.as.tv->tag, "ok") || !strcmp(v.as.tv->tag, "some")))
        return 1;
    if (v.tag == V_NIL || (v.tag == V_TAG && (!strcmp(v.as.tv->tag, "err") || !strcmp(v.as.tv->tag, "none"))))
        return 0;
    return -1;
}
BFN int vunwrap_or_state(Val v) {
    int s = vopt_res_state(v);
    if (s < 0) vraisef("unwrap_or of %s (want Ok/Err/Some/None)", vtypename(v));
    return s;
}
BFN int vor_fb(Val v) {
    return v.tag == V_NIL;
}
BFN const char *vname(Val v) {
    if (v.tag != V_STR) { vraise("want str"); return NULL; }
    return vstr_pin(v);
}
BFN Val vptr_of_str(Val v) {
    if (v.tag != V_STR) { vraise("ptr_of_str wants str"); return vunwind; }
    char *p = vstr_pin(v);
    if (verr_pending() || !p) return vunwind;
    return vint((bint)(uintptr_t)p);
}
BFN Val vptr_write_str(Val p, Val s) {
    if (!vptr_live(p)) return vunwind;
    if (s.tag != V_STR) { vraise("ptr_write_str wants str"); return vunwind; }
    char *cs = vstr_pin_ret(s);
    if (verr_pending() || !cs) return vunwind;
    *(const char **)p.as.ival = cs;
    return vint(0);
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
static void _vfmt_ts7(double t, char out[32]) {
    if (!(t >= 0.0) || !isfinite(t)) { memcpy(out, "0.0000000", 10); return; }
    uint64_t ticks = (uint64_t)(t * 10000000.0 + 0.5);
    unsigned long long sec = (unsigned long long)(ticks / 10000000ULL);
    unsigned frac = (unsigned)(ticks % 10000000ULL);
    snprintf(out, 32, "%llu.%07u", sec, frac);
}
BFN void vlogv(const char *lvl, Vec *a) {
    size_t total = 0;
    bint i;
    for (i = 0; i < a->len; i++) {
        if (i) {
            if (total >= (size_t)BRAT_MAX_STR) { vraise("out of memory"); return; }
            total += 1;
        }
        bint sl = vsprint_len(a->items[i]);
        if (verr_pending()) return;
        if (sl < 0 || (size_t)sl > (size_t)BRAT_MAX_STR - total) {
            vraise("out of memory");
            return;
        }
        total += (size_t)sl;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) vfatal_oom();
    size_t n = 0;
    for (i = 0; i < a->len; i++) {
        if (i) buf[n++] = ' ';
        n += (size_t)vsprint(a->items[i], buf + n);
    }
    buf[n] = 0;
    char tsbuf[32];
    _vfmt_ts7(vtime(), tsbuf);
    BRAT_PRINT_LOCK();
    printf("{\"ts\": %s, \"lvl\": \"%s\", \"msg\": \"", tsbuf, lvl);
    for (size_t j = 0; j < n; j++) {
        unsigned char ch = (unsigned char)buf[j];
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch == '\n') printf("\\n");
        else if (ch == '\r') printf("\\r");
        else if (ch == '\t') printf("\\t");
        else if (ch < 0x20) printf("\\u%04x", ch);
        else putchar(ch);
    }
    printf("\"}\n");
    fflush(stdout);
    BRAT_PRINT_UNLOCK();
    free(buf);
}
BFN Val vpush(Val v, Val x) {
    Vec *w = vvec_of_m(v, "push");
    if (verr_pending()) { vrc_dec(x); return vunwind; }
    return vint(vec_push_len(w, x));
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
    Val r = vrc_inc(w->items[0]);
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
BFN Val vlast(Val v) {
    Vec *w = vvec_of_m(v, "last");
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (!w->len) { BRAT_CTR_UNLOCK_OBJ(w); return vnil(); }
    Val r = vrc_inc(w->items[w->len - 1]);
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
    BRAT_CTR_LOCK_OBJ(w);
    if (vmixed(w)) { BRAT_CTR_UNLOCK_OBJ(w); vraise("type error in <"); return vunwind; }
    if (w->len > 1) {
        qsort(w->items, (size_t)w->len, sizeof(Val), vcmp_q);
        if (verr_pending()) { BRAT_CTR_UNLOCK_OBJ(w); return vunwind; }
    }
    BRAT_CTR_UNLOCK_OBJ(w);
    return v;
}
BFN Val vrev(Val v) {
    Vec *w = vvec_of_m(v, "rev");
    if (verr_pending()) return vunwind;
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(w);
    for (bint i = w->len - 1; i >= 0; i--) vec_push_raw(o, vrc_inc(w->items[i]));
    BRAT_CTR_UNLOCK_OBJ(w);
    return vvecv(o);
}
BFN Val vins(Val v, Val i, Val x) {
    Vec *w = vvec_of_m(v, "ins");
    if (verr_pending()) { vrc_dec(x); return vunwind; }
    bint k = 0;
    if (!vmeth_int(i, "ins", &k)) { vrc_dec(x); return vunwind; }
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
    bint k = 0;
    if (!vmeth_int(i, "del", &k)) return vunwind;
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
    bint x = 0, y = 0;
    if (!vmeth_int(a, "slice", &x)) return vunwind;
    if (!vmeth_int(b, "slice", &y)) return vunwind;
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(w);
    bint n = w->len;
    if (x < 0) x = (x < -n) ? 0 : x + n;
    if (x < 0) x = 0;
    if (x > n) x = n;
    if (y < 0) y = (y < -n) ? 0 : y + n;
    if (y < 0) y = 0;
    if (y > n) y = n;
    for (bint i = x; i < y; i++) vec_push_raw(o, vrc_inc(w->items[i]));
    BRAT_CTR_UNLOCK_OBJ(w);
    return vvecv(o);
}
BFN Val vsum(Val v) {
    Vec *w = vvec_of_m(v, "sum");
    if (verr_pending()) return vunwind;
    uint64_t acc = 0;
    double dl = 0, di = 0;
    int isf = 0;
    BRAT_CTR_LOCK_OBJ(w);
    for (bint i = 0; i < w->len; i++) {
        Val x = w->items[i];
        if (x.tag == V_INT || x.tag == V_BOOL) { acc += (uint64_t)x.as.ival; di += (double)x.as.ival; }
        else if (x.tag == V_FLOAT) { dl += x.as.fval; isf = 1; }
        else { BRAT_CTR_UNLOCK_OBJ(w); vraise("sum of non-numeric"); return vunwind; }
    }
    BRAT_CTR_UNLOCK_OBJ(w);
    /* Next to a float the integers add up as floats: their 64-bit sum may have wrapped. */
    if (isf) return vfloat(dl + di);
    return vint((bint)acc);
}
static inline Val _vextremum(Val v, const char *mname, int dir) {
    Vec *w = vvec_of_m(v, mname);
    if (verr_pending()) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    if (vmixed(w)) { BRAT_CTR_UNLOCK_OBJ(w); vraise("type error in <"); return vunwind; }
    if (!w->len) { BRAT_CTR_UNLOCK_OBJ(w); return vnil(); }
    Val r = w->items[0];
    for (bint i = 1; i < w->len; i++) {
        int c = vcmp(w->items[i], r);
        if (verr_pending()) { BRAT_CTR_UNLOCK_OBJ(w); return vunwind; }
        if (dir < 0 ? (c < 0) : (c > 0)) r = w->items[i];
    }
    r = vrc_inc(r);
    BRAT_CTR_UNLOCK_OBJ(w);
    return r;
}
BFN Val vmin(Val v) { return _vextremum(v, "min", -1); }
BFN Val vmax(Val v) { return _vextremum(v, "max", 1); }
BFN Val venum(Val v) {
    Vec *w = vvec_of_m(v, "enumerate");
    if (verr_pending()) return vunwind;
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(w);
    for (bint i = 0; i < w->len; i++) {
        Vec *p = vvec();
        vec_push_raw(p, vint(i));
        vec_push_raw(p, vrc_inc(w->items[i]));
        vec_push_raw(o, vvecv(p));
    }
    BRAT_CTR_UNLOCK_OBJ(w);
    return vvecv(o);
}
BFN Val vmap_get_or(Val m, Val k, Val d) {
    if (m.tag != V_STRUCT) { vraise("get_or of non-map"); return vunwind; }
    Val ke;
    if (!_vmap_key_guard(k, &ke)) return ke;
    BRAT_CTR_LOCK_OBJ(m.as.map);
    Val *o = map_get_v(m.as.map, k);
    Val r = o ? vrc_inc(*o) : vrc_inc(d);
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return r;
}
BFN_HOT Val vmap_get_or_ic(Val m, const char *k, Val d, BFieldIC *ic) {
    if (m.tag != V_STRUCT) { vraise("get_or of non-map"); return vunwind; }
    BRAT_CTR_LOCK_OBJ(m.as.map);
    Val *o = map_get_ic(m.as.map, k, ic);
    Val r = o ? vrc_inc(*o) : vrc_inc(d);
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return r;
}
BFN Val vmap_items(Val m) {
    if (m.tag != V_STRUCT) { vraise("items of non-map"); return vunwind; }
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(m.as.map);
    for (bint i = 0; i < m.as.map->used; i++) {
        if (m.as.map->keys[i].tag == V_MOVED) continue;
        Vec *p = vvec();
        vec_push_raw(p, vrc_inc(m.as.map->keys[i]));
        vec_push_raw(p, vrc_inc(m.as.map->vals[i]));
        vec_push_raw(o, vvecv(p));
    }
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return vvecv(o);
}
BFN Val vmap_vals(Val m) {
    if (m.tag != V_STRUCT) { vraise("vals of non-map"); return vunwind; }
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(m.as.map);
    for (bint i = 0; i < m.as.map->used; i++)
        if (m.as.map->keys[i].tag != V_MOVED) vec_push_raw(o, vrc_inc(m.as.map->vals[i]));
    BRAT_CTR_UNLOCK_OBJ(m.as.map);
    return vvecv(o);
}
BFN Val vcopy(Val v) {
    if (v.tag == V_VEC) {
        Vec *o = vvec();
        BRAT_CTR_LOCK_OBJ(v.as.vec);
        for (bint i = 0; i < v.as.vec->len; i++) vec_push_raw(o, vrc_inc(v.as.vec->items[i]));
        BRAT_CTR_UNLOCK_OBJ(v.as.vec);
        return vvecv(o);
    }
    if (v.tag == V_STRUCT) {
        Map *src = v.as.map;
        Map *m = vmap(src->type);
        Vec *snap = vvec();
        BRAT_CTR_LOCK_OBJ(src);
        for (bint i = 0; i < src->used; i++) {
            if (src->keys[i].tag != V_MOVED) {
                vec_push_raw(snap, vrc_inc(src->keys[i]));
                vec_push_raw(snap, vrc_inc(src->vals[i]));
            }
        }
        BRAT_CTR_UNLOCK_OBJ(src);
        for (bint i = 0; i + 1 < snap->len; i += 2)
            map_set_v(m, snap->items[i], snap->items[i + 1]);
        snap->len = 0;
        vrc_dec(vvecv(snap));
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
    bint x = 0, y = 0;
    if (!vmeth_int(a, "slice", &x)) return vunwind;
    if (!vmeth_int(b, "slice", &y)) return vunwind;
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
    const char *p = vstr_c(&s);
    const char *end = p + vstr_len_fast(&s);
    while (p < end && vis_space(*p)) p++;
    size_t rem = (size_t)(end - p);
    char sbuf[128];
    char *tmp = (rem < sizeof sbuf) ? sbuf : (char *)malloc(rem + 1);
    if (!tmp) vfatal_oom();
    memcpy(tmp, p, rem);
    tmp[rem] = 0;
    size_t n = (memchr(tmp, 0, rem) == NULL) ? vfloat_prefix(tmp) : 0;
    const char *tail = p + n;
    while (tail < end && vis_space(*tail)) tail++;
    Vec *a = vvec();
    if (n == 0 || tail != end) {
        if (tmp != sbuf) free(tmp);
        vec_push(a, vstr("not a float"));
        return vtagv("err", a);
    }
    int ok = 0;
    double v = _bstrtod_c(tmp, n, &ok);
    if (tmp != sbuf) free(tmp);
    if (!ok) { vec_push(a, vstr("not a float")); return vtagv("err", a); }
    vec_push(a, vfloat(v));
    return vtagv("ok", a);
}
BFN Val vzip(Val a, Val b) {
    Vec *x = vvec_of_m(a, "zip");
    if (verr_pending()) return vunwind;
    if (b.tag != V_VEC) { vraise("method `zip`: bad argument"); return vunwind; }
    Vec *y = b.as.vec;
    Vec *o = vvec();
    unsigned sx = BSTRIPE_IDX(x), sy = BSTRIPE_IDX(y);
    if (_BRAT_MT()) {
        if (sx == sy) {
            BMUTEX_LOCK(&_brat_stripes[sx].mu);
        } else if (sx < sy) {
            BMUTEX_LOCK(&_brat_stripes[sx].mu);
            BMUTEX_LOCK(&_brat_stripes[sy].mu);
        } else {
            BMUTEX_LOCK(&_brat_stripes[sy].mu);
            BMUTEX_LOCK(&_brat_stripes[sx].mu);
        }
    }
    bint n = x->len < y->len ? x->len : y->len;
    for (bint i = 0; i < n; i++) {
        Vec *p = vvec();
        vec_push_raw(p, vrc_inc(x->items[i]));
        vec_push_raw(p, vrc_inc(y->items[i]));
        vec_push_raw(o, vvecv(p));
    }
    if (_BRAT_MT()) {
        BMUTEX_UNLOCK(&_brat_stripes[sx].mu);
        if (sx != sy) BMUTEX_UNLOCK(&_brat_stripes[sy].mu);
    }
    return vvecv(o);
}
BFN Val vchunks(Val v, Val n) {
    Vec *w = vvec_of_m(v, "chunks");
    if (verr_pending()) return vunwind;
    bint k = 0;
    if (!vmeth_int(n, "chunks", &k)) return vunwind;
    if (k <= 0) { vraise("chunks wants n > 0"); return vunwind; }
    Vec *o = vvec();
    BRAT_CTR_LOCK_OBJ(w);
    for (bint i = 0; i < w->len; ) {
        bint end = (w->len - i > k) ? (i + k) : w->len;
        Vec *p = vvec();
        for (bint j = i; j < end; j++) vec_push_raw(p, vrc_inc(w->items[j]));
        vec_push_raw(o, vvecv(p));
        i = end;
    }
    BRAT_CTR_UNLOCK_OBJ(w);
    return vvecv(o);
}

BFN void vjprint_d(Val v, int d) {
    if (d > BRAT_MAX_VALUE_DEPTH) { printf("\"...\""); return; }
    bint i;
    switch (v.tag) {
    case V_NIL: printf("null"); break;
    case V_BOOL: printf(v.as.b ? "true" : "false"); break;
    case V_INT: printf(BINT_FMT, (bint)v.as.ival); break;
    case V_FLOAT: printf("%s", isfinite(v.as.fval) ? vfmt_double(v.as.fval) : "null"); break;
    case V_STR: {
        const char *p = vstr_c(&v);
        size_t slen = vstr_len_fast(&v);
        printf("\"");
        for (size_t j = 0; j < slen; j++) {
            unsigned char ch = (unsigned char)p[j];
            if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
            else if (ch == '\n') printf("\\n");
            else if (ch == '\r') printf("\\r");
            else if (ch == '\t') printf("\\t");
            else if (ch < 0x20) printf("\\u%04x", ch);
            else putchar(ch);
        }
        printf("\"");
        break;
    }
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
    char tsbuf[32];
    _vfmt_ts7(vtime(), tsbuf);
    printf("{\"ts\": %s, \"span\": [", tsbuf);
    vspan_items(v);
    printf("]}\n");
    fflush(stdout);
}

BFN_HOT void vfieldset_ic(Val s, const char *k, Val v, BFieldIC *ic);
BFN_HOT void vstruct_slot_set(Val s, const BShape *shp, bint slot, const char *k, Val v) {
    if (BRAT_LIKELY(s.tag == V_STRUCT && s.as.map->shape == shp)) {
        Map *m = s.as.map;
        BRAT_CTR_LOCK_OBJ(m);
        Val old = m->vals[slot];
        m->vals[slot] = v;
        BRAT_CTR_UNLOCK_OBJ(m);
        vrelease1(old);
        return;
    }
    vfieldset_ic(s, k, v, NULL);
}
BFN_HOT void vfieldset_ic(Val s, const char *k, Val v, BFieldIC *ic) {
    if (s.tag != V_STRUCT) { vrelease1(v); vraise("bad field set"); return; }
    Map *m = s.as.map;
    size_t kl = 0;
    int have_kl = 0;
    BRAT_CTR_LOCK_OBJ(m);
    if (ic) {
        bint sl = -1;
        const BShape *sh = m->shape;
        if (sh) {
            if (_bic_match_shape(ic, sh, &sl) && sl >= 0) {
                Val old = m->vals[sl];
                m->vals[sl] = v;
                BRAT_CTR_UNLOCK_OBJ(m);
                vrelease1(old);
                return;
            }
        } else if (_bic_match_unshaped(ic, m, k, &kl, &have_kl, &sl)) {
            Val old = m->vals[sl];
            m->vals[sl] = v;
            BRAT_CTR_UNLOCK_OBJ(m);
            vrelease1(old);
            return;
        }
    }
    bint at = map_find_cstr(m, k, have_kl ? kl : strlen(k));
    if (at < 0) {
        BRAT_CTR_UNLOCK_OBJ(m);
        vrelease1(v);
        vraise("bad field set");
        return;
    }
    _bic_record(ic, m->shape, at);
    Val old = m->vals[at];
    m->vals[at] = v;
    BRAT_CTR_UNLOCK_OBJ(m);
    vrelease1(old);
}
BFN Val vcall_any(Val fn, Vec *a);
BFN Val vfold(Val obj, Val init, Val fn) {
    Vec *w = vvec_of_m(obj, "fold");
    if (verr_pending()) { vrc_dec(init); vrc_dec(fn); return vunwind; }
    vrc_inc(obj);
    Val acc = init;
    Val ab[2];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 2);
    sv.vec.len = 2;
    for (bint i = 0; i < w->len; i++) {
        Val item = vrc_inc(w->items[i]);
        ab[0] = acc;
        ab[1] = item;
        Val next = vcall_any(fn, &sv.vec);
        vrc_dec(item);
        if (verr_pending()) {
            vrc_dec(acc);
            vrc_dec(fn);
            vrc_dec(obj);
            return vunwind;
        }
        vrc_dec(acc);
        acc = next;
    }
    vrc_dec(fn);
    vrc_dec(obj);
    return acc;
}
BFN Val vpred(Val obj, Val fn, const char *meth) {
    Vec *w = vvec_of_m(obj, meth);
    if (verr_pending()) { vrc_dec(fn); return vunwind; }
    vrc_inc(obj);
    Val ab[1];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 1);
    sv.vec.len = 1;
    for (bint i = 0; i < w->len; i++) {
        Val item = vrc_inc(w->items[i]);
        ab[0] = item;
        Val r = vcall_any(fn, &sv.vec);
        if (verr_pending()) {
            vrc_dec(item);
            vrc_dec(fn);
            vrc_dec(obj);
            return vunwind;
        }
        int ok = vis_true(r);
        vrc_dec(r);
        if (meth[0] == 'a' && meth[1] == 'n' && ok) {
            vrc_dec(item);
            vrc_dec(fn);
            vrc_dec(obj);
            return vbool(1);
        }
        if (meth[0] == 'a' && meth[1] == 'l' && !ok) {
            vrc_dec(item);
            vrc_dec(fn);
            vrc_dec(obj);
            return vbool(0);
        }
        if (meth[0] == 'f' && ok) {
            vrc_dec(fn);
            vrc_dec(obj);
            return item;
        }
        vrc_dec(item);
    }
    vrc_dec(fn);
    vrc_dec(obj);
    if (meth[0] == 'f') return vnil();
    return vbool(meth[1] == 'l' ? 1 : 0);
}
BFN Val vpos(Val v, Val x) {
    if (verr_pending()) return vunwind;
    Vec *w = vvec_of_m(v, "pos");
    if (verr_pending() || !w) return vunwind;
    BRAT_CTR_LOCK_OBJ(w);
    for (bint i = 0; i < w->len; i++) {
        int eq = veq(w->items[i], x);
        if (verr_pending()) { BRAT_CTR_UNLOCK_OBJ(w); return vunwind; }
        if (eq) { BRAT_CTR_UNLOCK_OBJ(w); return vint(i); }
    }
    BRAT_CTR_UNLOCK_OBJ(w);
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
BFN void _bthread_tls_drain(void) {
    if (vdfsp == 0 && vdfr) {
        free(vdfr);
        vdfr = NULL;
        vdfcap = 0;
    }
    for (int i = 0; i < VTXT_SLOTS; i++) {
        free(_vtxt_keep[i]);
        _vtxt_keep[i] = NULL;
    }
#ifndef BRAT_ASAN
    while (barena_pool) {
        BChunk *nx = barena_pool->prev;
        free(barena_pool);
        barena_pool = nx;
    }
    barena_pool_chunks = 0;
    barena_pool_bytes = 0;
#endif
}
#if (defined(__GNUC__) || defined(__clang__)) && !defined(BRAT_RT_SHARED)
__attribute__((destructor)) static void _bmain_tls_dtor(void) { _bthread_tls_drain(); }
#endif
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
    if (verr_pending()) { vrc_dec(v); vrc_dec(fn); return vunwind; }
    bint n = w->len;
    Val ab[1];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 1);
    Vec *one = &sv.vec;
    one->len = 1;
    if (n <= 1) {
        if (n == 1) {
            Val only = vrc_inc(w->items[0]);
            ab[0] = only;
            Val k0 = vcall_any(fn, one);
            vrc_dec(only);
            if (verr_pending()) { vrc_dec(v); vrc_dec(fn); return vunwind; }
            vrc_dec(k0);
        }
        vrc_dec(fn);
        return v;
    }
    _VSortEntry *ents = (_VSortEntry *)malloc((size_t)n * sizeof(_VSortEntry));
    if (!ents) vfatal_oom();
    for (bint i = 0; i < n; i++) {
        ents[i].item = vrc_inc(w->items[i]);
        ents[i].key = vnil();
        ents[i].idx = i;
    }
    bint keyed = 0;
    for (; keyed < n; keyed++) {
        ab[0] = ents[keyed].item;
        ents[keyed].key = vcall_any(fn, one);
        if (verr_pending()) break;
    }
    vrc_dec(fn);
    if (verr_pending()) {
        for (bint i = 0; i < n; i++) { vrc_dec(ents[i].item); vrc_dec(ents[i].key); }
        free(ents);
        vrc_dec(v);
        return vunwind;
    }
    qsort(ents, (size_t)n, sizeof(_VSortEntry), vsortby_q);
    if (verr_pending()) {
        for (bint i = 0; i < n; i++) { vrc_dec(ents[i].item); vrc_dec(ents[i].key); }
        free(ents);
        vrc_dec(v);
        return vunwind;
    }
    if (w->len != n) {
        for (bint i = 0; i < n; i++) { vrc_dec(ents[i].item); vrc_dec(ents[i].key); }
        free(ents);
        vrc_dec(v);
        vraise("sort_by: container changed while keys were computed");
        return vunwind;
    }
    for (bint i = 0; i < n; i++) {
        vrc_dec(w->items[i]);
        w->items[i] = ents[i].item;
        vrc_dec(ents[i].key);
    }
    free(ents);
    return v;
}
#define VARG(a, i) ((a)->len > (i) ? (a)->items[(i)] : vnil())
#if (BRAT_CRT_MASK & BRAT_CRT_CONC)
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
#endif /* BRAT_CRT_CONC (channels) */

static int _vcheck_meth_arity(const char *meth, bint got) {
    static const struct { const char *m; signed char lo, hi; } tab[] = {
        {"split", 0, 1}, {"lines", 0, 0}, {"trim", 0, 0}, {"lower", 0, 0},
        {"upper", 0, 0}, {"starts_with", 1, 1}, {"ends_with", 1, 1},
        {"contains", 1, 1}, {"parse_int", 0, 0}, {"replace", 2, 2},
        {"index_of", 1, 1}, {"repeat", 1, 1}, {"split_once", 1, 1},
        {"parse_float", 0, 0}, {"slice", 2, 2}, {"join", 1, 1},
        {"push", 1, 1}, {"pop", 0, 0}, {"first", 0, 0}, {"last", 0, 0},
        {"sort", 0, 0}, {"rev", 0, 0}, {"ins", 2, 2}, {"del", 1, 1},
        {"sum", 0, 0}, {"min", 0, 0}, {"max", 0, 0}, {"enumerate", 0, 0},
        {"zip", 1, 1}, {"chunks", 1, 1}, {"pos", 1, 1}, {"get", 1, 1},
        {"has", 1, 1}, {"keys", 0, 0}, {"set", 2, 2}, {"items", 0, 0},
        {"vals", 0, 0}, {"get_or", 2, 2}, {"copy", 0, 0}, {"chars", 0, 0},
        {"unwrap", 0, 0}, {"unwrap_or", 1, 1}, {"is_ok", 0, 0}, {"is_err", 0, 0},
        {"is_some", 0, 0}, {"is_none", 0, 0},
        {"send", 1, 1}, {"recv", 0, 0}, {"close", 0, 0}, {0, 0, 0}
    };
    for (int i = 0; tab[i].m; i++) {
        if (!strcmp(meth, tab[i].m)) {
            if (got >= tab[i].lo && got <= tab[i].hi) return 1;
            char msg[96];
            if (tab[i].lo == tab[i].hi)
                snprintf(msg, sizeof msg, "method `%s` wants %d args", meth, (int)tab[i].lo);
            else
                snprintf(msg, sizeof msg, "method `%s` wants %d to %d args", meth, (int)tab[i].lo, (int)tab[i].hi);
            vraise(msg);
            return 0;
        }
    }
    return 1;
}

BFN Val vbuiltin(Val obj, const char *meth, Vec *a) {
    bint nargs = a ? a->len : 0;
    if (!_vcheck_meth_arity(meth, nargs)) return vunwind;
    if (obj.tag == V_STR) {
        if (!strcmp(meth, "split")) {
            if (nargs == 0) return vsplit_ws(obj);
            if (VARG(a, 0).tag == V_STR && vstr_empty(VARG(a, 0))) { vraise("empty separator"); return vunwind; }
            return vsplit(obj, VARG(a, 0));
        }
        if (!strcmp(meth, "lines")) return vlines(obj);
        if (!strcmp(meth, "trim")) return vtrim(obj);
#if (BRAT_CRT_MASK & BRAT_CRT_CASE)
        if (!strcmp(meth, "lower")) return vcase(obj, 0);
        if (!strcmp(meth, "upper")) return vcase(obj, 1);
#endif
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
        if (!strcmp(meth, "any")) return vpred(obj, vrc_inc(VARG(a, 0)), "any");
        if (!strcmp(meth, "all")) return vpred(obj, vrc_inc(VARG(a, 0)), "all");
        if (!strcmp(meth, "find")) return vpred(obj, vrc_inc(VARG(a, 0)), "find");
        if (!strcmp(meth, "fold")) return vfold(obj, vrc_inc(VARG(a, 0)), vrc_inc(VARG(a, 1)));
        if (!strcmp(meth, "sort_by")) return vsort_by(vrc_inc(obj), vrc_inc(VARG(a, 0)));
    }
#if (BRAT_CRT_MASK & BRAT_CRT_CONC)
    if (obj.tag == V_INT) {
        if (!strcmp(meth, "send")) return vchan_send(obj, VARG(a, 0));
        if (!strcmp(meth, "recv")) return vchan_recv(obj);
        if (!strcmp(meth, "close")) return vchan_close(obj);
    }
#endif
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
    if (obj.tag == V_TAG || obj.tag == V_NIL) {
        if (!strcmp(meth, "unwrap")) return vunwrap(obj);
        if (!strcmp(meth, "unwrap_or")) {
            int st = vunwrap_or_state(obj);
            if (st < 0) return vunwind;
            return st ? vtag_payload(obj) : vrc_inc(VARG(a, 0));
        }
        if (!strcmp(meth, "is_ok")) return vbool(vis_ok(obj));
        if (!strcmp(meth, "is_err")) return vbool(vis_err(obj));
        if (!strcmp(meth, "is_some")) return vbool(vis_some(obj));
        if (!strcmp(meth, "is_none")) return vbool(vis_none(obj));
    }
    vraisef("no method %s", meth); return vunwind;
}

#if (BRAT_CRT_MASK & BRAT_CRT_CONC)
typedef struct BTask BTask;
struct BTask {
    Val clo;
    Val result;
    Val arg;                /* single-item task argument */
    Val *slot;              /* single-item result slot */
    const Val *batch_items; /* `par for` contiguous chunk input slice */
    Val *batch_slots;       /* `par for` contiguous chunk output slice */
    bint batch_len;
    int has_arg;
    int err;
    BTask *qnext;           /* the run queue and the finished list */
    BTask *next;            /* the scope that owns it, when there is one */
    struct VScope *scope;
};
static BTask *g_done = 0;

/* Lazy persistent pool: one OS thread per spawn segfaults around 300. */
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
static int g_pool_stop = 0;
static int g_pool_started = 0;
static int BRAT_UNUSED g_pool_atexit_reg = 0;
#if !defined(_WIN32)
static pid_t g_pool_pid = 0;
#endif
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
static void BRAT_UNUSED _bpool_atexit(void);

static int bpool_start_locked(void) {
    if (g_pool_started) return 1;
    _btls_pool_drain();
    g_pool_stop = 0;
    int n = bpool_ncpu();
    if (n > BPOOL_MAX) n = BPOOL_MAX;
    if (n < 1) n = 1;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&g_pool_n, n, __ATOMIC_RELEASE);
#elif defined(_MSC_VER)
    InterlockedExchange((volatile LONG *)&g_pool_n, (LONG)n);
#else
    g_pool_n = n;
#endif
#if !defined(_WIN32)
    g_pool_pid = getpid();
#endif
    int started = 0;
    for (int i = 0; i < n; i++) {
#if defined(_WIN32)
        g_pool[i] = CreateThread(NULL, 0, bpool_worker, NULL, 0, NULL);
        if (!g_pool[i]) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&g_pool_n, i, __ATOMIC_RELEASE);
#elif defined(_MSC_VER)
            InterlockedExchange((volatile LONG *)&g_pool_n, (LONG)i);
#else
            g_pool_n = i;
#endif
            break;
        }
#else
        if (pthread_create(&g_pool[i], NULL, bpool_worker, NULL) != 0) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&g_pool_n, i, __ATOMIC_RELEASE);
#else
            g_pool_n = i;
#endif
            break;
        }
#endif
        started = i + 1;
    }
    g_pool_started = started;
#if !defined(BRAT_RT_SHARED)
    if (started > 0 && !g_pool_atexit_reg) {
        g_pool_atexit_reg = 1;
        atexit(_bpool_atexit);
    }
#endif
    return started > 0;
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
    if (t->batch_slots) {
        Val ab[1];
        BStackVec sv;
        VSTACK_VEC_INIT(sv, ab, 1);
        sv.vec.len = 1;
        for (bint k = 0; k < t->batch_len; k++) {
            ab[0] = t->batch_items[k];
            Val r = vcall_any(t->clo, &sv.vec);
            if (verr_pending()) {
                vrc_dec(r);
                t->err = 1;
                t->result = verr_own(verr_take());
                break;
            }
            if (r.tag == V_TAG && !strcmp(r.as.tv->tag, "err")) {
                t->err = 1;
                t->result = verr_own(r);
                break;
            }
            t->batch_slots[k] = r;
        }
        vrelease(t->clo);
        t->clo = vnil();
    } else {
        Val ab[1];
        BStackVec sv;
        VSTACK_VEC_INIT(sv, ab, 1);
        Val held_arg = vnil();
        if (t->has_arg) {
            held_arg = t->arg;
            ab[0] = held_arg;
            sv.vec.len = 1;
            t->arg = vnil();
        }
        t->result = vcall_any(t->clo, &sv.vec);
        if (t->has_arg) vrc_dec(held_arg);
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
        while (g_q_head == 0 && !g_pool_stop) {
#if defined(_WIN32)
            btask_unlock();
            Sleep(0);
            btask_lock();
#else
            pthread_cond_wait(&g_q_cond, &btlock);
#endif
        }
        if (g_q_head == 0 && g_pool_stop) {
            btask_unlock();
            _bthread_tls_drain();
            break;
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
    if (!self) _btls_pool_drain();
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
 * order. The body is pure: the checker refuses anything else: so the tasks
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
    bint max_chunks = (bint)bpool_ncpu() * 4;
    if (max_chunks < 1) max_chunks = 1;
    if (max_chunks > 128) max_chunks = 128;
    bint chunks = n < max_chunks ? n : max_chunks;
    BTask *tasks = (BTask *)calloc((size_t)chunks, sizeof(BTask));
    if (!tasks) {
        free(slots);
        vrc_dec(clo); vrc_dec(op); vrc_dec(unit);
        vraise("par for: out of memory");
        return vunwind;
    }
    VScope pf_sc;
    memset(&pf_sc, 0, sizeof pf_sc);
    pf_sc.errv = vnil();
    btask_lock();
    int started = bpool_start_locked();
    if (!started) {
        btask_unlock();
        free(tasks);
        free(slots);
        vrc_dec(clo); vrc_dec(op); vrc_dec(unit);
        vraise("par for: could not start worker thread");
        return vunwind;
    }
    bint base = n / chunks;
    bint rem = n % chunks;
    bint off = 0;
    for (bint c = 0; c < chunks; c++) {
        bint len = base + (c < rem ? 1 : 0);
        BTask *t = &tasks[c];
        t->clo = vrc_inc(clo);
        t->batch_items = v->items + off;
        t->batch_slots = slots + off;
        t->batch_len = len;
        t->result = vnil();
        t->scope = &pf_sc;
        t->next = pf_sc.tasks;
        pf_sc.tasks = t;
        pf_sc.nleft++;
        if (g_q_tail) g_q_tail->qnext = t;
        else g_q_head = t;
        g_q_tail = t;
        g_q_nleft++;
        off += len;
    }
#if !defined(_WIN32)
    if (chunks > 1) pthread_cond_broadcast(&g_q_cond);
    else pthread_cond_signal(&g_q_cond);
#endif
    bpool_idle_wake();
    btask_unlock();

    _vscope_drain(&pf_sc);
    free(tasks);
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
    Val ab[2];
    BStackVec sv;
    VSTACK_VEC_INIT(sv, ab, 2);
    sv.vec.len = 2;
    for (bint i = 0; i < n; i++) {
        ab[0] = acc;
        ab[1] = slots[i];
        Val next_acc = vcall_any(op, &sv.vec);
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

static void BRAT_UNUSED _bpool_atexit(void) {
#if !defined(_WIN32)
    if (g_pool_pid != getpid()) return;
#endif
    btask_lock();
    int n = g_pool_started;
    if (n <= 0 || g_q_nleft > 0) {
        btask_unlock();
        return;
    }
    g_pool_stop = 1;
    g_pool_started = 0;
#if !defined(_WIN32)
    pthread_cond_broadcast(&g_q_cond);
#endif
    btask_unlock();
    for (int i = 0; i < n; i++) {
#if defined(_WIN32)
        WaitForSingleObject(g_pool[i], INFINITE);
        CloseHandle(g_pool[i]);
#else
        pthread_join(g_pool[i], NULL);
#endif
    }
    btask_lock();
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&g_pool_n, 0, __ATOMIC_RELEASE);
#elif defined(_MSC_VER)
    InterlockedExchange((volatile LONG *)&g_pool_n, 0);
#else
    g_pool_n = 0;
#endif
    g_pool_stop = 0;
    btask_unlock();
    _btls_pool_drain();
}
#else
BFN void vjoin_all(void) { _btls_pool_drain(); }
#endif /* BRAT_CRT_CONC */

