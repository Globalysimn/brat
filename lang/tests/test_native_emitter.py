"""Basic shape of the C written by the native emitter."""
import _cxx
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.join(HERE, "..", "examples")

n = 0


def check(cond, msg):
    global n
    assert cond, msg
    n += 1


def _read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def _balanced(s):
    return s.count("{") == s.count("}") and s.count("{") > 0


hello_src = _read(os.path.join(EXAMPLES, "hello.brat"))
control_src = _read(os.path.join(EXAMPLES, "control.brat"))
quest_src = "let x = 1?\n"

h = _cxx.emit_source(hello_src, EXAMPLES)
c = _cxx.emit_source(control_src, EXAMPLES)
q = _cxx.emit_source(quest_src, EXAMPLES)

check('#include "crt.h"' in h, "hello missing crt.h")
check("int main(int argc, char **argv)" in h, "hello missing main")
check("vprint" in h, "hello missing vprint")
check("vquest_ok(vquest" in q, "quest missing unwind")
check("#error" not in h + c + q, "emitter has #error")
check(_balanced(h), "hello braces unbalanced")
check(_balanced(c), "control braces unbalanced")
check(_balanced(q), "quest braces unbalanced")
check("setjmp" not in h and "longjmp" not in h, "hello still uses setjmp/longjmp")
check("verr_pending" in h, "hello missing error-as-value checks")
check("MTAB" in c and "vcall_any" in c, "tables/switch missing")

print("native emitter tests ok (%d asserts)" % n)
print("hello bytes:", len(h))
print("control bytes:", len(c))
