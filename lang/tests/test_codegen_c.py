import importlib.util as _ilu

import os as _o
_HERE = _o.path.dirname(_o.path.abspath(__file__))
_ENGINE = _o.path.join(_HERE, '..', 'engine')
_EX = _o.path.join(_HERE, '..', 'examples')
_spec2 = _ilu.spec_from_file_location("codegen_c_mod", _o.path.join(_ENGINE, "codegen_c.py"))
cc = _ilu.module_from_spec(_spec2)
_spec2.loader.exec_module(cc)

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


hello_src = _read(_o.path.join(_EX, "hello.brat"))
control_src = _read(_o.path.join(_EX, "control.brat"))
quest_src = "let x = 1?"

h = cc.emit(hello_src)
c = cc.emit(control_src)
q = cc.emit(quest_src)

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

print("codegen_c tests ok (%d asserts)" % n)
print("hello bytes:", len(h))
print("control bytes:", len(c))