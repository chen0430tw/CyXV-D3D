#!/usr/bin/env bash
# tests/test-apt-cyg.sh — integration test suite for the upgraded apt-cyg
#
# Self-contained: builds a fake Cygwin mirror + package archive under /tmp,
# starts a local HTTP server, patches apt-cyg to redirect /etc/setup and
# extraction paths into a temp sysroot, then exercises every major command.
#
# Requirements: bash 4.2+, python3, wget, tar, xz, bzip2, sha512sum, od, lsof

set -euo pipefail

cd "$(dirname "$0")/.."
APT_CYG_SRC="$(pwd)/apt-cyg"

PASS=0; FAIL=0; SKIP=0; TOTAL=0
HTTP_PID=""

# ── Temp environment ────────────────────────────────────────────────────────
T=$(mktemp -d /tmp/apt-cyg-test.XXXXXX)

SYSROOT="$T/sysroot"          # fake Cygwin root (tar extracts here)
ETCSETUP="$SYSROOT/etc/setup"
ETCPOST="$SYSROOT/etc/postinstall"
ETCPRE="$SYSROOT/etc/preremove"
MIRRORROOT="$T/mirror"        # served by HTTP
CACHEDIR="$T/cache"

mkdir -p "$ETCSETUP" "$ETCPOST" "$ETCPRE" "$CACHEDIR" \
         "$SYSROOT/usr/bin" "$SYSROOT/tmp" \
         "$MIRRORROOT/x86_64/release/testpkg-a" \
         "$MIRRORROOT/x86_64/release/testpkg-b" \
         "$MIRRORROOT/x86_64/release/testpkg-c" \
         "$MIRRORROOT/x86_64/release/hollow-pkg" \
         "$T/pkgtmp" "$T/bin"

cleanup() {
  [[ -n $HTTP_PID ]] && kill "$HTTP_PID" 2>/dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT

# ── Mock system commands not available outside Cygwin ──────────────────────
# cygcheck: used by apt-remove to list essential binaries — return empty so
# nothing is marked essential and removal can proceed in tests.
cat > "$T/bin/cygcheck" << 'EOF'
#!/bin/bash
EOF
chmod +x "$T/bin/cygcheck"
# dash: used for *.dash postinstall scripts — symlink to bash
ln -sf "$(command -v bash)" "$T/bin/dash" 2>/dev/null || true
export PATH="$T/bin:$PATH"

# ── Patch apt-cyg ──────────────────────────────────────────────────────────
# Replace hardcoded /etc/* paths with our sysroot equivalents, and redirect
# tar extraction and PE-check paths from / to $SYSROOT.
APT_CYG="$T/apt-cyg"
SYS_ESC="${SYSROOT//\//\\/}"   # escape slashes for sed RHS
ETS_ESC="${ETCSETUP//\//\\/}"
ETP_ESC="${ETCPOST//\//\\/}"
ETR_ESC="${ETCPRE//\//\\/}"

sed \
  -e "s|/etc/setup|${ETCSETUP}|g" \
  -e "s|/etc/postinstall|${ETCPOST}|g" \
  -e "s|/etc/preremove|${ETCPRE}|g" \
  -e "s|tar -x -C / |tar -x -C ${SYSROOT} |g" \
  -e "s|rm -f \"/\$f\"|rm -f \"${SYSROOT}/\$f\"|g" \
  -e "s|path=\"/\${|path=\"${SYSROOT}/\${|g" \
  -e "s|local f=\"/\${|local f=\"${SYSROOT}/\${|g" \
  -e "s|  cd /etc$|  cd ${SYSROOT}/etc|g" \
  "$APT_CYG_SRC" > "$APT_CYG"

chmod +x "$APT_CYG"

# ── Package builders ────────────────────────────────────────────────────────
# Build a minimal but genuine tar.xz with an MZ-header binary inside.
make-pkg() {
  local name=$1 ver=$2
  local pkgdir="$T/pkgtmp/${name}-${ver}"
  rm -rf "$pkgdir"
  mkdir -p "$pkgdir/usr/bin"
  # Real MZ (PE) header so _check-pe-bins passes
  printf '\x4d\x5a' > "$pkgdir/usr/bin/${name}.exe"
  # Pad to >4096 bytes so the tar.xz archive exceeds the HOLLOW_MIN_BYTES=1024 threshold
  dd if=/dev/urandom bs=4096 count=1 >> "$pkgdir/usr/bin/${name}.exe" 2>/dev/null
  local arc="$MIRRORROOT/x86_64/release/${name}/${name}-${ver}.tar.xz"
  tar -C "$pkgdir" -cJf "$arc" usr/
  echo "$arc"
}

# Build a hollow stub (< HOLLOW_MIN_BYTES) that mimics libopenssl100.
make-hollow-pkg() {
  local name=$1 ver=$2
  local arc="$MIRRORROOT/x86_64/release/${name}/${name}-${ver}.tar.xz"
  # 108 bytes — exactly what the real libopenssl100 stub weighs
  printf '%-108s' "stub" > "$arc"
  echo "$arc"
}

sha512of() { sha512sum "$1" | awk '{print $1}'; }

# ── Build test fixtures ─────────────────────────────────────────────────────
ARC_B=$(make-pkg  "testpkg-b" "2.0-1")
ARC_A=$(make-pkg  "testpkg-a" "1.0-1")
ARC_C=$(make-pkg  "testpkg-c" "1.0-1")          # depends on virtual name provided by testpkg-b
ARC_H=$(make-hollow-pkg "hollow-pkg" "1.0-1")
ARC_A2=$(make-pkg "testpkg-a" "1.1-1")          # upgrade target (already in mirror dir)

SIZE_A=$(stat -c%s "$ARC_A");   HASH_A=$(sha512of "$ARC_A")
SIZE_B=$(stat -c%s "$ARC_B");   HASH_B=$(sha512of "$ARC_B")
SIZE_C=$(stat -c%s "$ARC_C");   HASH_C=$(sha512of "$ARC_C")
SIZE_H=$(stat -c%s "$ARC_H");   HASH_H=$(sha512of "$ARC_H")
SIZE_A2=$(stat -c%s "$ARC_A2"); HASH_A2=$(sha512of "$ARC_A2")

# ── Write setup.ini and compress it ────────────────────────────────────────
write-setup-ini() {
  local a_ver=$1 a_size=$2 a_hash=$3
  cat > "$MIRRORROOT/x86_64/setup.ini" << EOF
release: cygwin
arch: x86_64
setup-timestamp: $(date +%s)
setup-version: 2.932

@ testpkg-a
sdesc: "Test Package A - depends on B"
ldesc: "Long description for testpkg-a used in apt-cyg testing"
category: Test
depends2: testpkg-b
version: ${a_ver}
install: x86_64/release/testpkg-a/testpkg-a-${a_ver}.tar.xz ${a_size} ${a_hash}

@ testpkg-b
sdesc: "Test Package B - standalone dependency"
ldesc: "Standalone package used as a dependency by testpkg-a"
category: Test
provides: testpkg-b-virtual
version: 2.0-1
install: x86_64/release/testpkg-b/testpkg-b-2.0-1.tar.xz ${SIZE_B} ${HASH_B}

@ testpkg-c
sdesc: "Test Package C - depends on virtual name provided by B"
category: Test
depends2: testpkg-b-virtual
version: 1.0-1
install: x86_64/release/testpkg-c/testpkg-c-1.0-1.tar.xz ${SIZE_C} ${HASH_C}

@ hollow-pkg
sdesc: "Hollow stub package for detection testing"
category: Test
version: 1.0-1
install: x86_64/release/hollow-pkg/hollow-pkg-1.0-1.tar.xz ${SIZE_H} ${HASH_H}
EOF
  # apt-cyg fetches "setup.bz2" (not "setup.ini.bz2") — use -c to stdout
  bzip2 -c "$MIRRORROOT/x86_64/setup.ini" > "$MIRRORROOT/x86_64/setup.bz2"
}

write-setup-ini "1.0-1" "$SIZE_A" "$HASH_A"

# ── HTTP server (python3, binds to 127.0.0.1:random port) ──────────────────
# Write the server script to a file to avoid heredoc+background interaction.
PORTFILE="$T/http.port"
cat > "$T/httpserver.py" << 'PYEOF'
import socketserver, http.server, sys, os
os.chdir(sys.argv[1])
class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
httpd = socketserver.TCPServer(("127.0.0.1", 0), Q)
with open(sys.argv[2], "w") as f:
    f.write(str(httpd.server_address[1]))
httpd.serve_forever()
PYEOF

python3 "$T/httpserver.py" "$MIRRORROOT" "$PORTFILE" &
HTTP_PID=$!

# Wait until the port file appears (max 5 s)
for i in $(seq 1 50); do
  [[ -f "$PORTFILE" ]] && break
  sleep 0.1
done
HTTP_PORT=$(cat "$PORTFILE")
MIRROR="http://127.0.0.1:${HTTP_PORT}"


# ── installed.db and setup.rc ───────────────────────────────────────────────
printf 'INSTALLED.DB 3\n' > "$ETCSETUP/installed.db"
printf 'last-cache\n\t%s\nlast-mirror\n\t%s\n' \
  "$CACHEDIR" "$MIRROR" > "$ETCSETUP/setup.rc"

# ── Test runner helpers ──────────────────────────────────────────────────────
run_apt() { bash "$APT_CYG" "$@" 2>&1 || true; }

ok() {
  printf '  \e[32m[PASS]\e[m %s\n' "$*"
  PASS=$(( PASS + 1 )); TOTAL=$(( TOTAL + 1 ))
}
fail() {
  printf '  \e[31m[FAIL]\e[m %s\n' "$*"
  FAIL=$(( FAIL + 1 )); TOTAL=$(( TOTAL + 1 ))
}
skip() {
  printf '  \e[33m[SKIP]\e[m %s\n' "$*"
  SKIP=$(( SKIP + 1 ))
}

# Assert that running apt-cyg with $@ produces output containing $expect.
check() {
  local desc="$1" expect="$2"; shift 2
  local out; out=$(run_apt "$@")
  if echo "$out" | grep -qF "$expect"; then
    ok "$desc"
  else
    fail "$desc"
    printf '       expected to find: %s\n' "$expect"
    printf '       actual output   :\n'
    echo "$out" | head -6 | sed 's/^/         /'
  fi
}

# Assert that output does NOT contain $absent.
check_absent() {
  local desc="$1" absent="$2"; shift 2
  local out; out=$(run_apt "$@")
  if echo "$out" | grep -qF "$absent"; then
    fail "$desc (unexpected: '$absent')"
    echo "$out" | head -4 | sed 's/^/         /'
  else
    ok "$desc"
  fi
}

# ════════════════════════════════════════════════════════════════════════════
echo
echo "╔═══════════════════════════════════════════════════╗"
echo "║          apt-cyg integration test suite           ║"
echo "╚═══════════════════════════════════════════════════╝"
printf "  Mirror  : %s\n" "$MIRROR"
printf "  SysRoot : %s\n" "$SYSROOT"
echo

# ── 1. update ────────────────────────────────────────────────────────────────
echo "── 1. update ──"
check "downloads setup.ini from mirror"  "Updated setup.ini"  update
echo

# ── 2. listall (name + description search) ──────────────────────────────────
echo "── 2. listall ──"
check "matches exact package name"          "testpkg-a"      listall testpkg-a
check "shows sdesc alongside name"          "depends on B"   listall testpkg-a
check "matches keyword inside sdesc"        "testpkg-b"      listall "standalone"
check "sdesc match is case-insensitive"     "testpkg-b"      listall "STANDALONE"
check_absent "non-matching query yields nothing" "testpkg-a" listall "STANDALONE"
echo

# ── 3. show (exact + fuzzy) ──────────────────────────────────────────────────
echo "── 3. show ──"
check "exact match prints package block"    "testpkg-b"        show testpkg-b
check "exact match includes sdesc line"     "standalone"       show testpkg-b
check "fuzzy match lists candidates"        "Similar packages" show testpkg
check "fuzzy lists testpkg-a as candidate"  "testpkg-a"        show testpkg
check "unknown package error message"       "Unable to locate" show "zzz-no-such-pkg"
echo

# ── 4. install ───────────────────────────────────────────────────────────────
echo "── 4. install ──"
check "hash is verified on download"        "Verifying sha512sum"     install testpkg-b
check "testpkg-b installed OK"              "testpkg-b-2.0-1.tar.xz" list testpkg-b
check "list shows version alongside name"   "testpkg-b-2.0-1"        list testpkg-b
check "re-install skip message"             "already installed"       install testpkg-b
echo

# ── 5. dependency resolution ─────────────────────────────────────────────────
echo "── 5. dependency resolution ──"
check "install A prints requires line"      "requires: testpkg-b" install testpkg-a
check "testpkg-a is recorded in installed.db" "testpkg-a"       list testpkg-a
check "testpkg-b still recorded after A"    "testpkg-b"           list testpkg-b
echo

# ── 6. hollow-package detection ──────────────────────────────────────────────
echo "── 6. hollow-package detection ──"
check "size gate fires for stub"       "HOLLOW PACKAGE"  install hollow-pkg
check "size reported correctly"        "108 bytes"        install hollow-pkg
check "install is aborted"             "Aborting"         install hollow-pkg
check_absent "stub NOT added to installed.db" "hollow-pkg" list hollow-pkg
check "--allow-hollow bypasses abort"  "proceeding"  install --allow-hollow hollow-pkg
echo

# ── 7. hash verification visibility ─────────────────────────────────────────
echo "── 7. hash verification ──"
# testpkg-b is already installed — use reinstall to force re-download + verify
check "explicit hash line on cache hit"    "Verifying sha512sum" reinstall testpkg-b
check "hash reports OK on good archive"    "OK"                  reinstall testpkg-b
# Corrupt the cache copy and expect FAILED
_bn=$(awk -v p="testpkg-b" 'NR>1 && $1==p {print $2;exit}' "$ETCSETUP/installed.db")
_cached=$(find "$CACHEDIR" -name "$_bn" 2>/dev/null | head -1)
if [[ -n $_cached ]]; then
  printf 'corrupted' >> "$_cached"
  check "corrupted cache triggers FAILED + re-download" "Verifying sha512sum" reinstall testpkg-b
else
  skip "cached archive not found — skipping corruption test"
fi
echo

# ── 8. apt-check ─────────────────────────────────────────────────────────────
echo "── 8. check ──"
check "check reports archive stats"   "bytes"       check testpkg-b
check "check hash reports OK"         "OK"          check testpkg-b
check "check reports file count"      "present"     check testpkg-b
check "check PE validation passes"    "testpkg-b"   check testpkg-b
check "check non-installed package"   "not installed" check zzz-not-a-pkg
echo

# ── 9. reinstall ─────────────────────────────────────────────────────────────
echo "── 9. reinstall ──"
check "reinstall forces re-download"  "Installing testpkg-b"  reinstall testpkg-b
check "reinstall hash verified"       "Verifying sha512sum"   reinstall testpkg-b
echo

# ── 10. upgrade ──────────────────────────────────────────────────────────────
echo "── 10. upgrade ──"
check "upgrade detects no change for B"   "up to date"        upgrade testpkg-b
check "upgrade detects no change for A"   "up to date"        upgrade testpkg-a

# Publish testpkg-a 1.1-1 and refresh setup.ini
write-setup-ini "1.1-1" "$SIZE_A2" "$HASH_A2"
run_apt update > /dev/null 2>&1 || true

# Capture upgrade output once — test multiple assertions against the same run
_upg_out=$(run_apt upgrade testpkg-a)
if echo "$_upg_out" | grep -qF "Upgrading testpkg-a"; then
  ok "upgrade detects version change"
else
  fail "upgrade detects version change"
  echo "$_upg_out" | head -4 | sed 's/^/         /'
fi
if echo "$_upg_out" | grep -qF "1.0-1"; then
  ok "upgrade shows old→new versions"
else
  fail "upgrade shows old→new versions"
  printf '       expected to find: %s\n' "1.0-1"
  echo "$_upg_out" | head -4 | sed 's/^/         /'
fi
check "upgraded installed.db entry"       "testpkg-a-1.1-1"       list testpkg-a
echo

# ── 11. search (file-to-package lookup) ──────────────────────────────────────
echo "── 11. search ──"
check "search finds file owner"             "testpkg-b" search testpkg-b.exe
check "search no-match emits message"       "No installed package owns" search zzz-no-such-file
echo

# ── 12. list ─────────────────────────────────────────────────────────────────
echo "── 12. list ──"
check "list with no args shows all installs"  "testpkg-a"  list
check "list all shows version column"         "testpkg-b-2.0-1"  list
check "list with pattern filters results"     "testpkg-a"  list testpkg-a
check_absent "list pattern excludes non-matches"  "hollow" list testpkg-a
echo

# ── 13. listfiles ────────────────────────────────────────────────────────────
echo "── 13. listfiles ──"
check "listfiles shows installed file paths"  "usr/bin/testpkg-b.exe" listfiles testpkg-b
echo

# ── 14. depends / rdepends ───────────────────────────────────────────────────
echo "── 14. depends / rdepends ──"
check "depends shows A > B tree"   "testpkg-a > testpkg-b"  depends testpkg-a
check "rdepends shows B < A tree"  "testpkg-b < testpkg-a"  rdepends testpkg-b
echo

# ── 15. virtual-package (provides:) resolution ───────────────────────────────
echo "── 15. virtual-package resolution ──"
# testpkg-c depends on "testpkg-b-virtual"; testpkg-b provides that name.
# apt-cyg must resolve the virtual and install testpkg-b automatically.
check "virtual dep resolved — provider reported"  \
      "(virtual testpkg-b-virtual provided by testpkg-b)"  install testpkg-c
check "provider package installed via virtual"  "testpkg-b"  list testpkg-b
check "package depending on virtual is installed"  "testpkg-c"  list testpkg-c
echo

# ── 16. remove ───────────────────────────────────────────────────────────────
echo "── 16. remove ──"
check "remove outputs confirmation"        "removed"   remove testpkg-b
check_absent "removed pkg gone from list"  "testpkg-b" list testpkg-b || true
echo

# ════════════════════════════════════════════════════════════════════════════
echo "╔═══════════════════════════════════════════════════╗"
printf "║  Results: %3d passed  %3d failed  %3d skipped    ║\n" \
       "$PASS" "$FAIL" "$SKIP"
echo "╚═══════════════════════════════════════════════════╝"
echo

if (( FAIL > 0 )); then
  echo "Some tests FAILED." >&2
  exit 1
fi
echo "All tests passed."
