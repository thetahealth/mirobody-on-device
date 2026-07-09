package ai.thetahealth.mirobody.ui.vendor

import ai.thetahealth.mirobody.data.vendor.VendorLink
import ai.thetahealth.mirobody.data.vendor.VendorRepository
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** One catalog row: brand id/name, whether it's connected (and still pending
 *  verification), and its server-provided icon data URI (null -> monogram). */
data class VendorItem(
    val id: String,
    val name: String,
    val connected: Boolean,
    val pending: Boolean,
    val iconDataUri: String?,
)

data class VendorsUiState(
    val devices: List<VendorItem> = emptyList(),
    val platforms: List<VendorItem> = emptyList(),
    val loading: Boolean = true,
    val error: String? = null,
)

/**
 * Backs the Connected-devices dialog: lists the static vendor catalog (devices /
 * platforms), marks the ones the server says are connected, and drives connect
 * (open the OAuth URL in a browser) / disconnect. Mirrors the web vendors.js.
 */
class VendorsViewModel(private val repo: VendorRepository) : ViewModel() {

    private val _state = MutableStateFlow(VendorsUiState())
    val state: StateFlow<VendorsUiState> = _state.asStateFlow()

    // A one-shot OAuth URL for the dialog to open in a browser; cleared once
    // launched (see consumeConnectUrl). The vendor redirects to the server
    // callback, which stores the grant, so on return we just re-list.
    private val _connectUrl = MutableStateFlow<String?>(null)
    val connectUrl: StateFlow<String?> = _connectUrl.asStateFlow()

    private var icons: Map<String, String> = emptyMap()

    init { load() }

    fun load() {
        viewModelScope.launch {
            _state.value = _state.value.copy(loading = true, error = null)
            try {
                val connected = repo.connected()
                if (icons.isEmpty()) icons = repo.icons()
                _state.value = build(connected)
            } catch (e: Exception) {
                _state.value = _state.value.copy(loading = false, error = e.message)
            }
        }
    }

    fun connect(id: String) {
        viewModelScope.launch {
            runCatching { repo.authorizeUrl(id) }
                .onSuccess { url ->
                    if (!url.isNullOrBlank()) _connectUrl.value = url
                    else _state.value = _state.value.copy(error = "No authorize URL")
                }
                .onFailure { _state.value = _state.value.copy(error = it.message) }
        }
    }

    fun consumeConnectUrl() { _connectUrl.value = null }

    fun unlink(id: String) {
        viewModelScope.launch {
            runCatching { repo.unlink(id) }
                .onSuccess { load() }
                .onFailure { _state.value = _state.value.copy(error = it.message) }
        }
    }

    private fun build(connected: Map<String, VendorLink>): VendorsUiState {
        fun items(ids: List<String>): List<VendorItem> =
            ids.sortedBy { nameOf(it).lowercase() }.map { id ->
                val link = connected[id]
                VendorItem(
                    id = id,
                    name = nameOf(id),
                    connected = link != null,
                    pending = link != null && !link.verified,
                    iconDataUri = icons[id],
                )
            }
        return VendorsUiState(items(DEVICES), items(PLATFORMS), loading = false, error = null)
    }

    companion object {
        // Consumer wearables/devices; the B2B aggregation platforms sit behind the
        // second tab. Mirrors src/health/vendor/ (see the web client's CATALOG).
        val DEVICES = listOf(
            "oura", "whoop", "polar", "fitbit", "withings", "dexcom", "garmin", "huawei",
        )
        val PLATFORMS = listOf(
            "terra", "validic", "rook", "spike", "junction", "wefitter", "thryve",
            "human_api", "vitalera", "metriport", "open_wearables", "redox",
            "particle_health", "healthconnect", "lexisnexis",
        )

        private val NAMES = mapOf(
            "oura" to "Oura", "whoop" to "WHOOP", "polar" to "Polar", "fitbit" to "Fitbit",
            "withings" to "Withings", "dexcom" to "Dexcom", "garmin" to "Garmin",
            "huawei" to "Huawei Health", "terra" to "Terra", "validic" to "Validic",
            "rook" to "Rook", "spike" to "Spike", "junction" to "Junction",
            "wefitter" to "WeFitter", "thryve" to "Thryve", "human_api" to "Human API",
            "vitalera" to "Vitalera", "metriport" to "Metriport",
            "open_wearables" to "Open Wearables", "redox" to "Redox",
            "particle_health" to "Particle Health", "healthconnect" to "HealthConnect CoPilot",
            "lexisnexis" to "LexisNexis",
        )

        fun nameOf(id: String): String = NAMES[id] ?: id
    }
}
