import Foundation

/// UI-side chat models — mirror the data classes in `ui/chat/ChatViewModel.kt`.

enum Role {
    case user
    case assistant
}

/// A single tool invocation surfaced via `queryTitle` / `queryArguments` /
/// `queryDetail` SSE events; the three share `id` (the backend `tool_id`).
struct ToolCall: Identifiable, Equatable {
    let id: String
    var title: String = ""
    var argumentsJson: String = ""
    var resultJson: String = ""
    var resultReceived: Bool = false
}

struct ChatMessage: Identifiable, Equatable {
    let id: String
    let role: Role
    var text: String = ""
    var thinking: String = ""
    var toolCalls: [ToolCall] = []
    var imageUrls: [String] = []
    /// Each entry is an Apache ECharts `option` (JSON object, stringified) rendered as a chart.
    var chartOptions: [String] = []
    var streaming: Bool = false
    var error: String? = nil
    var costStats: CostStatistics? = nil
    /// Display names of files attached to this (user) turn, shown as chips in the bubble.
    var attachmentNames: [String] = []
}
