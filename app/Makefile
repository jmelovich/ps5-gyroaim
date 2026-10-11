# GyroAim control panel (a PS5 title).
#
# The title is built by the pinned native-app boilerplate, which expects to be
# the project root and refuses absolute paths, so our tree is linked into it as
# "gyroaim" by tools/setup-app-deps.sh and referenced relatively from there.

ROOT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
BP    := $(ROOT)/.deps/ps5-native-app-boilerplate
TITLE := PPSA99510

PS5_HOST ?= 192.168.0.192

# The boilerplate looks for clang-18 by name; PS5_CLANG overrides that. Host
# clang 22 is what steamlink-ps5 builds its title with.
HOST_CLANG := $(shell command -v clang-18 || command -v clang)

# Exercises the sandbox-bound operations on launch and reports to the kernel
# log, because buttons cannot be pressed from a development machine.
GA_PANEL_SELFTEST ?= 0

BUILD_ENV := PS5_CLANG=$(HOST_CLANG) \
             APP_DEFINITIONS=GA_PANEL_SELFTEST=$(GA_PANEL_SELFTEST) \
             APP_SOURCE_DIR=gyroaim/src \
             APP_PARAM=gyroaim/sce_sys/param.json \
             APP_SCE_SYS=gyroaim/sce_sys \
             APP_ASSETS= \
             APP_ROOT_FILES=gyroaim/gyroaim.elf \
             USE_CCACHE=0

.PHONY: all service clean deploy dist

all: dist

# The service ships inside the title, so the panel can install it on a console
# that has never had it.
service:
	@$(MAKE) --no-print-directory -C $(ROOT)/payload
	@cp $(ROOT)/payload/gyroaim.elf $(ROOT)/app/gyroaim.elf

dist: service
	@test -d $(BP) || { echo "run tools/setup-app-deps.sh first" >&2; exit 2; }
	@test -e $(BP)/gyroaim || { echo "run tools/setup-app-deps.sh first" >&2; exit 2; }
	@cd $(BP) && $(BUILD_ENV) bash tools/build.sh Folder
	@mkdir -p $(ROOT)/app/dist
	@cp -r $(BP)/dist/$(TITLE) $(BP)/dist/$(TITLE).zip $(ROOT)/app/dist/ 2>/dev/null || true
	@echo "title at app/dist/$(TITLE)"

deploy: dist
	@$(ROOT)/tools/deploy-app.py $(PS5_HOST) $(ROOT)/app/dist/$(TITLE)

clean:
	rm -rf $(ROOT)/app/dist $(ROOT)/app/gyroaim.elf
	rm -rf $(BP)/build $(BP)/dist
