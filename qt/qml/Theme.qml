pragma Singleton

import QtQuick

// The Material 3 light scheme shared with the Android app and the web client
// (htdoc/src/config.js `color`). Kept in sync so all three surfaces read the
// same. `fontScale` follows the persisted font-size offset added to the 16px
// base, mirroring config.applyFontScale.
QtObject {
    readonly property color primary:      "#1e3a6b"
    readonly property color primaryFg:    "#ffffff"
    readonly property color brand:        "#1e3a6b"
    readonly property color background:   "#f2efe9"
    readonly property color surfaceLow:   "#faf7f1"
    readonly property color surfaceFg:    "#1a1c1e"
    readonly property color surfaceVarFg: "#52565c"
    readonly property color outline:      "#74787c"
    readonly property color outlineVar:   "#ddd6c9"
    // Solid navy user bubble with light text (matches the web client + Android).
    readonly property color userBubble:     "#1e3a6b"
    readonly property color userBubbleText:  "#ffffff"
    readonly property color error:        "#ba1a1a"

    // Base font point size; the whole UI scales relative to this. Bound to the
    // app's persisted offset by Main.qml (the five tiers are -4..+4).
    property int fontOffset: 0
    readonly property real baseSize: 11 + fontOffset   // ~16px at offset 0
}
