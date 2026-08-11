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
#
# Svg is what DRAWS a rendered formula and a ```svg figure: both reach the UI as an
# .svg file behind a plain QML Image, and Qt loads those through the qsvg image
# format plugin this module brings. Not optional -- without it a formula is a blank
# rectangle rather than a fallback.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Bluetooth Svg)

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

# The app icon comes in two halves, because on a desktop "the icon" is two things:
#
#   icon/mirobody.rc   the EXECUTABLE's icon (Explorer, Start menu, pinned taskbar
#                      entry). Windows only reads this from a resource compiled into
#                      the binary, hence the .rc in the source list. Windows-only --
#                      an ELF or a Mach-O has no such thing.
#   icon/mirobody.png  the WINDOW's icon, embedded as a Qt resource and applied in
#                      main.cpp (see the qt_add_resources call below). This is what
#                      Linux has instead, and what the title bar / alt-tab use
#                      everywhere.
#
# Both are generated from the shared brand mark; see icon/gen_icon.py.
set(mirobody_qt_platform_sources "")
if(WIN32)
    list(APPEND mirobody_qt_platform_sources icon/mirobody.rc)
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
    renderhost.cpp
    renderhost.hpp
    ${mirobody_qt_platform_sources}
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
        qml/OnDeviceDialog.qml
        qml/ConfirmDialog.qml
        qml/ReplyBody.qml
        qml/ReplySegment.qml
        qml/ToolCard.qml
    RESOURCES
        qml/strings.js
        qml/slash.js
        qml/markdown.js
)

# The built-in guide `/help` renders. NOT a copy: these are the very files the web
# client serves from its doc root and the Android app ships in its assets, so the
# three clients answer /help with one document and it cannot drift between them.
# PREFIX pins them to ":/help/<name>.md", which AppController::helpDocument() reads.
qt_add_resources(mirobody_qt "mirobody_qt_help"
    PREFIX "/help"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../htdoc/static/help"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../htdoc/static/help/help-en.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/../htdoc/static/help/help-zh.md"
)

# The window icon, in a resource of its own rather than the QML module's. The module's
# prefix depends on the QTP0001 policy (":/Mirobody/" as things stand, ":/qt/qml/..."
# once that policy is actually set -- see the note above), and main.cpp needs a literal
# path that cannot quietly stop resolving. PREFIX + BASE here pin it to ":/icons/mirobody.png".
qt_add_resources(mirobody_qt "mirobody_qt_appicon"
    PREFIX "/icons"
    BASE "icon"
    FILES icon/mirobody.png
)

target_link_libraries(mirobody_qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Bluetooth
    Qt6::Svg
)

# --- LaTeX + ECharts rendering (Qt WebEngine) --------------------------------
# A reply can carry formulas ($…$ / $$…$$) and ```echarts charts. Every Mirobody
# client renders those through a browser engine -- harmony and iOS in a hidden
# webview, Android with a WebView for charts -- because MathJax and ECharts are the
# implementations, and reimplementing either natively would put this client's output
# out of step with the rest.
#
# So renderhost.cpp drives ONE offscreen QWebEnginePage over render/render.html and
# hands the results back as .svg / .png files that the message list draws as ordinary
# Images. See qt/renderhost.hpp for why it is shaped that way.
#
# ON when Qt WebEngine is installed, and it degrades rather than fails: built without
# it, formulas and charts show their source, while ```svg figures, tables, code and
# everything else render exactly the same (neither needs a browser). Force it off with
# -DMIROBODY_RICH_RENDER=OFF.
option(MIROBODY_RICH_RENDER "Render LaTeX and ECharts via Qt WebEngine" ON)
if(MIROBODY_RICH_RENDER)
    find_package(Qt6 6.5 QUIET COMPONENTS WebEngineQuick)
    if(Qt6WebEngineQuick_FOUND)
        target_compile_definitions(mirobody_qt PRIVATE MIROBODY_WEBENGINE)
        target_link_libraries(mirobody_qt PRIVATE Qt6::WebEngineQuick)
        message(STATUS "mirobody_qt: LaTeX + ECharts rendering ON (Qt WebEngine)")
    else()
        message(STATUS
            "mirobody_qt: Qt WebEngine not found -- formulas and charts will show their "
            "source. Install the 'Qt WebEngine' component to enable them.")
    endif()
endif()

# The render host's page and the two libraries behind it.
#
# The libraries are read out of harmony/'s render directory rather than copied here:
# that is where the repo keeps the offscreen-render assets, harmony loads the same
# three files for the same two jobs, and a second copy of a 2 MB MathJax bundle is a
# second thing to keep in step. Only render.html is this client's own -- it carries
# Theme.qml's palette and has no colour-mode parameter (see the comment in it).
#
# PREFIX pins them to ":/render/", which is where renderhost.cpp loads the page from
# and where the page's relative <script src> tags resolve.
set(mirobody_qt_render_dir "${CMAKE_CURRENT_SOURCE_DIR}/../harmony/entry/src/main/resources/rawfile/render")
if(MIROBODY_RICH_RENDER AND Qt6WebEngineQuick_FOUND)
    foreach(lib tex-svg.js echarts.min.js chart-theme.js)
        if(NOT EXISTS "${mirobody_qt_render_dir}/${lib}")
            message(FATAL_ERROR
                "Render asset missing: ${mirobody_qt_render_dir}/${lib}. It is tracked in "
                "the repo; configure with -DMIROBODY_RICH_RENDER=OFF to build without "
                "LaTeX/ECharts rendering.")
        endif()
    endforeach()
    # The hidden view that runs the page. Added to the QML module only in this branch:
    # it does `import QtWebEngine`, which a build without the module cannot resolve.
    qt_target_qml_sources(mirobody_qt QML_FILES qml/RenderHostView.qml)

    qt_add_resources(mirobody_qt "mirobody_qt_render"
        PREFIX "/render"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/render"
        FILES render/render.html
    )
    qt_add_resources(mirobody_qt "mirobody_qt_render_libs"
        PREFIX "/render"
        BASE "${mirobody_qt_render_dir}"
        FILES
            "${mirobody_qt_render_dir}/tex-svg.js"
            "${mirobody_qt_render_dir}/echarts.min.js"
            "${mirobody_qt_render_dir}/chart-theme.js"
    )
endif()

# On-device private LLM (any GGUF via llama.cpp). OFF by default so the app builds
# without the SDK (locallmengine.cpp compiles as a stub). This is the same runtime the
# Electron desktop client uses, so both desktops run the same GGUFs. To enable,
# build llama.cpp as shared libs (pure CMake; see qt/README.md), assemble an SDK dir
# (include/llama.h + ggml.h, lib/llama.{lib,so,dylib}, bin/*.dll), then:
#   -DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=<that dir>
# The runtime DLLs/.so must sit next to the exe (build-qt.cmd deploy copies them).
option(MIROBODY_ONDEVICE_LLM "Build on-device LLM (any GGUF via llama.cpp)" OFF)
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
# the test executables directly. Console apps so QtTest output shows.
find_package(Qt6 6.5 QUIET COMPONENTS Test)
if(Qt6Test_FOUND)
    enable_testing()

    qt_add_executable(mirobody_qt_tests
        tests/blehealth_codec_test.cpp
        blehealth_codec.cpp
        blehealth_codec.hpp
    )
    target_compile_features(mirobody_qt_tests PRIVATE cxx_std_17)
    target_include_directories(mirobody_qt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mirobody_qt_tests PRIVATE Qt6::Core Qt6::Test)
    set_target_properties(mirobody_qt_tests PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    add_test(NAME mirobody_qt_tests COMMAND mirobody_qt_tests)

    # qml/markdown.js -- the reply splitter and the math scanner. Plain JavaScript with
    # no QML dependency, so QJSEngine runs it directly; no scene, no window. Reads the
    # .js and the help document from the source tree, hence the source-dir define.
    qt_add_executable(mirobody_qt_markdown_tests tests/markdown_test.cpp)
    target_compile_features(mirobody_qt_markdown_tests PRIVATE cxx_std_17)
    target_compile_definitions(mirobody_qt_markdown_tests PRIVATE
        MIROBODY_QT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(mirobody_qt_markdown_tests PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
    set_target_properties(mirobody_qt_markdown_tests
        PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    add_test(NAME mirobody_qt_markdown_tests COMMAND mirobody_qt_markdown_tests)
endif()
