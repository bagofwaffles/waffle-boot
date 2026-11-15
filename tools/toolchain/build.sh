#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fetch and build the bfin-elf cross toolchain (binutils + C-only GCC)
# from the public Blackfin+ GNU toolchain fork, pinned to one commit.
#
# Usage: tools/toolchain/build.sh [--print-prefix | --print-commit]
#
# Environment:
#   WAFFLE_TOOLCHAIN_CACHE  cache root (default:
#                           ${XDG_CACHE_HOME:-~/.cache}/waffle-boot/toolchain)
#   JOBS                    parallel make jobs (default: nproc)
#
# The toolchain is installed into $WAFFLE_TOOLCHAIN_CACHE/bfin-elf-<commit>;
# the Makefile finds it there (or pass CROSS_COMPILE=<dir>/bin/bfin-elf- to make).

set -euo pipefail

TOOLCHAIN_REPO=https://github.com/deanm1278/blackfin-plus-gnu
TOOLCHAIN_COMMIT=e4408a15b9be6cd911283f103a1a3f69e9a6b8c0
TARGET=bfin-elf

CACHE=${WAFFLE_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/waffle-boot/toolchain}
PREFIX=$CACHE/$TARGET-$TOOLCHAIN_COMMIT
WORK=$CACHE/work-$TOOLCHAIN_COMMIT
STAMP=$PREFIX/.waffle-toolchain-complete
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}
DARWIN=$([ "$(uname -s)" = Darwin ] && echo 1 || echo 0)

case "${1:-}" in
	--print-prefix) echo "$PREFIX"; exit 0 ;;
	--print-commit) echo "$TOOLCHAIN_COMMIT"; exit 0 ;;
	"") ;;
	*) echo "usage: $0 [--print-prefix | --print-commit]" >&2; exit 2 ;;
esac

if [ -f "$STAMP" ]; then
	echo "toolchain already built: $PREFIX"
	exit 0
fi

log() { printf '==> %s\n' "$*"; }

# Fetch only the pinned commit and only the two trees we build.
fetch() {
	local src=$WORK/src
	if [ -f "$src/.waffle-fetched" ]; then
		return
	fi
	rm -rf "$src"
	mkdir -p "$src"
	log "fetching $TOOLCHAIN_REPO @ $TOOLCHAIN_COMMIT"
	git -C "$src" init -q
	git -C "$src" remote add origin "$TOOLCHAIN_REPO"
	git -C "$src" config core.sparseCheckout true
	printf '/binutils-gdb/\n/gcc/\n' > "$src/.git/info/sparse-checkout"
	git -C "$src" fetch -q --depth 1 origin "$TOOLCHAIN_COMMIT"
	git -C "$src" checkout -q FETCH_HEAD
	test "$(git -C "$src" rev-parse HEAD)" = "$TOOLCHAIN_COMMIT"
	# binutils' install-sh is committed without its execute bit; configure
	# falls back to it where mkdir -p is not GNU's (macOS).
	find "$src" -name install-sh -exec chmod +x {} +
	touch "$src/.waffle-fetched"
}

# These 2017-era sources predate current host compilers; relax the host
# language defaults rather than patching the sources.  Documentation is
# skipped (MAKEINFO=true) because newer texinfo rejects the old manuals.
HOST_CFLAGS="-O2 -fcommon"
GCC_DEPS=()
if [ "$DARWIN" = 1 ]; then
	# The bundled 2017 zlib defines fdopen() away on any Mac target, which
	# breaks the declaration in current SDK headers.  In CFLAGS, not
	# CPPFLAGS: only CFLAGS reaches the subdirectories' configure runs.
	HOST_CFLAGS="$HOST_CFLAGS -Dfdopen=fdopen"
	# GMP, MPFR and MPC from Homebrew.
	if command -v brew >/dev/null 2>&1; then
		brew_prefix=$(brew --prefix)
		GCC_DEPS=(--with-gmp="$brew_prefix" --with-mpfr="$brew_prefix"
			  --with-mpc="$brew_prefix")
	fi
fi
HOST_ENV=(
	CFLAGS="$HOST_CFLAGS -std=gnu11"
	CXXFLAGS="$HOST_CFLAGS -std=gnu++14"
	MAKEINFO=true
)

build_binutils() {
	local b=$WORK/build-binutils
	rm -rf "$b"
	mkdir -p "$b"
	log "configuring binutils"
	(cd "$b" && env "${HOST_ENV[@]}" "$WORK/src/binutils-gdb/configure" \
		--target=$TARGET --prefix="$PREFIX" \
		--disable-nls --disable-werror \
		--disable-gdb --disable-gdbserver --disable-sim --disable-readline \
		--disable-libdecnumber --disable-gold --disable-gprof)
	log "building binutils (-j$JOBS)"
	make -C "$b" -j"$JOBS" MAKEINFO=true
	make -C "$b" install MAKEINFO=true
}

build_gcc() {
	local b=$WORK/build-gcc
	rm -rf "$b"
	mkdir -p "$b"
	log "configuring gcc (C only, no libc)"
	(cd "$b" && env "${HOST_ENV[@]}" PATH="$PREFIX/bin:$PATH" \
		"$WORK/src/gcc/configure" \
		--target=$TARGET --prefix="$PREFIX" \
		--enable-languages=c --without-headers --with-newlib \
		--disable-nls --disable-werror --disable-bootstrap \
		--disable-shared --disable-threads --disable-libssp \
		--disable-libquadmath --disable-libgomp --disable-libatomic \
		--disable-decimal-float --disable-libmudflap --disable-lto \
		${GCC_DEPS[@]+"${GCC_DEPS[@]}"})
	if [ "$DARWIN" = 1 ]; then
		# GCC 8 has Darwin host hooks for PowerPC and x86 only; on other
		# Macs (arm64) host_hooks is left undefined. Use the defaults,
		# which only serve precompiled headers.
		PATH="$PREFIX/bin:$PATH" make -C "$b" configure-gcc
		sed -i.orig 's/^host_hook_obj=host-darwin\.o$/host_hook_obj=host-default.o/' \
			"$b/gcc/Makefile"
	fi
	log "building gcc + libgcc (-j$JOBS)"
	PATH="$PREFIX/bin:$PATH" make -C "$b" -j"$JOBS" all-gcc all-target-libgcc MAKEINFO=true
	PATH="$PREFIX/bin:$PATH" make -C "$b" install-gcc install-target-libgcc MAKEINFO=true
}

mkdir -p "$CACHE"
rm -rf "$PREFIX"
fetch
build_binutils
build_gcc
"$PREFIX/bin/$TARGET-gcc" --version | head -n1
echo "$TOOLCHAIN_COMMIT" > "$STAMP"
rm -rf "$WORK"
log "toolchain installed in $PREFIX"
