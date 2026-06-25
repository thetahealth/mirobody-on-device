pragma Singleton

import QtQuick
import "strings.js" as Strings

// Reactive i18n facade over strings.js. Bindings that call `I18n.t("key")` read
// the `language` property inside t(), so changing the language re-evaluates them
// automatically. `language` is driven by Main.qml from app.language.
QtObject {
    id: root

    property string language: "en"

    // The ten languages the app offers, code -> native label (config.js
    // LANGUAGES). Native labels are spelled out directly (the file is UTF-8).
    readonly property var languages: [
        ["zh", "中文"],
        ["ja", "日本語"],
        ["ko", "한국어"],
        ["en", "English"],
        ["fr", "Français"],
        ["de", "Deutsch"],
        ["ru", "Русский"],
        ["es", "Español"],
        ["ar", "العربية"],
        ["he", "עברית"]
    ]

    // The five font-size tiers: [offset, labelKey] (config.js FONT_TIERS).
    readonly property var fontTiers: [
        [-4, "fontSmaller"],
        [-2, "fontSmall"],
        [ 0, "fontNormal"],
        [ 2, "fontLarge"],
        [ 4, "fontLarger"]
    ]

    function t(key, arg) {
        return Strings.tr(root.language, key, arg);
    }

    function isRtl(code) {
        return code === "ar" || code === "he";
    }

    function languageLabel(code) {
        for (var i = 0; i < languages.length; ++i)
            if (languages[i][0] === code) return languages[i][1];
        return code;
    }

    function fontTierLabel(offset) {
        for (var i = 0; i < fontTiers.length; ++i)
            if (fontTiers[i][0] === offset) return t(fontTiers[i][1]);
        return t("fontNormal");
    }
}
