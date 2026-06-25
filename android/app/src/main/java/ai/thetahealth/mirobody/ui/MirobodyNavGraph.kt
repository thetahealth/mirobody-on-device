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
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import ai.thetahealth.mirobody.ui.auth.EmailScreen
import ai.thetahealth.mirobody.ui.auth.VerifyScreen
import ai.thetahealth.mirobody.ui.chat.ChatScreen
import ai.thetahealth.mirobody.ui.settings.BaseUrlScreen
import java.net.URLDecoder
import java.net.URLEncoder

object Routes {
    const val SPLASH = "splash"
    const val BASE_URL = "settings/baseurl"
    const val EMAIL = "auth/email"
    const val VERIFY = "auth/verify/{email}"
    const val CHAT = "chat"

    fun verify(email: String) = "auth/verify/${URLEncoder.encode(email, "UTF-8")}"
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
                        route == Routes.EMAIL ||
                        route == Routes.VERIFY
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
                onCodeSent = { email -> nav.navigate(Routes.verify(email)) },
                onSignedIn = {
                    nav.navigate(Routes.CHAT) {
                        popUpTo(Routes.EMAIL) { inclusive = true }
                    }
                },
            )
        }

        composable(
            route = Routes.VERIFY,
            arguments = listOf(navArgument("email") { type = NavType.StringType }),
        ) { entry ->
            val email = URLDecoder.decode(
                entry.arguments?.getString("email").orEmpty(),
                "UTF-8",
            )
            VerifyScreen(
                email = email,
                onVerified = {
                    nav.navigate(Routes.CHAT) {
                        popUpTo(Routes.EMAIL) { inclusive = true }
                    }
                },
                onBack = { nav.popBackStack() },
            )
        }

        composable(Routes.CHAT) {
            ChatScreen(
                onSignOut = {
                    nav.navigate(Routes.EMAIL) {
                        popUpTo(Routes.CHAT) { inclusive = true }
                    }
                },
            )
        }
    }
}
