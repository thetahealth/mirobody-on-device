#include "database/database.hpp"

// #include "duckdb.hpp"  // TODO: enable when implementing

#include <stdexcept>
#include <utility>

namespace mirobody { namespace database {

struct Database::Impl {
    // TODO: std::unique_ptr<duckdb::DuckDB> db;
    // TODO: std::unique_ptr<duckdb::Connection> conn;
};

//------------------------------------------------------------------------------

Database::Database(const std::string& uri) : impl_(new Impl) {
    (void)uri;
    throw std::runtime_error("DuckDB database backend: not implemented");
}

//------------------------------------------------------------------------------

Database::~Database() = default;

//------------------------------------------------------------------------------

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

//------------------------------------------------------------------------------

Result Database::execute(const std::string& sql,
                         const std::vector<Value>& params) {
    (void)sql;
    (void)params;
    throw std::runtime_error("DuckDB database backend: not implemented");
}

//------------------------------------------------------------------------------
// Transaction (stub -- this backend is not implemented)
//------------------------------------------------------------------------------

struct Transaction::Impl {};

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() = default;
Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&&) noexcept = default;

Result Transaction::execute(const std::string& sql,
                            const std::vector<Value>& params) {
    (void)sql;
    (void)params;
    throw std::runtime_error("DuckDB database backend: not implemented");
}

void Transaction::commit() {
    throw std::runtime_error("DuckDB database backend: not implemented");
}

Transaction Database::begin() {
    throw std::runtime_error("DuckDB database backend: not implemented");
}

}}
