package ai.thetahealth.mirobody.ui.chat

import ai.thetahealth.mirobody.R
import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.drawable.BitmapDrawable
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import coil.compose.AsyncImage
import coil.imageLoader
import coil.request.ImageRequest
import coil.request.SuccessResult
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Full-screen image viewer with pinch / double-tap zoom and a "save to gallery" action.
 *
 *  - Pinch with two fingers, or double-tap, to zoom; drag with one finger to pan when
 *    zoomed in. Pan resets when zoom returns to 1.0.
 *  - Close button (top-left) and hardware back dismiss the dialog.
 *  - Download button (top-right) inserts a PNG into Pictures/Mirobody via [MediaStore].
 *    On Android < Q this first requests `WRITE_EXTERNAL_STORAGE`; on Q+ no permission
 *    is required.
 */
@Composable
fun ImageViewerDialog(url: String, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbarHost = remember { SnackbarHostState() }
    val savedMsg = stringResource(R.string.chat_image_saved)
    val failedMsg = stringResource(R.string.chat_image_save_failed)
    val permDeniedMsg = stringResource(R.string.chat_image_save_permission_denied)
    val closeCd = stringResource(R.string.common_close)
    val saveCd = stringResource(R.string.common_save)

    val saveImpl: () -> Unit = {
        scope.launch {
            val ok = saveImageToGallery(context, url)
            snackbarHost.showSnackbar(if (ok) savedMsg else failedMsg)
        }
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) saveImpl()
        else scope.launch { snackbarHost.showSnackbar(permDeniedMsg) }
    }
    val onSave: () -> Unit = {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            permissionLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        } else {
            saveImpl()
        }
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color.Black),
        ) {
            ZoomableAsyncImage(
                url = url,
                modifier = Modifier.fillMaxSize(),
            )
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(onClick = onDismiss) {
                    Icon(
                        imageVector = Icons.Outlined.Close,
                        contentDescription = closeCd,
                        tint = Color.White,
                    )
                }
                Spacer(modifier = Modifier.weight(1f))
                IconButton(onClick = onSave) {
                    Icon(
                        imageVector = Icons.Outlined.Download,
                        contentDescription = saveCd,
                        tint = Color.White,
                    )
                }
            }
            SnackbarHost(
                hostState = snackbarHost,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(16.dp),
            )
        }
    }
}

@Composable
private fun ZoomableAsyncImage(url: String, modifier: Modifier = Modifier) {
    var scale by remember(url) { mutableFloatStateOf(1f) }
    var offset by remember(url) { mutableStateOf(Offset.Zero) }
    AsyncImage(
        model = ImageRequest.Builder(LocalContext.current)
            .data(chatImageModel(url))
            .crossfade(true)
            .build(),
        contentDescription = null,
        contentScale = ContentScale.Fit,
        modifier = modifier
            .pointerInput(url) {
                detectTransformGestures { _, pan, zoom, _ ->
                    scale = (scale * zoom).coerceIn(MIN_SCALE, MAX_SCALE)
                    offset = if (scale > 1f) offset + pan else Offset.Zero
                }
            }
            .pointerInput(url) {
                detectTapGestures(
                    onDoubleTap = {
                        if (scale > 1f) {
                            scale = 1f
                            offset = Offset.Zero
                        } else {
                            scale = 2.5f
                        }
                    },
                )
            }
            .graphicsLayer(
                scaleX = scale,
                scaleY = scale,
                translationX = offset.x,
                translationY = offset.y,
            ),
    )
}

private const val MIN_SCALE = 1f
private const val MAX_SCALE = 5f

/**
 * Resolves an `image` SSE chunk to a Coil-compatible model.
 *
 * Raw SVG XML (e.g. `<svg>…</svg>` returned inline as the chunk content) is converted
 * to a byte array so Coil's registered `SvgDecoder` can pick it up via the first-1024-
 * byte `<svg` sniff. Everything else — http(s) URLs, `data:` URIs, `content://` — is
 * passed through unchanged so Coil's standard fetchers handle it.
 */
internal fun chatImageModel(content: String): Any {
    val trimmed = content.trimStart()
    return if (trimmed.startsWith("<svg", ignoreCase = true) || trimmed.startsWith("<?xml")) {
        content.toByteArray()
    } else {
        content
    }
}

/**
 * Executes the Coil request (uses the shared cache) and writes the resulting bitmap to
 * MediaStore as PNG. Returns true on success.
 */
private suspend fun saveImageToGallery(context: Context, url: String): Boolean =
    withContext(Dispatchers.IO) {
        val result = runCatching {
            val request = ImageRequest.Builder(context)
                .data(chatImageModel(url))
                .allowHardware(false)
                .build()
            context.imageLoader.execute(request)
        }.getOrNull()
        val bitmap = (result as? SuccessResult)?.drawable?.let { it as? BitmapDrawable }?.bitmap
            ?: return@withContext false

        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, "mirobody_${System.currentTimeMillis()}.png")
            put(MediaStore.MediaColumns.MIME_TYPE, "image/png")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(
                    MediaStore.MediaColumns.RELATIVE_PATH,
                    "${Environment.DIRECTORY_PICTURES}/Mirobody",
                )
            }
        }
        val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: return@withContext false
        runCatching {
            resolver.openOutputStream(uri)?.use { os ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, os)
            }
        }.isSuccess
    }
