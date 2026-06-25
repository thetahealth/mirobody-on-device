package ai.thetahealth.mirobody

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat

class MirobodyService : Service() {

    private val bridge = NativeBridge()
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        acquireWakeLock()

        val openaiKey = intent?.getStringExtra(EXTRA_OPENAI_KEY).orEmpty()
        val geminiKey = intent?.getStringExtra(EXTRA_GEMINI_KEY).orEmpty()
        val configPath = intent?.getStringExtra(EXTRA_CONFIG_PATH).orEmpty()
        val port = intent?.getIntExtra(EXTRA_LISTEN_PORT, DEFAULT_PORT) ?: DEFAULT_PORT

        val ok = bridge.start(
            configPath = configPath,
            dataDir = filesDir.absolutePath,
            openaiKey = openaiKey,
            geminiKey = geminiKey,
            listenPort = port,
        )
        if (!ok) {
            Log.e(TAG, "native server failed to start")
            stopSelf()
            return START_NOT_STICKY
        }
        Log.i(TAG, "native server listening on 127.0.0.1:${bridge.listenPort()}")
        return START_STICKY
    }

    override fun onDestroy() {
        bridge.stop()
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startForegroundCompat() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mirobody:server").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun buildNotification(): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Mirobody")
            .setContentText("Local gateway running")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    private fun createNotificationChannel() {
        val mgr = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Mirobody service",
            NotificationManager.IMPORTANCE_LOW,
        )
        mgr.createNotificationChannel(channel)
    }

    companion object {
        private const val TAG = "MirobodyService"
        private const val CHANNEL_ID = "mirobody_service"
        private const val NOTIFICATION_ID = 1001
        private const val DEFAULT_PORT = 8080

        const val EXTRA_OPENAI_KEY = "openai_key"
        const val EXTRA_GEMINI_KEY = "gemini_key"
        const val EXTRA_CONFIG_PATH = "config_path"
        const val EXTRA_LISTEN_PORT = "listen_port"
    }
}
