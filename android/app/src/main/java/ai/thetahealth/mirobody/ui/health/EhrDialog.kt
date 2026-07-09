package ai.thetahealth.mirobody.ui.health

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.LocalAppContainer

/**
 * Connect an EHR (SMART on FHIR): search the provider directory or paste a FHIR
 * base URL, connect (opens the SMART consent page in a browser), then Sync to pull
 * records. Mirrors the web client's EHR modal. The app language is applied
 * app-wide (MainActivity.attachBaseContext), so no locale wrapping is needed here.
 */
@Composable
fun EhrDialog(onDismiss: () -> Unit) {
    val container = LocalAppContainer.current
    val vm: EhrViewModel = viewModel(
        factory = viewModelFactory { initializer { EhrViewModel(container.ehrRepository) } },
    )
    val state by vm.state.collectAsState()
    val connectUrl by vm.connectUrl.collectAsState()
    val context = LocalContext.current

    LaunchedEffect(connectUrl) {
        val url = connectUrl ?: return@LaunchedEffect
        runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
        vm.consumeConnectUrl()
    }

    var manualUrl by remember { mutableStateOf("") }
    var query by remember { mutableStateOf("") }

    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = RoundedCornerShape(14.dp), color = MaterialTheme.colorScheme.background) {
            Column(
                modifier = Modifier.padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = stringResource(R.string.chat_ehr_title),
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
                    text = stringResource(R.string.chat_ehr_subtitle),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )

                // Manual FHIR base URL (e.g. a SMART sandbox).
                OutlinedTextField(
                    value = manualUrl,
                    onValueChange = { manualUrl = it },
                    singleLine = true,
                    label = { Text(stringResource(R.string.chat_ehr_manual_label)) },
                    placeholder = { Text("https://launch.smarthealthit.org/v/r4/fhir") },
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedButton(
                    onClick = { vm.connect(manualUrl) },
                    enabled = manualUrl.isNotBlank() && !state.busy,
                    modifier = Modifier.align(Alignment.End),
                ) { Text(stringResource(R.string.chat_ehr_connect)) }

                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

                // Directory search.
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    OutlinedTextField(
                        value = query,
                        onValueChange = { query = it },
                        singleLine = true,
                        label = { Text(stringResource(R.string.chat_ehr_search_hint)) },
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedButton(
                        onClick = { vm.search(query) },
                        enabled = query.isNotBlank() && !state.searching,
                    ) { Text(stringResource(R.string.chat_ehr_search)) }
                }

                Box(modifier = Modifier.heightIn(max = 240.dp)) {
                    when {
                        state.searching -> Box(
                            modifier = Modifier.fillMaxWidth().padding(16.dp),
                            contentAlignment = Alignment.Center,
                        ) { CircularProgressIndicator() }
                        state.results.isEmpty() -> Unit
                        else -> LazyColumn(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            items(state.results, key = { it.fhirBaseUrl }) { p ->
                                Column(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .clickable { vm.connect(p.fhirBaseUrl) }
                                        .padding(horizontal = 12.dp, vertical = 10.dp),
                                ) {
                                    Text(
                                        text = p.name.ifBlank { p.fhirBaseUrl },
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.onSurface,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                    Text(
                                        text = p.fhirBaseUrl,
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                }
                            }
                        }
                    }
                }

                // Status line: error, sync result, or busy.
                val line = when {
                    state.error != null -> state.error
                    state.syncedCount != null -> stringResource(R.string.chat_ehr_sync_done, state.syncedCount!!)
                    else -> null
                }
                if (line != null) {
                    Text(
                        text = line,
                        style = MaterialTheme.typography.bodySmall,
                        color = if (state.error != null) MaterialTheme.colorScheme.error
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                OutlinedButton(
                    onClick = { vm.sync() },
                    enabled = !state.busy,
                    modifier = Modifier.align(Alignment.End),
                ) { Text(stringResource(R.string.chat_ehr_sync_now)) }
            }
        }
    }
}
