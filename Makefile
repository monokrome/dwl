# dwl-config Makefile
# Applies patches to dwl submodule and builds

PATCHES = patches/combined.patch patches/gradient.patch patches/cfact.patch
WREN_PATCH = patches/wren.patch
HOSTNAME ?= $(shell hostname)
MONITOR_CONFIG = monitors/$(HOSTNAME).h
DWL_DIR = dwl
BIN_DIR = bin
WREN_DIR = wren

.PHONY: all wren build clean unpatch install uninstall

# Default: build without wren scripting
all: patch build copy

# Build with wren scripting support
wren: patch patch-wren copy-wren-files build-wren copy

# Initialize submodules if needed
$(DWL_DIR)/.git:
	git submodule update --init --recursive

$(WREN_DIR)/.git:
	git submodule update --init --recursive

# Apply combined patches and copy config files
patch: $(DWL_DIR)/.git
	cd $(DWL_DIR) && git checkout . && git clean -fd
	@for p in $(PATCHES); do \
		echo "    Applying $$p..."; \
		patch -d $(DWL_DIR) -p1 < $$p || exit 1; \
	done
	cp config.h $(DWL_DIR)/config.h
	@if [ -f "$(MONITOR_CONFIG)" ]; then \
		mkdir -p $(DWL_DIR)/monitors; \
		cp $(MONITOR_CONFIG) $(DWL_DIR)/monitors/; \
	fi

# Apply wren scripting patch (after combined patch)
patch-wren: $(WREN_DIR)/.git
	@echo "    Applying $(WREN_PATCH)..."
	patch -d $(DWL_DIR) -p1 < $(WREN_PATCH)

# Copy wren scripting files to dwl directory
copy-wren-files:
	cp scripting.c $(DWL_DIR)/
	cp scripting.h $(DWL_DIR)/

# Build dwl (standard)
build:
	$(MAKE) -C $(DWL_DIR)

# Build dwl with wren scripting
build-wren:
	$(MAKE) -C $(DWL_DIR) scripting

# Copy built binary to bin/
copy:
	mkdir -p $(BIN_DIR)
	cp $(DWL_DIR)/dwl $(BIN_DIR)/dwl
	@echo "Built: $(BIN_DIR)/dwl"

# Clean build artifacts (keeps patches applied)
clean:
	$(MAKE) -C $(DWL_DIR) clean
	rm -rf $(BIN_DIR)

# Full clean - reset submodule to pristine state
unpatch:
	cd $(DWL_DIR) && git checkout . && git clean -fd
	rm -rf $(BIN_DIR)

# Install dwl system-wide
install: all
	$(MAKE) -C $(DWL_DIR) install

# Uninstall dwl
uninstall:
	$(MAKE) -C $(DWL_DIR) uninstall
