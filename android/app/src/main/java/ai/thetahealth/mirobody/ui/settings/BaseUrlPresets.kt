package ai.thetahealth.mirobody.ui.settings

import ai.thetahealth.mirobody.data.settings.SettingsStore

internal val BASE_URL_PRESETS: List<String> = listOf(
    SettingsStore.DEFAULT_BASE_URL,
    "https://test.mirobody.ai",
    "https://gray.mirobody.ai",
    "https://mirobody.ai",
)
