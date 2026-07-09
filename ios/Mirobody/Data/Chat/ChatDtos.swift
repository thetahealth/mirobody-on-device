import Foundation

// MARK: - Request DTOs (mirror data/chat/dto/ChatStreamRequest.kt)

struct ChatStreamRequest: Encodable {
    var question: String
    var sessionId: String
    var agent: String
    var provider: String = ""
    var enableMcp: Int = 1
    var fileList: [FileRef] = []
    var promptName: String = ""
    var language: String = "en"
    var timezone: String = "Asia/Shanghai"
    var scene: String? = nil
    // Opaque care-circle member handle for the "currently for" subject (whose health
    // the AI's family_health tool should default to). Nil/omitted = self.
    var subject: String? = nil
    // Privacy mode: when true the server persists nothing for this turn and disables
    // memory (src/chat/params.cpp). false is equivalent to the server default.
    var incognito: Bool = false

    enum CodingKeys: String, CodingKey {
        case question
        case sessionId = "session_id"
        case agent
        case provider
        case enableMcp = "enable_mcp"
        case fileList = "file_list"
        case promptName = "prompt_name"
        case language
        case timezone
        case scene
        case subject
        case incognito
    }
}

struct FileRef: Encodable {
    var fileKey: String = ""
    var fileType: String = ""
    var fileName: String = ""
    var fileUrl: String = ""
    var fileSize: Int = 0
    var duration: Int = 0
    var storageKey: String = ""
    var url: String = ""

    enum CodingKeys: String, CodingKey {
        case fileKey = "file_key"
        case fileType = "file_type"
        case fileName = "file_name"
        case fileUrl = "file_url"
        case fileSize = "file_size"
        case duration
        case storageKey = "storage_key"
        case url
    }
}

/// A file the user attached to a chat turn, read into memory at pick time. Sent as
/// a multipart `file` part to /api/chat (mirrors the web client's FormData upload);
/// the server stores it and references it on the turn. Mirrors `ChatAttachment.kt`.
struct ChatAttachment: Identifiable {
    let id = UUID()
    let fileName: String
    let mimeType: String
    let data: Data
}

// MARK: - History (mirror data/chat/dto/HistoryDto.kt)

struct HistoryResponse: Decodable {
    let summaries: [SessionSummary]

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        summaries = try c.decodeIfPresent([SessionSummary].self, forKey: .summaries) ?? []
    }
    enum CodingKeys: String, CodingKey { case summaries }
}

struct SessionSummary: Decodable, Identifiable {
    let sessionId: String
    /// Epoch milliseconds (UTC); 0 when absent.
    let timestamp: Int64
    let summary: String
    let queryUserId: String
    let owned: Bool
    let sharedWithCount: Int
    /// Owner email on a thread shared TO me (drives the "Shared by" badge); "" otherwise.
    let sharedBy: String

    var id: String { sessionId }

    enum CodingKeys: String, CodingKey {
        case sessionId = "session_id"
        case timestamp
        case summary
        case queryUserId = "query_user_id"
        case owned
        case sharedWithCount = "shared_with_count"
        case sharedBy = "shared_by"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        sessionId = try c.decodeIfPresent(String.self, forKey: .sessionId) ?? ""
        timestamp = try c.decodeIfPresent(Int64.self, forKey: .timestamp) ?? 0
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        queryUserId = try c.decodeIfPresent(String.self, forKey: .queryUserId) ?? ""
        owned = try c.decodeIfPresent(Bool.self, forKey: .owned) ?? true
        sharedWithCount = try c.decodeIfPresent(Int.self, forKey: .sharedWithCount) ?? 0
        sharedBy = try c.decodeIfPresent(String.self, forKey: .sharedBy) ?? ""
    }
}

// MARK: - Conversation detail (mirror data/chat/dto/ConversationDetail)

/// `GET /api/conversation?id=` — the full thread for one saved conversation.
struct ConversationDetail: Decodable {
    let id: String
    let summary: String
    let owned: Bool
    let sharedBy: String
    let messages: [ConversationMessage]

    enum CodingKeys: String, CodingKey {
        case id, summary, owned, messages
        case sharedBy = "shared_by"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        // `id` may arrive as a (large) number or a string; accept either.
        if let s = try? c.decode(String.self, forKey: .id) {
            id = s
        } else if let n = try? c.decode(Int64.self, forKey: .id) {
            id = String(n)
        } else {
            id = ""
        }
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        owned = try c.decodeIfPresent(Bool.self, forKey: .owned) ?? true
        sharedBy = try c.decodeIfPresent(String.self, forKey: .sharedBy) ?? ""
        messages = try c.decodeIfPresent([ConversationMessage].self, forKey: .messages) ?? []
    }
}

struct ConversationMessage: Decodable {
    let role: String
    let content: String
    let agent: String
    let provider: String
    let createdAt: Int64

    enum CodingKeys: String, CodingKey {
        case role, content, agent, provider
        case createdAt = "created_at"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        role = try c.decodeIfPresent(String.self, forKey: .role) ?? ""
        content = try c.decodeIfPresent(String.self, forKey: .content) ?? ""
        agent = try c.decodeIfPresent(String.self, forKey: .agent) ?? ""
        provider = try c.decodeIfPresent(String.self, forKey: .provider) ?? ""
        createdAt = try c.decodeIfPresent(Int64.self, forKey: .createdAt) ?? 0
    }
}

struct HistoryDeleteRequest: Encodable {
    let sessionId: String
    enum CodingKeys: String, CodingKey { case sessionId = "session_id" }
}

// MARK: - Providers (mirror data/chat/dto/ProviderInfo.kt)

/// One agent group from `GET /api/providers` (`[{ agent, providers[] }]`). The
/// default agent's group carries an empty `agent`.
struct ProviderGroup: Decodable {
    let agent: String
    let providers: [String]

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        agent = try c.decodeIfPresent(String.self, forKey: .agent) ?? ""
        providers = try c.decodeIfPresent([String].self, forKey: .providers) ?? []
    }
    enum CodingKeys: String, CodingKey { case agent, providers }
}

/// A single selectable provider, flattened from a `ProviderGroup`: `agent` (empty
/// for the default agent) + one `provider` (model). Submitted verbatim as the chat
/// request's `agent` / `provider` — no string parsing.
struct ProviderInfo: Hashable, Identifiable {
    let agent: String
    let provider: String

    var id: String { key }

    /// Stable key for persisting / restoring the selection.
    var key: String { agent.isEmpty ? provider : "\(agent)/\(provider)" }

    /// Picker display: the model name (or the on-device label).
    var label: String { isOnDevice ? ProviderInfo.onDeviceName : provider }

    // MARK: On-device (private) provider — client-only, no server round-trip.

    /// Sentinel `provider` marking the synthetic on-device provider. When selected,
    /// chat routes to `OnDeviceLlmEngine` instead of the SSE stream. Mirrors Android.
    static let onDeviceCode = "__ondevice_gemma4__"
    /// Stable display name (kept non-localized so the persisted selection survives a
    /// UI-language change).
    static let onDeviceName = "Gemma 4 · On-device"
    static let onDevice = ProviderInfo(agent: "", provider: onDeviceCode)

    var isOnDevice: Bool { provider == ProviderInfo.onDeviceCode }
}

// MARK: - SSE chunk + cost stats (mirror data/chat/dto/SseEvent.kt)

struct RawSseChunk: Decodable {
    let type: String
    let content: JSONValue?
    let replyId: String?
    let toolId: String?
    /// Set only on `chart` events: the Apache ECharts `option`, a nested JSON object.
    let chart: JSONValue?
    /// Set on `conversation` events as a decimal-string fallback when `content` is
    /// absent (large thread ids are carried as strings to avoid precision loss).
    let conversationId: String?

    enum CodingKeys: String, CodingKey {
        case type
        case content
        case replyId = "reply_id"
        case toolId = "tool_id"
        case chart
        case conversationId = "conversation_id"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        type = try c.decodeIfPresent(String.self, forKey: .type) ?? ""
        content = try c.decodeIfPresent(JSONValue.self, forKey: .content)
        replyId = try c.decodeIfPresent(String.self, forKey: .replyId)
        toolId = try c.decodeIfPresent(String.self, forKey: .toolId)
        chart = try c.decodeIfPresent(JSONValue.self, forKey: .chart)
        conversationId = try c.decodeIfPresent(String.self, forKey: .conversationId)
    }
}

struct CostStatistics: Decodable, Equatable {
    let model: String
    let inputTokens: Int
    let outputTokens: Int
    let thoughtTokens: Int
    let totalTokens: Int
    let totalCost: Double

    enum CodingKeys: String, CodingKey {
        case model
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
        case thoughtTokens = "thought_tokens"
        case totalTokens = "total_tokens"
        case totalCost = "total_cost"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        // Tokens may arrive as Int or (after a JSONValue round-trip) Double — accept both.
        func int(_ k: CodingKeys) -> Int {
            if let i = try? c.decode(Int.self, forKey: k) { return i }
            if let d = try? c.decode(Double.self, forKey: k) { return Int(d) }
            return 0
        }
        model = try c.decodeIfPresent(String.self, forKey: .model) ?? ""
        inputTokens = int(.inputTokens)
        outputTokens = int(.outputTokens)
        thoughtTokens = int(.thoughtTokens)
        totalTokens = int(.totalTokens)
        totalCost = (try? c.decode(Double.self, forKey: .totalCost)) ?? 0
    }
}

/// High-level event surfaced to the chat view model — mirrors the Kotlin
/// `ChatStreamEvent` sealed interface.
enum ChatStreamEvent {
    case id(replyId: String)
    case thinking(delta: String)
    case reply(delta: String)
    case queryTitle(toolId: String, title: String)
    case queryArguments(toolId: String, argumentsJson: String)
    case queryDetail(toolId: String, detailJson: String)
    case image(url: String)
    case chart(optionJson: String)
    /// The durable server-side conversation (thread) id for this turn, as a string.
    case conversation(id: String)
    case heartbeat
    case error(message: String)
    case end
    case stats(CostStatistics)
    case unknown(rawType: String, rawData: String)
}
