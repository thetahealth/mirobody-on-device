package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.data.health.EhrProvider
import ai.thetahealth.mirobody.data.health.EhrRepository
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

data class EhrUiState(
    val results: List<EhrProvider> = emptyList(),
    val searching: Boolean = false,
    val busy: Boolean = false,       // connecting or syncing
    val error: String? = null,
    val syncedCount: Int? = null,    // non-null after a successful sync
)

/**
 * Backs the EHR-connect dialog: directory search, connect (open the SMART OAuth
 * URL in a browser), and sync (pull FHIR). Mirrors the web client's ehr.js.
 */
class EhrViewModel(private val repo: EhrRepository) : ViewModel() {

    private val _state = MutableStateFlow(EhrUiState())
    val state: StateFlow<EhrUiState> = _state.asStateFlow()

    // One-shot OAuth URL for the dialog to open in a browser; the EHR redirects to
    // the server callback, which stores the link, so on return the user can Sync.
    private val _connectUrl = MutableStateFlow<String?>(null)
    val connectUrl: StateFlow<String?> = _connectUrl.asStateFlow()

    fun search(query: String) {
        viewModelScope.launch {
            _state.value = _state.value.copy(searching = true, error = null, syncedCount = null)
            runCatching { repo.providers(query) }
                .onSuccess { _state.value = _state.value.copy(results = it, searching = false) }
                .onFailure { _state.value = _state.value.copy(searching = false, error = it.message) }
        }
    }

    fun connect(fhirBaseUrl: String) {
        if (fhirBaseUrl.isBlank()) return
        viewModelScope.launch {
            _state.value = _state.value.copy(busy = true, error = null, syncedCount = null)
            runCatching { repo.authorizeUrl(fhirBaseUrl.trim().trimEnd('/')) }
                .onSuccess { url ->
                    _state.value = _state.value.copy(busy = false)
                    if (!url.isNullOrBlank()) _connectUrl.value = url
                    else _state.value = _state.value.copy(error = "No authorize URL")
                }
                .onFailure { _state.value = _state.value.copy(busy = false, error = it.message) }
        }
    }

    fun consumeConnectUrl() { _connectUrl.value = null }

    fun sync() {
        viewModelScope.launch {
            _state.value = _state.value.copy(busy = true, error = null, syncedCount = null)
            runCatching { repo.sync() }
                .onSuccess { _state.value = _state.value.copy(busy = false, syncedCount = it) }
                .onFailure { _state.value = _state.value.copy(busy = false, error = it.message) }
        }
    }
}
