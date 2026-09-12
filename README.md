# vulkangears

A small [glxgears](https://en.wikipedia.org/wiki/Glxgears)-style demo written directly against
the **Vulkan** API in **C++11** (no C++14/17, no framework, no GLM).

It opens a resizable 800x600 window and draws three checker-textured gears of
different sizes that really mesh: a 30-tooth driver, a 14-tooth idler and a
22-tooth output gear, spinning at speeds coupled by their tooth counts.
The console prints a full Vulkan report (API version, device, driver, memory
heaps, the app's own allocations) and a live FPS / frame-time / memory counter.

![vulkangears running](docs/screenshot.png)

*(actual window capture, running on the software rasteriser in this container -
on a machine with a GPU the device line will name that GPU instead)*

---

## Building

CMake is the build system; there is no Makefile.

```sh
cmake -S . -B build            # configure
cmake --build build -j         # build (add --config Release on multi-config generators)
ctest --test-dir build         # optional: the GPU-free geometry checks
./build/vulkangears            # run
```

### Linux without root

This machine has no system Vulkan/GLFW development packages, so
`scripts/fetch_deps.sh` vendors them first (Debian/Ubuntu, x86_64). It needs
**no root**: it downloads the `-dev` packages with `apt-get download` and unpacks
them into `third_party/sysroot`, which CMake then links against with an
`$ORIGIN`-relative rpath. Nothing is installed system-wide.

```sh
./scripts/fetch_deps.sh        # one-off; re-run any time to top the sysroot up
cmake -S . -B build && cmake --build build -j
```

If the sysroot is absent CMake falls back to the system Vulkan SDK and GLFW
(`find_package(glfw3)`, `vcpkg`, `conan`, or a system install); if GLFW cannot be
found at all it is downloaded and built automatically (`-DVKG_FETCH_GLFW=OFF`
disables that).

### Windows (Visual Studio)

Prerequisites:

1. **Vulkan SDK** from [vulkan.lunarg.com](https://vulkan.lunarg.com/) - gives
   `vulkan-1.lib`, the `vulkan/vulkan.h` headers and `glslangValidator.exe`.
   CMake finds it through the `VULKAN_SDK` environment variable.
2. **CMake 3.16+** and **Visual Studio 2019/2022** with the "Desktop development
   with C++" workload.
3. **GLFW** - either let CMake fetch it (`git` must be on `PATH`), or install it
   with `vcpkg install glfw3:x64-windows`.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\vulkangears.exe
```

For a MinGW/Ninja build the executable lands in `build\` instead of
`build\Release\`. If GLFW comes from vcpkg as a DLL, CMake copies it next to the
executable for you; `vulkan-1.dll` is already installed by your GPU driver.
`--headless` works on Windows too, which is a quick way to smoke-test the
renderer without a visible window.

In-source builds (`cmake .`) are refused by CMakeLists.txt, so always pass `-B`.

### Requirements

A C++11 compiler, CMake 3.16+, and either the Vulkan SDK + GLFW or the vendored
sysroot. No Python is needed: the SPIR-V is embedded by `cmake/EmbedSpirv.cmake`.

The sources are C++11. MSVC has no "C++11 mode" switch - it compiles this subset
in its default mode - so no standard flag is passed for MSVC, while GCC/Clang get
`-std=c++11`.

## Command line

```
window
  --width N            window width in pixels (default 800)
  --height N           window height in pixels (default 600)
  --vsync              force FIFO presentation (default is uncapped MAILBOX)
  --novsync            prefer an uncapped present mode (default)
  --frames N           stop after N frames (default: run until the window closes)

rendering
  --samples N          MSAA sample count, 1 disables it (default 4)
  --speed F            angular speed of the first gear in rad/s (default 1.15)
  --checker-scale N    integer checker frequency multiplier (default 1)
  --no-cull            disable backface culling
  --no-validation      do not enable the Khronos validation layer
  --verbose            also print informational messages from Vulkan layers

device selection
  --gpu N              use physical device N (see --list-devices)
  --gpu-name TEXT      pick the first device whose name contains TEXT
  --list-devices       print every Vulkan device and exit

headless / offscreen
  --headless           render offscreen (no window) and write a PPM file
  --out FILE           output file for --headless (default vulkangears.ppm)

misc
  --fps-interval MS    console status line period (default 1000)
  --help               show this help
```

Useful invocations:

```sh
./vulkangears --frames 600 --fps-interval 500      # short benchmark, still prints diagnostics
./vulkangears --samples 1                          # MSAA off (much faster on a software rasteriser)
./vulkangears --headless --out gears.ppm           # offscreen render, no display needed
./vulkangears --list-devices                       # what Vulkan devices exist
```

Press `ESC` or close the window to quit; on exit a session summary is printed.

## What the console shows

```
================================================================
 vulkangears - three meshing, checker-textured spinning gears
================================================================
 Vulkan loader       : 1.3.275   (instance API 1.1.0, 1 device(s) present)
 Instance layers     : VK_LAYER_KHRONOS_validation
 Instance extensions : VK_EXT_debug_utils VK_KHR_surface VK_KHR_xcb_surface
 Device [0]          : llvmpipe (LLVM 20.1.2, 256 bits)
   API version       : 1.4.318
   driver            : llvmpipe (Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2)) conformance 1.3.1
   vendor / device   : Mesa (0x10005) / 0x0000
   type              : CPU / software
   driver version    : 25.2.8 (raw 0x06402008)
   queue families    : graphics 0 (shared), present 0 (shared)
 Device extensions   : VK_KHR_swapchain VK_EXT_memory_budget VK_KHR_driver_properties
 Target              : window 800x600
 Colour format       : B8G8R8A8_SRGB (SRGB_NONLINEAR)
 Present mode        : MAILBOX (no vsync, no tearing), 4 swapchain images, transform 1
 Depth format        : D32_SFLOAT
 Multisampling       : 4x (MSAA, resolved before present)
 Backface culling    : on
 Checker texture     : 256x256, 8x8 cells, 9 mip levels, anisotropic + trilinear filtering
 Gear train          : 30T (15.00 r) -> 14T (7.00 r) -> 22T (11.00 r), module 1.00
 Mesh timing error   : joint A-B +0.0000 %, joint B-C -0.0000 % of a tooth pitch
 Geometry            : 3432 triangles, 5550 vertices, 3 draw calls per frame
 Memory heaps:
    heap 0  7.15 GiB   device-local   used 4.31 GiB / budget 7.15 GiB
      type 0  device-local host-visible coherent cached

 Application allocations:
    depth buffer                  7.32 MiB
    MSAA colour target            7.32 MiB
    gear vertex buffers     x3    173.44 KiB
    gear index buffers      x3    20.11 KiB
    checker texture               342.25 KiB
    texture staging buffer        341.33 KiB
    scene uniform buffers         224 B
    total                         15.51 MiB
 Uniform buffer      : 224 B (2 frames in flight, 112 B per frame)
================================================================

[info]  window is open - press ESC or close it to quit
[fps]     7.3 fps | 137.07 ms avg (min 117.09, max 177.94) | 32 frames | app 15.51 MiB in 11 allocations | heap0 4.35 GiB/7.15 GiB | 5.4 s
```

The `[fps]` line is printed once per `--fps-interval` and carries the interval's
frames, average/min/max frame time, cumulative frame count, the application's own
device-memory footprint and the device-local heap usage/budget (the latter comes
from `VK_EXT_memory_budget` when the driver offers it). The window title is kept
in sync with the same numbers.

Validation-layer messages are streamed to the console as they happen, tagged
`[vk/validation]`, `[vk/general]` or `[vk/performance]`; the summary counts them.
`--verbose` adds the loader's informational chatter.

## Layout

```
CMakeLists.txt            the build system (Windows/Linux/macOS)
cmake/EmbedSpirv.cmake    turns .spv files into a C++ array (no runtime file I/O)
scripts/fetch_deps.sh     vendors the missing -dev packages without root (Linux)
shaders/gear.vert|frag    push constants, checker sampling, Blinn-Phong + rim light
src/main.cpp              argument parsing, GLFW window, main loop, signals
src/vk_gears.{h,cpp}      instance/device/swapchain/render pass/pipeline/frames
src/gear.{h,cpp}          gear profile + train kinematics + checker texture
src/diag.{h,cpp}          logging, FPS counter, memory ledger, PPM output
src/platform.{h,cpp}      the only POSIX/Win32 specific code
src/math3d.h              minimal vec3/mat4 (column major, Vulkan clip space)
tests/mesh_check.cpp      geometry checks, run by `ctest`
```

Rendering basics: one graphics queue, two frames in flight, 4x MSAA resolved
straight into the swapchain image, a depth buffer, dynamic viewport/scissor,
one pipeline shared by all three gears (per-gear data travels in a 96-byte push
constant: model matrix, tint, checker phase) and one procedurally generated
mipmapped checker texture. The SPIR-V is compiled at build time and embedded in
the executable, so the binary has no data files to find at runtime.

## The gears

* **Involute teeth.** A gear tooth is a 20 degree pressure-angle involute. This
  matters: with the naive straight-sided tooth the tips collide with the mating
  flank, which the geometry test demonstrates. The tooth is thinned to 0.48 of the
  circular pitch so there is a real backlash instead of theoretical contact.
* **Correct meshing.** Two external gears with the same module mesh when their
  centres are `pitchRadius1 + pitchRadius2` apart and

  ```
  teeth1 * angle1 + teeth2 * angle2 = constant
  ```

  which makes teeth pass the line of centres at the same instants. The start
  phases are solved from that relation (see `solveJointPhase`), so the gears are
  correctly timed rather than merely spinning near each other.
* **A chain, not a loop.** A and B mesh, B and C mesh, and A and C do not touch.
  That is deliberate: three external gears in a closed loop are locked and could
  not turn at all - an odd property of gear trains that a demo could easily get
  wrong.

## Verification

```sh
ctest --test-dir build --output-on-failure
```

builds and runs a small geometry test (no GPU needed) that proves, over a full
meshing cycle:

* the extruded profile is a closed, simple, star-shaped polygon whose rim
  normals point outward, and every triangle's winding agrees with its normals,
* the start phases satisfy the meshing equation (`~4e-5 %` of a tooth pitch) and
  the speeds keep `teeth * omega` constant,
* **neighbouring teeth never interpenetrate**, sampled 241 times per cycle,
  while staying engaged with ~6 % of a module of backlash,
* the outer gears really are clear of each other,
* the checker texture and its mip chain are well formed.

The renderer itself is checked by running with the Khronos validation layer
enabled (it is vendored by `fetch_deps.sh` and picked up automatically), and by
comparing a backface-culled render against an unculled one - they must be
identical, which catches inverted winding. `--headless` makes all of that
possible on a machine with no display.

## Notes

* Portable C++11 + Vulkan. Linux/X11 and Wayland via GLFW; Windows via the
  Win32 backend the same GLFW build provides. Only `src/platform.cpp` differs
  between platforms (executable path, `setenv` vs `SetEnvironmentVariableA`,
  terminal detection).
* The vendored-sysroot route (`scripts/fetch_deps.sh`) is Debian/Ubuntu x86_64
  only; everywhere else CMake uses the Vulkan SDK and GLFW from vcpkg, a system
  install or an automatic fetch.
* The Windows code paths are written against documented Win32 APIs but have not
  been compiled on Windows from this machine - if MSVC or the SDK layout
  complains, the fixes should be small and local to `src/platform.cpp` and the
  dependency discovery in `CMakeLists.txt`.
* Default MSAA is 4x. On a software rasteriser (llvmpipe/lavapipe) that costs a
  lot - this container renders around 7 fps at 800x600; pass `--samples 1` there.
  On any real GPU it is far above the display refresh rate.
* The demo picks a discrete GPU if one is present; use `--gpu` / `--gpu-name` /
  `--list-devices` to choose explicitly.
