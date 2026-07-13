TARGET := kxo

# kbuild pass (entered via `$(MAKE) -C $(KDIR) M=$(CURDIR) modules`)
ifneq ($(KERNELRELEASE),)

obj-m := $(TARGET).o
$(TARGET)-objs := src/main.o src/game.o src/xoroshiro.o src/mcts.o src/negamax.o
$(TARGET)-objs += src/zobrist.o src/rl.o src/rl-state.o src/ai_sched.o

ccflags-y := -std=gnu99 -Wno-declaration-after-statement -I$(src)/include

# Top-level pass (invoked by the user directly)
else

UNAME_S := $(shell uname -s)

KDIR ?= /lib/modules/$(shell uname -r)/build

GIT_HOOKS := .git/hooks/applied

XO_CPPFLAGS := -Iinclude
XO_CFLAGS   := -g

USER_OBJS := user/xo-user.o user/tui.o user/coro.o

VANITY_BASE := 0000e59602509f70319e2e4b915fcf1b9a1e2476
VANITY_PREFIX := 0000

.PHONY: all kmod check-unit check cppcheck clean check-hashes check-commitlog

ifneq ($(UNAME_S),Linux)
define REQUIRE_LINUX
	$(error This target requires a Linux host (detected: $(UNAME_S)))
endef
endif

ifeq ($(UNAME_S),Linux)
all: $(GIT_HOOKS) check-hashes kmod xo-user
else
all: $(GIT_HOOKS) check-hashes
	@echo "Non-Linux host detected ($(UNAME_S)). Only git hooks were installed."
	@echo "Build and test require a Linux environment."
endif

kmod: src/main.c
	$(REQUIRE_LINUX)
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

xo-user: $(USER_OBJS)
	$(REQUIRE_LINUX)
	$(CC) $(LDFLAGS) -o $@ $^

user/%.o: user/%.c
	$(REQUIRE_LINUX)
	$(CC) $(CPPFLAGS) $(XO_CPPFLAGS) $(CFLAGS) $(XO_CFLAGS) -c -o $@ $<

# Conservative header dependencies for userspace objects
$(USER_OBJS): include/coro.h include/game.h include/tui.h

UNIT_TESTS := tests/test-game tests/test-coro

tests/test-game: tests/test-game.c tests/common.h include/game.h src/game.c include/util.h
	$(REQUIRE_LINUX)
	$(CC) -std=gnu99 -Wall -Wextra -g -Itests -Iinclude -o $@ $<

tests/test-coro: tests/test-coro.c tests/common.h user/coro.c include/coro.h
	$(REQUIRE_LINUX)
	$(CC) -std=gnu99 -Wall -Wextra -g -Iinclude -o $@ tests/test-coro.c user/coro.c

check-unit: $(UNIT_TESTS)
	$(REQUIRE_LINUX)
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

check: check-unit all
	$(REQUIRE_LINUX)
	@sudo tests/test-integration.sh
	@echo ""
	@echo "All tests passed."

cppcheck:
	@scripts/cppcheck.sh

check-commitlog:
	@scripts/check-commitlog.sh

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

check-hashes:
	@if git cat-file -e $(VANITY_BASE) 2>/dev/null; then \
	    bad=$$(git rev-list --no-merges $(VANITY_BASE)..HEAD | \
	        grep -v '^$(VANITY_PREFIX)'); \
	    if [ -n "$$bad" ]; then \
	        echo "Error: the following commits do not start with '$(VANITY_PREFIX)':"; \
	        for h in $$bad; do \
	            echo "  $$h $$(git show -s --format=%s $$h)"; \
	        done; \
	        echo ""; \
	        echo "Git hooks were not installed correctly."; \
	        echo "Run 'scripts/install-git-hooks' and amend the problematic commits."; \
	        exit 1; \
	    fi; \
	fi

clean:
ifeq ($(UNAME_S),Linux)
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
	$(RM) xo-user $(USER_OBJS) $(UNIT_TESTS)

endif # KERNELRELEASE
