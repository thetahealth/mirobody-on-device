#include "chatmodel.hpp"

ChatModel::ChatModel(QObject* parent) : QAbstractListModel(parent) {}

int ChatModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return messages_.size();
}

QVariant ChatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= messages_.size())
        return {};
    const Message& m = messages_.at(index.row());
    switch (role) {
    case RoleRole:      return m.role;
    case ContentRole:   return m.content;
    case TimestampRole: return m.ts;
    case ProviderRole:  return m.provider;
    case CostRole:      return m.cost;
    case ThinkingRole:  return m.thinking;
    default:            return {};
    }
}

QHash<int, QByteArray> ChatModel::roleNames() const {
    return {
        {RoleRole,      "role"},
        {ContentRole,   "content"},
        {TimestampRole, "ts"},
        {ProviderRole,  "provider"},
        {CostRole,      "cost"},
        {ThinkingRole,  "thinking"},
    };
}

void ChatModel::emitChanged(int row, const QVector<int>& roles) {
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, roles);
}

int ChatModel::appendMessage(const QString& role, const QString& content, qint64 ts) {
    const int row = messages_.size();
    beginInsertRows(QModelIndex(), row, row);
    Message m;
    m.role = role;
    m.content = content;
    m.ts = ts;
    messages_.push_back(m);
    endInsertRows();
    emit countChanged();
    return row;
}

void ChatModel::setContent(int row, const QString& content) {
    if (row < 0 || row >= messages_.size()) return;
    messages_[row].content = content;
    emitChanged(row, {ContentRole});
}

void ChatModel::appendThinking(int row, const QString& text) {
    if (row < 0 || row >= messages_.size()) return;
    messages_[row].thinking += text;
    emitChanged(row, {ThinkingRole});
}

void ChatModel::setProvider(int row, const QString& provider) {
    if (row < 0 || row >= messages_.size()) return;
    messages_[row].provider = provider;
    emitChanged(row, {ProviderRole});
}

void ChatModel::setCost(int row, const QVariantMap& cost) {
    if (row < 0 || row >= messages_.size()) return;
    messages_[row].cost = cost;
    emitChanged(row, {CostRole});
}

void ChatModel::removeAt(int row) {
    if (row < 0 || row >= messages_.size()) return;
    beginRemoveRows(QModelIndex(), row, row);
    messages_.removeAt(row);
    endRemoveRows();
    emit countChanged();
}

void ChatModel::clear() {
    if (messages_.isEmpty()) return;
    beginResetModel();
    messages_.clear();
    endResetModel();
    emit countChanged();
}

QVariantList ChatModel::snapshot() const {
    QVariantList out;
    out.reserve(messages_.size());
    for (const Message& m : messages_) {
        QVariantMap o;
        o.insert("role", m.role);
        o.insert("content", m.content);
        o.insert("ts", m.ts);
        if (!m.cost.isEmpty()) o.insert("cost", m.cost);
        if (!m.provider.isEmpty()) o.insert("provider", m.provider);
        out.push_back(o);
    }
    return out;
}

void ChatModel::loadSnapshot(const QVariantList& items) {
    beginResetModel();
    messages_.clear();
    for (const QVariant& v : items) {
        const QVariantMap o = v.toMap();
        Message m;
        m.role     = o.value("role").toString();
        m.content  = o.value("content").toString();
        m.ts       = o.value("ts").toLongLong();
        m.provider = o.value("provider").toString();
        m.cost     = o.value("cost").toMap();
        if (m.role.isEmpty()) continue;
        messages_.push_back(m);
    }
    endResetModel();
    emit countChanged();
}
