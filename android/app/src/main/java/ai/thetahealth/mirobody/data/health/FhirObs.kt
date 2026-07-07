package ai.thetahealth.mirobody.data.health

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import java.time.Instant
import java.time.format.DateTimeFormatter
import kotlin.math.roundToLong

/**
 * Shared builder for a FHIR R4 `Observation` from a single decoded reading — the one
 * place the Bluetooth paths (BLE GATT in `ble.GattHealthCodec`, classic HDP in
 * `hdp.Ieee11073Agent`) agree on the wire shape the server's /fhir endpoint stores.
 * Matches [FhirObservation] (which maps the on-device platform samples); `provenance`
 * records which transport produced it.
 */
object FhirObs {

    private fun iso(millis: Long): String =
        DateTimeFormatter.ISO_INSTANT.format(Instant.ofEpochMilli(millis))

    fun observation(
        loinc: String,
        display: String,
        category: String,
        value: Double,
        integral: Boolean,
        unit: String,
        ucum: String,
        nowMillis: Long,
        provenance: String,
    ): JsonObject = buildJsonObject {
        put("resourceType", "Observation")
        put("status", "final")
        putJsonArray("category") {
            addJsonObject {
                putJsonArray("coding") {
                    addJsonObject {
                        put("system", "http://terminology.hl7.org/CodeSystem/observation-category")
                        put("code", category)
                    }
                }
            }
        }
        putJsonObject("code") {
            putJsonArray("coding") {
                addJsonObject {
                    put("system", "http://loinc.org")
                    put("code", loinc)
                    put("display", display)
                }
            }
        }
        put("effectiveDateTime", iso(nowMillis))
        putJsonObject("valueQuantity") {
            if (integral) put("value", value.roundToLong()) else put("value", value)
            put("unit", unit)
            put("system", "http://unitsofmeasure.org")
            put("code", ucum)
        }
        putJsonObject("method") {
            putJsonArray("coding") {
                addJsonObject { put("display", provenance) }
            }
        }
    }
}
