
static void vraise_err_tag_mode(Val v, int is_exc) {
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
    _verr_pending = 1;
    _verr_exc = is_exc;
}

static void vraise_err_tag(Val v) {
    vraise_err_tag_mode(v, 1);
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
BFN Val vquest_ret(Val v) {
    if (v.tag == V_TAG && !strcmp(v.as.tv->tag, "err")) {
        vraise_err_tag_mode(v, 0);
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

BFN_HOT Val *vstruct_slot_ptr(Val s, const BShape *shp, bint slot, const char *k) {
    if (BRAT_LIKELY(s.tag == V_STRUCT && s.as.map->shape == shp))
        return &s.as.map->vals[slot];
    if (s.tag == V_STRUCT) {
        Val *o = map_get(s.as.map, k);
        if (o) return o;
    }
    vraisef("no field %s\n", k);
    return NULL;
}

BFN_HOT Val vstruct_slot_val(Val s, const BShape *shp, bint slot, const char *k) {
    if (BRAT_LIKELY(s.tag == V_STRUCT && s.as.map->shape == shp))
        return s.as.map->vals[slot];
    return vfield(s, k);
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

BFN Val _vok_int(bint n) { Vec *a = vvec(); vec_push(a, vint(n)); return vtagv("ok", a); }
BFN Val _verr_c(const char *m) { Vec *a = vvec(); vec_push(a, vstr(_verror_text(m))); return vtagv("err", a); }

#if (BRAT_CRT_MASK & BRAT_CRT_NET)
static bmutex_t _vsk_mu = BMUTEX_INIT;
static void _vsk_lock(void) { BMUTEX_LOCK(&_vsk_mu); }
static void _vsk_unlock(void) { BMUTEX_UNLOCK(&_vsk_mu); }

#ifndef _VSK_TIMEOUT_S
#define _VSK_TIMEOUT_S 30        /* connected sockets have a fixed lifetime */
#endif
#define _VRECV_MAX ((bint)1 << 20)    /* one tcp_recv returns at most 1 MiB */

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
#endif /* BRAT_CRT_NET */

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
#if (BRAT_CRT_MASK & BRAT_CRT_NET)
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
#endif /* BRAT_CRT_NET */
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

BFN Val vappendfile(Val p, Val t) {
    if (t.tag != V_STR) return verr("append_file wants string path and text");
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("append_file wants string path and text");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("append_file byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    int escaped = 0;
    FILE *f = _vopen_confined_stream(rp, 2, 0, &escaped, NULL);
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

BFN Val vfs_exists(Val p) {
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad != 0 || !rp) return vbool(0);
#if defined(_WIN32)
    wchar_t wp[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rp, -1, wp, 4096) <= 0)
        return vbool(0);
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = FALSE;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (!_vfiles_free()) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(wp, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           &sa, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) return vbool(0);
    BY_HANDLE_FILE_INFORMATION fi;
    int ok = GetFileType(h) == FILE_TYPE_DISK &&
             GetFileInformationByHandle(h, &fi) &&
             !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
             (_vfiles_free() ||
              (!(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && _vhandle_confined(h)));
    CloseHandle(h);
    return vbool(ok);
#else
    if (_vfiles_free()) {
        struct stat st;
        return vbool(stat(rp, &st) == 0 && S_ISREG(st.st_mode));
    }
    char leaf[1024];
    int esc = 0;
    int dfd = _vopen_parent_confined(rp, leaf, sizeof leaf, &esc);
    if (dfd < 0) return vbool(0);
    struct stat st;
    int ok = _vfd_confined(dfd) &&
             fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
             S_ISREG(st.st_mode);
    close(dfd);
    return vbool(ok);
#endif
}

#define _VFSTR_MAX 256
#define _VFSTR_BUF 8192
#ifndef BRAT_MAX_LINE_BYTES
#define BRAT_MAX_LINE_BYTES (16u * 1024u * 1024u)
#endif

typedef struct {
    FILE *fp;
    char *buf;
    size_t pos;
    size_t len;
    uint64_t gen;
    int active;
    int closing;
    int mu_inited;
    bmutex_t op_mu;
} BFileSlot;

static BFileSlot BRAT_UNUSED _vfstr_slots[_VFSTR_MAX];
static uint64_t _vfstr_next_gen = 1;
static bmutex_t BRAT_UNUSED _vfstr_mu = BMUTEX_INIT;
static bmutex_t BRAT_UNUSED _vstdin_mu = BMUTEX_INIT;

static BFileSlot *_vfstr_acquire(bint handle) {
    if (handle <= 0) return NULL;
    int slot = (int)((uint64_t)handle & 0xFFu);
    uint64_t gen = (uint64_t)handle >> 8;
    if (slot <= 0 || slot >= _VFSTR_MAX || gen == 0) return NULL;
    BMUTEX_LOCK(&_vfstr_mu);
    BFileSlot *s = &_vfstr_slots[slot];
    if (!s->fp || s->closing || s->gen != gen) {
        BMUTEX_UNLOCK(&_vfstr_mu);
        return NULL;
    }
    s->active++;
    BMUTEX_UNLOCK(&_vfstr_mu);
    BMUTEX_LOCK(&s->op_mu);
    return s;
}

static void _vfstr_release(BFileSlot *s) {
    BMUTEX_UNLOCK(&s->op_mu);
    FILE *to_close = NULL;
    char *to_free = NULL;
    BMUTEX_LOCK(&_vfstr_mu);
    s->active--;
    if (s->closing && s->active == 0) {
        to_close = s->fp;
        to_free = s->buf;
        s->fp = NULL;
        s->buf = NULL;
        s->pos = 0;
        s->len = 0;
        s->closing = 0;
        s->gen = 0;
    }
    BMUTEX_UNLOCK(&_vfstr_mu);
    if (to_free) free(to_free);
    if (to_close) fclose(to_close);
}

static Val _vok_line_opt(const char *data, size_t n, int is_eof) {
    if (is_eof) {
        Vec *o = vvec();
        vec_push(o, vnil());
        return vtagv("ok", o);
    }
    if (n > 0 && data[n - 1] == '\r') n--;
    if (bsize_over_bint(n) || memchr(data, 0, n) != NULL || !utf8_valid(data, (bint)n))
        return verr("invalid utf-8");
    Vec *sm = vvec();
    vec_push(sm, vstrn(data, (bint)n));
    Vec *ok = vvec();
    vec_push(ok, vtagv("some", sm));
    return vtagv("ok", ok);
}

BFN Val vfs_open(Val p) {
    char pbuf[8192];
    int pbad = 0;
    const char *rp = _vconfine_path(&p, pbuf, sizeof pbuf, &pbad);
    if (pbad == 1) return verr("fs_open wants a string path");
    if (pbad == 2) return verr("embedded NUL in file path");
    if (pbad == 4) return verr("fs_open byte out of range 0..255");
    if (!rp) return verr("path outside the program directory");
    int escaped = 0;
    FILE *f = _vopen_confined_stream(rp, 0, 0, &escaped, NULL);
    if (!f) {
        if (escaped) return verr("path escaped during open");
        return verr_os(rp);
    }
    char *rbuf = (char *)malloc(_VFSTR_BUF);
    if (!rbuf) {
        fclose(f);
        vraise("out of memory");
        return vunwind;
    }
    BMUTEX_LOCK(&_vfstr_mu);
    int slot = -1;
    for (int i = 1; i < _VFSTR_MAX; i++) {
        if (!_vfstr_slots[i].fp && !_vfstr_slots[i].closing && _vfstr_slots[i].active == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        BMUTEX_UNLOCK(&_vfstr_mu);
        free(rbuf);
        fclose(f);
        return verr("too many open file streams");
    }
    BFileSlot *s = &_vfstr_slots[slot];
    if (!s->mu_inited) {
#if !defined(_WIN32)
        pthread_mutex_init(&s->op_mu, NULL);
#else
        s->op_mu = (bmutex_t)BMUTEX_INIT;
#endif
        s->mu_inited = 1;
    }
    uint64_t gen = _vfstr_next_gen++;
    if (gen == 0 || (gen >> 55) != 0) {
        _vfstr_next_gen = 2;
        gen = 1;
    }
    s->fp = f;
    s->buf = rbuf;
    s->pos = 0;
    s->len = 0;
    s->gen = gen;
    s->active = 0;
    s->closing = 0;
    bint handle = (bint)((gen << 8) | (uint64_t)slot);
    BMUTEX_UNLOCK(&_vfstr_mu);
    return _vok_int(handle);
}

BFN Val vfs_close(Val hv) {
    if (hv.tag != V_INT) return verr("bad file handle");
    bint handle = hv.as.ival;
    if (handle <= 0) return verr("bad file handle");
    int slot = (int)((uint64_t)handle & 0xFFu);
    uint64_t gen = (uint64_t)handle >> 8;
    if (slot <= 0 || slot >= _VFSTR_MAX || gen == 0) return verr("bad file handle");
    FILE *to_close = NULL;
    char *to_free = NULL;
    BMUTEX_LOCK(&_vfstr_mu);
    BFileSlot *s = &_vfstr_slots[slot];
    if (!s->fp || s->closing || s->gen != gen) {
        BMUTEX_UNLOCK(&_vfstr_mu);
        return verr("bad file handle");
    }
    if (s->active == 0) {
        to_close = s->fp;
        to_free = s->buf;
        s->fp = NULL;
        s->buf = NULL;
        s->pos = 0;
        s->len = 0;
        s->gen = 0;
        s->closing = 0;
    } else {
        s->closing = 1;
    }
    BMUTEX_UNLOCK(&_vfstr_mu);
    if (to_free) free(to_free);
    if (to_close && fclose(to_close) != 0) return verr_os("close");
    return _vok_int(0);
}

BFN Val vfs_read_line(Val hv) {
    if (hv.tag != V_INT) return verr("bad file handle");
    BFileSlot *s = _vfstr_acquire(hv.as.ival);
    if (!s) return verr("bad file handle");

    char stack_acc[4096];
    char *acc = stack_acc;
    size_t acc_len = 0;
    size_t acc_cap = sizeof stack_acc;
    int got_any = 0;

    for (;;) {
        if (s->pos < s->len) {
            size_t avail = s->len - s->pos;
            const char *nl = (const char *)memchr(s->buf + s->pos, '\n', avail);
            if (nl) {
                size_t chunk = (size_t)(nl - (s->buf + s->pos));
                if (chunk > BRAT_MAX_LINE_BYTES || acc_len > BRAT_MAX_LINE_BYTES - chunk) {
                    s->pos = (size_t)(nl - s->buf) + 1;
                    if (acc != stack_acc) free(acc);
                    _vfstr_release(s);
                    return verr("line exceeds 16 MiB limit");
                }
                if (acc_len == 0) {
                    const char *direct = s->buf + s->pos;
                    s->pos += chunk + 1;
                    Val r = _vok_line_opt(direct, chunk, 0);
                    _vfstr_release(s);
                    return r;
                }
                if (acc_len + chunk > acc_cap) {
                    size_t ncap = acc_cap * 2;
                    while (ncap < acc_len + chunk) ncap *= 2;
                    char *nb = (acc == stack_acc) ? (char *)malloc(ncap) : (char *)realloc(acc, ncap);
                    if (!nb) {
                        if (acc != stack_acc) free(acc);
                        _vfstr_release(s);
                        vraise("out of memory");
                        return vunwind;
                    }
                    if (acc == stack_acc) memcpy(nb, stack_acc, acc_len);
                    acc = nb;
                    acc_cap = ncap;
                }
                memcpy(acc + acc_len, s->buf + s->pos, chunk);
                acc_len += chunk;
                s->pos += chunk + 1;
                Val r = _vok_line_opt(acc, acc_len, 0);
                if (acc != stack_acc) free(acc);
                _vfstr_release(s);
                return r;
            }
            if (avail > BRAT_MAX_LINE_BYTES || acc_len > BRAT_MAX_LINE_BYTES - avail) {
                s->pos = s->len;
                /* Drain up to the terminating newline so subsequent reads stay aligned. */
                for (;;) {
                    size_t rn = fread(s->buf, 1, _VFSTR_BUF, s->fp);
                    if (rn == 0) break;
                    const char *end_nl = (const char *)memchr(s->buf, '\n', rn);
                    if (end_nl) {
                        s->pos = (size_t)(end_nl - s->buf) + 1;
                        s->len = rn;
                        break;
                    }
                }
                if (acc != stack_acc) free(acc);
                _vfstr_release(s);
                return verr("line exceeds 16 MiB limit");
            }
            if (acc_len + avail > acc_cap) {
                size_t ncap = acc_cap * 2;
                while (ncap < acc_len + avail) ncap *= 2;
                char *nb = (acc == stack_acc) ? (char *)malloc(ncap) : (char *)realloc(acc, ncap);
                if (!nb) {
                    if (acc != stack_acc) free(acc);
                    _vfstr_release(s);
                    vraise("out of memory");
                    return vunwind;
                }
                if (acc == stack_acc) memcpy(nb, stack_acc, acc_len);
                acc = nb;
                acc_cap = ncap;
            }
            memcpy(acc + acc_len, s->buf + s->pos, avail);
            acc_len += avail;
            got_any = 1;
            s->pos = s->len = 0;
        }
        errno = 0;
        size_t rn = fread(s->buf, 1, _VFSTR_BUF, s->fp);
        if (rn == 0) {
            int failed = ferror(s->fp);
            int rerr = errno;
            if (failed) {
                clearerr(s->fp);
                if (acc != stack_acc) free(acc);
                _vfstr_release(s);
                errno = rerr ? rerr : EIO;
                return verr_os("read");
            }
            Val r = _vok_line_opt(acc, acc_len, !got_any && acc_len == 0);
            if (acc != stack_acc) free(acc);
            _vfstr_release(s);
            return r;
        }
        s->pos = 0;
        s->len = rn;
    }
}

BFN Val vstdin_read_line(void) {
    BMUTEX_LOCK(&_vstdin_mu);
    char stack_acc[4096];
    char *acc = stack_acc;
    size_t len = 0;
    size_t cap = sizeof stack_acc;
    int got_any = 0;
    int over_cap = 0;
    errno = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            int failed = ferror(stdin);
            int rerr = errno;
            if (failed) {
                clearerr(stdin);
                if (acc != stack_acc) free(acc);
                BMUTEX_UNLOCK(&_vstdin_mu);
                errno = rerr ? rerr : EIO;
                return verr_os("stdin");
            }
            break;
        }
        got_any = 1;
        if (c == '\n') break;
        if (over_cap) continue;
        if (len >= BRAT_MAX_LINE_BYTES) {
            over_cap = 1;
            continue;
        }
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = (acc == stack_acc) ? (char *)malloc(ncap) : (char *)realloc(acc, ncap);
            if (!nb) {
                if (acc != stack_acc) free(acc);
                BMUTEX_UNLOCK(&_vstdin_mu);
                vraise("out of memory");
                return vunwind;
            }
            if (acc == stack_acc) memcpy(nb, stack_acc, len);
            acc = nb;
            cap = ncap;
        }
        acc[len++] = (char)c;
    }
    if (over_cap) {
        if (acc != stack_acc) free(acc);
        BMUTEX_UNLOCK(&_vstdin_mu);
        return verr("line exceeds 16 MiB limit");
    }
    Val r = _vok_line_opt(acc, len, !got_any);
    if (acc != stack_acc) free(acc);
    BMUTEX_UNLOCK(&_vstdin_mu);
    return r;
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
        struct stat sst, dst;
        if (!_vfd_confined(sfd) || !_vfd_confined(dfd) ||
            (fstatat(sfd, sleaf, &sst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(sst.st_mode)) ||
            (fstatat(dfd, dleaf, &dst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(dst.st_mode))) {
            close(sfd);
            close(dfd);
            return verr("path escaped during open");
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
        struct stat lst;
        if (!_vfd_confined(dfd) ||
            (fstatat(dfd, leaf, &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode))) {
            close(dfd);
            return verr("path escaped during open");
        }
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

#if (BRAT_CRT_MASK & BRAT_CRT_TLS)
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
#endif /* BRAT_CRT_TLS */
