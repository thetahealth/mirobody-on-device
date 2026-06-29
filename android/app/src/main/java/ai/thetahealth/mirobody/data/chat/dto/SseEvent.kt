package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

/**
 * Wire shape of every `data: {...}` SSE chunk emitted by `/api/chat`.
 *
 * `content` is typed as [JsonElement] because the field is polymorphic on the wire:
 *  - reply / thinking / error: string
 *  - queryTitle: string (tool name)
 *  - queryArguments / queryDetail: string (JSON-stringified args / tool result)
 *  - heartbeat / end: empty string
 *
 * `tool_id` is set on queryTitle / queryArguments / queryDetail and links the three
 * events of a single tool invocation together.
 *
 * The `costStatistics` event carries its payload in a sibling `cost` object
 * ({model, input_tokens, output_tokens, ...}), NOT in `content` (see CostEvent in
 * src/chat/event/event.cpp; the web client reads `json.cost`).
 */
@Serializable
data class RawSseChunk(
    val type: String = "",
    val content: JsonElement? = null,
    @SerialName("reply_id") val replyId: String? = null,
    @SerialName("tool_id") val toolId: String? = null,
    // Set only on `chart` events: the Apache ECharts `option`, a nested JSON object.
    val chart: JsonElement? = null,
    // Set only on `costStatistics` events: the per-turn LLM accounting object.
    val cost: JsonElement? = null,
    // Set on `conversation` events as a decimal-string fallback when `content` is absent
    // (the client carries large thread ids as strings to avoid JS integer-precision loss).
    @SerialName("conversation_id") val conversationId: String? = null,
)

/** Per-turn LLM accounting emitted by every provider client on stream completion. */
@Serializable
data class CostStatistics(
    val model: String = "",
    @SerialName("input_tokens") val inputTokens: Int = 0,
    @SerialName("output_tokens") val outputTokens: Int = 0,
    @SerialName("thought_tokens") val thoughtTokens: Int = 0,
    @SerialName("total_tokens") val totalTokens: Int = 0,
    @SerialName("total_cost") val totalCost: Double = 0.0,
)

/** High-level event surfaced to the chat repository / UI. */
sealed interface ChatStreamEvent {
    data class Id(val replyId: String) : ChatStreamEvent
    data class Thinking(val delta: String) : ChatStreamEvent
    data class Reply(val delta: String) : ChatStreamEvent
    /** Backend announces a new tool invocation. `title` is the tool name. */
    data class QueryTitle(val toolId: String, val title: String) : ChatStreamEvent
    /** Args for a previously announced tool, JSON-stringified. */
    data class QueryArguments(val toolId: String, val argumentsJson: String) : ChatStreamEvent
    /** Tool execution result (string or JSON), arrives after the tool returns. */
    data class QueryDetail(val toolId: String, val detailJson: String) : ChatStreamEvent
    /** Backend-emitted image URL (e.g. a chart rendered by an agent). */
    data class Image(val url: String) : ChatStreamEvent
    /** Backend-emitted Apache ECharts `option` (JSON object, stringified) to render as a chart. */
    data class Chart(val optionJson: String) : ChatStreamEvent
    /** The durable server-side conversation (thread) id for this turn, as a decimal string. */
    data class Conversation(val id: String) : ChatStreamEvent
    data object Heartbeat : ChatStreamEvent
    data class Error(val message: String) : ChatStreamEvent
    data object End : ChatStreamEvent
    data class Stats(val stats: CostStatistics) : ChatStreamEvent
    data class Unknown(val rawType: String, val rawData: String) : ChatStreamEvent
}