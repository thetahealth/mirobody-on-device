package ai.thetahealth.mirobody.ui.circle

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ai.thetahealth.mirobody.data.circle.CircleRepository
import ai.thetahealth.mirobody.data.circle.dto.Circle
import ai.thetahealth.mirobody.data.circle.dto.CircleInvite
import ai.thetahealth.mirobody.data.net.ErrorBus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class CircleUiState(
    val circles: List<Circle> = emptyList(),
    val invites: List<CircleInvite> = emptyList(),
    val loading: Boolean = false,
    val busy: Boolean = false,          // a mutation is in flight
    val error: String? = null,
)

class CircleViewModel(
    private val repo: CircleRepository,
    private val errorBus: ErrorBus,
) : ViewModel() {
    private val _state = MutableStateFlow(CircleUiState())
    val state: StateFlow<CircleUiState> = _state.asStateFlow()

    fun refresh() {
        _state.update { it.copy(loading = true, error = null) }
        viewModelScope.launch {
            runCatching { repo.members() }
                .onSuccess { res ->
                    _state.update {
                        it.copy(loading = false, circles = res.circles, invites = res.invites)
                    }
                }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update { it.copy(loading = false, error = t.message) }
                }
        }
    }

    // Each mutation re-reads the roster on success so the UI reflects server truth
    // (roles, statuses, and the opaque member handles all stay consistent).
    private fun mutate(block: suspend CircleRepository.() -> Unit) {
        if (_state.value.busy) return
        _state.update { it.copy(busy = true) }
        viewModelScope.launch {
            runCatching { repo.block() }
                .onSuccess {
                    runCatching { repo.members() }.onSuccess { res ->
                        _state.update {
                            it.copy(busy = false, circles = res.circles, invites = res.invites)
                        }
                    }.onFailure { _state.update { it.copy(busy = false) } }
                }
                .onFailure { t ->
                    errorBus.emit(t)
                    _state.update { it.copy(busy = false) }
                }
        }
    }

    fun createCircle(name: String) = mutate { create(name) }
    fun renameCircle(circleId: Long, name: String) = mutate { rename(circleId, name) }
    fun deleteCircle(circleId: Long) = mutate { delete(circleId) }
    fun invite(email: String, circleId: Long) = mutate { invite(email, circleId) }
    fun accept(token: String) = mutate { accept(token) }
    fun decline(token: String) = mutate { decline(token) }
    fun remove(member: Long) = mutate { remove(member) }
    fun setNickname(member: Long, nickname: String) = mutate { setNickname(member, nickname) }
    fun setRole(member: Long, role: String) = mutate { setRole(member, role) }
    fun setHealthSharing(circleId: Long, access: String) = mutate { setHealthSharing(circleId, access) }
}
