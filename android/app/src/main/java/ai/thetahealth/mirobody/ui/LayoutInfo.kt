package ai.thetahealth.mirobody.ui

import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * Screen-size signal that drives responsive layout. Computed once in MainActivity from the
 * root window's BoxWithConstraints and provided via [LocalLayoutInfo]. Watch builds (the
 * `watch` product flavour, BuildConfig.IS_WATCH) and any genuinely small window get
 * compacted spacing and tighter content caps so the full feature set still fits on a
 * ~410x502 dp screen.
 *
 * Layout decisions read [dense] rather than [isWatch] directly, so a small/short phone
 * window (split-screen, foldable cover) compacts too, and previews behave sensibly.
 */
data class LayoutInfo(
    val widthDp: Dp,
    val heightDp: Dp,
    /** True on the `watch` product flavour (BuildConfig.IS_WATCH). */
    val isWatch: Boolean,
) {
    /** Narrow width: single tight column, minimal horizontal padding. */
    val isCompact: Boolean get() = widthDp < 360.dp

    /** Short height: collapse vertical chrome so chat history + composer keep room. */
    val isShort: Boolean get() = heightDp < 560.dp

    /** Apply the dense, watch-tuned layout. */
    val dense: Boolean get() = isWatch || isShort || isCompact

    /** Cap for primary content (chat list, forms). On dense screens fill the width. */
    val contentMaxWidth: Dp get() = if (dense) widthDp else ContentMaxWidth

    /** Cap for the modal history drawer. */
    val drawerMaxWidth: Dp get() = if (dense) 280.dp else DrawerMaxWidth

    /** Fraction of width the history drawer occupies. */
    val drawerWidthFraction: Float get() = if (dense) 0.86f else 0.75f

    /** Horizontal screen padding for scrollable content. */
    val screenPadding: Dp get() = if (dense) 10.dp else 16.dp

    /** Vertical spacing between chat messages. */
    val messageSpacing: Dp get() = if (dense) 8.dp else 14.dp

    /** Max width of a chat bubble (user message / thinking block). */
    val bubbleMaxWidth: Dp get() = if (dense) 280.dp else 320.dp
}

/**
 * Fallback assumes a comfortable phone, so screens rendered without a provider (Compose
 * previews, isolated tests) look right. MainActivity overrides this with the real window.
 */
val LocalLayoutInfo = staticCompositionLocalOf {
    LayoutInfo(widthDp = 411.dp, heightDp = 891.dp, isWatch = false)
}
