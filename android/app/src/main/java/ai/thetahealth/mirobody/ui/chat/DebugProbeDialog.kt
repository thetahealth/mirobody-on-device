package ai.thetahealth.mirobody.ui.chat

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import ai.thetahealth.mirobody.NativeBridge
import org.json.JSONObject
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.data.llm.BenchmarkRun
import ai.thetahealth.mirobody.data.llm.ModelManager
import ai.thetahealth.mirobody.data.llm.OnDeviceBackend
import ai.thetahealth.mirobody.data.llm.OnDeviceModel
import ai.thetahealth.mirobody.data.llm.OnDeviceModelSpec
import ai.thetahealth.mirobody.data.llm.OnDeviceRuntime
import ai.thetahealth.mirobody.data.llm.OnDeviceModelStatus
import ai.thetahealth.mirobody.data.llm.benchmarkOf
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import kotlinx.coroutines.launch

/**
 * The developer probe, reached only by typing `/probe` (see [SlashCommand]).
 *
 * DELIBERATELY UNDOCUMENTED, on the same reasoning HarmonyOS's `pages/NativeProbe.ets`
 * uses: a page of device facts and a benchmark runner is a debugging screen, and listing
 * it in the palette or the built-in help would offer it as a feature. Typing the word in
 * full still opens it, which is the whole distinction between undocumented and
 * unavailable.
 *
 * It answers the questions a spec sheet cannot: does `Backend.GPU()` do anything on THIS
 * phone, and is it faster than the CPU build of the same weights. Those are two downloads
 * of the same model compiled differently ([OnDeviceModel.DEBUG_MODELS]), measured with
 * LiteRT-LM's own `benchmark()` so prefill and decode are reported separately — they are
 * bound by different resources, and an accelerator can win one while losing the other.
 */
@Composable
fun DebugProbeDialog(models: ModelManager, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val results = remember { mutableStateMapOf<String, Result<BenchmarkRun>>() }
    var running by remember { mutableStateOf<String?>(null) }
    // Live, so a download's progress and the verify pass show without a refresh.
    val statuses by models.statuses.collectAsState()

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(color = MaterialTheme.colorScheme.background, modifier = Modifier.fillMaxSize()) {
            Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
                DialogTitleWithClose(stringResource(R.string.probe_title), onDismiss)
                Column(modifier = Modifier.verticalScroll(rememberScrollState())) {

                    ProbeSection(stringResource(R.string.probe_device))
                    for ((k, v) in deviceFacts(context)) ProbeFact(k, v)

                    Spacer(Modifier.height(16.dp))
                    ProbeFact(
                        "model cache",
                        "%.1f GB".format(models.cacheBytes() / 1024.0 / 1024.0 / 1024.0),
                    )

                    Spacer(Modifier.height(16.dp))
                    ProbeSection("llama.cpp")
                    // has_* is the silicon, built_* is what we compiled for. A cross
                    // build with the wrong -march silently drops ggml's fast kernels and
                    // nothing else reports it, so the pair is the point: `has` true with
                    // `built` false means this device is leaving speed on the table for
                    // want of a flag in android/build-llama.cmd.
                    for ((k, v) in remember { llamaFacts() }) ProbeFact(k, v)

                    Spacer(Modifier.height(16.dp))
                    ProbeSection(stringResource(R.string.probe_benchmark))
                    Text(
                        stringResource(R.string.probe_benchmark_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))

                    // Every model we know how to run, CPU builds and the GPU twin
                    // together, so the comparison is one screen rather than two visits.
                    for (spec in OnDeviceModel.ALL) {
                        BenchmarkRow(
                            spec = spec,
                            status = statuses[spec.id] ?: OnDeviceModelStatus.Absent,
                            busy = running == spec.id,
                            anyRunning = running != null,
                            result = results[spec.id],
                            onDownload = { scope.launch { models.download(spec) } },
                            onRun = {
                                running = spec.id
                                scope.launch {
                                    results[spec.id] = benchmarkOf(
                                        spec = spec,
                                        modelPath = models.fileFor(spec).absolutePath,
                                        cacheDir = models.cacheDirFor(spec).absolutePath,
                                    )
                                    running = null
                                }
                            },
                        )
                        HorizontalDivider(
                            color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f),
                        )
                    }
                    Spacer(Modifier.height(24.dp))
                }
            }
        }
    }
}

/** "LiteRT" / "llama.cpp" — the engine, said the way people say it. */
private fun OnDeviceRuntime.label(): String = when (this) {
    OnDeviceRuntime.LITERT_LM -> "LiteRT"
    OnDeviceRuntime.LLAMA_CPP -> "llama.cpp"
}

@Composable
private fun ProbeSection(label: String) {
    Text(
        text = label,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
    )
}

@Composable
private fun ProbeFact(key: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Text(
            text = key,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(0.42f),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            modifier = Modifier.weight(0.58f),
        )
    }
}

@Composable
private fun BenchmarkRow(
    spec: OnDeviceModelSpec,
    status: OnDeviceModelStatus,
    busy: Boolean,
    anyRunning: Boolean,
    result: Result<BenchmarkRun>?,
    onRun: () -> Unit,
    onDownload: () -> Unit,
) {
    val ready = status is OnDeviceModelStatus.Ready
    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(spec.displayName, style = MaterialTheme.typography.bodyMedium)
                Text(
                    text = spec.runtime.label() + " · " + spec.backend.name + " · " + when (status) {
                        is OnDeviceModelStatus.Ready -> stringResource(R.string.probe_on_disk)
                        is OnDeviceModelStatus.Downloading ->
                            if (status.totalBytes > 0) "%.0f%%".format(status.fraction * 100) else "…"
                        is OnDeviceModelStatus.Verifying -> "%.0f%%".format(status.fraction * 100)
                        is OnDeviceModelStatus.Failed -> status.message
                        is OnDeviceModelStatus.Absent ->
                            "%.1f GB".format(spec.approxBytes / 1024.0 / 1024.0 / 1024.0)
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = if (status is OnDeviceModelStatus.Failed) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            when {
                busy -> CircularProgressIndicator(modifier = Modifier.height(20.dp))
                // Only a downloaded model can be measured, and only one at a time: two
                // engines at once would each hold a couple of GB and measure the
                // contention rather than the backend.
                ready -> TextButton(onClick = onRun, enabled = !anyRunning) {
                    Text(stringResource(R.string.probe_run))
                }
                status is OnDeviceModelStatus.Downloading ||
                    status is OnDeviceModelStatus.Verifying -> Unit   // progress is the label
                // The GPU twin lives only here (it is deliberately out of the catalog,
                // see OnDeviceModel.DEBUG_MODELS), so without this button there is no
                // way to obtain it anywhere in the app.
                else -> TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.probe_download))
                }
            }
        }
        result?.fold(
            onSuccess = { r ->
                Column(
                    verticalArrangement = Arrangement.spacedBy(1.dp),
                    modifier = Modifier.padding(top = 6.dp),
                ) {
                    ProbeFact("init", "%.2f s".format(r.initSeconds))
                    ProbeFact("first token", "%.2f s".format(r.timeToFirstTokenSeconds))
                    ProbeFact("prefill", "%.1f tok/s  (%d tok)".format(r.prefillTokensPerSecond, r.prefillTokens))
                    ProbeFact("decode", "%.1f tok/s  (%d tok)".format(r.decodeTokensPerSecond, r.decodeTokens))
                }
            },
            onFailure = { t ->
                // A backend that cannot run here is the answer, not an error to hide.
                Text(
                    text = t.message ?: t::class.java.simpleName,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 6.dp),
                )
            },
        )
    }
}

/**
 * The native engine's own view of itself: is it compiled in, on what backend, and are we
 * getting the kernels this chip can run.
 *
 * Two shapes, because the question changed. A statically linked build has one answer
 * baked in, so the useful thing is chip against build. A GGML_CPU_ALL_VARIANTS build
 * chose at startup, so the useful thing is WHICH it chose — and an empty choice there is
 * a packaging bug that otherwise shows up only as everything being slow.
 */
private fun llamaFacts(): List<Pair<String, String>> = runCatching {
    val j = JSONObject(NativeBridge().localStatus())
    if (!j.optBoolean("available")) return@runCatching listOf("engine" to "not built in")
    val has = j.optJSONObject("has")
    val built = j.optJSONObject("built")
    val head = listOf(
        "engine" to j.optString("backend", "?"),
        "devices" to j.optString("devices", "?").ifBlank { "cpu" },
    )
    val keys = listOf("fp16", "dotprod", "i8mm", "sve")
    if (!j.optBoolean("dispatch")) {
        // One row per feature, "chip / build", so a mismatch is impossible to miss.
        return@runCatching head + ("feature (chip / build)" to "") + keys.map { k ->
            val h = has?.optBoolean(k) == true
            val b = built?.optBoolean(k) == true
            k to (if (h) "yes" else "no") + " / " + (if (b) "yes" else "no") +
                if (h && !b) "   ← unused" else ""
        }
    }
    val variant = j.optString("variant")
    head + ("kernels" to variant.ifBlank { "none loaded   ← modules missing" }) +
        ("feature (chip)" to "") +
        keys.map { k -> k to if (has?.optBoolean(k) == true) "yes" else "no" }
}.getOrElse { listOf("engine" to (it.message ?: "unavailable")) }

/**
 * What the device is, in the terms that explain a benchmark: the SoC decides which GPU
 * driver is present, the page size is the 16 KB question, and total RAM is what a 2–4 GB
 * model is competing with.
 */
private fun deviceFacts(context: Context): List<Pair<String, String>> {
    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
    val mem = ActivityManager.MemoryInfo().also { am?.getMemoryInfo(it) }
    val gb = { b: Long -> "%.1f GB".format(b / 1024.0 / 1024.0 / 1024.0) }
    return buildList {
        add("device" to "${Build.MANUFACTURER} ${Build.MODEL}")
        add("soc" to if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            listOf(Build.SOC_MANUFACTURER, Build.SOC_MODEL).filter { it.isNotBlank() }
                .joinToString(" ").ifBlank { Build.HARDWARE }
        } else Build.HARDWARE)
        add("abi" to Build.SUPPORTED_ABIS.joinToString(", "))
        add("android" to "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        add("ram" to gb(mem.totalMem) + " total, " + gb(mem.availMem) + " free")
        // The 16 KB-page question, answered by the device rather than by the build flags.
        add("page size" to "${runCatching { android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE) }.getOrDefault(-1L)} B")
    }
}
