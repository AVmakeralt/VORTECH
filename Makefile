# VORTECH Compiler Makefile
# Maximum practical performance with minimum compiler complexity.

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror=return-type -g -O2
LDFLAGS = -lm

SRCDIR  = src
OBJDIR  = obj

SOURCES = $(SRCDIR)/arena.c \
          $(SRCDIR)/diag.c \
          $(SRCDIR)/lexer.c \
          $(SRCDIR)/ast.c \
          $(SRCDIR)/parser.c \
          $(SRCDIR)/hir_build.c \
          $(SRCDIR)/ssa_build.c \
          $(SRCDIR)/ssa_verify.c \
          $(SRCDIR)/opt.c \
          $(SRCDIR)/opt_constfold.c \
          $(SRCDIR)/opt_dce.c \
          $(SRCDIR)/opt_copyprop.c \
          $(SRCDIR)/opt_cse.c \
          $(SRCDIR)/opt_sccp.c \
          $(SRCDIR)/opt_licm.c \
          $(SRCDIR)/opt_unroll.c \
          $(SRCDIR)/isel.c \
          $(SRCDIR)/regalloc.c \
          $(SRCDIR)/peephole.c \
          $(SRCDIR)/emit.c \
          $(SRCDIR)/main.c

OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

TARGET  = vortech

.PHONY: all clean test install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET) *.s *.o *.out

test: $(TARGET)
	@echo "Testing VORTECH compiler..."
	@mkdir -p tests
	@echo 'fn main() -> i32 { return 42; }' > tests/test_basic.vt
	@./$(TARGET) -O2 tests/test_basic.vt -o tests/test_basic
	@echo "Basic test compiled."

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# Debug build
debug: CFLAGS += -DDEBUG -O0 -ggdb
debug: clean $(TARGET)

# Release build
release: CFLAGS += -O3 -DNDEBUG -flto
release: LDFLAGS += -flto
release: clean $(TARGET)
