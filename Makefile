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

# ----- Test -----
.PHONY: test

test: compile
	@set -e; \
	if [ -n "$(ARGS)" ]; then \
		dir="test/$(word 1,$(ARGS))"; \
	else \
		dir="test"; \
	fi; \
	if [ ! -d "$$dir" ]; then \
		echo "Test directory not found: $$dir"; \
		exit 1; \
	fi; \
	find "$$dir" -type f -name '*.lox' -print0 | \
	while IFS= read -r -d '' file; do \
		echo "Running $$file"; \
		./$(OUT_DIR)/$(BIN) "$$file" || true; \
	done