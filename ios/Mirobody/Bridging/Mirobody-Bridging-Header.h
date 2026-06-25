// Objective-C bridging header — exposes the mirobody C API to Swift.
//
// mirobody.h lives at repo-root src/mirobody.h and is reachable via the
// HEADER_SEARCH_PATHS entry in project.yml ($(SRCROOT)/../src). Including it here
// is harmless in pure-client builds: the declarations compile, but the symbols
// (mirobody_start/stop/...) are only LINKED when mirobody.xcframework is added
// and MIROBODY_EMBEDDED is defined. ServerController.swift guards every call site
// with `#if MIROBODY_EMBEDDED`, so a pure-client build never references them.

#include "mirobody.h"
