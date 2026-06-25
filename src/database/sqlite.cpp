#include "database/database.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace mirobody { namespace database {

namespace {

void throw_sqlite_error(sqlite3* db, const char* what) {
    std::string msg = what;
    msg += ": ";
    msg += sqlite3_errmsg(db);
    throw std::runtime_error(msg);
}

}

//------------------------------------------------------------------------------

struct Database::Impl {
    sqlite3* db = nullptr;
};

//------------------------------------------------------------------------------

Database::Database(const std::string& uri) : impl_(new Impl) {
    const int rc = sqlite3_open_v2(uri.c_str(), &impl_->db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                   nullptr);
    if (rc != SQLITE_OK) {
        const char* err = impl_->db ? sqlite3_errmsg(impl_->db) : "unknown error";
        std::string msg = std::string("sqlite3_open_v2 failed: ") + err;
        if (impl_->db) sqlite3_close(impl_->db);
        impl_->db = nullptr;
        throw std::runtime_error(msg);
    }
    // Best-effort WAL. If the platform's SQLite refuses (read-only filesystem,
    // a network filesystem that does not implement the locking primitives),
    // stay in the default journal mode rather than fail the open.
    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
}

//------------------------------------------------------------------------------

Database::~Database() {
    if (impl_ && impl_->db) {
        sqlite3_close(impl_->db);
    }
}

//------------------------------------------------------------------------------

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

//------------------------------------------------------------------------------

namespace {

// Run one statement on a given connection. Shared by Database::execute and
// Transaction -- both drive the single underlying sqlite3 handle.
Result exec_on_db(sqlite3* db,
                  const std::string& sql,
                  const std::vector<Value>& params) {
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                           &stmt, nullptr) != SQLITE_OK) {
        throw_sqlite_error(db, "sqlite3_prepare_v2");
    }

    struct StmtGuard {
        sqlite3_stmt* s;
        ~StmtGuard() { sqlite3_finalize(s); }
    } guard{stmt};

    for (std::size_t i = 0; i < params.size(); ++i) {
        const Value& v = params[i];
        const int idx = static_cast<int>(i + 1);
        int rc = SQLITE_OK;
        switch (v.type()) {
            case Value::Type::Null:
                rc = sqlite3_bind_null(stmt, idx);
                break;
            case Value::Type::Int:
                rc = sqlite3_bind_int64(stmt, idx, v.as_int());
                break;
            case Value::Type::Double:
                rc = sqlite3_bind_double(stmt, idx, v.as_double());
                break;
            case Value::Type::Text:
                rc = sqlite3_bind_text(stmt, idx, v.as_text().data(),
                                       static_cast<int>(v.as_text().size()),
                                       SQLITE_TRANSIENT);
                break;
            case Value::Type::Blob:
                rc = sqlite3_bind_blob(stmt, idx, v.as_blob().data(),
                                       static_cast<int>(v.as_blob().size()),
                                       SQLITE_TRANSIENT);
                break;
        }
        if (rc != SQLITE_OK) {
            throw_sqlite_error(db, "sqlite3_bind_*");
        }
    }

    Result result;
    const int ncols = sqlite3_column_count(stmt);
    if (ncols > 0) {
        result.columns.reserve(static_cast<std::size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            result.columns.emplace_back(name ? name : "");
        }
    }

    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            throw_sqlite_error(db, "sqlite3_step");
        }
        std::vector<Value> row;
        row.reserve(static_cast<std::size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            switch (sqlite3_column_type(stmt, c)) {
                case SQLITE_NULL:
                    row.emplace_back();
                    break;
                case SQLITE_INTEGER:
                    row.emplace_back(static_cast<std::int64_t>(
                        sqlite3_column_int64(stmt, c)));
                    break;
                case SQLITE_FLOAT:
                    row.emplace_back(sqlite3_column_double(stmt, c));
                    break;
                case SQLITE_TEXT: {
                    const unsigned char* p = sqlite3_column_text(stmt, c);
                    const int n = sqlite3_column_bytes(stmt, c);
                    row.emplace_back(std::string(
                        reinterpret_cast<const char*>(p),
                        static_cast<std::size_t>(n)));
                    break;
                }
                case SQLITE_BLOB: {
                    const void* p = sqlite3_column_blob(stmt, c);
                    const int n = sqlite3_column_bytes(stmt, c);
                    row.emplace_back(Value::blob(std::string(
                        static_cast<const char*>(p),
                        static_cast<std::size_t>(n))));
                    break;
                }
            }
        }
        result.rows.emplace_back(std::move(row));
    }

    result.rows_affected  = sqlite3_changes(db);
    result.last_insert_id = sqlite3_last_insert_rowid(db);

    return result;
}

}  // namespace

//------------------------------------------------------------------------------

Result Database::execute(const std::string& sql,
                         const std::vector<Value>& params) {
    return exec_on_db(impl_->db, sql, params);
}

//------------------------------------------------------------------------------
// Transaction
//------------------------------------------------------------------------------

// SQLite has a single connection, so the transaction just drives BEGIN / COMMIT
// / ROLLBACK on it. The dtor rolls back if commit() was not called.
struct Transaction::Impl {
    sqlite3* db     = nullptr;
    bool     active = false;   // true between BEGIN and commit()/rollback
    ~Impl() {
        if (active && db) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
};

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() = default;
Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&&) noexcept = default;

Result Transaction::execute(const std::string& sql,
                            const std::vector<Value>& params) {
    if (!impl_ || !impl_->active) {
        throw std::runtime_error("Transaction already finished");
    }
    return exec_on_db(impl_->db, sql, params);
}

void Transaction::commit() {
    if (!impl_ || !impl_->active) {
        throw std::runtime_error("Transaction already finished");
    }
    if (sqlite3_exec(impl_->db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        // Leave active = true so the Impl dtor rolls back.
        throw_sqlite_error(impl_->db, "COMMIT");
    }
    impl_->active = false;
}

//------------------------------------------------------------------------------

Transaction Database::begin() {
    if (sqlite3_exec(impl_->db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw_sqlite_error(impl_->db, "BEGIN");
    }
    std::unique_ptr<Transaction::Impl> ti(new Transaction::Impl);
    ti->db     = impl_->db;
    ti->active = true;
    return Transaction(std::move(ti));
}

}}
