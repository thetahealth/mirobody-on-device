cmake_minimum_required(VERSION 3.5.0)
project(mirobody_napi)

# The HarmonyOS native module: libmirobody.so = the NAPI bridge + the C ABI
# (src/platform/c_api.cpp) + mirobody_core.
#
# The core is pulled in by add_subdirectory on the repo root. OHOS selects the
# mobile profile by itself (MIROBODY_MOBILE in the root CMakeLists.txt): no HTTP
# front door, so libwebsockets is neither needed nor built, and the BYOK provider
# menu, so mirobody_get_providers() lists only what the user has a key for.
#
# Its third-party dependencies come prebuilt from harmony/prebuilt/<abi>/ -- run
# harmony/build-prebuilt.cmd (or .sh) once per ABI before building the app, or
# configuring will fail on the first find_package.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${NATIVERENDER_ROOT_PATH})

# entry/src/main/cpp -> main -> src -> entry -> harmony -> repo root
get_filename_component(MIROBODY_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# harmony/prebuilt/ is the one home for prebuilt native artifacts, from two producers:
# <abi>/ for the core's vcpkg dependencies (build-prebuilt) and llama-sdk/<abi>/ for the
# on-device engine (build-llama, resolved further down). Named once here so the shared
# parent is visible in the code and not just implied by two similar strings.
set(MIROBODY_PREBUILT_ROOT "${MIROBODY_REPO_ROOT}/harmony/prebuilt")
set(MIROBODY_PREBUILT      "${MIROBODY_PREBUILT_ROOT}/${OHOS_ARCH}")

if(NOT EXISTS "${MIROBODY_PREBUILT}/lib")
    message(FATAL_ERROR
        "No prebuilt dependencies for ${OHOS_ARCH} at ${MIROBODY_PREBUILT}.\n"
        "Run harmony/build-prebuilt.cmd ${OHOS_ARCH} (or ./build-prebuilt.sh ${OHOS_ARCH}) first.")
endif()

# find_package() inside the core resolves against the prebuilt tree. FIND_ROOT_PATH
# matters because the OHOS toolchain sets CMAKE_FIND_ROOT_PATH_MODE_* to ONLY.
list(PREPEND CMAKE_PREFIX_PATH    "${MIROBODY_PREBUILT}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${MIROBODY_PREBUILT}")

# Cached + FORCE: the root CMakeLists declares these as option()/CACHE entries, so
# a plain set() would lose to the cache on the second configure.
set(MIROBODY_DATABASE_BACKEND   SQLITE CACHE STRING "" FORCE)
set(MIROBODY_BUILD_TOOLS        OFF    CACHE BOOL   "" FORCE)
set(MIROBODY_BUILD_TESTS        OFF    CACHE BOOL   "" FORCE)
set(MIROBODY_BUILD_SHARED       OFF    CACHE BOOL   "" FORCE)
set(MIROBODY_BUILD_PYTHON       OFF    CACHE BOOL   "" FORCE)
set(MIROBODY_BUILD_JNI          OFF    CACHE BOOL   "" FORCE)

# On-device LLM: llama.cpp cross-compiled for this ABI, linked STATICALLY into
# libmirobody.so — a HAP has nowhere to put a loose companion .so, and the device
# refuses to exec a standalone binary anyway (SELinux denies the shell domain
# execute on data_local_tmp), so in-process is the only way to run a model here.
#
# The SDK is discovered the same way the prebuilt tree above is -- by repo-relative
# path, under the same prebuilt/ parent, at prebuilt/llama-sdk/<abi>, where
# harmony/build-llama.cmd (or .sh) assembles it. So building it IS turning the lane on,
# and there is no machine-local absolute path to paste into this tracked
# build-profile.json5 and then keep out of commits.
# Absent, the core compiles its stub and the app still builds, with on-device chat
# reporting itself unavailable -- the supported off state.
#
# An explicit -DLLAMA_CPP_DIR still wins, for an SDK assembled elsewhere (LLAMA_SDK_DIR).
# Probing include/llama.h rather than the directory: a half-assembled SDK should fail
# loudly in the root CMakeLists, which checks the archives too, not be silently ignored.
if(NOT DEFINED LLAMA_CPP_DIR)
    set(_llama_sdk "${MIROBODY_PREBUILT_ROOT}/llama-sdk/${OHOS_ARCH}")
    # Re-configure when the SDK appears or goes away. Without this the probe only runs on
    # a configure that something ELSE triggered: deleting the SDK left a generated
    # build.ninja still naming libllama.a as a link input, so an incremental build died
    # with a raw `ninja: error: ... missing and no known rule to make it` instead of
    # turning the lane off. Both paths are listed because the marker file covers removal
    # of a built SDK and the parent directory covers one appearing where there was none.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${MIROBODY_PREBUILT_ROOT}/llama-sdk"
                 "${_llama_sdk}/include/llama.h")
    if(EXISTS "${_llama_sdk}/include/llama.h")
        set(LLAMA_CPP_DIR "${_llama_sdk}")
    endif()
endif()
if(DEFINED LLAMA_CPP_DIR)
    set(MIROBODY_ONDEVICE_LLM ON CACHE BOOL "" FORCE)
    message(STATUS "mirobody_napi: on-device LLM from ${LLAMA_CPP_DIR}")
else()
    # FORCEd OFF, not just left alone: now that the presence of the SDK is the switch,
    # deleting it must actually turn the lane off. Without this the cached ON from an
    # earlier configure survives and the root CMakeLists fails demanding LLAMA_CPP_DIR.
    set(MIROBODY_ONDEVICE_LLM OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory("${MIROBODY_REPO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/mirobody-core")

# The target is mirobody_napi but the artifact must be libmirobody.so: it has to
# match nm_modname in napi_init.cpp and the `import ... from 'libmirobody.so'` on
# the ArkTS side. Keeping the CMake target name distinct also avoids colliding
# with the root's own `mirobody` target should the front door ever be turned on.
#
# c_api.cpp lives in the mirobody_shared target upstream, which is off here, so it
# is compiled directly into this module -- that is what provides mirobody_chat.
# The res/* OBJECT libraries must be listed as sources rather than linked: their
# tools/agents self-register from file-scope statics, which a static archive would
# discard as unreferenced.
add_library(mirobody_napi SHARED
    napi_init.cpp
    membw.cpp
    "${MIROBODY_REPO_ROOT}/src/platform/c_api.cpp"
    $<TARGET_OBJECTS:mcp_tools>
    $<TARGET_OBJECTS:mcp_resources>
    $<TARGET_OBJECTS:agent_impls>)

set_target_properties(mirobody_napi PROPERTIES OUTPUT_NAME mirobody)

# The bandwidth probe measures DRAM, and at the Debug default (-O0) an
# unvectorized read loop measures loop overhead instead -- it reported 8.2 GB/s
# while llama.cpp's decode was sustaining ~14.3 GB/s, an impossibility that is
# how the bug was caught. Only this loop needs the optimizer; -O2 stays scoped to
# the one file so the rest of the bridge still debugs as built.
set_source_files_properties(membw.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_include_directories(mirobody_napi PRIVATE "${MIROBODY_REPO_ROOT}/src")

# libace_napi: the N-API implementation. libhilog_ndk: native logging, so the
# worker thread's output shows up in DevEco's Log window alongside ArkTS logs.
target_link_libraries(mirobody_napi PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    mirobody_core)
