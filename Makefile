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

all: $(BIN)
$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@
$(OBJDIR)/%.o: lang/cxx/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@
asan:
	$(MAKE) OPT="-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" BIN=lang/cxx/bin/brat-cxx-asan \
	        OBJDIR=build/obj-asan all
clean:
	rm -rf build/obj build/obj-asan $(BIN) lang/cxx/bin/brat-cxx-asan
.PHONY: all asan clean

-include $(DEP)
