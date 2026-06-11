CXX      := clang++-18
LLVM_CFG := llvm-config-18
LLVM_CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags)
# strip -fno-exceptions so we can use C++ exceptions in our code
# -mcx16: enable cmpxchg16b for the lock-free 16-byte CAS on shared
#         scalar payloads.  This is the default on x86_64-v2 and
#         later; we set it explicitly so the codegen uses cmpxchg16b
#         on every supported CPU.
# -Wno-atomic-alignment: silence the conservative "exceeds the max
#         lock-free size (8 bytes)" warning on the 16-byte CAS.
#         The CAS is lock-free on x86_64 (cmpxchg16b) and aarch64
#         (ldxp+stxp); clang's warning is a portability warning.
CXXFLAGS := $(filter-out -fno-exceptions,$(LLVM_CXXFLAGS)) -std=c++17 -g -Wall -Wno-unused-function -Wno-atomic-alignment -fexceptions -mcx16
# -latomic: link the libatomic runtime for the 16-byte __atomic_*
#          builtins on hosts that don't have a direct cmpxchg16b.
#          (x86_64-v2+ inlines it; older hosts need the shim.)
LDFLAGS  := $(shell $(LLVM_CFG) --ldflags) $(shell $(LLVM_CFG) --libs core orcjit native) -lpthread -ldl -lpcre2-8 -lsqlite3 -latomic

CC       := clang-18
# -mcx16 + -Wno-atomic-alignment: same rationale as the C++ flags above;
# needed for the runtime.c atomic primitives.
CFLAGS   := -g -O2 -mcx16 -Wno-atomic-alignment

SRCDIR   := src
OBJS     := $(SRCDIR)/main.o $(SRCDIR)/lexer.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/jit.o
RT_OBJ   := $(SRCDIR)/runtime.o
EVAL_OBJ := $(SRCDIR)/eval_jit.o
EVAL_LIB := $(SRCDIR)/libperlc_eval.a

TARGET   := perlc

.PHONY: all clean test

all: $(TARGET) $(EVAL_LIB)

$(TARGET): $(OBJS) $(RT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/runtime.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/eval_jit.o: $(SRCDIR)/eval_jit.cpp $(SRCDIR)/eval_jit.h $(SRCDIR)/lexer.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h $(SRCDIR)/jit.h $(SRCDIR)/runtime.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(EVAL_LIB): $(EVAL_OBJ) $(SRCDIR)/lexer.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/jit.o
	ar rcs $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(SRCDIR)/main.o: $(SRCDIR)/main.cpp $(SRCDIR)/lexer.h $(SRCDIR)/ast.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h
$(SRCDIR)/lexer.o: $(SRCDIR)/lexer.cpp $(SRCDIR)/lexer.h
$(SRCDIR)/parser.o: $(SRCDIR)/parser.cpp $(SRCDIR)/parser.h $(SRCDIR)/ast.h $(SRCDIR)/lexer.h
$(SRCDIR)/codegen.o: $(SRCDIR)/codegen.cpp $(SRCDIR)/codegen.h $(SRCDIR)/ast.h $(SRCDIR)/runtime.h
$(SRCDIR)/jit.o: $(SRCDIR)/jit.cpp $(SRCDIR)/jit.h $(SRCDIR)/runtime.h

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.a $(TARGET)

test: $(TARGET)
	@echo "=== hello.pl    ===" && ./$(TARGET) tests/hello.pl    -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== arith.pl    ===" && ./$(TARGET) tests/arith.pl    -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== fib.pl      ===" && ./$(TARGET) tests/fib.pl      -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== hash.pl     ===" && ./$(TARGET) tests/hash.pl     -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== builtins.pl ===" && ./$(TARGET) tests/builtins.pl -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== refs.pl     ===" && ./$(TARGET) tests/refs.pl     -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== regex.pl    ===" && ./$(TARGET) tests/regex.pl    -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== regex_g.pl  ===" && ./$(TARGET) tests/regex_g.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== modifiers.pl ===" && ./$(TARGET) tests/modifiers.pl -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== range.pl     ===" && ./$(TARGET) tests/range.pl     -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== sprintf.pl   ===" && ./$(TARGET) tests/sprintf.pl   -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== fileio.pl    ===" && ./$(TARGET) tests/fileio.pl   -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test 2>/dev/null
	@echo "=== builtins2.pl ===" && ./$(TARGET) tests/builtins2.pl -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== features.pl  ===" && ./$(TARGET) tests/features.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test 2>/dev/null
	@echo "=== advanced.pl  ===" && ./$(TARGET) tests/advanced.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test hello world
	@echo "=== oop.pl       ===" && ./$(TARGET) tests/oop.pl       -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== closures.pl  ===" && ./$(TARGET) tests/closures.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== usemod.pl    ===" && ./$(TARGET) tests/usemod.pl    -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== inherit.pl   ===" && ./$(TARGET) tests/inherit.pl   -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== defaults.pl     ===" && ./$(TARGET) tests/defaults.pl     -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== newfeatures.pl  ===" && ./$(TARGET) tests/newfeatures.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== fibn.pl         ===" && ./$(TARGET) tests/fibn.pl         -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test 10
	@echo "=== tier1.pl        ===" && ./$(TARGET) tests/tier1.pl        -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== tier2.pl        ===" && ./$(TARGET) tests/tier2.pl        -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== tier3.pl        ===" && ./$(TARGET) tests/tier3.pl        -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== threads.pl      ===" && ./$(TARGET) tests/threads.pl      -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== threads_atomic.pl ===" && ./$(TARGET) tests/threads_atomic.pl -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== destroy.pl      ===" && ./$(TARGET) tests/destroy.pl      -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== eval_string.pl  ===" && ./$(TARGET) tests/eval_string.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test 2>/dev/null
	@echo "=== fileops.pl      ===" && ./$(TARGET) tests/fileops.pl      -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== interp.pl       ===" && ./$(TARGET) tests/interp.pl       -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== misc.pl         ===" && ./$(TARGET) tests/misc.pl         -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== tr.pl           ===" && ./$(TARGET) tests/tr.pl           -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== wantarray.pl    ===" && ./$(TARGET) tests/wantarray.pl    -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== regex_named.pl  ===" && ./$(TARGET) tests/regex_named.pl  -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
	@echo "=== completeness.pl ===" && ./$(TARGET) tests/completeness.pl -o /tmp/perlc_test 2>/dev/null && /tmp/perlc_test
