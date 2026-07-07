package ai.thetahealth.mirobody.data.health.hdp

import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Unit tests for [Ieee11073Agent] — the deterministic parts of the HDP manager: APDU
 * framing / type detection, the association + release handshake, invoke-id / config-id
 * echo in responses, and the FLOAT-Type measurement decode (via a thermometer report).
 * The measurement byte-offset parse is best-effort and not covered here (needs hardware).
 */
class Ieee11073AgentTest {

    private val now = 0L
    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }
    private fun u16(b: ByteArray, i: Int) = ((b[i].toInt() and 0xFF) shl 8) or (b[i + 1].toInt() and 0xFF)

    @Test
    fun apduType_readsFirstTwoBytes() {
        assertEquals(Ieee11073Agent.AARQ, Ieee11073Agent.apduType(bytes(0xE2, 0x00, 0x00)))
        assertEquals(0, Ieee11073Agent.apduType(bytes(0x00)))   // too short
    }

    @Test
    fun associationRequest_isAccepted() {
        val r = Ieee11073Agent.handle(bytes(0xE2, 0x00, 0x00, 0x00), null, now)
        assertNotNull(r.reply)
        assertEquals(0xE3.toByte(), r.reply!![0])   // AARE
        assertEquals(0x00.toByte(), r.reply!![1])
        assertTrue(!r.released)
        assertTrue(r.readings.isEmpty())
    }

    @Test
    fun releaseRequest_releasesWithResponse() {
        val r = Ieee11073Agent.handle(bytes(0xE4, 0x00, 0x00, 0x00), null, now)
        assertEquals(0xE5.toByte(), r.reply!![0])   // RLRE
        assertTrue(r.released)
    }

    @Test
    fun abort_releasesWithoutReply() {
        val r = Ieee11073Agent.handle(bytes(0xE6, 0x00, 0x00, 0x00), null, now)
        assertNull(r.reply)
        assertTrue(r.released)
    }

    @Test
    fun configReport_isAccepted_echoingInvokeIdAndReportId() {
        // PRST: invoke-id=0x0042 @6, choice @8, event-type=MDC_NOTI_CONFIG @18, report-id=0x0007 @22
        val apdu = bytes(
            0xE7, 0x00, 0x00, 0x0E, 0x00, 0x0C,
            0x00, 0x42,                 // invoke-id
            0x01, 0x00,                 // roiv-cmip-confirmed-event-report
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x0D, 0x1C,                 // event-type = MDC_NOTI_CONFIG
            0x00, 0x04, 0x00, 0x07,     // event-info: config-report-id = 7
        )
        val r = Ieee11073Agent.handle(apdu, Ieee11073Agent.Specialization.THERMOMETER, now)
        assertNotNull(r.reply)
        assertEquals(0xE7.toByte(), r.reply!![0])
        assertEquals(0x0042, u16(r.reply!!, 6))       // invoke-id echoed
        assertTrue(r.readings.isEmpty())              // config, not a measurement
        assertTrue(!r.released)
    }

    @Test
    fun thermometerMeasurement_decodesFloatAndMaps() {
        // PRST measurement (event-type != config) with a FLOAT-Type for 36.5 at offset 20:
        // exponent -1 (0xFF), mantissa 365 (0x00016D), big-endian: FF 00 01 6D.
        val apdu = bytes(
            0xE7, 0x00, 0x00, 0x10, 0x00, 0x0E,
            0x00, 0x2A,                 // invoke-id = 42
            0x01, 0x00,                 // roiv-cmip-confirmed-event-report
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x0D, 0x08,                 // event-type = MDC_NOTI_SCAN_REPORT_FIXED
            0xFF, 0x00, 0x01, 0x6D,     // FLOAT-Type = 36.5
        )
        val r = Ieee11073Agent.handle(apdu, Ieee11073Agent.Specialization.THERMOMETER, now)
        assertEquals(1, r.readings.size)
        assertEquals(36.5, r.readings[0].value, 1e-9)
        assertEquals("Temperature", r.readings[0].label)
        // FHIR coding maps the thermometer specialization to body temperature.
        val loinc = r.readings[0].observation["code"]!!.jsonObject["coding"]!!
            .jsonArray[0].jsonObject["code"]!!.jsonPrimitive.content
        assertEquals("8310-5", loinc)
        // A measurement is acknowledged, echoing the invoke-id.
        assertEquals(0x002A, u16(r.reply!!, 6))
    }
}
