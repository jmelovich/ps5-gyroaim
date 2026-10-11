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

# The injected stub is copied byte for byte into another process, so it must be
# position independent.
CFLAGS  := -Wall -Wextra -Werror -O2 -g -std=gnu11 -fPIC \
           -fno-stack-protector \
           -D__PROSPERO__ -DGYROAIM_PORT=$(GYROAIM_PORT)

# The SDK wrapper already supplies crt1.o, -lc, -lkernel_web and the Sce
# dependencies; kernel_*/klog_* live in crt1.o.
LDFLAGS := -lpthread -ldl

SRC := main.c log.c proc.c
OBJ := $(SRC:%.c=build/%.o)

.PHONY: all clean deploy

all: $(ELF)

$(ELF): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	@mkdir -p build

clean:
	rm -rf build $(ELF)

deploy: $(ELF)
	@$(ROOT)/tools/deploy.py $(PS5_HOST) $(ELF) $(PS5_PORT)
