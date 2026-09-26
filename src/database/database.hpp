#pragma once


#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "blob.hpp"

namespace mirobody { namespace database {

class Database;

//------------------------------------------------------------------------------
// Backend configuration structs
//------------------------------------------------------------------------------
//
// Connection parameters for the SQLite backend. Held by the top-level
// `mirobody::Config` and populated by `load_config()`.

struct SQLiteConfig {
    std::string path;          // filesystem path or ":memory:"

    Database open() const;
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
// so `BEGIN` / `COMMIT` behave correctly.
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

// Thin wrapper over SQLite (src/database/sqlite.cpp), the one backend this repo
// links. Kept as a class rather than raw sqlite3 calls so the rest of the core
// sees rows, columns and typed values, not a C API.
//
// Not thread-safe. Use one Database per thread, or serialize access in the
// caller.
class Database {
public:
    // Open or create the SQLite database at `uri`: a filesystem path, or
    // ":memory:" for an in-memory DB.
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
    Result execute(const std::string& sql,
                   const std::vector<Value>& params = {});

    // Begin a multi-statement transaction (see Transaction).
    Transaction begin();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}}
