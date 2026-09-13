cmake_minimum_required(VERSION 3.16)

# Refuse in-source builds before anything else runs: they mix generated files
# with the sources and leave CMakeCache.txt/CMakeFiles behind.  Configure into a
# separate directory instead (see README: cmake -S . -B build).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported: they would litter the source tree "
        "with generated files.\n"
        "Configure into a directory instead:\n"
        "    cmake -S . -B build\n"
        "and remove any CMakeCache.txt, CMakeFiles/, cmake_install.cmake, "
        "CTestTestfile.cmake or generated/ that an earlier attempt left in "
        "${CMAKE_SOURCE_DIR}.")
endif()

# C is enabled alongside CXX because GLFW - a C project that FetchContent adds
# as a subproject - calls project(... LANGUAGES C).  A language the top-level
# project() does not enable has to be discovered in the middle of the configure,
# and that nested detection is fragile with the Visual Studio generator: it can
# fail with "No CMAKE_C_COMPILER could be found" even though cl.exe is installed.
# Enabling C here makes CMake detect both compilers in the normal top-level pass.
project(vulkangears
    VERSION 1.0.0
    DESCRIPTION "Three meshing checker-textured gears, rendered with Vulkan"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language settings
# ---------------------------------------------------------------------------
# The sources are C++11.  MSVC has no C++11 mode switch - it compiles this
# subset in its default mode - which CMake handles by simply not passing a flag.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VKG_BUILD_TESTS "Build the GPU-free gear geometry checks run by ctest" ON)
option(VKG_FETCH_GLFW "Download and build GLFW when it is not installed" ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# There are two supported ways to get Vulkan, GLFW and a GLSL compiler:
#
#   * Linux:  third_party/sysroot, populated by scripts/fetch_deps.sh.  That
#             needs no root and keeps the build self-contained.
#   * Windows: the LunarG Vulkan SDK (Vulkan headers, vulkan-1.lib and
#             glslangValidator/glslc) plus GLFW from vcpkg/conan/a local build,
#             or fetched automatically from GitHub.
#
# If the vendored sysroot is present it wins, so on this checkout a plain
# "cmake -S . -B build" needs no system Vulkan or GLFW packages at all.
# ---------------------------------------------------------------------------
set(VKG_SYSROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot"
    CACHE PATH "Vendored Linux dependency sysroot created by scripts/fetch_deps.sh")

set(VKG_USE_SYSROOT OFF)
if(EXISTS "${VKG_SYSROOT}/usr/include/vulkan/vulkan.h")
    set(VKG_USE_SYSROOT ON)
endif()

add_library(vkg_deps INTERFACE)

if(VKG_USE_SYSROOT)
    # Find the multiarch library directory (usr/lib/x86_64-linux-gnu, ...).
    file(GLOB VKG_SYSROOT_LIBDIRS LIST_DIRECTORIES true "${VKG_SYSROOT}/usr/lib/*")
    set(VKG_SYSROOT_LIBDIR "")
    foreach(candidate ${VKG_SYSROOT_LIBDIRS})
        if(IS_DIRECTORY "${candidate}" AND EXISTS "${candidate}/libvulkan.so")
            set(VKG_SYSROOT_LIBDIR "${candidate}")
        endif()
    endforeach()
    if(NOT VKG_SYSROOT_LIBDIR)
        message(FATAL_ERROR
            "third_party/sysroot exists but contains no libvulkan.so.\n"
            "Run ./scripts/fetch_deps.sh again (or delete third_party/sysroot).")
    endif()

    target_include_directories(vkg_deps INTERFACE "${VKG_SYSROOT}/usr/include")
    target_link_directories(vkg_deps INTERFACE "${VKG_SYSROOT_LIBDIR}")
    target_link_libraries(vkg_deps INTERFACE vulkan glfw)
    set(VKG_VULKAN_DESCRIPTION "vendored ${VKG_SYSROOT_LIBDIR}")
else()
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR
            "Vulkan was not found.\n"
            "  Windows: install the LunarG Vulkan SDK from https://vulkan.lunarg.com/ "
            "(it sets VULKAN_SDK and provides vulkan-1.lib, the headers and glslangValidator).\n"
            "  Linux:   run ./scripts/fetch_deps.sh to vendor the development packages "
            "without root, or install libvulkan-dev.\n"
            "  macOS:   install the Vulkan SDK and MoltenVK.")
    endif()

    if(TARGET Vulkan::Vulkan)
        target_link_libraries(vkg_deps INTERFACE Vulkan::Vulkan)
        set(VKG_VULKAN_DESCRIPTION "Vulkan::Vulkan (${Vulkan_LIBRARY})")
    else()
        target_include_directories(vkg_deps INTERFACE ${VULKAN_INCLUDE_DIRS})
        target_link_libraries(vkg_deps INTERFACE ${VULKAN_LIBRARIES})
        set(VKG_VULKAN_DESCRIPTION "${VULKAN_LIBRARIES}")
    endif()
endif()

# --- GLFW ------------------------------------------------------------------
set(VKG_GLFW_TARGET "")
set(VKG_GLFW_DESCRIPTION "vendored libglfw")

if(NOT VKG_USE_SYSROOT)
    find_package(glfw3 3.3 QUIET)
    if(TARGET glfw3::glfw)
        set(VKG_GLFW_TARGET glfw3::glfw)
    elseif(TARGET glfw)
        set(VKG_GLFW_TARGET glfw)
    endif()

    if(VKG_GLFW_TARGET)
        set(VKG_GLFW_DESCRIPTION "system GLFW (${VKG_GLFW_TARGET})")
    elseif(VKG_FETCH_GLFW)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        message(STATUS "vulkangears: GLFW not installed, fetching it from GitHub")
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
        if(TARGET glfw)
            set(VKG_GLFW_TARGET glfw)
        endif()
        set(VKG_GLFW_DESCRIPTION "GLFW fetched from GitHub")
    else()
        message(FATAL_ERROR
            "GLFW was not found and VKG_FETCH_GLFW is OFF.\n"
            "Install it (vcpkg install glfw3, or -Dglfw3_DIR=<dir>), set "
            "VKG_FETCH_GLFW=ON, or use the vendored sysroot on Linux.")
    endif()

    # Link whichever GLFW was resolved into the dependency interface.  This has
    # to happen after the branch above, not inside it, so that a fetched GLFW
    # contributes its include directory and library exactly like a system one.
    if(VKG_GLFW_TARGET)
        target_link_libraries(vkg_deps INTERFACE ${VKG_GLFW_TARGET})
    endif()
endif()

# --- platform libraries ----------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(vkg_deps INTERFACE Threads::Threads)
if(UNIX)
    target_link_libraries(vkg_deps INTERFACE m ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# GLSL -> SPIR-V
# ---------------------------------------------------------------------------
set(VKG_SHADER_COMPILER "")
set(VKG_SHADER_MODE "")
set(VKG_SHADER_LAUNCHER "")

find_program(VKG_GLSLANG_VALIDATOR
    NAMES glslangValidator glslangValidator.exe
    HINTS "${VKG_SYSROOT}/usr/bin" "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
find_program(VKG_GLSLC
    NAMES glslc glslc.exe
    HINTS "${VKG_SYSROOT}/usr/bin" "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

if(VKG_GLSLANG_VALIDATOR)
    set(VKG_SHADER_COMPILER "${VKG_GLSLANG_VALIDATOR}")
    set(VKG_SHADER_MODE "glslang")
elseif(VKG_GLSLC)
    set(VKG_SHADER_COMPILER "${VKG_GLSLC}")
    set(VKG_SHADER_MODE "glslc")
else()
    message(FATAL_ERROR
        "No GLSL compiler found (looked for glslangValidator and glslc).\n"
        "Install the LunarG Vulkan SDK, or run ./scripts/fetch_deps.sh on Linux, "
        "or point CMake at one with -DVKG_GLSLANG_VALIDATOR=<path>.")
endif()

# The vendored glslangValidator needs its own shared libraries on the search
# path; cmake -E env sets that for the build step only.
if(VKG_USE_SYSROOT AND VKG_SHADER_MODE STREQUAL "glslang")
    set(VKG_SHADER_LAUNCHER
        "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${VKG_SYSROOT_LIBDIR}")
endif()

set(VKG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${VKG_GENERATED_DIR}")

set(VKG_SPV_FILES "")
foreach(stage IN ITEMS vert frag)
    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gear.${stage}")
    set(shader_binary "${VKG_GENERATED_DIR}/gear.${stage}.spv")

    if(VKG_SHADER_MODE STREQUAL "glslc")
        set(shader_flags "-fshader-stage=${stage}" "--target-env=vulkan1.0")
    else()
        set(shader_flags "-V" "--target-env" "vulkan1.0")
    endif()

    add_custom_command(
        OUTPUT "${shader_binary}"
        COMMAND ${VKG_SHADER_LAUNCHER} "${VKG_SHADER_COMPILER}" ${shader_flags}
                "${shader_source}" -o "${shader_binary}"
        DEPENDS "${shader_source}"
        COMMENT "Compiling shaders/gear.${stage} -> gear.${stage}.spv"
        VERBATIM)
    list(APPEND VKG_SPV_FILES "${shader_binary}")
endforeach()

set(VKG_EMBEDDED_HEADER "${VKG_GENERATED_DIR}/spirv_embedded.h")
add_custom_command(
    OUTPUT "${VKG_EMBEDDED_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DVKG_EMBED_OUTPUT=${VKG_EMBEDDED_HEADER}"
            "-DVKG_EMBED_NAMES=gear_vert_spv;gear_frag_spv"
            "-DVKG_EMBED_FILES=${VKG_SPV_FILES}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS ${VKG_SPV_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    COMMENT "Embedding SPIR-V into spirv_embedded.h"
    VERBATIM)

# ---------------------------------------------------------------------------
# The demo
# ---------------------------------------------------------------------------
add_executable(vulkangears
    src/main.cpp
    src/vk_gears.cpp
    src/gear.cpp
    src/diag.cpp
    src/platform.cpp
    "${VKG_EMBEDDED_HEADER}")

target_include_directories(vulkangears PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${VKG_GENERATED_DIR}")

target_link_libraries(vulkangears PRIVATE vkg_deps)

if(MSVC)
    target_compile_options(vulkangears PRIVATE /W3 /utf-8)
    # fopen/vsnprintf and friends are "deprecated" in MSVC's eyes; the portable
    # spellings are fine here.
    target_compile_definitions(vulkangears PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(vulkangears PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Running from the build tree with the vendored sysroot: point the dynamic
# loader at it relative to the executable so no environment variables are needed.
if(VKG_USE_SYSROOT AND UNIX)
    file(RELATIVE_PATH VKG_SYSROOT_RELATIVE "${CMAKE_CURRENT_BINARY_DIR}" "${VKG_SYSROOT_LIBDIR}")
    set_target_properties(vulkangears PROPERTIES
        BUILD_RPATH "\$ORIGIN/${VKG_SYSROOT_RELATIVE}"
        INSTALL_RPATH "\$ORIGIN/${VKG_SYSROOT_RELATIVE}")
endif()

# A dynamic GLFW (for example from vcpkg) needs its DLL next to the executable.
if(WIN32 AND VKG_GLFW_TARGET)
    get_target_property(VKG_GLFW_TYPE ${VKG_GLFW_TARGET} TYPE)
    if(VKG_GLFW_TYPE STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET vulkangears POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${VKG_GLFW_TARGET}>"
                    "$<TARGET_FILE_DIR:vulkangears>"
            COMMENT "Copying the GLFW runtime next to vulkangears"
            VERBATIM)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests: geometry checks that need no GPU and no window
# ---------------------------------------------------------------------------
if(VKG_BUILD_TESTS)
    enable_testing()
    add_executable(mesh_check tests/mesh_check.cpp src/gear.cpp src/diag.cpp)
    target_include_directories(mesh_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(mesh_check PRIVATE /W3 /utf-8)
        target_compile_definitions(mesh_check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(mesh_check PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(mesh_check PRIVATE m)
    endif()
    add_test(NAME gear_geometry COMMAND mesh_check)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS vulkangears RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "vulkangears configuration")
message(STATUS "  build type        : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  C++ standard      : 11")
message(STATUS "  compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Vulkan            : ${VKG_VULKAN_DESCRIPTION}")
message(STATUS "  GLFW              : ${VKG_GLFW_DESCRIPTION}")
message(STATUS "  GLSL compiler     : ${VKG_SHADER_COMPILER} (${VKG_SHADER_MODE})")
message(STATUS "  generated sources : ${VKG_GENERATED_DIR}")
message(STATUS "  build tests       : ${VKG_BUILD_TESTS}")
message(STATUS "")
