package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import android.app.Activity
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.OAuthProvider
import kotlinx.coroutines.tasks.await

/**
 * "Sign in with X" (formerly Twitter) via Firebase Auth — the Android
 * counterpart of [GoogleAuthRepository] / [GithubAuthRepository], differing only
 * in the OAuth provider id ("twitter.com", Firebase's built-in id for X).
 *
 * Android has no native X sign-in API, so we reuse the same Firebase
 * OAuthProvider mechanism as Google/GitHub: Firebase opens a Custom Tab against
 * X's authorize endpoint, runs OAuth, and lands a Firebase ID token. That token
 * is POSTed to `/firebase/verify` (the backend's FirebaseTokenValidator path — it
 * validates any Firebase ID token by its `iss`/`aud` and reads the email,
 * regardless of which provider the user actually used). There is no native
 * `/x/verify`: every platform uses this Firebase route, so a user gets one
 * account keyed by the same email everywhere.
 *
 * Requires "Twitter" to be enabled as a sign-in provider in the Firebase console
 * (Authentication -> Sign-in method -> Twitter), with the X app's API key/secret
 * configured there and "Request email address from users" turned on (X only
 * releases an email then).
 */
class XAuthRepository(
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    suspend fun signInWithX(activity: Activity): AuthTokenResponse {
        val auth = FirebaseAuth.getInstance()

        val provider = OAuthProvider.newBuilder("twitter.com", auth).build()

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
