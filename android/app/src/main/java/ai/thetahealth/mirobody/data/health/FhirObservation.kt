package ai.thetahealth.mirobody.data.health

import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import java.time.Instant
import java.time.format.DateTimeFormatter

/**
 * Turns a normalized [HealthSample] into a FHIR R4 `Observation` resource, the
 * shape the server's /fhir endpoint validates and stores (src/fhir, whose write
 * model mirrors Android Health Connect). The server assigns the id and meta, so
 * we send only the clinical content.
 */
object FhirObservation {

    /** ISO-8601 UTC instant, e.g. 2026-06-23T14:00:00Z. */
    private fun iso(millis: Long): String =
        DateTimeFormatter.ISO_INSTANT.format(Instant.ofEpochMilli(millis))

    fun from(s: HealthSample) = buildJsonObject {
        put("resourceType", "Observation")
        put("status", "final")
        putJsonArray("category") {
            addJsonObject {
                putJsonArray("coding") {
                    addJsonObject {
                        put("system", "http://terminology.hl7.org/CodeSystem/observation-category")
                        put("code", s.metric.category)
                    }
                }
            }
        }
        putJsonObject("code") {
            putJsonArray("coding") {
                addJsonObject {
                    put("system", "http://loinc.org")
                    put("code", s.metric.loinc)
                    put("display", s.metric.display)
                }
            }
        }
        // Instantaneous samples carry a single effectiveDateTime; interval samples
        // (a day's steps, a sleep session) carry an effectivePeriod.
        if (s.startMillis == s.endMillis) {
            put("effectiveDateTime", iso(s.startMillis))
        } else {
            putJsonObject("effectivePeriod") {
                put("start", iso(s.startMillis))
                put("end", iso(s.endMillis))
            }
        }
        putJsonObject("valueQuantity") {
            put("value", s.value)
            put("unit", s.metric.unit)
            put("system", "http://unitsofmeasure.org")
            put("code", s.metric.ucumUnit)
        }
        // Record the on-device origin (Health Connect / HMS) for provenance.
        putJsonObject("method") {
            putJsonArray("coding") {
                addJsonObject { put("display", s.source) }
            }
        }
    }
}
