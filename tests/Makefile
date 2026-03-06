# tests/Makefile — build and run the XWin stub test
#
# Usage:
#   make          — build xwin_stub
#   make test     — run with LD_PRELOAD=../cyxv.dll
#   make test-hook — run with hook path (needs rva_init_extensions in config)

CC      := gcc
CFLAGS  := -O0 -g -Wall -Wextra
LDFLAGS := -rdynamic    # export all symbols so cyxv's dlsym can find them

STUB    := xwin_stub
CYXV    := ../cyxv.dll

.PHONY: all test test-hook clean

all: $(STUB)

$(STUB): xwin_stub.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lpthread

# ── Thread path test (no config, sleep-2 route) ──────────────────────
test: $(STUB) $(CYXV)
	@echo
	@echo "=== Thread path (no hook, sleep-2) ==="
	@echo "Expect: hook NOT installed, init thread runs after 2s sleep"
	@echo
	DISPLAY=:0 LD_PRELOAD=$(abspath $(CYXV)) ./$(STUB)

# ── Hook path test (reads rva_init_extensions from config) ───────────
#
# The stub exports InitExtensions() at a runtime VA.
# Find it with:   nm -D ./xwin_stub | grep InitExtensions
# Then:           base = first address in /proc/<pid>/maps for xwin_stub
#                 RVA  = VA - base
# Write to config: echo "rva_init_extensions = <RVA>" >> ~/.config/cyxv.conf
test-hook: $(STUB) $(CYXV)
	@echo
	@echo "=== Hook path ==="
	@echo "Finding InitExtensions RVA for this build…"
	@VA=$$(nm -D ./$(STUB) 2>/dev/null | awk '/\bInitExtensions\b/{print $$1}'); \
	 if [ -z "$$VA" ]; then \
	   echo "nm -D failed; try: objdump -t xwin_stub | grep InitExtensions"; \
	   exit 1; \
	 fi; \
	 VA=0x$$VA; \
	 echo "  InitExtensions VA (preferred load) = $$VA"; \
	 echo "  (ASLR will shift this at runtime — see note below)"; \
	 echo; \
	 echo "NOTE: xwin_stub is a PIE binary.  At runtime the actual VA"; \
	 echo "differs.  Run once without hook, then check /proc/<pid>/maps"; \
	 echo "and recalculate RVA = runtime_VA - runtime_base."; \
	 echo; \
	 DISPLAY=:0 LD_PRELOAD=$(abspath $(CYXV)) ./$(STUB)

clean:
	rm -f $(STUB)
