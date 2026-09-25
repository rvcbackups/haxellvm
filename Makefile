override CC := clang
override CXX := clang++
LLVM_CONFIG ?= llvm-config
CFLAGS := -std=c17 -Wall -Wextra -Werror -Iinclude $(shell $(LLVM_CONFIG) --cflags)
CXXFLAGS := -Wall -Wextra -Werror -Iinclude $(shell $(LLVM_CONFIG) --cxxflags) -Wno-unused-parameter
LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core analysis native nativecodegen --system-libs)
SOURCES := src/main.c src/preprocess.c src/lexer.c src/parser.c src/codegen.c src/resolve.c src/macro.c
CPP_SOURCES := src/diagnostic.cpp
HEADERS := include/haxellvm/ast.h include/haxellvm/lexer.h include/haxellvm/parser.h include/haxellvm/codegen.h include/haxellvm/resolve.h include/haxellvm/macro.h
OBJECTS := $(SOURCES:.c=.o) $(CPP_SOURCES:.cpp=.o)

haxellvm: $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: test clean

test: haxellvm
	python3 -m unittest discover -s tests -v

clean:
	rm -f haxellvm src/*.o *.ll *.s