package ai.thetahealth.mirobody.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R

/**
 * A dialog's title row: the title on the leading edge, a **✕ that dismisses** on the
 * trailing one.
 *
 * Material3's `AlertDialog` has no slot for a close affordance, so every dialog that
 * wanted one was either growing its own header or doing without — which is how the app
 * ended up with the full-screen dialogs (image viewer, care circle, EHR, vendors) all
 * carrying a ✕ and every `AlertDialog` carrying none. Dropping this into the `title` slot
 * is what makes the two kinds agree.
 *
 * [onClose] is deliberately not "just dismiss": several dialogs have to unwind something
 * on the way out (stop a BLE scan, stop an HDP listener), and the ✕ must do exactly what
 * that dialog's cancel does, or the two exits leave the app in different states. Pass the
 * SAME lambda the bottom cancel button uses.
 *
 * The bottom button is a separate decision: a dialog whose only action is leaving needs
 * no button at all (this ✕ is the way out), while one with a real action keeps its
 * cancel so the pair reads as a choice.
 */
@Composable
internal fun DialogTitleWithClose(title: String, onClose: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.headlineSmall,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        IconButton(onClick = onClose, modifier = Modifier.size(32.dp)) {
            Icon(
                imageVector = Icons.Outlined.Close,
                contentDescription = stringResource(R.string.common_close),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp),
            )
        }
    }
}
