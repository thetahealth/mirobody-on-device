package ai.thetahealth.mirobody.data.net

import kotlinx.serialization.SerializationException
import retrofit2.HttpException
import java.io.IOException

/**
 * App-level error taxonomy. Network/Retrofit/serialization exceptions and our own
 * [ApiException] are normalized into one of these so the UI can render a localized
 * message and decide on follow-up (e.g. route to login on [Auth]).
 */
sealed class AppError(open val cause: Throwable?) {
    /** HTTP 401/403 — auth expired/invalid. Caller should route back to login. */
    data class Auth(override val cause: Throwable? = null) : AppError(cause)

    /** Any other non-2xx HTTP status. */
    data class Http(val status: Int, override val cause: Throwable? = null) : AppError(cause)

    /** Envelope returned with code != 0. `serverMessage` is the backend `msg` if present. */
    data class Api(val code: Int, val serverMessage: String?, override val cause: Throwable? = null) :
        AppError(cause)

    /** JSON missing/extra fields, wrong type, malformed payload. */
    data class Parse(override val cause: Throwable? = null) : AppError(cause)

    /** Connectivity / timeout / unreachable host. */
    data class Network(override val cause: Throwable? = null) : AppError(cause)

    /** Anything we didn't categorize. */
    data class Unknown(override val cause: Throwable? = null) : AppError(cause)
}

fun Throwable.toAppError(): AppError = when (this) {
    is ApiException -> AppError.Api(apiCode, message)
    is HttpException -> when (code()) {
        401, 403 -> AppError.Auth(this)
        else -> AppError.Http(code(), this)
    }
    is SerializationException -> AppError.Parse(this)
    is IOException -> AppError.Network(this)
    else -> AppError.Unknown(this)
}
