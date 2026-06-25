package ai.thetahealth.mirobody.data.health

/** Half-open time window [startMillis, endMillis) in epoch milliseconds (UTC). */
data class TimeRange(val startMillis: Long, val endMillis: Long) {
    companion object {
        /** The last [days] days ending now. */
        fun lastDays(days: Int, nowMillis: Long): TimeRange =
            TimeRange(nowMillis - days.toLong() * 24 * 60 * 60 * 1000, nowMillis)
    }
}

/**
 * One on-device health store. Two implementations exist — [HealthConnectSource]
 * (Android Health Connect, for GMS devices: Pixel, Samsung, Xiaomi, …) and
 * [HmsHealthSource] (Huawei Health Kit, for Huawei devices with HMS Core). The
 * caller picks one at runtime via [HealthSourceFactory] based on what the device
 * exposes; the rest of the app talks only to this interface.
 *
 * Permission *requesting* is intentionally not here: it needs an Activity result
 * launcher, so the UI drives it (see [permissionsContractInput] /
 * [hasAllPermissions]); everything else is suspendable and Activity-free.
 */
interface HealthSource {
    /** Human-readable source name, e.g. "Health Connect" / "HMS Health Kit". */
    val name: String

    /** True when this store's backing service is installed/usable on the device. */
    suspend fun isAvailable(): Boolean

    /** True when the user has already granted read access to every metric we read. */
    suspend fun hasAllPermissions(): Boolean

    /**
     * Opaque input the UI hands to this source's permission-request launcher (a
     * Health Connect permission contract, or an HMS sign-in intent). Returned as
     * `Any` so this interface stays free of platform types; the screen casts it
     * to the launcher it registered. See the source's own KDoc for the concrete
     * type and the matching ActivityResultContract.
     */
    fun permissionsContractInput(): Any

    /** Read all supported metrics over [range]. Assumes permissions are granted. */
    suspend fun read(range: TimeRange): List<HealthSample>
}
