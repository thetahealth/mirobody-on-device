#include "appcontroller.hpp"

#include "apiclient.hpp"
#include "modeldownloader.hpp"
#include "locallmengine.hpp"

#include <QSettings>
#include <QLocale>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QByteArray>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QUrl>

#include <algorithm>

namespace {

// localStorage key names, kept identical to net.js / config.js so a future
// shared store would line up; here they are QSettings keys.
//
// Multi-account token store (net.js): each account's JWT lives under
// kTokenPrefix + <sub>, and kCurrentKey names the active sub, so several accounts
// coexist and the drawer can quick-switch. kLegacyTokenKey is the old
// single-token slot, migrated once on load.
const char* kTokenPrefix   = "mirobody-x-token-";
const char* kCurrentKey     = "mirobody-x-current";
const char* kLegacyTokenKey = "mirobody-x-token";
const char* kBaseUrlKey  = "mirobody-base-url";
const char* kProviderKey = "mirobody-provider";
const char* kLanguageKey = "mirobody-language";
const char* kFontKey     = "mirobody-font-offset";

const char* kDefaultModel = "gemini-2.5-flash";

// Synthetic, client-only providers that run a local GGUF fully on-device (no server).
// One picker entry per registered model, named "<model> · On-device"; all share the
// kOnDeviceCode sentinel and carry the model name in a "model" field. Mirrors
// ProviderInfo.onDevice on Android/iOS.
const char* kOnDeviceSuffix = " \xC2\xB7 On-device";  // " · On-device" (UTF-8)
const char* kOnDeviceCode   = "__ondevice_gemma4__";

// The ten languages the app offers (config.js LANGUAGES); the picker shows
// these and the chosen code rides on each agent request.
const QStringList kSupportedLanguages = {
    "zh", "ja", "ko", "en", "fr", "de", "ru", "es", "ar", "he"
};

// Base64url-decode (no padding) -- used to read the JWT payload.
QByteArray base64UrlDecode(QByteArray s) {
    s.replace('-', '+').replace('_', '/');
    while (s.size() % 4) s.append('=');
    return QByteArray::fromBase64(s);
}

QString defaultLanguage() {
    const QString sys = QLocale::system().name().left(2).toLower();
    return kSupportedLanguages.contains(sys) ? sys : QStringLiteral("en");
}

// Decode a JWT payload (middle segment) into an object, or an empty object when
// missing/malformed. Mirrors net.js decodePayload().
QJsonObject decodePayload(const QString& token) {
    const QStringList parts = token.split('.');
    if (parts.size() < 2) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(base64UrlDecode(parts.at(1).toUtf8()));
    return doc.isObject() ? doc.object() : QJsonObject();
}

// The `email` claim of a JWT, or "" when absent/unparseable. Used to restore the
// signed-in address into the settings menu on a returning session.
QString emailClaim(const QString& token) {
    return decodePayload(token).value("email").toString();
}

// The `sub` claim as a string (the server re-salts it per issuance). "" when
// absent. Mirrors net.js setToken()'s sub derivation.
QString subClaim(const QString& token) {
    const QJsonValue sub = decodePayload(token).value("sub");
    if (sub.isString()) return sub.toString();
    if (sub.isDouble()) return QString::number(sub.toVariant().toLongLong());
    return {};
}

} // namespace

//------------------------------------------------------------------------------

AppController::AppController(QObject* parent)
    : QObject(parent),
      api_(new ApiClient(this)),
      chat_(new ChatModel(this)),
      settings_(new QSettings(QStringLiteral("thetahealth"),
                              QStringLiteral("mirobody-qt"))) {
    // Restore persisted settings (the QSettings stand-in for localStorage).
    migrateLegacyToken();
    const QString token = currentToken();
    baseUrl_    = settings_->value(kBaseUrlKey, baseUrlPresets().value(0)).toString();
    language_   = settings_->value(kLanguageKey, defaultLanguage()).toString();
    fontOffset_ = settings_->value(kFontKey, 0).toInt();
    provider_   = settings_->value(kProviderKey).toString();

    api_->setBaseUrl(baseUrl_);
    api_->setToken(token);

    // On-device private LLM (Gemma 4 via LiteRT-LM): the downloader fetches the model
    // on demand; the engine loads it lazily on the first local turn.
    downloader_  = new ModelDownloader(this);
    localEngine_ = new LocalLmEngine(this);
    // Adding/removing/downloading a model changes the picker's on-device entries.
    connect(downloader_, &ModelDownloader::modelsChanged, this, &AppController::rebuildProviders);

    // Direct BLE GATT ingestion shares the one ApiClient, so it always posts with
    // the current base URL + bearer token (updated on login / backend change).
    ble_ = new BleHealth(api_, this);

    if (!token.isEmpty()) {
        loggedIn_ = true;
        email_ = emailClaim(token);
        loadConversation();
        loadProviders();
    }
}

AppController::~AppController() = default;

QString AppController::appVersion() const {
#ifdef MIROBODY_QT_VERSION
    return QStringLiteral(MIROBODY_QT_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

QStringList AppController::baseUrlPresets() const {
    // Local dev server first (config.yml's HTTP_PORT), then the hosted
    // environment (config.js BASE_URL_PRESETS).
    return {
        QStringLiteral("http://127.0.0.1:8080"),
        QStringLiteral("https://test.mirobody.ai"),
    };
}

//------------------------------------------------------------------------------
// Login
//------------------------------------------------------------------------------

void AppController::sendCode(const QString& email) {
    const QString e = email.trimmed();
    if (e.isEmpty()) { emit loginError(QString()); return; }
    QJsonObject body{{"email", e}};
    api_->postEnvelope("/email/login", body,
        [this](const QJsonValue&) { emit codeSent(); },
        [this](const QString& msg, int) { emit loginError(msg); },
        /*withAuth=*/false);
}

void AppController::verifyCode(const QString& email, const QString& code) {
    const QString e = email.trimmed();
    QJsonObject body{{"email", e}, {"code", code.trimmed()}};
    api_->postEnvelope("/email/verify", body,
        [this](const QJsonValue& data) {
            const QString token = data.toObject().value("access_token").toString();
            if (token.isEmpty()) { emit verifyError(QString()); return; }
            completeLogin(token);
        },
        [this](const QString& msg, int) { emit verifyError(msg); },
        /*withAuth=*/false);
}

void AppController::completeLogin(const QString& accessToken) {
    // Store under this account's own slot and make it current (dedupes by email).
    storeToken(accessToken);
    api_->setToken(currentToken());

    // Whether this was a fresh sign-in or a second account being added, reset the
    // per-session state and load THIS account's data.
    if (addingAccount_) { addingAccount_ = false; emit addingAccountChanged(); }
    activateSession();
    emit accountsChanged();

    if (!loggedIn_) { loggedIn_ = true; emit loggedInChanged(); }
}

void AppController::signOut() {
    stopStreaming();
    // Drop this user's local history while we still know who they are (the key
    // is derived from the token), then drop the account slot -- mirrors
    // app.signOut() + net.clearToken().
    const QString path = conversationPath();
    if (!path.isEmpty()) QFile::remove(path);

    const bool haveAnother = dropCurrentAccount();
    emit accountsChanged();

    if (haveAnother) {
        // Fall back to another stored account, reloading its data in place.
        api_->setToken(currentToken());
        activateSession();
        if (!loggedIn_) { loggedIn_ = true; emit loggedInChanged(); }
        return;
    }

    // No account left: land on the login screen.
    api_->setToken(QString());
    chat_->clear();
    conversationId_.clear();
    incoSavedValid_ = false;
    setReadOnly(false);
    if (incognito_) { incognito_ = false; emit incognitoChanged(); }
    providers_.clear();
    emit providersChanged();
    setEmail(QString());
    if (loggedIn_) { loggedIn_ = false; emit loggedInChanged(); }
}

//------------------------------------------------------------------------------
// Multi-account (net.js token store)
//------------------------------------------------------------------------------

QString AppController::currentSub() const {
    return settings_->value(kCurrentKey).toString();
}

QString AppController::tokenFor(const QString& sub) const {
    if (sub.isEmpty()) return {};
    return settings_->value(QString::fromLatin1(kTokenPrefix) + sub).toString();
}

QString AppController::currentToken() const {
    return tokenFor(currentSub());
}

void AppController::storeToken(const QString& token) {
    if (token.isEmpty()) return;
    const QString sub = subClaim(token);
    if (sub.isEmpty()) return;
    const QString email = emailClaim(token);
    // Dedupe by email: the server re-salts `sub` per issuance, so drop any other
    // slot with the same email so there is one entry per account, not per login.
    if (!email.isEmpty()) {
        const QVariantList accts = listAccounts();
        for (const QVariant& v : accts) {
            const QVariantMap a = v.toMap();
            if (a.value("sub").toString() != sub && a.value("email").toString() == email)
                settings_->remove(QString::fromLatin1(kTokenPrefix) + a.value("sub").toString());
        }
    }
    settings_->setValue(QString::fromLatin1(kTokenPrefix) + sub, token);
    settings_->setValue(kCurrentKey, sub);
}

bool AppController::dropCurrentAccount() {
    const QString sub = currentSub();
    if (!sub.isEmpty()) settings_->remove(QString::fromLatin1(kTokenPrefix) + sub);
    const QVariantList rest = listAccounts();
    if (!rest.isEmpty()) {
        settings_->setValue(kCurrentKey, rest.first().toMap().value("sub").toString());
        return true;
    }
    settings_->remove(kCurrentKey);
    return false;
}

void AppController::migrateLegacyToken() {
    const QString legacy = settings_->value(kLegacyTokenKey).toString();
    if (legacy.isEmpty()) return;
    storeToken(legacy);
    settings_->remove(kLegacyTokenKey);
}

QVariantList AppController::listAccounts() const {
    QVariantList out;
    const QString cur = currentSub();
    const QString prefix = QString::fromLatin1(kTokenPrefix);
    const QStringList keys = settings_->allKeys();
    for (const QString& k : keys) {
        if (!k.startsWith(prefix)) continue;
        const QString sub = k.mid(prefix.size());
        if (sub.isEmpty()) continue;
        const QString tok = settings_->value(k).toString();
        QVariantMap m;
        m.insert("sub", sub);
        m.insert("email", emailClaim(tok));
        m.insert("current", sub == cur);
        out.push_back(m);
    }
    // Current account first (mirrors net.listAccounts()).
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("current").toBool() && !b.toMap().value("current").toBool();
    });
    return out;
}

void AppController::switchAccount(const QString& sub) {
    if (sub.isEmpty() || tokenFor(sub).isEmpty()) return;
    if (sub == currentSub() && !addingAccount_) return;
    stopStreaming();
    settings_->setValue(kCurrentKey, sub);
    api_->setToken(currentToken());
    if (addingAccount_) { addingAccount_ = false; emit addingAccountChanged(); }
    activateSession();
    emit accountsChanged();
    if (!loggedIn_) { loggedIn_ = true; emit loggedInChanged(); }
}

void AppController::addAccount() {
    if (addingAccount_) return;
    addingAccount_ = true;
    emit addingAccountChanged();
}

void AppController::cancelAddAccount() {
    if (!addingAccount_) return;
    addingAccount_ = false;
    emit addingAccountChanged();
}

void AppController::activateSession() {
    // Reset per-session state, then load the current account's data. The single
    // entry point after any session change so no previous account leaks through.
    stopStreaming();
    if (incognito_) { incognito_ = false; emit incognitoChanged(); }
    incoSavedValid_ = false;
    conversationId_.clear();
    setReadOnly(false);
    setEmail(emailClaim(currentToken()));
    chat_->clear();
    loadConversation();      // restore THIS account's persisted conversation
    providers_.clear();
    emit providersChanged();
    loadProviders();
}

//------------------------------------------------------------------------------
// Settings
//------------------------------------------------------------------------------

void AppController::setBaseUrl(const QString& url) {
    QString v = url.trimmed();
    while (v.endsWith('/')) v.chop(1);
    if (v == baseUrl_) return;
    baseUrl_ = v;
    api_->setBaseUrl(v);
    settings_->setValue(kBaseUrlKey, v);
    emit baseUrlChanged();
    // Re-fetch providers from the new backend; an existing token rides along.
    providers_.clear();
    emit providersChanged();
    if (loggedIn_) loadProviders();
}

void AppController::setLanguage(const QString& code) {
    if (code == language_ || !kSupportedLanguages.contains(code)) return;
    language_ = code;
    settings_->setValue(kLanguageKey, code);
    emit languageChanged();
}

void AppController::setFontOffset(int offset) {
    if (offset == fontOffset_) return;
    fontOffset_ = offset;
    settings_->setValue(kFontKey, offset);
    emit fontOffsetChanged();
}

void AppController::setProvider(const QString& provider) {
    if (provider == provider_) return;
    provider_ = provider;
    if (provider.isEmpty()) settings_->remove(kProviderKey);
    else                    settings_->setValue(kProviderKey, provider);
    emit providerChanged();
}

void AppController::setEmail(const QString& email) {
    if (email == email_) return;
    email_ = email;
    emit emailChanged();
}

void AppController::setStreaming(bool s) {
    if (s == streaming_) return;
    streaming_ = s;
    emit streamingChanged();
}

void AppController::setReadOnly(bool ro) {
    if (ro == readOnly_) return;
    readOnly_ = ro;
    emit readOnlyChanged();
}

//------------------------------------------------------------------------------
// Providers
//------------------------------------------------------------------------------

void AppController::loadProviders() {
    if (api_->token().isEmpty()) return;
    api_->postEnvelope("/api/providers", QJsonObject(),
        [this](const QJsonValue& data) {
            // /api/providers returns groups: [{ agent, providers[] }]. Flatten to one
            // entry per model. "name" is the display + selection value (QML textRole/
            // valueRole = "name"); "agent" ("" for the default agent) rides on the
            // request. "code" mirrors the model. On-device entries are appended by
            // rebuildProviders() (one per model), so the two live in one picker.
            serverProviders_.clear();
            const QJsonArray arr = data.toArray();
            for (const QJsonValue& gv : arr) {
                const QJsonObject g = gv.toObject();
                const QString agent = g.value("agent").toString();
                for (const QJsonValue& pv : g.value("providers").toArray()) {
                    const QString p = pv.toString();
                    if (p.isEmpty()) continue;
                    QVariantMap m;
                    m.insert("name", p);
                    m.insert("code", p);
                    m.insert("agent", agent);
                    serverProviders_.push_back(m);
                }
            }
            rebuildProviders();
        },
        [this](const QString&, int code) {
            if (code == 401) { signOut(); return; }
            // Server unreachable, but on-device chat still works offline.
            serverProviders_.clear();
            rebuildProviders();
        });
}

// providers_ = the cached server providers + one synthetic entry per on-device model
// ("<model> · On-device", sentinel code, model name in "model"). Rerun whenever the
// server list or the model registry changes; keeps/repairs the current selection.
void AppController::rebuildProviders() {
    providers_ = serverProviders_;
    QStringList names;
    for (const QVariant& v : providers_) names << v.toMap().value("name").toString();
    // Only *downloaded* on-device models appear in the picker; the rest are reached
    // via the manager (the "Manage on-device AI" entry the QML picker appends).
    for (const QString& model : downloader_->names()) {
        if (!downloader_->isReady(model)) continue;
        const QString label = model + QString::fromUtf8(kOnDeviceSuffix);
        QVariantMap m;
        m.insert("name", label);
        m.insert("code", QString::fromUtf8(kOnDeviceCode));
        m.insert("model", model);
        providers_.push_back(m);
        names << label;
    }
    if (!names.isEmpty() && !names.contains(provider_)) setProvider(names.first());
    emit providersChanged();
}

//------------------------------------------------------------------------------
// Chat streaming
//------------------------------------------------------------------------------

void AppController::sendMessage(const QString& text) {
    const QString q = text.trimmed();
    if (q.isEmpty() || streaming_) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int userRow = chat_->appendMessage("user", q, now);
    saveConversation();

    const int assistantRow = chat_->appendMessage("assistant", QString(), now);
    const QString turnProvider = provider_;

    setStreaming(true);

    // Find the selected provider entry once.
    QVariantMap sel;
    for (const QVariant& v : providers_) {
        const QVariantMap m = v.toMap();
        if (m.value("name").toString() == turnProvider) { sel = m; break; }
    }

    // --- On-device mode: route to the local engine, no server ------------
    if (sel.value("code").toString() == QString::fromUtf8(kOnDeviceCode)) {
        sendOnDeviceMessage(sel.value("model").toString(), turnProvider, assistantRow);
        return;
    }

    // --- Agent mode: a provider is selected -------------------------------
    // Send {agent, provider} verbatim -- no parsing. agent is "" for the default agent.
    if (!turnProvider.isEmpty()) {
        const QString agentName = sel.value("agent").toString();
        const QString providerName = turnProvider;

        QJsonObject body{
            {"agent", agentName},
            {"provider", providerName},
            {"question", q},
            {"language", language_},
            {"conversation_id", conversationId_},
            {"incognito", incognito_},
        };
        SseStream* s = api_->openStream("/api/chat", body);
        stream_ = s;
        auto acc = std::make_shared<QString>();

        connect(s, &SseStream::message, this, [this, assistantRow, acc, turnProvider](const QString& chunk) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(chunk.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
            const QJsonObject o = doc.object();
            const QString type = o.value("type").toString();
            if (type == "conversation") {
                // The durable thread id for this turn; remember it so the next
                // turn continues the same thread, and persist it (never when
                // incognito -- those ids are ephemeral). content carries it as a
                // precise decimal string.
                const QString id = o.value("content").isString()
                    ? o.value("content").toString()
                    : QString::number(o.value("conversation_id").toVariant().toLongLong());
                if (!id.isEmpty()) {
                    conversationId_ = id;
                    if (!incognito_) saveConversation();
                }
            } else if (type == "reply") {
                *acc += o.value("content").toString();
                chat_->setContent(assistantRow, *acc);
            } else if (type == "thinking") {
                chat_->appendThinking(assistantRow, o.value("content").toString());
            } else if (type == "costStatistics" && o.contains("cost")) {
                chat_->setCost(assistantRow, o.value("cost").toObject().toVariantMap());
                chat_->setProvider(assistantRow, turnProvider);
            } else if (type == "error") {
                if (stream_ == sender()) stream_ = nullptr;
                failTurn(/*userRow=*/assistantRow - 1, assistantRow,
                         QStringLiteral("Error: ") + o.value("content").toString());
            }
        });
        connect(s, &SseStream::complete, this, [this, assistantRow, turnProvider]() {
            stream_ = nullptr;
            finishTurn(assistantRow, turnProvider);
        });
        connect(s, &SseStream::error, this, [this, assistantRow, acc](const QString& reason) {
            stream_ = nullptr;
            if (reason == QLatin1String("unauthorized")) { signOut(); return; }
            failTurn(assistantRow - 1, assistantRow,
                     acc->isEmpty() ? (QStringLiteral("Error: ") + reason) : *acc);
        });
        return;
    }

    // --- Proxy mode: no provider selected -> OpenAI-style chunks ----------
    // The request carries the full conversation (all rows up to and including the
    // new user turn -- i.e. everything except the empty assistant placeholder).
    QJsonArray messages;
    const QVariantList snap = chat_->snapshot();
    for (int i = 0; i < assistantRow && i < snap.size(); ++i) {
        const QVariantMap m = snap.at(i).toMap();
        messages.push_back(QJsonObject{
            {"role", m.value("role").toString()},
            {"content", m.value("content").toString()},
        });
    }

    QJsonObject body{
        {"model", QString::fromLatin1(kDefaultModel)},
        {"messages", messages},
        {"stream", true},
    };
    SseStream* s = api_->openStream("/api/chat", body);
    stream_ = s;
    auto acc = std::make_shared<QString>();

    connect(s, &SseStream::message, this, [this, assistantRow, acc](const QString& chunk) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(chunk.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
        const QJsonObject o = doc.object();
        // A {"session_id":...} control chunk announces the resumable session; the
        // OpenAI chunks then carry choices[0].delta.content.
        if (o.contains("session_id")) {
            *acc = QString();
            chat_->setContent(assistantRow, QString());
            return;
        }
        const QJsonArray choices = o.value("choices").toArray();
        if (!choices.isEmpty()) {
            const QString delta = choices.first().toObject()
                .value("delta").toObject().value("content").toString();
            if (!delta.isEmpty()) {
                *acc += delta;
                chat_->setContent(assistantRow, *acc);
            }
        }
    });
    connect(s, &SseStream::complete, this, [this, assistantRow]() {
        stream_ = nullptr;
        finishTurn(assistantRow, QString());
    });
    connect(s, &SseStream::error, this, [this, assistantRow, acc](const QString& reason) {
        stream_ = nullptr;
        if (reason == QLatin1String("unauthorized")) { signOut(); return; }
        failTurn(assistantRow - 1, assistantRow,
                 acc->isEmpty() ? (QStringLiteral("Error: ") + reason) : *acc);
    });
}

void AppController::finishTurn(int assistantRow, const QString& provider) {
    setStreaming(false);
    if (!provider.isEmpty()) chat_->setProvider(assistantRow, provider);
    saveConversation();
}

void AppController::failTurn(int userRow, int assistantRow, const QString& errorText) {
    // Show the error in the assistant bubble; the failed exchange stays visible
    // for the session but is dropped from persistence (the web client likewise
    // does not persist a turn that errored before completing).
    setStreaming(false);
    chat_->setContent(assistantRow, errorText);
    Q_UNUSED(userRow);
    // Persist a snapshot that excludes the failed pair (the trailing user +
    // assistant rows) by temporarily not saving them.
    saveConversation();
}

void AppController::stopStreaming() {
    if (stream_) {
        stream_->abort();
        stream_ = nullptr;
    }
    if (localEngine_) localEngine_->cancel();
    setStreaming(false);
}

//------------------------------------------------------------------------------
// On-device chat (a local GGUF via llama.cpp) — emits the same reply/finish/fail
// flow the SSE path does, so ChatModel updates identically. `model` is the registry
// entry name; `label` is the picker's display name (used as the turn's provider tag).
//------------------------------------------------------------------------------

void AppController::sendOnDeviceMessage(const QString& model, const QString& label, int assistantRow) {
    const QString turnProvider = label;
    if (model.isEmpty() || !downloader_->isReady(model)) {
        failTurn(assistantRow - 1, assistantRow,
                 QStringLiteral("Error: on-device model not ready. Open ⚙ → On-device AI to download or pick one."));
        return;
    }

    // Conversation context: every row up to and including the new user turn (i.e.
    // all but the empty assistant placeholder at assistantRow).
    QVariantList history;
    const QVariantList snap = chat_->snapshot();
    for (int i = 0; i < assistantRow && i < snap.size(); ++i) {
        const QVariantMap m = snap.at(i).toMap();
        history.push_back(QVariantMap{
            {QStringLiteral("role"), m.value("role").toString()},
            {QStringLiteral("content"), m.value("content").toString()},
        });
    }

    auto acc   = std::make_shared<QString>();
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    auto cleanup = [this, conns]() {
        for (const auto& c : *conns) disconnect(c);
        conns->clear();
    };

    conns->push_back(connect(localEngine_, &LocalLmEngine::replyChunk, this,
        [this, assistantRow, acc](const QString& delta) {
            *acc += delta;
            chat_->setContent(assistantRow, *acc);
        }));
    conns->push_back(connect(localEngine_, &LocalLmEngine::finished, this,
        [this, assistantRow, turnProvider, cleanup]() {
            cleanup();
            finishTurn(assistantRow, turnProvider);
        }));
    conns->push_back(connect(localEngine_, &LocalLmEngine::failed, this,
        [this, assistantRow, acc, cleanup](const QString& reason) {
            cleanup();
            failTurn(assistantRow - 1, assistantRow,
                     acc->isEmpty() ? (QStringLiteral("Error: ") + reason) : *acc);
        }));

    localEngine_->generate(downloader_->pathFor(model), history);
}

//------------------------------------------------------------------------------
// Local per-user conversation persistence (mirrors db.js)
//------------------------------------------------------------------------------

QString AppController::userId() const {
    const QString token = api_->token();
    if (token.isEmpty()) return {};
    const QStringList parts = token.split('.');
    if (parts.size() < 2) return {};
    const QByteArray payload = base64UrlDecode(parts.at(1).toUtf8());
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    const QJsonObject o = doc.object();
    if (o.contains("sub"))   return QString::number(o.value("sub").toVariant().toLongLong());
    if (o.contains("email")) return o.value("email").toString();
    return {};
}

QString AppController::conversationPath() const {
    const QString id = userId();
    if (id.isEmpty()) return {};
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) return {};
    QDir().mkpath(dir);
    // Sanitise the id for a filename (an email contains '@'/'.').
    QString safe = id;
    safe.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    return dir + QStringLiteral("/conversation-") + safe + QStringLiteral(".json");
}

void AppController::loadConversation() {
    const QString path = conversationPath();
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    // Tolerate both the bare array (legacy) and the {conversation_id, messages}
    // wrapper written below, so a returning session resumes the same thread.
    if (doc.isArray()) {
        chat_->loadSnapshot(doc.array().toVariantList());
    } else if (doc.isObject()) {
        const QJsonObject o = doc.object();
        conversationId_ = o.value("conversation_id").toString();
        chat_->loadSnapshot(o.value("messages").toArray().toVariantList());
    }
}

void AppController::saveConversation() {
    // Incognito turns are never persisted (mirrors db skip in chat.js submit).
    if (incognito_) return;
    const QString path = conversationPath();
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QJsonObject wrapper{
        {"conversation_id", conversationId_},
        {"messages", QJsonArray::fromVariantList(chat_->snapshot())},
    };
    f.write(QJsonDocument(wrapper).toJson(QJsonDocument::Compact));
    f.close();
}

//------------------------------------------------------------------------------
// Server-side history (the drawer)
//------------------------------------------------------------------------------

void AppController::loadHistory(int page, int pageSize) {
    const QString path = QStringLiteral("/api/history?page=%1&page_size=%2")
                             .arg(page).arg(pageSize);
    api_->getEnvelope(path,
        [this](const QJsonValue& data) {
            emit historyLoaded(data.toObject().value("summaries").toArray().toVariantList());
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit historyError(msg);
        });
}

void AppController::deleteHistory(const QString& sessionId) {
    if (sessionId.isEmpty()) return;
    QJsonObject body{{"session_id", sessionId}};
    api_->postEnvelope("/api/history/delete", body, nullptr,
        [](const QString&, int) {});
}

void AppController::openConversation(const QString& sessionId) {
    if (sessionId.isEmpty()) return;
    const QString path = QStringLiteral("/api/conversation?id=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(sessionId)));
    api_->getEnvelope(path,
        [this, sessionId](const QJsonValue& data) {
            const QJsonObject o = data.toObject();
            const QJsonArray msgs = o.value("messages").toArray();
            QVariantList out;
            for (const QJsonValue& mv : msgs) {
                const QJsonObject m = mv.toObject();
                QVariantMap row;
                row.insert("role", m.value("role").toString());
                row.insert("content", m.value("content").toString());
                // created_at is epoch ms (matches ChatModel's TimestampRole).
                row.insert("ts", m.value("created_at").toVariant().toLongLong());
                if (m.value("role").toString() == "assistant" && m.contains("provider"))
                    row.insert("provider", m.value("provider").toString());
                out.push_back(row);
            }
            chat_->loadSnapshot(out);
            const QJsonValue idv = o.value("id");
            if (idv.isString())      conversationId_ = idv.toString();
            else if (idv.isDouble()) conversationId_ = QString::number(idv.toVariant().toLongLong());
            else                     conversationId_ = sessionId;
            // Opening a saved conversation leaves incognito (it's a real thread).
            if (incognito_) { incognito_ = false; emit incognitoChanged(); }
            incoSavedValid_ = false;
            // Owned threads stay editable; one shared *to* the user is read-only.
            setReadOnly(!o.value("owned").toBool(true));
            // Mirror an owned/editable thread locally so a reload continues it.
            if (!readOnly_) saveConversation();
        },
        [this](const QString&, int code) {
            if (code == 401) { signOut(); return; }
            // Best-effort: a load failure leaves the drawer open (mirrors web).
        });
}

//------------------------------------------------------------------------------
// New chat + incognito
//------------------------------------------------------------------------------

void AppController::newChat() {
    if (streaming_) return;
    chat_->clear();
    conversationId_.clear();
    setReadOnly(false);
    // Leaves incognito state as-is (that's toggleIncognito's job); clears the
    // local mirror so a reload doesn't resurrect the old thread.
    if (!incognito_) saveConversation();
}

void AppController::toggleIncognito() {
    if (streaming_) return;
    if (!incognito_) {
        // Stash the real conversation and start a blank ephemeral one.
        incoSavedMessages_ = chat_->snapshot();
        incoSavedConvId_   = conversationId_;
        incoSavedReadOnly_ = readOnly_;
        incoSavedValid_    = true;
        incognito_ = true;
        emit incognitoChanged();
        chat_->clear();
        conversationId_.clear();
        setReadOnly(false);
    } else {
        // Restore the stash, discarding the ephemeral turns (never persisted).
        incognito_ = false;
        emit incognitoChanged();
        conversationId_ = incoSavedValid_ ? incoSavedConvId_ : QString();
        setReadOnly(incoSavedValid_ ? incoSavedReadOnly_ : false);
        chat_->loadSnapshot(incoSavedValid_ ? incoSavedMessages_ : QVariantList());
        incoSavedValid_ = false;
        incoSavedMessages_.clear();
        incoSavedConvId_.clear();
    }
}

//------------------------------------------------------------------------------
// Connect EHR (health.ehr.*)
//------------------------------------------------------------------------------

void AppController::ehrSearchProviders(const QString& query) {
    const QString path = QStringLiteral("/health/ehr/providers?q=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed())));
    api_->getEnvelope(path,
        [this](const QJsonValue& data) {
            emit ehrProvidersLoaded(data.toObject().value("providers").toArray().toVariantList());
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit ehrError(msg);
        });
}

void AppController::ehrConnect(const QString& fhirBaseUrl) {
    QString url = fhirBaseUrl.trimmed();
    while (url.endsWith('/')) url.chop(1);
    if (url.isEmpty()) return;
    QJsonObject body{{"fhir_base_url", url}};
    api_->postEnvelope("/health/ehr/authorize", body,
        [this](const QJsonValue& data) {
            const QString authUrl = data.toObject().value("authorize_url").toString();
            if (!authUrl.isEmpty()) {
                // OAuth happens server-side; open the system browser (Android's
                // Intent.ACTION_VIEW equivalent). The user returns and taps Sync.
                QDesktopServices::openUrl(QUrl(authUrl));
                emit ehrConnecting();
            } else {
                emit ehrError(QString());
            }
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit ehrError(msg);
        });
}

void AppController::ehrSync() {
    api_->postEnvelope("/health/ehr/sync", QJsonObject(),
        [this](const QJsonValue& data) {
            emit ehrSynced(data.toObject().value("posted").toInt());
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit ehrError(msg);
        });
}

//------------------------------------------------------------------------------
// Connected devices / vendors
//------------------------------------------------------------------------------

void AppController::loadVendors() {
    api_->getEnvelope("/vendors",
        [this](const QJsonValue& data) {
            emit vendorsLoaded(data.toArray().toVariantList());
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit vendorsError(msg);
        });
    // Icons ship separately as a { id: dataURI } map; fetched in parallel.
    api_->getEnvelope("/vendors/icons",
        [this](const QJsonValue& data) {
            emit vendorIconsLoaded(data.toObject().toVariantMap());
        },
        [](const QString&, int) {});   // icons are best-effort (monogram fallback)
}

void AppController::vendorConnect(const QString& id) {
    if (id.isEmpty()) return;
    const QString path = QStringLiteral("/vendors/%1/authorize")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(id)));
    api_->getEnvelope(path,
        [this](const QJsonValue& data) {
            const QString authUrl = data.toObject().value("authorize_url").toString();
            if (!authUrl.isEmpty()) QDesktopServices::openUrl(QUrl(authUrl));
            emit vendorActionDone();
        },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit vendorsError(msg);
        });
}

void AppController::vendorUnlink(const QString& id) {
    if (id.isEmpty()) return;
    const QString path = QStringLiteral("/vendors/%1/unlink")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(id)));
    api_->postEnvelope(path, QJsonObject(),
        [this](const QJsonValue&) { emit vendorActionDone(); },
        [this](const QString& msg, int code) {
            if (code == 401) { signOut(); return; }
            emit vendorsError(msg);
        });
}
