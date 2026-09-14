# Device parameters
DEVICE ?= tanmatsu
PORT ?= /dev/ttyACM0
BADGELINKPORT ?= $(PORT)

# Build parameters
IDF_VERSION ?= v6.0.2
# Test variant: base, nohosted, ringfix or chunkedmsync (see README.md).
VARIANT ?= base
BUILD ?= build/$(DEVICE)-$(VARIANT)
FAT ?= 0
SDKCONFIG_DEFAULTS ?= sdkconfigs/general;sdkconfigs/$(DEVICE);sdkconfigs/variant-$(VARIANT)
SDKCONFIG ?= sdkconfig_$(DEVICE)_$(VARIANT)

# SDK
IDF_PATH ?= $(shell cat .IDF_PATH 2>/dev/null || test -d `pwd`/esp-idf && echo `pwd`/esp-idf || echo '$(HOME)/.espressif/$(IDF_VERSION)/esp-idf')
IDF_TOOLS_PATH ?= $(shell cat .IDF_TOOLS_PATH 2>/dev/null || test -d `pwd`/esp-idf-tools && echo `pwd`/esp-idf-tools || echo '$(HOME)/.espressif/tools')
IDF_SOURCE ?= $(shell cat .IDF_PATH 2>/dev/null && echo '$(IDF_PATH)/export.sh' || test -d `pwd`/esp-idf && echo '$(IDF_PATH)/export.sh' || echo '$(HOME)/.espressif/tools/activate_idf_$(IDF_VERSION).sh')
IDF_EXPORT_QUIET ?= 1
IDF_GITHUB_ASSETS ?= dl.espressif.com/github_assets
IDF_INSTALL_PATH ?= $(shell echo `pwd`/esp-idf)
IDF_INSTALL_TOOLS_PATH ?= $(shell echo `pwd`/esp-idf-tools)

MAKEFLAGS += --silent
SHELL := /usr/bin/env bash


####

# Set IDF_TARGET based on device name

ifeq ($(DEVICE), tanmatsu)
IDF_TARGET ?= esp32p4
else ifeq ($(DEVICE), mch2022)
IDF_TARGET ?= esp32
else ifeq ($(DEVICE), kami)
IDF_TARGET ?= esp32
else ifeq ($(DEVICE), hackerhotel-2024)
IDF_TARGET ?= esp32c6
else ifeq ($(DEVICE), hackaday2025)
IDF_TARGET ?= esp32s3
else ifeq ($(DEVICE), heltecv3)
IDF_TARGET ?= esp32s3
else ifeq ($(DEVICE), esp32-p4-function-ev-board)
IDF_TARGET ?= esp32p4
else ifeq ($(DEVICE), esp32-s31-korvo-1)
IDF_TARGET ?= esp32s31
else ifeq ($(DEVICE), why2025)
IDF_TARGET ?= esp32p4
else
$(warning "Unknown device, defaulting to ESP32")
IDF_TARGET ?= esp32
endif

IDF_PARAMS := -B $(BUILD) -DDEVICE=$(DEVICE) -DSDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" -DSDKCONFIG=$(SDKCONFIG) -DIDF_TARGET=$(IDF_TARGET) -DFAT=$(FAT)

# Variants with patched IDF components: patches/<variant>/<component>/*.patch
# are applied to a copy of $(IDF_PATH)/components/<component> in
# build/components-<variant>, which the project picks up in preference to the
# IDF's own (see CMakeLists.txt).
VARIANT_PATCH_DIRS := $(wildcard patches/$(VARIANT)/*)
VARIANT_COMPONENTS_DIR := build/components-$(VARIANT)
ifneq ($(VARIANT_PATCH_DIRS),)
IDF_PARAMS += -DIDF6TEST_EXTRA_COMPONENTS=$(CURDIR)/$(VARIANT_COMPONENTS_DIR)
endif

# All variants, in the order cycleall runs them.
VARIANTS ?= base nohosted ringfix chunkedmsync

#####

export IDF_TOOLS_PATH
export IDF_GITHUB_ASSETS

# General targets

.PHONY: all
all: build

# Badgelink

# Determine badgelink connection argument: --tcp for host:port, --port for serial devices
BADGELINK_CONN := $(if $(findstring :,$(BADGELINKPORT)),--tcp $(BADGELINKPORT),--port $(BADGELINKPORT))

.PHONY: badgelink
badgelink:
	rm -rf badgelink
	git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink
	cd badgelink/tools; ./install.sh

# One app for all variants, installed straight into AppFS (no /int or /sd app
# directory, so the launcher's own AppFS management never touches it). Each
# install overwrites the single copy; the app reports its variant, and
# tools/sdtest.py refuses to test a different variant than asked for.
APP_SLUG ?= at.cavac.idf6test
APP_TITLE ?= IDF6 SD test
APP_VERSION ?= 1

# Writing an AppFS entry erases flash sectors and takes several seconds, far
# longer than badgelink's 0.25s default request timeout.
APPFS_TIMEOUT ?= 30

# A fresh badgelink connection intermittently fails with "Invalid sync" when the
# TCP proxy still holds bytes from a previous session; retry instead of failing.
BADGELINK := tools/badgelink_retry.sh $(BADGELINK_CONN)

.PHONY: install
install: build mode_badgelink
	@echo "=== Installing $(APP_SLUG) ($(VARIANT)) into AppFS ==="
	$(BADGELINK) --timeout $(APPFS_TIMEOUT) appfs upload $(APP_SLUG) "$(APP_TITLE)" $(APP_VERSION) $(BUILD)/application.bin
	@echo "=== Installation complete ==="

# Needs BadgeLink mode: after an app hands the badge back, USB is in debug mode.
.PHONY: run
run: mode_badgelink
	$(BADGELINK) start $(APP_SLUG)

# Test automation (see TESTPLAN.md)
#
#   make testrun VARIANT=base SUITE=matrix ARGS="reps=3"   app must be running
#   make cycle   VARIANT=base SUITE=matrix                 build, install, run, test
#   make cycleall SUITE=matrix                             cycle over $(VARIANTS)
SUITE ?= smoke
ARGS ?=

.PHONY: testrun cycle cycleall recover
# After a crash or hang: reset the badge (as make monitor does), wait for the
# app, send EXIT so it returns to the launcher.
recover:
	source "$(IDF_SOURCE)" >/dev/null && python3 tools/recover.py --port "$(PORT)"

testrun:
	source "$(IDF_SOURCE)" >/dev/null && \
	python3 tools/sdtest.py --port "$(PORT)" --variant $(VARIANT) --suite $(SUITE) --args "$(ARGS)"

cycle: build install run
	$(MAKE) testrun

cycleall:
	for v in $(VARIANTS); do $(MAKE) cycle VARIANT=$$v SUITE=$(SUITE) ARGS="$(ARGS)"; done

# Preparation

.PHONY: prepare
prepare: sdk

.PHONY: submodules
submodules: 
	if [ ! -f .submodules_update_done ]; then \
		echo "Updating submodules"; \
		git submodule update --init --recursive; \
		touch .submodules_update_done; \
	fi

.PHONY: sdk
sdk:
	if test -d "$(IDF_INSTALL_PATH)"; then echo -e "ESP-IDF target folder exists!\r\nPlease remove the folder or un-set the environment variable."; exit 1; fi
	if test -d "$(IDF_INSTALL_TOOLS_PATH)"; then echo -e "ESP-IDF tools target folder exists!\r\nPlease remove the folder or un-set the environment variable."; exit 1; fi
	git clone --recursive --branch "$(IDF_VERSION)" https://github.com/espressif/esp-idf.git "$(IDF_INSTALL_PATH)" --depth=1 --shallow-submodules
	cd "$(IDF_INSTALL_PATH)"; git submodule update --init --recursive
	cd "$(IDF_INSTALL_PATH)"; bash install.sh all

.PHONY: eim-sdk
eim-sdk:
	eim install --do-not-track true -i $(IDF_VERSION)

.PHONY: check-sdk
check-sdk:
	@if test -d $(IDF_PATH); then \
		printf '%s\n' "ESP-IDF $(IDF_VERSION) found!"; \
	else \
		printf 'ESP-IDF SDK not found. %s\n' "Please install ESP-IDF $(IDF_VERSION), either by running 'make prepare' (installs to this folder) or 'make eim-sdk' (installs using Espressif installation manager) or if manually installed set the IDF_PATH and IDF_TOOLS_PATH environment variables or create files .IDF_PATH and .IDF_TOOLS_PATH in this folder containing the paths." >&2; \
		exit 1; \
	fi

.PHONY: print-sdk
print-sdk:
	echo "ESP-IDF path:               $(IDF_PATH)"
	echo "ESP-IDF tools:              $(IDF_TOOLS_PATH)"
	echo "ESP-IDF source command:     $(IDF_SOURCE)"
	echo "ESP-IDF installation path:  $(IDF_INSTALL_PATH)"
	echo "ESP-IDF installation tools: $(IDF_INSTALL_TOOLS_PATH)"

.PHONY: reinstallsdk
reinstallsdk:
	cd "$(IDF_PATH)"; bash install.sh all

.PHONY: removesdk
removesdk:
	rm -rf "$(IDF_PATH)"
	rm -rf "$(IDF_TOOLS_PATH)"

.PHONY: refreshsdk
refreshsdk: removesdk sdk

.PHONY: menuconfig
menuconfig:
	source "$(IDF_SOURCE)" && idf.py menuconfig -DDEVICE=$(DEVICE) -DSDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" -DSDKCONFIG=$(SDKCONFIG) -DIDF_TARGET=$(IDF_TARGET)
	
# Cleaning

.PHONY: clean
clean:
	rm -rf $(BUILD)
	rm -f .submodules_update_done
	rm -rf managed_components
	rm -f sdkconfig_*

.PHONY: fullclean
fullclean: clean
	rm -rf build
	rm -f sdkconfig_*
	rm -f sdkconfig
	rm -f sdkconfig.old
	rm -f sdkconfig.ci
	rm -f sdkconfig.defaults

# Check if build environment is set up correctly
.PHONY: checkbuildenv
checkbuildenv:
	if [ -z "$(IDF_PATH)" ]; then echo "IDF_PATH is not set!"; exit 1; fi
	if [ -z "$(IDF_TOOLS_PATH)" ]; then echo "IDF_TOOLS_PATH is not set!"; exit 1; fi

# Building

.PHONY: build
build: check-sdk icons checkbuildenv $(if $(VARIANT_PATCH_DIRS),variant-components)
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) build

# Only rewritten when a patch is newer than the copy, so incremental builds stay
# incremental.
.PHONY: variant-components
variant-components:
	for d in $(VARIANT_PATCH_DIRS); do \
		comp=$$(basename $$d); dst=$(VARIANT_COMPONENTS_DIR)/$$comp; \
		if [ -d $$dst ] && [ -z "$$(find $$d -newer $$dst/.patched 2>/dev/null)" ]; then continue; fi; \
		echo "Patching $$comp for $(VARIANT)"; \
		rm -rf $$dst; mkdir -p $(VARIANT_COMPONENTS_DIR); \
		cp -r "$(IDF_PATH)/components/$$comp" $$dst || exit 1; \
		for p in $$d/*.patch; do patch -d $$dst -p1 < $$p || exit 1; done; \
		touch $$dst/.patched; \
	done

.PHONY: buildvariants installvariants
buildvariants:
	for v in $(VARIANTS); do $(MAKE) build VARIANT=$$v || exit 1; done
installvariants:
	for v in $(VARIANTS); do $(MAKE) install VARIANT=$$v || exit 1; done

.PHONY: reconfigure
reconfigure: check-sdk checkbuildenv $(if $(VARIANT_PATCH_DIRS),variant-components)
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) reconfigure

# Hardware

.PHONY: flash
flash: build
	source "$(IDF_SOURCE)" && \
	idf.py $(IDF_PARAMS) flash -p $(PORT)

.PHONY: flashmonitor
flashmonitor: build
	source "$(IDF_SOURCE)" && \
	idf.py $(IDF_PARAMS) flash -p $(PORT) monitor

.PHONY: prepappfs
prepappfs:
	source "$(IDF_SOURCE)" && \
	python3 managed_components/badgeteam__appfs/tools/appfs_generate.py \
	8192000 \
	appfs.bin

.PHONY: erase
erase:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) erase-flash -p $(PORT)

.PHONY: monitor
monitor:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) monitor -p $(PORT)

# USB mode switching
#
# mode_badgelink asks the firmware (the launcher, in USB_DEBUG mode) to switch
# USB into BadgeLink mode by sending "BADGELINK\n" on the debug console, then
# waits until BadgeLink actually answers. The request sometimes gets lost
# silently, so it is repeated every few seconds. Does nothing if BadgeLink
# already answers, so other targets can depend on it.
#
# PORT accepts either a local device path (e.g. /dev/ttyACM0) or an
# rfc2217:// URL when the device is being forwarded over the network.
BADGELINK_SWITCH_TRIES ?= 5
BADGELINK_WAIT_TRIES ?= 5

.PHONY: mode_badgelink
mode_badgelink:
	if badgelink/tools/badgelink.sh $(BADGELINK_CONN) appfs list >/dev/null 2>&1; then \
		echo "Badge is already in BadgeLink mode"; exit 0; \
	fi; \
	source "$(IDF_SOURCE)" >/dev/null; \
	for s in $$(seq 1 $(BADGELINK_SWITCH_TRIES)); do \
		echo "Requesting switch to BadgeLink mode on $(PORT) (attempt $$s)..."; \
		python3 -c "import serial, sys; s=serial.serial_for_url('$(PORT)', timeout=1); s.write(b'BADGELINK\n'); s.flush(); sys.stdout.write(s.read(128).decode(errors='replace')); s.close()" || true; \
		echo; \
		for i in $$(seq 1 $(BADGELINK_WAIT_TRIES)); do \
			sleep 1; \
			if badgelink/tools/badgelink.sh $(BADGELINK_CONN) appfs list >/dev/null 2>&1; then \
				echo "Badge is in BadgeLink mode"; exit 0; \
			fi; \
		done; \
	done; \
	echo "Timed out waiting for the badge to enter BadgeLink mode" >&2; \
	exit 1

# Ask the firmware (running in USB_DEVICE / BadgeLink mode) to switch its USB
# back to flash/monitor (USB_DEBUG) mode via the BadgeLink `mode` command.
# Prefers the badgelink checkout created by `make badgelink`; falls back to a
# component copy. Uses BADGELINKPORT (defaults to PORT) for the connection:
# host:port -> --tcp, plain path -> --port.
BADGELINK_CLONE_SH   := badgelink/tools/badgelink.sh
BADGELINK_LOCAL_SH   := components/badgeteam__badgelink/tools/badgelink.sh
BADGELINK_MANAGED_SH := managed_components/badgeteam__badgelink/tools/badgelink.sh

.PHONY: mode_debug
mode_debug:
	if [ -x "$(BADGELINK_CLONE_SH)" ]; then \
		BL_SH="$(BADGELINK_CLONE_SH)"; \
	elif [ -x "$(BADGELINK_LOCAL_SH)" ]; then \
		BL_SH="$(BADGELINK_LOCAL_SH)"; \
	elif [ -x "$(BADGELINK_MANAGED_SH)" ]; then \
		BL_SH="$(BADGELINK_MANAGED_SH)"; \
	else \
		echo "badgelink.sh not found, run 'make badgelink' first"; \
		exit 1; \
	fi; \
	echo "Using $$BL_SH"; \
	"$$BL_SH" $(BADGELINK_CONN) mode debug

.PHONY: openocd
openocd:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) openocd

.PHONY: openocdftdi
openocdftdi:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) openocd --openocd-commands "-f board/esp32p4-ftdi.cfg"

.PHONY: gdb
gdb:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) gdb

.PHONY: gdbgui
gdbgui:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) gdbgui

.PHONY: gdbtui
gdbtui:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) gdbtui

# Tools

.PHONY: size
size:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) size

.PHONY: size-components
size-components:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) size-components

.PHONY: size-files
size-files:
	source "$(IDF_SOURCE)" && idf.py $(IDF_PARAMS) size-files

.PHONY: efuse
efuse:
	$(IDF_PATH)/components/efuse/efuse_table_gen.py --idf_target esp32p4 $(IDF_PATH)/components/efuse/esp32p4/esp_efuse_table.csv main/esp_efuse_custom_table.csv

# Formatting

.PHONY: format
format:
	find main/ -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' | xargs clang-format -i

# Re-compile protobuf files
# If you are an end user, you do not need to run this;
# the output files are already there in the repository.

.PHONY: compile-protobuf
compile-protobuf:
	protoc --pyi_out=tools --python_out=tools badgelink.proto
	python3 main/badgelink/nanopb/generator/nanopb_generator.py -D main/badgelink -f badgelink.options badgelink.proto

# Take all svg files from main/static/icons and put them in main/fat/icons as png using tools/connvert.sh
ICONS_SRC := $(wildcard main/static/icons/*.svg)
ICONS_DST := $(patsubst main/static/icons/%.svg,main/fat/icons/%.png,$(ICONS_SRC))

.PHONY: icons
icons: $(ICONS_DST)

main/fat/icons/%.png: main/static/icons/%.svg
	mkdir -p main/fat/icons
	tools/convert.sh $< $@
	
# Build all targets
.PHONY: buildall
buildall:
	$(MAKE) build DEVICE=tanmatsu
	$(MAKE) build DEVICE=esp32-p4-function-ev-board
	$(MAKE) build DEVICE=mch2022
	$(MAKE) build DEVICE=hackaday2025
	$(MAKE) build DEVICE=hackerhotel-2024
	$(MAKE) build DEVICE=heltecv3
	$(MAKE) build DEVICE=kami

# Vscode
.PHONY: vscode
vscode:
	rm -rf .vscode
	cp -r .vscode.template .vscode
