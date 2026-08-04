package ai.thetahealth.mirobody.data.health.hdp

import ai.thetahealth.mirobody.data.health.FhirObs
import kotlinx.serialization.json.JsonObject
import kotlin.math.pow

/**
 * Minimal, **EXPERIMENTAL** manager-side (sink) half of the IEEE 11073-20601 optimized
 * exchange protocol, spoken over the raw HDP channel file descriptor that Android's
 * (deprecated, API ≤ 28 only) `BluetoothHealth` hands us. This is the legacy classic-
 * Bluetooth counterpart of the BLE `GattHealthCodec` — it exists so the community knows
 * old 11073 medical devices can still be ingested, and as an extension point.
 *
 * What is implemented and deterministic (unit-tested):
 *   - APDU framing / type detection (AARQ / PRST / RLRQ / ABRT).
 *   - The association handshake: accept the agent's association, accept its config
 *     (echoing the config-report-id), acknowledge data reports (echoing the invoke-id).
 *   - The IEEE 11073 FLOAT-Type decode (identical maths to the BLE float32).
 *
 * What is BEST-EFFORT and NOT hardware-validated:
 *   - Locating the observed value inside a measurement report. A fully correct parse
 *     tracks the agent's configuration (handle → type/format); here we extract the
 *     first plausible FLOAT and map it via the sink's device specialization (so a
 *     thermometer's value becomes a body temperature, etc.). Good enough for single-
 *     value devices (thermometer, scale, SpO₂, glucose, heart rate); compound reports
 *     (e.g. blood pressure's systolic/diastolic/MAP) need the config-driven parse.
 *
 * Contributions to complete the MDER parse are welcome — see src/health/README.md.
 */
object Ieee11073Agent {

    // APDU CHOICE types (IEEE 11073-20601 §6.3.1), the first 2 bytes of every APDU.
    const val AARQ = 0xE200   // association request  (agent → us)
    const val AARE = 0xE300   // association response (us → agent)
    const val RLRQ = 0xE400   // release request
    const val RLRE = 0xE500   // release response
    const val ABRT = 0xE600   // abort
    const val PRST = 0xE700   // presentation (data)

    /** A decoded reading: `value` in `unit` for the UI, plus the FHIR body to POST. */
    data class Reading(val label: String, val value: Double, val unit: String, val observation: JsonObject)

    /** What to do after handling one inbound APDU. */
    data class Result(val reply: ByteArray?, val readings: List<Reading>, val released: Boolean)

    /**
     * The IEEE 11073 device specialization the sink registered this channel for
     * (`MDC_DEV_SPEC_PROFILE_*` data type). Codes per the -104xx specializations; see
     * src/health/README.md's HDP table. A sink is registered for each, so any of these
     * devices can connect; only the single-value ones in [MAP] are auto-mapped to FHIR.
     */
    enum class Specialization(val dataType: Int) {
        PULSE_OXIMETER(0x1004), ECG(0x1006), BLOOD_PRESSURE(0x1007), THERMOMETER(0x1008),
        WEIGHT_SCALE(0x100F), GLUCOSE(0x1011), PEAK_FLOW(0x1015);

        companion object { fun of(dataType: Int) = entries.firstOrNull { it.dataType == dataType } }
    }

    fun apduType(apdu: ByteArray): Int =
        if (apdu.size >= 2) ((apdu[0].toInt() and 0xFF) shl 8) or (apdu[1].toInt() and 0xFF) else 0

    /**
     * Handle one inbound APDU. `spec` is the specialization the channel was opened for
     * (from the sink data type), used to label extracted measurements.
     */
    fun handle(apdu: ByteArray, spec: Specialization?, nowMillis: Long): Result = when (apduType(apdu)) {
        AARQ -> Result(ASSOCIATION_RESPONSE_ACCEPTED, emptyList(), false)
        RLRQ -> Result(RELEASE_RESPONSE, emptyList(), true)
        ABRT -> Result(null, emptyList(), true)
        PRST -> handlePresentation(apdu, spec, nowMillis)
        else -> Result(null, emptyList(), false)
    }

    // --- data APDUs ----------------------------------------------------------

    // PRST layout: E7 00 | len(2) | octet-string-len(2) | invoke-id(2) | data.choice(2)
    //              | choice.len(2) | ...event report... | obj-handle(2) | event-time(4)
    //              | event-type(2) | event-info-len(2) | event-info(...)
    private fun handlePresentation(apdu: ByteArray, spec: Specialization?, now: Long): Result {
        if (apdu.size < 20) return Result(null, emptyList(), false)
        val invokeId = u16(apdu, 6)
        val eventType = u16(apdu, 18)
        return when (eventType) {
            MDC_NOTI_CONFIG -> {
                // Agent announced its configuration; accept it, echoing config-report-id
                // (first 2 bytes of event-info) so the agent proceeds to send data.
                val configReportId = if (apdu.size >= 24) u16(apdu, 22) else 0
                Result(configAcceptResponse(invokeId, configReportId), emptyList(), false)
            }
            else -> {
                // A measurement/scan report: acknowledge, then best-effort extract.
                val readings = spec?.let { parseMeasurement(apdu, it, now) } ?: emptyList()
                Result(dataAckResponse(invokeId), readings, false)
            }
        }
    }

    // Best-effort: scan the event-info for the first FLOAT-Type that decodes to a finite,
    // in-range value for the specialization, and map it to FHIR. NOT hardware-validated.
    private fun parseMeasurement(apdu: ByteArray, spec: Specialization, now: Long): List<Reading> {
        val m = MAP[spec] ?: return emptyList()
        var off = 20                                   // start of event report body
        while (off + 4 <= apdu.size) {
            val v = floatType(apdu, off)
            if (v.isFinite() && v >= m.min && v <= m.max) {
                return listOf(Reading(m.label, v, m.unit,
                    FhirObs.observation(m.loinc, m.display, m.category, v, m.integral, m.unit, m.ucum, now, "Bluetooth HDP")))
            }
            off += 1
        }
        return emptyList()
    }

    // --- IEEE 11073 FLOAT-Type: 8-bit signed exponent + 24-bit signed mantissa (BE) ---
    // (Same maths as the BLE float32; the wire order in MDER is big-endian.)
    private fun floatType(b: ByteArray, off: Int): Double {
        if (off + 4 > b.size) return Double.NaN
        val raw = (u8(b, off) shl 24) or (u8(b, off + 1) shl 16) or (u8(b, off + 2) shl 8) or u8(b, off + 3)
        var mantissa = raw and 0x00FFFFFF
        val exponent = (raw shr 24).toByte().toInt()
        if (mantissa >= 0x800000) mantissa -= 0x01000000
        val special = raw and 0x00FFFFFF
        if (special == 0x7FFFFF || special == 0x800000 || special == 0x7FFFFE || special == 0x800002)
            return Double.NaN                          // NaN / NRes / +INF / -INF
        return mantissa * 10.0.pow(exponent)
    }

    private fun u8(b: ByteArray, i: Int) = b[i].toInt() and 0xFF
    private fun u16(b: ByteArray, i: Int) = (u8(b, i) shl 8) or u8(b, i + 1)

    // --- canned / patched responses (IEEE 11073-20601; verify against hardware) ------

    // Association response, result = accepted-unknown-config (0x0003): tells the agent
    // to send its configuration next. 44-byte data-proto-info for MDER, manager role.
    private val ASSOCIATION_RESPONSE_ACCEPTED: ByteArray = byteArrayOfInts(
        0xE3, 0x00, 0x00, 0x2C, 0x00, 0x03, 0x50, 0x79, 0x00, 0x26,
        0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)

    private val RELEASE_RESPONSE: ByteArray = byteArrayOfInts(0xE5, 0x00, 0x00, 0x02, 0x00, 0x00)

    // rors-cmip-confirmed-event-report (0x0200) accepting a config report, echoing the
    // agent's invoke-id and config-report-id, config-result = accepted-config (0x0000).
    private fun configAcceptResponse(invokeId: Int, configReportId: Int): ByteArray = byteArrayOfInts(
        0xE7, 0x00, 0x00, 0x16, 0x00, 0x14,
        invokeId shr 8, invokeId and 0xFF,             // invoke-id (echo)
        0x02, 0x01,                                    // rors-cmip-confirmed-event-report
        0x00, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0D, 0x1C,                                    // event-type = MDC_NOTI_CONFIG
        0x00, 0x04,                                    // event-reply-info length
        configReportId shr 8, configReportId and 0xFF, // config-report-id (echo)
        0x00, 0x00)                                    // config-result = accepted-config

    // rors-cmip-confirmed-event-report acknowledging a measurement, echoing invoke-id.
    private fun dataAckResponse(invokeId: Int): ByteArray = byteArrayOfInts(
        0xE7, 0x00, 0x00, 0x12, 0x00, 0x10,
        invokeId shr 8, invokeId and 0xFF,             // invoke-id (echo)
        0x02, 0x01,                                    // rors-cmip-confirmed-event-report
        0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0D, 0x08,                                    // event-type = MDC_NOTI_SCAN_REPORT_FIXED
        0x00, 0x00)                                    // event-reply-info length = 0

    private fun byteArrayOfInts(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    // --- MDC specialization → FHIR coding (single-value primary metric) --------------
    const val MDC_NOTI_CONFIG = 0x0D1C

    private data class Coding(
        val loinc: String, val display: String, val category: String,
        val unit: String, val ucum: String, val integral: Boolean,
        val min: Double, val max: Double, val label: String,
    )

    // Only single-value specializations with a confident LOINC + UCUM are auto-mapped.
    // ECG (waveform), blood pressure (compound), and peak flow (no confident code yet)
    // still connect and are acknowledged, but their reports aren't mapped here — the
    // same "defer rather than mismap" honesty rule the vendor clients follow.
    private val MAP: Map<Specialization, Coding> = mapOf(
        Specialization.THERMOMETER to Coding("8310-5", "Body temperature", "vital-signs", "°C", "Cel", false, 25.0, 45.0, "Temperature"),
        Specialization.PULSE_OXIMETER to Coding("59408-5", "Oxygen saturation", "vital-signs", "%", "%", true, 50.0, 100.0, "SpO₂"),
        Specialization.WEIGHT_SCALE to Coding("29463-7", "Body weight", "vital-signs", "kg", "kg", false, 1.0, 400.0, "Weight"),
        Specialization.GLUCOSE to Coding("2339-0", "Glucose in Blood", "laboratory", "mg/dL", "mg/dL", false, 10.0, 1000.0, "Glucose"),
    )
}
