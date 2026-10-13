# Host-side tests. These compile the service's own logic with the host
# compiler, so they run anywhere and need no console.
CC      ?= cc
CFLAGS  := -Wall -Wextra -O1 -g -std=gnu11
BIN     := curve_test transform_test qr_test config_test

.PHONY: all clean check

all: $(BIN)

curve_test: curve_test.c ../payload/config.c ../payload/log.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

transform_test: transform_test.c ../payload/config.c ../payload/log.c \
                ../payload/stub.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

qr_test: qr_test.c ../app/src/vendor/qrcodegen.c
	$(CC) $(CFLAGS) -o $@ $^

# The settings layering reads real files, so it gets a directory of its own
# rather than the console's /data, which does not exist here.
config_test: config_test.c ../payload/config.c ../payload/log.c
	$(CC) $(CFLAGS) -DGA_DIR='"$(CURDIR)/build/conf"' -o $@ $^ -lm

check: all
	@mkdir -p build/conf && rm -f build/conf/*.txt
	./config_test
	./curve_test
	./transform_test
	./qr_test
	@# The portal's script is checked against a stub DOM where node exists.
	@if command -v node >/dev/null; then \
	    node page_test.js; \
	else \
	    echo "node not found, skipping the page check"; \
	fi

clean:
	rm -f $(BIN) qr-sample.pgm qr-sample.png
