package ai.thetahealth.mirobody.data.auth

import kotlinx.coroutines.CompletableDeferred
import java.util.concurrent.CancellationException

/**
 * Bridges the WeChat OpenSDK callback (delivered to `wxapi.WXEntryActivity` in a
 * separate Activity instance) back to the coroutine in [WechatAuthRepository]
 * that launched the sign-in request.
 *
 * The SDK has no return value: `sendReq()` launches WeChat, and the result later
 * arrives as `onResp` in WXEntryActivity. We park a [CompletableDeferred] here
 * between the two so the repository can `await()` the OAuth code.
 *
 * Only one sign-in can be in flight at a time, which matches the UI (the button
 * is disabled while signing in).
 */
object WechatAuthBridge {
    @Volatile
    private var pending: CompletableDeferred<String>? = null

    /** Arm a new request and return the deferred the repository awaits. */
    @Synchronized
    fun begin(): CompletableDeferred<String> {
        pending?.cancel()
        val d = CompletableDeferred<String>()
        pending = d
        return d
    }

    /** SendAuth.Resp succeeded: hand the OAuth code to the waiter. */
    @Synchronized
    fun onCode(code: String) {
        pending?.complete(code)
        pending = null
    }

    @Synchronized
    fun onError(message: String) {
        pending?.completeExceptionally(IllegalStateException(message))
        pending = null
    }

    @Synchronized
    fun onCancel() {
        pending?.completeExceptionally(CancellationException("WeChat sign-in cancelled"))
        pending = null
    }
}
