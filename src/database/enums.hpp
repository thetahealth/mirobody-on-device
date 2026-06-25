#pragma once

#include <cstdint>

namespace mirobody { namespace database {

// Domain enums persisted as small integers in the database. The DB columns are
// bare SMALLINT with no CHECK/foreign key: every write goes through the C++
// server (all language bindings call in via the C-ABI), so these enums are the
// single source of truth for which integers are valid. Storing integers keeps
// the value identical across all SQL dialects (pg/mysql/sqlite/clickhouse/duckdb)
// and is friendly to downstream numpy/pandas analytics.
//
// RULES:
//   - NEVER renumber an existing value -- stored rows carry the old integer.
//   - Only append new values; keep 0 reserved for "unknown/unset".

//-----------------------------------------------------------------------------

// Login method / identity provider. Stored in user_login_logs.login_method and
// user_identities.login_method. Adding a provider needs integration code anyway
// (a deploy), so the set lives here rather than in a runtime lookup table; which
// methods are *offered* on the login screen is an app-config concern, not the DB.
enum class LoginMethod : int16_t {
    Unknown = 0,
    Email   = 1,
    Apple   = 2,
    Google  = 3,
    GitHub  = 4,
    X       = 5,
    WeChat  = 6,
    Tanka   = 7,
};

//-----------------------------------------------------------------------------

// MCP primitive kind. Stored in user_mcp_access_logs.kind.
enum class McpKind : int16_t {
    Unknown  = 0,
    Tool     = 1,
    Resource = 2,
    Prompt   = 3,
    Skill    = 4,
};

//-----------------------------------------------------------------------------

// Chat message author role. Stored in messages.role. Mirrors the role strings
// the LLM clients carry on llm::ChatMessage ("user" / "assistant" / "system" /
// "tool"); persisted as the integer here so the column is dialect-uniform.
enum class MessageRole : int16_t {
    Unknown   = 0,
    User      = 1,
    Assistant = 2,
    System    = 3,
    Tool      = 4,
};

//-----------------------------------------------------------------------------

// Long-term memory category. Stored in memories.kind. Mirrors the `remember`
// tool's "fact | preference | episode" tag; persisted as the integer so the
// column is dialect-uniform. The local backend maps the tool's string to/from
// this; the remote/mem0/zep backends keep the string (their external contract).
enum class MemoryKind : int16_t {
    Unknown    = 0,
    Fact       = 1,
    Preference = 2,
    Episode    = 3,
};

}}
