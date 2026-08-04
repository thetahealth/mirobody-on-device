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
    // True while the login view is shown over an existing session to add a second
    // account (mirrors app.js `addingAccount`). The chat view is hidden meanwhile.
    Q_PROPERTY(bool addingAccount READ addingAccount NOTIFY addingAccountChanged)
    // Incognito ("privacy mode"): nothing is persisted and each turn carries
    // incognito:true so the server keeps no memory (mirrors chat.toggleIncognito).
    Q_PROPERTY(bool incognito READ incognito NOTIFY incognitoChanged)
    // A conversation shared *to* the user opens read-only: the composer is hidden.
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString email READ email NOTIFY emailChanged)
    Q_PROPERTY(QString baseUrl READ baseUrl NOTIFY baseUrlChanged)
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(int fontOffset READ fontOffset NOTIFY fontOffsetChanged)
    Q_PROPERTY(QString provider READ provider NOTIFY providerChanged)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(ChatModel* chat READ chat CONSTANT)
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
    bool          addingAccount() const { return addingAccount_; }
    bool          incognito() const { return incognito_; }
    bool          readOnly() const { return readOnly_; }
    QString       email() const { return email_; }
    QString       baseUrl() const { return baseUrl_; }
    QString       language() const { return language_; }
    int           fontOffset() const { return fontOffset_; }
    QString       provider() const { return provider_; }
    QVariantList  providers() const { return providers_; }
    bool          streaming() const { return streaming_; }
    ChatModel*    chat() const { return chat_; }
    QStringList   baseUrlPresets() const;
    ModelDownloader* onDeviceModel() const { return downloader_; }
    BleHealth*    ble() const { return ble_; }

    // --- login -----------------------------------------------------------
    Q_INVOKABLE void sendCode(const QString& email);
    Q_INVOKABLE void verifyCode(const QString& email, const QString& code);
    Q_INVOKABLE void signOut();

    // --- multi-account (net.js account store) ----------------------------
    // Every stored account as [{sub, email, current}], the current one first.
    Q_INVOKABLE QVariantList listAccounts() const;
    // Make an already-stored account current (reloads its conversation/providers).
    Q_INVOKABLE void switchAccount(const QString& sub);
    // Show the login view over the current session to sign in another account
    // without dropping the stored ones; cancelAddAccount() backs out.
    Q_INVOKABLE void addAccount();
    Q_INVOKABLE void cancelAddAccount();

    // --- settings --------------------------------------------------------
    Q_INVOKABLE void setBaseUrl(const QString& url);
    Q_INVOKABLE void setLanguage(const QString& code);
    Q_INVOKABLE void setFontOffset(int offset);
    Q_INVOKABLE void setProvider(const QString& provider);

    // --- chat ------------------------------------------------------------
    Q_INVOKABLE void loadProviders();
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void stopStreaming();
    // Drop the current thread for a fresh one (drawer "New chat").
    Q_INVOKABLE void newChat();
    // Enter/leave incognito, swapping the whole session (chat.toggleIncognito).
    Q_INVOKABLE void toggleIncognito();

    // --- server-side history (the drawer) --------------------------------
    Q_INVOKABLE void loadHistory(int page, int pageSize);
    Q_INVOKABLE void deleteHistory(const QString& sessionId);
    // Resume a saved conversation into the chat view (owned => editable, a thread
    // shared to the user => read-only). GET /api/conversation?id=<sessionId>.
    Q_INVOKABLE void openConversation(const QString& sessionId);

    // --- Connect EHR (health.ehr.*) --------------------------------------
    Q_INVOKABLE void ehrSearchProviders(const QString& query);
    Q_INVOKABLE void ehrConnect(const QString& fhirBaseUrl);   // authorize + open browser
    Q_INVOKABLE void ehrSync();

    // --- Connected devices / vendors -------------------------------------
    Q_INVOKABLE void loadVendors();       // GET /vendors + GET /vendors/icons
    Q_INVOKABLE void vendorConnect(const QString& id);   // authorize + open browser
    Q_INVOKABLE void vendorUnlink(const QString& id);

signals:
    void loggedInChanged();
    void addingAccountChanged();
    void incognitoChanged();
    void readOnlyChanged();
    void accountsChanged();
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

    // Connect EHR.
    void ehrProvidersLoaded(const QVariantList& providers);
    void ehrError(const QString& message);
    void ehrConnecting();                // authorize accepted; browser opened
    void ehrSynced(int posted);

    // Vendors.
    void vendorsLoaded(const QVariantList& vendors);
    void vendorIconsLoaded(const QVariantMap& icons);
    void vendorsError(const QString& message);
    void vendorActionDone();             // connect opened / unlink finished

private:
    void completeLogin(const QString& accessToken);
    void setStreaming(bool s);
    void setEmail(const QString& email);
    void setReadOnly(bool ro);

    // --- multi-account token store (net.js) ------------------------------
    QString currentSub() const;                    // the active account's `sub`
    QString tokenFor(const QString& sub) const;    // stored JWT for `sub`
    QString currentToken() const;                  // token of the current account
    void    storeToken(const QString& token);      // store + make current (dedupe by email)
    // Drop the current account's slot; return true and switch to another stored
    // account if one remains, else clear the pointer and return false.
    bool    dropCurrentAccount();
    void    migrateLegacyToken();                  // old single-token slot -> per-account
    // Reload the current account's identity (email) + conversation + providers.
    void    activateSession();

    // Local per-user conversation persistence (mirrors db.js).
    QString userId() const;                 // decoded from the JWT `sub`/`email`
    QString conversationPath() const;       // "" when no user
    void    loadConversation();
    void    saveConversation();

    void finishTurn(int assistantRow, const QString& provider);
    void failTurn(int userRow, int assistantRow, const QString& errorText);

    // On-device turn (Gemma 4 via LiteRT-LM), mirroring the SSE handler in sendMessage.
    void sendOnDeviceMessage(const QString& model, const QString& label, int assistantRow);
    void rebuildProviders();   // providers_ = server providers + one synthetic entry per on-device model

    ApiClient* api_  = nullptr;
    ChatModel* chat_ = nullptr;
    std::unique_ptr<QSettings> settings_;
    SseStream* stream_ = nullptr;   // current in-flight chat stream, if any
    ModelDownloader* downloader_ = nullptr;
    LocalLmEngine*   localEngine_ = nullptr;
    BleHealth*       ble_ = nullptr;

    bool         loggedIn_   = false;
    bool         addingAccount_ = false;
    bool         incognito_  = false;
    bool         readOnly_   = false;
    QString      email_;
    QString      baseUrl_;
    QString      language_;
    int          fontOffset_ = 0;
    QString      provider_;
    QVariantList providers_;
    QVariantList serverProviders_;   // cached server list; providers_ = this + on-device models
    bool         streaming_  = false;

    // The server thread id for the running conversation, so the next turn
    // continues it and a reload resumes it (mirrors state.currentConversationId).
    QString      conversationId_;

    // Incognito stash: the real conversation set aside while incognito is on,
    // restored on leaving (mirrors state.incognitoSaved).
    bool         incoSavedValid_ = false;
    QVariantList incoSavedMessages_;
    QString      incoSavedConvId_;
    bool         incoSavedReadOnly_ = false;
};
