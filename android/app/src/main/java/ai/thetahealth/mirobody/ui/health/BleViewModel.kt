package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.data.health.ble.BleHealthController
import androidx.lifecycle.ViewModel

/**
 * Thin adapter over [BleHealthController] for the BLE device dialog. The controller
 * lives in the AppContainer (a live BLE connection should outlive dialog
 * recomposition), so this just forwards its state and actions. Mirrors the other
 * settings ViewModels.
 */
class BleViewModel(private val controller: BleHealthController) : ViewModel() {
    val state = controller.state

    fun startScan() = controller.startScan()
    fun stopScan() = controller.stopScan()
    fun connect(address: String) = controller.connect(address)
    fun disconnect() = controller.disconnect()
}
