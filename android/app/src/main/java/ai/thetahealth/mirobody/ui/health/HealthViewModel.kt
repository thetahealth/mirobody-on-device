package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.data.health.HealthRepository
import ai.thetahealth.mirobody.data.health.SyncResult
import ai.thetahealth.mirobody.data.health.TimeRange
import ai.thetahealth.mirobody.data.net.ErrorBus
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Drives the on-device health sync dialog: resolves the active source (Health
 * Connect or HMS), exposes the Health Connect permission set the UI must request,
 * and runs the read→POST sync. Mirrors the other settings ViewModels.
 */
class HealthViewModel(
    private val repo: HealthRepository,
    private val errorBus: ErrorBus,
) : ViewModel() {

    data class UiState(
        val loading: Boolean = true,
        val sourceName: String? = null,
        /** Health Connect permission strings to launch the permission contract with; empty otherwise. */
        val permissions: Set<String> = emptySet(),
        val syncing: Boolean = false,
        val result: SyncResult? = null,
    )

    private val _state = MutableStateFlow(UiState())
    val state = _state.asStateFlow()

    init {
        viewModelScope.launch {
            val source = repo.source()
            // Health Connect exposes its permission Set<String> here; HMS throws
            // (no contract input yet), so runCatching keeps the dialog usable.
            @Suppress("UNCHECKED_CAST")
            val perms = source
                ?.let { runCatching { it.permissionsContractInput() }.getOrNull() } as? Set<String>
                ?: emptySet()
            _state.update { it.copy(loading = false, sourceName = source?.name, permissions = perms) }
        }
    }

    /** Read the last [days] days from the active source and POST as FHIR. */
    fun sync(days: Int = 7) {
        viewModelScope.launch {
            _state.update { it.copy(syncing = true, result = null) }
            val now = System.currentTimeMillis()
            val result = runCatching { repo.sync(TimeRange.lastDays(days, now)) }
                .getOrElse { t ->
                    errorBus.emit(t)
                    SyncResult(0, 0, _state.value.sourceName, t.message)
                }
            _state.update { it.copy(syncing = false, result = result) }
        }
    }
}
