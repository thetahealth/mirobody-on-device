#pragma once

// On-device model registry for the Qt client: a built-in CATALOG of GGUF models plus
// whatever the user has added themselves (a local file already on disk, or any other
// GGUF by URL). Which model is *used* is chosen either here — clicking a downloaded
// model picks it — or in the provider picker, where every backend, server or local, is
// comparable. The Qt analogue of Android's `OnDeviceModel` + `ModelManager`.
//
// GGUF only, because llama.cpp is the Qt client's only engine. Android carries a second
// runtime (LiteRT-LM) for the `.litertlm` builds Google publishes for phones; on the
// desktop that lane buys nothing, so the catalog here is the same four models as
// Android's, all as the GGUF each one is downloadable in.
//
// The catalog is not persisted — it is a compiled-in table, rebuilt every run, so a new
// release's models simply appear. Only the user's own entries are saved (QSettings).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class ModelDownloader : public QObject {
    Q_OBJECT
    // The built-in models, smallest first: {id, name, size, ram, status, progress,
    // downloaded, total, error}.
    Q_PROPERTY(QVariantList catalog READ catalog NOTIFY modelsChanged)
    // The user's own entries (imported file / added URL): the same map plus {remote}.
    Q_PROPERTY(QVariantList imported READ imported NOTIFY modelsChanged)
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    QVariantList catalog() const;
    QVariantList imported() const;

    // --- the provider picker's view of the registry (id-keyed) ---------------
    // Every model whose file is present, as [{id, name}] — one picker entry each.
    QVariantList readyModels() const;
    QString      pathFor(const QString& id) const;   // absolute file path (may not exist)
    Q_INVOKABLE bool isReady(const QString& id) const;

    // Register an existing local GGUF (accepts a path or a file: URL). No copy is made.
    Q_INVOKABLE void addLocal(const QString& name, const QString& fileOrUrl);
    // Register a downloadable GGUF by URL (not fetched until download() is called).
    Q_INVOKABLE void addRemote(const QString& name, const QString& url);
    // Start (or restart) a download. One at a time.
    Q_INVOKABLE void download(const QString& id);
    // Abort the in-flight download.
    Q_INVOKABLE void cancel();
    // Delete a catalog model's downloaded file (the row stays, offering the download
    // again); for a user entry, also forget the row — and never delete the user's own
    // file, only one we downloaded.
    Q_INVOKABLE void remove(const QString& id);

signals:
    void modelsChanged();

private:
    struct Entry {
        QString id;
        QString name;
        QString url;      // download URL ("" for an imported local file)
        QString path;     // imported: the user's file; otherwise modelsDir()/<file>
        qint64  bytes = 0;  // catalog: the file's exact length; else 0 (unknown)
        QString ram;      // recommended RAM, catalog only
        bool    builtin = false;
    };

    QString  statusOf(const Entry& e) const;   // "absent" | "downloading" | "ready" | "failed"
    QVariantMap rowFor(const Entry& e) const;
    Entry*   find(const QString& id);
    const Entry* find(const QString& id) const;
    QString  modelsDir() const;
    QString  uniqueName(const QString& base) const;
    void     load();
    void     save() const;

    QNetworkAccessManager* nam_ = nullptr;
    // Catalog entries first (in table order), then the user's, so `readyModels()`
    // offers the curated models ahead of the improvised ones.
    QVector<Entry> entries_;

    // In-flight download state (one at a time).
    QPointer<QNetworkReply> reply_;
    QFile*  file_ = nullptr;
    QString downloadingId_;
    qint64  received_ = 0;
    qint64  total_    = 0;
    // Last failure, so a dead download reads as "Retry" rather than silently reverting
    // to "not downloaded" — an unreachable host and a never-started download look
    // identical otherwise.
    QString failedId_;
    QString failedMessage_;
};
