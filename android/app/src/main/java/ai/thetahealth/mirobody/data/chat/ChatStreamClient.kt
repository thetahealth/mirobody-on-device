package ai.thetahealth.mirobody.data.chat

import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamRequest
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import ai.thetahealth.mirobody.data.chat.dto.CostStatistics
import ai.thetahealth.mirobody.data.chat.dto.RawSseChunk
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MediaType.Companion.toMediaTypeOrNull
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.logging.HttpLoggingInterceptor
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources

/**
 * POST /api/chat as Server-Sent Events. Each SSE `data:` line is a JSON chunk
 * — see [RawSseChunk] / [ChatStreamEvent].
 */
class ChatStreamClient(
    client: OkHttpClient,
    private val json: Json,
) {
    // SSE callers must not have a BODY-level HttpLoggingInterceptor in the chain: it
    // calls source.request(Long.MAX_VALUE) on the response, draining the entire body
    // before the call returns and stalling event delivery until the server closes the
    // connection. Strip any logging interceptors for the streaming client; non-stream
    // requests still log via the original shared client.
    private val streamingClient: OkHttpClient = client.newBuilder()
        .apply {
            val kept = interceptors().filterNot { it is HttpLoggingInterceptor }
            interceptors().clear()
            interceptors().addAll(kept)
        }
        .build()
    private val sseFactory = EventSources.createFactory(streamingClient)

    fun stream(
        request: ChatStreamRequest,
        attachments: List<ChatAttachment> = emptyList(),
    ): Flow<ChatStreamEvent> = callbackFlow {
        // With attachments, post a multipart form so the files ride along (the server
        // stores them and references them on the turn); otherwise the lighter JSON body.
        // The server reads the same field names from either form (src/chat/params.cpp).
        val body: RequestBody = if (attachments.isEmpty()) {
            json.encodeToString(ChatStreamRequest.serializer(), request)
                .toRequestBody("application/json".toMediaType())
        } else {
            MultipartBody.Builder().setType(MultipartBody.FORM).apply {
                addFormDataPart("agent", request.agent)
                addFormDataPart("provider", request.provider)
                addFormDataPart("question", request.question)
                addFormDataPart("language", request.language)
                addFormDataPart("session_id", request.sessionId)
                request.subject?.takeIf { it.isNotBlank() }?.let { addFormDataPart("subject", it) }
                for (att in attachments) {
                    addFormDataPart(
                        "file",
                        att.fileName,
                        att.bytes.toRequestBody(att.mimeType.toMediaTypeOrNull()),
                    )
                }
            }.build()
        }
        val httpReq = Request.Builder()
            .url("http://placeholder.invalid/api/chat")
            .post(body)
            .header("Accept", "text/event-stream")
            .header("Cache-Control", "no-cache")
            .build()

        val listener = object : EventSourceListener() {
            override fun onOpen(eventSource: EventSource, response: Response) = Unit

            override fun onEvent(
                eventSource: EventSource,
                id: String?,
                type: String?,
                data: String,
            ) {
                val event = parseChunk(data)
                trySend(event)
                if (event is ChatStreamEvent.End) {
                    eventSource.cancel()
                    close()
                }
            }

            override fun onClosed(eventSource: EventSource) {
                close()
            }

            override fun onFailure(
                eventSource: EventSource,
                t: Throwable?,
                response: Response?,
            ) {
                val msg = t?.message
                    ?: response?.let { "HTTP ${it.code}" }
                    ?: "stream failed"
                trySend(ChatStreamEvent.Error(msg))
                close()
            }
        }

        val source = sseFactory.newEventSource(httpReq, listener)
        awaitClose { source.cancel() }
    }

    private fun parseChunk(data: String): ChatStreamEvent = try {
        val chunk = json.decodeFromString(RawSseChunk.serializer(), data)
        when (chunk.type) {
            "id" -> ChatStreamEvent.Id(chunk.replyId.orEmpty())
            "thinking" -> ChatStreamEvent.Thinking(chunk.content.asString())
            "reply" -> ChatStreamEvent.Reply(chunk.content.asString())
            "queryTitle" -> chunk.toolId
                ?.takeIf { it.isNotBlank() }
                ?.let { ChatStreamEvent.QueryTitle(it, chunk.content.asString()) }
                ?: ChatStreamEvent.Unknown(chunk.type, data)
            "queryArguments" -> chunk.toolId
                ?.takeIf { it.isNotBlank() }
                ?.let { ChatStreamEvent.QueryArguments(it, chunk.content.asString()) }
                ?: ChatStreamEvent.Unknown(chunk.type, data)
            "queryDetail" -> chunk.toolId
                ?.takeIf { it.isNotBlank() }
                ?.let { ChatStreamEvent.QueryDetail(it, chunk.content.asString()) }
                ?: ChatStreamEvent.Unknown(chunk.type, data)
            "image" -> {
                val url = chunk.content.asString()
                if (url.isBlank()) ChatStreamEvent.Unknown(chunk.type, data)
                else ChatStreamEvent.Image(url)
            }
            "chart" -> {
                // The option is a nested JSON object; re-stringify it for echarts.setOption().
                val option = chunk.chart
                if (option == null || option is JsonNull) ChatStreamEvent.Unknown(chunk.type, data)
                else ChatStreamEvent.Chart(option.toString())
            }
            "conversation" -> {
                val id = chunk.content.asString().ifBlank { chunk.conversationId.orEmpty() }
                if (id.isBlank()) ChatStreamEvent.Unknown(chunk.type, data)
                else ChatStreamEvent.Conversation(id)
            }
            "heartbeat" -> ChatStreamEvent.Heartbeat
            "error" -> ChatStreamEvent.Error(chunk.content.asString())
            "end" -> ChatStreamEvent.End
            "costStatistics" -> {
                val node = chunk.cost
                if (node != null && node !is JsonNull) {
                    // If decode throws, the outer try/catch turns it into Unknown("parse_error", ...).
                    ChatStreamEvent.Stats(json.decodeFromJsonElement(CostStatistics.serializer(), node))
                } else {
                    ChatStreamEvent.Unknown(chunk.type, data)
                }
            }
            else -> ChatStreamEvent.Unknown(chunk.type, data)
        }
    } catch (t: Throwable) {
        ChatStreamEvent.Unknown(rawType = "parse_error", rawData = data)
    }

    /** Backend emits `content` as a string for reply/thinking/error; tolerate either form. */
    private fun JsonElement?.asString(): String = when (this) {
        null -> ""
        is JsonPrimitive -> contentOrNull.orEmpty()
        else -> toString()
    }
}