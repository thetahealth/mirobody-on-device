package ai.thetahealth.mirobody.data.net

import kotlinx.serialization.Serializable

@Serializable
data class ApiEnvelope<T>(
    val code: Int = 0,
    val message: String? = null,
    val data: T? = null,
)

class ApiException(
    val apiCode: Int,
    message: String?,
) : RuntimeException(message ?: "API error (code=$apiCode)")

fun <T> ApiEnvelope<T>.unwrap(): T {
    if (code != 0) throw ApiException(code, message)
    return data ?: throw ApiException(code, message ?: "empty response data")
}

fun ApiEnvelope<*>.ensureOk() {
    if (code != 0) throw ApiException(code, message)
}