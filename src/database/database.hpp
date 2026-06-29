#pragma once

#include "compat/cxx11.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace database {

class Database;

//------------------------------------------------------------------------------
// Backend configuration structs
//------------------------------------------------------------------------------
//
// Per-backend connection parameters. Held by the top-level `mirobody::Config`
// and populated by `load_config()`. The `open()` helpers build a backend-
// appropriate URI and hand it to the `Database` constructor: caller is
// responsible for matching the chosen struct to the backend that was actually
// linked at build time (see MIROBODY_DATABASE_BACKEND in CMakeLists.txt).

struct PostgreSQLConfig {
    std::string host;
    int         port = 5432;
    std::string user;
    std::string password;
    std::string database;
    std::string schema;
    std::string encryption_key;
    int         min_connection = 0;
    int         max_connection = 0;

    Database open() const;
};

//------------------------------------------------------------------------------

struct SQLiteConfig {
    std::string path;          // filesystem path or ":memory:"

    Database open() const;
};

//------------------------------------------------------------------------------

struct DuckDBConfig {
    std::string path;          // filesystem path or ":memory:"
};

//------------------------------------------------------------------------------

struct MySQLConfig {
    std::string host;
    int         port = 3306;
    std::string user;
    std::string password;
    std::string database;
    std::string encryption_key;
    int         min_connection = 0;
    int         max_connection = 0;

    Database open() const;
};

//------------------------------------------------------------------------------

struct ClickHouseConfig {
    std::string host;
    int         port = 9000;   // 9000 native, 9440 TLS native, 8123 HTTP
    std::string user;
    std::string password;
    std::string database;
    std::string encryption_key;
    int         min_connection = 0;
    int         max_connection = 0;
};

//------------------------------------------------------------------------------
// Value
//------------------------------------------------------------------------------

// Tagged union of the five SQL value kinds the wrapper passes between caller
// and backend. Constructed implicitly from common C++ literals so call sites
// can write
//
//     db.execute("INSERT INTO t VALUES (?, ?)", {42, "alice"});
//
// without naming Value at all.
class Value {
public:
    enum class Type { Null, Int, Double, Text, Blob };

    Value() = default;
    Value(std::nullptr_t)  {}
    Value(int v)           : type_(Type::Int),    i_(v) {}
    Value(std::int64_t v)  : type_(Type::Int),    i_(v) {}
    Value(double v)        : type_(Type::Double), d_(v) {}
    Value(const char* v)   : type_(v ? Type::Text : Type::Null), s_(v ? v : "") {}
    Value(std::string v)   : type_(Type::Text),   s_(std::move(v)) {}

    static Value blob(std::string bytes) {
        Value v;
        v.type_ = Type::Blob;
        v.s_ = std::move(bytes);
        return v;
    }

    Type type() const                  { return type_; }
    bool is_null() const               { return type_ == Type::Null; }
    std::int64_t as_int() const        { return i_; }
    double as_double() const           { return d_; }
    const std::string& as_text() const { return s_; }
    const std::string& as_blob() const { return s_; }

private:
    Type type_ = Type::Null;
    std::int64_t i_ = 0;
    double d_ = 0.0;
    std::string s_;
};

//------------------------------------------------------------------------------
// Result
//------------------------------------------------------------------------------

// Outcome of one execute() call. For SELECTs, `columns` holds the column
// names and `rows` holds one std::vector<Value> per row in declaration order.
// For DML, `rows_affected` is set and `last_insert_id` is populated when the
// backend exposes one (SQLite rowid; semantics differ on other backends).
struct Result {
    std::vector<std::string>          columns;
    std::vector<std::vector<Value>>   rows;
    std::int64_t                      rows_affected   = 0;
    std::int64_t                      last_insert_id  = 0;
};

//------------------------------------------------------------------------------
// Transaction
//------------------------------------------------------------------------------

// A multi-statement transaction pinned to a single backend connection, obtained
// from Database::begin(). All execute() calls on it run on that one connection,
// so `BEGIN` / `COMMIT` behave correctly even on pooled backends (PostgreSQL).
//
// commit() commits and ends the transaction. If a Transaction is destroyed
// without a successful commit() -- including when an exception unwinds past it
// -- it rolls back. Move-only and not thread-safe; it must not outlive the
// Database that created it.
class Transaction {
public:
    ~Transaction();
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Run one statement inside the transaction. Same contract as
    // Database::execute. Throws if the transaction has already been committed
    // or rolled back.
    Result execute(const std::string& sql,
                   const std::vector<Value>& params = {});

    // Commit and end the transaction. Throws on failure (the connection is
    // rolled back and returned to the pool). After this the Transaction is
    // finished and execute()/commit() will throw.
    void commit();

private:
    friend class Database;
    struct Impl;
    explicit Transaction(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

//------------------------------------------------------------------------------
// Database
//------------------------------------------------------------------------------

// Thin wrapper over whichever SQL backend is linked into this build. Exactly
// one backend is built per binary (see "Database backends" in the README), so
// this class is not virtual; the .cpp that gets compiled selects the backend.
//
// Not thread-safe. Use one Database per thread, or serialize access in the
// caller.
class Database {
public:
    // Open or create a database at `uri`. The URI is backend-specific:
    //   - SQLite:     filesystem path, or ":memory:" for an in-memory DB.
    //   - DuckDB:     filesystem path, or ":memory:".
    //   - PostgreSQL: libpq connection string ("host=... dbname=...").
    //   - MySQL:      "mysql://user:pass@host:port/dbname".
    //   - ClickHouse: "clickhouse://user:pass@host:port/dbname" (stub today).
    // Throws std::runtime_error on failure.
    explicit Database(const std::string& uri);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    // Run one SQL statement with positional `?` placeholders. Parameters are
    // bound in order from `params`. Returns rows + column names for SELECTs,
    // and `rows_affected` / `last_insert_id` for DML. Throws
    // std::runtime_error on prepare, bind, or execution failure.
    //
    // With pooled backends (PostgreSQL, MySQL), each call may run on a
    // different physical connection, so multi-statement transactions via
    // bare `BEGIN` / `COMMIT` across separate execute() calls will not stay
    // on the same connection. Transactions of that kind need a dedicated
    // wrapper (not implemented yet).
    Result execute(const std::string& sql,
                   const std::vector<Value>& params = {});

    // Begin a multi-statement transaction (see Transaction). On pooled backends
    // a connection is held for the transaction's lifetime. Throws on backends
    // that do not implement transactions yet.
    Transaction begin();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}}
