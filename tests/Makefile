CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-sign-compare
CPPFLAGS += -I stubs -I ../src
BUILD_DIR ?= .build

HARNESS := golden_render tx_pipeline spi_io perf_stats params_parse fb_damage registry
BINARIES := $(addprefix $(BUILD_DIR)/,$(HARNESS))
DRIVER_SOURCES := $(wildcard ../src/*.c ../src/*.h)
STUB_HEADERS := $(wildcard stubs/linux/*.h stubs/linux/*/*.h stubs/drm/*.h stubs/asm/*.h)
NEON_SUPPORTED := $(filter aarch64 arm64,$(shell uname -m))

.PHONY: all golden check check-neon clean $(HARNESS)

all: $(BINARIES)

$(HARNESS): %: $(BUILD_DIR)/%

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%: %.c $(DRIVER_SOURCES) $(STUB_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/golden_render_neon: golden_render.c $(DRIVER_SOURCES) $(STUB_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCONFIG_ARM64 -DCONFIG_KERNEL_MODE_NEON \
		-o $@ $< ../src/render_neon.c

$(BUILD_DIR)/registry: registry.c $(DRIVER_SOURCES) $(STUB_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -o $@ $<

golden: $(BUILD_DIR)/golden_render
	./$(BUILD_DIR)/golden_render > $(BUILD_DIR)/golden.txt.tmp
	mv $(BUILD_DIR)/golden.txt.tmp $(BUILD_DIR)/golden.txt
	@echo "Candidate written to $(BUILD_DIR)/golden.txt; review before replacing golden.txt"

check: $(BINARIES) golden.txt
	./$(BUILD_DIR)/golden_render --check golden.txt
	./$(BUILD_DIR)/tx_pipeline
	./$(BUILD_DIR)/spi_io
	./$(BUILD_DIR)/perf_stats
	./$(BUILD_DIR)/params_parse
	./$(BUILD_DIR)/fb_damage
	./$(BUILD_DIR)/registry

ifneq ($(NEON_SUPPORTED),)
check: check-neon
check-neon: $(BUILD_DIR)/golden_render_neon golden.txt
	./$(BUILD_DIR)/golden_render_neon --check golden.txt
else
check-neon:
	@echo "NEON tests require a native AArch64 host"
endif

clean:
	rm -f $(BINARIES) $(BUILD_DIR)/golden_render_neon \
		$(BUILD_DIR)/golden.txt $(BUILD_DIR)/golden.txt.tmp
