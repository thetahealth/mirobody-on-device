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

namespace {

// localStorage key names, kept identical to net.js / config.js so a future
// shared store would line up; here they are QSettings keys.
const char* kTokenKey    = "mirobody-x-token";
const char* kBaseUrlKey  = "mirobody-base-url";
const char* kProviderKey = "mirobody-provider";
const char* kLanguageKey = "mirobody-language";
const char* kFontKey     = "mirobody-font-offset";

const char* kDefaultModel = "gemini-2.5-flash";

// Synthetic, client-only provider that runs Gemma 4 fully on-device (no server).
// Mirrors ProviderInfo.onDevice on Android/iOS.
const char* kOnDeviceName = "Gemma 4 \xC2\xB7 On-device"; // "Gemma 4 · On-device" (UTF-8)
const char* kOnDeviceCode = "__ondevice_gemma4__";

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

// The `email` claim of a JWT, or "" when absent/unparseable. Used to restore the
// signed-in address into the settings menu on a returning session.
QString emailClaim(const QString& token) {
    const QStringList parts = token.split('.');
    if (parts.size() < 2) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(base64UrlDecode(parts.at(1).toUtf8()));
    return doc.isObject() ? doc.object().value("email").toString() : QString();
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
    const QString token = settings_->value(kTokenKey).toString();
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
    // environments (config.js BASE_URL_PRESETS).
    return {
        QStringLiteral("http://127.0.0.1:8080"),
        QStringLiteral("https://test.mirobody.ai"),
        QStringLiteral("https://gray.mirobody.ai"),
        QStringLiteral("https://mirobody.ai"),
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
        [this, e](const QJsonValue& data) {
            const QString token = data.toObject().value("access_token").toString();
            if (token.isEmpty()) { emit verifyError(QString()); return; }
            setEmail(e);
            completeLogin(token);
        },
        [this](const QString& msg, int) { emit verifyError(msg); },
        /*withAuth=*/false);
}

void AppController::completeLogin(const QString& accessToken) {
    api_->setToken(accessToken);
    settings_->setValue(kTokenKey, accessToken);

    chat_->clear();
    loadConversation();      // restore THIS user's persisted conversation
    loadProviders();

    if (!loggedIn_) { loggedIn_ = true; emit loggedInChanged(); }
    // The email the user just signed in with is shown in the settings menu.
}

void AppController::signOut() {
    stopStreaming();
    // Drop this user's local history while we still know who they are (the key
    // is derived from the token), then clear the token -- mirrors app.signOut().
    const QString path = conversationPath();
    if (!path.isEmpty()) QFile::remove(path);

    api_->setToken(QString());
    settings_->remove(kTokenKey);
    chat_->clear();
    providers_.clear();
    emit providersChanged();
    setEmail(QString());

    if (loggedIn_) { loggedIn_ = false; emit loggedInChanged(); }
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

//------------------------------------------------------------------------------
// Providers
//------------------------------------------------------------------------------

void AppController::loadProviders() {
    if (api_->token().isEmpty()) return;
    api_->postEnvelope("/api/providers", QJsonObject(),
        [this](const QJsonValue& data) {
            providers_.clear();
            // /api/providers returns groups: [{ agent, providers[] }]. Flatten to one
            // entry per model. "name" is the display + selection value (QML textRole/
            // valueRole = "name"); "agent" ("" for the default agent) rides on the
            // request. "code" mirrors the model here and is only special-cased for the
            // on-device sentinel in QML.
            const QJsonArray arr = data.toArray();
            QStringList names;
            for (const QJsonValue& gv : arr) {
                const QJsonObject g = gv.toObject();
                const QString agent = g.value("agent").toString();
                const QJsonArray provs = g.value("providers").toArray();
                for (const QJsonValue& pv : provs) {
                    const QString p = pv.toString();
                    if (p.isEmpty()) continue;
                    names << p;
                    QVariantMap m;
                    m.insert("name", p);
                    m.insert("code", p);
                    m.insert("agent", agent);
                    providers_.push_back(m);
                }
            }
            // Always offer the on-device provider alongside the server's.
            appendOnDeviceProvider();
            names << QString::fromUtf8(kOnDeviceName);
            // Restore the cached selection if still on offer, else default to the
            // first provider (config/chat.js setProviderOptions).
            if (!names.isEmpty() && !names.contains(provider_)) {
                setProvider(names.first());
            }
            emit providersChanged();
        },
        [this](const QString&, int code) {
            if (code == 401) { signOut(); return; }
            // Server unreachable, but on-device chat still works offline — keep it.
            if (providers_.isEmpty()) {
                appendOnDeviceProvider();
                if (provider_.isEmpty()) setProvider(QString::fromUtf8(kOnDeviceName));
                emit providersChanged();
            }
        });
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

    // --- On-device mode: route to the local engine, no server ------------
    if (turnProvider == QString::fromUtf8(kOnDeviceName)) {
        sendOnDeviceMessage(q, assistantRow);
        return;
    }

    // --- Agent mode: a provider is selected -------------------------------
    // turnProvider is the selected model name; look up its agent from providers_
    // ("" for the default agent). Send {agent, provider} verbatim -- no parsing.
    if (!turnProvider.isEmpty()) {
        QString agentName;
        for (const QVariant& v : providers_) {
            const QVariantMap m = v.toMap();
            if (m.value("name").toString() == turnProvider) {
                agentName = m.value("agent").toString();
                break;
            }
        }
        const QString providerName = turnProvider;

        QJsonObject body{
            {"agent", agentName},
            {"provider", providerName},
            {"question", q},
            {"language", language_},
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
            if (type == "reply") {
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
// On-device chat (Gemma 4 via LiteRT-LM) — emits the same reply/finish/fail flow
// the SSE path does, so ChatModel updates identically.
//------------------------------------------------------------------------------

void AppController::appendOnDeviceProvider() {
    QVariantMap m;
    m.insert("code", QString::fromUtf8(kOnDeviceCode));
    m.insert("name", QString::fromUtf8(kOnDeviceName));
    providers_.push_back(m);
}

void AppController::sendOnDeviceMessage(const QString& question, int assistantRow) {
    Q_UNUSED(question);
    const QString turnProvider = QString::fromUtf8(kOnDeviceName);
    if (!downloader_->isReady()) {
        failTurn(assistantRow - 1, assistantRow,
                 QStringLiteral("Error: on-device model not downloaded yet."));
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

    localEngine_->generate(downloader_->modelPath(), history);
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
    if (!doc.isArray()) return;
    // Also surface the signed-in email (decoded above) into the settings menu.
    chat_->loadSnapshot(doc.array().toVariantList());
}

void AppController::saveConversation() {
    const QString path = conversationPath();
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(QJsonArray::fromVariantList(chat_->snapshot())).toJson(QJsonDocument::Compact));
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
