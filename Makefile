CC = clang
CFLAGS = -Wall -Wextra -Wpedantic -std=c99
SRC = src/main.c src/terminal.c src/buffer.c src/editor.c src/display.c src/highlight.c src/config.c src/clipboard.c src/lsp.c src/window.c src/explorer.c src/headers.c
OBJ = $(SRC:.c=.o)
DEPS = $(OBJ:.o=.d)
TARGET = bin/pvim
BUILD_DIR = $(CURDIR)

.PHONY: all clean compile_commands

all: $(TARGET) compile_commands.json

$(TARGET): $(OBJ)
	@mkdir -p $(dir $(TARGET))
	$(CC) $(OBJ) -o $(TARGET)

# -MMD writes a .d file listing every header each object depends on, so a
# change to a shared header (e.g. the EditorState layout in editor.h) rebuilds
# every object that includes it instead of leaving a stale, ABI-mismatched .o.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# Expose the exact build flags to clangd via compile_commands.json so its
# diagnostics and completions match the real translation units.
compile_commands.json: $(SRC)
	@printf '[\n' > $@
	@set -e; n=0; total=0; for f in $(SRC); do total=$$((total + 1)); done; \
	for f in $(SRC); do \
	  n=$$((n + 1)); \
	  if [ $$n -lt $$total ]; then c=,; else c=; fi; \
	  printf '  {"directory": "%s", "arguments": ["%s", "-Wall", "-Wextra", "-Wpedantic", "-std=c99", "-c", "%s", "-o", "%s"], "file": "%s"}%s\n' \
	    "$(BUILD_DIR)" "$(CC)" "$$f" "$${f%.c}.o" "$$f" "$$c" >> $@; \
	done
	@printf ']\n' >> $@

clean:
	rm -f $(OBJ) $(DEPS) $(TARGET) compile_commands.json
