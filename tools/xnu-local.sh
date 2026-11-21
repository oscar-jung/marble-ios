#!/usr/bin/env bash
# Build the SWEET-config xnu (ios-sweet patches + xnu/marble-patches) on this Mac.
#
#   tools/xnu-local.sh [marble|sweet] [--quick]
#
# Same recipe as .github/workflows/xnu.yml (blacktop/darwin-xnu-build at a
# pinned commit), in build-local/dxb-<board>/ (gitignored). The first run
# builds everything (dependencies, headers, kernel). --quick afterwards applies
# patches that are new since the last run and re-runs only the kernel make
# step, which is incremental.
#
# Env: DEVELOPER_DIR (default: xcode-select), KDKROOT (default:
# build-local/KDK_13.5_22G74.kdk, unpacked without sudo; see docs).
# Output: build-local/out-<board>/kernel.development.sweet
set -euo pipefail

BOARD="${1:-marble}"
QUICK=0
[ "${2:-}" = "--quick" ] && QUICK=1
case "$BOARD" in
	marble) BOARD_FLAGS="-DSWEET_BOARD_MARBLE" ;;
	sweet) BOARD_FLAGS="" ;;
	*) echo "usage: $0 [marble|sweet] [--quick]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BL="$ROOT/build-local"
DXB="$BL/dxb-$BOARD"
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549

export MACOS_VERSION=13.5
export KERNEL_CONFIG=DEVELOPMENT
export MACHINE_CONFIG=SWEET
export KDKROOT="${KDKROOT:-$BL/KDK_13.5_22G74.kdk}"
export DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"
export SWEET_BOARD_FLAGS="$BOARD_FLAGS"
export MAKEFLAGS="${MAKEFLAGS:--k}"

[ -d "$KDKROOT/System/Library/KernelSupport" ] || { echo "KDK not found at $KDKROOT" >&2; exit 1; }
echo "board=$BOARD quick=$QUICK xcode=$DEVELOPER_DIR kdk=$KDKROOT flags='$SWEET_BOARD_FLAGS'"

if [ ! -d "$DXB/.git" ]; then
	git init -q "$DXB"
	git -C "$DXB" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
	git -C "$DXB" checkout -q FETCH_HEAD
	# libfirehose_kernel is built arm64e-only; the kernel here is plain arm64
	sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' "$DXB/build.sh"
fi
cp "$ROOT"/third_party/ios-sweet/xnu/patches/*.patch "$ROOT"/third_party/ios-sweet/xnu/patches/*.sh "$DXB/patches/"
cp "$ROOT"/xnu/marble-patches/*.patch "$DXB/patches/"

cd "$DXB"
if [ "$QUICK" = 1 ] && [ -d xnu ] && [ -d build ]; then
	# Patches applied since the last run (build.sh's own loop does the same check).
	for p in "$ROOT"/xnu/marble-patches/*.patch; do
		if git -C xnu apply --check "$p" 2>/dev/null; then
			echo "applying new patch $(basename "$p")"
			git -C xnu apply "$p"
		fi
	done
	# build.sh with every step but the toolchain/xnu choice and the kernel build turned off.
	sed -e 's/^    get_xnu$/    : get_xnu/' -e 's/^    patches$/    : patches/' -e 's/^    venv$/    : venv/' \
	    -e 's/^    build_bootstrap_cmds$/    : build_bootstrap_cmds/' -e 's/^    build_dtrace$/    : build_dtrace/' \
	    -e 's/^    build_availabilityversions$/    : build_availabilityversions/' \
	    -e 's/^    xnu_headers$/    : xnu_headers/' -e 's/^    libsystem_headers$/    : libsystem_headers/' \
	    -e 's/^    libsyscall_headers$/    : libsyscall_headers/' -e 's/^    build_libplatform$/    : build_libplatform/' \
	    -e 's/^    build_libdispatch$/    : build_libdispatch/' \
	    -e 's|^    if \[ ! -f "${BUILD_DIR}/xnu.obj/kernel.${KERNEL_TYPE}" \]; then$|    if true; then  # quick: always run the incremental make|' \
	    build.sh > build-quick.sh
	grep -q 'if true; then  # quick' build-quick.sh || { echo "build.sh changed: cannot make build-quick.sh" >&2; exit 1; }
	/opt/homebrew/bin/bash build-quick.sh
else
	/opt/homebrew/bin/bash build.sh
fi

OUT="$BL/out-$BOARD"
mkdir -p "$OUT"
cp -f fakeroot/System/Library/Kernels/kernel.development.sweet "$OUT/"
(cd "$OUT" && shasum -a 256 kernel.development.sweet > SHA256SUMS)
ls -l "$OUT"
