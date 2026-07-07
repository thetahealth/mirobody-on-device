#include "apiclient.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QStringList>
#include <QUrl>

//------------------------------------------------------------------------------
// SseStream
//------------------------------------------------------------------------------

SseStream::SseStream(QNetworkReply* reply, QObject* parent)
    : QObject(parent), reply_(reply) {
    reply_->setParent(this);
    connect(reply_, &QNetworkReply::readyRead, this, &SseStream::onReadyRead);
    connect(reply_, &QNetworkReply::finished,  this, &SseStream::onFinished);
}

void SseStream::abort() {
    aborted_ = true;
    finished_ = true;
    if (reply_) reply_->abort();
    deleteLater();
}

void SseStream::onReadyRead() {
    if (finished_) return;
    buffer_ += reply_->readAll();

    // SSE event blocks are separated by a blank line ("\n\n"). Normalise CRLF so
    // a server (or proxy) that uses "\r\n" still splits correctly.
    buffer_.replace("\r\n", "\n");
    int idx;
    while (!finished_ && (idx = buffer_.indexOf("\n\n")) >= 0) {
        const QString block = QString::fromUtf8(buffer_.left(idx));
        buffer_.remove(0, idx + 2);
        dispatchBlock(block);
    }
}

void SseStream::onFinished() {
    if (finished_) { return; }
    // Surface a transport/HTTP error; a 401 is reported verbatim so the caller
    // can sign the user out (matching net.js).
    if (reply_->error() != QNetworkReply::NoError) {
        const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        done(false, http == 401 ? QStringLiteral("unauthorized")
                                : reply_->errorString());
        return;
    }
    // Stream ended without an explicit [DONE]; treat a clean close as success.
    done(true);
}

// Parse one event block: collect its "data:" lines, read the "event:" name, and
// route the terminal / error frames. Mirrors net.js dispatch().
void SseStream::dispatchBlock(const QString& block) {
    QString payload;
    QString event;
    const QStringList lines = block.split('\n');
    for (const QString& line : lines) {
        if (line.startsWith("data:")) {
            QString chunk = line.mid(5);
            if (chunk.startsWith(' ')) chunk.remove(0, 1);
            payload += (payload.isEmpty() ? QString() : QStringLiteral("\n")) + chunk;
        } else if (line.startsWith("event:")) {
            event = line.mid(6).trimmed();
        }
    }

    if (payload.isEmpty()) return;            // keepalive / comment line
    if (event == QLatin1String("error")) {
        done(false, payload);
    } else if (payload == QLatin1String("[DONE]")) {
        done(true);
    } else {
        emit message(payload);
    }
}

void SseStream::done(bool ok, const QString& reason) {
    if (finished_) return;
    finished_ = true;
    if (!aborted_) {
        if (ok) emit complete();
        else    emit error(reason);
    }
    deleteLater();
}

//------------------------------------------------------------------------------
// ApiClient
//------------------------------------------------------------------------------

ApiClient::ApiClient(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

QString ApiClient::absoluteUrl(const QString& path) const {
    return baseUrl_.isEmpty() ? path : baseUrl_ + path;
}

void ApiClient::postEnvelope(const QString& path,
                             const QJsonObject& body,
                             std::function<void(const QJsonValue&)> onSuccess,
                             std::function<void(const QString&, int)> onError,
                             bool withAuth) {
    QNetworkRequest req{QUrl(absoluteUrl(path))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=utf-8"));
    if (withAuth && !token_.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token_.toUtf8());
    }

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = nam_->post(req, payload);

    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && http != 200) {
            if (onError) onError(http ? QStringLiteral("HTTP %1").arg(http)
                                      : reply->errorString(), http);
            return;
        }

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            if (onError) onError(QStringLiteral("bad response"), http);
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            if (onError) onError(obj.value(QStringLiteral("msg")).toString(), code);
            return;
        }
        if (onSuccess) onSuccess(obj.value(QStringLiteral("data")));
    });
}

void ApiClient::getEnvelope(const QString& path,
                            std::function<void(const QJsonValue&)> onSuccess,
                            std::function<void(const QString&, int)> onError) {
    QNetworkRequest req{QUrl(absoluteUrl(path))};
    if (!token_.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token_.toUtf8());
    }
    QNetworkReply* reply = nam_->get(req);

    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && http != 200) {
            if (onError) onError(http ? QStringLiteral("HTTP %1").arg(http)
                                      : reply->errorString(), http);
            return;
        }
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            if (onError) onError(QStringLiteral("bad response"), http);
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            if (onError) onError(obj.value(QStringLiteral("msg")).toString(), code);
            return;
        }
        if (onSuccess) onSuccess(obj.value(QStringLiteral("data")));
    });
}

void ApiClient::postRaw(const QString& path,
                        const QByteArray& contentType,
                        const QByteArray& body,
                        std::function<void(bool, int)> onDone) {
    QNetworkRequest req{QUrl(absoluteUrl(path))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!token_.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token_.toUtf8());
    }
    QNetworkReply* reply = nam_->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [reply, onDone]() {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && http >= 200 && http < 300;
        if (onDone) onDone(ok, http);
    });
}

SseStream* ApiClient::openStream(const QString& path, const QJsonObject& body) {
    QNetworkRequest req{QUrl(absoluteUrl(path))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=utf-8"));
    req.setRawHeader("Accept", "text/event-stream");
    if (!token_.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token_.toUtf8());
    }
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = nam_->post(req, payload);
    return new SseStream(reply, this);
}
