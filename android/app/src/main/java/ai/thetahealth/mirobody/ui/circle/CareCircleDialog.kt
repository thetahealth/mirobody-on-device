package ai.thetahealth.mirobody.ui.circle

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.data.circle.dto.Circle
import ai.thetahealth.mirobody.data.circle.dto.CircleInvite
import ai.thetahealth.mirobody.data.circle.dto.CircleMember
import ai.thetahealth.mirobody.ui.LocalAppContainer

private const val ROLE_OWNER = 2
private const val ROLE_MAINTAINER = 1
private const val ACCESS_VIEW = 1
private const val ACCESS_EDIT = 2

/**
 * Full-screen care-circle management dialog — the Android counterpart of the web
 * client's "My care circles" modal. Lists circles I belong to (members, roles,
 * per-circle health-sharing), pending invitations to me, and circle creation.
 * Cross-user references use the opaque `member` handle; my own identity is the JWT.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CareCircleDialog(
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: CircleViewModel = viewModel(
        factory = viewModelFactory {
            initializer { CircleViewModel(container.circleRepository, container.errorBus) }
        },
    )
    LaunchedEffect(Unit) { vm.refresh() }
    val state by vm.state.collectAsState()

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
            Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                Column(modifier = Modifier.fillMaxSize()) {
                    // Header
                    Row(
                        modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp, bottom = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            stringResource(R.string.circle_title),
                            style = MaterialTheme.typography.titleLarge,
                            modifier = Modifier.weight(1f),
                        )
                        IconButton(onClick = onDismiss) {
                            Icon(Icons.Filled.Close, contentDescription = stringResource(R.string.common_close))
                        }
                    }
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))

                    if (state.loading && state.circles.isEmpty() && state.invites.isEmpty()) {
                        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator()
                        }
                    } else {
                        Column(
                            modifier = Modifier
                                .fillMaxSize()
                                .verticalScroll(rememberScrollState())
                                .padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(20.dp),
                        ) {
                            if (state.invites.isNotEmpty()) {
                                SectionLabel(stringResource(R.string.circle_invites))
                                state.invites.forEach { inv ->
                                    InviteRow(inv, onAccept = { vm.accept(inv.token) }, onDecline = { vm.decline(inv.token) })
                                }
                            }

                            if (state.circles.isEmpty()) {
                                Text(
                                    stringResource(R.string.circle_no_circles),
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    style = MaterialTheme.typography.bodyMedium,
                                )
                            } else {
                                state.circles.forEach { circle -> CircleCard(circle, vm) }
                            }

                            CreateCircleRow(onCreate = { name -> vm.createCircle(name) })
                        }
                    }
                }
            }
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontWeight = FontWeight.SemiBold,
    )
}

@Composable
private fun InviteRow(invite: CircleInvite, onAccept: () -> Unit, onDecline: () -> Unit) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        val who = invite.ownerEmail.ifBlank { invite.circleName }
        Text(
            if (invite.circleName.isNotBlank() && invite.ownerEmail.isNotBlank()) {
                "${invite.ownerEmail} · ${invite.circleName}"
            } else {
                who
            },
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodyMedium,
        )
        TextButton(onClick = onAccept) { Text(stringResource(R.string.circle_accept)) }
        TextButton(onClick = onDecline) { Text(stringResource(R.string.circle_decline)) }
    }
}

@Composable
private fun CircleCard(circle: Circle, vm: CircleViewModel) {
    val isOwner = circle.myRole == ROLE_OWNER
    val isAdmin = circle.myRole >= ROLE_MAINTAINER
    var showDelete by remember { mutableStateOf(false) }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                circle.name,
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f),
            )
            if (isOwner) {
                TextButton(onClick = { showDelete = true }) {
                    Text(stringResource(R.string.circle_delete_circle), color = MaterialTheme.colorScheme.error)
                }
            }
        }

        SectionLabel(stringResource(R.string.circle_members))
        if (circle.members.isEmpty()) {
            Text(
                stringResource(R.string.circle_no_members),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        } else {
            circle.members.forEach { m -> MemberRow(circle, m, vm) }
        }

        // Invite into this specific circle.
        if (isAdmin) {
            InviteIntoRow(onInvite = { email -> vm.invite(email, circle.circleId) })
        }

        // My own per-circle health-sharing level.
        HealthShareRow(
            current = circle.myHealthAccess,
            onChange = { access -> vm.setHealthSharing(circle.circleId, access) },
        )

        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f))
    }

    if (showDelete) {
        AlertDialog(
            onDismissRequest = { showDelete = false },
            title = { Text(stringResource(R.string.circle_delete_circle)) },
            text = { Text(stringResource(R.string.circle_delete_confirm, circle.name)) },
            confirmButton = {
                TextButton(onClick = { showDelete = false; vm.deleteCircle(circle.circleId) }) {
                    Text(stringResource(R.string.common_delete), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showDelete = false }) { Text(stringResource(R.string.common_cancel)) }
            },
        )
    }
}

@Composable
private fun MemberRow(circle: Circle, m: CircleMember, vm: CircleViewModel) {
    val label = m.nickname.ifBlank { m.email.ifBlank { "#${m.member}" } }
    var showRemove by remember { mutableStateOf(false) }
    var roleMenu by remember { mutableStateOf(false) }

    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            if (m.nickname.isNotBlank() && m.email.isNotBlank()) {
                Text(
                    m.email,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (m.me) {
            Text(
                stringResource(R.string.circle_you),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(end = 6.dp),
            )
        }
        when {
            m.role == ROLE_OWNER -> Text(
                stringResource(R.string.circle_owner),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            m.status != "accepted" -> Text(
                stringResource(R.string.circle_pending),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        // Owner can change a non-owner member's role.
        if (circle.myRole == ROLE_OWNER && m.role != ROLE_OWNER && !m.me) {
            Box {
                TextButton(onClick = { roleMenu = true }) {
                    Text(
                        stringResource(
                            if (m.role == ROLE_MAINTAINER) R.string.circle_role_maintainer
                            else R.string.circle_role_member,
                        ),
                    )
                }
                DropdownMenu(expanded = roleMenu, onDismissRequest = { roleMenu = false }) {
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.circle_role_member)) },
                        onClick = { roleMenu = false; vm.setRole(m.member, "member") },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.circle_role_maintainer)) },
                        onClick = { roleMenu = false; vm.setRole(m.member, "maintainer") },
                    )
                }
            }
        }

        // Owner removes anyone but the owner; a maintainer removes plain members.
        val canRemove = m.role != ROLE_OWNER && !m.me &&
            (circle.myRole == ROLE_OWNER || (circle.myRole == ROLE_MAINTAINER && m.role < ROLE_MAINTAINER))
        if (canRemove) {
            TextButton(onClick = { showRemove = true }) {
                Text(stringResource(R.string.circle_remove), color = MaterialTheme.colorScheme.error)
            }
        }
    }

    if (showRemove) {
        AlertDialog(
            onDismissRequest = { showRemove = false },
            title = { Text(stringResource(R.string.circle_remove)) },
            text = { Text(stringResource(R.string.circle_remove_confirm, label)) },
            confirmButton = {
                TextButton(onClick = { showRemove = false; vm.remove(m.member) }) {
                    Text(stringResource(R.string.circle_remove), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showRemove = false }) { Text(stringResource(R.string.common_cancel)) }
            },
        )
    }
}

@Composable
private fun InviteIntoRow(onInvite: (String) -> Unit) {
    var email by remember { mutableStateOf("") }
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OutlinedTextField(
            value = email,
            onValueChange = { email = it },
            placeholder = { Text(stringResource(R.string.circle_invite_hint)) },
            singleLine = true,
            modifier = Modifier.weight(1f),
        )
        OutlinedButton(
            onClick = {
                val e = email.trim()
                if (e.isNotEmpty()) { onInvite(e); email = "" }
            },
            modifier = Modifier.height(56.dp),   // match the text field's height
        ) { Text(stringResource(R.string.circle_invite)) }
    }
}

@Composable
private fun HealthShareRow(current: Int, onChange: (String) -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val labelRes = when {
        current >= ACCESS_EDIT -> R.string.circle_health_edit
        current >= ACCESS_VIEW -> R.string.circle_health_view
        else -> R.string.circle_health_off
    }
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            stringResource(R.string.circle_health_share),
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodyMedium,
        )
        Box {
            OutlinedButton(onClick = { menu = true }) { Text(stringResource(labelRes)) }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.circle_health_off)) },
                    onClick = { menu = false; onChange("off") },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.circle_health_view)) },
                    onClick = { menu = false; onChange("view") },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.circle_health_edit)) },
                    onClick = { menu = false; onChange("edit") },
                )
            }
        }
    }
}

@Composable
private fun CreateCircleRow(onCreate: (String) -> Unit) {
    var name by remember { mutableStateOf("") }
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OutlinedTextField(
            value = name,
            onValueChange = { name = it },
            placeholder = { Text(stringResource(R.string.circle_new_name_hint)) },
            singleLine = true,
            modifier = Modifier.weight(1f),
        )
        OutlinedButton(
            onClick = {
                val n = name.trim()
                if (n.isNotEmpty()) { onCreate(n); name = "" }
            },
            modifier = Modifier.height(56.dp),   // match the text field's height
        ) { Text(stringResource(R.string.circle_create)) }
    }
}
