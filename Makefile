CXX      := clang++-18
LLVM_CFG := llvm-config-18
LLVM_CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags)
# strip -fno-exceptions so we can use C++ exceptions in our code
CXXFLAGS := $(filter-out -fno-exceptions,$(LLVM_CXXFLAGS)) -std=c++17 -g -Wall -Wno-unused-function -fexceptions
LDFLAGS  := $(shell $(LLVM_CFG) --ldflags) $(shell $(LLVM_CFG) --libs core orcjit native) -lpthread -ldl -lpcre2-8

CC       := clang-18
CFLAGS   := -g -O2

SRCDIR   := src
OBJS     := $(SRCDIR)/main.o $(SRCDIR)/lexer.o $(SRCDIR)/parser.o $(SRCDIR)/codegen.o $(SRCDIR)/jit.o
RT_OBJ   := $(SRCDIR)/runtime.o

TARGET   := perlc

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS) $(RT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/runtime.o: $(SRCDIR)/runtime.c $(SRCDIR)/runtime.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(SRCDIR)/main.o: $(SRCDIR)/main.cpp $(SRCDIR)/lexer.h $(SRCDIR)/ast.h $(SRCDIR)/parser.h $(SRCDIR)/codegen.h
$(SRCDIR)/lexer.o: $(SRCDIR)/lexer.cpp $(SRCDIR)/lexer.h
$(SRCDIR)/parser.o: $(SRCDIR)/parser.cpp $(SRCDIR)/parser.h $(SRCDIR)/ast.h $(SRCDIR)/lexer.h
$(SRCDIR)/codegen.o: $(SRCDIR)/codegen.cpp $(SRCDIR)/codegen.h $(SRCDIR)/ast.h $(SRCDIR)/runtime.h
$(SRCDIR)/jit.o: $(SRCDIR)/jit.cpp $(SRCDIR)/jit.h $(SRCDIR)/runtime.h

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)

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
