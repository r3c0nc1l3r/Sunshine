#!/usr/bin/env bash
# shellcheck disable=SC2016
#
# Build the Sunshine fork's Arch package inside a disposable archlinux:base-devel
# container, so the build host's own packages are never touched.
#
# Usage:
#   build_arch_pkg.sh --commit <sha> --base-version <upstream-version> --release <n> --out <dir>
#                     [--clone-url <url>] [--cuda] [--no-tests] [--jobs <n>]
#
#   --commit        Commit to build (7-40 hex digits). It must be pushed to the clone URL.
#   --base-version  Upstream nightly version the fork is based on, e.g. 2026.914.233613.
#   --release       Fork release number on that base (positive integer). pkgver becomes
#                   <base-version>.<release>.
#   --out           Output directory. Created if missing. Receives the package, the test
#                   results and coverage XML when present, BUILDINFO.txt and SHA256SUMS.
#   --clone-url     Repository the PKGBUILD clones (https only). Default:
#                   https://github.com/r3c0nc1l3r/Sunshine.git
#   --cuda          Build with CUDA (installs the cuda package in the container).
#   --no-tests      Skip the unit tests (no Xvfb, no coverage).
#   --jobs          Parallel build jobs, 1-8 (default 8). Also the container CPU limit.
#
# Runs on an Arch build host with docker. The host itself only runs docker; all
# pacman, makepkg and build work happens inside the container, which is removed on exit.
# The package path is printed on success.

set -euo pipefail

IMAGE="archlinux:base-devel"
DEFAULT_CLONE_URL="https://github.com/r3c0nc1l3r/Sunshine.git"

usage() {
  cat >&2 <<'USAGE'
Usage: build_arch_pkg.sh --commit <sha> --base-version <upstream-version> --release <n> --out <dir>
                         [--clone-url <url>] [--cuda] [--no-tests] [--jobs <n>]
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

COMMIT=""
BASE_VERSION=""
RELEASE=""
OUT=""
CLONE_URL="$DEFAULT_CLONE_URL"
CUDA=false
TESTS=true
JOBS=8

while [[ $# -gt 0 ]]; do
  case "$1" in
    --commit)
      [[ $# -ge 2 ]] || die "--commit needs a value"
      COMMIT="$2"
      shift 2
      ;;
    --base-version)
      [[ $# -ge 2 ]] || die "--base-version needs a value"
      BASE_VERSION="$2"
      shift 2
      ;;
    --release)
      [[ $# -ge 2 ]] || die "--release needs a value"
      RELEASE="$2"
      shift 2
      ;;
    --out)
      [[ $# -ge 2 ]] || die "--out needs a value"
      OUT="$2"
      shift 2
      ;;
    --clone-url)
      [[ $# -ge 2 ]] || die "--clone-url needs a value"
      CLONE_URL="$2"
      shift 2
      ;;
    --cuda)
      CUDA=true
      shift
      ;;
    --no-tests)
      TESTS=false
      shift
      ;;
    --jobs)
      [[ $# -ge 2 ]] || die "--jobs needs a value"
      JOBS="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      usage
      die "unknown argument: $1"
      ;;
  esac
done

[[ -n "$COMMIT" ]] || die "--commit is required"
[[ -n "$BASE_VERSION" ]] || die "--base-version is required"
[[ -n "$RELEASE" ]] || die "--release is required"
[[ -n "$OUT" ]] || die "--out is required"

[[ "$COMMIT" =~ ^[0-9a-fA-F]{7,40}$ ]] || die "--commit must be 7-40 hex digits"
[[ "$BASE_VERSION" =~ ^[0-9]+(\.[0-9]+)+$ ]] || die "--base-version must look like 2026.914.233613"
[[ "$RELEASE" =~ ^[1-9][0-9]*$ ]] || die "--release must be a positive integer"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] && ((JOBS <= 8)) || die "--jobs must be an integer from 1 to 8"
[[ "$CLONE_URL" =~ ^https://[^[:space:]]+$ ]] || die "--clone-url must be an https URL without spaces"

command -v docker > /dev/null 2>&1 || die "docker is not installed or not in PATH"

mkdir -p -- "$OUT"
OUT_ABS="$(cd -- "$OUT" && pwd)"

# The container runs as root and writes into /out. Outputs are chowned back to the invoking user.
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

# Inner script, run as root inside the container. Values come in as environment variables.
read -r -d '' INNER_SCRIPT <<'INNER_EOF' || true
set -euo pipefail

log() {
  printf '\n==> %s\n' "$*"
}

die() {
  echo "error: $*" >&2
  exit 1
}

src=/home/builder/Sunshine
pkgdir=/home/builder/pkg

log "Refresh keyring and update the container system"
pacman -Syy --disable-download-timeout --needed --noconfirm archlinux-keyring
pacman -Syu --disable-download-timeout --noconfirm

pkgs=(base-devel cmake git namcap sudo xorg-server-xvfb)
if [[ "$CUDA" == true ]]; then
  pkgs+=(cuda)
fi
pacman -S --disable-download-timeout --needed --noconfirm "${pkgs[@]}"

log "Create builder user (makepkg refuses to run as root)"
useradd -m builder
printf 'builder ALL=(ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/builder
chmod 0440 /etc/sudoers.d/builder

log "Set makepkg parallelism to ${JOBS} jobs"
printf 'MAKEFLAGS="-j%s"\n' "$JOBS" >> /etc/makepkg.conf

log "Clone ${CLONE_URL}"
sudo -H -u builder git clone --no-checkout -- "$CLONE_URL" "$src"
sudo -H -u builder git -C "$src" checkout --detach "$COMMIT"
FULL_COMMIT="$(sudo -H -u builder git -C "$src" rev-parse HEAD)"
case "$FULL_COMMIT" in
  "${COMMIT,,}"*) ;;
  *) die "checked out ${FULL_COMMIT}, which does not match ${COMMIT}" ;;
esac

log "Configure PKGBUILD for ${BASE_VERSION}.${RELEASE}"
sudo -H -u builder mkdir -p "$src/build"
sudo -H -u builder env \
  BRANCH=lanternbox \
  BUILD_VERSION="$BASE_VERSION" \
  CLONE_URL="$CLONE_URL" \
  COMMIT="$FULL_COMMIT" \
  bash -c 'cd "$1/build" && cmake -DSUNSHINE_CONFIGURE_ONLY=ON -DSUNSHINE_CONFIGURE_PKGBUILD=ON -DSUNSHINE_SUB_VERSION=".$2" ..' \
  _ "$src" "$RELEASE"

log "Prepare package directory"
sudo -H -u builder mkdir -p "$pkgdir"
sudo -H -u builder cp "$src/build/PKGBUILD" "$src/build/sunshine.install" "$pkgdir/"
sudo -H -u builder bash -c 'cd "$1" && makepkg --printsrcinfo > .SRCINFO' _ "$pkgdir"
PKGVER="$(sed -n '/^[[:space:]]*pkgver = /{s///p;q;}' "$pkgdir/.SRCINFO")"
[[ -n "$PKGVER" ]] || die "could not read pkgver from .SRCINFO"
echo "pkgver: ${PKGVER}"

log "namcap check (warnings allowed)"
sudo -H -u builder bash -c 'cd "$1" && namcap -i PKGBUILD' _ "$pkgdir" \
  || echo "namcap reported problems; continuing"

if [[ "$TESTS" == true ]]; then
  log "Start Xvfb on :1 for headless tests"
  Xvfb :1 -screen 0 1024x768x24 > /tmp/xvfb.log 2>&1 &
  sleep 2
fi

log "Build package with makepkg"
sudo -H -u builder env \
  _use_cuda="$CUDA" \
  _run_unit_tests="$TESTS" \
  _support_headless_testing="$TESTS" \
  DISPLAY=:1 \
  bash -c 'cd "$1" && nice -n 19 makepkg -s --noconfirm' _ "$pkgdir"

log "Collect artifacts into /out"
mapfile -t built < <(find "$pkgdir" -maxdepth 1 -name '*.pkg.tar.zst' ! -name 'sunshine-debug*' -printf '%f\n' | sort)
((${#built[@]} > 0)) || die "makepkg produced no package"
outputs=()
for f in "${built[@]}"; do
  cp -- "$pkgdir/$f" /out/
  outputs+=("$f")
done

if [[ "$TESTS" == true ]]; then
  for f in "$pkgdir/src/build/coverage.xml" "$pkgdir/src/build/tests/test_results.xml"; do
    if [[ -f "$f" ]]; then
      cp -- "$f" /out/
      outputs+=("$(basename -- "$f")")
    fi
  done
fi

{
  echo "commit: ${FULL_COMMIT}"
  echo "base_version: ${BASE_VERSION}"
  echo "release: ${RELEASE}"
  echo "pkgver: ${PKGVER}"
  echo "package: ${built[*]}"
  echo "cuda: $([[ "$CUDA" == true ]] && echo on || echo off)"
  echo "tests: $([[ "$TESTS" == true ]] && echo on || echo off)"
  echo "built_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > /out/BUILDINFO.txt
outputs+=(BUILDINFO.txt)

(cd /out && sha256sum -- "${outputs[@]}" > SHA256SUMS && chown "$HOST_UID:$HOST_GID" -- "${outputs[@]}" SHA256SUMS)
INNER_EOF

echo "Building ${BASE_VERSION}.${RELEASE} from ${COMMIT} (cuda ${CUDA}, tests ${TESTS}, jobs ${JOBS})"

docker run --rm \
  --cpus "$JOBS" \
  -v "$OUT_ABS:/out" \
  -e COMMIT="$COMMIT" \
  -e BASE_VERSION="$BASE_VERSION" \
  -e RELEASE="$RELEASE" \
  -e CLONE_URL="$CLONE_URL" \
  -e CUDA="$CUDA" \
  -e TESTS="$TESTS" \
  -e JOBS="$JOBS" \
  -e HOST_UID="$HOST_UID" \
  -e HOST_GID="$HOST_GID" \
  "$IMAGE" \
  bash -c "$INNER_SCRIPT"

pkg_names="$(sed -n 's/^package: //p' "$OUT_ABS/BUILDINFO.txt")"
echo "Package: $OUT_ABS/${pkg_names%% *}"
