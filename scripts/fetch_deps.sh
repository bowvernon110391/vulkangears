#!/usr/bin/env bash
#
# fetch_deps.sh - vendor the missing build/runtime dependencies of vulkangears
#                 into ./third_party/ WITHOUT needing root.
#
# The demo needs, but this machine does not have installed:
#   * Vulkan headers + loader stub   (libvulkan-dev, libvulkan1)
#   * a GLSL -> SPIR-V compiler      (glslang-tools, spirv-tools)
#   * a window/input library         (libglfw3-dev, libglfw3)
#   * optionally the Khronos validation layers for the debug output
#
# Instead of "apt-get install" (which needs root) we download the .deb files and
# unpack them into a private sysroot with "dpkg-deb -x".  CMake links against
# that sysroot and gives the executable an $ORIGIN-relative rpath, so the demo
# runs from the build tree with no environment variables and no root.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEB_DIR="$ROOT/third_party/debs"
SYSROOT="$ROOT/third_party/sysroot"
LIBDIR="$SYSROOT/usr/lib/x86_64-linux-gnu"

# Packages we unpack.  Core system libraries (libc6, libstdc++6, libgcc-s1) are
# deliberately NOT vendored - they are already present on the system and
# vendoring them could confuse the dynamic loader.
PACKAGES=(
    libvulkan-dev      # vk* headers + libvulkan.so linker symlink
    libvulkan1         # the loader itself (so the symlink resolves)
    glslang-tools      # glslangValidator: GLSL -> SPIR-V at build time
    spirv-tools        # libraries glslangValidator links against
    libglfw3-dev       # GLFW headers
    libglfw3           # GLFW shared library
    vulkan-validationlayers  # VK_LAYER_KHRONOS_validation + debug utils output
)

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v apt-get  >/dev/null || die "apt-get not found; this script supports Debian/Ubuntu hosts"
command -v dpkg-deb >/dev/null || die "dpkg-deb not found (install dpkg)"
command -v g++      >/dev/null || die "g++ not found (install build-essential)"

mkdir -p "$DEB_DIR" "$SYSROOT"

# ---------------------------------------------------------------------------
# 1. download the .deb files (cached in third_party/debs)
# ---------------------------------------------------------------------------
missing=()
for pkg in "${PACKAGES[@]}"; do
    # any version of this package already cached?
    if ! ls "$DEB_DIR/${pkg}"_*.deb >/dev/null 2>&1; then
        missing+=("$pkg")
    fi
done

if [ ${#missing[@]} -gt 0 ]; then
    say "downloading: ${missing[*]}"
    ( cd "$DEB_DIR" && apt-get download "${missing[@]}" ) \
        || die "apt-get download failed (need a working apt sources.list + network)"
else
    say "all packages already cached in third_party/debs"
fi

# ---------------------------------------------------------------------------
# 2. unpack every .deb into the private sysroot
# ---------------------------------------------------------------------------
say "unpacking into third_party/sysroot"
for deb in "$DEB_DIR"/*.deb; do
    [ -e "$deb" ] || die "no .deb files in $DEB_DIR"
    dpkg-deb -x "$deb" "$SYSROOT"
done

# ---------------------------------------------------------------------------
# 3. make the validation layer loadable without LD_LIBRARY_PATH
#
#    Ubuntu's layer manifest names the library by bare soname; the Vulkan
#    loader dlopen()s it with the *loader's* search path, which does not
#    include our private sysroot.  Rewriting library_path to an absolute path
#    means setting VK_LAYER_PATH alone is enough (the demo does that itself).
# ---------------------------------------------------------------------------
shopt -s nullglob
for manifest in "$SYSROOT"/usr/share/vulkan/explicit_layer.d/*.json; do
    lib="$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)"
    case "$lib" in
        /*) ;;                                  # already absolute
        "")
            warn "no library_path in $manifest" ;;
        *)
            if [ -f "$LIBDIR/$lib" ]; then
                sed -i "s|\"library_path\"[[:space:]]*:[[:space:]]*\"$lib\"|\"library_path\" : \"$LIBDIR/$lib\"|" "$manifest"
                say "patched $(basename "$manifest") -> $LIBDIR/$lib"
            else
                warn "layer library $lib referenced by $(basename "$manifest") not found in $LIBDIR"
            fi ;;
    esac
done
shopt -u nullglob

# ---------------------------------------------------------------------------
# 4. checks
# ---------------------------------------------------------------------------
fail=0
check() { # <description> <path>
    if [ -e "$2" ]; then printf '    ok   %-42s %s\n' "$1" "$2"
    else printf '    MISS %-42s %s\n' "$1" "$2"; fail=1; fi
}
say "verifying vendored files"
check "vulkan headers"        "$SYSROOT/usr/include/vulkan/vulkan.h"
check "vulkan loader"         "$LIBDIR/libvulkan.so"
check "GLFW headers"          "$SYSROOT/usr/include/GLFW/glfw3.h"
check "GLFW library"          "$LIBDIR/libglfw.so"
check "glslangValidator"      "$SYSROOT/usr/bin/glslangValidator"
if ls "$SYSROOT"/usr/share/vulkan/explicit_layer.d/*validation*.json >/dev/null 2>&1; then
    printf '    ok   %-42s %s\n' "validation layer manifest" "$SYSROOT/usr/share/vulkan/explicit_layer.d"
else
    printf '    --   %-42s (not vendored, validation will be disabled)\n' "validation layer manifest"
fi

say "checking that the vendored glslangValidator runs"
if LD_LIBRARY_PATH="$LIBDIR" "$SYSROOT/usr/bin/glslangValidator" --version; then
    printf '    ok   glslangValidator is runnable\n'
else
    warn "glslangValidator could not run; shader compilation may fail"
    fail=1
fi

say "checking the vendored GLFW can be resolved by the linker"
if echo 'int main(void){return 0;}' | g++ -x c++ - -o /tmp/.vkgears_glfw_probe \
        -I"$SYSROOT/usr/include" -L"$LIBDIR" -lglfw -lvulkan 2>/dev/null; then
    printf '    ok   -lglfw -lvulkan link fine\n'
    rm -f /tmp/.vkgears_glfw_probe
else
    warn "linking against the vendored libraries failed"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    die "dependency setup is incomplete"
fi

say "done - now configure and build with CMake:"
printf '\n    cmake -S . -B build && cmake --build build -j\n\n'
