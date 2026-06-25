#include "database/database.hpp"

#include <mysql/mysql.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>

// MySQL backend. Unlike the PostgreSQL backend (which uses libpq's binary param
// protocol), this uses the text protocol: `?` placeholders are substituted with
// escaped literals (mysql_real_escape_string) and the query is run with
// mysql_real_query / mysql_store_result. The placeholder walk mirrors pg.cpp's
// translate_placeholders so a `?` inside a string/comment is left untouched. The
// MySQL prepared-statement result API (MYSQL_BIND, per-column re-fetch for
// variable-length values) is far fiddlier to get right; the text protocol gives
// the same safety via escaping with much simpler result extraction.

namespace mirobody { namespace database {

namespace {

//------------------------------------------------------------------------------
// One-time client-library init
//------------------------------------------------------------------------------

void ensure_lib_init() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw std::runtime_error("mysql_library_init failed");
        }
    });
}

//------------------------------------------------------------------------------
// Connection string (keyword=value, built by MySQLConfig::open)
//------------------------------------------------------------------------------

struct ConnParams {
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    int         port     = 3306;
    int         pool_min = 0;
    int         pool_max = 0;
};

ConnParams parse_conn(const std::string& s) {
    ConnParams p;
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i; };

    while (true) {
        skip_ws();
        if (i >= n) break;

        const std::size_t ks = i;
        while (i < n && s[i] != '=') ++i;
        if (i >= n) break;
        const std::string key = s.substr(ks, i - ks);
        ++i;   // skip '='

        std::string val;
        if (i < n && s[i] == '\'') {
            ++i;   // opening quote
            while (i < n) {
                const char c = s[i++];
                if (c == '\\' && i < n)      val += s[i++];   // backslash escape
                else if (c == '\'')          break;           // closing quote
                else                         val += c;
            }
        } else {
            const std::size_t vs = i;
            while (i < n && s[i] != ' ' && s[i] != '\t') ++i;
            val = s.substr(vs, i - vs);
        }

        if      (key == "host")     p.host     = val;
        else if (key == "user")     p.user     = val;
        else if (key == "password") p.password = val;
        else if (key == "dbname")   p.database = val;
        else if (key == "port")     p.port     = std::atoi(val.c_str());
        else if (key == "pool_min") p.pool_min = std::atoi(val.c_str());
        else if (key == "pool_max") p.pool_max = std::atoi(val.c_str());
    }

    // Same defaulting as the PostgreSQL pool: minconn -> 1; maxconn -> 10 if
    // minconn < 5 else minconn * 2.
    if (p.pool_min <= 0) p.pool_min = 1;
    if (p.pool_max <= 0) p.pool_max = (p.pool_min < 5) ? 10 : p.pool_min * 2;
    if (p.pool_max < p.pool_min) p.pool_max = p.pool_min;
    return p;
}

//------------------------------------------------------------------------------
// Parameter rendering (text protocol)
//------------------------------------------------------------------------------

// One bound Value as a SQL literal on `conn` (its charset drives the escaping).
std::string render_value(MYSQL* conn, const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:
            return "NULL";
        case Value::Type::Int:
            return std::to_string(v.as_int());
        case Value::Type::Double: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", v.as_double());
            return std::string(buf);
        }
        case Value::Type::Text: {
            const std::string& s = v.as_text();
            std::string esc(s.size() * 2 + 1, '\0');
            const unsigned long m =
                mysql_real_escape_string(conn, &esc[0], s.data(),
                                         static_cast<unsigned long>(s.size()));
            esc.resize(m);
            return "'" + esc + "'";
        }
        case Value::Type::Blob: {
            const std::string& b = v.as_blob();
            if (b.empty()) return "''";   // X'' is not valid; empty literal is
            static const char hex[] = "0123456789abcdef";
            std::string out;
            out.reserve(b.size() * 2 + 3);
            out += "X'";
            for (std::size_t i = 0; i < b.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(b[i]);
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            }
            out += "'";
            return out;
        }
    }
    return "NULL";
}

// Substitute `?` placeholders with rendered param literals, leaving any `?`
// inside single-/double-/backtick-quoted text or -- / * comments untouched.
std::string substitute_placeholders(MYSQL* conn, const std::string& sql,
                                    const std::vector<Value>& params) {
    std::string out;
    out.reserve(sql.size() + 64);
    std::size_t pi = 0;

    enum ScanState { QS_NORMAL, QS_SQUOTE, QS_DQUOTE, QS_BTICK, QS_LINE, QS_BLOCK };
    ScanState st = QS_NORMAL;

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c  = sql[i];
        const char nx = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

        switch (st) {
            case QS_NORMAL:
                if      (c == '\'')             { out += c; st = QS_SQUOTE; }
                else if (c == '"')              { out += c; st = QS_DQUOTE; }
                else if (c == '`')              { out += c; st = QS_BTICK; }
                else if (c == '-' && nx == '-') { out += c; out += nx; ++i; st = QS_LINE; }
                else if (c == '/' && nx == '*') { out += c; out += nx; ++i; st = QS_BLOCK; }
                else if (c == '?') {
                    if (pi >= params.size()) {
                        throw std::runtime_error(
                            "MySQL: more ? placeholders than bound parameters");
                    }
                    out += render_value(conn, params[pi++]);
                } else {
                    out += c;
                }
                break;
            case QS_SQUOTE:
                out += c;
                if (c == '\\' && nx != '\0') { out += nx; ++i; }   // \' / \\ escape
                else if (c == '\'') {
                    if (nx == '\'') { out += nx; ++i; }            // '' doubling
                    else            st = QS_NORMAL;
                }
                break;
            case QS_DQUOTE:
                out += c;
                if (c == '\\' && nx != '\0') { out += nx; ++i; }
                else if (c == '"')           st = QS_NORMAL;
                break;
            case QS_BTICK:
                out += c;
                if (c == '`') st = QS_NORMAL;
                break;
            case QS_LINE:
                out += c;
                if (c == '\n') st = QS_NORMAL;
                break;
            case QS_BLOCK:
                out += c;
                if (c == '*' && nx == '/') { out += nx; ++i; st = QS_NORMAL; }
                break;
        }
    }
    return out;
}

//------------------------------------------------------------------------------
// Result-column -> Value mapping
//------------------------------------------------------------------------------

// charsetnr 63 == binary; a BLOB-family column with it is a real BLOB, otherwise
// it is TEXT (MySQL reports TEXT as a BLOB type with a non-binary charset).
const unsigned int CHARSET_BINARY = 63;

Value value_from_field(const MYSQL_FIELD& f, const char* data, unsigned long len) {
    switch (f.type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
            try {
                return Value(static_cast<std::int64_t>(std::stoll(std::string(data, len))));
            } catch (...) {
                return Value(std::string(data, len));
            }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            try {
                return Value(std::stod(std::string(data, len)));
            } catch (...) {
                return Value(std::string(data, len));
            }
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            if (f.charsetnr == CHARSET_BINARY) {
                return Value::blob(std::string(data, len));
            }
            return Value(std::string(data, len));   // TEXT
        default:
            // varchar/char, decimal, date/time, json, ... as their text form.
            return Value(std::string(data, len));
    }
}

//------------------------------------------------------------------------------
// Connection pool
//------------------------------------------------------------------------------

class ConnPool {
public:
    ConnPool(ConnParams params, int min_size, int max_size)
        : params_(std::move(params)),
          max_size_(max_size),
          total_(0) {
        for (int i = 0; i < min_size; ++i) {
            idle_.push(make_conn());
            ++total_;
        }
    }

    ~ConnPool() {
        std::lock_guard<std::mutex> lk(mu_);
        while (!idle_.empty()) {
            mysql_close(idle_.front());
            idle_.pop();
        }
    }

    MYSQL* acquire() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !idle_.empty() || total_ < max_size_; });
        if (!idle_.empty()) {
            MYSQL* c = idle_.front();
            idle_.pop();
            return c;
        }
        ++total_;
        lk.unlock();
        try {
            return make_conn();
        } catch (...) {
            lk.lock();
            --total_;
            cv_.notify_one();
            throw;
        }
    }

    void release(MYSQL* c) {
        if (!c) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (mysql_ping(c) == 0) {   // still alive (and reconnect is off in 8.0)
            idle_.push(c);
        } else {
            mysql_close(c);
            --total_;
        }
        cv_.notify_one();
    }

private:
    MYSQL* make_conn() {
        MYSQL* c = mysql_init(nullptr);
        if (!c) throw std::runtime_error("mysql_init returned null");
        mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!mysql_real_connect(c, params_.host.c_str(), params_.user.c_str(),
                                params_.password.c_str(), params_.database.c_str(),
                                static_cast<unsigned int>(params_.port),
                                nullptr, 0)) {
            std::string err = "mysql_real_connect failed: ";
            err += mysql_error(c);
            mysql_close(c);
            throw std::runtime_error(err);
        }
        return c;
    }

    ConnParams              params_;
    int                     max_size_;
    int                     total_;
    std::queue<MYSQL*>      idle_;
    std::mutex              mu_;
    std::condition_variable cv_;
};

//------------------------------------------------------------------------------

// RAII lease that returns the connection to the pool on scope exit.
struct ConnLease {
    ConnPool* pool;
    MYSQL*    conn;
    ~ConnLease() { if (conn) pool->release(conn); }
};

//------------------------------------------------------------------------------

// Run one statement on an already-held connection. Shared by Database::execute
// (which leases a pooled connection per call) and Transaction (which pins one).
Result exec_on_conn(MYSQL* conn, const std::string& sql,
                    const std::vector<Value>& params) {
    const std::string query = substitute_placeholders(conn, sql, params);

    if (mysql_real_query(conn, query.c_str(),
                         static_cast<unsigned long>(query.size())) != 0) {
        std::string err = "mysql_real_query failed: ";
        err += mysql_error(conn);
        throw std::runtime_error(err);
    }

    Result result;
    MYSQL_RES* res = mysql_store_result(conn);
    if (res) {
        struct Guard { MYSQL_RES* r; ~Guard() { mysql_free_result(r); } } guard{res};

        const unsigned int ncols  = mysql_num_fields(res);
        MYSQL_FIELD* const  fields = mysql_fetch_fields(res);
        result.columns.reserve(ncols);
        for (unsigned int c = 0; c < ncols; ++c) {
            result.columns.emplace_back(fields[c].name ? fields[c].name : "");
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            const unsigned long* lens = mysql_fetch_lengths(res);
            std::vector<Value> out_row;
            out_row.reserve(ncols);
            for (unsigned int c = 0; c < ncols; ++c) {
                if (!row[c]) { out_row.emplace_back(); continue; }   // NULL
                out_row.emplace_back(value_from_field(fields[c], row[c], lens[c]));
            }
            result.rows.emplace_back(std::move(out_row));
        }
        result.rows_affected = static_cast<std::int64_t>(mysql_num_rows(res));
        return result;
    }

    // No result set: either a DML statement, or an error.
    if (mysql_field_count(conn) != 0) {
        std::string err = "mysql_store_result failed: ";
        err += mysql_error(conn);
        throw std::runtime_error(err);
    }
    result.rows_affected  = static_cast<std::int64_t>(mysql_affected_rows(conn));
    result.last_insert_id = static_cast<std::int64_t>(mysql_insert_id(conn));
    return result;
}

}  // namespace

//------------------------------------------------------------------------------
// Database
//------------------------------------------------------------------------------

struct Database::Impl {
    std::unique_ptr<ConnPool> pool;
};

Database::Database(const std::string& uri) : impl_(new Impl) {
    ensure_lib_init();
    ConnParams p = parse_conn(uri);
    impl_->pool.reset(new ConnPool(p, p.pool_min, p.pool_max));
}

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Result Database::execute(const std::string& sql,
                         const std::vector<Value>& params) {
    ConnLease lease{impl_->pool.get(), impl_->pool->acquire()};
    return exec_on_conn(lease.conn, sql, params);
}

//------------------------------------------------------------------------------
// Transaction
//------------------------------------------------------------------------------

// Holds the pinned connection for the transaction's life. The dtor rolls back
// and returns it to the pool, so an un-committed Transaction rolls back
// automatically.
struct Transaction::Impl {
    ConnPool* pool = nullptr;
    MYSQL*    conn = nullptr;   // nulled once committed and released
    ~Impl() {
        if (conn) {
            mysql_real_query(conn, "ROLLBACK", 8);
            pool->release(conn);
        }
    }
};

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() = default;
Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&&) noexcept = default;

Result Transaction::execute(const std::string& sql,
                            const std::vector<Value>& params) {
    if (!impl_ || !impl_->conn) {
        throw std::runtime_error("Transaction already finished");
    }
    return exec_on_conn(impl_->conn, sql, params);
}

void Transaction::commit() {
    if (!impl_ || !impl_->conn) {
        throw std::runtime_error("Transaction already finished");
    }
    MYSQL* c = impl_->conn;
    const bool ok = (mysql_real_query(c, "COMMIT", 6) == 0);
    std::string err;
    if (!ok) {
        err = "COMMIT failed: ";
        err += mysql_error(c);
        mysql_real_query(c, "ROLLBACK", 8);
    }
    impl_->pool->release(c);
    impl_->conn = nullptr;
    if (!ok) throw std::runtime_error(err);
}

Transaction Database::begin() {
    MYSQL* c = impl_->pool->acquire();
    if (mysql_real_query(c, "START TRANSACTION", 17) != 0) {
        std::string err = "START TRANSACTION failed: ";
        err += mysql_error(c);
        impl_->pool->release(c);
        throw std::runtime_error(err);
    }
    std::unique_ptr<Transaction::Impl> ti(new Transaction::Impl);
    ti->pool = impl_->pool.get();
    ti->conn = c;
    return Transaction(std::move(ti));
}

}}
