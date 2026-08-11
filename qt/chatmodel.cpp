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
    case LocalRole:     return m.local;
    case ImagesRole:    return m.images;
    case ChartsRole:    return m.charts;
    case ToolsRole:     return toolList(m);
    default:            return {};
    }
}

QVariantList ChatModel::toolList(const Message& m) {
    QVariantList out;
    out.reserve(m.tools.size());
    for (const Tool& t : m.tools) {
        out.push_back(QVariantMap{
            {QStringLiteral("name"),   t.name},
            {QStringLiteral("args"),   t.args},
            {QStringLiteral("result"), t.result},
            {QStringLiteral("done"),   t.done},
        });
    }
    return out;
}

QHash<int, QByteArray> ChatModel::roleNames() const {
    return {
        {RoleRole,      "role"},
        {ContentRole,   "content"},
        {TimestampRole, "ts"},
        {ProviderRole,  "provider"},
        {CostRole,      "cost"},
        {ThinkingRole,  "thinking"},
        {LocalRole,     "local"},
        {ImagesRole,    "images"},
        {ChartsRole,    "charts"},
        {ToolsRole,     "tools"},
    };
}

void ChatModel::emitChanged(int row, const QVector<int>& roles) {
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, roles);
}

int ChatModel::appendMessage(const QString& role, const QString& content, qint64 ts, bool local) {
    const int row = messages_.size();
    beginInsertRows(QModelIndex(), row, row);
    Message m;
    m.role = role;
    m.content = content;
    m.ts = ts;
    m.local = local;
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

void ChatModel::addImage(int row, const QString& url) {
    if (row < 0 || row >= messages_.size() || url.isEmpty()) return;
    messages_[row].images.push_back(url);
    emitChanged(row, {ImagesRole});
}

void ChatModel::addChart(int row, const QString& optionJson) {
    if (row < 0 || row >= messages_.size() || optionJson.isEmpty()) return;
    messages_[row].charts.push_back(optionJson);
    emitChanged(row, {ChartsRole});
}

void ChatModel::toolEvent(int row, const QString& phase, const QString& toolId,
                          const QString& content) {
    if (row < 0 || row >= messages_.size()) return;
    Message& m = messages_[row];

    const QString id = toolId.isEmpty() ? QStringLiteral("?") : toolId;
    int at = m.toolIndex.value(id, -1);
    const bool startsNew = at < 0 || at >= m.tools.size()
                           || (phase == QLatin1String("title") && m.tools.at(at).done);
    if (startsNew) {
        at = m.tools.size();
        m.tools.push_back(Tool{});
        m.toolIndex.insert(id, at);
    }

    Tool& tool = m.tools[at];
    if (phase == QLatin1String("title")) {
        tool.name += content;
    } else if (phase == QLatin1String("arguments")) {
        tool.args += content;
    } else {
        // detail: the result -- the call is over.
        tool.result = content;
        tool.done   = true;
    }
    emitChanged(row, {ToolsRole});
}

void ChatModel::settleTools(int row) {
    if (row < 0 || row >= messages_.size()) return;
    Message& m = messages_[row];
    bool changed = false;
    for (Tool& t : m.tools) {
        if (!t.done) { t.done = true; changed = true; }
    }
    if (changed) emitChanged(row, {ToolsRole});
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

QVariantList ChatModel::snapshot(bool includeLocal) const {
    QVariantList out;
    out.reserve(messages_.size());
    for (const Message& m : messages_) {
        if (m.local && !includeLocal) continue;   // a command's answer is not conversation
        QVariantMap o;
        o.insert("role", m.role);
        o.insert("content", m.content);
        o.insert("ts", m.ts);
        if (!m.cost.isEmpty()) o.insert("cost", m.cost);
        if (!m.provider.isEmpty()) o.insert("provider", m.provider);
        if (m.local) o.insert("local", true);
        // The visual half of a settled turn. Charts and images are part of the answer
        // -- a reply that drew a trend says nothing without it -- so a reopened
        // conversation shows them rather than a paragraph referring to a picture that
        // is gone. Tool cards ride along for the same reason the web client rebuilds
        // its footer from what was persisted: the reply is not only its prose.
        if (!m.images.isEmpty()) o.insert("images", m.images);
        if (!m.charts.isEmpty()) o.insert("charts", m.charts);
        if (!m.tools.isEmpty())  o.insert("tools", toolList(m));
        out.push_back(o);
    }
    return out;
}

QVariantList ChatModel::context(int rowExclusive) const {
    QVariantList out;
    const int end = qMin(rowExclusive, static_cast<int>(messages_.size()));
    for (int i = 0; i < end; ++i) {
        const Message& m = messages_.at(i);
        if (m.local) continue;
        out.push_back(QVariantMap{
            {QStringLiteral("role"),    m.role},
            {QStringLiteral("content"), m.content},
        });
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
        m.local    = o.value("local").toBool();
        m.images   = o.value("images").toStringList();
        m.charts   = o.value("charts").toStringList();
        for (const QVariant& t : o.value("tools").toList()) {
            const QVariantMap tm = t.toMap();
            Tool tool;
            tool.name   = tm.value("name").toString();
            tool.args   = tm.value("args").toString();
            tool.result = tm.value("result").toString();
            // A restored call is over by definition: nothing is still running in a
            // conversation that was written to disk.
            tool.done   = true;
            m.tools.push_back(tool);
        }
        if (m.role.isEmpty()) continue;
        messages_.push_back(m);
    }
    endResetModel();
    emit countChanged();
}
