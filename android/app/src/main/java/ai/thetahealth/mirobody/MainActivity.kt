package ai.thetahealth.mirobody

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.core.content.ContextCompat
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import ai.thetahealth.mirobody.ui.LayoutInfo
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import ai.thetahealth.mirobody.ui.LocalLayoutInfo
import ai.thetahealth.mirobody.ui.MirobodyNavGraph
import ai.thetahealth.mirobody.ui.ProvideLocale
import ai.thetahealth.mirobody.ui.theme.MirobodyTheme
import ai.thetahealth.mirobody.ui.toLocalizedMessage

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        startMirobodyService()
        enableEdgeToEdge()
        val container = (application as MirobodyApp).container
        setContent {
            MirobodyTheme {
                val fontSizePreview = remember { mutableStateOf<Int?>(null) }
                CompositionLocalProvider(
                    LocalAppContainer provides container,
                    LocalFontSizePreview provides fontSizePreview,
                    // ProvideLocale (below) swaps LocalContext for a createConfigurationContext
                    // result whose unwrap chain no longer reaches this Activity, so
                    // rememberLauncherForActivityResult can't find the registry owner by
                    // unwrapping the context. Provide it explicitly here, above the locale wrap.
                    LocalActivityResultRegistryOwner provides this@MainActivity,
                ) {
                    val language by container.settings.language.collectAsState(initial = "en")
                    val persisted by container.settings.fontSizeOffset.collectAsState(initial = 0)
                    // Once the persisted value catches up to the preview (post-confirm DataStore
                    // write), drop the preview so we stop overriding.
                    LaunchedEffect(persisted, fontSizePreview.value) {
                        if (fontSizePreview.value == persisted) fontSizePreview.value = null
                    }
                    val effectiveOffset = fontSizePreview.value ?: persisted
                    ProvideLocale(language) {
                        val base = LocalDensity.current
                        // Offset is in "px relative to a 14sp body" → convert to a fontScale multiplier
                        // layered on top of the system/accessibility scale.
                        val scaled = Density(base.density, base.fontScale * (1f + effectiveOffset / 14f))
                        CompositionLocalProvider(LocalDensity provides scaled) {
                            val snackbarHost = remember { SnackbarHostState() }
                            val context = LocalContext.current
                            LaunchedEffect(Unit) {
                                container.errorBus.errors.collect { err ->
                                    snackbarHost.showSnackbar(err.toLocalizedMessage(context))
                                }
                            }
                            // Measure the root window once and publish it as LayoutInfo so
                            // every screen can compact itself on small/watch displays.
                            BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
                                val layoutInfo = LayoutInfo(
                                    widthDp = maxWidth,
                                    heightDp = maxHeight,
                                    isWatch = BuildConfig.IS_WATCH,
                                )
                                CompositionLocalProvider(LocalLayoutInfo provides layoutInfo) {
                                    MirobodyNavGraph()
                                    SnackbarHost(
                                        hostState = snackbarHost,
                                        modifier = Modifier
                                            .align(Alignment.BottomCenter)
                                            .imePadding()
                                            .navigationBarsPadding(),
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Launch the in-process C++ server (MirobodyService -> libmirobody.so) so the client can reach
    // it at localhost:8080 (SettingsStore.DEFAULT_BASE_URL). If the native library is absent the
    // service stops itself and the UI keeps running as a plain client. OpenAI/Gemini keys are left
    // empty here; supply them via config/secure storage to enable upstream chat.
    private fun startMirobodyService() {
        // Pure-client build (no libmirobody.so): nothing to host, so don't start the
        // foreground service at all — it would only stop itself immediately.
        if (!NativeBridge.available) return
        val intent = Intent(this, MirobodyService::class.java)
            .putExtra(MirobodyService.EXTRA_LISTEN_PORT, 8080)
        ContextCompat.startForegroundService(this, intent)
    }
}