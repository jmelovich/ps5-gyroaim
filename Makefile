# GyroAim
#
#   make            build the service and the control panel
#   make release    build both from clean and print the archive hash
#   make test       the host-side tests
#   make deploy     install the panel on the console, which bundles the service
#
# PS5_HOST sets the console's address.

PS5_HOST ?= 192.168.0.192
TITLE    := PPSA99510

.PHONY: all service app release test deploy clean setup

all: service app

setup:
	@tools/setup-deps.sh
	@tools/setup-app-deps.sh

service:
	@$(MAKE) --no-print-directory -C payload

app:
	@$(MAKE) --no-print-directory -C app dist

test:
	@$(MAKE) --no-print-directory -C tests check

deploy:
	@$(MAKE) --no-print-directory -C app deploy PS5_HOST=$(PS5_HOST)

release: clean
	@$(MAKE) --no-print-directory -C tests check
	@$(MAKE) --no-print-directory -C app dist
	@echo
	@tools/check-release.sh
	@echo
	@echo "archive: app/dist/$(TITLE).zip"
	@du -h app/dist/$(TITLE).zip | cut -f1 | sed 's/^/size:    /'
	@sha256sum app/dist/$(TITLE).zip | cut -d" " -f1 | sed 's/^/sha256:  /'
	@echo
	@echo "contents:"
	@cd app/dist/$(TITLE) && find . -type f | sed 's|^\./|  |' | sort
	@echo
	@echo "the bundled service, which a catalog maintainer approves by hash:"
	@sha256sum app/dist/$(TITLE)/gyroaim.elf | cut -d" " -f1 | sed 's/^/  /'

clean:
	@$(MAKE) --no-print-directory -C payload clean
	@$(MAKE) --no-print-directory -C app clean
	@$(MAKE) --no-print-directory -C tests clean
