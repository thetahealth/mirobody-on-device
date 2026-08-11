package ai.thetahealth.mirobody.data.llm

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The catalog is hand-transcribed from Hugging Face's tree API, and a single wrong
 * character in a digest is invisible until a user watches a model they already have
 * download itself again. These assertions are cheap and catch exactly that class of typo.
 */
class OnDeviceCatalogTest {

    private val hex = Regex("^[0-9a-f]{64}$")

    @Test
    fun `every catalog entry carries a well-formed sha256`() {
        for (spec in OnDeviceModel.ALL) {
            assertTrue(
                "${spec.id}: sha256 must be 64 lowercase hex chars, was '${spec.sha256}'",
                hex.matches(spec.sha256),
            )
        }
    }

    @Test
    fun `sizes are real byte counts, not estimates`() {
        for (spec in OnDeviceModel.ALL) {
            // Every shipped model is well over 100 MB; a "0" or a truncated literal is
            // the failure this catches.
            assertTrue("${spec.id}: implausible size ${spec.approxBytes}", spec.approxBytes > 100_000_000L)
        }
    }

    @Test
    fun `ids, filenames and digests are unique`() {
        val ids = OnDeviceModel.ALL.map { it.id }
        val files = OnDeviceModel.ALL.map { it.fileName }
        val digests = OnDeviceModel.ALL.map { it.sha256 }
        assertEquals("duplicate id", ids.size, ids.toSet().size)
        // Two entries sharing a filename would fight over the same path on disk.
        assertEquals("duplicate fileName", files.size, files.toSet().size)
        // A copy-paste that left two entries with the same digest would make one of them
        // permanently unverifiable.
        assertEquals("duplicate sha256", digests.size, digests.toSet().size)
    }

    @Test
    fun `every entry downloads from Hugging Face over https`() {
        // Not pinned to one org any more: GGUF conversions come from the community
        // (unsloth and friends), while .litertlm still comes from litert-community.
        // The host is what matters — it is where the digests above were read from.
        for (spec in OnDeviceModel.ALL) {
            assertTrue(
                "${spec.id}: unexpected host in ${spec.downloadUrl}",
                spec.downloadUrl.startsWith("https://huggingface.co/"),
            )
        }
    }

    @Test
    fun `a runtime and its file extension agree`() {
        // The engines do not read each other's formats, and the extension is the only
        // thing a user sees; a mismatch here loads nothing and blames the model.
        for (spec in OnDeviceModel.ALL) {
            val expected = when (spec.runtime) {
                OnDeviceRuntime.LITERT_LM -> ".litertlm"
                OnDeviceRuntime.LLAMA_CPP -> ".gguf"
            }
            assertTrue(
                "${spec.id}: ${spec.runtime} cannot load ${spec.fileName}",
                spec.fileName.endsWith(expected),
            )
        }
    }

    @Test
    fun `the catalog is ordered smallest to largest`() {
        // The file's own contract — low-memory devices should meet the light options
        // first — and the thing an inserted entry silently breaks.
        val sizes = OnDeviceModel.CATALOG.map { it.approxBytes }
        assertEquals(sizes.sorted(), sizes)
    }

    @Test
    fun `an imported spec carries no digest`() {
        val spec = OnDeviceModel.importedSpec(
            id = "imported-x", displayName = "mine",
            path = "/sdcard/Download/mine.litertlm", sizeBytes = 123L,
        )
        // Empty is what makes ModelManager fall back to a presence check instead of
        // measuring the user's own file against a digest nobody published.
        assertEquals("", spec.sha256)
        assertTrue(spec.isImported)
    }

    @Test
    fun `the catalog offers only what the chat path can actually run`() {
        // CATALOG feeds the provider picker, so everything in it must be able to hold a
        // conversation today. Both runtimes now can — OnDeviceEngines routes on
        // spec.runtime and each has a streaming engine behind it — so the rule left is
        // about accelerator builds: a GPU file is the same weights as its CPU twin, and
        // two entries named "Gemma 4 E2B" differing by an acronym is a worse chooser
        // than one. Those stay in DEBUG_MODELS, where comparing them is the point.
        assertTrue(OnDeviceModel.CATALOG.none { it.backend == OnDeviceBackend.GPU })
    }

    @Test
    fun `every runtime the catalog names has an engine behind it`() {
        // The real invariant the LLAMA_CPP exclusion used to stand in for. It is a
        // compile-time check by construction: OnDeviceEngines maps the enum exhaustively,
        // so a new OnDeviceRuntime breaks the build rather than the picker. This asserts
        // the other half — that nothing in the catalog names a runtime we do not route.
        val routed = setOf(OnDeviceRuntime.LITERT_LM, OnDeviceRuntime.LLAMA_CPP)
        for (spec in OnDeviceModel.ALL) {
            assertTrue("${spec.id}: unrouted runtime ${spec.runtime}", spec.runtime in routed)
        }
    }

    @Test
    fun `debug models are measured, never offered, and always resolvable`() {
        val offered = OnDeviceModel.CATALOG.map { it.id }.toSet()
        for (spec in OnDeviceModel.DEBUG_MODELS) {
            assertTrue("${spec.id} is in both lists", spec.id !in offered)
            // Resolvable or the engine cannot load what the probe just benchmarked.
            assertEquals(spec, OnDeviceModel.byId(spec.id))
        }
    }
}
