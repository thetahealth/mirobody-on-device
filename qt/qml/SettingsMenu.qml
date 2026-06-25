import QtQuick
import QtQuick.Controls
import Mirobody

// The settings dropdown, mirroring the web client's buildSettingsMenu: the
// signed-in email, then Language / Font size / Backend / About, and a
// destructive Sign out once authenticated. The dialogs themselves live in Main;
// this just emits the open* signals.
Menu {
    id: menu

    signal openLanguage()
    signal openFont()
    signal openBackend()
    signal openAbout()

    MenuItem {
        enabled: false
        visible: app.email.length > 0
        height: visible ? implicitHeight : 0
        text: app.email
    }
    MenuSeparator { visible: app.email.length > 0 }

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

    MenuSeparator { visible: app.loggedIn }
    MenuItem {
        visible: app.loggedIn
        height: visible ? implicitHeight : 0
        text: I18n.t("signOut")
        contentItem: Label {
            text: I18n.t("signOut")
            color: Theme.error
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
        }
        onTriggered: app.signOut()
    }
}
