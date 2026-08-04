# Mirobody Qt Quick desktop frontend.
#
# A standalone QML client for a running mirobody server, the native counterpart
# of the web client in res/htdoc. It speaks the same HTTP + Server-Sent-Events
# API (POST /email/login, /email/verify, /api/providers, /api/chat, ...) over
# Qt's networking stack and links NOTHING from mirobody_core -- so this subtree
# has its own toolchain requirements (Qt6) and is built only when the top-level
# MIROBODY_BUILD_QT option is ON.
#
# Build (from the repo root), e.g.:
#   cmake -B build-qt -S . -DMIROBODY_BUILD_QT=ON \
#         -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<compiler>
#   cmake --build build-qt --target mirobody_qt
#
# C++11 is the repo-wide floor, but Qt6 headers require C++17; this target opts
# up locally without disturbing the rest of the tree.

cmake_minimum_required(VERSION 3.19)

# Qt 6.5+ for QQmlApplicationEngine::loadFromModule() (main.cpp) and the
# qt_add_qml_module CMake API used below.
# Bluetooth backs the direct BLE GATT health-sensor path (blehealth.*); Qt maps
# QLowEnergyController onto WinRT / Core Bluetooth / BlueZ, so one build covers
# Windows, macOS, and Linux.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Bluetooth)

# CMAKE_AUTOMOC drives moc over the QObject-derived backend classes.
set(CMAKE_AUTOMOC ON)

# Opt into the modern QML module behaviour: the ":/qt/qml/" resource prefix
# (QTP0001) and auto-handling of .qml files living in the qml/ subdirectory
# (QTP0004), so no hand-written qmldir is needed.
if(POLICY QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()
if(POLICY QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(mirobody_qt
    main.cpp
    apiclient.cpp
    apiclient.hpp
    chatmodel.cpp
    chatmodel.hpp
    appcontroller.cpp
    appcontroller.hpp
    blehealth.cpp
    blehealth.hpp
    blehealth_codec.cpp
    blehealth_codec.hpp
    modeldownloader.cpp
    modeldownloader.hpp
    locallmengine.cpp
    locallmengine.hpp
)

# Qt headers are C++17; keep this requirement local to the Qt target so the rest
# of the repo stays at its C++11 floor.
target_compile_features(mirobody_qt PRIVATE cxx_std_17)

# The QML module. QML_FILES are compiled to C++ by the Qt Quick compiler and
# embedded in the binary; RESOURCES carry the plain-JS string table. Theme.qml
# and I18n.qml are singletons (declared with `pragma Singleton`); flag them so
# the engine instantiates one shared object each.
set_source_files_properties(qml/Theme.qml qml/I18n.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(mirobody_qt
    URI Mirobody
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/I18n.qml
        qml/LoginPage.qml
        qml/ChatPage.qml
        qml/MessageDelegate.qml
        qml/HistoryDrawer.qml
        qml/CostDialog.qml
        qml/BackendDialog.qml
        qml/BleDialog.qml
        qml/EhrDialog.qml
        qml/VendorsDialog.qml
        qml/LanguageDialog.qml
        qml/FontDialog.qml
        qml/ConfirmDialog.qml
    RESOURCES
        qml/strings.js
)

target_link_libraries(mirobody_qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Bluetooth
)

# On-device private LLM (Gemma GGUF via llama.cpp). OFF by default so the app builds
# without the SDK (locallmengine.cpp compiles as a stub). This is the same runtime the
# Electron desktop client uses, so both desktops run the same Gemma GGUF. To enable,
# build llama.cpp as shared libs (pure CMake; see qt/README.md), assemble an SDK dir
# (include/llama.h + ggml.h, lib/llama.{lib,so,dylib}, bin/*.dll), then:
#   -DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=<that dir>
# The runtime DLLs/.so must sit next to the exe (build-qt.cmd deploy copies them).
option(MIROBODY_ONDEVICE_LLM "Build on-device LLM (Gemma GGUF via llama.cpp)" OFF)
if(MIROBODY_ONDEVICE_LLM)
    if(NOT DEFINED LLAMA_CPP_DIR)
        message(FATAL_ERROR
            "MIROBODY_ONDEVICE_LLM=ON requires -DLLAMA_CPP_DIR=<llama.cpp SDK dir (include/, lib/, bin/)>")
    endif()
    # Re-resolve every configure: find_library caches its result, so without this a
    # changed LLAMA_CPP_DIR (e.g. switching backend cpu<->avx2<->vulkan) would keep
    # linking the previously found lib.
    unset(LLAMA_LIB CACHE)
    find_library(LLAMA_LIB
        NAMES llama
        PATHS "${LLAMA_CPP_DIR}/lib"
        NO_DEFAULT_PATH)
    if(NOT LLAMA_LIB)
        message(FATAL_ERROR
            "llama library not found under ${LLAMA_CPP_DIR}/lib. Check LLAMA_CPP_DIR.")
    endif()
    target_compile_definitions(mirobody_qt PRIVATE MIROBODY_ONDEVICE_LLM)
    target_include_directories(mirobody_qt PRIVATE "${LLAMA_CPP_DIR}/include")
    target_link_libraries(mirobody_qt PRIVATE "${LLAMA_LIB}")
    # The seeded default model is hard-coded in modeldownloader.cpp (models are managed
    # at runtime). Packagers can override that default here without editing source:
    #   -DMIROBODY_GGUF_REPO=<hf-org/repo> -DMIROBODY_GGUF_FILE=<file.gguf>
    if(DEFINED MIROBODY_GGUF_REPO)
        target_compile_definitions(mirobody_qt PRIVATE MIROBODY_GGUF_REPO="${MIROBODY_GGUF_REPO}")
    endif()
    if(DEFINED MIROBODY_GGUF_FILE)
        target_compile_definitions(mirobody_qt PRIVATE MIROBODY_GGUF_FILE="${MIROBODY_GGUF_FILE}")
    endif()
    message(STATUS "mirobody_qt: on-device LLM ON -> ${LLAMA_LIB}")
endif()

# On Windows, make this a GUI app (no console window) for release builds.
set_target_properties(mirobody_qt PROPERTIES
    WIN32_EXECUTABLE $<IF:$<CONFIG:Debug>,FALSE,TRUE>
    MACOSX_BUNDLE TRUE
)

# --- Unit tests -------------------------------------------------------------
# The BLE GATT decoder (blehealth_codec.*) is pure Qt-Core logic, so it is tested
# without the Bluetooth module or any hardware. Built only when Qt Test is present
# (it always is in a full Qt install). Run with `ctest` in the build dir, or invoke
# the mirobody_qt_tests executable directly. A console app so QtTest output shows.
find_package(Qt6 6.5 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
    qt_add_executable(mirobody_qt_tests
        tests/blehealth_codec_test.cpp
        blehealth_codec.cpp
        blehealth_codec.hpp
    )
    target_compile_features(mirobody_qt_tests PRIVATE cxx_std_17)
    target_include_directories(mirobody_qt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mirobody_qt_tests PRIVATE Qt6::Core Qt6::Test)
    set_target_properties(mirobody_qt_tests PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    enable_testing()
    add_test(NAME mirobody_qt_tests COMMAND mirobody_qt_tests)
endif()
