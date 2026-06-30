#include "modeldownloader.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

namespace {
// Gemma 4 E2B in LiteRT-LM format — Apache-2.0, ungated (litert-community).
constexpr char kModelFile[] = "gemma-4-E2B-it.litertlm";
constexpr char kDownloadUrl[] =
    "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/model.litertlm?download=true";
} // namespace

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    if (isReady()) status_ = QStringLiteral("ready");
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

QString ModelDownloader::modelPath() const {
    return modelsDir() + "/" + QString::fromLatin1(kModelFile);
}

bool ModelDownloader::isReady() const {
    QFile f(modelPath());
    return f.exists() && f.size() > 0;
}

void ModelDownloader::setStatus(const QString& s) {
    if (status_ == s) return;
    status_ = s;
    emit statusChanged();
}

void ModelDownloader::setProgress(double p) {
    progress_ = p;
    emit progressChanged();
}

void ModelDownloader::start() {
    if (isReady()) { setStatus(QStringLiteral("ready")); return; }
    if (reply_) return; // already downloading

    const QString partPath = modelPath() + ".part";
    file_ = new QFile(partPath);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file_; file_ = nullptr;
        setStatus(QStringLiteral("failed"));
        return;
    }

    setProgress(0.0);
    setStatus(QStringLiteral("downloading"));

    QNetworkRequest req{QUrl(QString::fromLatin1(kDownloadUrl))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = nam_->get(req);

    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        if (total > 0) setProgress(double(received) / double(total));
    });
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        const bool ok = reply_->error() == QNetworkReply::NoError;
        if (file_) {
            file_->write(reply_->readAll());
            file_->close();
        }
        const QString partPath = modelPath() + ".part";
        reply_->deleteLater();
        reply_ = nullptr;
        delete file_; file_ = nullptr;

        if (ok) {
            QFile::remove(modelPath());
            if (QFile::rename(partPath, modelPath())) {
                setProgress(1.0);
                setStatus(QStringLiteral("ready"));
            } else {
                setStatus(QStringLiteral("failed"));
            }
        } else {
            QFile::remove(partPath);
            setStatus(QStringLiteral("failed"));
        }
    });
}

void ModelDownloader::cancel() {
    if (reply_) reply_->abort();
    if (file_) { file_->close(); delete file_; file_ = nullptr; }
    QFile::remove(modelPath() + ".part");
    setStatus(isReady() ? QStringLiteral("ready") : QStringLiteral("absent"));
}

void ModelDownloader::remove() {
    cancel();
    QFile::remove(modelPath());
    setProgress(0.0);
    setStatus(QStringLiteral("absent"));
}
