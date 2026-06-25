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
        container = AppContainer(applicationContext)
        Coil.setImageLoader(
            ImageLoader.Builder(this)
                .components { add(SvgDecoder.Factory()) }
                .build()
        )
    }
}