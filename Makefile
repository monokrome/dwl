# dwl-config Makefile
# Applies patches to dwl submodule and builds

PATCHES = patches/combined.patch
HOSTNAME ?= $(shell hostname)
MONITOR_CONFIG = monitors/$(HOSTNAME).h

.PHONY: all build clean patch unpatch install uninstall

all: build

# Initialize submodule if needed
dwl/.git:
	git submodule update --init --recursive

# Apply patches and copy config files
patch: dwl/.git
	cd dwl && git checkout . && git clean -fd
	@for p in $(PATCHES); do \
		echo "    Applying $$p..."; \
		patch -d dwl -p1 < $$p || exit 1; \
	done
	cp config.h dwl/config.h
	@if [ -f "$(MONITOR_CONFIG)" ]; then \
		mkdir -p dwl/monitors; \
		cp $(MONITOR_CONFIG) dwl/monitors/; \
	fi

# Build dwl
build: patch
	$(MAKE) -C dwl

# Clean build artifacts (keeps patches applied)
clean:
	$(MAKE) -C dwl clean

# Full clean - reset submodule to pristine state
unpatch:
	cd dwl && git checkout . && git clean -fd

# Install dwl
install: build
	$(MAKE) -C dwl install

# Uninstall dwl
uninstall:
	$(MAKE) -C dwl uninstall
