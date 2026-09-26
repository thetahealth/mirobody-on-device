cmake_minimum_required(VERSION 3.19)
project(mirobody CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json so IDEs (VSCode C/C++ extension, clangd, …)
# can resolve include paths without manual c_cpp_properties.json edits.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    # /Zc:__cplusplus makes __cplusplus reflect the actual language version
    # MSVC is compiling with. Without it the macro is stuck at 199711L
    # forever, which silently defeats the static_assert in platform/log.hpp.
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# --- Target profile: mobile vs desktop/server ---------------------------------
# THE build-target axis. One switch, one consequence (it used to also decide the
# provider menu; a provider with no credential is now skipped on every target --
# see offer() in res/agents/baseline.cpp):
#
#   ON  (mobile: HarmonyOS, and Android/iOS once they migrate)
#       - No HTTP/WebSocket front door. The app embeds the core through the C ABI
#         (mirobody_chat + the file calls in src/mirobody.h), so the server, its
#         router, every *service.cpp that registers routes, the SSE/WS/MQTT chat
#         transports and the realtime (voice) LLM clients are all dropped -- and
#         with them the libwebsockets dependency. mirobody_start / _stop /
#         _is_running / _listen_port become not-running stubs (src/platform/c_api.cpp).
#         What remains is the whole domain tier: agents, the chat dispatcher, chat
#         events and their filters (so render_chart still collapses into a
#         ChartEvent), the MCP tool registry, the LLM clients that speak HTTP over
#         libcurl, transcode, storage, memory.
#
#   OFF (desktop development build)
#       - Full front door: the loopback HTTP/SSE server, CLIs and tests.
#
# Android and iOS default to OFF for now: their entry points ARE the embedded
# loopback server (src/platform/android_jni.cpp, src/platform/ios_bridge.mm both
# stand up mirobody_start), so flipping them today would not compile. They move to
# ON once the HarmonyOS C-ABI path has proven out.
if(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    set(_MIROBODY_MOBILE_DEFAULT ON)
else()
    set(_MIROBODY_MOBILE_DEFAULT OFF)
endif()
option(MIROBODY_MOBILE
       "Build the mobile/embedded profile: no HTTP front door"
       ${_MIROBODY_MOBILE_DEFAULT})

if(MIROBODY_MOBILE)
    message(STATUS "target profile: mobile (no front door)")
else()
    message(STATUS "target profile: desktop/server (front door on)")
    find_package(libwebsockets CONFIG QUIET)
    if(NOT libwebsockets_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
    endif()
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(RapidJSON CONFIG QUIET)
if(NOT RapidJSON_FOUND)
    find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
endif()

find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)
endif()

# Image codecs for the src/transcode image transcoder (Aliyun Qwen-VL input compliance).
# JPEG is libjpeg-turbo (classic libjpeg API via the JPEG:: target); the other
# three expose their own CONFIG targets. All four are linked into mirobody_core
# below.
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)
find_package(WebP CONFIG REQUIRED)

# libtiff in the mobile prebuilt sysroots is built with the LZMA codec, so
# TIFF::TIFF references lzma_* symbols (e.g. lzma_lzma_preset). The stock
# FindTIFF module links only libtiff itself and does not surface that
# transitive dependency, so resolve LibLZMA and link it explicitly when the
# sysroot provides it. On desktop (vcpkg) TIFF's own config already pulls in
# lzma, so this find is optional and the extra link is harmless.
find_package(LibLZMA QUIET)

# zlib — used by src/indicator/lexicon.cpp to (de)compress the lexicon artifact.
# Already present transitively via libpng; required explicitly so lexicon.cpp can
# include <zlib.h> and link ZLIB::ZLIB directly.
find_package(ZLIB REQUIRED)

# brotli — OPTIONAL HTTP response encoder (src/server/router.cpp). When present,
# buffered dynamic responses are brotli-compressed for clients that accept it
# (better ratio than gzip); when absent they fall back to gzip. Static assets are
# pre-compressed at build time regardless (htdoc/scripts/precompress.cjs), so this
# only affects dynamic responses. Never fatal — key all logic off MIROBODY_HAS_BROTLI.
#   vcpkg `brotli` port -> unofficial-brotli CONFIG (unofficial::brotli::brotlienc)
#   apt  libbrotli-dev  -> pkg-config libbrotlienc
find_package(unofficial-brotli CONFIG QUIET)
set(MIROBODY_HAS_BROTLI FALSE)
if(TARGET unofficial::brotli::brotlienc)
    set(MIROBODY_HAS_BROTLI TRUE)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(BROTLIENC QUIET IMPORTED_TARGET libbrotlienc)
        if(BROTLIENC_FOUND)
            set(MIROBODY_HAS_BROTLI TRUE)
        endif()
    endif()
endif()

# Document transcoder (src/transcode/document). xlnt reads .xlsx; the
# xlnt-community fork (the one vcpkg ships) consumes cleanly at C++17, so
# document.cpp builds as part of the C++17 core like the rest of the tree. CSV
# is hand-written (no dependency). xlnt is auto-detected: present on Windows (the
# vcpkg manifest provides it) but optional elsewhere -- where it is absent (e.g.
# the mobile prebuilt sysroots) .xlsx degrades to a clear runtime error and the
# core does not link it.
#
# NB: the package is "Xlnt" (it installs XlntConfig.cmake, and vcpkg's usage hint
# uses that casing). find_package(xlnt) only resolves on case-insensitive
# filesystems (Windows); on Linux/macOS the config filename is case-sensitive, so
# use the canonical casing and key all logic off the exported target xlnt::xlnt.
find_package(Xlnt CONFIG QUIET)
if(TARGET xlnt::xlnt)
    message(STATUS "document transcoder: xlsx support ON (xlnt found)")
else()
    message(STATUS "document transcoder: xlsx support OFF (xlnt not found; CSV still works)")
endif()

# Optional document capabilities, OFF by default: their native deps are not on
# the default install path. PDFium ships as a prebuilt binary (bblanchon/
# pdfium-binaries), Tesseract is a heavy build, and libxls has no vcpkg port and
# is vendored. Enable per-build once the matching library is available, e.g.
#   cmake -DMIROBODY_ENABLE_PDF=ON -DMIROBODY_ENABLE_OCR=ON ...
option(MIROBODY_ENABLE_PDF "Build PDF support (PDFium) into the document transcoder" OFF)
option(MIROBODY_ENABLE_OCR "OCR scanned PDF pages with Tesseract; requires MIROBODY_ENABLE_PDF" OFF)
option(MIROBODY_ENABLE_XLS "Build legacy .xls support (vendored libxls) into the document transcoder" OFF)
if(MIROBODY_ENABLE_OCR AND NOT MIROBODY_ENABLE_PDF)
    message(FATAL_ERROR "MIROBODY_ENABLE_OCR requires MIROBODY_ENABLE_PDF (OCR only runs on rasterized PDF pages)")
endif()

# --- Database -----------------------------------------------------------------
# SQLite is the one SQL backend: the phone keeps its record in a single file, and
# the development build uses the same schema so what the tests cover is what ships.
# The server-side backends (Postgres and friends) live in the main mirobody repo.
find_package(SQLite3 REQUIRED)
set(_MIROBODY_DB_SOURCE src/database/sqlite.cpp)
set(_MIROBODY_DB_LINK   SQLite::SQLite3)
set(_MIROBODY_DB_SQLDIR sqlite)

find_package(BLAS)

# --- Sentry (crash / error reporting) -----------------------------------------
# sentry-native is linked only into the standalone desktop/server executable and
# initialised in src/main.cpp from the SENTRY_DSN config value. OPTIONAL: the
# cross-compiled sysroots (Android, iOS, HarmonyOS) omit it, and it is not worth a
# hard failure on a host that simply hasn't installed it. src/sentry.hpp keys off
# MIROBODY_HAS_SENTRY and degrades to no-ops when absent, so main.cpp is unchanged
# either way -- the binary just runs without crash reporting.
find_package(sentry CONFIG QUIET)
if(TARGET sentry::sentry)
    message(STATUS "crash reporting: sentry ON")
else()
    message(STATUS "crash reporting: sentry OFF (sentry-native not found)")
endif()

# Short git SHA captured at configure time. Used as the Sentry release fallback
# ("local-<sha>") in dev builds, mirroring the Python MCP server's
# _get_sentry_release(); k8s images override it via the SENTRY_RELEASE env. Empty
# when this is not a git checkout (e.g. an unpacked source tarball).
find_package(Git QUIET)
set(MIROBODY_GIT_SHA "")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MIROBODY_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

set(MIROBODY_CORE_SOURCES
    src/cache/cache.cpp
    src/cache/memory_kv.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/database/config.cpp
    src/database/schema.cpp
    src/jwt/apple.cpp
    src/jwt/firebase.cpp
    src/jwt/google.cpp
    src/jwt/jwt.cpp
    src/platform/log.cpp
    # The C ABI's pure JSON/string transforms. In the core (not just
    # mirobody_shared, where c_api.cpp itself lives) so the test binary can
    # reach them -- see src/platform/c_api_json.hpp.
    src/platform/c_api_json.cpp
    # The C ABI's on-device LLM half. In the core for a different reason: the
    # three platform bridges are mutually exclusive (c_api.cpp desktop,
    # ios_bridge.mm iOS, android_jni.cpp Android) and nothing in this one is
    # platform-specific, so hosting it in any single bridge would leave the other
    # two without it. Compiles to almost nothing when MIROBODY_ONDEVICE_LLM is
    # OFF -- it calls the stub, whose available() is false.
    src/platform/c_api_llm.cpp
    src/user/email.cpp
    src/health/vendor_link.cpp
    src/health/vendor_fhir.cpp
    src/health/timeseries/ingest.cpp
    src/chat/chat.cpp
    src/chat/history.cpp
    src/chat/dispatcher.cpp
    src/chat/params.cpp
    src/chat/event/event.cpp
    src/chat/event/filter/filter.cpp
    src/chat/event/filter/chart.cpp
    src/chat/event/filter/ask.cpp
    src/chat/packet.cpp
    src/chat/agent.cpp
    src/circle/access.cpp
    src/mcp/tool.cpp
    src/mcp/resource.cpp
    src/mcp/ask.cpp
    src/transcode/file.cpp
    src/transcode/parser.cpp
    src/oauth/pkce.cpp
    src/client/http_client.cpp
    src/client/gcp_auth.cpp
    src/storage/storage.cpp
    src/storage/sign.cpp
    src/storage/local_storage.cpp
    src/memory/memory.cpp
    src/memory/local_memory.cpp
    src/transcode/image.cpp
    src/transcode/document.cpp
    src/fhir/units/families.cpp
    src/fhir/units/tokens.cpp
    src/fhir/units/normalize.cpp
    src/fhir/fhir.cpp
    src/fhir/resource.cpp
    src/fhir/store.cpp
    src/fhir/write.cpp
    src/indicator/normalize.cpp
    src/indicator/word.cpp
    src/indicator/lexicon.cpp
    src/indicator/resolve.cpp
    src/indicator/sources.cpp
    src/health/vendor/vendor.cpp
    src/health/vendor/oauth2.cpp
    src/health/vendor/registry.cpp
    src/health/vendor/ehr/ehr.cpp
    src/health/vendor/ehr/directory.cpp
    src/llm/event.cpp
    src/llm/sse_parser.cpp
    src/llm/mirothinker.cpp
    src/llm/openai_chat.cpp
    src/llm/openai_responses.cpp
    src/llm/gemini.cpp
    src/llm/local.cpp
    src/llm/embedding.cpp
    src/config/fernet.cpp
    ${_MIROBODY_DB_SOURCE}
)

# Sources that need the HTTP front door (see MIROBODY_MOBILE above).
# Each either includes <libwebsockets.h> directly or pulls it in transitively
# through server/router.hpp -- which every *service.hpp that registers routes
# does. Keep this list in sync when adding a route-registering service.
set(MIROBODY_HTTP_SOURCES
    src/server/server.cpp
    src/server/router.cpp
    src/chat/service.cpp
    src/chat/transport/sse.cpp
    src/circle/service.cpp
    src/mcp/service.cpp
    src/oauth/service.cpp
    src/user/service.cpp
    src/health/vendor_service.cpp
    src/health/ehr_connect.cpp
    src/fhir/rest.cpp
)

if(NOT MIROBODY_MOBILE)
    list(APPEND MIROBODY_CORE_SOURCES ${MIROBODY_HTTP_SOURCES})
endif()

add_library(mirobody_core STATIC ${MIROBODY_CORE_SOURCES})
target_include_directories(mirobody_core PUBLIC src)

# --- On-device LLM (src/llm/local.cpp: llama.cpp + a GGUF model) --------------
#
# src/llm/local.cpp compiles a stub when this is OFF, so the default build needs
# no llama.cpp.
#
# LLAMA_CPP_DIR points at an SDK dir: include/llama.h + lib/. Both layouts work —
# a shared build (one libllama) as the desktops use, and a static build (libllama
# plus the ggml archives) as the mobile clients need, since there is no separate
# .so to ship next to a HAP/APK. The ggml libs are optional so a shared SDK
# doesn't have to fake them.
option(MIROBODY_ONDEVICE_LLM "Build the on-device LLM engine (llama.cpp + GGUF)" OFF)
if(MIROBODY_ONDEVICE_LLM)
    if(NOT DEFINED LLAMA_CPP_DIR)
        message(FATAL_ERROR
            "MIROBODY_ONDEVICE_LLM=ON requires -DLLAMA_CPP_DIR=<llama.cpp SDK dir (include/, lib/)>")
    endif()
    # find_library caches, so without clearing it a changed LLAMA_CPP_DIR (switching
    # backend or target ABI) would silently keep linking the previously found libs.
    foreach(_v LLAMA_LIB GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        unset(${_v} CACHE)
    endforeach()
    # NO_CMAKE_FIND_ROOT_PATH is required for the cross builds: the OHOS/Android
    # toolchains set CMAKE_FIND_ROOT_PATH_MODE_LIBRARY to ONLY, which re-roots
    # PATHS under the sysroot and makes an absolute SDK dir unfindable.
    set(_llama_find PATHS "${LLAMA_CPP_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LLAMA_LIB     NAMES llama     ${_llama_find})
    find_library(GGML_LIB      NAMES ggml      ${_llama_find})
    find_library(GGML_CPU_LIB  NAMES ggml-cpu  ${_llama_find})
    find_library(GGML_BASE_LIB NAMES ggml-base ${_llama_find})
    # Optional: present only in an SDK built with the Vulkan backend. Its presence IS
    # the switch — no separate option to keep in sync with which SDK is on disk.
    find_library(GGML_VULKAN_LIB NAMES ggml-vulkan ${_llama_find})
    if(NOT LLAMA_LIB)
        message(FATAL_ERROR
            "llama library not found under ${LLAMA_CPP_DIR}/lib. Check LLAMA_CPP_DIR.")
    endif()
    if(NOT EXISTS "${LLAMA_CPP_DIR}/include/llama.h")
        message(FATAL_ERROR "llama.h not found under ${LLAMA_CPP_DIR}/include.")
    endif()
    # The SDK's API LEVEL, not just its presence. src/llm/local.cpp passes the n_vocab
    # first argument that upstream 935cad649 (2026-08-04, "move n_vocab from
    # llama_sampler_data to penalty_sampler", #26520) restored to
    # llama_sampler_init_penalties; the four-argument form had stood since 644fd71b4
    # (2024-12-16). Against an older SDK everything else compiles and only that one call
    # fails, as "no matching function ... requires 5 arguments, but 4 were provided" --
    # which reads like a bug in our code rather than a stale dependency.
    #
    # Worth a check rather than trusting the checkout, because a stale SDK outlives its
    # source: the per-platform build-llama.* scripts reuse an assembled SDK and do not
    # update an existing llama.cpp clone. Whitespace is flattened first so the match does not depend on how
    # the declaration happens to be wrapped.
    file(READ "${LLAMA_CPP_DIR}/include/llama.h" _llama_h)
    string(REGEX REPLACE "[ \t\r\n]+" "" _llama_h_flat "${_llama_h}")
    if(NOT _llama_h_flat MATCHES "llama_sampler_init_penalties\\(int32_tn_vocab")
        message(FATAL_ERROR
            "The llama.cpp SDK at ${LLAMA_CPP_DIR} is older than this repo needs: its "
            "llama_sampler_init_penalties has no n_vocab parameter, so src/llm/local.cpp "
            "will not compile against it.\n"
            "Rebuild the SDK from a llama.cpp at 935cad649 (2026-08-04) or newer -- "
            "`git -C <llama.cpp> pull`, then re-run the build script for this frontend "
            "with `clean` so the cached SDK is discarded rather than reused.")
    endif()
    unset(_llama_h)
    unset(_llama_h_flat)
    target_compile_definitions(mirobody_core PUBLIC MIROBODY_ONDEVICE_LLM)
    target_include_directories(mirobody_core PRIVATE "${LLAMA_CPP_DIR}/include")
    # llama first: with static archives the linker resolves left to right, and
    # llama references ggml, not the other way round.
    target_link_libraries(mirobody_core PUBLIC "${LLAMA_LIB}")
    if(GGML_VULKAN_LIB)
        # Before the ggml archives: the backend references their symbols, and a static
        # link resolves left to right.
        target_link_libraries(mirobody_core PUBLIC "${GGML_VULKAN_LIB}" vulkan)
        target_compile_definitions(mirobody_core PUBLIC MIROBODY_ONDEVICE_LLM_VULKAN)
        message(STATUS "mirobody_core: Vulkan backend -> ${GGML_VULKAN_LIB}")
    endif()
    foreach(_lib GGML_LIB GGML_CPU_LIB GGML_BASE_LIB)
        if(${_lib})
            target_link_libraries(mirobody_core PUBLIC "${${_lib}}")
        endif()
    endforeach()
    # No ggml-cpu to link against means the SDK was built GGML_BACKEND_DL: the CPU
    # backend is not one library any more but a set of libggml-cpu-<arch>.so modules,
    # dlopened at startup so the fastest one THIS chip can run wins (android/build-llama.cmd).
    # Its absence IS the switch, the same way GGML_VULKAN_LIB's presence is one -- there
    # is no separate option to keep in sync with what is actually on disk.
    #
    # It has to reach C++ because the ggml_cpu_has_*() family lives in those modules and
    # so cannot be called: with DL, src/llm/local.cpp would not link at all. Those calls
    # answered "what was this binary compiled for", a question that no longer has one
    # answer -- which variant got loaded replaces it. See LocalCpuFeatures::variant.
    if(NOT GGML_CPU_LIB)
        target_compile_definitions(mirobody_core PUBLIC MIROBODY_ONDEVICE_LLM_DL)
        message(STATUS "mirobody_core: ggml CPU backends are dlopened at runtime")
    endif()
    message(STATUS "mirobody_core: on-device LLM ON -> ${LLAMA_LIB}")
endif()

# MIROBODY_DATABASE_BACKEND_DIR is the res/sql/<dir> subdirectory the schema
# loader reads (see src/database/schema.cpp).
target_compile_definitions(mirobody_core PUBLIC
    MIROBODY_DATABASE_SQLITE=1
    MIROBODY_DATABASE_BACKEND_DIR="${_MIROBODY_DB_SQLDIR}")

# --- MCP tools (Option A: glob + self-registration) ---------------------------
# Every res/mcp_tools/*.cpp self-registers via MIROBODY_REGISTER_TOOL. They are
# compiled into an OBJECT library and their object files are fed DIRECTLY into
# each final binary below (via $<TARGET_OBJECTS:mcp_tools>), NOT linked through
# the mirobody_core archive. That distinction is load-bearing: a self-
# registering translation unit is referenced by nothing, so if it sat in a
# static archive the linker would discard it and the tool would silently vanish.
# Objects handed straight to the final target are always retained, so their
# file-scope registration statics run at startup.
#
# CONFIGURE_DEPENDS re-globs on every build, so dropping a new tool .cpp into
# res/mcp_tools/ and rebuilding picks it up with no edit to this file.
file(GLOB MCP_TOOL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/mcp_tools/*.cpp)

add_library(mcp_tools OBJECT ${MCP_TOOL_SOURCES})
# OBJECT libraries do not link, but this propagates mirobody_core's include
# dirs (src/) and compile features (cxx_std_17) to the tool sources.
target_link_libraries(mcp_tools PRIVATE mirobody_core)

# Tool definitions are aggregate records whose trailing `raw_input_schema` is
# intentionally omitted (see the documented pattern in src/mcp/tool.hpp). That
# omission is well-defined value-initialization, but -Wextra's
# -Wmissing-field-initializers flags it as if it were accidental. Silence it for
# the tool sources only, so the rest of the tree keeps the strict warning set.
if(NOT MSVC)
    target_compile_options(mcp_tools PRIVATE -Wno-missing-field-initializers)
endif()

# --- MCP resources (same glob + self-registration as the MCP tools) -----------
# Every res/mcp_resources/*.cpp self-registers via MIROBODY_REGISTER_RESOURCE.
# Like the tools, the objects are fed straight into each final binary (via
# $<TARGET_OBJECTS:mcp_resources>) so the linker cannot discard the unreferenced
# self-registering translation units. Drop a new resource .cpp into
# res/mcp_resources/ and rebuilding picks it up with no edit to this file.
file(GLOB MCP_RESOURCE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/mcp_resources/*.cpp)

add_library(mcp_resources OBJECT ${MCP_RESOURCE_SOURCES})
target_link_libraries(mcp_resources PRIVATE mirobody_core)

# Resource definitions are aggregate records with optional trailing fields, the
# same -Wmissing-field-initializers situation as the tools.
if(NOT MSVC)
    target_compile_options(mcp_resources PRIVATE -Wno-missing-field-initializers)
endif()

# --- Agents (same glob + self-registration as the MCP tools) ------------------
# res/agents/*.cpp each self-register an AgentRegistration via
# MIROBODY_REGISTER_AGENT; the objects are fed straight into the final targets
# (below) for the same linker-survival reason as mcp_tools.
file(GLOB AGENT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/res/agents/*.cpp)

add_library(agent_impls OBJECT ${AGENT_SOURCES})
target_link_libraries(agent_impls PRIVATE mirobody_core)

# Bake the C++17 floor into the target's INTERFACE so anything that links
# mirobody_core (the desktop binary, the iOS static lib, the Android shared
# object, tests, and every CLI under cli/) inherits the same requirement.
# Downstream consumers that try to build at a lower standard get a CMake-time
# error instead of a confusing compile failure deep inside a header.
target_compile_features(mirobody_core PUBLIC cxx_std_17)

# On Android, compile the core with per-symbol sections and hidden visibility so
# the JNI .so's link-time --gc-sections / --exclude-libs can actually drop and
# localize the unused parts of the statically-linked deps (see the `mirobody`
# target below). Left off desktop/iOS so their exported C API is unaffected.
if(ANDROID)
    target_compile_options(mirobody_core PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden
        -ffunction-sections -fdata-sections)
endif()

target_link_libraries(mirobody_core PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    JPEG::JPEG
    PNG::PNG
    TIFF::TIFF
    WebP::webp
    WebP::webpdecoder
    ZLIB::ZLIB
    ${_MIROBODY_DB_LINK}
)

# See the LibLZMA find above: libtiff's LZMA codec needs liblzma at link time.
if(TARGET LibLZMA::LibLZMA)
    target_link_libraries(mirobody_core PUBLIC LibLZMA::LibLZMA)
endif()

# Optional brotli encoder (see the find near ZLIB): link it and define the gate so
# src/server/router.cpp compiles the brotli path. Without it, HTTP compression is
# gzip-only and the brotli code is #ifdef'd out.
if(MIROBODY_HAS_BROTLI)
    if(TARGET unofficial::brotli::brotlienc)
        target_link_libraries(mirobody_core PUBLIC
            unofficial::brotli::brotlienc unofficial::brotli::brotlicommon)
    else()
        target_link_libraries(mirobody_core PUBLIC PkgConfig::BROTLIENC)
    endif()
    target_compile_definitions(mirobody_core PRIVATE MIROBODY_ENABLE_BROTLI=1)
    message(STATUS "http compression: brotli ON")
else()
    message(STATUS "http compression: brotli OFF (dynamic responses gzip-only)")
endif()

# --- Document transcoder dependencies (src/transcode/document.cpp) ------------
# document.cpp is part of mirobody_core and uses the same C++17 floor. Its
# format handlers are wired here: xlnt (.xlsx) is linked when found; PDF (PDFium), OCR
# (Tesseract) and legacy .xls (libxls) are opt-in via the options near the image
# codecs. Each gate adds a PRIVATE compile definition so only document.cpp sees
# it; absent a gate the corresponding format throws a clear runtime error.
if(TARGET xlnt::xlnt)
    target_link_libraries(mirobody_core PUBLIC xlnt::xlnt)
    target_compile_definitions(mirobody_core PRIVATE MIROBODY_ENABLE_XLSX=1)
endif()

if(MIROBODY_ENABLE_PDF)
    # PDFium has no vcpkg port; consume a prebuilt (bblanchon/pdfium-binaries).
    # Accept either an installed CONFIG package or a `pdfium` imported target
    # provided via -DCMAKE_PREFIX_PATH / a find module.
    find_package(unofficial-pdfium CONFIG QUIET)
    if(TARGET unofficial::pdfium::pdfium)
        target_link_libraries(mirobody_core PUBLIC unofficial::pdfium::pdfium)
    elseif(TARGET pdfium)
        target_link_libraries(mirobody_core PUBLIC pdfium)
    else()
        message(FATAL_ERROR "MIROBODY_ENABLE_PDF=ON but no PDFium target found; "
                            "point CMAKE_PREFIX_PATH at a bblanchon/pdfium-binaries release")
    endif()
    target_compile_definitions(mirobody_core PRIVATE MIROBODY_ENABLE_PDF=1)
    if(MIROBODY_ENABLE_OCR)
        find_package(Tesseract CONFIG REQUIRED)
        target_link_libraries(mirobody_core PUBLIC Tesseract::libtesseract)
        target_compile_definitions(mirobody_core PRIVATE MIROBODY_ENABLE_OCR=1)
    endif()
endif()
if(MIROBODY_ENABLE_XLS)
    # libxls has no vcpkg port; vendor it under third_party/libxls and expose a
    # `libxls` target, or provide one via CMAKE_PREFIX_PATH.
    if(NOT TARGET libxls)
        find_package(libxls CONFIG REQUIRED)
    endif()
    target_link_libraries(mirobody_core PUBLIC libxls)
    target_compile_definitions(mirobody_core PRIVATE MIROBODY_ENABLE_XLS=1)
endif()

# PUBLIC so the res/* OBJECT libraries see it too: they link mirobody_core
# PRIVATE, which still applies its INTERFACE definitions when compiling them.
if(MIROBODY_MOBILE)
    target_compile_definitions(mirobody_core PUBLIC MIROBODY_MOBILE=1)
else()
    if(libwebsockets_FOUND)
        if(TARGET websockets_shared)
            target_link_libraries(mirobody_core PUBLIC websockets_shared)
        elseif(TARGET websockets)
            target_link_libraries(mirobody_core PUBLIC websockets)
        else()
            message(FATAL_ERROR "libwebsockets CONFIG found but no known target (websockets/websockets_shared)")
        endif()
    else()
        target_link_libraries(mirobody_core PUBLIC PkgConfig::LIBWEBSOCKETS)
    endif()
endif()

if(yaml-cpp_FOUND)
    if(TARGET yaml-cpp::yaml-cpp)
        target_link_libraries(mirobody_core PUBLIC yaml-cpp::yaml-cpp)
    else()
        target_link_libraries(mirobody_core PUBLIC yaml-cpp)
    endif()
else()
    target_link_libraries(mirobody_core PUBLIC PkgConfig::YAMLCPP)
endif()

if(TARGET rapidjson)
    target_link_libraries(mirobody_core PUBLIC rapidjson)
elseif(RAPIDJSON_INCLUDE_DIR)
    target_include_directories(mirobody_core PUBLIC ${RAPIDJSON_INCLUDE_DIR})
endif()

if(BLAS_FOUND)
    target_link_libraries(mirobody_core PUBLIC BLAS::BLAS)
    target_compile_definitions(mirobody_core PUBLIC MIROBODY_HAS_BLAS=1)
endif()

if(WIN32)
    # Windows system import libs needed by the (transitive) native deps. With the
    # dynamic vcpkg triplet each dep DLL carries these itself, but a static build
    # (the *-windows-static-md triplet the wheel uses) links the deps' .lib files
    # straight into our module, so the final link must satisfy their system-lib
    # references here. Concretely: libuv (pulled in via libwebsockets) needs
    # dbghelp (SymSetOptions/SymGetOptions/MiniDumpWriteDump) plus iphlpapi /
    # userenv / psapi. Linking an unused import lib is a no-op, so this is safe
    # for the dynamic desktop build too.
    target_link_libraries(mirobody_core PUBLIC ws2_32 dbghelp iphlpapi userenv psapi)
endif()

if(ANDROID)
    # Android target: build a shared library loaded by the host App via JNI.
    add_library(mirobody SHARED
        src/platform/android_jni.cpp
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>
    )
    target_link_libraries(mirobody PRIVATE mirobody_core log android)

    # Shrink the shipped libmirobody.so. The 260 MB of prebuilt static deps and
    # mirobody_core get linked in here, so by default their globals land in the
    # .so's dynamic symbol table (bloat) and nothing unused is dropped.
    #   --exclude-libs,ALL : localize all symbols pulled from static archives so
    #                        they are NOT re-exported (biggest win; also lets
    #                        --gc-sections reclaim them).
    #   --gc-sections + -ffunction/-fdata-sections : drop unreferenced code/data.
    #   -fvisibility=hidden : only JNIEXPORT entry points stay exported, so JNI
    #                         still resolves; everything else is hidden.
    #   --strip-all : strip symbols at link on non-debug builds (AGP also strips).
    target_compile_options(mirobody PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden
        -ffunction-sections -fdata-sections)
    # 16 KB page support. Android 15+ devices may run with a 16 KB kernel page size,
    # and the loader can only map a segment whose alignment is a multiple of the page
    # size -- so a .so whose LOAD segments are 4 KB-aligned (the default up to NDK r27;
    # r28 flipped it) simply will not load there. Play also gates on this. The APK side
    # is already handled: jniLibs.useLegacyPackaging=false stores the .so uncompressed
    # and page-aligned in the zip, which is necessary but says nothing about the ELF.
    #
    # Not conditional on the NDK version: passing it on a toolchain that already does
    # this is a no-op, and pinning behaviour to a version we would have to remember to
    # re-check is how this regresses.
    target_link_options(mirobody PRIVATE
        -Wl,-z,max-page-size=16384
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
        $<$<NOT:$<CONFIG:Debug>>:-Wl,--strip-all>)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS)
    # iOS target: build a static library that exposes the C API in
    # src/mirobody.h. The host app links libmirobody.a + libmirobody_core.a
    # (plus the prebuilt third-party deps) and calls in from Swift through a
    # bridging header, or from Objective-C directly.
    add_library(mirobody STATIC
        src/platform/ios_bridge.mm
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>
    )
    target_link_libraries(mirobody PRIVATE mirobody_core)
    set_target_properties(mirobody PROPERTIES
        PUBLIC_HEADER src/mirobody.h
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    )
elseif(MIROBODY_MOBILE)
    # src/main.cpp *is* the HTTP server's entry point, so there is nothing to
    # build without the front door. The library targets above still produce a
    # usable core for C ABI hosts.
    message(STATUS "skipping the mirobody executable (MIROBODY_MOBILE=ON)")
else()
    # Desktop / embedded Linux target: build a standalone executable.
    add_executable(mirobody src/main.cpp
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>)
    target_link_libraries(mirobody PRIVATE mirobody_core)

    # Crash reporting lives only in the desktop entry point (src/main.cpp), so
    # sentry-native is linked here and the build-time git SHA (the release
    # fallback) is exposed to it. Optional: without the package MIROBODY_HAS_SENTRY
    # stays 0 and src/sentry.hpp compiles to no-ops.
    target_compile_definitions(mirobody PRIVATE
        MIROBODY_GIT_SHA="${MIROBODY_GIT_SHA}")
    if(TARGET sentry::sentry)
        target_link_libraries(mirobody PRIVATE sentry::sentry)
        target_compile_definitions(mirobody PRIVATE MIROBODY_HAS_SENTRY=1)
    endif()

    # The crashpad backend launches a separate crashpad_handler process during
    # sentry_init; the SDK does not auto-discover it, so src/main.cpp points
    # sentry_options_set_handler_path at one shipped next to the binary. On the
    # Windows/vcpkg build the handler comes as an imported target with a stable
    # location, so copy it (and the WER helper it loads) into the output dir after
    # each build as a dev convenience. On Linux the handler is placed next to the
    # binary by the Dockerfile instead — the upstream source build has no install
    # rule for it and the exported target points into the build tree, so a copy
    # here would be unreliable; the runtime code degrades to a warning if absent.
    if(WIN32 AND TARGET sentry_crashpad::crashpad_handler)
        add_custom_command(TARGET mirobody POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sentry_crashpad::crashpad_handler>
                $<TARGET_FILE_DIR:mirobody>
            VERBATIM)
        if(TARGET sentry_crashpad::crashpad_wer)
            add_custom_command(TARGET mirobody POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:sentry_crashpad::crashpad_wer>
                    $<TARGET_FILE_DIR:mirobody>
                VERBATIM)
        endif()
    endif()
endif()

# MIROBODY_BUILD_SHARED: libmirobody, the C-ABI shared library for FFI hosts
# (on by default on desktop; the mobile platforms ship their own target).
option(MIROBODY_BUILD_SHARED "Build libmirobody, the C-ABI shared library for FFI hosts" ON)

# Linking the static core/objects into a shared object needs them
# position-independent. No-op on Windows; required on Linux/macOS.
if(MIROBODY_BUILD_SHARED)
    set_property(TARGET mirobody_core  PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET mcp_tools      PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET mcp_resources  PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET agent_impls    PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

# --- C-ABI shared library (any FFI host) --------------------------------------
# libmirobody.{dll,so,dylib} exports the flat C functions in src/mirobody.h
# (implemented in c_api.cpp and c_api_llm.cpp). On Windows the export table is
# restricted to exactly those functions via mirobody.def (rather
# than WINDOWS_EXPORT_ALL_SYMBOLS, which would also publish ~1000 mangled C++
# symbols). Built by default on desktop; skipped on mobile, which ship their own
# platform target also named "mirobody".
if(MIROBODY_BUILD_SHARED AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    add_library(mirobody_shared SHARED
        src/platform/c_api.cpp
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>)
    target_link_libraries(mirobody_shared PRIVATE mirobody_core)
    set_target_properties(mirobody_shared PROPERTIES
        OUTPUT_NAME mirobody                 # -> mirobody.dll / libmirobody.so
        PUBLIC_HEADER src/mirobody.h)
    if(WIN32)
        target_sources(mirobody_shared PRIVATE src/platform/mirobody.def)
    endif()
endif()

# The CLIs and tests assume the full core (the suite covers the router), so both
# are skipped when the front door is off. That build exists to produce a library for an
# embedded host, not to be developed against.
option(MIROBODY_BUILD_TOOLS "Build debug CLIs under cli/" ON)
if(MIROBODY_BUILD_TOOLS AND NOT MIROBODY_MOBILE AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    foreach(_tool mirothinker openai_chat openai_responses gemini google_jwt firebase_jwt jwt_keygen fernet mcp agent vendor image document fhir indicator file_parser file_rekey)
        add_executable(${_tool} cli/${_tool}.cpp)
        target_link_libraries(${_tool} PRIVATE mirobody_core)
        target_include_directories(${_tool} PRIVATE cli)
    endforeach()

    # The mcp / agent CLIs inspect their registries, so they need the
    # self-registering res/* objects linked in directly -- same reasoning as the
    # app and test targets (see the OBJECT libraries above).
    target_sources(mcp   PRIVATE $<TARGET_OBJECTS:mcp_tools> $<TARGET_OBJECTS:mcp_resources>)
    # The agent CLI also needs mcp_tools: an agent's clients are built with
    # mcp::registry().functions_json(), so without them it would run every turn
    # with an empty tool list -- not the configuration it is meant to reproduce.
    target_sources(agent PRIVATE $<TARGET_OBJECTS:agent_impls> $<TARGET_OBJECTS:mcp_tools>)
endif()

option(MIROBODY_BUILD_TESTS "Build unit tests" ON)
if(MIROBODY_BUILD_TESTS AND NOT MIROBODY_MOBILE AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    enable_testing()
    add_subdirectory(tests)
endif()
