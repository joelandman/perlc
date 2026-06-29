CXX      := g++
LLVM_CFG := llvm-config-18
LLVM_CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags)

# Force complete libstdc++ containers before any LLVM header in every TU.
# This is the key workaround for the incomplete __gnu_cxx::__normal_iterator
# problem with LLVM 18 + clang-18 + gcc-16 libstdc++ on this host.
FORCE_STD := -include src/force_complete_std.h

CXXFLAGS := -std=c++17 -g -Wall -Wno-unused-function -Wno-atomic-alignment -fexceptions -mcx16 -I/usr/lib/llvm-18/include -include src/force_complete_std.h

TSAN_CXXFLAGS := $(CXXFLAGS) -fsanitize=thread -fno-omit-frame-pointer
TSAN_CFLAGS := $(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer

LDFLAGS  := $(shell $(LLVM_CFG) --ldflags) $(shell $(LLVM_CFG) --libs core native) -lpthread -ldl -lpcre2-8 -lsqlite3 -latomic

CC       := clang-18
CFLAGS   := -g -O2 -mcx16 -Wno-atomic-alignment

SRCDIR   := src
OBJS     := $(SRCDIR)/main.o $(SRCDIR)/lexer.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/ast.o $(SRCDIR)/llvm_early_init.o
RT_OBJ   := $(SRCDIR)/runtime.o
TSAN_OBJS := $(SRCDIR)/main_tsan.o $(SRCDIR)/lexer_tsan.o $(SRCDIR)/parser_tsan.o $(SRCDIR)/codegen_tsan.o
TSAN_RT_OBJ := $(SRCDIR)/runtime_tsan.o
TSAN_TARGET := perlc_tsan

TARGET   := perlc
ASSERT_TESTS := tests/test_do_filename.pl tests/test_require_simple.pl tests/dbi_sqlite.pl tests/xs_ffi.pl
TSAN_TESTS := tests/threads.pl tests/threads_atomic.pl tests/destroy.pl

.PHONY: all clean test test-tsan test-full bench test-opt% test-opt-matrix

all: $(TARGET)

$(TARGET): $(OBJS) $(RT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TSAN_TARGET): $(TSAN_OBJS) $(TSAN_RT_OBJ)
	$(CXX) $(TSAN_CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/runtime.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/runtime_tsan.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(TSAN_CFLAGS) -c -o $@ $<

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(SRCDIR)/%_tsan.o: $(SRCDIR)/%.cpp
	$(CXX) $(TSAN_CXXFLAGS) -c -o $@ $<

$(SRCDIR)/main.o: $(SRCDIR)/main.cpp $(SRCDIR)/lexer.h $(SRCDIR)/ast.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h $(SRCDIR)/llvm_early_init.h src/force_complete_std.h
$(SRCDIR)/lexer.o: $(SRCDIR)/lexer.cpp $(SRCDIR)/lexer.h
$(SRCDIR)/parser.o: $(SRCDIR)/parser.cpp $(SRCDIR)/parser.h $(SRCDIR)/ast.h $(SRCDIR)/lexer.h
$(SRCDIR)/codegen.o: $(SRCDIR)/codegen.cpp $(SRCDIR)/codegen.h $(SRCDIR)/ast.h $(SRCDIR)/runtime.h
$(SRCDIR)/ast.o: $(SRCDIR)/ast.cpp $(SRCDIR)/ast.h
$(SRCDIR)/llvm_early_init.o: $(SRCDIR)/llvm_early_init.cpp $(SRCDIR)/llvm_early_init.h

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.a $(TARGET) $(TSAN_TARGET)

test: $(TARGET)
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

test-full: $(TARGET)
	@./tests/harness.sh

test-opt%:
	@OPT_LEVEL=$* ./tests/harness.sh

test-opt-matrix: $(TARGET)
	@set -e; \
	for L in 0 2 3; do \
		echo "=== OPT_LEVEL=$$L ==="; \
		OPT_LEVEL=$$L ./tests/harness.sh; \
	done

bench: $(TARGET)
	@./bench/bench.sh $(BENCH_ARGS)
