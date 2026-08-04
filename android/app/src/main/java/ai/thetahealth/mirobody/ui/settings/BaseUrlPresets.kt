package ai.thetahealth.mirobody.ui.settings

import ai.thetahealth.mirobody.data.settings.SettingsStore

// DEFAULT_BASE_URL is build-dependent now (localhost for an embedded build, the test server
// for a pure client), so both fixed entries are listed explicitly and `distinct` drops the
// duplicate rather than the list silently losing whichever one the build did not pick.
internal val BASE_URL_PRESETS: List<String> = listOf(
    SettingsStore.DEFAULT_BASE_URL,
    "http://localhost:8080",
    "https://test.mirobody.ai",
).distinct()
