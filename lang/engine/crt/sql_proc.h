
#if (BRAT_CRT_MASK & BRAT_CRT_SQL)
#if !defined(_WIN32)
typedef struct _BSqlApi {
    int ready;
    int (*open_v2)(const char *, void **, int, const char *);
    int (*busy_timeout)(void *, int);
    int (*close_v2)(void *);
    int (*set_authorizer)(void *, int (*)(void *, int, const char *, const char *, const char *, const char *), void *);
    int (*db_config)(void *, int, ...);
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
    *(void **)&_bsql.open_v2 = dlsym(h, "sqlite3_open_v2");
    *(void **)&_bsql.busy_timeout = dlsym(h, "sqlite3_busy_timeout");
    *(void **)&_bsql.close_v2 = dlsym(h, "sqlite3_close_v2");
    *(void **)&_bsql.set_authorizer = dlsym(h, "sqlite3_set_authorizer");
    *(void **)&_bsql.db_config = dlsym(h, "sqlite3_db_config");
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
    if (!_bsql.open_v2 || !_bsql.set_authorizer ||
        !_bsql.busy_timeout || !_bsql.close_v2 || !_bsql.prepare_v2 ||
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

static int _bsql_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int _bsql_confined_auth(void *ud, int action, const char *p1,
                               const char *p2, const char *dbn, const char *trig) {
    (void)ud; (void)p2; (void)dbn; (void)trig;
    /* 24 = SQLITE_ATTACH (also covers VACUUM INTO), 25 = SQLITE_DETACH,
     * 31 = SQLITE_FUNCTION load_extension, 19 = SQLITE_PRAGMA. */
    if (action == 24 || action == 25) return 1; /* SQLITE_DENY */
    if (action == 19 && p1) {
        if (_bsql_ieq(p1, "temp_store_directory") ||
            _bsql_ieq(p1, "data_store_directory") ||
            _bsql_ieq(p1, "writable_schema") ||
            _bsql_ieq(p1, "trusted_schema"))
            return 1; /* SQLITE_DENY */
    }
    if (action == 31 && p1 && _bsql_ieq(p1, "load_extension")) return 1;
    return 0; /* SQLITE_OK */
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
    char leaf[1024];
    int dfd = -1, tfd = -1;
    struct stat tst;
    memset(&tst, 0, sizeof tst);
    int confined = !_vfiles_free();
    if (strcmp(raw, ":memory:") != 0) {
        const char *rp = vconfine(raw);
        if (!rp) return verr("path outside the program directory");
        snprintf(conf_buf, sizeof conf_buf, "%s", rp);
        if (confined) {
            int esc = 0;
            dfd = _vopen_parent_confined(conf_buf, leaf, sizeof leaf, &esc);
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
            tfd = openat(dfd, leaf, fl, 0666);
            int te = errno;
            if (tfd < 0) {
                close(dfd);
                if (te == ELOOP) return verr("path escaped during open");
                errno = te;
                return verr_os(conf_buf);
            }
            if (!_vfd_confined(tfd) || fstat(tfd, &tst) != 0 || !S_ISREG(tst.st_mode)) {
                close(tfd);
                close(dfd);
                return verr("path escaped during open");
            }
        }
        use_path = conf_buf;
    }
    if (!_bsql_init()) {
        if (tfd >= 0) close(tfd);
        if (dfd >= 0) close(dfd);
        return verr("libsqlite3 unavailable");
    }
    void *db = NULL;
    /* 0x00000002 = READWRITE, 0x00000004 = CREATE, 0x00008000 = NOMUTEX,
     * 0x01000000 = NOFOLLOW (never pass SQLITE_OPEN_URI). */
    int sflags = 0x00000002 | 0x00000004 | 0x00008000 | (confined ? 0x01000000 : 0);
    int orc = _bsql.open_v2(use_path, &db, sflags, NULL);
    if (orc != 0 || !db) {
        const char *em = db ? _bsql.errmsg(db) : "sqlite3_open_v2 failed";
        Val r = (confined && em && strstr(em, "symbolic link"))
                    ? verr("path escaped during open")
                    : verr(em);
        if (db) _bsql.close_v2(db);
        if (tfd >= 0) close(tfd);
        if (dfd >= 0) close(dfd);
        return r;
    }
    if (_bsql.db_config) {
        int ignored = 0;
        _bsql.db_config(db, 1005 /* ENABLE_LOAD_EXTENSION */, 0, &ignored);
        _bsql.db_config(db, 1010 /* DEFENSIVE */, 1, &ignored);
        _bsql.db_config(db, 1017 /* TRUSTED_SCHEMA */, 0, &ignored);
    }
    if (confined && _bsql.set_authorizer(db, _bsql_confined_auth, NULL) != 0) {
        Val r = verr(_bsql.errmsg(db));
        _bsql.close_v2(db);
        if (tfd >= 0) close(tfd);
        if (dfd >= 0) close(dfd);
        return r;
    }
    if (_bsql.busy_timeout(db, 5000) != 0) {
        Val r = verr(_bsql.errmsg(db));
        _bsql.close_v2(db);
        if (tfd >= 0) close(tfd);
        if (dfd >= 0) close(dfd);
        return r;
    }
    if (strcmp(use_path, ":memory:") != 0 && confined) {
        struct stat pst, ust;
        int ok_post = _vfd_confined(dfd) && _vfd_confined(tfd) &&
                      fstatat(dfd, leaf, &pst, AT_SYMLINK_NOFOLLOW) == 0 &&
                      S_ISREG(pst.st_mode) &&
                      pst.st_dev == tst.st_dev && pst.st_ino == tst.st_ino &&
                      lstat(use_path, &ust) == 0 &&
                      S_ISREG(ust.st_mode) &&
                      ust.st_dev == tst.st_dev && ust.st_ino == tst.st_ino;
        static const char *sfx2[] = {"-wal", "-journal", "-shm"};
        for (int k = 0; ok_post && k < 3; k++) {
            char sc[1040];
            snprintf(sc, sizeof sc, "%s%s", leaf, sfx2[k]);
            struct stat sst;
            if (fstatat(dfd, sc, &sst, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(sst.st_mode))
                ok_post = 0;
        }
        close(tfd);
        close(dfd);
        if (!ok_post) {
            _bsql.close_v2(db);
            return verr("path escaped during open");
        }
    } else {
        if (tfd >= 0) close(tfd);
        if (dfd >= 0) close(dfd);
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
        if (i >= 0x7ffffffe) {
            _bsql.finalize(stmt);
            _vsql_release(slot);
            return verr("too many sqlite parameters");
        }
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
                if (!cname) cname = "";
                size_t cnlen = strlen(cname);
                if (!utf8_valid(cname, (bint)cnlen)) {
                    vrc_dec(vstruct(row));
                    _bsql.finalize(stmt);
                    _vsql_release(slot);
                    vrc_dec(vvecv(rows));
                    return verr("invalid utf-8 in sqlite column name");
                }
                if (cnlen == 5 && memcmp(cname, "$type", 5) == 0) {
                    vrc_dec(vstruct(row));
                    _bsql.finalize(stmt);
                    _vsql_release(slot);
                    vrc_dec(vvecv(rows));
                    return verr("map key \"$type\" is reserved");
                }
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
                map_set_v(row, cnlen ? vstrn(cname, (bint)cnlen) : vstr_sso("", 0), cv);
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
#endif /* BRAT_CRT_SQL */

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

#if (BRAT_CRT_MASK & BRAT_CRT_PROC)
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

BFN bint _vproc_default_timeout_ms(void) {
    const char *ev = getenv("BRAT_PROC_TIMEOUT_MS");
    if (ev && *ev) {
        char *end = NULL;
        long long v = strtoll(ev, &end, 10);
        if (end != ev && *end == 0 && v > 0 && v <= 3600000LL) return (bint)v;
    }
    return 60000;
}

BFN uint64_t _vproc_mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

BFN char *_vbytes_to_cstr(Vec *v) {
    char *s = (char *)malloc((size_t)v->len + 1);
    if (!s) return NULL;
    for (bint i = 0; i < v->len; i++) s[i] = (char)v->items[i].as.ival;
    s[v->len] = 0;
    return s;
}

#if !defined(_WIN32)
static int _vpipe_cloexec(int p[2]) {
    if (pipe(p) != 0) return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
}
#endif

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
    if (_vpipe_cloexec(out_p) != 0) return verr_os("pipe");
    if (_vpipe_cloexec(err_p) != 0) { close(out_p[0]); close(out_p[1]); return verr_os("pipe"); }
    if (_vpipe_cloexec(ex_p) != 0) {
        close(out_p[0]); close(out_p[1]); close(err_p[0]); close(err_p[1]);
        return verr_os("pipe");
    }
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
#if defined(SIGPIPE)
        signal(SIGPIPE, SIG_DFL);
#endif
        close(out_p[0]);
        close(err_p[0]);
        close(ex_p[0]);
        int *wpipes[3] = {&out_p[1], &err_p[1], &ex_p[1]};
        for (int wi = 0; wi < 3; wi++) {
            if (*wpipes[wi] >= 0 && *wpipes[wi] < 3) {
                int m = fcntl(*wpipes[wi], F_DUPFD_CLOEXEC, 3);
                if (m < 0) _exit(126);
                close(*wpipes[wi]);
                *wpipes[wi] = m;
            }
        }
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            if (devnull == STDIN_FILENO) fcntl(STDIN_FILENO, F_SETFD, 0);
            else { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        if (dup2(out_p[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(err_p[1], STDERR_FILENO) < 0) _exit(126);
        close(out_p[1]);
        close(err_p[1]);
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
#endif /* BRAT_CRT_PROC */
/* Raise `fn: strerror(errno)`. Format before raising: vraise allocates and may clobber errno. */
BFN void vraise_errno(const char *fn) {
    char m[256];
    snprintf(m, sizeof m, "%s: %s", fn, strerror(errno));
    vraise(m);
}

BFN_HOT Val vstr_of(Val v) {
    if (v.tag == V_STR) return vrc_inc(v);
    if (v.tag == V_INT) {
        uint64_t mag = (v.as.ival < 0) ? (0ULL - (uint64_t)v.as.ival) : (uint64_t)v.as.ival;
        char tmp[24];
        int pos = 24;
        do {
            tmp[--pos] = (char)('0' + (mag % 10ULL));
            mag /= 10ULL;
        } while (mag != 0ULL);
        if (v.as.ival < 0) tmp[--pos] = '-';
        int len = 24 - pos;
        if (len <= BRAT_SSO_MAX) return vstr_sso(tmp + pos, (size_t)len);
        return vstrn(tmp + pos, (bint)len);
    }
    if (v.tag == V_BOOL) return v.as.b ? vstr_sso("true", 4) : vstr_sso("false", 5);
    if (v.tag == V_NIL) return vstr_sso("None", 4);
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
   characters (ASCII whitespace plus Unicode Zs / line / paragraph separators). */
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
    if (s.tag != V_STR) { vraise("no method split"); return vunwind; }
    if (sep.tag != V_STR) { vraise("method `split`: bad argument"); return vunwind; }
    size_t sl = vstr_len_fast(&s), dl = vstr_len_fast(&sep);
    if (!dl) { vraise("empty separator"); return vunwind; }
    Vec *out = vvec();
    const char *p = vstr_c(&s);
    const char *end = p + sl;
    const char *seps = vstr_c(&sep);
    const char *q;
    while ((q = _bmemmem(p, (size_t)(end - p), seps, dl)) != NULL) {
        vec_push(out, vstrn(p, q - p));
        p = q + dl;
    }
    vec_push(out, vstrn(p, end - p));
    return vvecv(out);
}
BFN Val vtrim(Val s) {
    if (s.tag != V_STR) { vraise("no method trim"); return vunwind; }
    const char *p = vstr_c(&s);
    const char *end = p + vstr_len_fast(&s);
    while (p < end && vis_space(*p)) p++;
    bint n = (bint)(end - p);
    while (n > 0 && vis_space(p[n - 1])) n--;
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
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
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
#if (BRAT_CRT_MASK & BRAT_CRT_CASE)
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
#endif /* BRAT_CRT_CASE */

#if (BRAT_CRT_MASK & BRAT_CRT_JSON)
typedef struct {
    const unsigned char *s;
    size_t len;
    size_t pos;
    char err[160];
} _BJsonIn;

static void _bjson_err(_BJsonIn *in, size_t at, const char *msg) {
    if (!in->err[0])
        snprintf(in->err, sizeof in->err, "%s at byte " BINT_FMT, msg, (bint)at);
}

static void _bjson_ws(_BJsonIn *in) {
    while (in->pos < in->len) {
        unsigned char c = in->s[in->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in->pos++;
        else break;
    }
}

static int _bjson_hex4(const unsigned char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static int _bjson_parse_str(_BJsonIn *in, Val *out) {
    size_t start_quote = in->pos++;
    size_t raw_start = in->pos;
    int has_esc = 0;
    while (in->pos < in->len) {
        unsigned char c = in->s[in->pos];
        if (c == '"') break;
        if (c < 0x20u) {
            _bjson_err(in, in->pos, "control character in string");
            return 0;
        }
        if (c == '\\') {
            has_esc = 1;
            in->pos++;
            if (in->pos >= in->len) {
                _bjson_err(in, start_quote, "unterminated string");
                return 0;
            }
        }
        in->pos++;
    }
    if (in->pos >= in->len || in->s[in->pos] != '"') {
        _bjson_err(in, start_quote, "unterminated string");
        return 0;
    }
    if (!has_esc) {
        size_t slen = in->pos - raw_start;
        in->pos++;
        *out = vstrn((const char *)(in->s + raw_start), (bint)slen);
        return 1;
    }
    size_t cap = (in->pos - raw_start) + 8;
    char *buf = (char *)malloc(cap);
    if (!buf) vfatal_oom();
    size_t blen = 0;
    size_t p = raw_start;
    size_t end = in->pos;
    while (p < end) {
        unsigned char c = in->s[p++];
        if (blen + 8 >= cap) {
            cap = cap * 2 + 16;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); vfatal_oom(); }
            buf = nb;
        }
        if (c != '\\') {
            buf[blen++] = (char)c;
            continue;
        }
        size_t esc_at = p - 1;
        unsigned char e = in->s[p++];
        switch (e) {
        case '"': buf[blen++] = '"'; break;
        case '\\': buf[blen++] = '\\'; break;
        case '/': buf[blen++] = '/'; break;
        case 'b': buf[blen++] = '\b'; break;
        case 'f': buf[blen++] = '\f'; break;
        case 'n': buf[blen++] = '\n'; break;
        case 'r': buf[blen++] = '\r'; break;
        case 't': buf[blen++] = '\t'; break;
        case 'u': {
            unsigned hi = 0;
            if (p + 4 > end || !_bjson_hex4(in->s + p, &hi)) {
                free(buf);
                _bjson_err(in, esc_at, "bad \\uXXXX escape");
                return 0;
            }
            p += 4;
            if (hi == 0) {
                free(buf);
                _bjson_err(in, esc_at, "embedded NUL is not allowed in BRAT str");
                return 0;
            }
            if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                free(buf);
                _bjson_err(in, esc_at, "lone UTF-16 low surrogate");
                return 0;
            }
            unsigned cp = hi;
            if (hi >= 0xD800u && hi <= 0xDBFFu) {
                unsigned lo = 0;
                if (p + 6 > end || in->s[p] != '\\' || in->s[p + 1] != 'u' ||
                    !_bjson_hex4(in->s + p + 2, &lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                    free(buf);
                    _bjson_err(in, esc_at, "unpaired UTF-16 high surrogate");
                    return 0;
                }
                p += 6;
                cp = 0x10000u + (((hi - 0xD800u) << 10) | (lo - 0xDC00u));
            }
            blen += (size_t)vcp_put(cp, buf + blen);
            break;
        }
        default:
            free(buf);
            _bjson_err(in, esc_at, "bad string escape");
            return 0;
        }
    }
    in->pos = end + 1;
    *out = vstrn(buf, (bint)blen);
    free(buf);
    return 1;
}

static int _bjson_parse_num(_BJsonIn *in, Val *out) {
    size_t start = in->pos;
    int neg = 0;
    if (in->s[in->pos] == '-') {
        neg = 1;
        in->pos++;
        if (in->pos >= in->len) { _bjson_err(in, start, "bad number"); return 0; }
    }
    if (in->s[in->pos] == '0') {
        in->pos++;
        if (in->pos < in->len && in->s[in->pos] >= '0' && in->s[in->pos] <= '9') {
            _bjson_err(in, start, "leading zero in number");
            return 0;
        }
    } else if (in->s[in->pos] >= '1' && in->s[in->pos] <= '9') {
        while (in->pos < in->len && in->s[in->pos] >= '0' && in->s[in->pos] <= '9')
            in->pos++;
    } else {
        _bjson_err(in, start, "bad number");
        return 0;
    }
    int is_flt = 0;
    if (in->pos < in->len && in->s[in->pos] == '.') {
        is_flt = 1;
        in->pos++;
        if (in->pos >= in->len || in->s[in->pos] < '0' || in->s[in->pos] > '9') {
            _bjson_err(in, start, "bad fraction in number");
            return 0;
        }
        while (in->pos < in->len && in->s[in->pos] >= '0' && in->s[in->pos] <= '9')
            in->pos++;
    }
    if (in->pos < in->len && (in->s[in->pos] == 'e' || in->s[in->pos] == 'E')) {
        is_flt = 1;
        in->pos++;
        if (in->pos < in->len && (in->s[in->pos] == '+' || in->s[in->pos] == '-'))
            in->pos++;
        if (in->pos >= in->len || in->s[in->pos] < '0' || in->s[in->pos] > '9') {
            _bjson_err(in, start, "bad exponent in number");
            return 0;
        }
        while (in->pos < in->len && in->s[in->pos] >= '0' && in->s[in->pos] <= '9')
            in->pos++;
    }
    size_t nlen = in->pos - start;
    if (!is_flt) {
        uint64_t lim = neg ? ((uint64_t)9223372036854775807ULL + 1ULL) : 9223372036854775807ULL;
        uint64_t acc = 0;
        size_t i = start + (neg ? 1u : 0u);
        while (i < in->pos) {
            unsigned d = (unsigned)(in->s[i++] - '0');
            if (acc > (lim - d) / 10ULL) {
                _bjson_err(in, start, "integer overflow");
                return 0;
            }
            acc = acc * 10ULL + d;
        }
        bint iv = neg ? (acc == lim ? (-9223372036854775807LL - 1LL) : -(bint)acc) : (bint)acc;
        *out = vint(iv);
        return 1;
    }
    int ok = 0;
    double dv = _bstrtod_c((const char *)in->s + start, nlen, &ok);
    ok = ok && isfinite(dv);
    if (!ok) {
        _bjson_err(in, start, "number out of f64 range");
        return 0;
    }
    *out = vfloat(dv);
    return 1;
}

static int _bjson_parse_val(_BJsonIn *in, bint depth, Val *out) {
    if (depth >= BRAT_MAX_VALUE_DEPTH) {
        _bjson_err(in, in->pos, "nesting limit exceeded");
        return 0;
    }
    _bjson_ws(in);
    if (in->pos >= in->len) {
        _bjson_err(in, in->pos, "unexpected end of input");
        return 0;
    }
    unsigned char c = in->s[in->pos];
    if (c == '"') return _bjson_parse_str(in, out);
    if (c == '-' || (c >= '0' && c <= '9')) return _bjson_parse_num(in, out);
    if (c == 't') {
        if (in->pos + 4 <= in->len && memcmp(in->s + in->pos, "true", 4) == 0) {
            in->pos += 4;
            *out = vbool(1);
            return 1;
        }
        _bjson_err(in, in->pos, "expected 'true'");
        return 0;
    }
    if (c == 'f') {
        if (in->pos + 5 <= in->len && memcmp(in->s + in->pos, "false", 5) == 0) {
            in->pos += 5;
            *out = vbool(0);
            return 1;
        }
        _bjson_err(in, in->pos, "expected 'false'");
        return 0;
    }
    if (c == 'n') {
        if (in->pos + 4 <= in->len && memcmp(in->s + in->pos, "null", 4) == 0) {
            in->pos += 4;
            *out = vnil();
            return 1;
        }
        _bjson_err(in, in->pos, "expected 'null'");
        return 0;
    }
    if (c == '[') {
        size_t open_at = in->pos++;
        Vec *arr = vvec();
        _bjson_ws(in);
        if (in->pos < in->len && in->s[in->pos] == ']') {
            in->pos++;
            *out = vvecv(arr);
            return 1;
        }
        for (;;) {
            Val item;
            if (!_bjson_parse_val(in, depth + 1, &item)) {
                vrc_dec(vvecv(arr));
                return 0;
            }
            vec_push(arr, item);
            _bjson_ws(in);
            if (in->pos >= in->len) {
                vrc_dec(vvecv(arr));
                _bjson_err(in, open_at, "unterminated array");
                return 0;
            }
            if (in->s[in->pos] == ']') {
                in->pos++;
                *out = vvecv(arr);
                return 1;
            }
            if (in->s[in->pos] != ',') {
                vrc_dec(vvecv(arr));
                _bjson_err(in, in->pos, "expected ',' or ']' in array");
                return 0;
            }
            in->pos++;
        }
    }
    if (c == '{') {
        size_t open_at = in->pos++;
        Map *obj = vmap("");
        _bjson_ws(in);
        if (in->pos < in->len && in->s[in->pos] == '}') {
            in->pos++;
            *out = vstruct(obj);
            return 1;
        }
        for (;;) {
            _bjson_ws(in);
            if (in->pos >= in->len || in->s[in->pos] != '"') {
                vrc_dec(vstruct(obj));
                _bjson_err(in, in->pos < in->len ? in->pos : open_at, "expected string key");
                return 0;
            }
            size_t key_at = in->pos;
            Val key;
            if (!_bjson_parse_str(in, &key)) {
                vrc_dec(vstruct(obj));
                return 0;
            }
            if (vmapkey_reserved(key)) {
                vrc_dec(key);
                vrc_dec(vstruct(obj));
                _bjson_err(in, key_at, "map key \"$type\" is reserved");
                return 0;
            }
            _bjson_ws(in);
            if (in->pos >= in->len || in->s[in->pos] != ':') {
                vrc_dec(key);
                vrc_dec(vstruct(obj));
                _bjson_err(in, in->pos, "expected ':' after object key");
                return 0;
            }
            in->pos++;
            Val val;
            if (!_bjson_parse_val(in, depth + 1, &val)) {
                vrc_dec(key);
                vrc_dec(vstruct(obj));
                return 0;
            }
            map_set_v(obj, key, val);
            _bjson_ws(in);
            if (in->pos >= in->len) {
                vrc_dec(vstruct(obj));
                _bjson_err(in, open_at, "unterminated object");
                return 0;
            }
            if (in->s[in->pos] == '}') {
                in->pos++;
                *out = vstruct(obj);
                return 1;
            }
            if (in->s[in->pos] != ',') {
                vrc_dec(vstruct(obj));
                _bjson_err(in, in->pos, "expected ',' or '}' in object");
                return 0;
            }
            in->pos++;
        }
    }
    _bjson_err(in, in->pos, "unexpected character");
    return 0;
}

BFN Val vjson_parse(Val s) {
    if (s.tag != V_STR) return _verr_c("json_parse wants str");
    _BJsonIn in;
    in.s = (const unsigned char *)vstr_c(&s);
    in.len = vstr_len_fast(&s);
    in.pos = 0;
    in.err[0] = 0;
    Val root;
    if (!_bjson_parse_val(&in, 0, &root))
        return _verr_c(in.err[0] ? in.err : "invalid JSON");
    _bjson_ws(&in);
    if (in.pos != in.len) {
        vrc_dec(root);
        _bjson_err(&in, in.pos, "trailing tokens");
        return _verr_c(in.err);
    }
    Vec *a = vvec();
    vec_push(a, root);
    return vtagv("ok", a);
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} _BJsonOut;

static void _bjson_put(_BJsonOut *o, const char *s, size_t n) {
    if (!n) return;
    if (n > BRAT_MAX_STR - o->len) { free(o->buf); o->buf = NULL; vraise("out of memory"); return; }
    if (o->len + n + 1 > o->cap) {
        size_t need = o->len + n + 1;
        size_t ncap = o->cap * 2;
        if (ncap < need) ncap = need + 64;
        char *nb = (char *)realloc(o->buf, ncap);
        if (!nb) { free(o->buf); o->buf = NULL; vfatal_oom(); }
        o->buf = nb;
        o->cap = ncap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void _bjson_spaces(_BJsonOut *o, bint count) {
    static const char sp32[32] = "                                ";
    while (count > 0 && !verr_pending()) {
        size_t chunk = count > 32 ? 32u : (size_t)count;
        _bjson_put(o, sp32, chunk);
        count -= (bint)chunk;
    }
}

static void _bjson_quote(_BJsonOut *o, const char *s, size_t n) {
    static const char hex[16] = "0123456789abcdef";
    _bjson_put(o, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n && !verr_pending(); i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char uesc[6];
        size_t elen = 0;
        if (c == '"') { esc = "\\\""; elen = 2; }
        else if (c == '\\') { esc = "\\\\"; elen = 2; }
        else if (c == '\b') { esc = "\\b"; elen = 2; }
        else if (c == '\f') { esc = "\\f"; elen = 2; }
        else if (c == '\n') { esc = "\\n"; elen = 2; }
        else if (c == '\r') { esc = "\\r"; elen = 2; }
        else if (c == '\t') { esc = "\\t"; elen = 2; }
        else if (c < 0x20u) {
            uesc[0] = '\\'; uesc[1] = 'u'; uesc[2] = '0'; uesc[3] = '0';
            uesc[4] = hex[(c >> 4) & 0xFu]; uesc[5] = hex[c & 0xFu];
            esc = uesc; elen = 6;
        }
        if (esc) {
            if (i > run) _bjson_put(o, s + run, i - run);
            _bjson_put(o, esc, elen);
            run = i + 1;
        }
    }
    if (n > run && !verr_pending()) _bjson_put(o, s + run, n - run);
    if (!verr_pending()) _bjson_put(o, "\"", 1);
}

typedef struct {
    Val kstr;
    bint idx;
} _BJsonKeyEnt;

static int _bjson_key_cmp(const void *pa, const void *pb) {
    const _BJsonKeyEnt *a = (const _BJsonKeyEnt *)pa;
    const _BJsonKeyEnt *b = (const _BJsonKeyEnt *)pb;
    return strcmp(vstr_c(&a->kstr), vstr_c(&b->kstr));
}

static void _bjson_emit(_BJsonOut *o, Val v, bint indent, bint depth) {
    if (verr_pending()) return;
    if (depth >= BRAT_MAX_VALUE_DEPTH) {
        free(o->buf); o->buf = NULL;
        vraise("recursion too deep");
        return;
    }
    switch (v.tag) {
    case V_NIL:
        _bjson_put(o, "null", 4);
        return;
    case V_BOOL:
        _bjson_put(o, v.as.b ? "true" : "false", v.as.b ? 4u : 5u);
        return;
    case V_INT: {
        char tmp[32];
        int ln = snprintf(tmp, sizeof tmp, BINT_FMT, (bint)v.as.ival);
        _bjson_put(o, tmp, (size_t)ln);
        return;
    }
    case V_FLOAT: {
        if (!isfinite(v.as.fval)) {
            _bjson_put(o, "null", 4);
            return;
        }
        const char *fs = vfmt_double(v.as.fval);
        _bjson_put(o, fs, strlen(fs));
        return;
    }
    case V_STR:
        _bjson_quote(o, vstr_c(&v), vstr_len_fast(&v));
        return;
    case V_VEC: {
        Vec *w = v.as.vec;
        if (w->len == 0) {
            _bjson_put(o, "[]", 2);
            return;
        }
        if (indent <= 0) {
            _bjson_put(o, "[", 1);
            for (bint i = 0; i < w->len && !verr_pending(); i++) {
                if (i) _bjson_put(o, ",", 1);
                _bjson_emit(o, w->items[i], 0, depth + 1);
            }
            if (!verr_pending()) _bjson_put(o, "]", 1);
            return;
        }
        _bjson_put(o, "[\n", 2);
        for (bint i = 0; i < w->len && !verr_pending(); i++) {
            if (i) _bjson_put(o, ",\n", 2);
            _bjson_spaces(o, (depth + 1) * indent);
            _bjson_emit(o, w->items[i], indent, depth + 1);
        }
        if (!verr_pending()) {
            _bjson_put(o, "\n", 1);
            _bjson_spaces(o, depth * indent);
            _bjson_put(o, "]", 1);
        }
        return;
    }
    case V_STRUCT: {
        Map *m = v.as.map;
        _BJsonKeyEnt sbuf[16];
        _BJsonKeyEnt *ents = m->used <= 16 ? sbuf : (_BJsonKeyEnt *)malloc(sizeof(_BJsonKeyEnt) * (size_t)m->used);
        if (!ents) vfatal_oom();
        size_t cnt = 0;
        for (bint i = 0; i < m->used; i++) {
            Val k = m->keys[i];
            if (k.tag == V_MOVED || vmapkey_reserved(k)) continue;
            Val ks = (k.tag == V_STR) ? vrc_inc(k) : vstr_of(k);
            if (verr_pending()) {
                for (size_t j = 0; j < cnt; j++) vrc_dec(ents[j].kstr);
                if (ents != sbuf) free(ents);
                return;
            }
            ents[cnt].kstr = ks;
            ents[cnt].idx = i;
            cnt++;
        }
        if (cnt == 0) {
            if (ents != sbuf) free(ents);
            _bjson_put(o, "{}", 2);
            return;
        }
        if (cnt > 1) qsort(ents, cnt, sizeof(_BJsonKeyEnt), _bjson_key_cmp);
        if (indent <= 0) {
            _bjson_put(o, "{", 1);
            for (size_t i = 0; i < cnt && !verr_pending(); i++) {
                if (i) _bjson_put(o, ",", 1);
                _bjson_quote(o, vstr_c(&ents[i].kstr), vstr_len_fast(&ents[i].kstr));
                _bjson_put(o, ":", 1);
                _bjson_emit(o, m->vals[ents[i].idx], 0, depth + 1);
            }
            if (!verr_pending()) _bjson_put(o, "}", 1);
        } else {
            _bjson_put(o, "{\n", 2);
            for (size_t i = 0; i < cnt && !verr_pending(); i++) {
                if (i) _bjson_put(o, ",\n", 2);
                _bjson_spaces(o, (depth + 1) * indent);
                _bjson_quote(o, vstr_c(&ents[i].kstr), vstr_len_fast(&ents[i].kstr));
                _bjson_put(o, ": ", 2);
                _bjson_emit(o, m->vals[ents[i].idx], indent, depth + 1);
            }
            if (!verr_pending()) {
                _bjson_put(o, "\n", 1);
                _bjson_spaces(o, depth * indent);
                _bjson_put(o, "}", 1);
            }
        }
        for (size_t i = 0; i < cnt; i++) vrc_dec(ents[i].kstr);
        if (ents != sbuf) free(ents);
        return;
    }
    default: {
        Val sv = vstr_of(v);
        if (verr_pending()) return;
        _bjson_quote(o, vstr_c(&sv), vstr_len_fast(&sv));
        vrc_dec(sv);
        return;
    }
    }
}

BFN Val vjson_stringify(Val v, Val indent_v) {
    bint indent = (indent_v.tag == V_INT) ? indent_v.as.ival : 0;
    if (indent > 64) indent = 64;
    _BJsonOut o;
    o.cap = 128;
    o.len = 0;
    o.buf = (char *)malloc(o.cap);
    if (!o.buf) vfatal_oom();
    _bjson_emit(&o, v, indent, 0);
    if (verr_pending()) {
        free(o.buf);
        return vunwind;
    }
    Val res = vstrn(o.buf, (bint)o.len);
    free(o.buf);
    return res;
}
#endif /* BRAT_CRT_JSON */
