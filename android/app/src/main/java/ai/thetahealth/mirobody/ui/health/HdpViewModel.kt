package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.data.health.hdp.HdpHealthController
import androidx.lifecycle.ViewModel

/**
 * Thin adapter over [HdpHealthController] for the legacy HDP dialog. The controller is
 * an AppContainer singleton (a live channel should outlive dialog recomposition), so
 * this just forwards its state and actions.
 */
class HdpViewModel(private val controller: HdpHealthController) : ViewModel() {
    val state = controller.state

    fun start() = controller.startListening()
    fun stop() = controller.stop()
}
