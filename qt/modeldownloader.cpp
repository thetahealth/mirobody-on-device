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
constexpr char kModelsKey[] = "onDeviceModel/models";
constexpr char kLegacyLocalKey[] = "onDeviceModel/localPath";  // migrated on load

// The models the app offers out of the box. The same four Android carries, as GGUF:
// llama.cpp is this client's only engine, so the `.litertlm` half of Android's catalog
// (which exists because Google's runtime reads Gemma's MatFormer layout faster on a
// phone) has nothing to add on a desktop, where the same weights are downloadable in a
// format one engine can read.
//
// Ordered smallest -> largest so a modest machine sees the light options first; the
// bigger ones want roughly their own size in RAM again at inference.
//
// `bytes` is Hugging Face's own `lfs.size` for that exact file, read from the repo's
// tree API -- not an estimate. It is the presence gate as well as the pre-flight number
// (see statusOf), so re-read it from the API if a URL is ever repointed: a stale length
// makes a correctly downloaded file look absent forever.
struct CatalogItem { const char* id; const char* name; const char* url; qint64 bytes; const char* ram; };
constexpr CatalogItem kCatalog[] = {
    { "qwen35-2b", "Qwen3.5 2B",
      "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf?download=true",
      1'280'835'840LL, "4 GB+" },
    { "qwen35-4b", "Qwen3.5 4B",
      "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf?download=true",
      2'740'937'888LL, "8 GB+" },
    { "gemma-4-e2b", "Gemma 4 E2B",
      "https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF/resolve/main/gemma-4-E2B-it-Q4_K_M.gguf?download=true",
      3'106'738'272LL, "6 GB+" },
    { "gemma-4-e4b", "Gemma 4 E4B",
      "https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_K_M.gguf?download=true",
      4'977'171'584LL, "8 GB+" },
};

// The file name a download URL resolves to (last path segment, minus any query).
QString fileFromUrl(const QString& url) {
    QString u = url;
    const int q = u.indexOf(QLatin1Char('?'));
    if (q >= 0) u = u.left(q);
    const int s = u.lastIndexOf(QLatin1Char('/'));
    return s >= 0 ? u.mid(s + 1) : u;
}

// Human-readable byte count ("2.9 GB"), matching Android's formatBytes.
QString formatBytes(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("0 B");
    static const char* const units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = double(bytes);
    int i = 0;
    while (value >= 1024 && i < 4) { value /= 1024; ++i; }
    return i == 0 ? QString::number(bytes) + QStringLiteral(" B")
                  : QString::asprintf("%.1f %s", value, units[i]);
}
} // namespace

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    // The catalog is compiled in, so it is rebuilt rather than loaded: a release that
    // adds or repoints a model takes effect without migrating anyone's settings.
    for (const auto& c : kCatalog) {
        Entry e;
        e.id      = QString::fromLatin1(c.id);
        e.name    = QString::fromLatin1(c.name);
        e.url     = QString::fromLatin1(c.url);
        e.path    = modelsDir() + "/" + fileFromUrl(e.url);
        e.bytes   = c.bytes;
        e.ram     = QString::fromLatin1(c.ram);
        e.builtin = true;
        entries_.push_back(e);
    }
    load();
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

ModelDownloader::Entry* ModelDownloader::find(const QString& id) {
    for (auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

const ModelDownloader::Entry* ModelDownloader::find(const QString& id) const {
    for (const auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

QString ModelDownloader::statusOf(const Entry& e) const {
    if (e.id == downloadingId_) return QStringLiteral("downloading");
    const QFileInfo fi(e.path);
    // For a catalog model "present" means the RIGHT file: the models directory outlives
    // any single run, so a leftover can be a foreign build renamed to look right, or a
    // half-copied file. Its exact length is known, and comparing it costs a stat -- the
    // cheap half of the check Android pays a full SHA-256 for. A user's own file has no
    // length to check against, only that it is there.
    if (fi.exists() && fi.size() > 0 && (!e.builtin || fi.size() == e.bytes))
        return QStringLiteral("ready");
    if (e.id == failedId_) return QStringLiteral("failed");
    return QStringLiteral("absent");
}

QVariantMap ModelDownloader::rowFor(const Entry& e) const {
    const QString st = statusOf(e);
    const bool dl = st == QLatin1String("downloading");
    // A catalog model advertises the download's size before there is a file; a user
    // entry can only report the file it actually has (empty until it has one, so QML
    // leaves the subtitle off rather than printing "0 B").
    const qint64 size = e.bytes > 0 ? e.bytes : QFileInfo(e.path).size();
    return QVariantMap{
        {QStringLiteral("id"),         e.id},
        {QStringLiteral("name"),       e.name},
        {QStringLiteral("size"),       size > 0 ? formatBytes(size) : QString()},
        {QStringLiteral("ram"),        e.ram},
        {QStringLiteral("file"),       QFileInfo(e.path).fileName()},
        {QStringLiteral("status"),     st},
        {QStringLiteral("progress"),   dl && total_ > 0 ? double(received_) / double(total_)
                                     : (st == QLatin1String("ready") ? 1.0 : 0.0)},
        {QStringLiteral("downloaded"), dl ? formatBytes(received_) : QString()},
        {QStringLiteral("total"),      dl && total_ > 0 ? formatBytes(total_) : QString()},
        {QStringLiteral("error"),      e.id == failedId_ ? failedMessage_ : QString()},
        {QStringLiteral("remote"),     !e.url.isEmpty()},
        {QStringLiteral("builtin"),    e.builtin},
    };
}

QVariantList ModelDownloader::catalog() const {
    QVariantList out;
    for (const auto& e : entries_) if (e.builtin) out.push_back(rowFor(e));
    return out;
}

QVariantList ModelDownloader::imported() const {
    QVariantList out;
    for (const auto& e : entries_) if (!e.builtin) out.push_back(rowFor(e));
    return out;
}

QVariantList ModelDownloader::readyModels() const {
    QVariantList out;
    for (const auto& e : entries_) {
        if (statusOf(e) != QLatin1String("ready")) continue;
        out.push_back(QVariantMap{{QStringLiteral("id"), e.id}, {QStringLiteral("name"), e.name}});
    }
    return out;
}

QString ModelDownloader::pathFor(const QString& id) const {
    const Entry* e = find(id);
    return e ? e->path : QString();
}

bool ModelDownloader::isReady(const QString& id) const {
    const Entry* e = find(id);
    return e && statusOf(*e) == QLatin1String("ready");
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
    e.id   = QStringLiteral("user-") + e.name;
    e.path = path;
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
    e.id   = QStringLiteral("user-") + e.name;
    e.url  = u;
    e.path = modelsDir() + "/" + file;
    entries_.push_back(e);
    save();
    emit modelsChanged();
}

void ModelDownloader::download(const QString& id) {
    Entry* e = find(id);
    if (!e || e->url.isEmpty() || reply_) return;         // one download at a time
    if (statusOf(*e) == QLatin1String("ready")) return;

    const QString path = e->path;
    file_ = new QFile(path + ".part");
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file_; file_ = nullptr;
        failedId_ = id;
        failedMessage_ = QStringLiteral("Cannot write to ") + QFileInfo(path).absolutePath();
        emit modelsChanged();
        return;
    }
    downloadingId_ = id;
    received_ = 0;
    total_ = 0;
    failedId_.clear();
    failedMessage_.clear();
    emit modelsChanged();

    QNetworkRequest req{QUrl(e->url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = nam_->get(req);

    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        received_ = got;
        total_ = total;
        emit modelsChanged();
    });
    // Capture path/id by value so the handler survives the entry being removed mid-download.
    connect(reply_, &QNetworkReply::finished, this, [this, id, path]() {
        const bool ok = reply_->error() == QNetworkReply::NoError;
        // An abort is the user's own cancel, not a failure to report back to them.
        const bool aborted = reply_->error() == QNetworkReply::OperationCanceledError;
        const QString err = reply_->errorString();
        if (file_) { file_->write(reply_->readAll()); file_->close(); }
        reply_->deleteLater(); reply_ = nullptr;
        delete file_; file_ = nullptr;
        downloadingId_.clear();
        received_ = total_ = 0;
        if (ok) { QFile::remove(path); QFile::rename(path + ".part", path); }
        else    { QFile::remove(path + ".part"); }
        if (!ok && !aborted) { failedId_ = id; failedMessage_ = err; }
        emit modelsChanged();
    });
}

void ModelDownloader::cancel() {
    if (reply_) reply_->abort();   // the finished handler removes the .part file
}

void ModelDownloader::remove(const QString& id) {
    Entry* e = find(id);
    if (!e) return;
    const bool builtin = e->builtin;
    const bool ours = !e->url.isEmpty();   // we downloaded it; an import is the user's own
    const QString path = e->path;
    if (downloadingId_ == id && reply_) reply_->abort();       // async cleanup of .part
    if (failedId_ == id) { failedId_.clear(); failedMessage_.clear(); }
    // A catalog model is part of the app, so deleting it frees the disk and leaves the
    // row offering the download again; a user entry disappears entirely.
    if (!builtin) {
        for (int i = 0; i < entries_.size(); ++i)
            if (entries_[i].id == id) { entries_.remove(i); break; }
    }
    if (ours) QFile::remove(path);
    save();
    emit modelsChanged();
}

void ModelDownloader::load() {
    QSettings s;
    const QJsonArray arr =
        QJsonDocument::fromJson(s.value(QString::fromLatin1(kModelsKey)).toString().toUtf8()).array();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.name = o.value("name").toString();
        e.url  = o.value("url").toString();
        e.path = o.value("path").toString();
        if (e.name.isEmpty()) continue;
        // Pre-catalog settings stored no id, and one release's seeded default is another's
        // catalog entry: drop anything pointing at a file the catalog now owns rather than
        // listing the same model twice.
        bool dup = false;
        for (const auto& c : entries_) if (c.builtin && c.path == e.path) dup = true;
        if (dup) continue;
        e.id = o.value("id").toString();
        if (e.id.isEmpty()) e.id = QStringLiteral("user-") + e.name;
        entries_.push_back(e);
    }

    // Migrate the pre-registry single-model local override into an entry.
    const QString oldLocal = s.value(QString::fromLatin1(kLegacyLocalKey)).toString();
    if (!oldLocal.isEmpty() && QFileInfo::exists(oldLocal)) {
        bool have = false;
        for (const auto& e : entries_) if (e.path == oldLocal) have = true;
        if (!have) {
            Entry e;
            e.name = uniqueName(QFileInfo(oldLocal).completeBaseName());
            e.id   = QStringLiteral("user-") + e.name;
            e.path = oldLocal;
            entries_.push_back(e);
        }
        s.remove(QString::fromLatin1(kLegacyLocalKey));
    }
}

void ModelDownloader::save() const {
    QJsonArray arr;
    for (const auto& e : entries_) {
        if (e.builtin) continue;      // the catalog is compiled in, not persisted
        QJsonObject o;
        o["id"]   = e.id;
        o["name"] = e.name;
        o["url"]  = e.url;
        o["path"] = e.path;
        arr.append(o);
    }
    QSettings s;
    s.setValue(QString::fromLatin1(kModelsKey),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}
