#include "modeldownloader.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace {
// Curated default model, overridable at build time (build-qt's e2b/e4b + q4/q8
// tokens -> CMake defines). Seeds the list on first run so one-click download works.
#ifndef MIROBODY_GGUF_REPO
#define MIROBODY_GGUF_REPO "ggml-org/gemma-4-E4B-it-GGUF"
#endif
#ifndef MIROBODY_GGUF_FILE
#define MIROBODY_GGUF_FILE "gemma-4-E4B-it-Q4_0.gguf"
#endif

constexpr char kModelsKey[] = "onDeviceModel/models";
constexpr char kLegacyLocalKey[] = "onDeviceModel/localPath";  // migrated on load

// The file name a download URL resolves to (last path segment, minus any query).
QString fileFromUrl(const QString& url) {
    QString u = url;
    const int q = u.indexOf(QLatin1Char('?'));
    if (q >= 0) u = u.left(q);
    const int s = u.lastIndexOf(QLatin1Char('/'));
    return s >= 0 ? u.mid(s + 1) : u;
}
} // namespace

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    load();
    seedDefaultIfEmpty();
    save();
}

ModelDownloader::~ModelDownloader() {
    if (reply_) reply_->abort();
    delete file_;
}

QString ModelDownloader::modelsDir() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base + "/models");
    return base + "/models";
}

ModelDownloader::Entry* ModelDownloader::find(const QString& name) {
    for (auto& e : entries_) if (e.name == name) return &e;
    return nullptr;
}

QString ModelDownloader::statusOf(const Entry& e) const {
    if (e.name == downloadingName_) return QStringLiteral("downloading");
    const QFileInfo fi(e.path);
    return (fi.exists() && fi.size() > 0) ? QStringLiteral("ready") : QStringLiteral("absent");
}

QVariantList ModelDownloader::models() const {
    QVariantList out;
    for (const auto& e : entries_) {
        const QString st = statusOf(e);
        out.push_back(QVariantMap{
            {QStringLiteral("name"),     e.name},
            {QStringLiteral("status"),   st},
            {QStringLiteral("progress"), st == QLatin1String("downloading") ? progress_
                                        : (st == QLatin1String("ready") ? 1.0 : 0.0)},
            {QStringLiteral("remote"),   e.remote},
        });
    }
    return out;
}

QVariantList ModelDownloader::suggestions() const {
    // Curated, verified GGUFs (stable ggml-org / Qwen repos). Entries already in the
    // list are filtered out (matched by URL), so each can be added once.
    struct S { const char* name; const char* url; const char* size; };
    static const S kCatalog[] = {
        { "Gemma 4 E4B (Q4_0)",
          "https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_0.gguf?download=true",
          "4.6 GB" },
        { "Gemma 4 E2B (Q4_0)",
          "https://huggingface.co/ggml-org/gemma-4-E2B-it-GGUF/resolve/main/gemma-4-E2B-it-Q4_0.gguf?download=true",
          "2.8 GB" },
        { "Qwen2.5 3B Instruct (Q4_0)",
          "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_0.gguf?download=true",
          "2.0 GB" },
        { "Qwen2.5 1.5B Instruct (Q4_0)",
          "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_0.gguf?download=true",
          "1.1 GB" },
    };
    QVariantList out;
    for (const auto& s : kCatalog) {
        const QString url = QString::fromLatin1(s.url);
        bool present = false;
        for (const auto& e : entries_) if (e.url == url) { present = true; break; }
        if (present) continue;
        out.push_back(QVariantMap{
            {QStringLiteral("name"),  QString::fromLatin1(s.name)},
            {QStringLiteral("url"),   url},
            {QStringLiteral("label"), QString::fromLatin1(s.name) + QStringLiteral(" · ")
                                      + QString::fromLatin1(s.size)},
        });
    }
    return out;
}

QStringList ModelDownloader::names() const {
    QStringList out;
    for (const auto& e : entries_) out << e.name;
    return out;
}

QString ModelDownloader::pathFor(const QString& name) const {
    for (const auto& e : entries_) if (e.name == name) return e.path;
    return QString();
}

bool ModelDownloader::isReady(const QString& name) const {
    for (const auto& e : entries_) if (e.name == name) return statusOf(e) == QLatin1String("ready");
    return false;
}

QString ModelDownloader::uniqueName(const QString& base) const {
    QString b = base.trimmed();
    if (b.isEmpty()) b = QStringLiteral("model");
    QString name = b;
    int n = 2;
    auto taken = [this](const QString& x) {
        for (const auto& e : entries_) if (e.name == x) return true;
        return false;
    };
    while (taken(name)) name = b + QStringLiteral(" (") + QString::number(n++) + QLatin1Char(')');
    return name;
}

void ModelDownloader::addLocal(const QString& name, const QString& fileOrUrl) {
    QString path = fileOrUrl;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists() || fi.size() <= 0) return;

    Entry e;
    e.name = uniqueName(name.trimmed().isEmpty() ? fi.completeBaseName() : name);
    e.path = path;
    e.remote = false;
    entries_.push_back(e);
    save();
    emit modelsChanged();
}

void ModelDownloader::addRemote(const QString& name, const QString& url) {
    const QString u = url.trimmed();
    if (u.isEmpty()) return;
    const QString file = fileFromUrl(u);
    if (file.isEmpty()) return;

    Entry e;
    e.name = uniqueName(name.trimmed().isEmpty() ? QFileInfo(file).completeBaseName() : name);
    e.url = u;
    e.path = modelsDir() + "/" + file;
    e.remote = true;
    entries_.push_back(e);
    save();
    emit modelsChanged();
}

void ModelDownloader::download(const QString& name) {
    Entry* e = find(name);
    if (!e || !e->remote || reply_) return;               // one download at a time
    if (statusOf(*e) == QLatin1String("ready")) return;

    const QString path = e->path;
    file_ = new QFile(path + ".part");
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file_; file_ = nullptr;
        return;
    }
    downloadingName_ = name;
    progress_ = 0.0;
    emit modelsChanged();

    QNetworkRequest req{QUrl(e->url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = nam_->get(req);

    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) { progress_ = double(got) / double(total); emit modelsChanged(); }
    });
    // Capture path by value so the handler survives the entry being removed mid-download.
    connect(reply_, &QNetworkReply::finished, this, [this, path]() {
        const bool ok = reply_->error() == QNetworkReply::NoError;
        if (file_) { file_->write(reply_->readAll()); file_->close(); }
        reply_->deleteLater(); reply_ = nullptr;
        delete file_; file_ = nullptr;
        downloadingName_.clear();
        if (ok) { QFile::remove(path); QFile::rename(path + ".part", path); }
        else    { QFile::remove(path + ".part"); }
        emit modelsChanged();
    });
}

void ModelDownloader::cancel() {
    if (reply_) reply_->abort();   // the finished handler removes the .part file
}

void ModelDownloader::remove(const QString& name) {
    Entry* e = find(name);
    if (!e) return;
    const bool remote = e->remote;
    const QString path = e->path;
    if (downloadingName_ == name && reply_) reply_->abort();   // async cleanup of .part
    for (int i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name) { entries_.remove(i); break; }
    if (remote) QFile::remove(path);                           // never delete a user's local file
    save();
    emit modelsChanged();
}

void ModelDownloader::seedDefaultIfEmpty() {
    if (!entries_.isEmpty()) return;
    const QString file = QString::fromLatin1(MIROBODY_GGUF_FILE);
    Entry e;
    e.remote = true;
    e.url  = QStringLiteral("https://huggingface.co/" MIROBODY_GGUF_REPO "/resolve/main/"
                            MIROBODY_GGUF_FILE "?download=true");
    e.path = modelsDir() + "/" + file;
    e.name = QFileInfo(file).completeBaseName();
    entries_.push_back(e);
}

void ModelDownloader::load() {
    QSettings s;
    const QJsonArray arr =
        QJsonDocument::fromJson(s.value(QString::fromLatin1(kModelsKey)).toString().toUtf8()).array();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.name   = o.value("name").toString();
        e.url    = o.value("url").toString();
        e.path   = o.value("path").toString();
        e.remote = o.value("remote").toBool();
        if (!e.name.isEmpty()) entries_.push_back(e);
    }

    // Migrate the pre-registry single-model local override into an entry.
    const QString oldLocal = s.value(QString::fromLatin1(kLegacyLocalKey)).toString();
    if (!oldLocal.isEmpty() && QFileInfo::exists(oldLocal)) {
        bool have = false;
        for (const auto& e : entries_) if (e.path == oldLocal) have = true;
        if (!have) {
            Entry e;
            e.name = uniqueName(QFileInfo(oldLocal).completeBaseName());
            e.path = oldLocal;
            entries_.push_back(e);
        }
        s.remove(QString::fromLatin1(kLegacyLocalKey));
    }
}

void ModelDownloader::save() const {
    QJsonArray arr;
    for (const auto& e : entries_) {
        QJsonObject o;
        o["name"]   = e.name;
        o["url"]    = e.url;
        o["path"]   = e.path;
        o["remote"] = e.remote;
        arr.append(o);
    }
    QSettings s;
    s.setValue(QString::fromLatin1(kModelsKey),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}
