package ai.thetahealth.mirobody.data.health

import android.content.Context
import android.util.Log
import com.huawei.hmf.tasks.Task
import com.huawei.hms.api.ConnectionResult
import com.huawei.hms.api.HuaweiApiAvailability
import com.huawei.hms.hihealth.HuaweiHiHealth
import com.huawei.hms.hihealth.data.DataType
import com.huawei.hms.hihealth.data.Field
import com.huawei.hms.hihealth.data.SamplePoint
import com.huawei.hms.hihealth.options.ReadOptions
import com.huawei.hms.hihealth.result.ReadReply
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.suspendCancellableCoroutine

/**
 * On-device source backed by **Huawei Health Kit** (HMS Core's HiHealth API) —
 * the read surface for Huawei devices, which have HMS Core and Huawei Health but
 * NO Google Health Connect. GMS devices use [HealthConnectSource] instead.
 *
 * GATED + VERSION-SENSITIVE. Huawei Health Kit requires (1) an approved Health Kit
 * entitlement on your AppGallery Connect app, (2) `agconnect-services.json` in the
 * module, and (3) the user signed in with a Huawei ID having granted the Health
 * Kit scopes. Until those exist this source is inert ([isAvailable]/[hasAllPermissions]
 * report false). The HiHealth class/field names below follow Huawei's published
 * Android reference; reconcile them against the exact SDK version you pull, and
 * grant the matching read scopes (e.g. healthkit/step.read, heartrate.read,
 * activity_record.read) in the sign-in request.
 *
 * Permission request (UI): launch the HMS Account sign-in Intent (with the Health
 * Kit scopes) returned by [permissionsContractInput], then call [read].
 */
class HmsHealthSource(private val context: Context) : HealthSource {

    override val name: String = "HMS Health Kit"

    override suspend fun isAvailable(): Boolean =
        HuaweiApiAvailability.getInstance()
            .isHuaweiMobileServicesAvailable(context) == ConnectionResult.SUCCESS

    /**
     * Whether the signed-in Huawei ID has granted the Health Kit read scopes.
     * Reconcile against your scope set; conservatively false when HMS is absent so
     * the factory falls back rather than crashing on a non-Huawei device.
     */
    override suspend fun hasAllPermissions(): Boolean {
        if (!isAvailable()) return false
        // TODO(huawei): check AccountAuthManager's granted scopes against the Health
        // Kit read scopes once the entitlement + sign-in flow are wired. Returning
        // false keeps the source inert until then.
        return false
    }

    /**
     * The HMS Account sign-in Intent carrying the Health Kit scopes. The UI casts
     * this to Intent and launches it with StartActivityForResult, then re-checks
     * [hasAllPermissions]. Built by the caller's auth layer; left as the documented
     * seam here rather than a fabricated scope list.
     */
    override fun permissionsContractInput(): Any =
        throw NotImplementedError(
            "HMS Health Kit sign-in Intent must be built with your approved Health Kit " +
                "scopes (AppGallery Connect entitlement required) — see class KDoc",
        )

    override suspend fun read(range: TimeRange): List<HealthSample> {
        if (!isAvailable()) return emptyList()
        val controller = HuaweiHiHealth.getDataController(context)
        val out = ArrayList<HealthSample>()

        // Steps — continuous step deltas; FIELD_STEPS is an int count per point.
        readPoints(controller.read(options(DataType.DT_CONTINUOUS_STEPS_DELTA, range))).forEach { p ->
            out += HealthSample(
                metric = HealthMetric.STEPS,
                value = p.getFieldValue(Field.FIELD_STEPS).asIntValue().toDouble(),
                startMillis = p.getStartTime(TimeUnit.MILLISECONDS),
                endMillis = p.getEndTime(TimeUnit.MILLISECONDS),
                source = name,
            )
        }
        // Heart rate — instantaneous bpm.
        readPoints(controller.read(options(DataType.DT_INSTANTANEOUS_HEART_RATE, range))).forEach { p ->
            val t = p.getStartTime(TimeUnit.MILLISECONDS)
            out += HealthSample(
                metric = HealthMetric.HEART_RATE,
                value = p.getFieldValue(Field.FIELD_BPM).asDoubleValue(),
                startMillis = t,
                endMillis = t,
                source = name,
            )
        }
        // Weight — instantaneous body weight (kg).
        readPoints(controller.read(options(DataType.DT_INSTANTANEOUS_BODY_WEIGHT, range))).forEach { p ->
            val t = p.getStartTime(TimeUnit.MILLISECONDS)
            out += HealthSample(
                metric = HealthMetric.WEIGHT,
                value = p.getFieldValue(Field.FIELD_BODY_WEIGHT).asDoubleValue(),
                startMillis = t,
                endMillis = t,
                source = name,
            )
        }
        // Sleep is read through HiHealth's separate sleep-records API, not the
        // DataController; left out here until that path is wired against the SDK.
        return out
    }

    private fun options(type: DataType, range: TimeRange): ReadOptions =
        ReadOptions.Builder()
            .read(type)
            .setTimeRange(range.startMillis, range.endMillis, TimeUnit.MILLISECONDS)
            .build()

    private suspend fun readPoints(task: Task<ReadReply>): List<SamplePoint> = try {
        task.await().sampleSets.flatMap { it.samplePoints }
    } catch (t: Throwable) {
        Log.w(TAG, "HMS read failed", t)
        emptyList()
    }

    private suspend fun <T> Task<T>.await(): T = suspendCancellableCoroutine { cont ->
        addOnSuccessListener { cont.resume(it) }
        addOnFailureListener { cont.resumeWithException(it) }
    }

    private companion object {
        const val TAG = "HmsHealthSource"
    }
}
