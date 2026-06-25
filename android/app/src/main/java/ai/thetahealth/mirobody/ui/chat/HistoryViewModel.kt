package ai.thetahealth.mirobody.ui.chat

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ai.thetahealth.mirobody.data.chat.ChatRepository
import ai.thetahealth.mirobody.data.chat.dto.SessionSummary
import ai.thetahealth.mirobody.data.net.ErrorBus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class HistoryUiState(
    val items: List<SessionSummary> = emptyList(),
    val loading: Boolean = false,
    val error: String? = null,
)

class HistoryViewModel(
    private val repo: ChatRepository,
    private val errorBus: ErrorBus,
) : ViewModel() {
    private val _state = MutableStateFlow(HistoryUiState())
    val state: StateFlow<HistoryUiState> = _state.asStateFlow()

    fun refresh() {
        _state.update { it.copy(loading = true, error = null) }
        viewModelScope.launch {
            runCatching { repo.history() }
                .onSuccess { items -> _state.update { it.copy(loading = false, items = items) } }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update {
                        it.copy(loading = false, error = t.message ?: "Failed to load history")
                    }
                }
        }
    }

    fun deleteHistory(sessionId: String) {
        if (sessionId.isBlank()) return
        // Optimistically drop the row; restore if the server call fails. The user is
        // informed of the failure via the app-wide Snackbar (ErrorBus).
        val previous = _state.value.items
        _state.update { it.copy(items = previous.filterNot { row -> row.sessionId == sessionId }) }
        viewModelScope.launch {
            runCatching { repo.deleteHistory(sessionId) }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update { it.copy(items = previous) }
                }
        }
    }
}
