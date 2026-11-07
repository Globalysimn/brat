#include "session.h"

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace {

using View = std::vector<const Stmt *>;

const std::set<std::string> kOps{"send", "recv", "close"};
const std::set<SK> kDecls{SK::Fn, SK::Impl, SK::Trait, SK::Enum, SK::Struct, SK::Extern,
                          SK::ExternStruct, SK::ExportFn};

std::string need_send(const std::string &name, int recv, int send) {
    return "`" + name + "`: " + std::to_string(recv) + " receive(s) but only " +
           std::to_string(send) + " send(s) — `recv` waits forever (hint: send as many " +
           "values as you receive, or `close` the channel)";
}

std::string need_recv(const std::string &name, int send, int cap, int recv) {
    return "`" + name + "`: " + std::to_string(send) + " value(s) sent into a queue of " +
           std::to_string(cap) + " and only " + std::to_string(recv) +
           " received — `send` blocks once the queue is full (hint: receive more, or "
           "make the channel larger)";
}

std::string kind_of(const Expr *e) {
    if (e == nullptr) return "any";
    switch (e->k) {
    case EK::Num: return e->is_float ? "f64" : "i64";
    case EK::Str: return "str";
    case EK::RStr: return "str";
    case EK::Bool: return "bool";
    case EK::VecLit: return "vec";
    case EK::MapLit: return "map";
    case EK::None: return "none";
    case EK::Call:
        if (e->a == "chan") return "chan";
        return "any";
    default: return "any";
    }
}

View view(const std::vector<StmtP> &body) {
    View out;
    out.reserve(body.size());
    for (const auto &s : body) out.push_back(s.get());
    return out;
}

View else_of(const Stmt &s) {
    if (!s.els) return {};
    if (s.els->k == SK::Block) return view(s.els->body);
    return {s.els.get()};
}

const std::string kMain = "main";
const std::string kChild = "child";

struct Event {
    std::string side;
    std::string op;
    std::string kind;
    std::string quant;
};

struct Chan {
    std::string name;
    std::optional<int> cap;             // a literal capacity, else nothing to prove
    std::vector<Event> events;
    std::vector<int> event_lines;       // one per event, 0 when unknown
    std::vector<int> lines;
    bool escaped = false;

    void add(const std::string &side, const std::string &op, const std::string &kind,
             const std::string &quant, int line) {
        events.push_back({side, op, kind, quant});
        event_lines.push_back(line);
        if (line) lines.push_back(line);
    }

    // The line of the send that finds the queue full, or 0 when no prefix can
    // block. A straight-line pipe in one scope runs its operations in source
    // order, so a send arriving with `cap` values already waiting has nobody to
    // hand its value to: it blocks. Crediting a later receive -- the old rule
    // was `send <= cap + recv` -- let `chan(1)` with two sends and one receive
    // pass the checker and then hang, in both backends, forever. Two sides mean
    // a `spawn`, and a spawn orders nothing: no prefix, nothing said.
    int first_blocking_send() const {
        if (!cap || events.empty() || !known()) return 0;
        const std::string &first = events.front().side;
        for (const auto &e : events)
            if (e.side != first) return 0;
        int waiting = 0;
        for (size_t k = 0; k < events.size(); ++k) {
            const Event &e = events[k];
            if (e.op == "send") {
                if (waiting >= *cap) return event_lines[k] ? event_lines[k] : (lines.empty() ? 0 : lines.back());
                ++waiting;
            } else if (e.op == "recv") {
                --waiting;
            }
        }
        return 0;
    }

    // Exact only with no loop, no branch and no escape: otherwise counting is a guess.
    bool closed() const {
        for (const auto &e : events)
            if (e.op == "close") return true;
        return false;
    }

    bool known() const {
        if (escaped) return false;
        for (const auto &e : events)
            if (e.quant != "1") return false;
        return true;
    }

    void counted(const std::string &op, int &n, bool &exact) const {
        n = 0;
        exact = true;
        for (const auto &e : events) {
            if (e.op != op) continue;
            if (e.quant == "1") {
                n++;
            } else {
                exact = false;
            }
        }
    }

    std::string payload() const {
        std::set<std::string> kinds;
        for (const auto &e : events)
            if (e.op == "send" && e.kind != "any") kinds.insert(e.kind);
        return kinds.size() == 1 ? *kinds.begin() : "any";
    }
};

using Chans = std::map<std::string, Chan>;

void collect_expr(const Expr *e, Chans &chans, const std::string &side,
                  const std::string &quant, int line);

// `send` on a closed channel returns an `err`, `close` twice is idempotent, so
// ordering inside one scope is not a refusal — only the counts are.
void collect(const View &stmts, Chans &chans, const std::string &side,
             const std::string &quant) {
    for (const Stmt *s : stmts) {
        switch (s->k) {
        case SK::Let:
            if (s->e1 && s->e1->k == EK::Call && s->e1->a == "chan" &&
                !chans.count(s->a)) {
                Chan c;
                c.name = s->a;
                if (!s->e1->kids.empty() && s->e1->kids[0]->k == EK::Num &&
                    !s->e1->kids[0]->is_float && s->e1->kids[0]->i >= 1 &&
                    s->e1->kids[0]->i <= 1048576) {
                    c.cap = static_cast<int>(s->e1->kids[0]->i);
                }
                chans[s->a] = c;
            }
            collect_expr(s->e1.get(), chans, side, quant, s->line);
            continue;
        case SK::Expr:
        case SK::Return:
        case SK::Defer:
        case SK::Set:
        case SK::SetIdx:
        case SK::SetField:
            collect_expr(s->e1.get(), chans, side, quant, s->line);
            collect_expr(s->e2.get(), chans, side, quant, s->line);
            collect_expr(s->e3.get(), chans, side, quant, s->line);
            continue;
        case SK::Spawn:
            collect(view(s->body), chans, kChild, quant);
            continue;
        case SK::If:
            collect_expr(s->e1.get(), chans, side, quant, s->line);
            collect(view(s->body), chans, side, "|");
            collect(else_of(*s), chans, side, "|");
            continue;
        case SK::While:
        case SK::ForRange:
        case SK::ForIter:
            collect_expr(s->e1.get(), chans, side, quant, s->line);
            collect_expr(s->e2.get(), chans, side, quant, s->line);
            collect_expr(s->e3.get(), chans, side, quant, s->line);
            collect(view(s->body), chans, side, "n");
            collect(view(s->body2), chans, side, "n");
            continue;
        case SK::Try:
            collect(view(s->body), chans, side, "|");
            collect(view(s->body2), chans, side, "|");
            collect(else_of(*s), chans, side, "|");
            continue;
        case SK::Block:
        case SK::Scope:
        case SK::Unsafe:
            collect(view(s->body), chans, side, quant);
            continue;
        default:
            if (kDecls.count(s->k)) continue;   // each body is analysed on its own
            collect_expr(s->e1.get(), chans, side, quant, s->line);
            collect_expr(s->e2.get(), chans, side, quant, s->line);
            collect_expr(s->e3.get(), chans, side, quant, s->line);
            collect(view(s->body), chans, side, "|");
            collect(view(s->body2), chans, side, "|");
            continue;
        }
    }
}

void mark_escaped_expr(const Expr *e, const std::set<std::string> &shadow, Chans &chans);

void mark_escaped_stmts(const std::vector<StmtP> &stmts, std::set<std::string> shadow, Chans &chans) {
    for (const auto &sp : stmts) {
        if (!sp) continue;
        const Stmt &s = *sp;
        mark_escaped_expr(s.e1.get(), shadow, chans);
        mark_escaped_expr(s.e2.get(), shadow, chans);
        mark_escaped_expr(s.e3.get(), shadow, chans);
        if ((s.k == SK::Let || s.k == SK::LetTmp || s.k == SK::ForRange || s.k == SK::ForIter) &&
            !s.a.empty()) {
            shadow.insert(s.a);
        }
        mark_escaped_stmts(s.body, shadow, chans);
        {
            std::set<std::string> sub2 = shadow;
            if (s.k == SK::Try && s.opt1 && !s.opt1->empty()) sub2.insert(*s.opt1);
            mark_escaped_stmts(s.body2, sub2, chans);
        }
        for (const Stmt *el = s.els.get(); el != nullptr; el = el->els.get()) {
            mark_escaped_expr(el->e1.get(), shadow, chans);
            mark_escaped_stmts(el->body, shadow, chans);
        }
    }
}

void mark_escaped_expr(const Expr *e, const std::set<std::string> &shadow, Chans &chans) {
    if (!e) return;
    if (e->k == EK::Var && !shadow.count(e->a)) {
        auto it = chans.find(e->a);
        if (it != chans.end()) it->second.escaped = true;
        return;
    }
    for (const auto &k : e->kids) mark_escaped_expr(k.get(), shadow, chans);
    if (e->fn) {
        std::set<std::string> sub = shadow;
        for (const auto &p : e->fn->params) sub.insert(p.name);
        mark_escaped_stmts(e->fn->body, std::move(sub), chans);
    }
}

void collect_expr(const Expr *e, Chans &chans, const std::string &side,
                  const std::string &quant, int line) {
    if (e == nullptr) return;
    if (e->k == EK::MCall && kOps.count(e->b) && !e->kids.empty() &&
        e->kids[0]->k == EK::Var) {
        auto it = chans.find(e->kids[0]->a);
        if (it != chans.end()) {
            const Expr *payload = (e->b == "send" && e->kids.size() > 1)
                                      ? e->kids[1].get() : nullptr;
            it->second.add(side, e->b, kind_of(payload), quant, line);
            for (size_t i = 1; i < e->kids.size(); i++)
                collect_expr(e->kids[i].get(), chans, side, quant, line);
            return;
        }
    }
    if (e->k == EK::Var) {
        auto it = chans.find(e->a);
        if (it != chans.end()) it->second.escaped = true;   // `fill(ch)`, `return ch`
        return;
    }
    for (const auto &k : e->kids) collect_expr(k.get(), chans, side, quant, line);
    if (e->fn) {
        std::set<std::string> shadow;
        for (const auto &p : e->fn->params) shadow.insert(p.name);
        mark_escaped_stmts(e->fn->body, std::move(shadow), chans);
    }
}

void one_body(const View &stmts, std::vector<BorrowDiag> &out) {
    Chans chans;
    collect(stmts, chans, kMain, "1");
    for (const auto &[name, ch] : chans) {
        int send = 0, recv = 0;
        bool send_ok = true, recv_ok = true;
        ch.counted("send", send, send_ok);
        ch.counted("recv", recv, recv_ok);
        if (!(send_ok && recv_ok) || !ch.known()) continue;
        const int line = ch.lines.empty() ? 0 : ch.lines.back();
        // A send blocks only once the queue is full, and in source order the
        // question is a prefix one, not a total one.
        if (recv > send && !ch.closed()) {
            BorrowDiag d;
            d.line = line;
            d.msg = need_send(name, recv, send);
            out.push_back(d);
        }
        if (const int blocked = ch.first_blocking_send()) {
            BorrowDiag d;
            d.line = blocked;
            d.msg = need_recv(name, send, *ch.cap, recv);
            out.push_back(d);
        }
    }
}

}  // namespace

std::vector<BorrowDiag> session_check(const std::vector<StmtP> &body) {
    std::vector<BorrowDiag> out;
    View top;
    std::function<void(const std::vector<StmtP> &)> visit_top =
        [&](const std::vector<StmtP> &stmts) {
            for (const auto &sp : stmts) {
                const Stmt *node = sp.get();
                while (node->k == SK::Pub && !node->body.empty()) node = node->body[0].get();
                if (node->k == SK::Block) {
                    visit_top(node->body);
                    continue;
                }
                if ((node->k == SK::Fn || node->k == SK::ExportFn) && !node->fns.empty()) {
                    one_body(view(node->fns[0].body), out);
                    continue;
                }
                if (kDecls.count(node->k)) {
                    if (node->k == SK::Impl) {
                        for (const auto &f : node->fns) one_body(view(f.body), out);
                    }
                    continue;
                }
                top.push_back(node);
            }
        };
    visit_top(body);
    one_body(top, out);
    return out;
}
