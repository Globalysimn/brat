# BRAT build
CXX      ?= g++
CXXSTD   ?= -std=c++20
OPT      ?= -O2
WARN     ?= -Wall -Wextra -Wno-unused-parameter
CXXFLAGS ?= $(CXXSTD) $(OPT) $(WARN)
SRC      := $(wildcard lang/cxx/src/*.cpp)
OBJDIR   ?= build/obj
OBJ      := $(patsubst lang/cxx/src/%.cpp,$(OBJDIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)
BIN      := lang/cxx/bin/brat-cxx

ifeq ($(BIN),lang/cxx/bin/brat-cxx)
all: $(BIN) bin/brat
bin/brat: $(BIN)
	@mkdir -p bin && ln -sf ../lang/cxx/bin/brat-cxx bin/brat
else
all: $(BIN)
endif
$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OBJDIR)/%.o: lang/cxx/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@
asan:
	$(MAKE) OPT="-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" BIN=lang/cxx/bin/brat-cxx-asan \
	        OBJDIR=build/obj-asan all
test: $(BIN)
	python3 lang/tests/run_all.py
test-native: $(BIN)
	python3 lang/tests/test_agreement.py
	python3 lang/tests/test_bitwise.py
	python3 lang/tests/test_runtime_json_enum.py
	python3 lang/tests/test_defer_try_soundness.py
	python3 lang/tests/test_perf_codegen.py
	python3 lang/tests/test_bindgen_preprocess_sys.py
	python3 lang/tests/test_dwarf_pax_parfor.py
	python3 lang/tests/test_examples_and_valgrind.py
	python3 lang/tests/test_import_prune.py
bench: $(BIN)
	python3 scripts/bench.py
clean:
	rm -rf build/obj build/obj-asan $(BIN) lang/cxx/bin/brat-cxx-asan bin/brat
.PHONY: all asan test test-native bench clean

-include $(DEP)
