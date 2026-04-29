# ────────────────────────────────────────────────────────────────
# TCodec Makefile
#
# Auto-detects ARM NEON and builds accordingly.
# On ARM (aarch64): NEON always available, -march=armv8-a
# On x86_64:        Scalar-only fallback (for development/testing)
#
# Targets:
#   make          — Build static library + CLI tools
#   make shared   — Build shared library
#   make test     — Build and run tests
#   make clean    — Remove build artifacts
#   make bench    — Run benchmarks
# ────────────────────────────────────────────────────────────────

# ── Configuration ───────────────────────────────────────────────

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2
LDFLAGS ?=

# ── Architecture detection ──────────────────────────────────────

UNAME_M := $(shell uname -m)

ifeq ($(UNAME_M),aarch64)
    # ARM64 — NEON always available
    ARCH_CFLAGS  = -march=armv8-a -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else ifeq ($(UNAME_M),arm64)
    # Apple Silicon (macOS) — NEON always available
    ARCH_CFLAGS  = -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else ifeq ($(UNAME_M),armv7l)
    # ARM32 with NEON
    ARCH_CFLAGS  = -march=armv7-a -mfpu=neon -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else
    # x86_64 / other — scalar fallback
    ARCH_CFLAGS  = -DTCODEC_NEON=0
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     =
endif

# ── Paths ───────────────────────────────────────────────────────

SRC_DIR  = src
NEON_DIR = neon
INC_DIR  = include
TOOL_DIR = tools
TEST_DIR = test
BUILD_DIR = build

# ── Source files ─────────────────────────────────────────────────

CORE_SRC = $(wildcard $(SRC_DIR)/*.c)
ALL_SRC  = $(CORE_SRC) $(NEON_SRC)

CORE_OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRC))
NEON_OBJ = $(patsubst $(NEON_DIR)/%.c,$(BUILD_DIR)/%.o,$(NEON_SRC))
ALL_OBJ  = $(CORE_OBJ) $(NEON_OBJ)

TOOL_SRC = $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c
TOOL_OBJ = $(BUILD_DIR)/tcenc.o $(BUILD_DIR)/tcdec.o

TEST_SRC = $(TEST_DIR)/test_tcodec.c
TEST_OBJ = $(BUILD_DIR)/test_tcodec.o

# ── Targets ──────────────────────────────────────────────────────

LIB_STATIC = $(BUILD_DIR)/libtcodec.a
LIB_SHARED = $(BUILD_DIR)/libtcodec.so
ENC_BIN    = $(BUILD_DIR)/tcenc
DEC_BIN    = $(BUILD_DIR)/tcdec
TEST_BIN   = $(BUILD_DIR)/test_tcodec

# ── Flags ────────────────────────────────────────────────────────

WARN_FLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

COMMON_CFLAGS = $(CFLAGS) $(ARCH_CFLAGS) $(WARN_FLAGS) \
                -I$(INC_DIR) -std=c11 -D_GNU_SOURCE

RELEASE_CFLAGS = $(COMMON_CFLAGS) -DNDEBUG -flto
DEBUG_CFLAGS   = $(COMMON_CFLAGS) -g -DTCODEC_DEBUG -fsanitize=address

# ── Default: release build ───────────────────────────────────────

.PHONY: all clean shared test bench debug release nothreads cross-test cross-test-info install

all: release

release: $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN)

debug: CFLAGS := -O0
debug: COMMON_CFLAGS := $(DEBUG_CFLAGS)
debug: $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN) $(TEST_BIN)

# ── Build directory ──────────────────────────────────────────────

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ── Static library ───────────────────────────────────────────────

$(LIB_STATIC): $(ALL_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# ── Shared library ───────────────────────────────────────────────

shared: $(LIB_SHARED)

$(LIB_SHARED): $(ALL_OBJ) | $(BUILD_DIR)
	$(CC) -shared -o $@ $^ $(ARCH_LDFLAGS) $(LDFLAGS)

# ── CLI tools ────────────────────────────────────────────────────

$(ENC_BIN): $(TOOL_DIR)/tcenc.c $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

$(DEC_BIN): $(TOOL_DIR)/tcdec.c $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

# ── Test binary ──────────────────────────────────────────────────

$(TEST_BIN): $(TEST_SRC) $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(ENC_BIN) $(DEC_BIN)
	@echo "Benchmark requires a YUV input file."
	@echo "Usage: ./$(ENC_BIN) -w 3840 -h 2160 -q 32 -v -o /dev/null input.yuv"

# ── Object files ─────────────────────────────────────────────────

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(NEON_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(TOOL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

# ── Install ──────────────────────────────────────────────────────

PREFIX ?= /usr/local

install: release
	install -d $(PREFIX)/lib $(PREFIX)/include/tcodec $(PREFIX)/bin
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 644 $(INC_DIR)/tcodec.h $(INC_DIR)/tcodec_types.h $(PREFIX)/include/tcodec/
	install -m 755 $(ENC_BIN) $(PREFIX)/bin/
	install -m 755 $(DEC_BIN) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/lib/libtcodec.a
	rm -rf $(PREFIX)/include/tcodec
	rm -f $(PREFIX)/bin/tcenc $(PREFIX)/bin/tcdec

# ── Cross-compilation test for ARM (aarch64) ─────────────────────
#
# Compile-only test using Apple Clang's -target arm64-apple-macos11.
# Verifies that NEON guards are correct: scalar DCT/WHT implementations
# are excluded when TCODEC_NEON=1, and NEON intrinsics compile cleanly.
# No linking (we can't run ARM binaries on x86).

CROSS_CC      ?= clang
CROSS_TARGET  ?= arm64-apple-macos11
CROSS_CFLAGS  = -target $(CROSS_TARGET) -DTCODEC_NEON=1 -D_GNU_SOURCE \
                -I$(INC_DIR) -std=c11 \
                -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

.PHONY: cross-test cross-test-info

cross-test-info:
	@echo "Cross-compiling for ARM ($(CROSS_TARGET)) to verify NEON guards..."
	@echo "CC=$(CROSS_CC) CFLAGS=$(CROSS_CFLAGS)"

cross-test: cross-test-info
	@fail=0; \
	echo "── NEON sources ────────────────────────────────────"; \
	for f in $(NEON_DIR)/*.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── Core sources (with TCODEC_NEON=1) ────────────────"; \
	for f in $(SRC_DIR)/*.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── Tools + test (with TCODEC_NEON=1) ───────────────"; \
	for f in $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c $(TEST_DIR)/test_tcodec.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── NEON + no-threads (TCODEC_NEON=1 + TCODEC_NO_THREADS) ──"; \
	for f in $(SRC_DIR)/*.c $(NEON_DIR)/*.c $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c $(TEST_DIR)/test_tcodec.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -DTCODEC_NO_THREADS -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	if [ "$$fail" -ne 0 ]; then echo "$$fail file(s) FAILED cross-compilation!"; exit 1; \
	else echo "All cross-compilation tests passed."; fi

# ── No-threads build (for systems without pthread) ────────────────

# Note: requires clean build — run 'make clean && make nothreads' if objects
# from a normal build exist, since .o files won't be recompiled automatically.
nothreads: CFLAGS := $(CFLAGS) -DTCODEC_NO_THREADS
nothreads: ARCH_LDFLAGS :=
nothreads: clean $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN)
	@echo "Built without threading support (TCODEC_NO_THREADS)"

nothreads-test: CFLAGS := $(CFLAGS) -DTCODEC_NO_THREADS
nothreads-test: ARCH_LDFLAGS :=
nothreads-test: clean $(TEST_BIN)
	./$(TEST_BIN)

# ── Clean ─────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD_DIR)

# ── Info ──────────────────────────────────────────────────────────

info:
	@echo "Architecture: $(UNAME_M)"
	@echo "NEON:         $(if $(NEON_SRC),YES,NO)"
	@echo "CC:           $(CC)"
	@echo "CFLAGS:       $(COMMON_CFLAGS)"
	@echo "Sources:      $(CORE_SRC) $(NEON_SRC)"
