import QtQuick
import QtQuick.Controls
import Mirobody

// The settings gear dropdown, mirroring topbar.js buildSettingsMenu: app settings
// ONLY -- Language / Font size / Backend / About -- so it is identical on the
// login and chat screens. Everything session-scoped (email, Bluetooth, health
// connections, account switching, Sign out) lives in the nav drawer instead. The
// dialogs themselves live in Main; this just emits the open* signals.
Menu {
    id: menu

    signal openLanguage()
    signal openFont()
    signal openBackend()
    signal openAbout()

    MenuItem {
        text: I18n.t("language") + "   " + I18n.languageLabel(app.language)
        onTriggered: menu.openLanguage()
    }
    MenuItem {
        text: I18n.t("fontSize") + "   " + I18n.fontTierLabel(app.fontOffset)
        onTriggered: menu.openFont()
    }
    MenuItem {
        text: I18n.t("backend")
        onTriggered: menu.openBackend()
    }
    MenuItem {
        text: I18n.t("about")
        onTriggered: menu.openAbout()
    }
}
