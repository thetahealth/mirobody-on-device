package ai.thetahealth.mirobody.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.first
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import ai.thetahealth.mirobody.ui.auth.EmailScreen
import ai.thetahealth.mirobody.ui.chat.ChatScreen
import ai.thetahealth.mirobody.ui.settings.BaseUrlScreen

object Routes {
    const val SPLASH = "splash"
    const val BASE_URL = "settings/baseurl"
    const val EMAIL = "auth/email"
    const val CHAT = "chat"
}

@Composable
fun MirobodyNavGraph() {
    val container = LocalAppContainer.current
    val nav = rememberNavController()

    // When the token is cleared mid-session (e.g. UnauthorizedInterceptor on a 401),
    // pop back to the email login screen. `drop(1)` skips the initial value so this
    // only fires on a genuine transition, not on app startup.
    LaunchedEffect(Unit) {
        container.tokenFlow
            .drop(1)
            .collect { token ->
                if (token.isNullOrBlank()) {
                    val route = nav.currentDestination?.route
                    val onPublicRoute = route == null ||
                        route == Routes.SPLASH ||
                        route == Routes.BASE_URL ||
                        route == Routes.EMAIL
                    if (!onPublicRoute) {
                        nav.navigate(Routes.EMAIL) {
                            popUpTo(0) { inclusive = true }
                        }
                    }
                }
            }
    }

    NavHost(navController = nav, startDestination = Routes.SPLASH) {
        composable(Routes.SPLASH) {
            LaunchedEffect(Unit) {
                // Move any old single-token slot into the per-account scheme before
                // the first token read decides where to land.
                container.settings.migrateLegacyIfNeeded()
                val baseUrl = container.settings.baseUrl.first()
                val token = container.settings.accessToken.first()
                val target = when {
                    baseUrl.isNullOrBlank() -> Routes.BASE_URL
                    token.isNullOrBlank() -> Routes.EMAIL
                    else -> Routes.CHAT
                }
                nav.navigate(target) {
                    popUpTo(Routes.SPLASH) { inclusive = true }
                }
            }
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
        }

        composable(Routes.BASE_URL) {
            val canCancel = nav.previousBackStackEntry != null
            BaseUrlScreen(
                onSaved = {
                    if (canCancel) {
                        nav.popBackStack()
                    } else {
                        nav.navigate(Routes.EMAIL) {
                            popUpTo(Routes.BASE_URL) { inclusive = true }
                        }
                    }
                },
                onCancel = if (canCancel) {
                    { nav.popBackStack() }
                } else null,
            )
        }

        composable(Routes.EMAIL) {
            EmailScreen(
                // Reset the stack to a single fresh chat -- covers both a first
                // login and an "Add account" login (which had CHAT in the back
                // stack). A fresh ChatScreen loads the now-current account's data.
                onSignedIn = {
                    nav.navigate(Routes.CHAT) {
                        popUpTo(0) { inclusive = true }
                    }
                },
            )
        }

        composable(Routes.CHAT) {
            ChatScreen(
                // Recreate the chat for the now-current account (after a switch or a
                // sign-out that fell back to another account).
                onRelaunch = {
                    nav.navigate(Routes.CHAT) {
                        popUpTo(0) { inclusive = true }
                    }
                },
                // Show login over the session to add an account; keep CHAT in the
                // back stack so system-back cancels. Signing out with no account
                // left nulls the token -> the collector above returns to login.
                onAddAccount = {
                    nav.navigate(Routes.EMAIL)
                },
            )
        }
    }
}
