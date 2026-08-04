#pragma once

// On-device model registry for the Qt client: a user-managed list of GGUF models,
// each either a remote download (Hugging Face etc.) or a pointer to a local file
// already on disk. Which model is *used* is chosen in the provider picker (one
// picker entry per model), not here — this dialog only adds/downloads/removes.
// Persisted across runs (QSettings). The list is seeded on first run with a curated
// default (the MIROBODY_GGUF_* build defines) so one-click download still works.
// Since llama.cpp is model-agnostic, any GGUF works — not just Gemma.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class ModelDownloader : public QObject {
    Q_OBJECT
    // The model list for QML: each item is a map {name, status, progress, remote}.
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    // Curated downloadable models not yet added: {name, url, label} (label = "name · size").
    Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY modelsChanged)
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    QVariantList models() const;
    QVariantList suggestions() const;
    QStringList  names() const;                       // entry names, for the provider picker
    QString      pathFor(const QString& name) const;  // absolute file path of an entry (may not exist)
    Q_INVOKABLE  bool isReady(const QString& name) const;  // its file is present on disk

    // Register an existing local GGUF (accepts a path or a file: URL). No copy is made.
    Q_INVOKABLE void addLocal(const QString& name, const QString& fileOrUrl);
    // Register a downloadable GGUF by URL (not fetched until download() is called).
    Q_INVOKABLE void addRemote(const QString& name, const QString& url);
    // Start (or restart) downloading a remote entry. One download at a time.
    Q_INVOKABLE void download(const QString& name);
    // Abort the in-flight download.
    Q_INVOKABLE void cancel();
    // Remove an entry: a remote entry also deletes its downloaded file; a local
    // entry is only forgotten (the user's file is never deleted).
    Q_INVOKABLE void remove(const QString& name);

signals:
    void modelsChanged();

private:
    struct Entry {
        QString name;
        QString url;    // remote download URL ("" for a local entry)
        QString path;   // local: the user's file path; remote: modelsDir()/<derived file>
        bool    remote = false;
    };

    QString  statusOf(const Entry& e) const;   // "absent" | "downloading" | "ready"
    Entry*   find(const QString& name);
    QString  modelsDir() const;
    QString  uniqueName(const QString& base) const;
    void     load();
    void     save() const;
    void     seedDefaultIfEmpty();

    QNetworkAccessManager* nam_ = nullptr;
    QVector<Entry> entries_;

    // In-flight download state (one at a time).
    QPointer<QNetworkReply> reply_;
    QFile*  file_ = nullptr;
    QString downloadingName_;
    double  progress_ = 0.0;
};
