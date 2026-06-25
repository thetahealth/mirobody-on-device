package ai.thetahealth.mirobody.data.chat.dto

import kotlinx.serialization.Serializable

@Serializable
data class ProviderInfo(
    val name: String = "",
    val code: String = "",
) {
    val agentCode: String get() = name.substringBefore('/', missingDelimiterValue = "")
}
