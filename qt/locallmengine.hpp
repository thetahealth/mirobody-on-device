#pragma once

// On-device LLM (any GGUF via llama.cpp) for the Qt client. Emits the same
// reply/finished/failed events the SSE path produces, so AppController drives
// ChatModel identically for local turns. The Qt analogue of Android's LlamaCppEngine.
//
// One engine, one format. Android carries a second runtime (LiteRT-LM) because Google
// publishes `.litertlm` builds that are faster on a phone; on the desktop llama.cpp
// reads every model the catalog offers, so a second lane would buy nothing.
//
// The real engine links llama.cpp's shared library. It is compiled ONLY when configured
// with -DMIROBODY_ONDEVICE_LLM=ON and pointed at an SDK via -DLLAMA_CPP_DIR (include/ +
// lib/ + bin/); build-qt builds and caches one per backend (see qt/README.md).
// Otherwise a stub is built and the app still compiles/links — on-device turns then
// report "not built in", mirroring the graceful degradation used elsewhere in this repo
// (e.g. the absent JNI .so).

#include <QObject>
#include <QString>
#include <QVariantList>
#include <atomic>
#include <memory>

class LocalLmEngine : public QObject {
    Q_OBJECT
public:
    explicit LocalLmEngine(QObject* parent = nullptr);
    ~LocalLmEngine() override;

    /// True when built with the LiteRT-LM SDK (MIROBODY_ONDEVICE_LLM).
    static bool isAvailable();

    /// Stream a reply. `history` is a list of {role,content} maps (role "user"/
    /// "assistant"); the final user entry is the new question, the rest is context.
    /// Runs off the GUI thread; signals are delivered to the GUI thread via Qt's
    /// queued connections.
    void generate(const QString& modelPath, const QVariantList& history);
    void cancel();

signals:
    void replyChunk(const QString& delta);
    void finished();
    void failed(const QString& reason);

private:
    std::atomic<bool> cancelled_{false};

    struct Impl;                 // holds the LiteRT-LM engine; defined only in the real build
    std::unique_ptr<Impl> impl_;
};
