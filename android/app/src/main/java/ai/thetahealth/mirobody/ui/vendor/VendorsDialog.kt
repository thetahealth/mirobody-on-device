package ai.thetahealth.mirobody.ui.vendor

import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.Base64
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.ui.LocalAppContainer
import androidx.compose.runtime.DisposableEffect

/**
 * Connected-devices manager: the vendor catalog in two tabs (Devices / Platforms),
 * each row Connect (opens the OAuth consent URL in a browser) or Disconnect. On
 * returning from the browser the list re-fetches, so a completed connect shows up.
 * Mirrors the web client's vendors modal.
 */
@Composable
fun VendorsDialog(
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: VendorsViewModel = viewModel(
        factory = viewModelFactory {
            initializer { VendorsViewModel(container.vendorRepository) }
        },
    )
    val state by vm.state.collectAsState()
    val connectUrl by vm.connectUrl.collectAsState()
    val context = LocalContext.current

    // Hand a pending OAuth URL off to the browser, then clear it.
    LaunchedEffect(connectUrl) {
        val url = connectUrl ?: return@LaunchedEffect
        runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
        vm.consumeConnectUrl()
    }

    // Re-list when the user returns to the app (e.g. after connecting in the
    // browser), skipping the initial resume since init already loaded.
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        var first = true
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                if (first) first = false else vm.load()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    var unlinkTarget by remember { mutableStateOf<VendorItem?>(null) }
    var activeTab by remember { mutableStateOf(0) }

    Dialog(onDismissRequest = onDismiss) {
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.background,
            ) {
                Column(modifier = Modifier.padding(vertical = 16.dp)) {
                    // Header: title + close.
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(start = 22.dp, end = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = stringResource(R.string.chat_vendors),
                            style = MaterialTheme.typography.titleLarge,
                            color = MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.weight(1f),
                        )
                        IconButton(onClick = onDismiss) {
                            Icon(
                                Icons.Outlined.Close,
                                contentDescription = stringResource(R.string.common_close),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    Text(
                        text = stringResource(R.string.chat_vendors_subtitle),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 22.dp),
                    )
                    Spacer(Modifier.size(12.dp))

                    TabRow(selectedTabIndex = activeTab, modifier = Modifier.padding(horizontal = 16.dp)) {
                        Tab(
                            selected = activeTab == 0,
                            onClick = { activeTab = 0 },
                            text = { Text(stringResource(R.string.chat_vendor_devices)) },
                        )
                        Tab(
                            selected = activeTab == 1,
                            onClick = { activeTab = 1 },
                            text = { Text(stringResource(R.string.chat_vendor_platforms)) },
                        )
                    }
                    Spacer(Modifier.size(8.dp))

                    val items = if (activeTab == 0) state.devices else state.platforms
                    Box(modifier = Modifier.heightIn(max = 420.dp)) {
                        if (state.loading && items.isEmpty()) {
                            Box(
                                modifier = Modifier.fillMaxWidth().padding(24.dp),
                                contentAlignment = Alignment.Center,
                            ) { CircularProgressIndicator() }
                        } else {
                            LazyColumn(
                                modifier = Modifier.padding(horizontal = 16.dp),
                                verticalArrangement = Arrangement.spacedBy(6.dp),
                            ) {
                                items(items, key = { it.id }) { item ->
                                    VendorRow(
                                        item = item,
                                        onConnect = { vm.connect(item.id) },
                                        onDisconnect = { unlinkTarget = item },
                                    )
                                }
                            }
                        }
                    }

                    state.error?.let {
                        Text(
                            text = it,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.padding(horizontal = 22.dp, vertical = 6.dp),
                        )
                    }
                }
            }
    }

    unlinkTarget?.let { target ->
        AlertDialog(
            onDismissRequest = { unlinkTarget = null },
            title = { DialogTitleWithClose(stringResource(R.string.chat_vendor_disconnect), { unlinkTarget = null }) },
            text = { Text(stringResource(R.string.chat_vendor_unlink_confirm, target.name)) },
            confirmButton = {
                TextButton(onClick = { vm.unlink(target.id); unlinkTarget = null }) {
                    Text(
                        stringResource(R.string.chat_vendor_disconnect),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { unlinkTarget = null }) {
                    Text(stringResource(R.string.common_cancel))
                }
            },
        )
    }
}

@Composable
private fun VendorRow(
    item: VendorItem,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .border(
                1.dp,
                MaterialTheme.colorScheme.outlineVariant,
                RoundedCornerShape(8.dp),
            )
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        VendorIcon(item)
        Text(
            text = item.name,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .weight(1f)
                .padding(start = 10.dp),
        )
        if (item.pending) {
            Text(
                text = stringResource(R.string.chat_vendor_pending),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(end = 8.dp),
            )
        }
        if (item.connected) {
            OutlinedButton(onClick = onDisconnect) {
                Text(
                    stringResource(R.string.chat_vendor_disconnect),
                    color = MaterialTheme.colorScheme.error,
                )
            }
        } else {
            OutlinedButton(onClick = onConnect) {
                Text(stringResource(R.string.chat_vendor_connect))
            }
        }
    }
}

/** The brand's server-provided icon (data URI), or a colored monogram fallback. */
@Composable
private fun VendorIcon(item: VendorItem) {
    val bitmap = remember(item.iconDataUri) { decodeDataUri(item.iconDataUri) }
    if (bitmap != null) {
        Image(
            bitmap = bitmap,
            contentDescription = null,
            modifier = Modifier
                .size(24.dp)
                .clip(RoundedCornerShape(6.dp)),
        )
    } else {
        Box(
            modifier = Modifier
                .size(24.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primary),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = item.name.firstOrNull()?.uppercaseChar()?.toString() ?: "?",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onPrimary,
            )
        }
    }
}

/** Decode a "data:<mime>;base64,<...>" URI to an ImageBitmap, or null. */
private fun decodeDataUri(uri: String?): androidx.compose.ui.graphics.ImageBitmap? {
    if (uri == null) return null
    val comma = uri.indexOf(',')
    if (comma < 0) return null
    return runCatching {
        val bytes = Base64.decode(uri.substring(comma + 1), Base64.DEFAULT)
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
    }.getOrNull()
}
