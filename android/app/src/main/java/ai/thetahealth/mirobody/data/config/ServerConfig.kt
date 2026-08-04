package ai.thetahealth.mirobody.data.config

import kotlinx.serialization.Serializable

/**
 * Mirrors the `GET /auth/providers` document: which federated sign-ins this
 * deployment has configured, plus each one's public config.
 *
 * This replaced a `/mirobody.json` document that no mirobody server has ever
 * served — a leftover from the template-injected web build of the old backend.
 * The fetch always 404'd, so every flag stayed false and every federated sign-in
 * button was silently missing on this client. That document also carried a dozen
 * unrelated flags (EHR, QR, WebAuthn, HIE, feature list) that nothing read; they
 * are not reproduced here.
 *
 * A provider the server has no opinion about is ABSENT rather than disabled, so
 * these fields are nullable and "missing" reads the same as "off" only because
 * [isOn] treats it that way. X is the live case for absence: it is brokered
 * entirely through Firebase, has no server-side config, and so is not gated on
 * this document at all.
 */
@Serializable
data class ServerConfig(
    val google: Provider? = null,
    val apple: Provider? = null,
    val wechat: Provider? = null,
    val github: Provider? = null,
    val tanka: Provider? = null,
) {
    @Serializable
    data class Provider(
        val enabled: Boolean = false,
        /** Public config; present only when enabled, and only if there is any. */
        val config: ProviderConfig? = null,
        /**
         * Whether the NATIVE flow works, which is not the same question as [enabled]
         * and is the one this client must ask. Sent only for the providers where the
         * two can differ:
         *
         *  - google: the web button needs FIREBASE_WEB_API_KEY to boot the JS SDK;
         *    verifying an ID token needs only FIREBASE_PROJECT_ID. A server with the
         *    project id and no web key signs native users in fine. (X rides the same
         *    Firebase project, so it reads this too.)
         *  - wechat: two independent registrations — an Open Platform *website* app
         *    for the browser, a *mobile application* for the OpenSDK. This client
         *    uses the latter (POST /wechat/verify with flow="app").
         *
         * Gating on [enabled] would hide buttons that work.
         */
        val appEnabled: Boolean = false,
    )

    /**
     * The union of the per-provider public configs — which fields appear depends
     * on the provider: Firebase keys for google, `clientId` for apple/github,
     * `appid` for wechat, nothing at all for tanka.
     */
    @Serializable
    data class ProviderConfig(
        val apiKey: String? = null,
        val projectId: String? = null,
        val messagingSenderId: String? = null,
        val clientId: String? = null,
        val appid: String? = null,
    )

    /**
     * Whether the server can verify a Firebase ID token — it needs only
     * FIREBASE_PROJECT_ID, which is what `google.appEnabled` reports.
     *
     * This gates FOUR buttons on this client, not just Google. Android brokers
     * Google, X, GitHub *and* Apple through Firebase's OAuthProvider and POSTs the
     * resulting Firebase ID token to /firebase/verify — so the server settings that
     * matter are Firebase's, not GITHUB_CLIENT_ID or APPLE_CLIENT_ID. Those gate the
     * browser, which does its own server-side code exchange against /github/verify
     * and /apple/verify. Same button, different flow, different prerequisite.
     */
    val firebaseVerifyEnabled: Boolean get() = google?.appEnabled == true

    /**
     * The native Apple path (a real Apple id_token POSTed to /apple/verify), which
     * needs APPLE_CLIENT_ID to validate the `aud` claim. iOS uses it; Android does
     * not — see [firebaseVerifyEnabled].
     */
    val appleNativeEnabled: Boolean get() = apple?.enabled == true

    /** The mobile OpenSDK flow — see [Provider.appEnabled]; NOT the web one. */
    val wechatEnabled: Boolean get() = wechat?.appEnabled == true

    /** Firebase *web* config. Unused by this client — it initializes Firebase from
     *  the bundled google-services.json — but kept so the DTO matches the wire. */
    val firebase: ProviderConfig? get() = google?.takeIf { it.enabled }?.config
}
