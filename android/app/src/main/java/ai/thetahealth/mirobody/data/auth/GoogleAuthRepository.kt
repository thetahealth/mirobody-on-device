package ai.thetahealth.mirobody.data.auth

import ai.thetahealth.mirobody.data.net.unwrap
import ai.thetahealth.mirobody.data.settings.SettingsStore
import android.app.Activity
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.OAuthProvider
import kotlinx.coroutines.tasks.await

/**
 * Web-flow Google sign-in via Firebase Auth.
 *
 * The flow:
 *  1. Use the default [FirebaseAuth] — the default [com.google.firebase.FirebaseApp] is
 *     initialized at process start by `FirebaseInitProvider`, reading the bundled
 *     `app/google-services.json` via the `com.google.gms.google-services` Gradle plugin.
 *  2. Call `FirebaseAuth.startActivityForSignInWithProvider("google.com", activity)` —
 *     Firebase opens a Custom Tab against `accounts.google.com`, runs OAuth, redirects
 *     back via `https://<projectId>.firebaseapp.com/__/auth/handler` and lands the
 *     result via internal Activity result handling.
 *  3. Fetch the Firebase ID token (audience = Firebase project ID).
 *  4. POST that token to the backend's `/firebase/verify`, which routes it through
 *     `_firebase_validator` and returns a mirobody JWT.
 *  5. Sign out of Firebase locally — we only used it as a one-shot token issuer; the
 *     backend JWT is the authoritative session.
 *
 * This path does NOT depend on Google Play Services at runtime: the OAuth flow runs
 * entirely in a browser Custom Tab.
 */
class GoogleAuthRepository(
    private val api: AuthApi,
    private val settings: SettingsStore,
) {
    suspend fun signInWithGoogle(activity: Activity): AuthTokenResponse {
        val auth = FirebaseAuth.getInstance()

        val authResult = auth.pendingAuthResult?.await()
            ?: auth.startActivityForSignInWithProvider(
                activity,
                OAuthProvider.newBuilder("google.com", auth).build(),
            ).await()

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
