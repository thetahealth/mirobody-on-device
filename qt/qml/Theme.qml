pragma Singleton

import QtQuick

// The Material 3 light scheme shared with the Android app and the web client
// (htdoc/src/config.js `color`). Kept in sync so all three surfaces read the
// same. `fontScale` follows the persisted font-size offset added to the 16px
// base, mirroring config.applyFontScale.
QtObject {
    readonly property color primary:      "#2f5e78"
    readonly property color onPrimary:    "#ffffff"
    readonly property color brand:        "#3a78b5"
    readonly property color background:   "#fbfcfd"
    readonly property color surfaceLow:   "#f4f6f8"
    readonly property color onSurface:    "#1a1c1e"
    readonly property color onSurfaceVar: "#44474a"
    readonly property color outline:      "#74787c"
    readonly property color outlineVar:   "#c4c7cb"
    readonly property color userBubble:   Qt.rgba(0x3a / 255, 0x78 / 255, 0xb5 / 255, 0.12)
    readonly property color error:        "#ba1a1a"

    // Base font point size; the whole UI scales relative to this. Bound to the
    // app's persisted offset by Main.qml (the five tiers are -4..+4).
    property int fontOffset: 0
    readonly property real baseSize: 11 + fontOffset   // ~16px at offset 0
}
