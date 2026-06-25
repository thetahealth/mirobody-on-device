package ai.thetahealth.mirobody.ui

import android.content.Context
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.data.net.AppError

/** Render an [AppError] as a localized, user-friendly string using the given context. */
fun AppError.toLocalizedMessage(context: Context): String = when (this) {
    is AppError.Auth -> context.getString(R.string.error_auth)
    is AppError.Network -> context.getString(R.string.error_network)
    is AppError.Http -> context.getString(R.string.error_http, status)
    is AppError.Api -> {
        val suffix = serverMessage?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: ""
        context.getString(R.string.error_api, code, suffix)
    }
    is AppError.Parse -> context.getString(R.string.error_parse)
    is AppError.Unknown -> context.getString(R.string.error_unknown)
}
