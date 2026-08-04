CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
           -Wstrict-prototypes -Wpointer-arith -D_FORTIFY_SOURCE=2 \
           -fstack-protector-strong
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

# external timeout: kills the harness if it does not finish on its own
TEST_TIMEOUT ?= 60

TARGET  := k70debounce
TESTBIN := test_k70debounce

FIXTURE        ?= /tmp/k70debounce-fixture.bin
FIXTURE_DROPS  := 11

.PHONY: all clean test test-asan regress fixture install uninstall

all: $(TARGET)

$(TARGET): k70debounce.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# sanity build with sanitizers, for testing only (do not install)
$(TARGET)-asan: k70debounce.c
	$(CC) -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined \
	      -o $@ $< $(LDFLAGS)

$(TESTBIN): test_k70debounce.c
	$(CC) $(CFLAGS) -Wno-conversion -o $@ $< $(LDFLAGS)

# Full suite. Internal 45 s watchdog plus an external timeout.
test: $(TARGET) $(TESTBIN)
	@timeout -k 5 $(TEST_TIMEOUT) ./$(TESTBIN); \
	rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "EXTERNAL TIMEOUT after $(TEST_TIMEOUT)s"; fi; \
	exit $$rc

# Deterministic synthetic stream with exactly FIXTURE_DROPS chatter presses.
# No real captured data: the original logs were a keylog of real typing.
fixture:
	@python3 gen_fixture.py "$(FIXTURE)"

regress: $(TARGET) fixture
	@n=$$(timeout -k 2 20 ./$(TARGET) --offline --hold 26 --verbose \
	      < "$(FIXTURE)" 2>&1 >/dev/null | grep -c '^k70debounce: dropped '); \
	if [ "$$n" = "$(FIXTURE_DROPS)" ]; then \
	  echo "  PASS  regression on synthetic fixture: $(FIXTURE_DROPS) events dropped"; \
	else \
	  echo "  FAIL  regression on synthetic fixture: expected $(FIXTURE_DROPS), got $$n"; exit 1; \
	fi

test-asan: $(TARGET)-asan fixture
	@timeout -k 2 30 ./$(TARGET)-asan --offline --hold 26 \
	    < "$(FIXTURE)" >/dev/null && echo "  PASS  ASAN/UBSAN clean"

clean:
	rm -f $(TARGET) $(TARGET)-asan $(TESTBIN)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
