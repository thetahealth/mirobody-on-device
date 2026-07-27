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
set(MIROBODY_PREBUILT "${MIROBODY_REPO_ROOT}/harmony/prebuilt/${OHOS_ARCH}")

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
    "${MIROBODY_REPO_ROOT}/src/platform/c_api.cpp"
    $<TARGET_OBJECTS:mcp_tools>
    $<TARGET_OBJECTS:mcp_resources>
    $<TARGET_OBJECTS:agent_impls>)

set_target_properties(mirobody_napi PROPERTIES OUTPUT_NAME mirobody)

target_include_directories(mirobody_napi PRIVATE "${MIROBODY_REPO_ROOT}/src")

# libace_napi: the N-API implementation. libhilog_ndk: native logging, so the
# worker thread's output shows up in DevEco's Log window alongside ArkTS logs.
target_link_libraries(mirobody_napi PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    mirobody_core)
