package ai.thetahealth.mirobody.ui.chat

import android.content.Context
import androidx.annotation.StringRes
import ai.thetahealth.mirobody.R

/**
 * Slash commands — text the composer answers by itself, without a turn.
 *
 * `/help` renders the built-in guide (assets/help/), which is written as a model reply on
 * purpose: it is the user's documentation AND a live check of every construct the
 * renderer claims to support. A formatting regression shows up the first time anyone
 * types /help, without a model, a network, or a health permission.
 *
 * A command NEVER reaches a model. It produces a message flagged [ChatMessage.local],
 * which is dropped when the on-device transcript is built and when history is written —
 * so a help document several thousand characters long costs nothing in context on the
 * next turn, and never lands in the saved conversation.
 *
 * Ported from HarmonyOS `core/SlashCommand.ets`; the two clients offer the same three.
 */
const val SLASH_HELP = "/help"
const val SLASH_NEW = "/new"
const val SLASH_INCOGNITO = "/incognito"

/**
 * The developer probe (see `DebugProbeDialog`).
 *
 * DELIBERATELY UNDOCUMENTED: it is a developer tool — device facts and a benchmark runner
 * — and listing it in user-facing help would offer a debugging screen as a feature. A
 * slash command is the right shape for exactly that: reachable by whoever knows the word,
 * invisible to everyone else, and nothing in the navigation to explain. Same word and same
 * reasoning as HarmonyOS's `/probe`, so the two clients keep one vocabulary.
 */
const val SLASH_PROBE = "/probe"

/** One row of the command palette. */
data class SlashSpec(val name: String, @param:StringRes val desc: Int)

/**
 * The commands the palette OFFERS, in the order it lists them.
 *
 * `/help` leads because the palette is the discovery mechanism: whoever opened it by
 * typing a slash is exploring, and the first row should be the one that explains the
 * rest.
 *
 * Nothing here REPLACES a menu item. Every one of these is also a control in the drawer,
 * and stays one: a command line is a shortcut for people who like them, not a syntax to
 * memorize because the buttons went away.
 */
val SLASH_MENU: List<SlashSpec> = listOf(
    SlashSpec(SLASH_HELP, R.string.slash_help_desc),
    SlashSpec(SLASH_NEW, R.string.slash_new_desc),
    SlashSpec(SLASH_INCOGNITO, R.string.slash_incognito_desc),
)

object SlashCommand {

    private val cache = HashMap<String, String>()

    /**
     * The palette's rows for a draft, or [] when the draft is not a command being typed.
     *
     * Opens on a leading `/` and narrows by prefix as the user types, so `/n` shows one
     * row. It closes as soon as a space appears: past that the user is writing a sentence
     * that happens to start with a slash, not choosing a command.
     */
    fun suggest(draft: String): List<SlashSpec> {
        val t = draft.trimStart()
        if (!t.startsWith("/") || t.contains(' ') || t.contains('\n')) return emptyList()
        val q = t.lowercase()
        return SLASH_MENU.filter { it.name.startsWith(q) }
    }

    /**
     * The command this draft IS, or "".
     *
     * Matched on the WHOLE trimmed message, so a line that merely mentions "/help"
     * mid-sentence still goes to the model — a user asking "what does /help do" is asking
     * a question, not running one.
     */
    fun match(text: String): String {
        val t = text.trim().lowercase()
        SLASH_MENU.firstOrNull { it.name == t }?.let { return it.name }
        // Not in the palette, still runnable — that IS the distinction.
        return if (t == SLASH_PROBE) SLASH_PROBE else ""
    }

    /**
     * The guide's markdown, in the app's language.
     *
     * Assets carry no locale qualifiers, so the file is picked by name rather than
     * resolved by the resource system — hence the explicit tag test. Cached because the
     * document is a few KB and a user who types /help twice should not pay for it twice.
     */
    fun help(context: Context, language: String): String {
        val name = if (language.startsWith("zh")) "help-zh.md" else "help-en.md"
        cache[name]?.let { return it }
        val text = runCatching {
            context.assets.open("help/$name").bufferedReader().use { it.readText() }
        }.getOrDefault("")
        cache[name] = text
        return text
    }
}
