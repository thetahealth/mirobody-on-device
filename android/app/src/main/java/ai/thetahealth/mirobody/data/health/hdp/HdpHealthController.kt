package ai.thetahealth.mirobody.data.health.hdp

import ai.thetahealth.mirobody.data.health.HealthApi
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothHealth
import android.bluetooth.BluetoothHealthAppConfiguration
import android.bluetooth.BluetoothHealthCallback
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * Classic-Bluetooth **HDP (Health Device Profile)** ingestion — the legacy path for
 * IEEE 11073 medical devices, kept so old hardware can still reach the health record.
 * It works **only on Android 9 and below (API ≤ 28)**: the `BluetoothHealth` profile
 * was deprecated in API 29 and has no OS runtime support after that. Modern medical
 * sensors are BLE GATT and go through `ble.BleHealthController` instead.
 *
 * We register a **sink** app-configuration per supported device specialization and wait
 * for the device to open a channel (HDP agents typically initiate). On connection the OS
 * hands us a raw file descriptor carrying the IEEE 11073-20601 APDU stream, which
 * [Ieee11073Agent] drives (association → config → data), extracting measurements that we
 * POST as FHIR Observations — the same endpoint the BLE and platform paths use.
 *
 * This is a **basic, experimental** implementation (see [Ieee11073Agent]); the handshake
 * is complete, the measurement parse is best-effort and not hardware-validated.
 */
@Suppress("DEPRECATION")
@SuppressLint("MissingPermission")
class HdpHealthController(
    private val context: Context,
    private val api: HealthApi,
    private val scope: CoroutineScope,
) {
    data class State(
        val supported: Boolean,
        val listening: Boolean = false,
        val status: String = "",
        val connectedDevice: String? = null,
        val posted: Int = 0,
        val failed: Int = 0,
        val lastReading: String? = null,
    )

    /** HDP only exists on Android ≤ 9; above that BluetoothHealth is inert. */
    val isSupported: Boolean = Build.VERSION.SDK_INT <= 28

    private val _state = MutableStateFlow(State(supported = isSupported))
    val state = _state.asStateFlow()

    private val manager: BluetoothManager? =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private var health: BluetoothHealth? = null
    private val configs = HashMap<BluetoothHealthAppConfiguration, Ieee11073Agent.Specialization>()

    /** Register the sink configs and wait for a device to connect. */
    fun startListening() {
        if (!isSupported) {
            _state.update { it.copy(status = "HDP is only available on Android 9 and below") }
            return
        }
        val adapter = manager?.adapter?.takeIf { it.isEnabled }
        if (adapter == null) {
            _state.update { it.copy(status = "Bluetooth is off or unavailable") }
            return
        }
        _state.update { it.copy(listening = true, status = "Registering HDP sink…") }
        adapter.getProfileProxy(context, serviceListener, BluetoothProfile.HEALTH)
    }

    fun stop() {
        val h = health
        if (h != null) {
            for (config in configs.keys) runCatching { h.unregisterAppConfiguration(config) }
            manager?.adapter?.closeProfileProxy(BluetoothProfile.HEALTH, h)
        }
        configs.clear()
        health = null
        _state.update { it.copy(listening = false, connectedDevice = null, status = "Stopped") }
    }

    private val serviceListener = object : BluetoothProfile.ServiceListener {
        override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
            if (profile != BluetoothProfile.HEALTH) return
            val h = proxy as BluetoothHealth
            health = h
            for (spec in Ieee11073Agent.Specialization.entries) {
                h.registerSinkAppConfiguration("mirobody-${spec.name.lowercase()}", spec.dataType, healthCallback)
            }
            _state.update { it.copy(status = "Waiting for an HDP device to connect…") }
        }

        override fun onServiceDisconnected(profile: Int) {
            if (profile == BluetoothProfile.HEALTH) health = null
        }
    }

    private val healthCallback = object : BluetoothHealthCallback() {
        override fun onHealthAppConfigurationStatusChange(config: BluetoothHealthAppConfiguration, status: Int) {
            if (status == BluetoothHealth.APP_CONFIG_REGISTRATION_SUCCESS) {
                Ieee11073Agent.Specialization.of(config.dataType)?.let { configs[config] = it }
            }
        }

        override fun onHealthChannelStateChange(
            config: BluetoothHealthAppConfiguration,
            device: BluetoothDevice,
            prevState: Int,
            newState: Int,
            fd: ParcelFileDescriptor?,
            channelId: Int,
        ) {
            when (newState) {
                BluetoothHealth.STATE_CHANNEL_CONNECTED -> {
                    if (fd == null) return
                    val spec = configs[config]
                    _state.update {
                        it.copy(connectedDevice = deviceName(device), status = "Connected — reading…")
                    }
                    scope.launch(Dispatchers.IO) { readLoop(fd, spec) }
                }
                BluetoothHealth.STATE_CHANNEL_DISCONNECTED -> {
                    _state.update { it.copy(connectedDevice = null, status = "Device disconnected") }
                }
            }
        }
    }

    // Drive the 11073 exchange over the channel FD: read an APDU, let the agent produce
    // the reply + any readings, write the reply back, POST the readings.
    private fun readLoop(fd: ParcelFileDescriptor, spec: Ieee11073Agent.Specialization?) {
        val input = FileInputStream(fd.fileDescriptor)
        val output = FileOutputStream(fd.fileDescriptor)
        val buf = ByteArray(4096)
        try {
            while (true) {
                val n = input.read(buf)
                if (n <= 0) break
                val apdu = buf.copyOf(n)
                val result = Ieee11073Agent.handle(apdu, spec, System.currentTimeMillis())
                result.reply?.let { output.write(it); output.flush() }
                for (r in result.readings) {
                    _state.update { it.copy(lastReading = "${r.label}: ${round1(r.value)} ${r.unit}") }
                    post(r.observation)
                }
                if (result.released) break
            }
        } catch (t: Throwable) {
            Log.w(TAG, "HDP channel read ended", t)
        } finally {
            runCatching { fd.close() }
        }
    }

    private fun post(observation: JsonObject) {
        scope.launch {
            val ok = runCatching { api.postObservation(observation).isSuccessful }
                .getOrElse { t -> Log.w(TAG, "post Observation failed", t); false }
            _state.update { if (ok) it.copy(posted = it.posted + 1) else it.copy(failed = it.failed + 1) }
        }
    }

    private fun deviceName(device: BluetoothDevice): String =
        runCatching { device.name }.getOrNull() ?: device.address

    private fun round1(v: Double): String {
        val r = Math.round(v * 10.0) / 10.0
        return if (r == r.toLong().toDouble()) r.toLong().toString() else r.toString()
    }

    private companion object { const val TAG = "HdpHealthController" }
}
