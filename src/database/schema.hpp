#pragma once

#include "database/database.hpp"

#include <string>

namespace mirobody { namespace database {

// Apply the DDL in `dir` to `db` at startup. Every "*.sql" file in the directory
// is read in filename order (so a numeric prefix like 01_, 02_ controls
// sequencing), split into individual statements, and executed one at a time --
// the backends run a single command per execute(), so multi-statement files
// must be split here. The shipped DDL is written with IF NOT EXISTS, so applying
// it repeatedly is a no-op and safe to run on every boot.
//
// A missing or empty directory is logged and treated as "nothing to do" rather
// than an error (a deployment may manage its schema out of band). A statement
// that fails to execute propagates as std::runtime_error so startup can abort.
void apply_schema(Database& db, const std::string& dir);

}}
