#include "database/schema.hpp"

#include "platform/log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace mirobody { namespace database {

namespace {

//------------------------------------------------------------------------------

// The "*.sql" files in `dir`, sorted ascending by name so a numeric prefix
// orders them. Empty when the directory is absent or holds no matching files.
std::vector<std::string> list_sql_files(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    std::string pattern = dir + "\\*.sql";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files.push_back(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return files;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sql") == 0) {
            files.push_back(name);
        }
    }
    closedir(d);
#endif
    std::sort(files.begin(), files.end());
    return files;
}

//------------------------------------------------------------------------------

// Split a SQL script into its individual statements on top-level `;`, leaving
// any `;` inside single-quoted strings, double-quoted identifiers, dollar-quoted
// strings (`$$...$$` / `$tag$...$tag$`, as used by PL/pgSQL function bodies),
// line (`--`) or block (`/* */`) comments untouched. Statements consisting only
// of comments or whitespace (e.g. trailing text after the final `;`) are
// dropped; a leading comment is carried along with the statement it precedes,
// which the backend accepts.
std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> out;
    std::string cur;
    bool has_sql = false;   // any non-whitespace, non-comment content in `cur`

    // Lexer state. Lowercase names so they can't clash with <windows.h> macros.
    enum LexState { kNormal, kSquote, kDquote, kLine, kBlock, kDollar };
    LexState st = kNormal;
    std::string dollar_tag;   // active `$tag$` delimiter while st == kDollar

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c  = sql[i];
        const char nx = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

        switch (st) {
            case kNormal:
                if      (c == '\'')             { cur += c; st = kSquote; has_sql = true; }
                else if (c == '"')              { cur += c; st = kDquote; has_sql = true; }
                else if (c == '-' && nx == '-') { cur += c; cur += nx; ++i; st = kLine; }
                else if (c == '/' && nx == '*') { cur += c; cur += nx; ++i; st = kBlock; }
                else if (c == '$') {
                    // Possible dollar-quote opener `$tag$`: tag is empty or an
                    // identifier (letter/underscore start). `$1`, `$2` etc.
                    // (positional params — digit start) are NOT openers.
                    std::size_t j = i + 1;
                    while (j < sql.size() &&
                           (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) {
                        ++j;
                    }
                    const bool tag_ok = (j == i + 1) ||
                        !std::isdigit(static_cast<unsigned char>(sql[i + 1]));
                    if (j < sql.size() && sql[j] == '$' && tag_ok) {
                        dollar_tag.assign(sql, i, j - i + 1);   // includes both `$`
                        cur.append(dollar_tag);
                        i = j;                                  // resume after closing `$`
                        st = kDollar;
                        has_sql = true;
                    } else {
                        cur += c;
                        has_sql = true;
                    }
                }
                else if (c == ';') {
                    cur += c;
                    if (has_sql) out.push_back(cur);
                    cur.clear();
                    has_sql = false;
                } else {
                    cur += c;
                    if (!std::isspace(static_cast<unsigned char>(c))) has_sql = true;
                }
                break;
            case kSquote:
                cur += c;
                if (c == '\'') {
                    if (nx == '\'') { cur += nx; ++i; }   // escaped ''
                    else            st = kNormal;
                }
                break;
            case kDquote:
                cur += c;
                if (c == '"') st = kNormal;
                break;
            case kLine:
                cur += c;
                if (c == '\n') st = kNormal;
                break;
            case kBlock:
                cur += c;
                if (c == '*' && nx == '/') { cur += nx; ++i; st = kNormal; }
                break;
            case kDollar:
                // Opaque until the exact opening tag recurs. A different tag
                // ($inner$) inside is copied verbatim, matching PostgreSQL's
                // rule that only the matching delimiter closes the literal.
                if (c == '$' &&
                    sql.compare(i, dollar_tag.size(), dollar_tag) == 0) {
                    cur.append(dollar_tag);
                    i += dollar_tag.size() - 1;
                    st = kNormal;
                } else {
                    cur += c;
                }
                break;
        }
    }
    if (has_sql) out.push_back(cur);   // trailing statement without a ';'
    return out;
}

}  // namespace

//------------------------------------------------------------------------------

void apply_schema(Database& db, const std::string& dir) {
    std::vector<std::string> files = list_sql_files(dir);
    if (files.empty()) {
        platform::log_warn("schema: no .sql files in %s; skipping database init",
                           dir.c_str());
        return;
    }

    for (std::size_t f = 0; f < files.size(); ++f) {
        std::string path = dir;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
        path += files[f];

        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) {
            platform::log_warn("schema: cannot open %s; skipping", path.c_str());
            continue;
        }
        std::string sql((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

        std::vector<std::string> stmts = split_statements(sql);
        for (std::size_t s = 0; s < stmts.size(); ++s) {
            db.execute(stmts[s]);
        }
        platform::log_info("schema: applied %s (%zu statement%s)",
                           files[f].c_str(), stmts.size(),
                           stmts.size() == 1 ? "" : "s");
    }
}

}}
