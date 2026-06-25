package ai.thetahealth.mirobody.data.net

import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow

/**
 * App-wide error stream. ViewModels emit; a single collector at the root (typically
 * MainActivity) renders to a Snackbar. Extra buffer + DROP_OLDEST guarantees emit
 * never suspends — slow consumers just lose the oldest pending error.
 */
class ErrorBus {
    private val _errors = MutableSharedFlow<AppError>(
        extraBufferCapacity = 8,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    val errors: SharedFlow<AppError> = _errors.asSharedFlow()

    fun emit(error: AppError) {
        _errors.tryEmit(error)
    }

    fun emit(throwable: Throwable) {
        emit(throwable.toAppError())
    }
}
