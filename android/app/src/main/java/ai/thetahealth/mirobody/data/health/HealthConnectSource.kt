package ai.thetahealth.mirobody.data.health

import android.content.Context
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.HeartRateRecord
import androidx.health.connect.client.records.SleepSessionRecord
import androidx.health.connect.client.records.StepsRecord
import androidx.health.connect.client.records.WeightRecord
import androidx.health.connect.client.request.ReadRecordsRequest
import androidx.health.connect.client.time.TimeRangeFilter
import java.time.Instant

/**
 * On-device source backed by Android **Health Connect** — the read surface for
 * every GMS device (Pixel, Samsung, Xiaomi/Mi Fitness, Honor, OPPO, vivo, …),
 * which all write into Health Connect. Huawei devices have no Health Connect; they
 * use [HmsHealthSource] instead.
 *
 * Permission request (UI): register
 * `HealthConnectClient.getOrCreate(ctx).permissionController` with the contract
 * `PermissionController.createRequestPermissionResultContract()` and launch it
 * with [permissionsContractInput] (the set of [HealthPermission]s). On Android 14+
 * Health Connect is built in; on 9–13 it is the installable APK.
 */
class HealthConnectSource(private val context: Context) : HealthSource {

    override val name: String = "Health Connect"

    private val permissions: Set<String> = setOf(
        HealthPermission.getReadPermission(StepsRecord::class),
        HealthPermission.getReadPermission(HeartRateRecord::class),
        HealthPermission.getReadPermission(SleepSessionRecord::class),
        HealthPermission.getReadPermission(WeightRecord::class),
    )

    private fun clientOrNull(): HealthConnectClient? =
        if (HealthConnectClient.getSdkStatus(context) == HealthConnectClient.SDK_AVAILABLE) {
            HealthConnectClient.getOrCreate(context)
        } else {
            null
        }

    override suspend fun isAvailable(): Boolean = clientOrNull() != null

    override suspend fun hasAllPermissions(): Boolean {
        val client = clientOrNull() ?: return false
        return client.permissionController.getGrantedPermissions().containsAll(permissions)
    }

    /** The permission set to launch the Health Connect permission contract with. */
    override fun permissionsContractInput(): Any = permissions

    override suspend fun read(range: TimeRange): List<HealthSample> {
        val client = clientOrNull() ?: return emptyList()
        val filter = TimeRangeFilter.between(
            Instant.ofEpochMilli(range.startMillis),
            Instant.ofEpochMilli(range.endMillis),
        )
        val out = ArrayList<HealthSample>()

        // Steps: each record is a count over [startTime, endTime).
        client.readRecords(ReadRecordsRequest(StepsRecord::class, filter)).records.forEach { r ->
            out += HealthSample(
                metric = HealthMetric.STEPS,
                value = r.count.toDouble(),
                startMillis = r.startTime.toEpochMilli(),
                endMillis = r.endTime.toEpochMilli(),
                source = name,
            )
        }
        // Heart rate: each record holds instantaneous samples (bpm).
        client.readRecords(ReadRecordsRequest(HeartRateRecord::class, filter)).records.forEach { r ->
            r.samples.forEach { hr ->
                val t = hr.time.toEpochMilli()
                out += HealthSample(HealthMetric.HEART_RATE, hr.beatsPerMinute.toDouble(), t, t, name)
            }
        }
        // Sleep: one session -> its duration in minutes over [start, end).
        client.readRecords(ReadRecordsRequest(SleepSessionRecord::class, filter)).records.forEach { r ->
            val minutes = (r.endTime.toEpochMilli() - r.startTime.toEpochMilli()) / 60000.0
            out += HealthSample(
                metric = HealthMetric.SLEEP,
                value = minutes,
                startMillis = r.startTime.toEpochMilli(),
                endMillis = r.endTime.toEpochMilli(),
                source = name,
            )
        }
        // Weight: instantaneous body mass (kg).
        client.readRecords(ReadRecordsRequest(WeightRecord::class, filter)).records.forEach { r ->
            val t = r.time.toEpochMilli()
            out += HealthSample(HealthMetric.WEIGHT, r.weight.inKilograms, t, t, name)
        }
        return out
    }
}
