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
import ai.thetahealth.mirobody.data.llm.ChatTurn
import ai.thetahealth.mirobody.data.llm.MlKitTextService
import ai.thetahealth.mirobody.data.llm.ModelManager
import ai.thetahealth.mirobody.data.llm.OnDeviceModel
import ai.thetahealth.mirobody.data.llm.OnDeviceModelSpec
import ai.thetahealth.mirobody.data.llm.OnDeviceModelStatus
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
    // Per-model download/ready state, keyed by OnDeviceModelSpec.id. Drives the model
    // manager dialog and which on-device models appear in the picker. Mirrors
    // ModelManager.statuses.
    val onDeviceModels: Map<String, OnDeviceModelStatus> = emptyMap(),
    // User-imported on-device models (picked from the filesystem, live outside app storage).
    val onDeviceImported: List<OnDeviceModelSpec> = emptyList(),
    // Whether Gemini Nano (ML Kit) can rewrite on this device; gates the composer
    // "Polish" affordance. False on hardware without AICore.
    val polishAvailable: Boolean = false,
    // A rewrite is in flight (composer "Polish" shows a spinner, send is blocked).
    val polishing: Boolean = false,
    // Privacy mode: entering swaps the session for a fresh ephemeral one; while on,
    // turns aren't mirrored locally and each request carries incognito:true.
    val incognito: Boolean = false,
)

class ChatViewModel(
    private val repo: ChatRepository,
    private val circleRepo: CircleRepository,
    private val settings: SettingsStore,
    private val errorBus: ErrorBus,
    private val history: ChatHistoryStore,
    private val modelManager: ModelManager,
    private val mlKit: MlKitTextService,
) : ViewModel() {

    private val _state = MutableStateFlow(ChatUiState(sessionId = UUID.randomUUID().toString()))
    val state: StateFlow<ChatUiState> = _state.asStateFlow()

    private var streamJob: Job? = null

    // Session stashed when entering incognito, restored on exit (messages + thread id).
    private var incognitoSaved: Pair<List<ChatMessage>, String>? = null

    // Server providers, kept so the picker can be rebuilt when the set of downloaded
    // on-device models changes; and the persisted selection key for restore.
    private var remoteProviders: List<ProviderInfo> = emptyList()
    private var savedProviderKey: String? = null

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
        viewModelScope.launch {
            modelManager.statuses.collect { st ->
                _state.update { it.copy(onDeviceModels = st) }
                rebuildProviders(st)
            }
        }
        viewModelScope.launch {
            modelManager.imported.collect { imported ->
                _state.update { it.copy(onDeviceImported = imported) }
                rebuildProviders()
            }
        }
        // Probe Gemini Nano support once; the composer "Polish" button only shows if true.
        viewModelScope.launch {
            val supported = runCatching { mlKit.isRewriteSupported() }.getOrDefault(false)
            _state.update { it.copy(polishAvailable = supported) }
        }
    }

    /**
     * Rewrite the current draft on-device (Gemini Nano) in the given [tone], replacing
     * the composer text in place. No-op while empty or already polishing.
     */
    fun polishDraft(tone: MlKitTextService.Tone) {
        val s = _state.value
        val draft = s.input.trim()
        if (draft.isEmpty() || s.polishing) return
        _state.update { it.copy(polishing = true) }
        viewModelScope.launch {
            mlKit.polish(draft, tone, s.language)
                .onSuccess { rewritten -> _state.update { it.copy(input = rewritten) } }
                .onFailure { t -> errorBus.emit(t) }
            _state.update { it.copy(polishing = false) }
        }
    }

    /** Start (or resume) a model download. Safe to call when already running. */
    fun downloadOnDeviceModel(spec: OnDeviceModelSpec) {
        viewModelScope.launch { runCatching { modelManager.download(spec) } }
    }

    /** Remove a downloaded model to reclaim storage. */
    fun deleteOnDeviceModel(spec: OnDeviceModelSpec) {
        modelManager.delete(spec)
    }

    /** Import a model file the user picked from the filesystem (lives outside app storage). */
    fun importOnDeviceModel(uri: android.net.Uri) {
        viewModelScope.launch {
            runCatching { modelManager.import(uri) }
                .onFailure { errorBus.emit(it) }
                .onSuccess { if (it == null) errorBus.emit(IllegalStateException("Could not import model file")) }
        }
    }

    /** Forget an imported model (its file is kept unless we copied it in). */
    fun deleteImportedOnDeviceModel(spec: OnDeviceModelSpec) {
        modelManager.deleteImported(spec)
    }

    /** Whether the app can access shared storage for downloads / path-referenced imports. */
    fun hasStorageAccess(): Boolean = modelManager.hasStorageAccess()

    /** Mirror the current conversation to local storage (best-effort, off the UI path). */
    private fun persist() {
        val messages = _state.value.messages
        viewModelScope.launch { history.save(messages) }
    }

    fun loadProviders() {
        viewModelScope.launch {
            savedProviderKey = settings.selectedProviderName.first()
            runCatching { repo.listProviders() }
                .onSuccess { remote ->
                    remoteProviders = remote
                    _state.update { it.copy(error = null) }
                    rebuildProviders()
                }
                .onFailure { t ->
                    errorBus.emit(t)
                    // The server is unreachable, but on-device chat still works offline —
                    // keep the picker populated (manage entry + any downloaded models).
                    remoteProviders = emptyList()
                    _state.update { it.copy(error = t.message) }
                    rebuildProviders()
                }
        }
    }

    /**
     * Rebuild the provider picker: server providers + one entry per *downloaded*
     * on-device model + the "manage on-device AI" entry (mirroring the Qt desktop
     * client — undownloaded models aren't listed; the manage entry downloads them).
     * Called on provider load and whenever a model's download state changes. Keeps the
     * current selection if still present, else restores the saved one, else the first.
     */
    private fun rebuildProviders(
        statuses: Map<String, OnDeviceModelStatus> = _state.value.onDeviceModels,
    ) {
        val downloaded = OnDeviceModel.CATALOG
            .filter { statuses[it.id] is OnDeviceModelStatus.Ready }
            .map { ProviderInfo.forModel(it) }
        val imported = modelManager.imported.value.map { ProviderInfo.forModel(it) }
        val list = remoteProviders + downloaded + imported + ProviderInfo.manage
        _state.update { s ->
            val selected = s.selected?.let { sel -> list.firstOrNull { it.key == sel.key } }
                ?: list.firstOrNull { it.key == savedProviderKey }
                ?: list.firstOrNull()
            s.copy(providers = list, selected = selected)
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

    /**
     * Toggle privacy mode, swapping the whole session (mirrors the web client).
     * Entering stashes the current thread and starts a fresh ephemeral one; leaving
     * restores it. Inert while a reply is streaming.
     */
    fun toggleIncognito() {
        val s = _state.value
        if (s.sending) return
        streamJob?.cancel()
        if (!s.incognito) {
            incognitoSaved = s.messages to s.conversationId
            _state.update { it.copy(messages = emptyList(), conversationId = "", incognito = true) }
        } else {
            val (savedMessages, savedConv) = incognitoSaved ?: (emptyList<ChatMessage>() to "")
            incognitoSaved = null
            _state.update { it.copy(messages = savedMessages, conversationId = savedConv, incognito = false) }
        }
    }

    /**
     * Open a saved conversation from history into the chat view and continue it:
     * adopt its session id as the thread key so the next turn appends to it. Leaves
     * incognito (this is a real, persisted thread) and mirrors it locally if owned.
     */
    fun openConversation(sessionId: String) {
        if (_state.value.sending) return
        streamJob?.cancel()
        incognitoSaved = null
        viewModelScope.launch {
            runCatching { repo.conversation(sessionId) }
                .onSuccess { detail ->
                    val msgs = detail.messages
                        .filter { it.role == "user" || it.role == "assistant" }
                        .mapIndexed { i, m ->
                            ChatMessage(
                                id = "h-$sessionId-$i",
                                role = if (m.role == "assistant") Role.Assistant else Role.User,
                                text = m.content,
                                provider = if (m.role == "assistant") {
                                    if (m.agent.isBlank()) m.provider else "${m.agent}/${m.provider}"
                                } else "",
                            )
                        }
                    _state.update {
                        it.copy(
                            messages = msgs,
                            sessionId = sessionId,
                            conversationId = detail.id.ifBlank { sessionId },
                            incognito = false,
                            input = "",
                        )
                    }
                    if (detail.owned) history.save(msgs)
                }
                .onFailure { errorBus.emit(it) }
        }
    }

    /**
     * Clear the current thread and start a new one (the drawer's "New chat"). Also
     * clears the local mirror unless incognito (nothing is stored there anyway).
     */
    fun newChat() {
        if (_state.value.sending) return
        streamJob?.cancel()
        val wasIncognito = _state.value.incognito
        _state.update { it.copy(messages = emptyList(), conversationId = "", input = "") }
        if (!wasIncognito) viewModelScope.launch { history.clear() }
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
            settings.setSelectedProviderName(provider.key)
        }
    }

    fun send() {
        val s = _state.value
        val question = s.input.trim()
        val selected = s.selected
        val attachments = s.attachments
        // Allow an attachment-only turn (no text), matching the web composer.
        if ((question.isEmpty() && attachments.isEmpty()) || s.sending || selected == null) return
        // The "manage on-device AI" entry isn't a chat provider; selecting it opens the
        // manager (handled in the UI), never sends.
        if (selected.isManageEntry) return

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
            provider = selected.label,
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
        // Incognito turns are never mirrored locally.
        if (!s.incognito) persist()

        streamJob?.cancel()
        streamJob = viewModelScope.launch {
            runCatching {
                val onDeviceSpec = selected.modelSpec
                val flow = if (onDeviceSpec != null) {
                    // Offline path: hand the settled transcript (incl. this new question,
                    // excluding the in-flight placeholder) to the on-device engine.
                    val turns = _state.value.messages
                        .filter { it.id != assistantMsg.id && it.text.isNotBlank() }
                        .map { ChatTurn(fromUser = it.role == Role.User, text = it.text) }
                    repo.chatOnDevice(turns, onDeviceSpec)
                } else {
                    repo.chat(
                        sessionId = s.sessionId,
                        question = question,
                        agent = selected.agent,
                        provider = selected.provider,
                        language = s.language,
                        subject = s.subject,
                        attachments = attachments,
                        incognito = s.incognito,
                    )
                }
                flow.collect { event -> applyEvent(assistantMsg.id, event) }
            }.onFailure { t ->
                errorBus.emit(t)
                updateMessage(assistantMsg.id) { it.copy(streaming = false, error = t.message) }
            }
            _state.update { it.copy(sending = false) }
            if (!s.incognito) persist()   // settled turn (reply / error) is now safe to store
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
