#pragma once

// Thin HTTP/SSE client for the mirobody server API, the Qt counterpart of the
// web client's net.js. It owns one QNetworkAccessManager, prepends the
// configured base URL to every request, and attaches the bearer token. Two
// shapes are exposed: postEnvelope() for the standard {code, msg, data} JSON
// reply, and openStream() for the Server-Sent-Events response of POST /api/chat.

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// A single in-flight SSE request. Mirrors net.js's `stream()`: each event's
// "data:" payload is emitted via message(), the terminal "[DONE]" frame fires
// complete(), and an "event: error" frame (or any transport/HTTP failure) fires
// error(). The caller can cancel with abort(). The object deletes itself once it
// has emitted exactly one of complete()/error().
class SseStream : public QObject {
    Q_OBJECT
public:
    explicit SseStream(QNetworkReply* reply, QObject* parent = nullptr);

    // Cancel the stream. Emits nothing further (the request is aborted silently,
    // matching the web client's AbortController behaviour).
    void abort();

signals:
    void message(const QString& payload);
    void complete();
    void error(const QString& reason);

private slots:
    void onReadyRead();
    void onFinished();

private:
    void dispatchBlock(const QString& block);
    void done(bool ok, const QString& reason = QString());

    QNetworkReply* reply_ = nullptr;
    QByteArray     buffer_;
    bool           finished_ = false;
    bool           aborted_  = false;
};

class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(QObject* parent = nullptr);

    void    setBaseUrl(const QString& url) { baseUrl_ = url; }
    QString baseUrl() const { return baseUrl_; }

    void    setToken(const QString& token) { token_ = token; }
    QString token() const { return token_; }

    // POST `body` as JSON to `path` and unwrap the {code, msg, data} envelope.
    // onSuccess receives the `data` value; onError receives (msg, httpOrCode).
    // When withAuth is true (default) the bearer token rides on the request.
    void postEnvelope(const QString& path,
                      const QJsonObject& body,
                      std::function<void(const QJsonValue&)> onSuccess,
                      std::function<void(const QString&, int)> onError,
                      bool withAuth = true);

    // GET `path` and unwrap the same envelope (used by the history drawer).
    void getEnvelope(const QString& path,
                     std::function<void(const QJsonValue&)> onSuccess,
                     std::function<void(const QString&, int)> onError);

    // POST raw `body` (e.g. application/fhir+json) to `path` WITHOUT the
    // {code,msg,data} envelope -- the FHIR R4 endpoint answers with a bare
    // resource and a 2xx. `onDone(ok, httpStatus)`; the bearer token rides along.
    // Used by BleHealth to ingest Observations, mirroring the mobile apps.
    void postRaw(const QString& path,
                 const QByteArray& contentType,
                 const QByteArray& body,
                 std::function<void(bool, int)> onDone);

    // Open an SSE stream by POSTing `body` to `path`. The returned SseStream is
    // parented to this client; connect to its signals before the event loop
    // turns. Never returns null.
    SseStream* openStream(const QString& path, const QJsonObject& body);

private:
    QString absoluteUrl(const QString& path) const;

    QNetworkAccessManager* nam_ = nullptr;
    QString baseUrl_;
    QString token_;
};
