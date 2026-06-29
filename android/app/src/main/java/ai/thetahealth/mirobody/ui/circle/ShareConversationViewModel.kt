package ai.thetahealth.mirobody.ui.circle

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ai.thetahealth.mirobody.data.circle.CircleRepository
import ai.thetahealth.mirobody.data.net.ErrorBus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** One shareable circle member: the opaque handle plus its current share state. */
data class ShareTarget(
    val member: Long,
    val email: String,
    val nickname: String,
    val wasShared: Boolean,
    val initialAccess: String,   // "view" | "edit"
)

/** The user's edited choice for one member, applied on Save. */
data class ShareSelection(
    val member: Long,
    val checked: Boolean,
    val access: String,
    val wasShared: Boolean,
)

data class ShareUiState(
    val targets: List<ShareTarget> = emptyList(),
    val loading: Boolean = true,
    val busy: Boolean = false,
    val error: String? = null,
)

/**
 * Backs the "share this conversation" dialog. Lists accepted circle members
 * (flattened across all my circles, deduped, excluding myself — the backend lets
 * me share with anyone I'm in a circle with, except me) and applies the user's
 * share/unshare choices on Save. Mirrors the web client's share modal.
 */
class ShareConversationViewModel(
    private val repo: CircleRepository,
    private val errorBus: ErrorBus,
) : ViewModel() {
    private val _state = MutableStateFlow(ShareUiState())
    val state: StateFlow<ShareUiState> = _state.asStateFlow()

    fun load(conversationId: String) {
        _state.update { it.copy(loading = true, error = null) }
        viewModelScope.launch {
            val sharesRes = runCatching { repo.conversationShares(conversationId) }
            runCatching { repo.members() }
                .onSuccess { res ->
                    val accessByEmail = sharesRes.getOrDefault(emptyList())
                        .associate { it.email to it.access }
                    val seen = HashSet<String>()
                    val targets = res.circles
                        .flatMap { it.members }
                        .filter { it.status == "accepted" && !it.me }
                        .filter { seen.add(it.email.ifBlank { "#${it.member}" }) }
                        .map { m ->
                            val acc = accessByEmail[m.email]
                            ShareTarget(
                                member = m.member,
                                email = m.email,
                                nickname = m.nickname,
                                wasShared = acc != null,
                                initialAccess = if (acc == "edit") "edit" else "view",
                            )
                        }
                    _state.update { it.copy(loading = false, targets = targets) }
                }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update { it.copy(loading = false, error = t.message) }
                }
        }
    }

    /** Apply the selections (share newly-checked, unshare newly-unchecked), then [onDone]. */
    fun apply(conversationId: String, selections: List<ShareSelection>, onDone: () -> Unit) {
        if (_state.value.busy) return
        _state.update { it.copy(busy = true) }
        viewModelScope.launch {
            runCatching {
                for (sel in selections) {
                    if (sel.checked) {
                        repo.shareConversation(conversationId, listOf(sel.member), sel.access)
                    } else if (sel.wasShared) {
                        repo.unshareConversation(conversationId, sel.member)
                    }
                }
            }.onFailure { errorBus.emit(it) }
            _state.update { it.copy(busy = false) }
            onDone()
        }
    }
}
