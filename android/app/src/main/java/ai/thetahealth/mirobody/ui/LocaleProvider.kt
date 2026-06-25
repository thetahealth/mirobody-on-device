package ai.thetahealth.mirobody.ui

import android.content.res.Configuration
import android.os.LocaleList
import android.text.TextUtils
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.LayoutDirection
import java.util.Locale

/**
 * Overrides the Compose locale at runtime so `stringResource(...)` reloads
 * without an Activity recreate. Both LocalConfiguration and LocalContext are
 * replaced because `stringResource` reads from `LocalContext.current.resources`.
 *
 * Also provides LocalLayoutDirection from the locale, so a right-to-left language
 * (Arabic / Hebrew) mirrors the whole UI. Compose's start/end-aware layout does
 * the rest; nothing else needs per-locale handling.
 */
@Composable
fun ProvideLocale(languageCode: String, content: @Composable () -> Unit) {
    val baseContext = LocalContext.current
    val baseConfig = LocalConfiguration.current
    val newConfig = remember(languageCode, baseConfig) {
        val locale = Locale(languageCode)
        Locale.setDefault(locale)
        Configuration(baseConfig).apply {
            setLocale(locale)
            setLocales(LocaleList(locale))
        }
    }
    val newContext = remember(languageCode, baseContext) {
        baseContext.createConfigurationContext(newConfig)
    }
    val layoutDirection = remember(languageCode) {
        if (TextUtils.getLayoutDirectionFromLocale(Locale(languageCode)) == View.LAYOUT_DIRECTION_RTL) {
            LayoutDirection.Rtl
        } else {
            LayoutDirection.Ltr
        }
    }
    CompositionLocalProvider(
        LocalConfiguration provides newConfig,
        LocalContext provides newContext,
        LocalLayoutDirection provides layoutDirection,
    ) {
        content()
    }
}