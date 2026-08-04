package ai.thetahealth.mirobody.data.health.ble

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.double
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Unit tests for [GattHealthCodec] — the byte→reading→FHIR decoder shared (in
 * spirit) with the Qt and iOS clients. Payloads are hand-built to the GATT /
 * IEEE-11073 spec so we exercise the flag handling, uint8/uint16 heart rate, the
 * SFLOAT (blood pressure) and FLOAT (temperature) decoders, and the emitted
 * Observation coding — all without any Bluetooth hardware.
 */
class GattHealthCodecTest {

    private val now = 0L
    private fun bytes(vararg v: Int) = v.map { it.toByte() }.toByteArray()

    // Navigate observation.code.coding[0].code
    private fun loinc(obs: JsonObject): String =
        obs["code"]!!.jsonObject["coding"]!!.jsonArray[0].jsonObject["code"]!!.jsonPrimitive.content

    private fun category(obs: JsonObject): String =
        obs["category"]!!.jsonArray[0].jsonObject["coding"]!!.jsonArray[0].jsonObject["code"]!!.jsonPrimitive.content

    private fun quantity(obs: JsonObject): JsonObject = obs["valueQuantity"]!!.jsonObject
    private fun ucum(obs: JsonObject): String = quantity(obs)["code"]!!.jsonPrimitive.content

    @Test
    fun heartRate_uint8() {
        // flags=0x00 (8-bit HR), value=72
        val r = GattHealthCodec.decode(GattHealthCodec.CHR_HEART_RATE, bytes(0x00, 0x48), now)
        assertEquals(1, r.size)
        assertEquals(72.0, r[0].value, 0.0)
        assertEquals("bpm", r[0].unit)
        assertEquals("8867-4", loinc(r[0].observation))
        assertEquals("vital-signs", category(r[0].observation))
        assertEquals("/min", ucum(r[0].observation))
        assertEquals(72, quantity(r[0].observation)["value"]!!.jsonPrimitive.int)  // integral
    }

    @Test
    fun heartRate_uint16() {
        // flags=0x01 (16-bit HR), value=320 (0x0140, little-endian)
        val r = GattHealthCodec.decode(GattHealthCodec.CHR_HEART_RATE, bytes(0x01, 0x40, 0x01), now)
        assertEquals(1, r.size)
        assertEquals(320.0, r[0].value, 0.0)
    }

    @Test
    fun heartRate_truncated_yieldsNothing() {
        assertTrue(GattHealthCodec.decode(GattHealthCodec.CHR_HEART_RATE, bytes(0x00), now).isEmpty())
    }

    @Test
    fun bloodPressure_mmHg_sfloat() {
        // flags=0x00 (mmHg); systolic=120 (0x0078), diastolic=80 (0x0050), MAP=93 (0x005D),
        // each an IEEE-11073 SFLOAT (mantissa, exponent 0), little-endian.
        val r = GattHealthCodec.decode(
            GattHealthCodec.CHR_BLOOD_PRESSURE,
            bytes(0x00, 0x78, 0x00, 0x50, 0x00, 0x5D, 0x00), now,
        )
        assertEquals(3, r.size)
        assertEquals(120.0, r[0].value, 0.0)
        assertEquals("8480-6", loinc(r[0].observation))   // systolic
        assertEquals(80.0, r[1].value, 0.0)
        assertEquals("8462-4", loinc(r[1].observation))   // diastolic
        assertEquals(93.0, r[2].value, 0.0)
        assertEquals("8478-0", loinc(r[2].observation))   // MAP
        assertEquals("mmHg", r[0].unit)
        assertEquals("mm[Hg]", ucum(r[0].observation))
    }

    @Test
    fun temperature_celsius_float() {
        // flags=0x00 (Celsius); 32-bit FLOAT for 36.5 = mantissa 365 (0x00016D),
        // exponent -1 (0xFF), little-endian: 6D 01 00 FF.
        val r = GattHealthCodec.decode(
            GattHealthCodec.CHR_TEMPERATURE, bytes(0x00, 0x6D, 0x01, 0x00, 0xFF), now,
        )
        assertEquals(1, r.size)
        assertEquals(36.5, r[0].value, 1e-9)
        assertEquals("°C", r[0].unit)
        assertEquals("8310-5", loinc(r[0].observation))
        assertEquals("Cel", ucum(r[0].observation))
        assertEquals(36.5, quantity(r[0].observation)["value"]!!.jsonPrimitive.double, 1e-9)  // non-integral
    }

    @Test
    fun temperature_fahrenheit_flag() {
        // flags=0x01 (Fahrenheit); 98 = mantissa 98 (0x000062), exponent 0 (0x00): 62 00 00 00
        val r = GattHealthCodec.decode(
            GattHealthCodec.CHR_TEMPERATURE, bytes(0x01, 0x62, 0x00, 0x00, 0x00), now,
        )
        assertEquals(1, r.size)
        assertEquals(98.0, r[0].value, 0.0)
        assertEquals("°F", r[0].unit)
        assertEquals("[degF]", ucum(r[0].observation))
    }

    @Test
    fun unsupportedCharacteristic_yieldsNothing() {
        // The CCCD UUID is not a measurement characteristic → decode returns nothing.
        val r = GattHealthCodec.decode(GattHealthCodec.CCCD, bytes(0x00, 0x01), now)
        assertTrue(r.isEmpty())
    }
}
