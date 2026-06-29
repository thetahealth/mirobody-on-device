package ai.thetahealth.mirobody.ui.chat

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ai.thetahealth.mirobody.data.chat.ChatHistoryStore
import ai.thetahealth.mirobody.data.chat.ChatRepository
import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.ChatStreamEvent
import ai.thetahealth.mirobody.data.chat.dto.CostStatistics
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.data.circle.CircleRepository
import ai.thetahealth.mirobody.data.circle.dto.HealthSharer
import ai.thetahealth.mirobody.data.net.ErrorBus
import ai.thetahealth.mirobody.data.settings.SettingsStore
import java.util.UUID
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.Serializable
import kotlinx.serialization.Transient

@Serializable
enum class Role { User, Assistant }

/**
 * A single tool invocation surfaced via `queryTitle` / `queryArguments` / `queryDetail`
 * SSE events. All three events share [id] (the backend `tool_id`).
 */
@Serializable
data class ToolCall(
    val id: String,
    val title: String = "",
    val argumentsJson: String = "",
    val resultJson: String = "",
    val resultReceived: Boolean = false,
)

// @Serializable so the conversation can be persisted locally (see ChatHistoryStore).
// `streaming` is @Transient: it's a live-only flag that's always false once a turn
// has settled and been stored.
@Serializable
data class ChatMessage(
    val id: String,
    val role: Role,
    val text: String = "",
    val thinking: String = "",
    val toolCalls: List<ToolCall> = emptyList(),
    val imageUrls: List<String> = emptyList(),
    // Each entry is an Apache ECharts `option` (JSON object, stringified) rendered as a chart.
    val charts: List<String> = emptyList(),
    @Transient val streaming: Boolean = false,
    val error: String? = null,
    val costStats: CostStatistics? = null,
    // The "Agent/model" provider label this (assistant) turn ran on, shown in the
    // reply footer. Mirrors the web client's per-turn provider label.
    val provider: String = "",
    // Display names of files attached to this (user) turn, shown as chips in the bubble.
    val attachmentNames: List<String> = emptyList(),
)

data class ChatUiState(
    val sessionId: String,
    val messages: List<ChatMessage> = emptyList(),
    val input: String = "",
    val sending: Boolean = false,
    val providers: List<ProviderInfo> = emptyList(),
    val selected: ProviderInfo? = null,
    val error: String? = null,
    val language: String = "en",
    // Care-circle members who shared their health data with me (the "currently
    // for" picker). `subject` is the selected member handle, 0 = me.
    val sharers: List<HealthSharer> = emptyList(),
    val subject: Long = 0,
    // Files staged in the composer for the next turn (cleared on send).
    val attachments: List<ChatAttachment> = emptyList(),
    // The server-side thread id for the current conversation, learned from the
    // `conversation` SSE event. Empty until the first turn lands; powers sharing.
    val conversationId: String = "",
)

class ChatViewModel(
    private val repo: ChatRepository,
    private val circleRepo: CircleRepository,
    private val settings: SettingsStore,
    private val errorBus: ErrorBus,
    private val history: ChatHistoryStore,
) : ViewModel() {

    private val _state = MutableStateFlow(ChatUiState(sessionId = UUID.randomUUID().toString()))
    val state: StateFlow<ChatUiState> = _state.asStateFlow()

    private var streamJob: Job? = null

    init {
        loadProviders()
        loadSharers()
        // Restore the locally-persisted conversation so a relaunch resumes where
        // the user left off (the screen auto-scrolls to the latest message).
        viewModelScope.launch {
            val saved = history.load()
            if (saved.isNotEmpty()) {
                _state.update { if (it.messages.isEmpty()) it.copy(messages = saved) else it }
            }
        }
        viewModelScope.launch {
            settings.language.collect { lang -> _state.update { it.copy(language = lang) } }
        }
    }

    /** Mirror the current conversation to local storage (best-effort, off the UI path). */
    private fun persist() {
        val messages = _state.value.messages
        viewModelScope.launch { history.save(messages) }
    }

    fun loadProviders() {
        viewModelScope.launch {
            runCatching { repo.listProviders() }
                .onSuccess { list ->
                    val savedName = settings.selectedProviderName.first()
                    val restored = list.firstOrNull { it.name == savedName }
                    _state.update {
                        it.copy(
                            providers = list,
                            selected = it.selected ?: restored ?: list.firstOrNull(),
                        )
                    }
                }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update { it.copy(error = t.message) }
                }
        }
    }

    /** Load who has shared their health data with me; powers the subject picker. */
    fun loadSharers() {
        viewModelScope.launch {
            runCatching { circleRepo.healthSharedWithMe() }
                .onSuccess { list ->
                    _state.update {
                        // Drop a stale selection if that sharer is no longer listed.
                        val keep = it.subject != 0L && list.any { s -> s.member == it.subject }
                        it.copy(sharers = list, subject = if (keep) it.subject else 0)
                    }
                }
                .onFailure { _state.update { it.copy(sharers = emptyList(), subject = 0) } }
        }
    }

    fun onSubjectSelected(member: Long) {
        _state.update { it.copy(subject = member) }
    }

    fun addAttachment(attachment: ChatAttachment) {
        _state.update { it.copy(attachments = it.attachments + attachment) }
    }

    fun removeAttachment(index: Int) {
        _state.update {
            if (index !in it.attachments.indices) it
            else it.copy(attachments = it.attachments.filterIndexed { i, _ -> i != index })
        }
    }

    fun onInputChange(value: String) {
        _state.update { it.copy(input = value) }
    }

    fun onProviderSelected(provider: ProviderInfo) {
        _state.update { it.copy(selected = provider) }
        viewModelScope.launch {
            settings.setSelectedProviderName(provider.name)
        }
    }

    fun send() {
        val s = _state.value
        val question = s.input.trim()
        val selected = s.selected
        val attachments = s.attachments
        // Allow an attachment-only turn (no text), matching the web composer.
        if ((question.isEmpty() && attachments.isEmpty()) || s.sending || selected == null) return

        val userMsg = ChatMessage(
            id = "u-${System.currentTimeMillis()}",
            role = Role.User,
            text = question,
            attachmentNames = attachments.map { it.fileName },
        )
        val assistantMsg = ChatMessage(
            id = "a-${System.currentTimeMillis()}",
            role = Role.Assistant,
            streaming = true,
            provider = selected.name,
        )
        _state.update {
            it.copy(
                messages = it.messages + userMsg + assistantMsg,
                input = "",
                sending = true,
                error = null,
                attachments = emptyList(),   // consumed by this turn
            )
        }
        // Save now so the question survives an app kill mid-stream; the in-flight
        // assistant placeholder is filtered out by the store until it has content.
        persist()

        streamJob?.cancel()
        streamJob = viewModelScope.launch {
            runCatching {
                repo.chat(
                    sessionId = s.sessionId,
                    question = question,
                    agentCode = selected.agentCode,
                    provider = selected.code,
                    language = s.language,
                    subject = s.subject,
                    attachments = attachments,
                ).collect { event -> applyEvent(assistantMsg.id, event) }
            }.onFailure { t ->
                errorBus.emit(t)
                updateMessage(assistantMsg.id) { it.copy(streaming = false, error = t.message) }
            }
            _state.update { it.copy(sending = false) }
            persist()   // settled turn (reply / error) is now safe to store
        }
    }

    private fun applyEvent(targetId: String, event: ChatStreamEvent) {
        when (event) {
            is ChatStreamEvent.Reply ->
                updateMessage(targetId) { it.copy(text = it.text + event.delta) }
            is ChatStreamEvent.Thinking ->
                updateMessage(targetId) { it.copy(thinking = it.thinking + event.delta) }
            is ChatStreamEvent.QueryTitle ->
                updateMessage(targetId) {
                    upsertToolCall(it, event.toolId) { tc -> tc.copy(title = tc.title + event.title) }
                }
            is ChatStreamEvent.QueryArguments ->
                updateMessage(targetId) {
                    upsertToolCall(it, event.toolId) { tc ->
                        tc.copy(argumentsJson = tc.argumentsJson + event.argumentsJson)
                    }
                }
            is ChatStreamEvent.QueryDetail ->
                updateMessage(targetId) {
                    upsertToolCall(it, event.toolId) { tc ->
                        tc.copy(
                            resultJson = tc.resultJson + event.detailJson,
                            resultReceived = true,
                        )
                    }
                }
            is ChatStreamEvent.Image ->
                updateMessage(targetId) { it.copy(imageUrls = it.imageUrls + event.url) }
            is ChatStreamEvent.Chart ->
                updateMessage(targetId) { it.copy(charts = it.charts + event.optionJson) }
            is ChatStreamEvent.Error ->
                updateMessage(targetId) { it.copy(streaming = false, error = event.message) }
            ChatStreamEvent.End ->
                updateMessage(targetId) { it.copy(streaming = false) }
            is ChatStreamEvent.Stats ->
                updateMessage(targetId) { it.copy(costStats = event.stats) }
            is ChatStreamEvent.Conversation ->
                _state.update { if (it.conversationId == event.id) it else it.copy(conversationId = event.id) }
            ChatStreamEvent.Heartbeat,
            is ChatStreamEvent.Id,
            is ChatStreamEvent.Unknown -> Unit
        }
    }

    private fun updateMessage(id: String, transform: (ChatMessage) -> ChatMessage) {
        _state.update { current ->
            current.copy(messages = current.messages.map { if (it.id == id) transform(it) else it })
        }
    }

    /**
     * Append a transform to the [ToolCall] keyed by [toolId], creating a new one if it
     * hasn't been seen yet. Defensive against out-of-order events (e.g. queryArguments
     * arriving before queryTitle), though the backend guarantees title-first ordering.
     */
    private fun upsertToolCall(
        msg: ChatMessage,
        toolId: String,
        transform: (ToolCall) -> ToolCall,
    ): ChatMessage {
        val existing = msg.toolCalls.find { it.id == toolId }
        val updated = if (existing == null) {
            msg.toolCalls + transform(ToolCall(id = toolId))
        } else {
            msg.toolCalls.map { if (it.id == toolId) transform(it) else it }
        }
        return msg.copy(toolCalls = updated)
    }

    override fun onCleared() {
        streamJob?.cancel()
    }
}
