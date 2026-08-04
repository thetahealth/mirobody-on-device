package ai.thetahealth.mirobody.data.health.ble

import ai.thetahealth.mirobody.data.health.HealthApi
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.util.ArrayDeque

/**
 * Direct Bluetooth Low Energy (BLE GATT) health-sensor ingestion for Android — the
 * mobile-native counterpart of the desktop Qt `BleHealth`. Scans for standard-profile
 * sensors (HR strap, BP cuff, thermometer), connects, subscribes to each supported
 * measurement characteristic, decodes it via [GattHealthCodec], and POSTs the FHIR
 * Observation to the server's /fhir endpoint — the same ingestion path Health Connect
 * uses. See src/health/README.md ("Direct Bluetooth devices").
 *
 * On phones this is the fallback for standard medical sensors with no companion app;
 * most devices route through Health Connect instead (HealthConnectSource).
 *
 * Permission *requesting* is the UI's job (BLUETOOTH_SCAN/CONNECT on API 31+,
 * ACCESS_FINE_LOCATION below); every entry point no-ops cleanly on SecurityException
 * if a grant is missing.
 */
@SuppressLint("MissingPermission")
class BleHealthController(
    private val context: Context,
    private val api: HealthApi,
    private val scope: CoroutineScope,
) {
    data class Device(val name: String, val address: String, val supported: Boolean)

    data class State(
        val scanning: Boolean = false,
        val connected: Boolean = false,
        val status: String = "",
        val devices: List<Device> = emptyList(),
        val posted: Int = 0,
        val failed: Int = 0,
        val lastReading: String? = null,
    )

    private val _state = MutableStateFlow(State())
    val state = _state.asStateFlow()

    private val manager: BluetoothManager? =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager

    private val found = LinkedHashMap<String, android.bluetooth.BluetoothDevice>()
    private var scanCallback: ScanCallback? = null
    private var scanTimeout: Job? = null
    private var gatt: BluetoothGatt? = null

    // Android allows only one outstanding GATT operation; CCCD writes are queued and
    // drained one-per-onDescriptorWrite.
    private val opQueue = ArrayDeque<() -> Unit>()
    private var opInFlight = false

    // --- scan ----------------------------------------------------------------

    fun startScan() {
        val scanner = manager?.adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner
        if (scanner == null) {
            _state.update { it.copy(status = "Bluetooth is off or unavailable") }
            return
        }
        if (_state.value.scanning) return
        found.clear()
        _state.update { it.copy(scanning = true, status = "Scanning…", devices = emptyList()) }

        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) = onFound(result)
            override fun onScanFailed(errorCode: Int) {
                _state.update { it.copy(scanning = false, status = "Scan failed ($errorCode)") }
            }
        }
        scanCallback = cb
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            // Unfiltered scan (adverts often omit service UUIDs); filtered in onFound.
            scanner.startScan(null, settings, cb)
        } catch (e: SecurityException) {
            _state.update { it.copy(scanning = false, status = "Bluetooth permission denied") }
            return
        }
        scanTimeout = scope.launch {
            delay(SCAN_MILLIS)
            stopScan()
        }
    }

    fun stopScan() {
        scanTimeout?.cancel(); scanTimeout = null
        val scanner = manager?.adapter?.bluetoothLeScanner
        scanCallback?.let { cb -> runCatching { scanner?.stopScan(cb) } }
        scanCallback = null
        _state.update {
            if (!it.scanning) it
            else it.copy(scanning = false, status = if (it.devices.isEmpty()) "No devices found" else "Scan complete")
        }
    }

    private fun onFound(result: ScanResult) {
        val device = result.device
        val advUuids = result.scanRecord?.serviceUuids
        val supported = advUuids?.any { it.uuid in GattHealthCodec.SUPPORTED_SERVICES } == true
        val name = result.scanRecord?.deviceName ?: runCatching { device.name }.getOrNull()
        // Interesting if it advertises a supported service, or (fallback) has a name.
        if (!supported && name.isNullOrBlank()) return
        if (found.put(device.address, device) != null) return   // already listed

        val entry = Device(name = name ?: device.address, address = device.address, supported = supported)
        _state.update { it.copy(devices = it.devices + entry) }
    }

    // --- connect -------------------------------------------------------------

    fun connect(address: String) {
        stopScan()
        teardownGatt()
        val device = found[address] ?: runCatching { manager?.adapter?.getRemoteDevice(address) }.getOrNull()
        if (device == null) {
            _state.update { it.copy(status = "Device not available") }
            return
        }
        _state.update { it.copy(status = "Connecting…", posted = 0, failed = 0, lastReading = null) }
        gatt = try {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            _state.update { it.copy(status = "Bluetooth permission denied") }
            null
        }
    }

    fun disconnect() {
        teardownGatt()
        _state.update { it.copy(connected = false, status = "Disconnected") }
    }

    private fun teardownGatt() {
        opQueue.clear(); opInFlight = false
        gatt?.let { g -> runCatching { g.disconnect() }; runCatching { g.close() } }
        gatt = null
    }

    private val gattCallback = object : android.bluetooth.BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _state.update { it.copy(status = "Discovering services…") }
                    runCatching { g.discoverServices() }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _state.update { it.copy(connected = false, status = "Disconnected") }
                    runCatching { g.close() }
                    if (gatt === g) gatt = null
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            var any = false
            for (service in g.services) {
                if (service.uuid !in GattHealthCodec.SUPPORTED_SERVICES) continue
                for (ch in service.characteristics) {
                    if (ch.uuid !in GattHealthCodec.SUPPORTED_MEASUREMENTS) continue
                    any = true
                    enqueue { enableNotification(g, ch) }
                }
            }
            if (!any) {
                _state.update { it.copy(status = "No supported health service on this device") }
            } else {
                _state.update { it.copy(connected = true, status = "Streaming readings…") }
                drainQueue()
            }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val value = ch.value ?: return
            handleReadings(ch.uuid, value)
        }

        // API 33+ overload; delegates to the same handler (bytes come pre-read).
        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray,
        ) {
            handleReadings(ch.uuid, value)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            opInFlight = false
            drainQueue()
        }
    }

    @Suppress("DEPRECATION")
    private fun enableNotification(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
        runCatching {
            g.setCharacteristicNotification(ch, true)
            val cccd = ch.getDescriptor(GattHealthCodec.CCCD) ?: return
            val notify = ch.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0
            val value = if (notify) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        else BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
            cccd.value = value
            g.writeDescriptor(cccd)
        }.onFailure { opInFlight = false }
    }

    private fun handleReadings(charUuid: java.util.UUID, value: ByteArray) {
        val readings = GattHealthCodec.decode(charUuid, value, System.currentTimeMillis())
        for (r in readings) {
            _state.update { it.copy(lastReading = "${r.label}: ${round1(r.value)} ${r.unit}") }
            post(r.observation)
        }
    }

    private fun post(observation: kotlinx.serialization.json.JsonObject) {
        scope.launch {
            val ok = runCatching { api.postObservation(observation).isSuccessful }
                .getOrElse { t -> Log.w(TAG, "post Observation failed", t); false }
            _state.update { if (ok) it.copy(posted = it.posted + 1) else it.copy(failed = it.failed + 1) }
        }
    }

    // --- GATT op queue -------------------------------------------------------

    private fun enqueue(op: () -> Unit) { opQueue.add(op) }

    private fun drainQueue() {
        if (opInFlight) return
        val op = opQueue.poll() ?: return
        opInFlight = true
        op()
    }

    private fun round1(v: Double): String {
        val r = Math.round(v * 10.0) / 10.0
        return if (r == r.toLong().toDouble()) r.toLong().toString() else r.toString()
    }

    private companion object {
        const val TAG = "BleHealthController"
        const val SCAN_MILLIS = 8_000L
    }
}
