// ClickHouse backend — stub.
//
// Selected by MIROBODY_DATABASE_BACKEND=CLICKHOUSE. A real implementation
// would build on clickhouse-cpp (https://github.com/ClickHouse/clickhouse-cpp)
// once that dependency is added to vcpkg.json / system packages and pulled
// in via find_package below. For now both the constructor and execute()
// throw so any binary built against this backend fails fast and loudly
// instead of pretending to work.
//
// TODO:
//   - find_package(clickhouse-cpp ...) in CMakeLists.txt and link it
//   - parse ClickHouseConfig::open()-style URIs (clickhouse://...)
//   - implement a ConnPool around clickhouse::Client similar to pg.cpp
//   - translate `?` placeholders to ClickHouse's `{name:Type}` named params
//   - map Value to clickhouse::ColumnRef and back for the Result rows

#include "database/database.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace mirobody { namespace database {

struct Database::Impl {};

//------------------------------------------------------------------------------

Database::Database(const std::string& /*uri*/) : impl_(new Impl) {
    throw std::runtime_error(
        "ClickHouse backend is not yet implemented. Rebuild with "
        "MIROBODY_DATABASE_BACKEND set to DUCKDB, POSTGRESQL, MYSQL, or SQLITE.");
}

//------------------------------------------------------------------------------

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

//------------------------------------------------------------------------------

Result Database::execute(const std::string& /*sql*/, const std::vector<Value>& /*params*/) {
    throw std::runtime_error("ClickHouse backend is not yet implemented.");
}

//------------------------------------------------------------------------------
// Transaction (stub -- this backend is not implemented)
//------------------------------------------------------------------------------

struct Transaction::Impl {};

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() = default;
Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&&) noexcept = default;

Result Transaction::execute(const std::string& /*sql*/, const std::vector<Value>& /*params*/) {
    throw std::runtime_error("ClickHouse backend is not yet implemented.");
}

void Transaction::commit() {
    throw std::runtime_error("ClickHouse backend is not yet implemented.");
}

Transaction Database::begin() {
    throw std::runtime_error("ClickHouse backend is not yet implemented.");
}

}}
