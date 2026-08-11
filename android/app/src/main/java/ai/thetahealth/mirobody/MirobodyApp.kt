package ai.thetahealth.mirobody

import android.app.Application
import ai.thetahealth.mirobody.di.AppContainer
import coil.Coil
import coil.ImageLoader
import coil.decode.SvgDecoder

class MirobodyApp : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        // Before anything can touch llama.cpp. The engine dlopens one CPU backend out of
        // several -- whichever scores highest on this chip -- and it can only find them
        // if it is told where the app's libraries were unpacked.
        NativeBridge().localBackendPath(applicationInfo.nativeLibraryDir)
        container = AppContainer(applicationContext)
        Coil.setImageLoader(
            ImageLoader.Builder(this)
                .components { add(SvgDecoder.Factory()) }
                .build()
        )
    }
}