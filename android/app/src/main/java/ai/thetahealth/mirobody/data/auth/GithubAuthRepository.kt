package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import android.app.Activity
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.OAuthProvider
import kotlinx.coroutines.tasks.await

/**
 * "Sign in with GitHub" via Firebase Auth — the Android counterpart of
 * [GoogleAuthRepository] / [AppleAuthRepository], differing only in the OAuth
 * provider id ("github.com").
 *
 * Android has no native GitHub sign-in API, so we reuse the same Firebase
 * OAuthProvider mechanism as Google/Apple: Firebase opens a Custom Tab against
 * GitHub's authorize endpoint, runs OAuth, and lands a Firebase ID token. That
 * token is POSTed to `/firebase/verify` (the backend's FirebaseTokenValidator path
 * — it validates any Firebase ID token by its `iss`/`aud` and reads the email,
 * regardless of which provider the user actually used). The backend's native
 * `/github/verify` endpoint does its own server-side code exchange and is used by
 * the web client; on Android the Firebase route is the idiomatic one and needs no
 * GitHub client secret on the device.
 *
 * Requires "GitHub" to be enabled as a sign-in provider in the Firebase console
 * (Authentication -> Sign-in method -> GitHub), with the GitHub OAuth app's
 * client id / secret configured there.
 */
class GithubAuthRepository(
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    suspend fun signInWithGithub(activity: Activity): AuthTokenResponse {
        val auth = FirebaseAuth.getInstance()

        val provider = OAuthProvider.newBuilder("github.com", auth).apply {
            // Match the web flow's scope so the verified token carries an email.
            scopes = listOf("read:user", "user:email")
        }.build()

        val authResult = auth.pendingAuthResult?.await()
            ?: auth.startActivityForSignInWithProvider(activity, provider).await()

        val firebaseUser = authResult.user
            ?: error("Firebase sign-in returned no user")
        val firebaseIdToken = firebaseUser.getIdToken(false).await().token
            ?: error("Firebase user has no ID token")

        val backendToken = api.verifyFirebaseToken(FirebaseVerifyRequest(token = firebaseIdToken)).unwrap()
        settings.setAccessToken(backendToken.accessToken)
        firebaseUser.email?.let { settings.setLastEmail(it) }

        // The backend JWT is authoritative; we don't keep a Firebase session.
        auth.signOut()

        return backendToken
    }
}
