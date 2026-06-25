package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import android.app.Activity
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.OAuthProvider
import kotlinx.coroutines.tasks.await

/**
 * Web-flow "Sign in with Apple" via Firebase Auth — the Android counterpart of
 * [GoogleAuthRepository], differing only in the OAuth provider id ("apple.com").
 *
 * Android has no native Apple sign-in API, so we reuse the same Firebase
 * OAuthProvider mechanism as Google: Firebase opens a Custom Tab against Apple's
 * authorize endpoint, runs OAuth, and lands a Firebase ID token. That token is
 * POSTed to `/firebase/verify` (the backend's FirebaseTokenValidator path — it
 * validates any Firebase ID token by its `iss`/`aud` and reads the email,
 * regardless of which provider the user actually used). The backend's native
 * `/apple/verify` endpoint is for clients that obtain a *real Apple* id_token
 * (the iOS app and the web SDK); on Android the Firebase route is the idiomatic
 * one and needs no Apple Services ID on the device.
 *
 * Requires "Apple" to be enabled as a sign-in provider in the Firebase console
 * (Authentication -> Sign-in method -> Apple), with the Services ID / key
 * configured there.
 */
class AppleAuthRepository(
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    suspend fun signInWithApple(activity: Activity): AuthTokenResponse {
        val auth = FirebaseAuth.getInstance()

        val provider = OAuthProvider.newBuilder("apple.com", auth).apply {
            // Ask Apple for the email scope so the verified token carries one.
            scopes = listOf("email", "name")
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
