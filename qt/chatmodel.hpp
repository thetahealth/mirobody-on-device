#pragma once

// The chat transcript as a list model for the QML ListView. Holds the same
// running conversation the web client keeps in state.messages: alternating user
// and assistant turns, each assistant turn optionally carrying streamed
// "thinking" text, a provider label, and a cost/usage map. The controller
// appends turns and mutates the in-flight assistant turn as SSE chunks arrive.

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class ChatModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role {
        RoleRole = Qt::UserRole + 1,   // "user" | "assistant"
        ContentRole,
        TimestampRole,                 // epoch ms (qint64)
        ProviderRole,
        CostRole,                      // QVariantMap, empty when absent
        ThinkingRole,
        LocalRole,                     // answered by the client (a slash command)
        ImagesRole,                    // QStringList of URLs the backend served
        ChartsRole,                    // QStringList of ECharts `option` JSON texts
        ToolsRole                      // QVariantList of tool-call cards, see toolEvent
    };

    explicit ChatModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return messages_.size(); }

    // --- mutation API used by AppController -------------------------------
    // `local` marks a row the client produced by itself (a slash command's answer,
    // e.g. the /help guide). Such a row is shown like any other but is invisible to
    // both snapshot() and context(), so it is never persisted and never costs a
    // token on the next turn.
    int  appendMessage(const QString& role, const QString& content, qint64 ts,
                       bool local = false);
    void setContent(int row, const QString& content);
    void appendThinking(int row, const QString& text);
    void setProvider(int row, const QString& provider);
    void setCost(int row, const QVariantMap& cost);
    void addImage(int row, const QString& url);
    void addChart(int row, const QString& optionJson);

    /**
     * One `queryTitle` / `queryArguments` / `queryDetail` event for a tool call.
     *
     * `phase` is "title" | "arguments" | "detail"; title and arguments may arrive in
     * pieces and accumulate, and detail (the result) ends the call.
     *
     * Cards are keyed INDIRECTLY, as in the web client: some providers reuse or omit
     * tool ids across calls, and a title landing on a finished card would append to
     * its name ("Called list_filesread_file"). A title for a finished (or unknown) id
     * starts a fresh card instead.
     */
    void toolEvent(int row, const QString& phase, const QString& toolId,
                   const QString& content);
    /** The stream ended with calls still running: stop their spinners. */
    void settleTools(int row);

    void removeAt(int row);
    void clear();

    // Round-trip with the local-history JSON (mirrors db.js snapshot/restore).
    // [{role, content, ts, cost, provider}]. Locally answered rows are omitted by
    // default -- they are not conversation and must never reach disk. The one
    // caller that passes true is the incognito stash, which is memory holding the
    // view aside and back, so what the user is looking at should survive it.
    QVariantList snapshot(bool includeLocal = false) const;
    void         loadSnapshot(const QVariantList& items);

    // The turn context for the row about to be answered: rows [0, rowExclusive) as
    // [{role, content}], locally answered ones dropped. Used by both the proxy-mode
    // request body and the on-device engine, which must agree on what the model
    // has seen.
    QVariantList context(int rowExclusive) const;

signals:
    void countChanged();

private:
    /** One tool invocation, as the reply's status block shows it. */
    struct Tool {
        QString name;       // the tool's name; accumulates across `title` chunks
        QString args;       // request JSON; accumulates across `arguments` chunks
        QString result;     // the tool's return, once it has one
        bool    done = false;
    };

    struct Message {
        QString     role;
        QString     content;
        qint64      ts = 0;
        QString     provider;
        QVariantMap cost;
        QString     thinking;
        bool        local = false;
        QStringList images;
        QStringList charts;
        QVector<Tool> tools;
        // Provider tool_id -> the index in `tools` it is currently writing to. Not
        // persisted: it means nothing once the turn has settled.
        QHash<QString, int> toolIndex;
    };
    QVector<Message> messages_;

    static QVariantList toolList(const Message& m);
    void emitChanged(int row, const QVector<int>& roles);
};
