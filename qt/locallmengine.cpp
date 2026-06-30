#include "locallmengine.hpp"

#include <QString>

namespace {
// Build the system-instruction context from all turns except the final question,
// and pull out the latest user question. Shared by both build variants.
QString buildSystemInstruction(const QVariantList& history) {
    const QString base =
        QStringLiteral("You are Mirobody's private on-device health assistant. Answer "
                       "concisely. You have no internet or tools; rely only on the conversation.");
    if (history.size() <= 1) return base;
    QString transcript;
    for (int i = 0; i < history.size() - 1; ++i) {
        const QVariantMap m = history.at(i).toMap();
        const QString who = m.value("role").toString() == QLatin1String("user")
            ? QStringLiteral("User: ") : QStringLiteral("Assistant: ");
        transcript += who + m.value("content").toString() + QLatin1Char('\n');
    }
    return base + QStringLiteral("\n\nConversation so far:\n") + transcript;
}

QString latestQuestion(const QVariantList& history) {
    for (int i = history.size() - 1; i >= 0; --i) {
        const QVariantMap m = history.at(i).toMap();
        if (m.value("role").toString() == QLatin1String("user"))
            return m.value("content").toString();
    }
    return QString();
}
} // namespace

#ifdef MIROBODY_ONDEVICE_LLM

// ---------------------------------------------------------------------------
// Real engine — requires the LiteRT-LM C++ SDK. Header paths / symbol names track
// the LiteRT-LM C++ guide (https://developers.google.com/edge/litert-lm/cpp) and
// MUST be confirmed against the SDK you build/link.
// ---------------------------------------------------------------------------
#include <thread>
#include "absl/time/time.h"
#include "runtime/engine/engine.h"
#include "runtime/conversation/conversation.h"

struct LocalLmEngine::Impl {
    std::unique_ptr<litert::lm::Engine> engine;
    std::unique_ptr<litert::lm::Conversation> conversation;
    std::string loadedModelPath;
};

LocalLmEngine::LocalLmEngine(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

LocalLmEngine::~LocalLmEngine() = default;

bool LocalLmEngine::isAvailable() { return true; }

void LocalLmEngine::generate(const QString& modelPath, const QVariantList& history) {
    cancelled_ = false;
    const QString question = latestQuestion(history);
    if (question.isEmpty()) { emit finished(); return; }
    const QString system = buildSystemInstruction(history);
    const bool freshChat = history.size() <= 1;

    std::thread([this, modelPath, question, system, freshChat]() {
        using namespace litert::lm;
        // Lazily build the (expensive) engine, reusing it across turns.
        if (!impl_->engine || impl_->loadedModelPath != modelPath.toStdString()) {
            auto assets = ModelAssets::Create(modelPath.toStdString());
            if (!assets.ok()) { emit failed(QStringLiteral("Failed to open model")); return; }
            auto settings = EngineSettings::CreateDefault(*assets, Backend::CPU);
            auto engine = Engine::CreateEngine(settings);
            if (!engine.ok()) { emit failed(QStringLiteral("Failed to load model")); return; }
            impl_->engine = std::move(*engine);
            impl_->loadedModelPath = modelPath.toStdString();
            impl_->conversation.reset();
        }
        if (freshChat) impl_->conversation.reset();
        if (!impl_->conversation) {
            auto convo = impl_->engine->CreateConversation(/*system_instruction=*/system.toStdString());
            if (!convo.ok()) { emit failed(QStringLiteral("Failed to start conversation")); return; }
            impl_->conversation = std::move(*convo);
        }

        // Stream tokens via the async callback; emit each chunk as a delta. An empty
        // Message signals completion (per the C++ guide).
        impl_->conversation->SendMessageAsync(
            JsonMessage{{"role", "user"}, {"content", question.toStdString()}},
            [this](absl::StatusOr<Message> chunk) {
                if (cancelled_) return;
                if (!chunk.ok()) { emit failed(QStringLiteral("On-device generation failed")); return; }
                const std::string text = chunk->ToString();
                if (text.empty()) { emit finished(); return; }
                emit replyChunk(QString::fromStdString(text));
            });
        impl_->engine->WaitUntilDone(absl::InfiniteDuration());
    }).detach();
}

void LocalLmEngine::cancel() { cancelled_ = true; }

#else // !MIROBODY_ONDEVICE_LLM

// ---------------------------------------------------------------------------
// Stub — keeps the default Qt build green without the LiteRT-LM SDK.
// ---------------------------------------------------------------------------
struct LocalLmEngine::Impl {};

LocalLmEngine::LocalLmEngine(QObject* parent) : QObject(parent) {}
LocalLmEngine::~LocalLmEngine() = default;

bool LocalLmEngine::isAvailable() { return false; }

void LocalLmEngine::generate(const QString& modelPath, const QVariantList& history) {
    Q_UNUSED(modelPath);
    Q_UNUSED(history);
    emit failed(QStringLiteral("On-device model support is not built into this app."));
}

void LocalLmEngine::cancel() { cancelled_ = true; }

#endif
