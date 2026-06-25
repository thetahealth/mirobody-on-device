package ai.thetahealth.mirobody.ui

import androidx.compose.ui.unit.dp

/**
 * Layout caps for wide screens (tablets, foldables, large windows). Phone widths
 * stay below these caps so nothing changes there; on tablets these prevent the
 * single-column UI from stretching edge-to-edge.
 */

/** Max width for primary content (chat list, auth forms, settings forms). */
val ContentMaxWidth = 640.dp

/** Max width for the modal navigation drawer (history). */
val DrawerMaxWidth = 360.dp
