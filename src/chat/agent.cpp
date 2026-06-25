#include "chat/agent.hpp"

#include "platform/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace mirobody { namespace chat {

namespace {

// Decode the next UTF-8 code point from s[i..], advancing i past it. Returns
// the code point, or 0xFFFD on a malformed sequence (advancing one byte so the
// scan always makes progress).
std::uint32_t next_codepoint(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);

    std::size_t extra;
    std::uint32_t cp;
    if (c < 0x80)        { ++i; return c; }
    else if ((c >> 5) == 0x06) { cp = c & 0x1F; extra = 1; }
    else if ((c >> 4) == 0x0E) { cp = c & 0x0F; extra = 2; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
    else { ++i; return 0xFFFD; }

    for (std::size_t k = 1; k <= extra; ++k) {
        if (i + k >= s.size()) { ++i; return 0xFFFD; }
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc >> 6) != 0x02) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}

bool in_range(std::uint32_t cp, std::uint32_t lo, std::uint32_t hi) {
    return cp >= lo && cp <= hi;
}

// Turn an Accept-Language tag (already reduced to its first entry, e.g. "fr-FR",
// "zh-TW", "en") into a response-language fragment that slots into "Respond in
// <X>.". Known languages get a plain name; an unrecognized but non-empty tag is
// passed through by BCP-47 code so the model can still honor it. Empty in -> "".
std::string language_from_tag(const std::string& tag) {
    if (tag.empty()) return std::string();

    std::string lower = tag;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
    }
    const std::string primary = lower.substr(0, lower.find('-'));

    if (primary == "en") return "English";
    if (primary == "fr") return "French";
    if (primary == "es") return "Spanish";
    if (primary == "de") return "German";
    if (primary == "pt") return "Portuguese";
    if (primary == "it") return "Italian";
    if (primary == "nl") return "Dutch";
    if (primary == "ru") return "Russian";
    if (primary == "ja") return "Japanese";
    if (primary == "ko") return "Korean";
    if (primary == "ar") return "Arabic";
    if (primary == "he" || primary == "iw") return "Hebrew";   // "iw" is the legacy code
    if (primary == "zh") {
        // Traditional for Taiwan / Hong Kong / Macau or an explicit Hant tag.
        if (lower.find("tw") != std::string::npos || lower.find("hk") != std::string::npos ||
            lower.find("mo") != std::string::npos || lower.find("hant") != std::string::npos) {
            return "Traditional Chinese";
        }
        return "Simplified Chinese";
    }
    // Unknown: let the model interpret the BCP-47 tag itself.
    return "the language with code \"" + tag + "\"";
}

}   // namespace

//------------------------------------------------------------------------------
// detect_language
//------------------------------------------------------------------------------

std::string detect_language(const std::string& text, const std::string& accept_language) {
    const std::string pref = language_from_tag(accept_language);

    // No text (e.g. an upload-only turn): fall back to the client's preferred
    // language if it advertised one, else let the model decide (empty).
    if (text.empty()) return pref;

    for (std::size_t i = 0; i < text.size(); ) {
        const std::uint32_t cp = next_codepoint(text, i);

        // Japanese kana (Hiragana / Katakana).
        if (in_range(cp, 0x3040, 0x309F) || in_range(cp, 0x30A0, 0x30FF)) {
            return "Japanese";
        }
        // Korean Hangul syllables.
        if (in_range(cp, 0xAC00, 0xD7AF)) {
            return "Korean";
        }
        // CJK unified ideographs (treated as Simplified Chinese, per the
        // Python reference -- kana is checked first so Japanese wins on mixed).
        if (in_range(cp, 0x3400, 0x4DBF) || in_range(cp, 0x4E00, 0x9FFF) ||
            in_range(cp, 0xF900, 0xFAFF)) {
            return "Simplified Chinese";
        }
        // Arabic.
        if (in_range(cp, 0x0600, 0x06FF) || in_range(cp, 0x0750, 0x077F) ||
            in_range(cp, 0x08A0, 0x08FF)) {
            return "Arabic";
        }
        // Hebrew.
        if (in_range(cp, 0x0590, 0x05FF)) {
            return "Hebrew";
        }
        // Cyrillic.
        if (in_range(cp, 0x0400, 0x04FF) || in_range(cp, 0x0500, 0x052F)) {
            return "Russian";
        }
    }

    // Latin-script: mirror the user's message. If the client advertised a
    // preferred language, use it to disambiguate a short / ambiguous message.
    if (!pref.empty()) {
        return "the same language as the user's message (```" + text +
               "```); if its language is unclear, default to " + pref;
    }
    return "the same language as the user's message: ```" + text + "```";
}

//------------------------------------------------------------------------------
// AgentRegistry
//------------------------------------------------------------------------------

bool AgentRegistry::add(const AgentRegistration& reg) {
    if (reg.name.empty()) {
        platform::log_warn("agent: ignoring registration with empty name");
        return true;
    }
    if (index_.find(reg.name) != index_.end()) {
        platform::log_warn("agent: duplicate agent '%s' ignored", reg.name.c_str());
        return true;
    }
    index_[reg.name] = agents_.size();
    agents_.push_back(reg);
    platform::log_info("agent: registered '%s'%s", reg.name.c_str(),
                       reg.is_public ? "" : " (private)");
    return true;
}

const AgentRegistration* AgentRegistry::find(const std::string& name) const {
    std::unordered_map<std::string, std::size_t>::const_iterator it = index_.find(name);
    if (it == index_.end()) return nullptr;
    return &agents_[it->second];
}

std::vector<std::string> AgentRegistry::names(bool public_only) const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        if (public_only && !agents_[i].is_public) continue;
        out.push_back(agents_[i].name);
    }
    return out;
}

std::string AgentRegistry::first_name(bool public_only) const {
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        if (public_only && !agents_[i].is_public) continue;
        return agents_[i].name;
    }
    return std::string();
}

std::vector<std::string> AgentRegistry::provider_names(bool public_only) const {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        const AgentRegistration& reg = agents_[i];
        if (public_only && !reg.is_public) continue;

        std::unordered_map<std::string, ClientMap>::const_iterator it = clients_.find(reg.name);
        if (it == clients_.end()) continue;
        for (ClientMap::const_iterator p = it->second.begin(); p != it->second.end(); ++p) {
            out.push_back(reg.name + "/" + p->first);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void AgentRegistry::load_clients(const Config& cfg) {
    clients_.clear();
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        const AgentRegistration& reg = agents_[i];
        if (!reg.load_clients) continue;

        ClientMap map = reg.load_clients(cfg);
        platform::log_info("agent: loaded %zu provider client(s) for '%s'",
                           map.size(), reg.name.c_str());
        clients_[reg.name] = std::move(map);
    }
}

std::shared_ptr<llm::Client> AgentRegistry::client(const std::string& agent_name,
                                                   const std::string& provider) const {
    std::unordered_map<std::string, ClientMap>::const_iterator a = clients_.find(agent_name);
    if (a == clients_.end()) return nullptr;
    ClientMap::const_iterator p = a->second.find(provider);
    if (p == a->second.end()) return nullptr;
    return p->second;
}

std::unique_ptr<Agent> AgentRegistry::create(const std::string& name,
                                             const AgentRequest& req) const {
    const AgentRegistration* reg = find(name);
    if (!reg || !reg->factory) return std::unique_ptr<Agent>();
    return reg->factory(req);
}

//------------------------------------------------------------------------------

AgentRegistry& agent_registry() {
    static AgentRegistry instance;
    return instance;
}

}}
