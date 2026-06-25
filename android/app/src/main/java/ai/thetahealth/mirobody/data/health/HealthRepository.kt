package ai.thetahealth.mirobody.data.health

import android.util.Log

/** Outcome of a sync run: how many Observations the server accepted / rejected. */
data class SyncResult(
    val posted: Int,
    val failed: Int,
    val sourceName: String?,
    val error: String? = null,
)

/**
 * Reads on-device health data from whichever store the device exposes and pushes
 * it to the server's FHIR endpoint. This is the on-device ingestion path for the
 * "phone" health platforms: Apple is the iOS sibling; here Health Connect covers
 * GMS devices and HMS Health Kit covers Huawei.
 *
 * Permission *requesting* is the UI's job (it needs an Activity launcher — see
 * [HealthSource.permissionsContractInput]); [sync] assumes permission is already
 * granted and no-ops cleanly if it isn't.
 */
class HealthRepository(
    private val api: HealthApi,
    private val sourceFactory: HealthSourceFactory,
) {
    /** The active on-device source, or null when neither store is available. */
    suspend fun source(): HealthSource? = sourceFactory.create()

    /**
     * Read [range] from the active source and POST each reading as a FHIR
     * Observation. Posts are sequential (one create per Observation) to keep the
     * path simple; batching via a FHIR transaction Bundle (POST /fhir) is a later
     * optimization. Returns counts; never throws on a single failed post.
     */
    suspend fun sync(range: TimeRange): SyncResult {
        val source = sourceFactory.create()
            ?: return SyncResult(0, 0, null, "no on-device health source on this device")
        if (!source.hasAllPermissions()) {
            return SyncResult(0, 0, source.name, "health permissions not granted")
        }

        val samples = source.read(range)
        var posted = 0
        var failed = 0
        for (sample in samples) {
            val observation = FhirObservation.from(sample)
            val ok = runCatching { api.postObservation(observation).isSuccessful }
                .getOrElse { t ->
                    Log.w(TAG, "post Observation failed", t)
                    false
                }
            if (ok) posted++ else failed++
        }
        Log.i(TAG, "health sync via ${source.name}: posted=$posted failed=$failed of ${samples.size}")
        return SyncResult(posted, failed, source.name)
    }

    private companion object {
        const val TAG = "HealthRepository"
    }
}
