package ai.thetahealth.mirobody.wxapi

import ai.thetahealth.mirobody.BuildConfig
import ai.thetahealth.mirobody.data.auth.WechatAuthBridge
import android.app.Activity
import android.content.Intent
import android.os.Bundle
import com.tencent.mm.opensdk.constants.ConstantsAPI
import com.tencent.mm.opensdk.modelbase.BaseReq
import com.tencent.mm.opensdk.modelbase.BaseResp
import com.tencent.mm.opensdk.modelmsg.SendAuth
import com.tencent.mm.opensdk.openapi.IWXAPI
import com.tencent.mm.opensdk.openapi.IWXAPIEventHandler
import com.tencent.mm.opensdk.openapi.WXAPIFactory

/**
 * WeChat OpenSDK callback entry point. The SDK *requires* this class to live at
 * `<applicationId>.wxapi.WXEntryActivity`; WeChat launches it to deliver the
 * result of a request (here, the `SendAuth.Resp` carrying the OAuth code).
 *
 * It hands the code (or error/cancel) to [WechatAuthBridge], which unblocks the
 * coroutine in WechatAuthRepository, then finishes immediately — it has no UI.
 */
class WXEntryActivity : Activity(), IWXAPIEventHandler {
    private lateinit var api: IWXAPI

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        api = WXAPIFactory.createWXAPI(this, BuildConfig.WECHAT_APP_ID, true)
        try {
            api.handleIntent(intent, this)
        } catch (e: Exception) {
            WechatAuthBridge.onError(e.message ?: "WeChat callback failed")
            finish()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        api.handleIntent(intent, this)
    }

    override fun onReq(req: BaseReq) {
        // We never receive requests from WeChat (we only initiate auth); ignore.
        finish()
    }

    override fun onResp(resp: BaseResp) {
        if (resp.type == ConstantsAPI.COMMAND_SENDAUTH && resp is SendAuth.Resp) {
            when (resp.errCode) {
                BaseResp.ErrCode.ERR_OK -> WechatAuthBridge.onCode(resp.code ?: "")
                BaseResp.ErrCode.ERR_USER_CANCEL,
                BaseResp.ErrCode.ERR_AUTH_DENIED -> WechatAuthBridge.onCancel()
                else -> WechatAuthBridge.onError("WeChat error ${resp.errCode}")
            }
        }
        finish()
    }
}
