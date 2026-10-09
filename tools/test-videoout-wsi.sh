#!/usr/bin/env bash
# PS5 Vulkan - host tests of the PS5_Mesa VideoOut WSI (wsi_common_videoout.c).
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds RADV's host model (-Dradv-winsys=ps5 for Linux: a shared ICD whose
# winsys executes nothing but records every command and compiles every
# pipeline, and whose VideoOut refuses the sizes the console refused) from the
# pinned PS5_Mesa revision, or from a working tree given as MESA_TREE, and runs:
#
#   tests/videoout/test_videoout_scale.c      which framebuffer size a
#       swapchain presents into and where its image goes in it
#   tests/videoout/test_videoout_swapchain.c  swapchains of the sizes a game
#       switches between (GTA IV's 1920x1080, 1280x720, 1440x960 and back),
#       4:3, above 1080p and 4K, each presenting frames through the
#       Khronos loader; then checks from the backend's log that only the
#       sizes VideoOut takes were registered, nothing was refused, and every
#       present flipped.
#   tests/videoout/test_videoout_takeover.c   which swapchain has the one
#       display: a swapchain that never presented (wined3d's hidden caps
#       window under Zink) gives it to the next window's; one that presented
#       keeps it.
#
# Needs the Vulkan loader (libvulkan-dev), meson, ninja, and the host clc tools
# tools/build-radv.sh builds. Run the build under a memory cap on a shared
# machine, for example:
#   flock /tmp/heavy-build.lock systemd-run --user --scope -q -p MemoryMax=8G tools/test-videoout-wsi.sh
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mesa_fork="${PS5_MESA_FORK:-$root/../PS5_Mesa}"
mesa_revision=$(sed -n 's/^mesa_revision=//p' "$root/tools/build-radv.sh")
work="${VIDEOOUT_TEST_WORK:-$root/.deps/work/videoout-wsi-test}"
meson=${MESON:-$(command -v meson || echo "$HOME/.local/bin/meson")}
ninja=${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}
jobs=${JOBS:-8}
clc_bin="$root/.deps/work/radv-clc-bin"

[[ -x $meson && -x $ninja ]] || { echo "meson and ninja are needed (uv tool install meson ninja)" >&2; exit 2; }
[[ -x $clc_bin/mesa_clc ]] || { echo "no host clc tools in $clc_bin: run tools/build-radv.sh first" >&2; exit 2; }
export PATH="$clc_bin:$PATH"
mkdir -p "$work"

if [[ -n ${MESA_TREE:-} ]]; then
    source_tree=$(cd -- "$MESA_TREE" && pwd)
    revision="working tree $source_tree"
else
    source_tree="$work/src"
    revision=$mesa_revision
    if [[ ! -f $source_tree/.revision || $(<"$source_tree/.revision") != "$mesa_revision" ]]; then
        git -C "$mesa_fork" cat-file -e "$mesa_revision^{commit}" 2>/dev/null ||
            { echo "the Mesa fork at $mesa_fork does not have $mesa_revision" >&2; exit 2; }
        rm -rf "$source_tree"
        mkdir -p "$source_tree"
        git -C "$mesa_fork" archive "$mesa_revision" | tar -x -C "$source_tree"
        printf '%s\n' "$mesa_revision" > "$source_tree/.revision"
    fi
fi

# The PS5 winsys's AMD_NO_DRM path uses __u64 as FreeBSD's headers give it;
# on Linux it comes from linux/types.h (not in assembly sources).
fix="$work/host-types.h"
printf '#ifndef __ASSEMBLER__\n#include <linux/types.h>\n#endif\n' > "$fix"
build="$work/build"
options=(-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5
    -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs= -Dbuildtype=debugoptimized -Db_ndebug=false
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled
    -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dmesa-clc=system
    "-Dc_args=['-include', '$fix']" "-Dcpp_args=['-include', '$fix']")
if [[ ! -f $build/build.ninja || $(cat "$build/.options" 2>/dev/null) != "$source_tree ${options[*]}" ]]; then
    wipe=()
    [[ -f $build/build.ninja ]] && wipe=(--wipe)
    "$meson" setup "${wipe[@]}" "$build" "$source_tree" "${options[@]}" > "$work/setup.log" 2>&1 ||
        { tail -20 "$work/setup.log" >&2; exit 1; }
    printf '%s\n' "$source_tree ${options[*]}" > "$build/.options"
fi
echo "==> [videoout-wsi] building RADV's host model at $revision"
"$ninja" -C "$build" -j"$jobs" src/amd/vulkan/libvulkan_radeon.so > "$work/build.log" 2>&1 ||
    { grep -E "error|FAILED" "$work/build.log" | head -20 >&2; exit 1; }

cat > "$work/radv_host_icd.json" <<JSON
{"file_format_version": "1.0.1", "ICD": {"library_path": "$build/src/amd/vulkan/libvulkan_radeon.so", "api_version": "1.4.0"}}
JSON

cc=${CC:-cc}
"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror -I"$source_tree/src/vulkan/wsi" -I"$source_tree/include" \
    "$root/tests/videoout/test_videoout_scale.c" -o "$work/test_videoout_scale"
"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror "$root/tests/videoout/test_videoout_swapchain.c" -lvulkan \
    -o "$work/test_videoout_swapchain"
"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror "$root/tests/videoout/test_videoout_takeover.c" -lvulkan \
    -o "$work/test_videoout_takeover"

echo "==> [videoout-wsi] test_videoout_scale"
"$work/test_videoout_scale"

echo "==> [videoout-wsi] test_videoout_swapchain"
log="$work/swapchain.log"
VK_DRIVER_FILES="$work/radv_host_icd.json" VK_ICD_FILENAMES="$work/radv_host_icd.json" \
    MESA_VK_VIDEOOUT_HOST_LOG=1 timeout 600 "$work/test_videoout_swapchain" 2> "$log" ||
    { tail -20 "$log" >&2; echo "test_videoout_swapchain failed; see $log" >&2; exit 1; }

failures=0
expect() {
    local what=$1 want=$2 got=$3
    if [[ $got != "$want" ]]; then
        echo "$what: $got, want $want" >&2
        failures=$((failures + 1))
    fi
}
registered=$(grep -o 'register set [0-9]* buffers [0-9-]* [0-9]*x[0-9]*: 0x[0-9a-f]*' "$log" | sort -u |
    tr '\n' ';')
expect "registrations" "register set 0 buffers 0-4 1920x1080: 0x00000000;register set 1 buffers 5-9 3840x2160: 0x00000000;" \
    "$registered"
expect "refusals" 0 "$(grep -c 'were not registered' "$log" || true)"
# 2 passes of 8 sizes, 8 frames each.
expect "flips" 128 "$(grep -c 'videoout-host: flip' "$log" || true)"
expect "1440x960 placement" 2 \
    "$(grep -c 'a 1440x960 swapchain presents scaled to 1620x1080 at (150, 0) of 1920x1080 framebuffers' "$log" || true)"
expect "1280x720 placement" 2 \
    "$(grep -c 'a 1280x720 swapchain presents scaled to 1920x1080 at (0, 0) of 1920x1080 framebuffers' "$log" || true)"
expect "800x600 placement" 2 \
    "$(grep -c 'a 800x600 swapchain presents scaled to 1440x1080 at (240, 0) of 1920x1080 framebuffers' "$log" || true)"
expect "2560x1440 placement" 2 \
    "$(grep -c 'a 2560x1440 swapchain presents scaled to 3840x2160 at (0, 0) of 3840x2160 framebuffers' "$log" || true)"
expect "1920x1080 and 3840x2160 unscaled" 0 \
    "$(grep -c -E 'a (1920x1080|3840x2160) swapchain presents scaled' "$log" || true)"
if ((failures)); then
    echo "test_videoout_swapchain: $failures check(s) failed; see $log" >&2
    exit 1
fi
echo "test_videoout_swapchain: pass ($revision)"

echo "==> [videoout-wsi] test_videoout_takeover"
log="$work/takeover.log"
VK_DRIVER_FILES="$work/radv_host_icd.json" VK_ICD_FILENAMES="$work/radv_host_icd.json" \
    MESA_VK_VIDEOOUT_HOST_LOG=1 timeout 300 "$work/test_videoout_takeover" 2> "$log" ||
    { tail -20 "$log" >&2; echo "test_videoout_takeover failed; see $log" >&2; exit 1; }
expect "takeovers" 1 "$(grep -c 'a swapchain that never presented gives the display to a new one' "$log" || true)"
# The game's 4 frames, 4 more after the refusals, 4 after its oldSwapchain
# replacement, and 4 from the later window.
expect "takeover flips" 16 "$(grep -c 'videoout-host: flip' "$log" || true)"
if ((failures)); then
    echo "test_videoout_takeover: $failures check(s) failed; see $log" >&2
    exit 1
fi
echo "test_videoout_takeover: pass ($revision)"
