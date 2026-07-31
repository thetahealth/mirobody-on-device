cmake_minimum_required(VERSION 3.19)
project(mirobody CXX)

set(CMAKE_CXX_STANDARD 11)
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
#   OFF (desktop / server, including the Qt and Electron frontends)
#       - Full front door.
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

find_package(hiredis CONFIG QUIET)
if(NOT hiredis_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
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
# xlnt-community fork (the one vcpkg ships) consumes cleanly at C++11, so
# document.cpp builds as part of the C++11 core like the rest of the tree. CSV
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

# --- Database backend selection -----------------------------------------------
# Exactly one SQL backend is linked into mirobody_core per build. See the
# "Database backends" section in the README for the policy. On mobile the
# default is SQLITE; on desktop / server it is POSTGRESQL.

if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS)
    set(_MIROBODY_DB_DEFAULT "SQLITE")
else()
    set(_MIROBODY_DB_DEFAULT "POSTGRESQL")
endif()

set(MIROBODY_DATABASE_BACKEND "${_MIROBODY_DB_DEFAULT}" CACHE STRING
    "SQL backend: SQLITE (mobile), DUCKDB, POSTGRESQL, POSTGRESQL_LEGACY, MYSQL, CLICKHOUSE")
set_property(CACHE MIROBODY_DATABASE_BACKEND PROPERTY STRINGS
    SQLITE DUCKDB POSTGRESQL POSTGRESQL_LEGACY MYSQL CLICKHOUSE)

if(NOT MIROBODY_DATABASE_BACKEND)
    message(FATAL_ERROR
        "MIROBODY_DATABASE_BACKEND must be set. "
        "On desktop / server pick DUCKDB, POSTGRESQL, MYSQL, or CLICKHOUSE.")
endif()

if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS)
    if(NOT MIROBODY_DATABASE_BACKEND MATCHES "^(SQLITE|DUCKDB)$")
        message(FATAL_ERROR
            "On mobile, MIROBODY_DATABASE_BACKEND must be SQLITE or DUCKDB "
            "(got: '${MIROBODY_DATABASE_BACKEND}').")
    endif()
endif()

if(MIROBODY_DATABASE_BACKEND STREQUAL "SQLITE")
    find_package(SQLite3 REQUIRED)
    set(_MIROBODY_DB_SOURCE src/database/sqlite.cpp)
    set(_MIROBODY_DB_LINK   SQLite::SQLite3)
    set(_MIROBODY_DB_SQLDIR sqlite)
elseif(MIROBODY_DATABASE_BACKEND STREQUAL "DUCKDB")
    find_package(duckdb CONFIG REQUIRED)
    set(_MIROBODY_DB_SOURCE src/database/duckdb.cpp)
    # vcpkg's duckdb port exposes duckdb_static and duckdb_shared targets;
    # prefer the static one to keep the desktop binary self-contained, fall
    # back to the shared one if only that variant was built.
    if(TARGET duckdb_static)
        set(_MIROBODY_DB_LINK duckdb_static)
    else()
        set(_MIROBODY_DB_LINK duckdb_shared)
    endif()
    set(_MIROBODY_DB_SQLDIR duckdb)
elseif(MIROBODY_DATABASE_BACKEND STREQUAL "POSTGRESQL")
    find_package(PostgreSQL REQUIRED)
    set(_MIROBODY_DB_SOURCE src/database/pg.cpp)
    set(_MIROBODY_DB_LINK   PostgreSQL::PostgreSQL)
    set(_MIROBODY_DB_SQLDIR pg)
elseif(MIROBODY_DATABASE_BACKEND STREQUAL "POSTGRESQL_LEGACY")
    # Same libpq client as POSTGRESQL, but the DDL is loaded from
    # res/sql/pg_legacy instead of res/sql/pg (an older schema variant).
    # Defines MIROBODY_DATABASE_PG_LEGACY; connection code that is common to
    # both PG variants guards on (MIROBODY_DATABASE_PG || MIROBODY_DATABASE_PG_LEGACY).
    find_package(PostgreSQL REQUIRED)
    set(_MIROBODY_DB_SOURCE src/database/pg.cpp)
    set(_MIROBODY_DB_LINK   PostgreSQL::PostgreSQL)
    set(_MIROBODY_DB_SQLDIR pg_legacy)
elseif(MIROBODY_DATABASE_BACKEND STREQUAL "MYSQL")
    find_package(unofficial-libmysql CONFIG REQUIRED)
    set(_MIROBODY_DB_SOURCE src/database/mysql.cpp)
    set(_MIROBODY_DB_LINK   unofficial::libmysql::libmysql)
    set(_MIROBODY_DB_SQLDIR mysql)
elseif(MIROBODY_DATABASE_BACKEND STREQUAL "CLICKHOUSE")
    # TODO: find_package(clickhouse-cpp CONFIG REQUIRED) once the dependency
    # lands in vcpkg.json / system packages. Today the backend is a stub that
    # throws at runtime, so no client library is linked.
    set(_MIROBODY_DB_SOURCE src/database/clickhouse.cpp)
    set(_MIROBODY_DB_LINK   )
    set(_MIROBODY_DB_SQLDIR clickhouse)
endif()

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
    src/chat/packet.cpp
    src/chat/agent.cpp
    src/circle/access.cpp
    src/mcp/tool.cpp
    src/mcp/resource.cpp
    src/transcode/file.cpp
    src/transcode/parser.cpp
    src/oauth/pkce.cpp
    src/client/http_client.cpp
    src/storage/storage.cpp
    src/storage/sign.cpp
    src/storage/aws_s3.cpp
    src/storage/aliyun_oss.cpp
    src/storage/azure_blob.cpp
    src/storage/local_storage.cpp
    src/memory/memory.cpp
    src/memory/local_memory.cpp
    src/memory/remote_memory.cpp
    src/memory/mem0_memory.cpp
    src/memory/zep_memory.cpp
    src/transcode/image.cpp
    src/transcode/document.cpp
    src/fhir/units/families.cpp
    src/fhir/units/tokens.cpp
    src/fhir/units/normalize.cpp
    src/fhir/fhir.cpp
    src/fhir/resource.cpp
    src/fhir/store.cpp
    src/indicator/normalize.cpp
    src/indicator/word.cpp
    src/indicator/lexicon.cpp
    src/indicator/resolve.cpp
    src/indicator/sources.cpp
    src/health/vendor/vendor.cpp
    src/health/vendor/oauth2.cpp
    src/health/vendor/registry.cpp
    src/health/vendor/platform/rook.cpp
    src/health/vendor/platform/spike.cpp
    src/health/vendor/platform/terra.cpp
    src/health/vendor/platform/junction.cpp
    src/health/vendor/platform/wefitter.cpp
    src/health/vendor/platform/lexisnexis.cpp
    src/health/vendor/platform/thryve.cpp
    src/health/vendor/platform/validic.cpp
    src/health/vendor/platform/human_api.cpp
    src/health/vendor/platform/vitalera.cpp
    src/health/vendor/platform/open_wearables.cpp
    src/health/vendor/platform/redox.cpp
    src/health/vendor/platform/particle_health.cpp
    src/health/vendor/platform/healthconnect.cpp
    src/health/vendor/platform/metriport.cpp
    src/health/vendor/phone/huawei.cpp
    src/health/vendor/device/fitbit.cpp
    src/health/vendor/device/withings.cpp
    src/health/vendor/device/garmin.cpp
    src/health/vendor/device/dexcom.cpp
    src/health/vendor/device/oura.cpp
    src/health/vendor/device/whoop.cpp
    src/health/vendor/device/polar.cpp
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
    src/chat/transport/ws.cpp
    src/chat/transport/mqtt.cpp
    src/circle/service.cpp
    src/mcp/service.cpp
    src/oauth/service.cpp
    src/user/service.cpp
    src/user/tanka.cpp
    src/health/vendor_service.cpp
    src/health/werun.cpp
    src/health/ehr_connect.cpp
    src/fhir/rest.cpp
    # Realtime / voice LLM lanes: WebSocket upstreams via client/websocket_client.
    src/client/websocket_client.cpp
    src/llm/gemini_live.cpp
    src/llm/openai_realtime.cpp
)

if(NOT MIROBODY_MOBILE)
    list(APPEND MIROBODY_CORE_SOURCES ${MIROBODY_HTTP_SOURCES})
endif()

add_library(mirobody_core STATIC ${MIROBODY_CORE_SOURCES})
target_include_directories(mirobody_core PUBLIC src)

# --- On-device LLM (src/llm/local.cpp: llama.cpp + a GGUF model) --------------
#
# One switch for the whole repo: qt/CMakeLists.txt reads the same option, so the
# frontends can't disagree about whether the engine exists. src/llm/local.cpp
# compiles a stub when this is OFF, so the default build needs no llama.cpp.
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
    message(STATUS "mirobody_core: on-device LLM ON -> ${LLAMA_LIB}")
endif()

# Expose the selected backend to C++ as a presence macro so code can switch on
# the linked backend at compile time, e.g.
#     #if defined(MIROBODY_DATABASE_PG)
# Exactly one is defined, named after the res/sql subdirectory:
#     MIROBODY_DATABASE_PG / _PG_LEGACY / _SQLITE / _DUCKDB / _MYSQL / _CLICKHOUSE
# Variants that share a client (PG and PG_LEGACY both use libpq) are handled by
# the consuming code, which guards on both — see main.cpp / server/server.cpp.
# MIROBODY_DATABASE_BACKEND_DIR is the matching res/sql/<dir> subdirectory the
# schema loader reads (see main.cpp).
string(TOUPPER "${_MIROBODY_DB_SQLDIR}" _MIROBODY_DB_SQLDIR_UPPER)
target_compile_definitions(mirobody_core PUBLIC
    MIROBODY_DATABASE_${_MIROBODY_DB_SQLDIR_UPPER}=1
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
# dirs (src/) and compile features (cxx_std_11) to the tool sources.
target_link_libraries(mcp_tools PRIVATE mirobody_core)

# Tool definitions are C++11 aggregates whose trailing `raw_input_schema` is
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

# Resource definitions are C++11 aggregates with optional trailing fields, the
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
target_compile_features(mirobody_core PUBLIC cxx_std_11)

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
# document.cpp is part of mirobody_core (above) and stays C++11. Its format
# handlers are wired here: xlnt (.xlsx) is linked when found; PDF (PDFium), OCR
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

if(hiredis_FOUND)
    # vcpkg's hiredis port exposes hiredis::hiredis (and hiredis::hiredis_static).
    if(TARGET hiredis::hiredis_static)
        target_link_libraries(mirobody_core PUBLIC hiredis::hiredis_static)
    elseif(TARGET hiredis::hiredis)
        target_link_libraries(mirobody_core PUBLIC hiredis::hiredis)
    else()
        target_link_libraries(mirobody_core PUBLIC hiredis)
    endif()
else()
    target_link_libraries(mirobody_core PUBLIC PkgConfig::HIREDIS)
endif()

# --- hiredis SSL/TLS (optional) -----------------------------------------------
# hiredis ships TLS support in a separate library (libhiredis_ssl) that wraps
# OpenSSL. It is detected independently of how hiredis itself was found, since
# Debian/Ubuntu expose no pkg-config/CMake config for it but do install the
# header + lib at standard paths. When present we link it and define
# MIROBODY_HAS_HIREDIS_SSL so src/cache/cache.cpp compiles the rediss:// path;
# when absent (e.g. Windows vcpkg without the hiredis[ssl] feature, or mobile),
# REDIS_SSL=true is rejected at runtime as before. Prefer vcpkg's CMake target
# if it exists, else fall back to find_library/find_path.
if(TARGET hiredis::hiredis_ssl)
    target_link_libraries(mirobody_core PUBLIC hiredis::hiredis_ssl)
    target_compile_definitions(mirobody_core PUBLIC MIROBODY_HAS_HIREDIS_SSL=1)
    message(STATUS "hiredis SSL support: enabled (CMake target)")
else()
    find_library(HIREDIS_SSL_LIBRARY NAMES hiredis_ssl)
    find_path(HIREDIS_SSL_INCLUDE_DIR NAMES hiredis/hiredis_ssl.h)
    if(HIREDIS_SSL_LIBRARY AND HIREDIS_SSL_INCLUDE_DIR)
        target_link_libraries(mirobody_core PUBLIC ${HIREDIS_SSL_LIBRARY})
        target_include_directories(mirobody_core PUBLIC ${HIREDIS_SSL_INCLUDE_DIR})
        # hiredis_ssl is implemented on top of OpenSSL; OpenSSL::SSL/Crypto are
        # already linked above, satisfying its symbols.
        target_compile_definitions(mirobody_core PUBLIC MIROBODY_HAS_HIREDIS_SSL=1)
        message(STATUS "hiredis SSL support: enabled (${HIREDIS_SSL_LIBRARY})")
    else()
        message(STATUS "hiredis SSL support: not found (REDIS_SSL=true will be rejected at runtime)")
    endif()
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
    target_link_options(mirobody PRIVATE
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

# Host-binding build options, declared up front so the shared PIC check below
# sees both. MIROBODY_BUILD_PYTHON: the pybind11 wheel module (off; flipped on by
# pyproject.toml). MIROBODY_BUILD_SHARED: libmirobody, the C-ABI shared library
# for FFI hosts (on by default on desktop).
option(MIROBODY_BUILD_PYTHON "Build the Python extension module (.whl)" OFF)
option(MIROBODY_BUILD_SHARED "Build libmirobody, the C-ABI shared library for FFI hosts" ON)
option(MIROBODY_BUILD_JNI "Build mirobody_jni, the desktop JNI shim (needs a JDK)" OFF)

# Anything that links the static core/objects into a shared object (the Python
# module, libmirobody, the JNI shim) needs them position-independent. No-op on
# Windows (everything is relocatable); required on Linux/macOS.
if(MIROBODY_BUILD_PYTHON OR MIROBODY_BUILD_SHARED OR MIROBODY_BUILD_JNI)
    set_property(TARGET mirobody_core  PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET mcp_tools      PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET mcp_resources  PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET agent_impls    PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

# --- Python extension (Option #2: pybind11 + scikit-build-core) ---------------
# Builds mirobody._mirobody, a CPython extension wrapping the C++ Server. Off by
# default; the wheel build flips it on via pyproject.toml (scikit-build-core).
# See python/README.md. Like the mobile targets, the self-registering res/*
# objects are fed straight into the module so the tools/agents survive linking.
if(MIROBODY_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(_mirobody
        src/platform/python_bridge.cpp
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>)
    target_link_libraries(_mirobody PRIVATE mirobody_core)

    # scikit-build-core installs into the wheel's platlib root, so this drops
    # _mirobody.<abi>.{pyd,so} inside the `mirobody` package next to __init__.py.
    install(TARGETS _mirobody LIBRARY DESTINATION mirobody)
endif()

# --- C-ABI shared library (Java / Go / C# / Rust / … via FFI) ------------------
# libmirobody.{dll,so,dylib} exports the flat C functions in src/mirobody.h
# (implemented in c_api.cpp). One artifact, consumed by every language with a
# foreign-function interface — see clients under bindings/. On Windows the
# export table is restricted to the four C functions via mirobody.def (rather
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

# --- JNI shim (desktop JVM, in-process) ---------------------------------------
# mirobody_jni.{dll,so,dylib} exports the JNI symbols for the Java class
# ai.thetahealth.mirobody.NativeBridge (src/platform/jni_bridge.cpp), the same
# class the Android app binds. This is the supported in-process path for desktop
# HotSpot JVMs, where the Panama FFM downcall into libwebsockets crashes (see
# bindings/README.md). Off by default since it needs a JDK; build-shared.cmd
# turns it on when JAVA_HOME is set. The bridge links mirobody_core statically,
# so the only runtime deps are the vcpkg shared libraries.
if(MIROBODY_BUILD_JNI AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    find_package(JNI REQUIRED)
    add_library(mirobody_jni SHARED
        src/platform/jni_bridge.cpp
        $<TARGET_OBJECTS:mcp_tools>
        $<TARGET_OBJECTS:mcp_resources>
        $<TARGET_OBJECTS:agent_impls>)
    target_link_libraries(mirobody_jni PRIVATE mirobody_core ${JNI_LIBRARIES})
    target_include_directories(mirobody_jni PRIVATE ${JNI_INCLUDE_DIRS})
endif()

# The CLIs and tests assume the full core: gemini_live / openai_realtime drive
# the WebSocket LLM lanes and the suite covers the router, so both are skipped
# when the front door is off. That build exists to produce a library for an
# embedded host, not to be developed against.
option(MIROBODY_BUILD_TOOLS "Build debug CLIs under cli/" ON)
if(MIROBODY_BUILD_TOOLS AND NOT MIROBODY_MOBILE AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    foreach(_tool mirothinker openai_chat openai_responses gemini gemini_live openai_realtime google_jwt firebase_jwt jwt_keygen fernet mcp agent aws_s3 aliyun_oss azure_blob vendor image document fhir indicator file_parser file_rekey)
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

# --- Qt desktop frontend (qt/) ------------------------------------------------
# A standalone Qt Quick (QML) desktop client that talks to a running mirobody
# server over its HTTP/SSE API -- the native counterpart of the web client in
# res/htdoc. It does NOT link mirobody_core (it's a pure API client), so it has
# its own dependency set (Qt6) and its own CMake subtree; it builds only when
# explicitly enabled and only on desktop. See qt/README.md.
option(MIROBODY_BUILD_QT "Build the Qt Quick desktop frontend (qt/)" OFF)
if(MIROBODY_BUILD_QT AND NOT ANDROID AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS))
    add_subdirectory(qt)
endif()
