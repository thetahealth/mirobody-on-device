#pragma once

// The chat transcript as a list model for the QML ListView. Holds the same
// running conversation the web client keeps in state.messages: alternating user
// and assistant turns, each assistant turn optionally carrying streamed
// "thinking" text, a provider label, and a cost/usage map. The controller
// appends turns and mutates the in-flight assistant turn as SSE chunks arrive.

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QString>

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
        ThinkingRole
    };

    explicit ChatModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return messages_.size(); }

    // --- mutation API used by AppController -------------------------------
    int  appendMessage(const QString& role, const QString& content, qint64 ts);
    void setContent(int row, const QString& content);
    void appendThinking(int row, const QString& text);
    void setProvider(int row, const QString& provider);
    void setCost(int row, const QVariantMap& cost);
    void removeAt(int row);
    void clear();

    // Round-trip with the local-history JSON (mirrors db.js snapshot/restore).
    QVariantList snapshot() const;                 // [{role, content, ts, cost, provider}]
    void         loadSnapshot(const QVariantList& items);

signals:
    void countChanged();

private:
    struct Message {
        QString     role;
        QString     content;
        qint64      ts = 0;
        QString     provider;
        QVariantMap cost;
        QString     thinking;
    };
    QVector<Message> messages_;

    void emitChanged(int row, const QVector<int>& roles);
};
