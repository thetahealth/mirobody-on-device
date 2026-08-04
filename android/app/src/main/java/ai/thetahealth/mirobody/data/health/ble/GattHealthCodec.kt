package ai.thetahealth.mirobody.data.health.ble

import ai.thetahealth.mirobody.data.health.FhirObs
import kotlinx.serialization.json.JsonObject
import java.util.UUID
import kotlin.math.pow

/**
 * Decodes standard SIG GATT health measurements into FHIR R4 `Observation` bodies —
 * the Android counterpart of the desktop Qt `blehealth.cpp` decoders, producing the
 * same LOINC/UCUM shapes the server's /fhir endpoint stores (and matching
 * [ai.thetahealth.mirobody.data.health.FhirObservation]).
 *
 * Supported today: Heart Rate `0x2A37`, Blood Pressure `0x2A35`, Temperature
 * `0x2A1C`. Adding one is a branch in [decode]. Consumer watches/rings use
 * proprietary/encrypted GATT and are NOT decodable here — see src/health/README.md
 * ("Direct Bluetooth devices").
 */
object GattHealthCodec {

    /** One decoded reading: `value` in `unit` for the UI, plus the FHIR body to POST. */
    data class Reading(
        val label: String,
        val value: Double,
        val unit: String,
        val observation: JsonObject,
    )

    // Standard 16-bit UUIDs expanded onto the Bluetooth base UUID.
    private fun uuid16(v: Int): UUID =
        UUID.fromString(String.format("0000%04x-0000-1000-8000-00805f9b34fb", v))

    val SERVICE_HEART_RATE: UUID = uuid16(0x180D)
    val SERVICE_BLOOD_PRESSURE: UUID = uuid16(0x1810)
    val SERVICE_THERMOMETER: UUID = uuid16(0x1809)
    val SUPPORTED_SERVICES: Set<UUID> =
        setOf(SERVICE_HEART_RATE, SERVICE_BLOOD_PRESSURE, SERVICE_THERMOMETER)

    val CHR_HEART_RATE: UUID = uuid16(0x2A37)   // Heart Rate Measurement
    val CHR_BLOOD_PRESSURE: UUID = uuid16(0x2A35) // Blood Pressure Measurement
    val CHR_TEMPERATURE: UUID = uuid16(0x2A1C)  // Temperature Measurement
    val SUPPORTED_MEASUREMENTS: Set<UUID> =
        setOf(CHR_HEART_RATE, CHR_BLOOD_PRESSURE, CHR_TEMPERATURE)

    /** Client Characteristic Configuration Descriptor — enables notify/indicate. */
    val CCCD: UUID = uuid16(0x2902)

    /** Decode one measurement notification into zero or more readings. */
    fun decode(charUuid: UUID, value: ByteArray, nowMillis: Long): List<Reading> = when (charUuid) {
        CHR_HEART_RATE -> decodeHeartRate(value, nowMillis)
        CHR_BLOOD_PRESSURE -> decodeBloodPressure(value, nowMillis)
        CHR_TEMPERATURE -> decodeTemperature(value, nowMillis)
        else -> emptyList()
    }

    // --- decoders ------------------------------------------------------------

    // Heart Rate Measurement (0x2A37): flags byte, then HR as uint8 or uint16 (LE).
    private fun decodeHeartRate(v: ByteArray, now: Long): List<Reading> {
        if (v.size < 2) return emptyList()
        val hr = if (v.u8(0) and 0x01 != 0) {                 // bit0: 16-bit value format
            if (v.size < 3) return emptyList()
            (v.u8(1) or (v.u8(2) shl 8)).toDouble()
        } else {
            v.u8(1).toDouble()
        }
        return listOf(
            Reading(
                "Heart rate", hr, "bpm",
                observation("8867-4", "Heart rate", "vital-signs", hr, true, "beats/minute", "/min", now),
            ),
        )
    }

    // Blood Pressure Measurement (0x2A35): flags, then systolic/diastolic/MAP as SFLOAT.
    private fun decodeBloodPressure(v: ByteArray, now: Long): List<Reading> {
        if (v.size < 7) return emptyList()
        val kpa = v.u8(0) and 0x01 != 0                        // bit0: 0 = mmHg, 1 = kPa
        val unit = if (kpa) "kPa" else "mmHg"
        val ucum = if (kpa) "kPa" else "mm[Hg]"
        val out = ArrayList<Reading>(3)
        fun add(off: Int, loinc: String, display: String, label: String) {
            val n = sfloat(v.u8(off) or (v.u8(off + 1) shl 8))
            if (!n.isNaN()) {
                out += Reading(label, n, unit, observation(loinc, display, "vital-signs", n, true, unit, ucum, now))
            }
        }
        add(1, "8480-6", "Systolic blood pressure", "Systolic")
        add(3, "8462-4", "Diastolic blood pressure", "Diastolic")
        add(5, "8478-0", "Mean blood pressure", "Mean arterial")
        return out
    }

    // Temperature Measurement (0x2A1C): flags, then temperature as 32-bit FLOAT.
    private fun decodeTemperature(v: ByteArray, now: Long): List<Reading> {
        if (v.size < 5) return emptyList()
        val fahrenheit = v.u8(0) and 0x01 != 0                 // bit0: 0 = Celsius, 1 = Fahrenheit
        val raw = v.u8(1) or (v.u8(2) shl 8) or (v.u8(3) shl 16) or (v.u8(4) shl 24)
        val t = float32(raw)
        if (t.isNaN()) return emptyList()
        return listOf(
            Reading(
                "Temperature", t, if (fahrenheit) "°F" else "°C",
                observation(
                    "8310-5", "Body temperature", "vital-signs", t, false,
                    if (fahrenheit) "F" else "Cel", if (fahrenheit) "[degF]" else "Cel", now,
                ),
            ),
        )
    }

    // --- IEEE-11073 floats ---------------------------------------------------

    // 16-bit SFLOAT: 4-bit signed exponent + 12-bit signed mantissa; special codes -> NaN.
    private fun sfloat(raw: Int): Double {
        val m = raw and 0x0FFF
        if (m == 0x07FF || m == 0x0800 || m == 0x0801 || m == 0x0802 || m == 0x07FE) return Double.NaN
        var mantissa = m
        var exponent = raw shr 12
        if (exponent >= 0x0008) exponent -= 16                 // signed 4-bit
        if (mantissa >= 0x0800) mantissa -= 4096               // signed 12-bit
        return mantissa * 10.0.pow(exponent)
    }

    // 32-bit FLOAT: 8-bit signed exponent + 24-bit signed mantissa.
    private fun float32(raw: Int): Double {
        var mantissa = raw and 0x00FFFFFF
        val exponent = (raw shr 24).toByte().toInt()           // signed 8-bit
        if (mantissa >= 0x800000) mantissa -= 0x01000000       // signed 24-bit
        return mantissa * 10.0.pow(exponent)
    }

    private fun ByteArray.u8(i: Int): Int = this[i].toInt() and 0xFF

    // FHIR Observation via the shared builder; the provenance marks the BLE transport.
    private fun observation(
        loinc: String, display: String, category: String,
        value: Double, integral: Boolean, unit: String, ucum: String, nowMillis: Long,
    ): JsonObject = FhirObs.observation(
        loinc, display, category, value, integral, unit, ucum, nowMillis, "Bluetooth LE")
}
