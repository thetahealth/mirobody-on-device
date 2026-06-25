package ai.thetahealth.mirobody.data.health

import android.content.Context

/**
 * Picks the on-device health source at runtime: Health Connect on GMS devices,
 * HMS Health Kit on Huawei. One APK covers both ecosystems — only the matching
 * store is used per device. Returns null when neither is present (e.g. a device
 * with neither Health Connect nor HMS Core), so callers can degrade cleanly.
 *
 * Health Connect is preferred when both somehow resolve, since it's the broader
 * Android surface; Huawei devices won't have it, so they fall through to HMS.
 */
class HealthSourceFactory(private val context: Context) {
    suspend fun create(): HealthSource? {
        val healthConnect = HealthConnectSource(context)
        if (healthConnect.isAvailable()) return healthConnect

        val hms = HmsHealthSource(context)
        if (hms.isAvailable()) return hms

        return null
    }
}
