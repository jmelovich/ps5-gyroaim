# Host-side tests. These compile the service's own logic with the host
# compiler, so they run anywhere and need no console.
CC      ?= cc
CFLAGS  := -Wall -Wextra -O1 -g -std=gnu11
BIN     := curve_test transform_test

.PHONY: all clean check

all: $(BIN)

curve_test: curve_test.c ../payload/config.c ../payload/log.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

transform_test: transform_test.c ../payload/config.c ../payload/log.c \
                ../payload/stub.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

check: all
	./curve_test
	./transform_test

clean:
	rm -f $(BIN)
