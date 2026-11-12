
/* String from an explicit (pointer, length) pair. C buffers carry their own
 * length; trusting strlen there is how a NUL in the middle silently truncates.
 * Rejects embedded NUL and invalid UTF-8 at the boundary. */
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
BFN_HOT Val vat_owned(Val a, Val i);

BFN Vec *_vslice_vec(Val s) {
    if (verr_pending()) return NULL;
    if (s.tag != V_TAG || !s.as.tv || !s.as.tv->tag ||
        strcmp(s.as.tv->tag, "__brat_slice") != 0 || s.as.tv->len < 4) {
        vraise("not a slice");
        return NULL;
    }
    return s.as.tv;
}

BFN Val vslice_make(Val base, Val start, Val len, Val kind) {
    Vec *s = vvec();
    vec_push(s, vrc_inc(base));
    vec_push(s, vint(start.as.ival));
    vec_push(s, vint(len.as.ival));
    vec_push(s, vint(kind.as.ival));
    return vtagv("__brat_slice", s);
}

BFN Val vslice_of(Val v, Val a, Val b) {
    if (verr_pending()) return vunwind;
    if (v.tag != V_VEC) { vraise("slice_of wants a vec"); return vunwind; }
    if (a.tag != V_INT || b.tag != V_INT) { vraise("slice_of bounds must be integers"); return vunwind; }
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

static inline int _vslice_check_ptr(Val p, Val n, const char *bad_arg, const char *neg_n) {
    if (p.tag != V_INT || n.tag != V_INT) { vraise(bad_arg); return 0; }
    if (n.as.ival < 0) { vraise(neg_n); return 0; }
    if (p.as.ival < 0) { vraise("pointer is negative"); return 0; }
    if (p.as.ival == 0 && n.as.ival > 0) { vraise("null pointer dereference"); return 0; }
    return 1;
}

static inline int _vslice_check_idx(bint idx, bint n) {
    if (idx < 0 || idx >= n) {
        char m[96];
        snprintf(m, sizeof m, "slice index %" PRId64 " out of range 0..%" PRId64,
                 (int64_t)idx, (int64_t)n);
        vraise(m);
        return 0;
    }
    return 1;
}

BFN Val vslice_of_ptr(Val p, Val n, Val kind) {
    if (verr_pending()) return vunwind;
    if (p.tag != V_INT || n.tag != V_INT) { vraise("slice_of_ptr: invalid arguments"); return vunwind; }
    bint k = kind.as.ival;
    if (k < 1 || k > 5) {
        vraise("slice_of_ptr: kind must be i64, f64, u8, i32 or f32");
        return vunwind;
    }
    if (!_vslice_check_ptr(p, n, "slice_of_ptr: invalid arguments", "slice_of_ptr: n must be >= 0"))
        return vunwind;
    return vslice_make(p, vint(0), n, vint(k));
}

typedef struct { Val *items; bint len; } _Elems;

BFN Val vslice_elems(_Elems e) {
    Vec *v = vvec();
    for (bint i = 0; i < e.len; i++) vec_push(v, e.items[i]);
    Val elems = vvecv(v);
    Vec *out = vvec();
    vec_push(out, elems);
    vec_push(out, vint(0));
    vec_push(out, vint(e.len));
    vec_push(out, vint(7));
    vec_push(out, vint(0));
    vec_push(out, vrc_inc(elems));
    return vtagv("__brat_slice", out);
}

BFN Val vslice_of_struct(Val p, Val n, Val esize) {
    if (verr_pending()) return vunwind;
    if (!_vslice_check_ptr(p, n, "slice_of_struct: invalid arguments", "slice_of_struct: n must be >= 0"))
        return vunwind;
    if (esize.as.ival <= 0) { vraise("slice_of_struct: element size must be > 0"); return vunwind; }
    Vec *s = vvec();
    vec_push(s, vrc_inc(p));
    vec_push(s, vint(0));
    vec_push(s, vint(n.as.ival));
    vec_push(s, vint(6));
    vec_push(s, vint(esize.as.ival));
    return vtagv("__brat_slice", s);
}

BFN Val vslice_len(Val s) {
    Vec *sv = _vslice_vec(s);
    if (!sv) return vunwind;
    return sv->items[2];
}

BFN Val vslice_at(Val s, Val i) {
    Vec *sv = _vslice_vec(s);
    if (!sv) return vunwind;
    if (i.tag != V_INT) { vraise("slice_get: index wants i64"); return vunwind; }
    Val base = sv->items[0];
    bint start = sv->items[1].as.ival, n = sv->items[2].as.ival, k, idx = i.as.ival;
    if (!_vslice_check_idx(idx, n)) return vunwind;
    if (base.tag == V_VEC) return vat_owned(base, vint(start + idx));
    k = sv->items[3].as.ival;
    if (k == 7) {
        if (sv->len < 6) { vraise("not a slice"); return vunwind; }
        return vat_owned(sv->items[5], vint(start + idx));
    }
    if (k == 6) {
        if (sv->len < 5) { vraise("not a slice"); return vunwind; }
        uint64_t esize = (uint64_t)sv->items[4].as.ival;
        uint64_t off = (uint64_t)(start + idx) * esize;
        return vint((bint)((uint64_t)base.as.ival + off));
    }
    if (k == 2) return vfloat(((double *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 3) return vint((int64_t)((unsigned char *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 4) return vint((int64_t)((int32_t *)(intptr_t)base.as.ival)[start + idx]);
    if (k == 5) return vfloat((double)((float *)(intptr_t)base.as.ival)[start + idx]);
    return vint(((int64_t *)(intptr_t)base.as.ival)[start + idx]);
}

BFN Val vslice_set(Val s, Val i, Val v) {
    Vec *sv = _vslice_vec(s);
    if (!sv) { vrc_dec(v); return vunwind; }
    if (i.tag != V_INT) { vrc_dec(v); vraise("slice_set: index wants i64"); return vunwind; }
    Val base = sv->items[0];
    bint start = sv->items[1].as.ival, n = sv->items[2].as.ival, k, idx = i.as.ival;
    if (!_vslice_check_idx(idx, n)) { vrc_dec(v); return vunwind; }
    if (base.tag == V_VEC) {
        (void)vset_at(base, vint(start + idx), v);
        return verr_pending() ? vunwind : vint(0);
    }
    k = sv->items[3].as.ival;
    if (k == 7) {
        if (sv->len < 6) { vrc_dec(v); vraise("not a slice"); return vunwind; }
        (void)vset_at(sv->items[5], vint(start + idx), v);
        return verr_pending() ? vunwind : vint(0);
    }
    if (k == 6) {
        vrc_dec(v);
        vraise("slice_set: a window of C structs is read-only");
        return vunwind;
    }
    if (k == 2) {
        if (v.tag != V_INT && v.tag != V_FLOAT) { vrc_dec(v); vraise("slice_set: want f64, got other"); return vunwind; }
        ((double *)(intptr_t)base.as.ival)[start + idx] = vnum(v);
        return vint(0);
    }
    if (k == 4) {
        if (v.tag != V_INT) { vrc_dec(v); vraise("slice_set: want i32, got other"); return vunwind; }
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
        if (v.tag != V_INT && v.tag != V_FLOAT) { vrc_dec(v); vraise("slice_set: want f32, got other"); return vunwind; }
        ((float *)(intptr_t)base.as.ival)[start + idx] = (float)vnum(v);
        return vint(0);
    }
    if (v.tag != V_INT) { vrc_dec(v); vraise("slice_set: want i64, got other"); return vunwind; }
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
BFN_HOT Val vat_owned(Val a, Val i) {
    if (a.tag == V_STRUCT) {
        Map *m = a.as.map;
        if ((m->shape || !m->slots) && !m->hashes) {
            BRAT_CTR_LOCK_OBJ(m);
            for (bint j = 0; j < m->used; j++) {
                if (m->keys[j].tag != V_MOVED && vmap_key_same(m->keys[j], i)) {
                    Val r = vrc_inc(m->vals[j]);
                    BRAT_CTR_UNLOCK_OBJ(m);
                    return r;
                }
            }
            BRAT_CTR_UNLOCK_OBJ(m);
        }
    }
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
    Val r = vat(a, i);
    if (a.tag == V_STR) return r;
    /* A failed read hands back the error itself, which owns its own storage;
     * counting it here would keep the message alive after the catch releases
     * it -- one leaked Vec per caught out-of-range index. */
    if (verr_pending()) return r;
    return vrc_inc(r);
}
BFN_HOT Val vat_cstr_ic(Val a, const char *k, size_t klen, BFieldIC *ic) {
    if (a.tag == V_STRUCT) {
        Map *m = a.as.map;
        BRAT_CTR_LOCK_OBJ(m);
        Val *o = map_get_ic(m, k, ic);
        if (o) {
            Val r = vrc_inc(*o);
            BRAT_CTR_UNLOCK_OBJ(m);
            return r;
        }
        BRAT_CTR_UNLOCK_OBJ(m);
    }
    Val kv = (klen <= BRAT_SSO_MAX) ? vstr_sso(k, klen) : vstrn(k, (bint)klen);
    Val r = vat_owned(a, kv);
    vrc_dec(kv);
    return r;
}

/* Internal payload read for `match` variant destructuring (`__at`). Kept
 * separate from `vat` / `vat_owned` so user-level `Ok(1)[1]` raises
 * `index of non-indexable`. */
BFN Val vtag_at_owned(Val a, Val i) {
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
    return vat_owned(a, i);
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
       fixed-point formatting (decided by decimal exponent, not by
       precision). */
    if (isinf(x)) return x < 0 ? "-inf" : "inf";
#if !defined(_WIN32)
    locale_t cloc = _brat_c_locale();
    locale_t prev_loc = cloc ? uselocale(cloc) : (locale_t)0;
#endif
    for (p = 0; p <= 17; p++) {
        snprintf(b, 48, "%.*e", p, x);
#if defined(_WIN32)
        const char *dp = localeconv()->decimal_point;
        if (dp && dp[0] && !(dp[0] == '.' && dp[1] == 0)) {
            size_t dplen = strlen(dp);
            char *hit = strstr(b, dp);
            if (hit) { *hit = '.'; if (dplen > 1) memmove(hit + 1, hit + dplen, strlen(hit + dplen) + 1); }
        }
#endif
        int ok = 0;
        if (_bstrtod_c(b, strlen(b), &ok) == x && ok) break;
    }
    /* repr()'s fixed range, exactly: zero and [1e-4, 1e16). Seventeen digits
       of 1e-4 end at the twentieth place after the point. */
    if (x == 0.0 || (fabs(x) >= 1e-4 && fabs(x) < 1e16)) {
        for (p = 0; p <= 20; p++) {
            snprintf(b, 48, "%.*f", p, x);
#if defined(_WIN32)
            const char *dp = localeconv()->decimal_point;
            if (dp && dp[0] && !(dp[0] == '.' && dp[1] == 0)) {
                size_t dplen = strlen(dp);
                char *hit = strstr(b, dp);
                if (hit) { *hit = '.'; if (dplen > 1) memmove(hit + 1, hit + dplen, strlen(hit + dplen) + 1); }
            }
#endif
            int ok = 0;
            if (_bstrtod_c(b, strlen(b), &ok) == x && ok) break;
        }
    }
#if !defined(_WIN32)
    if (cloc) uselocale(prev_loc);
#endif
    if (!strpbrk(b, ".eEnN")) { strcat(b, ".0"); }
    return b;
}


/* Defined below (one predicate for the three printers); `print` comes first. */
BFN bint vtag_closure(Val v);

/* String quote escaping for debug formatting. */
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

BFN const char *vtag_disp_name_(const char *tag, char *buf, size_t bsz) {
    if (!tag || !tag[0]) return "Tag";
    if (!strcmp(tag, "ok")) return "Ok";
    if (!strcmp(tag, "err")) return "Err";
    if (!strcmp(tag, "some")) return "Some";
#ifdef BRAT_HAS_VTAB_DISP
    {
        const char *d = _brat_vtag_disp(tag);
        if (d) return d;
    }
#endif
    if (bsz > 1) {
        size_t n = strlen(tag);
        if (n >= bsz) n = bsz - 1;
        memcpy(buf, tag, n);
        buf[n] = 0;
        if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 'a' + 'A');
        return buf;
    }
    return tag;
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
        vfmt_emit_(cur.as.b ? "true" : "false", cur.as.b ? 4 : 5, &out, fp, &n);
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
    case V_TAG: {
        if (vtag_closure(cur)) {
            vfmt_emit_("fn", 2, &out, fp, &n);
            break;
        }
        char tbuf[128];
        const char *dn = vtag_disp_name_(cur.as.tv->tag, tbuf, sizeof tbuf);
        vfmt_emit_(dn, (bint)strlen(dn), &out, fp, &n);
        if (cur.as.tv->len == 0) break;
        vfmt_emit_("(", 1, &out, fp, &n);
        BFMT_PUSH_(cur, 0, 0);
        cur = cur.as.tv->items[0];
        q = 1;
        goto descend;
    }
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
       printing begins. The walk costs what `vtxt`/`vlogv` already pay. */
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
   leave a stray empty line on stdout. */
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

/* Exact numeric ordering without rounding 64-bit integers through `double`
   (which loses precision past 2^53). */
BFN int vcmp_int_double(double d, bint i) {
    if (d != d) return 1;
    if (d >= 9223372036854775808.0) return 1;
    if (d < -9223372036854775808.0) return -1;
    double fl = floor(d);
    bint fi = (bint)fl;
    if (fi < i) return -1;
    if (fi > i) return 1;
    return d > fl ? 1 : 0;
}
BFN int vcmp_di_eq(double d, bint i) { return (d == d) && vcmp_int_double(d, i) == 0; }
BFN int vcmp_di_lt(double d, bint i) { return (d == d) && vcmp_int_double(d, i) < 0; }
BFN int vcmp_di_le(double d, bint i) { return (d == d) && vcmp_int_double(d, i) <= 0; }
BFN int vcmp_di_gt(double d, bint i) { return (d == d) && vcmp_int_double(d, i) > 0; }
BFN int vcmp_di_ge(double d, bint i) { return (d == d) && vcmp_int_double(d, i) >= 0; }

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
   with a type rank, a question the program never asked; the runtime refuses
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
static BTLS char *_vtxt_keep[VTXT_SLOTS];
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
    bint k = vtxt_next % VTXT_SLOTS;
    free(_vtxt_keep[k]);
    _vtxt_keep[k] = heap;
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
        size_t cap = need < 32 ? 32 : (need <= SIZE_MAX / 2 ? need * 2 : need);
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
            if (!BIS_ARENA(h) && !BIS_IMMORTAL(h)) {
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
                        while (nc < need) {
                            if (nc > SIZE_MAX / 2) { nc = need; break; }
                            nc *= 2;
                        }
                        if (nc > SIZE_MAX - sizeof(BHead) || bsize_over_bint(nc)) vfatal_oom();
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
BFN_HOT bint bshl64(bint a, bint b) {
    return (bint)((uint64_t)a << ((uint64_t)b & 63u));
}
BFN_HOT bint bshr64(bint a, bint b) {
#if (-1LL >> 1) == -1LL
    return (bint)(a >> ((unsigned)b & 63u));
#else
    uint64_t s = (uint64_t)b & 63u;
    if (s == 0) return a;
    uint64_t ua = (uint64_t)a;
    uint64_t sign = (uint64_t)0 - (ua >> 63u);
    return (bint)((ua >> s) | (sign & (~0ULL << (64u - s))));
#endif
}
BFN_HOT Val vbnot(Val a) {
    if (a.tag == V_INT) return vint((bint)(~(uint64_t)a.as.ival));
    vraise("type error in unary ~"); return vunwind;
}
BFN_HOT Val vband(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival & (uint64_t)b.as.ival));
    vraise("type error in &"); return vunwind;
}
BFN_HOT Val vbor(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival | (uint64_t)b.as.ival));
    vraise("type error in |"); return vunwind;
}
BFN_HOT Val vbxor(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT)
        return vint((bint)((uint64_t)a.as.ival ^ (uint64_t)b.as.ival));
    vraise("type error in ^"); return vunwind;
}
BFN_HOT Val vbshl(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT) return vint(bshl64(a.as.ival, b.as.ival));
    vraise("type error in <<"); return vunwind;
}
BFN_HOT Val vbshr(Val a, Val b) {
    if (a.tag == V_INT && b.tag == V_INT) return vint(bshr64(a.as.ival, b.as.ival));
    vraise("type error in >>"); return vunwind;
}
/* SWAR-verified cached UTF-8 codepoint length + 7-bit ASCII flag (`O(1)` after first probe). */
BFN_HOT void vstr_meta(const Val *a, bint *out_cplen, int *out_ascii) {
    if (a->sso) {
        size_t sl = vstr_sso_len(a);
        uint64_t w0, w1;
        memcpy(&w0, (const char *)a + 1, 8);
        memcpy(&w1, (const char *)a + 8, 8);
        if (((w0 | w1) & 0x8080808080808080ULL) == 0) {
            *out_cplen = (bint)sl;
            *out_ascii = 1;
            return;
        }
        const unsigned char *u = (const unsigned char *)a + 1;
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
BFN Val vat(Val a, Val i) {
    if (a.tag == V_STRUCT) {
        Val *o = map_get_v(a.as.map, i);
        if (!o) {
            /* A map with a struct tag is reported as struct in diagnostics;
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
    vraise("index of non-indexable"); return vunwind;
}

static BTLS int _verr_pending;
/* _verr_exc: 1 for a runtime error, 0 for an `err` value (`raise`) that a
   callee returns like a normal value. Only exceptions survive a frame. */
static BTLS int _verr_exc;

/* Raises set a thread-local pending flag; callers check verr_pending() instead
   of longjmp, which is UB through non-volatile frames at -O2. */

BFN_HOT int verr_pending(void) { return _verr_pending; }
BFN_HOT int bverr_unlikely(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(_verr_pending, 0);
#else
    return _verr_pending;
#endif
}
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
    const char *s = arg ? arg : "";
    const char *p = strstr(fmt, "%s");
    if (p) {
        int pre = (int)(p - fmt);
        snprintf(buf, sizeof buf, "%.*s%s%s", pre, fmt, s, p + 2);
    } else {
        snprintf(buf, sizeof buf, "%s", fmt);
    }
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
#if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
#define BSTACK_HAS_PROBE 1
#define BSTACK_MARGIN ((size_t)256 * 1024)
#if defined(__linux__)
extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
#endif
static BTLS char *_bstack_floor;
static BTLS int _bstack_probed;
BFN void bstack_probe(void) {
    _bstack_probed = 1;
#if defined(__linux__)
    pthread_attr_t at;
    void *lo = NULL;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &at) != 0) return;
    if (pthread_attr_getstack(&at, &lo, &size) == 0 && size > 2 * BSTACK_MARGIN)
        _bstack_floor = (char *)lo + BSTACK_MARGIN;
    pthread_attr_destroy(&at);
#elif defined(__APPLE__) && defined(__MACH__)
    void *hi = pthread_get_stackaddr_np(pthread_self());
    size_t size = pthread_get_stacksize_np(pthread_self());
    if (hi != NULL && size > 2 * BSTACK_MARGIN)
        _bstack_floor = (char *)hi - size + BSTACK_MARGIN;
#elif defined(_WIN32)
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    if (hi > lo && (size_t)(hi - lo) > 2 * BSTACK_MARGIN)
        _bstack_floor = (char *)lo + BSTACK_MARGIN;
#endif
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
#if defined(BSTACK_HAS_PROBE)
    if (!_bstack_probed) bstack_probe();
#endif
}
#if defined(BSTACK_HAS_PROBE) && (defined(__GNUC__) || defined(__clang__))
static __attribute__((noinline, cold, unused)) int bdepth_stack_exceeded(void) {
    if (!_bstack_probed) bstack_probe();
    if (_bstack_floor != NULL && (char *)__builtin_frame_address(0) < _bstack_floor)
        return 1;
    return 0;
}
#endif
BFN_HOT int bdepth_push(void) {
    bint d = _bdepth + 1;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(d > _bbudget, 0)) return 1;
#if defined(BSTACK_HAS_PROBE)
    if (__builtin_expect((d & 63) == 0 && bdepth_stack_exceeded(), 0))
        return 1;
#endif
#else
    if (d > _bbudget) return 1;
#if defined(BSTACK_HAS_PROBE)
    if ((d & 63) == 0) {
        if (!_bstack_probed) bstack_probe();
        if (_bstack_floor && (char *)&d < _bstack_floor) return 1;
    }
#endif
#endif
    _bdepth = d;
    return 0;
}
BFN_HOT void bdepth_pop(void) { _bdepth--; }
BFN_HOT int bdepth_leaf_overflow(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(_bdepth >= _bbudget, 0);
#else
    return _bdepth >= _bbudget;
#endif
}
BFN_HOT bint bdepth_rem(void) { return _bbudget - _bdepth; }
BFN_HOT int bdepth_rem_leaf(bint rem) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(rem <= 0, 0);
#else
    return rem <= 0;
#endif
}
BFN_HOT int bdepth_probe_rem(bint rem) {
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(rem <= 0, 0)) return 1;
#if defined(BSTACK_HAS_PROBE)
    if (__builtin_expect((rem & 63) == 0 && bdepth_stack_exceeded(), 0))
        return 1;
#endif
    return 0;
#else
    if (rem <= 0) return 1;
#if defined(BSTACK_HAS_PROBE)
    if ((rem & 63) == 0) {
        if (!_bstack_probed) bstack_probe();
        if (_bstack_floor && (char *)&rem < _bstack_floor) return 1;
    }
#endif
    return 0;
#endif
}
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
    if (!rp && vbase_ready) return;
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

BFN int _vunder_base(const char *path, const char **rel_out) {
    size_t bl = strlen(vbase_g);
    if (bl == 0) return 0;
#if defined(_WIN32)
    if (_strnicmp(path, vbase_g, bl) != 0) return 0;
#else
    if (strncmp(path, vbase_g, bl) != 0) return 0;
#endif
    if (vbase_g[bl - 1] == '/') {
        if (rel_out) *rel_out = path + bl;
        return 1;
    }
    if (path[bl] == 0) {
        if (rel_out) *rel_out = path + bl;
        return 1;
    }
    if (path[bl] == '/') {
        if (rel_out) *rel_out = path + bl + 1;
        return 1;
    }
    return 0;
}

BFN const char *vconfine(const char *p) {
    if (_vfiles_free()) return p;
    if (!p) return NULL;
    if (!vbase_ready || !vbase_g[0]) return NULL; /* base must be set first */

    static BTLS char res[8192];
    int rn = _vis_abs_path(p) ? snprintf(res, sizeof res, "%s", p)
                              : snprintf(res, sizeof res, "%s/%s", vbase_g, p);
    if (rn < 0 || (size_t)rn >= sizeof res) return NULL;
    _vslash_norm(res);

    char real[8192];
    if (_vrealpath(res, real)) {
        rn = snprintf(res, sizeof res, "%s", real);
        if (rn < 0 || (size_t)rn >= sizeof res) return NULL;
    } else {
        if (vnorm(res) != 0) return NULL;
#if defined(_WIN32)
        char dir[8192];
        memcpy(dir, res, strlen(res) + 1);
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
            memcpy(dir, res, strlen(res) + 1);
            int resolved_parent = 0;
            for (char *slash = strrchr(dir, '/'); slash && slash != dir; slash = strrchr(dir, '/')) {
                size_t off = (size_t)(slash - dir);
                *slash = 0;
                if (realpath(dir, real)) {
                    char combined[8192];
                    int cn = snprintf(combined, sizeof combined, "%s%s", real, res + off);
                    if (cn < 0 || (size_t)cn >= sizeof res) return NULL;
                    memcpy(res, combined, (size_t)cn + 1);
                    if (vnorm(res) != 0) return NULL;
                    resolved_parent = 1;
                    break;
                }
            }
            struct stat st;
            if (lstat(res, &st) == 0 && S_ISLNK(st.st_mode)) {
                char lbuf[4096];
                ssize_t ln = readlink(res, lbuf, sizeof lbuf);
                if (ln <= 0 || (size_t)ln >= sizeof lbuf) return NULL;
                lbuf[ln] = 0;
                if (lbuf[0] == '/') {
                    rn = snprintf(res, sizeof res, "%s", lbuf);
                    if (rn < 0 || (size_t)rn >= sizeof res) return NULL;
                } else {
                    char *sl = strrchr(res, '/');
                    if (!sl) return NULL;
                    char combined[8192 + 4096 + 2];
                    *sl = 0;
                    int cn = snprintf(combined, sizeof combined, "%s/%s", res, lbuf);
                    /* `res` cannot hold what `combined` may: an over-wide path
                       is refused rather than truncated into a wrong one. */
                    if (cn < 0 || (size_t)cn >= sizeof res) return NULL;
                    memcpy(res, combined, (size_t)cn + 1);
                }
                if (vnorm(res) != 0) return NULL;
                if (realpath(res, real)) {
                    rn = snprintf(res, sizeof res, "%s", real);
                    if (rn < 0 || (size_t)rn >= sizeof res) return NULL;
                    break;
                }
                continue;
            }
            (void)resolved_parent;
            break;
        }
#endif
    }
    if (_vunder_base(res, NULL)) return res;
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
    return _vunder_base(target, NULL);
}

static inline HANDLE _vwin_open_attr(const wchar_t *wp, DWORD share, DWORD flags) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = FALSE;
    return CreateFileW(wp, FILE_READ_ATTRIBUTES, share, &sa, OPEN_EXISTING, flags, NULL);
}

static HANDLE _vwin_open_parent_confined(const wchar_t *wpath, int *escaped) {
    if (escaped) *escaped = 0;
    wchar_t wpar[4096];
    size_t n = wcslen(wpath);
    if (n == 0 || n >= 4096) { errno = EINVAL; return INVALID_HANDLE_VALUE; }
    memcpy(wpar, wpath, (n + 1) * sizeof(wchar_t));
    while (n > 0 && (wpar[n - 1] == L'\\' || wpar[n - 1] == L'/')) n--;
    size_t sep = n;
    while (sep > 0 && wpar[sep - 1] != L'\\' && wpar[sep - 1] != L'/') sep--;
    if (sep == 0) {
        wpar[0] = L'.';
        wpar[1] = 0;
    } else if (sep == 3 && wpar[1] == L':') {
        wpar[3] = 0;
    } else if (sep == 1) {
        wpar[1] = 0;
    } else {
        wpar[sep - 1] = 0;
    }
    HANDLE ph = _vwin_open_attr(wpar, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (ph == INVALID_HANDLE_VALUE) {
        DWORD le = GetLastError();
        if (le == ERROR_CANT_ACCESS_FILE && escaped) *escaped = 1;
        errno = (le == ERROR_FILE_NOT_FOUND || le == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION pfi;
    if (GetFileType(ph) != FILE_TYPE_DISK || !GetFileInformationByHandle(ph, &pfi) ||
        !(pfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(ph);
        errno = ENOTDIR;
        return INVALID_HANDLE_VALUE;
    }
    if ((pfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !_vhandle_confined(ph)) {
        CloseHandle(ph);
        if (escaped) *escaped = 1;
        errno = EACCES;
        return INVALID_HANDLE_VALUE;
    }
    return ph;
}
#endif

BFN int _vfd_confined(int fd) {
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
    ssize_t n = readlink(proc, target, sizeof target);
    if (n <= 0 || (size_t)n >= sizeof target) return 0;
    target[n] = 0;
#elif defined(__APPLE__)
    char target[PATH_MAX];
    if (fcntl(fd, F_GETPATH, target) != 0) return 0;
#else
    (void)fd;
    return 0;
#endif
    return _vunder_base(target, NULL);
#endif
}

#if !defined(_WIN32)
/* Walk from `vbase_g` to the parent directory of canonical `rp` using `openat`
 * with `O_NOFOLLOW | O_DIRECTORY` so a concurrent directory-to-symlink swap
 * between `vconfine()` and the leaf syscall fails closed with `ELOOP`. */
static int _vopen_parent_confined(const char *rp, char *leaf_out, size_t leaf_cap, int *escaped) {
    if (escaped) *escaped = 0;
    const char *cur = NULL;
    if (!_vunder_base(rp, &cur) || !*cur) {
        errno = (cur && !*cur) ? EISDIR : EACCES;
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
    HANDLE ph = INVALID_HANDLE_VALUE;
    if (!_vfiles_free() && write_mode) {
        ph = _vwin_open_parent_confined(wpath, escaped);
        if (ph == INVALID_HANDLE_VALUE) return NULL;
    }
    DWORD access = (write_mode == 2) ? FILE_APPEND_DATA : (write_mode ? GENERIC_WRITE : GENERIC_READ);
    DWORD disp = write_mode ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (!_vfiles_free()) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(wpath, access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           &sa, disp, flags, NULL);
    DWORD open_err = GetLastError();
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    if (h == INVALID_HANDLE_VALUE) {
        if (!_vfiles_free() && open_err == ERROR_CANT_ACCESS_FILE && escaped) *escaped = 1;
        errno = (open_err == ERROR_FILE_NOT_FOUND || open_err == ERROR_PATH_NOT_FOUND)
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
    if (write_mode == 1) {
        LARGE_INTEGER z;
        z.QuadPart = 0;
        if (!SetFilePointerEx(h, z, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h);
            errno = EIO;
            return NULL;
        }
    }
    int ofl = ((write_mode == 2) ? (_O_WRONLY | _O_APPEND) : (write_mode ? _O_WRONLY : _O_RDONLY)) | _O_BINARY | _O_NOINHERIT;
    int fd = _open_osfhandle((intptr_t)h, ofl);
    if (fd < 0) { CloseHandle(h); errno = EIO; return NULL; }
    FILE *f = _fdopen(fd, (write_mode == 2) ? "ab" : (write_mode ? "wb" : "rb"));
    if (!f) { _close(fd); errno = EIO; return NULL; }
    return f;
#else
    int fd = -1;
    int wfl = (write_mode == 2) ? (O_WRONLY | O_CREAT | O_APPEND) : (write_mode ? (O_WRONLY | O_CREAT) : O_RDONLY);
    if (_vfiles_free()) {
        int fl = wfl | O_CLOEXEC;
        fd = open(rp, fl, 0666);
        if (fd < 0) return NULL;
    } else {
        char leaf[1024];
        int dfd = _vopen_parent_confined(rp, leaf, sizeof leaf, escaped);
        if (dfd < 0) return NULL;
        int fl = wfl | O_CLOEXEC;
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
    if (write_mode == 1 && ftruncate(fd, 0) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    FILE *f = fdopen(fd, (write_mode == 2) ? "ab" : (write_mode ? "wb" : "rb"));
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
    /* `clock_gettime` rather than C11's `timespec_get`, which `-std=c99` hides.
       POSIX is already required by `fileno`, `realpath`, `strdup` and `pthread`,
       and the feature level is asked for in `core.h`. */
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
    while (ms > 0) {
        DWORD step = ms > (bint)0x7fffffff ? (DWORD)0x7fffffff : (DWORD)ms;
        Sleep(step);
        ms -= (bint)step;
    }
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
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
    size_t qlen = (size_t)vsprint_len_q(v, 1);
    char *o = (char *)malloc(qlen + strlen(prefix) + 4);
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

#if (BRAT_CRT_MASK & BRAT_CRT_RE)
static int r_cls_shorthand(char esc, unsigned uch) {
    int is_d = (uch >= '0' && uch <= '9');
    int is_w = is_d || (uch >= 'a' && uch <= 'z') || (uch >= 'A' && uch <= 'Z') || uch == '_';
    int is_s = (uch == ' ' || uch == '\t' || uch == '\n' || uch == '\r');
    if (esc == 'd') return is_d;
    if (esc == 'D') return !is_d;
    if (esc == 'w') return is_w;
    if (esc == 'W') return !is_w;
    if (esc == 's') return is_s;
    if (esc == 'S') return !is_s;
    return -1;
}

BFN bint vcp_next(const char *p, unsigned *out);

/* A class member is a code point, so `[é]` holds one character and `[а-я]`
   is a range of characters. */
static int r_cls_next(const char **bp, unsigned *out, char *shorthand) {
    const char *b = *bp;
    *shorthand = 0;
    if (!*b) return 0;
    if (*b == '\\' && b[1]) {
        char e = b[1];
        *bp = b + 2;
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
            *shorthand = e;
            *out = 0;
            return 2;
        }
        if (e == 'n') *out = '\n';
        else if (e == 't') *out = '\t';
        else if (e == 'r') *out = '\r';
        else *bp = b + 1 + vcp_next(b + 1, out);
        return 2;
    }
    *bp = b + vcp_next(b, out);
    return 1;
}

static int r_cls_match(const char *body, unsigned uch) {
    int neg = 0, found = 0;
    if (*body == '^') { neg = 1; body++; }
    while (*body) {
        unsigned lo = 0;
        char sh = 0;
        r_cls_next(&body, &lo, &sh);
        if (sh) {
            if (r_cls_shorthand(sh, uch) == 1) { found = 1; break; }
            continue;
        }
        if (*body == '-' && body[1]) {
            const char *peek = body + 1;
            unsigned hi = 0;
            char sh2 = 0;
            r_cls_next(&peek, &hi, &sh2);
            if (!sh2) {
                body = peek;
                if (uch >= lo && uch <= hi) { found = 1; break; }
                continue;
            }
        }
        if (uch == lo) { found = 1; break; }
    }
    return neg ? !found : found;
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
        if (m < g->plen && g->pat[m] == '^') m++;
        if (m < g->plen && g->pat[m] == ']') m++;
        while (m < g->plen && g->pat[m] != ']') {
            if (g->pat[m] == '\\') {
                if (m + 1 >= g->plen) { g->err = 1; return -1; }
                m += 2;
            } else {
                m++;
            }
        }
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
        if (g->i + 1 >= g->plen) { g->err = 1; return -1; }
        char e = g->pat[g->i + 1];
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
    /* A literal is a whole code point: `é+` repeats the character, not its last byte. */
    while (!g->err && g->p[body].q == 'l' && g->i < g->plen &&
           ((unsigned char)g->pat[g->i] & 0xC0) == 0x80) {
        bint at = r_emit(g, 'c', 0, 0);
        g->p[at].q = 'l';
        g->p[at].ch = g->pat[g->i++];
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
    /* An empty sequence falls through to whatever is emitted after it. */
    g->p[entry].x = (first_atom < 0) ? g->n : first_atom;
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
        r_seq(g);
        if (g->err) { free(sps); free(jmps); return -1; }
        jmps[nj++] = r_emit(g, 'j', 0, 0);
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

static int r_compile_prog_n(RProg *g, const char *pat, bint plen, bint *start_out) {
    g->p = NULL; g->n = 0; g->cap = 0;
    g->bodies = NULL; g->nbodies = 0; g->bodies_cap = 0;
    g->pat = pat; g->i = 0; g->plen = plen;
    g->ngroups = 0; g->err = 0;
    bint sv0 = r_emit(g, 'v', 0, 0);
    bint body = r_alt(g);
    if (g->err || body < 0 || g->i != g->plen) { r_free(g); return 0; }
    r_emit(g, 'v', 1, 0);
    r_emit(g, 'm', 0, 0);
    if (g->err) { r_free(g); return 0; }
    *start_out = sv0;
    return 1;
}

static void re_pike_add(RInst *p, bint pc, bint slen, bint si,
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
            if (si == slen && sp < stk_cap) { stk[sp].pc = q + 1; stk[sp].slot = 0; stk[sp].prev = 0; sp++; }
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
        /* A match starts on a code point, never inside one. */
        if (!matched && (si >= slen || ((unsigned char)s[si] & 0xC0) != 0x80)) {
            for (bint z = 0; z < nslots; z++) work_slots[z] = -1;
            re_pike_add(g->p, start, slen, si, list_pcs, list_slots, &nlist,
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
            bint to = pc + 1;
            if (in->q == 'l') { if (s[si] != in->ch) continue; }
            else {
                /* `.` and a class take one code point: it is tested at its first
                   byte and the thread waits here while the rest goes by. */
                if (in->q != 'd' && ((unsigned char)s[si] & 0xC0) != 0x80) {
                    unsigned cp = 0;
                    vcp_next(s + si, &cp);
                    if (!r_cls_match(in->body, cp)) continue;
                }
                if (si + 1 < slen && ((unsigned char)s[si + 1] & 0xC0) == 0x80) to = pc;
            }
            if (nslots > 0)
                memcpy(work_slots, list_slots + k * nslots, sizeof(bint) * (size_t)nslots);
            re_pike_add(g->p, to, slen, si + 1, next_pcs, next_slots, &nn,
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
    if (!r_compile_prog_n(&g, pat, (bint)vstr_len_fast(&pv), &start)) { vraise("bad regex pattern"); return 0; }
    bint slots[2] = {-1, -1};
    int ok = re_pike_search(&g, start, s, (bint)vstr_len_fast(&sv), 0, slots, 2);
    r_free(&g);
    return ok;
}

BFN Val vre_find(Val sv, Val pv) {
    if (sv.tag != V_STR || pv.tag != V_STR) { vraise("re_find wants str"); return vnil(); }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog_n(&g, pat, (bint)vstr_len_fast(&pv), &start)) { vraise("bad regex pattern"); return vnil(); }
    bint slots[2] = {-1, -1};
    int ok = re_pike_search(&g, start, s, (bint)vstr_len_fast(&sv), 0, slots, 2);
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
    if (!r_compile_prog_n(&g, pat, (bint)vstr_len_fast(&pv), &start)) { vraise("bad regex pattern"); return vnil(); }
    bint ng = g.ngroups > BRAT_RE_MAX_GROUPS ? BRAT_RE_MAX_GROUPS : g.ngroups;
    bint nslots = 2 * (ng + 1);
    bint *slots = (bint *)malloc(sizeof(bint) * (size_t)nslots);
    if (!slots) { r_free(&g); vfatal_oom(); }
    for (bint z = 0; z < nslots; z++) slots[z] = -1;
    int ok = re_pike_search(&g, start, s, (bint)vstr_len_fast(&sv), 0, slots, nslots);
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

static int _vre_buf_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (n == 0) return 1;
    size_t max_str = (size_t)1 << 30;
    if (n > max_str || *len > max_str - n) return 0;
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
    return 1;
}

BFN Val vre_replace(Val sv, Val pv, Val rv) {
    if (sv.tag != V_STR || pv.tag != V_STR || rv.tag != V_STR) {
        vraise("re_replace wants str");
        return vnil();
    }
    const char *s = vstr_c(&sv), *pat = vstr_c(&pv), *repl = vstr_c(&rv);
    RProg g;
    bint start = 0;
    if (!r_compile_prog_n(&g, pat, (bint)vstr_len_fast(&pv), &start)) { vraise("bad regex pattern"); return vnil(); }
    bint slen = (bint)vstr_len_fast(&sv), rlen = (bint)vstr_len_fast(&rv);
    bint ng = g.ngroups > BRAT_RE_MAX_GROUPS ? BRAT_RE_MAX_GROUPS : g.ngroups;
    bint nslots = 2 * (ng + 1);
    bint *slots = (bint *)malloc(sizeof(bint) * (size_t)nslots);
    if (!slots) { r_free(&g); vfatal_oom(); }
    char *out = NULL;
    size_t olen = 0, ocap = 0;
    int over = 0;
    bint cur = 0, search_from = 0;
    while (search_from <= slen && !over) {
        for (bint z = 0; z < nslots; z++) slots[z] = -1;
        if (!re_pike_search(&g, start, s, slen, search_from, slots, nslots)) break;
        bint m0 = slots[0], m1 = slots[1];
        if (m0 > cur && !_vre_buf_append(&out, &olen, &ocap, s + cur, (size_t)(m0 - cur))) { over = 1; break; }
        for (bint ri = 0; ri < rlen; ri++) {
            if (repl[ri] == '$' && ri + 1 < rlen) {
                char nxt = repl[ri + 1];
                if (nxt == '$') {
                    if (!_vre_buf_append(&out, &olen, &ocap, "$", 1)) { over = 1; break; }
                    ri++;
                    continue;
                }
                if (nxt >= '0' && nxt <= '9') {
                    bint gi = nxt - '0';
                    if (gi <= ng && slots[2 * gi] >= 0 && slots[2 * gi + 1] >= slots[2 * gi]) {
                        if (!_vre_buf_append(&out, &olen, &ocap, s + slots[2 * gi],
                                             (size_t)(slots[2 * gi + 1] - slots[2 * gi]))) { over = 1; break; }
                    }
                    ri++;
                    continue;
                }
            }
            if (repl[ri] == '\\' && ri + 1 < rlen && repl[ri + 1] >= '1' && repl[ri + 1] <= '9') {
                bint gi = repl[ri + 1] - '0';
                if (gi <= ng && slots[2 * gi] >= 0 && slots[2 * gi + 1] >= slots[2 * gi]) {
                    if (!_vre_buf_append(&out, &olen, &ocap, s + slots[2 * gi],
                                         (size_t)(slots[2 * gi + 1] - slots[2 * gi]))) { over = 1; break; }
                }
                ri++;
                continue;
            }
            if (!_vre_buf_append(&out, &olen, &ocap, repl + ri, 1)) { over = 1; break; }
        }
        if (over) break;
        cur = m1;
        if (m1 == m0) {
            if (m1 >= slen) break;
            /* Advance one full UTF-8 codepoint so split UTF-8 bytes are never emitted. */
            unsigned char lead = (unsigned char)s[m1];
            bint step = (lead < 0x80) ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : 4));
            if (m1 + step > slen) step = 1;
            if (!_vre_buf_append(&out, &olen, &ocap, s + m1, (size_t)step)) { over = 1; break; }
            cur = m1 + step;
            search_from = cur;
        } else {
            search_from = m1;
        }
    }
    if (!over && cur < slen && !_vre_buf_append(&out, &olen, &ocap, s + cur, (size_t)(slen - cur))) over = 1;
    free(slots);
    r_free(&g);
    if (over) { free(out); vraise("out of memory"); return vnil(); }
    if (!out) return vstr("");
    Val res = vstrn(out, (bint)olen);
    free(out);
    return res;
}
#endif /* BRAT_CRT_RE */
