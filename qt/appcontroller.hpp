#pragma once

// The app's orchestration hub, the Qt analogue of the web client's app.js +
// config.state. It owns the persisted settings (token, backend URL, language,
// font scale, last provider), drives the login flow and the streaming chat, and
// mirrors the running conversation to a per-user JSON file on disk (the desktop
// stand-in for the web client's IndexedDB history). One instance is exposed to
// QML as the context property `app`; the message list is `app.chat`.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonObject>
#include <memory>

#include "chatmodel.hpp"
// Full definition required: ModelDownloader* and BleHealth* are exposed as
// Q_PROPERTYs, so moc needs the complete type to register their metatypes (a
// forward declaration fails to compile).
#include "modeldownloader.hpp"
#include "blehealth.hpp"

class ApiClient;
class SseStream;
class QSettings;
class LocalLmEngine;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString email READ email NOTIFY emailChanged)
    Q_PROPERTY(QString baseUrl READ baseUrl NOTIFY baseUrlChanged)
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(int fontOffset READ fontOffset NOTIFY fontOffsetChanged)
    Q_PROPERTY(QString provider READ provider NOTIFY providerChanged)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(ChatModel* chat READ chat CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QStringList baseUrlPresets READ baseUrlPresets CONSTANT)
    // The on-device model downloader (status/progress + start/cancel/remove), exposed
    // to QML for the download affordance. Bind app.onDeviceModel.status / .progress.
    Q_PROPERTY(ModelDownloader* onDeviceModel READ onDeviceModel CONSTANT)
    // Direct BLE GATT health-sensor ingestion (scan/connect → FHIR). Bind
    // app.ble.devices / .status and drive app.ble.startScan() / connectDevice(i).
    Q_PROPERTY(BleHealth* ble READ ble CONSTANT)
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    bool          loggedIn() const { return loggedIn_; }
    QString       email() const { return email_; }
    QString       baseUrl() const { return baseUrl_; }
    QString       language() const { return language_; }
    int           fontOffset() const { return fontOffset_; }
    QString       provider() const { return provider_; }
    QVariantList  providers() const { return providers_; }
    bool          streaming() const { return streaming_; }
    ChatModel*    chat() const { return chat_; }
    QString       appVersion() const;
    QStringList   baseUrlPresets() const;
    ModelDownloader* onDeviceModel() const { return downloader_; }
    BleHealth*    ble() const { return ble_; }

    // --- login -----------------------------------------------------------
    Q_INVOKABLE void sendCode(const QString& email);
    Q_INVOKABLE void verifyCode(const QString& email, const QString& code);
    Q_INVOKABLE void signOut();

    // --- settings --------------------------------------------------------
    Q_INVOKABLE void setBaseUrl(const QString& url);
    Q_INVOKABLE void setLanguage(const QString& code);
    Q_INVOKABLE void setFontOffset(int offset);
    Q_INVOKABLE void setProvider(const QString& provider);

    // --- chat ------------------------------------------------------------
    Q_INVOKABLE void loadProviders();
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void stopStreaming();

    // --- server-side history (the drawer) --------------------------------
    Q_INVOKABLE void loadHistory(int page, int pageSize);
    Q_INVOKABLE void deleteHistory(const QString& sessionId);

signals:
    void loggedInChanged();
    void emailChanged();
    void baseUrlChanged();
    void languageChanged();
    void fontOffsetChanged();
    void providerChanged();
    void providersChanged();
    void streamingChanged();

    void codeSent();
    void loginError(const QString& message);
    void verifyError(const QString& message);
    void historyLoaded(const QVariantList& summaries);
    void historyError(const QString& message);

private:
    void completeLogin(const QString& accessToken);
    void setStreaming(bool s);
    void setEmail(const QString& email);

    // Local per-user conversation persistence (mirrors db.js).
    QString userId() const;                 // decoded from the JWT `sub`/`email`
    QString conversationPath() const;       // "" when no user
    void    loadConversation();
    void    saveConversation();

    void finishTurn(int assistantRow, const QString& provider);
    void failTurn(int userRow, int assistantRow, const QString& errorText);

    // On-device turn (Gemma 4 via LiteRT-LM), mirroring the SSE handler in sendMessage.
    void sendOnDeviceMessage(const QString& question, int assistantRow);
    void appendOnDeviceProvider();   // push the synthetic on-device entry into providers_

    ApiClient* api_  = nullptr;
    ChatModel* chat_ = nullptr;
    std::unique_ptr<QSettings> settings_;
    SseStream* stream_ = nullptr;   // current in-flight chat stream, if any
    ModelDownloader* downloader_ = nullptr;
    LocalLmEngine*   localEngine_ = nullptr;
    BleHealth*       ble_ = nullptr;

    bool         loggedIn_   = false;
    QString      email_;
    QString      baseUrl_;
    QString      language_;
    int          fontOffset_ = 0;
    QString      provider_;
    QVariantList providers_;
    bool         streaming_  = false;
};
