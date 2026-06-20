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
# TSan flags for thread-safety checking
TSAN_CXXFLAGS := $(CXXFLAGS) -fsanitize=thread -fno-omit-frame-pointer
TSAN_CFLAGS := $(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer
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

TSAN_OBJS := $(SRCDIR)/main_tsan.o $(SRCDIR)/lexer_tsan.o $(SRCDIR)/parser_tsan.o $(SRCDIR)/codegen_tsan.o $(SRCDIR)/jit_tsan.o
TSAN_RT_OBJ := $(SRCDIR)/runtime_tsan.o
TSAN_TARGET := perlc_tsan

TARGET   := perlc
ASSERT_TESTS := tests/test_do_filename.pl tests/test_require_simple.pl tests/dbi_sqlite.pl tests/xs_ffi.pl
TSAN_TESTS := tests/threads.pl tests/threads_atomic.pl tests/destroy.pl

.PHONY: all clean test test-tsan bench

all: $(TARGET) $(EVAL_LIB)

$(TARGET): $(OBJS) $(RT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TSAN_TARGET): $(TSAN_OBJS) $(TSAN_RT_OBJ)
	$(CXX) $(TSAN_CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/runtime.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/runtime_tsan.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(SRCDIR)/eval_jit.o: $(SRCDIR)/eval_jit.cpp $(SRCDIR)/eval_jit.h $(SRCDIR)/lexer.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h $(SRCDIR)/jit.h $(SRCDIR)/runtime.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(EVAL_LIB): $(EVAL_OBJ) $(SRCDIR)/lexer.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/jit.o
	ar rcs $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(SRCDIR)/%_tsan.o: $(SRCDIR)/%.cpp
	$(CXX) $(TSAN_CXXFLAGS) -c -o $@ $<

$(SRCDIR)/main.o: $(SRCDIR)/main.cpp $(SRCDIR)/lexer.h $(SRCDIR)/ast.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h
$(SRCDIR)/lexer.o: $(SRCDIR)/lexer.cpp $(SRCDIR)/lexer.h
$(SRCDIR)/parser.o: $(SRCDIR)/parser.cpp $(SRCDIR)/parser.h $(SRCDIR)/ast.h $(SRCDIR)/lexer.h
$(SRCDIR)/codegen.o: $(SRCDIR)/codegen.cpp $(SRCDIR)/codegen.h $(SRCDIR)/ast.h $(SRCDIR)/runtime.h
$(SRCDIR)/jit.o: $(SRCDIR)/jit.cpp $(SRCDIR)/jit.h $(SRCDIR)/runtime.h

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.a $(TARGET) $(TSAN_TARGET)

test: $(TARGET) $(EVAL_LIB)
	@set -e; \
	for t in $(ASSERT_TESTS); do \
		out="/tmp/perlc_$$(basename $$t .pl)"; \
		echo "=== $$t ==="; \
		./$(TARGET) $$t -o $$out >/tmp/perlc_compile.log 2>&1; \
		$$out; \
	done

test-tsan: $(TSAN_TARGET)
	@set -e; \
	for t in $(TSAN_TESTS); do \
		out="/tmp/perlc_tsan_$$(basename $$t .pl)"; \
		echo "=== TSan: $$t ==="; \
		./$(TSAN_TARGET) $$t -o $$out >/tmp/perlc_tsan_compile.log 2>&1; \
		TSAN_OPTIONS="halt_on_error=1" $$out 2>&1; \
	done

bench: $(TARGET)
	@./bench/bench.sh $(BENCH_ARGS)
