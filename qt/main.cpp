// Entry point for the Mirobody Qt Quick desktop client.
//
// Sets up the QML engine, exposes the AppController as the context property
// `app` (the QML tree reads `app.loggedIn`, drives `app.sendMessage(...)`, binds
// to `app.chat`, ...), and loads the Main window from the Mirobody QML module.

#include <QGuiApplication>
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
