package ai.thetahealth.mirobody.ui

import android.content.Context
import android.content.res.Configuration
import java.util.Locale

/**
 * Wrap a base context so its resources resolve in [languageCode] (and mirror the
 * layout direction, so Arabic / Hebrew go RTL). MainActivity.attachBaseContext
 * applies this to the whole Activity, so every context it hands out -- including
 * the separate windows Compose dialogs run in -- already uses the chosen language.
 */
fun localizedContext(base: Context, languageCode: String): Context {
    val locale = Locale(languageCode)
    val config = Configuration(base.resources.configuration)
    config.setLocale(locale)
    config.setLayoutDirection(locale)
    return base.createConfigurationContext(config)
}
