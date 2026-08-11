package ai.thetahealth.mirobody.ui.chat

import android.content.Context
import android.os.Build
import android.widget.Toast
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.ClipboardManager
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import ai.thetahealth.mirobody.R

/**
 * Copying a message, from either affordance: the footer's copy button or a long press on
 * the bubble.
 *
 * Ported from HarmonyOS `Index.copyText`, including the part that is easy to miss — the
 * two entry points are NOT the same interaction. A button answers a tap by *looking*
 * pressed, so it needs no further acknowledgement; a long press has no affordance at all
 * and a threshold the user cannot see, so a tick is what tells them the gesture landed.
 * Hence [haptic] rather than always buzzing.
 *
 * The confirmation differs from Harmony's on purpose. Harmony toasts on every copy;
 * **Android 13 (API 33) shows its own system confirmation** whenever an app writes to the
 * clipboard, and Google's guidance is not to add a second one. So ours only appears below
 * that, where nothing else would tell the user anything happened.
 */
internal class CopyAction(
    private val clipboard: ClipboardManager,
    private val context: Context,
    private val copiedLabel: String,
) {
    /**
     * @param haptic true for a gesture with no visual affordance (the long press); the
     *   footer button passes false. Callers whose platform already buzzed — a `View`'s
     *   own `performLongClick`, which fires `HapticFeedbackConstants.LONG_PRESS` before
     *   invoking its listener — should also pass false, or the press double-buzzes.
     */
    fun copy(text: String, haptic: Boolean, onHaptic: () -> Unit = {}) {
        if (text.isEmpty()) return
        clipboard.setText(AnnotatedString(text))
        if (haptic) onHaptic()
        // API 33+ posts its own "Copied" UI; a second one would stutter.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(context, copiedLabel, Toast.LENGTH_SHORT).show()
        }
    }
}

@Composable
internal fun rememberCopyAction(): CopyAction {
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current
    val label = androidx.compose.ui.res.stringResource(R.string.chat_copied)
    return remember(clipboard, context, label) { CopyAction(clipboard, context, label) }
}
