#!/usr/bin/env bash
#
# Blind tests for the optional setcap in the Arch package (fork issue #46).
#
# Covers the install script (setcap skipped when the marker file exists), the PKGBUILD
# package() marker (_setcap=false), and the build script flag (--no-setcap -> SETCAP=false).
#
# Standalone: runs in a mktemp work dir that is removed on exit. Stub commands (setcap,
# udevadm, modprobe, cmake, pacman, docker) come first on PATH and only record their calls.
# Each sourced script runs in its own subshell so its functions and variables do not leak.
#
# Usage: tests/scripts/test_arch_packaging.sh
# Exit status: 0 when every test passes, 1 otherwise.

set -u

REPO="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
INSTALL_SCRIPT="$REPO/packaging/linux/Arch/sunshine.install"
PKGBUILD="$REPO/packaging/linux/Arch/PKGBUILD"
BUILD_SCRIPT="$REPO/scripts/lanternbox/build_arch_pkg.sh"

TMP="$(mktemp -d)" || exit 1
cleanup() {
  if [[ -n "${TMP:-}" && -d "$TMP" ]]; then
    rm -rf -- "$TMP"
  fi
}
trap cleanup EXIT

STUB_BIN="$TMP/bin"
STUB_LOG="$TMP/calls"
mkdir -p -- "$STUB_BIN" "$STUB_LOG"
export STUB_LOG

# Stubs record one line per call into $STUB_LOG/<name>. They never run the real tools.
for name in setcap udevadm modprobe cmake pacman; do
  cat > "$STUB_BIN/$name" <<'STUB_EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_LOG/$(basename -- "$0")"
STUB_EOF
  chmod +x "$STUB_BIN/$name"
done
# pacman is queried by PKGBUILD (cuda detection); report "not installed".
cat > "$STUB_BIN/pacman" <<'STUB_EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$STUB_LOG/pacman"
exit 1
STUB_EOF
# docker records its argument list, one argument per line, and fails.
cat > "$STUB_BIN/docker" <<'STUB_EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$STUB_LOG/docker.args"
exit 1
STUB_EOF
chmod +x "$STUB_BIN/docker"

export PATH="$STUB_BIN:$PATH"

OUT_TXT=""
RC=0
T_MSG=""
PASSED=0
FAILED=0
TOTAL=0

# check_docker_stub: fail unless `docker` inside this test resolves to the stub.
check_docker_stub() {
  local resolved
  resolved="$(command -v docker 2>/dev/null)" || resolved=""
  if [[ "$resolved" != "$STUB_BIN/docker" ]]; then
    T_MSG="docker resolves to '${resolved:-nothing}', not the test stub"
    return 1
  fi
  return 0
}

if ! check_docker_stub; then
  echo "FAIL guard: $T_MSG (refusing to run)"
  exit 1
fi

for required in "$INSTALL_SCRIPT" "$PKGBUILD" "$BUILD_SCRIPT"; do
  if [[ ! -f "$required" ]]; then
    echo "FAIL guard: missing $required"
    exit 1
  fi
done

reset_calls() {
  rm -rf -- "$STUB_LOG"
  mkdir -p -- "$STUB_LOG"
}

# calls <name>: print the recorded calls of a stub.
calls() {
  if [[ -f "$STUB_LOG/$1" ]]; then
    cat -- "$STUB_LOG/$1"
  fi
}

# call_count <name>: number of recorded calls of a stub.
call_count() {
  if [[ -f "$STUB_LOG/$1" ]]; then
    wc -l < "$STUB_LOG/$1" | tr -d ' '
  else
    echo 0
  fi
}

# run_install <root> <function>: source the install script in a subshell with <root> as cwd
# and call the function. Sets OUT_TXT (stdout and stderr) and RC.
run_install() {
  local root=$1 fn=$2
  OUT_TXT="$(cd -- "$root" && set +u && . "$INSTALL_SCRIPT" && "$fn" 2>&1)"
  RC=$?
}

# run_pkgbuild <setcap value | unset> <pkgdir>: source PKGBUILD in a subshell (with _setcap
# set to the value, or unset), then call package() with a stub cmake. Sets OUT_TXT and RC.
# Fails if sourcing the PKGBUILD by itself ran any command (only variables and functions).
run_pkgbuild() {
  local mode=$1 target=$2
  rm -f -- "$STUB_LOG/cmake"
  OUT_TXT="$(
    set +u
    unset _setcap
    if [[ "$mode" != unset ]]; then
      _setcap="$mode"
    fi
    . "$PKGBUILD"
    if [[ -e "$STUB_LOG/cmake" ]]; then
      echo "sourcing PKGBUILD ran cmake"
      exit 1
    fi
    cmake() { printf '%s\n' "$*" >> "$STUB_LOG/cmake"; }
    pkgdir="$target"
    package 2>&1
  )"
  RC=$?
}

# run_build [extra args]: run the build script with stub docker; the fixed arguments are the
# minimum valid set. Sets OUT_TXT and RC. The stub docker always exits 1.
run_build() {
  local out="$TMP/out-$RANDOM$RANDOM"
  OUT_TXT="$(bash "$BUILD_SCRIPT" --commit 0123456 --base-version 2026.914.233613 --release 1 \
    --out "$out" "$@" 2>&1)"
  RC=$?
}

# docker_has_env <KEY=VALUE>: true when the docker stub got "-e KEY=VALUE".
docker_has_env() {
  local want=$1 i
  local -a args=()
  if [[ ! -f "$STUB_LOG/docker.args" ]]; then
    return 1
  fi
  mapfile -t args < "$STUB_LOG/docker.args"
  for ((i = 0; i < ${#args[@]} - 1; i++)); do
    if [[ "${args[i]}" == -e && "${args[i + 1]}" == "$want" ]]; then
      return 0
    fi
  done
  return 1
}

# make_root <dir> [marker]: fake package root with usr/bin/sunshine, optionally with the
# setcap-disabled marker file.
make_root() {
  local dir=$1 marker=${2:-no}
  mkdir -p -- "$dir/usr/bin"
  : > "$dir/usr/bin/sunshine"
  if [[ "$marker" == yes ]]; then
    mkdir -p -- "$dir/usr/share/sunshine"
    : > "$dir/usr/share/sunshine/setcap-disabled"
  fi
}

# Install script: the default package still runs setcap on the binary.
test_install_default_runs_setcap() {
  local root="$TMP/root-default"
  make_root "$root" no
  run_install "$root" post_install
  if [[ $RC -ne 0 ]]; then
    T_MSG="post_install returned $RC: $OUT_TXT"
    return 1
  fi
  if [[ "$(call_count setcap)" -ne 1 ]]; then
    T_MSG="setcap called $(call_count setcap) times, expected 1"
    return 1
  fi
  local line
  line="$(calls setcap)"
  if [[ "$line" != "cap_sys_admin,cap_sys_nice+p "*/usr/bin/sunshine ]]; then
    T_MSG="unexpected setcap arguments: $line"
    return 1
  fi
  if [[ "$(call_count udevadm)" -eq 0 ]]; then
    T_MSG="udevadm was not called"
    return 1
  fi
  if ! calls modprobe | grep -qx 'uhid'; then
    T_MSG="modprobe uhid was not called"
    return 1
  fi
  return 0
}

# Install script: with the marker, post_install and post_upgrade skip setcap but still run
# udev reload and modprobe, and report the skip.
marker_case() {
  local fn=$1 root="$TMP/root-marker-$1"
  make_root "$root" yes
  run_install "$root" "$fn"
  if [[ $RC -ne 0 ]]; then
    T_MSG="$fn returned $RC: $OUT_TXT"
    return 1
  fi
  if [[ "$(call_count setcap)" -ne 0 ]]; then
    T_MSG="$fn called setcap with the marker present"
    return 1
  fi
  if [[ "$(call_count udevadm)" -eq 0 ]]; then
    T_MSG="$fn did not call udevadm"
    return 1
  fi
  if ! calls modprobe | grep -qx 'uhid'; then
    T_MSG="$fn did not call modprobe uhid"
    return 1
  fi
  if [[ "$OUT_TXT" != *"setcap skipped"* ]]; then
    T_MSG="$fn did not print a 'setcap skipped' line: $OUT_TXT"
    return 1
  fi
  return 0
}

test_install_marker_skips_setcap_post_install() {
  marker_case post_install
}

test_install_marker_skips_setcap_post_upgrade() {
  marker_case post_upgrade
}

# PKGBUILD: _setcap=false makes package() write the marker file.
test_pkgbuild_setcap_false_writes_marker() {
  local pkgdir="$TMP/pkg-false"
  mkdir -p -- "$pkgdir"
  run_pkgbuild false "$pkgdir"
  if [[ $RC -ne 0 ]]; then
    T_MSG="package() returned $RC: $OUT_TXT"
    return 1
  fi
  if [[ ! -f "$pkgdir/usr/share/sunshine/setcap-disabled" ]]; then
    T_MSG="marker file not created"
    return 1
  fi
  return 0
}

# PKGBUILD: the default (unset) and _setcap=true packages do not write the marker.
test_pkgbuild_default_and_true_write_no_marker() {
  local unset_dir="$TMP/pkg-unset" true_dir="$TMP/pkg-true"
  mkdir -p -- "$unset_dir" "$true_dir"
  run_pkgbuild unset "$unset_dir"
  if [[ $RC -ne 0 ]]; then
    T_MSG="package() with default returned $RC: $OUT_TXT"
    return 1
  fi
  if [[ -e "$unset_dir/usr/share/sunshine/setcap-disabled" ]]; then
    T_MSG="marker created with _setcap unset"
    return 1
  fi
  run_pkgbuild true "$true_dir"
  if [[ $RC -ne 0 ]]; then
    T_MSG="package() with _setcap=true returned $RC: $OUT_TXT"
    return 1
  fi
  if [[ -e "$true_dir/usr/share/sunshine/setcap-disabled" ]]; then
    T_MSG="marker created with _setcap=true"
    return 1
  fi
  return 0
}

# Build script: --help documents --no-setcap.
test_build_help_lists_no_setcap() {
  OUT_TXT="$(bash "$BUILD_SCRIPT" --help 2>&1)"
  RC=$?
  if [[ $RC -ne 0 ]]; then
    T_MSG="--help returned $RC"
    return 1
  fi
  if [[ "$OUT_TXT" != *"--no-setcap"* ]]; then
    T_MSG="--help does not mention --no-setcap"
    return 1
  fi
  return 0
}

# Build script: --no-setcap reaches docker as SETCAP=false.
test_build_no_setcap_passes_setcap_false() {
  check_docker_stub || return 1
  run_build --no-setcap
  if [[ ! -f "$STUB_LOG/docker.args" ]]; then
    T_MSG="docker was not called: $OUT_TXT"
    return 1
  fi
  if ! docker_has_env SETCAP=false; then
    T_MSG="docker was not given '-e SETCAP=false'"
    return 1
  fi
  return 0
}

# Build script: without the flag, docker gets SETCAP=true.
test_build_default_passes_setcap_true() {
  check_docker_stub || return 1
  run_build
  if [[ ! -f "$STUB_LOG/docker.args" ]]; then
    T_MSG="docker was not called: $OUT_TXT"
    return 1
  fi
  if ! docker_has_env SETCAP=true; then
    T_MSG="docker was not given '-e SETCAP=true'"
    return 1
  fi
  if docker_has_env SETCAP=false; then
    T_MSG="docker was given SETCAP=false without --no-setcap"
    return 1
  fi
  return 0
}

# run_test <name> <function>: run one test with a clean stub log and report PASS or FAIL.
run_test() {
  local name=$1 fn=$2 rc=0
  T_MSG=""
  reset_calls
  "$fn" || rc=$?
  TOTAL=$((TOTAL + 1))
  if [[ $rc -eq 0 ]]; then
    echo "PASS $name"
    PASSED=$((PASSED + 1))
  else
    echo "FAIL $name${T_MSG:+ ($T_MSG)}"
    FAILED=$((FAILED + 1))
  fi
}

run_test install_default_runs_setcap test_install_default_runs_setcap
run_test install_marker_skips_setcap_post_install test_install_marker_skips_setcap_post_install
run_test install_marker_skips_setcap_post_upgrade test_install_marker_skips_setcap_post_upgrade
run_test pkgbuild_setcap_false_writes_marker test_pkgbuild_setcap_false_writes_marker
run_test pkgbuild_default_and_true_write_no_marker test_pkgbuild_default_and_true_write_no_marker
run_test build_help_lists_no_setcap test_build_help_lists_no_setcap
run_test build_no_setcap_passes_setcap_false test_build_no_setcap_passes_setcap_false
run_test build_default_passes_setcap_true test_build_default_passes_setcap_true

echo "Summary: $PASSED passed, $FAILED failed, $TOTAL total"
if [[ $FAILED -ne 0 ]]; then
  exit 1
fi
exit 0
