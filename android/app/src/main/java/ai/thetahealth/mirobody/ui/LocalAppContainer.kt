package ai.thetahealth.mirobody.ui

import androidx.compose.runtime.MutableState
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.staticCompositionLocalOf
import ai.thetahealth.mirobody.di.AppContainer

val LocalAppContainer = staticCompositionLocalOf<AppContainer> {
    error("AppContainer not provided")
}

/**
 * Transient override for the persisted font-size offset, used while previewing
 * the slider in the font-size dialog. `null` means "no override — use the
 * persisted value."
 */
val LocalFontSizePreview = compositionLocalOf<MutableState<Int?>> {
    error("LocalFontSizePreview not provided")
}