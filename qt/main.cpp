// Entry point for the Mirobody Qt Quick desktop client.
//
// Sets up the QML engine, exposes the AppController as the context property
// `app` (the QML tree reads `app.loggedIn`, drives `app.sendMessage(...)`, binds
// to `app.chat`, ...), and loads the Main window from the Mirobody QML module.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "appcontroller.hpp"
#include "chatmodel.hpp"
#include "blehealth.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mirobody"));
    QGuiApplication::setOrganizationName(QStringLiteral("thetahealth"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Mirobody"));

    // The WINDOW icon (title bar, alt-tab, and the running task's taskbar button).
    // Separate from the icon compiled into the .exe by icon/mirobody.rc: that one is
    // what the shell shows for the FILE, and it does not exist at all on Linux, where
    // this call is the only icon the app gets. Embedded, so nothing has to ship
    // beside the binary; the path is pinned by the qt_add_resources call for it.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/mirobody.png")));

    // A consistent, restyleable Controls look across platforms.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // ChatModel is never constructed from QML (AppController owns the instance);
    // registering it uncreatable lets QML resolve its properties/roles.
    qmlRegisterUncreatableType<ChatModel>(
        "Mirobody", 1, 0, "ChatModel",
        QStringLiteral("ChatModel is provided by AppController.chat"));

    // Likewise BleHealth is owned by AppController and exposed as `app.ble`; register
    // it uncreatable so QML resolves its properties/signals.
    qmlRegisterUncreatableType<BleHealth>(
        "Mirobody", 1, 0, "BleHealth",
        QStringLiteral("BleHealth is provided by AppController.ble"));

    QQmlApplicationEngine engine;

    AppController controller;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Mirobody", "Main");

    return app.exec();
}
