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

//-----------------------------------------------------------------------------

// Care-circle membership role. Stored in care_circle_members.role. The creator
// of a care_circles row is its Owner; the owner may promote members to
// Maintainer. Owner and Maintainer are admins (invite / remove members);
// renaming, deleting and role changes stay owner-only. Ordered by privilege so
// `role >= Maintainer` means "admin". Modern backends only (no legacy support).
enum class CircleRole : int16_t {
    Member     = 0,
    Maintainer = 1,
    Owner      = 2,
};

//-----------------------------------------------------------------------------

// Care-circle membership invite state. Stored in care_circle_members.status. An
// owner inviting someone creates the membership Pending; the invitee moves it to
// Accepted (they are then mutually in the circle with every other accepted
// member) or Declined. The owner's own membership is created Accepted.
enum class CircleStatus : int16_t {
    Unknown  = 0,
    Pending  = 1,
    Accepted = 2,
    Declined = 3,
};

//-----------------------------------------------------------------------------

// Conversation share access level. Stored in conversation_shares.access_level.
// Shared threads are read-only for now (View); Edit is reserved for later.
enum class ShareAccess : int16_t {
    Unknown = 0,
    View    = 1,
    Edit    = 2,
};

}}
