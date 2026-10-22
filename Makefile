# ----- Output -----
OUT_DIR := out
BIN := lox

# ----- Compiler -----
CC := clang
#CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2
CFLAGS := -std=c11 -Wall -Wextra -O2
LDFLAGS :=

# ----- Sources -----
SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c,$(OUT_DIR)/%.o,$(SRCS))

# ----- Arguments Hack -----
ARGS := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))

ifneq ($(ARGS),)
$(ARGS):
	@:
endif

.PHONY: $(ARGS)

# ----- Default -----
default: compile

# ----- Compile -----
compile: $(OUT_DIR)/$(BIN)

$(OUT_DIR)/$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OUT_DIR)/%.o: %.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# ----- Run (no args) -----
run: compile
	./$(OUT_DIR)/$(BIN)

# ----- Run with args -----
runfile: compile
	./$(OUT_DIR)/$(BIN) $(ARGS)

# ----- Clean -----
clean:
	rm -rf $(OUT_DIR)
