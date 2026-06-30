#pragma once

// Downloads the on-device model (Gemma 4 E2B, .litertlm, ~2.5 GB) from Hugging Face
// to per-user app data, with progress, and reports where it lives. The Qt analogue
// of Android's ModelManager / iOS's ModelManager. Independent of the chat backend:
// the download goes straight to Hugging Face over its own QNetworkAccessManager.

#include <QObject>
#include <QString>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class ModelDownloader : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)       // absent|downloading|ready|failed
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)  // 0..1
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    QString status() const { return status_; }
    double  progress() const { return progress_; }

    // Absolute path to the model file (whether or not it exists yet).
    QString modelPath() const;
    bool    isReady() const;

    Q_INVOKABLE void start();   // begin (or restart) the download
    Q_INVOKABLE void cancel();  // abort an in-flight download
    Q_INVOKABLE void remove();  // delete the model file to reclaim space

signals:
    void statusChanged();
    void progressChanged();

private:
    void setStatus(const QString& s);
    void setProgress(double p);

    QString modelsDir() const;

    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile*  file_ = nullptr;       // open .part file during download
    QString status_ = QStringLiteral("absent");
    double  progress_ = 0.0;
};
