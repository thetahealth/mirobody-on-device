package ai.thetahealth.mirobody.data.health

/**
 * A single on-device health reading, normalized across the platform sources
 * (Health Connect on GMS devices, HMS Health Kit on Huawei). Source-specific
 * records are mapped into this shape, then [FhirObservation] turns each one into a
 * FHIR R4 Observation that is POSTed to the embedded server's /fhir endpoint.
 *
 * `value` is the numeric reading in [HealthMetric.ucumUnit]. Instantaneous
 * readings (heart rate, weight) set `start == end`; interval readings (a day's
 * steps, a sleep session) span [start, end). Times are epoch milliseconds (UTC).
 */
data class HealthSample(
    val metric: HealthMetric,
    val value: Double,
    val startMillis: Long,
    val endMillis: Long,
    val source: String,
)

/**
 * The metrics we ingest, with the FHIR coding each maps to. Kept deliberately
 * small (the four domains both Health Connect and HMS expose cleanly); extend by
 * adding an entry plus the matching read in each [HealthSource].
 *
 * `category` is the FHIR observation-category token; `loinc`/`display` identify
 * the measurement; `unit`/`ucumUnit` are the human and UCUM units.
 */
enum class HealthMetric(
    val category: String,
    val loinc: String,
    val display: String,
    val unit: String,
    val ucumUnit: String,
) {
    STEPS("activity", "41950-7", "Number of steps in 24 hour Measured", "steps", "{steps}"),
    HEART_RATE("vital-signs", "8867-4", "Heart rate", "beats/minute", "/min"),
    SLEEP("activity", "93832-4", "Sleep duration", "min", "min"),
    WEIGHT("vital-signs", "29463-7", "Body weight", "kg", "kg"),
}
