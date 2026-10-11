# GyroAim payload
#
# make                 build gyroaim.elf
# make deploy          build, then send it to the console's ELF loader
# make PS5_HOST=1.2.3.4 deploy

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

PS5_PAYLOAD_SDK ?= $(ROOT)/.deps/ps5-payload-sdk
ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error no SDK at $(PS5_PAYLOAD_SDK); run tools/setup-deps.sh)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PS5_HOST ?= 192.168.0.192
PS5_PORT ?= 9021

ELF := gyroaim.elf

# Portal port. Deliberately not 8080: 8090 is Puckbridge's and 10101 is
# ShadowMount+'s, and a common port invites collisions with whatever else the
# console is running.
GYROAIM_PORT ?= 6080

# The self test briefly feeds the transform an artificial turn to prove it
# writes the stick. That nudges the camera, so keep it off for anything but
# deliberate validation.
GA_SELFTEST ?= 0

# The injected stub is copied byte for byte into another process, so it must be
# position independent.
CFLAGS  := -Wall -Wextra -Werror -O2 -g -std=gnu11 -fPIC \
           -fno-stack-protector \
           -D__PROSPERO__ -DGYROAIM_PORT=$(GYROAIM_PORT) -DGA_SELFTEST=$(GA_SELFTEST)

# The SDK wrapper already supplies crt1.o, -lc, -lkernel_web and the Sce
# dependencies; kernel_*/klog_* live in crt1.o.
LDFLAGS := -lScePad -lSceUserService -lpthread -ldl

SRC := main.c log.c proc.c pad.c hook.c kstuff.c stub.c inject.c config.c web.c
OBJ := $(SRC:%.c=build/%.o)

.PHONY: all clean deploy check-stub

all: $(ELF) check-stub

$(ELF): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	@mkdir -p build

# The stub block is copied into another process, so it must not reference
# anything outside itself. Any relocation in its section means a reference to
# .rodata or to another symbol, which would point into open space once copied.
# Most often it is a float literal, or a negation pulling in a sign mask.
check-stub: build/stub.o
	@tool=$$(command -v llvm-objdump || command -v objdump) || \
	    { echo "need objdump or llvm-objdump to check the stub" >&2; exit 1; }; \
	relocs=$$($$tool -r $< \
	    | awk '/RELOCATION RECORDS FOR \[.text.gastub\]/{f=1;next} \
	           /^RELOCATION RECORDS/{f=0} \
	           f && /R_X86_64/{print}'); \
	if [ -n "$$relocs" ]; then \
	    echo "stub block has external references:"; echo "$$relocs"; \
	    exit 1; \
	fi; \
	echo "stub block is self-contained"

clean:
	rm -rf build $(ELF)

deploy: $(ELF)
	@$(ROOT)/tools/deploy.py $(PS5_HOST) $(ELF) $(PS5_PORT)
