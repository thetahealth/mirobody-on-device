// URI builder for the SQLite config struct declared in database.hpp. SQLite is
// the only backend this repo links: the phone keeps its record in one file.

#include "database/database.hpp"

#include <string>

namespace mirobody { namespace database {

Database SQLiteConfig::open() const {
    return Database(path.empty() ? std::string(":memory:") : path);
}

} }
