package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.BuildConfig
import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import android.content.Context
import com.tencent.mm.opensdk.modelmsg.SendAuth
import com.tencent.mm.opensdk.openapi.IWXAPI
import com.tencent.mm.opensdk.openapi.WXAPIFactory
import kotlinx.coroutines.withTimeout

/**
 * "Sign in with WeChat" via the WeChat OpenSDK.
 *
 * The flow:
 *  1. Register the app id ([BuildConfig.WECHAT_APP_ID], an Open Platform
 *     "Mobile Application" id) with the SDK.
 *  2. Send a `SendAuth.Req` (scope `snsapi_userinfo`); the SDK switches to the
 *     WeChat app for the user to approve.
 *  3. WeChat returns to `ai.thetahealth.mirobody.wxapi.WXEntryActivity`, whose
 *     `onResp` forwards the OAuth `code` through [WechatAuthBridge].
 *  4. POST that code to `/wechat/verify` with `flow=app`; the backend exchanges
 *     it via WeChat's sns/oauth2/access_token (mobile-app credentials) and
 *     returns a mirobody JWT.
 *
 * Requires the WeChat app to be installed and the app id to be registered on the
 * WeChat Open Platform with this app's package name + signing signature.
 */
class WechatAuthRepository(
    context: Context,
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    private val appContext: Context = context.applicationContext
    private val appId: String = BuildConfig.WECHAT_APP_ID

    /** True when an app id is baked in; the button is otherwise non-functional. */
    val isConfigured: Boolean get() = appId.isNotBlank()

    private val wxApi: IWXAPI by lazy {
        WXAPIFactory.createWXAPI(appContext, appId, true).also {
            if (appId.isNotBlank()) it.registerApp(appId)
        }
    }

    suspend fun signInWithWeChat(): AuthTokenResponse {
        if (appId.isBlank()) error("WeChat sign-in is not configured.")
        if (!wxApi.isWXAppInstalled) error("WeChat is not installed.")

        // Re-register defensively (the SDK can drop registration when WeChat
        // restarts the process) and arm the bridge before launching.
        wxApi.registerApp(appId)
        val deferred = WechatAuthBridge.begin()

        val req = SendAuth.Req().apply {
            scope = "snsapi_userinfo"
            state = "mirobody_wechat"
        }
        if (!wxApi.sendReq(req)) {
            WechatAuthBridge.onCancel()
            error("Failed to launch WeChat.")
        }

        // The user may dawdle in WeChat; cap the wait so we don't leak a coroutine.
        val code = withTimeout(120_000) { deferred.await() }

        val backendToken = api.verifyWechatCode(WechatVerifyRequest(code = code, flow = "app")).unwrap()
        settings.setAccessToken(backendToken.accessToken)
        return backendToken
    }
}
