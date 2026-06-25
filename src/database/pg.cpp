#include "database/database.hpp"

#include <libpq-fe.h>

#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>

namespace mirobody { namespace database {

namespace {

//------------------------------------------------------------------------------
// Type mapping
//------------------------------------------------------------------------------

// PG OIDs we map to Value types. Anything else comes back as text.
const Oid OID_BOOL   = 16;
const Oid OID_BYTEA  = 17;
const Oid OID_INT8   = 20;
const Oid OID_INT2   = 21;
const Oid OID_INT4   = 23;
const Oid OID_FLOAT4 = 700;
const Oid OID_FLOAT8 = 701;

//------------------------------------------------------------------------------

// Translate `?` positional placeholders into libpq `$1`, `$2`, ... while
// respecting single-quoted strings, double-quoted identifiers, line comments
// (`--`) and block comments (`/* ... */`). A `?` inside any of those is
// preserved.
std::string translate_placeholders(const std::string& sql) {
    std::string out;
    out.reserve(sql.size() + 16);
    int n = 0;

    enum State { S_NORMAL, S_SQUOTE, S_DQUOTE, S_LINE, S_BLOCK };
    State st = S_NORMAL;

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c  = sql[i];
        const char nx = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

        switch (st) {
            case S_NORMAL:
                if      (c == '\'')                { out += c; st = S_SQUOTE; }
                else if (c == '"')                 { out += c; st = S_DQUOTE; }
                else if (c == '-' && nx == '-')    { out += c; out += nx; ++i; st = S_LINE; }
                else if (c == '/' && nx == '*')    { out += c; out += nx; ++i; st = S_BLOCK; }
                else if (c == '?')                 { out += '$'; out += std::to_string(++n); }
                else                                 out += c;
                break;
            case S_SQUOTE:
                out += c;
                if (c == '\'') {
                    if (nx == '\'') { out += nx; ++i; }  // escaped ''
                    else            st = S_NORMAL;
                }
                break;
            case S_DQUOTE:
                out += c;
                if (c == '"') st = S_NORMAL;
                break;
            case S_LINE:
                out += c;
                if (c == '\n') st = S_NORMAL;
                break;
            case S_BLOCK:
                out += c;
                if (c == '*' && nx == '/') { out += nx; ++i; st = S_NORMAL; }
                break;
        }
    }
    return out;
}

//------------------------------------------------------------------------------

// Encode raw bytes as PG bytea text literal: `\x<hex>`.
std::string bytea_encode(const std::string& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 + bytes.size() * 2);
    out += "\\x";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(bytes[i]);
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

// Decode bytea text literal back to raw bytes. Only handles the modern
// `\x<hex>` form (PG default since 9.0); legacy `escape` output is ignored.
std::string bytea_decode(const char* text, std::size_t n) {
    std::string out;
    if (!text || n < 2 || text[0] != '\\' || text[1] != 'x') return out;
    out.reserve((n - 2) / 2);
    auto hex = [](unsigned char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
        return 0;
    };
    for (std::size_t i = 2; i + 1 < n; i += 2) {
        const unsigned char hi = hex(static_cast<unsigned char>(text[i]));
        const unsigned char lo = hex(static_cast<unsigned char>(text[i + 1]));
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

//------------------------------------------------------------------------------

// libpq rejects unknown keywords, so the pool_min / pool_max keywords that
// PostgreSQLConfig::open() appends have to be parsed off here before the
// conninfo is handed to PQconnectdb.
struct ParsedURI {
    std::string conninfo;
    int pool_min = 0;
    int pool_max = 0;
};

ParsedURI parse_uri(const std::string& uri) {
    ParsedURI r;
    r.conninfo = uri;

    auto extract = [&](const std::string& key) -> int {
        std::size_t pos;
        std::size_t hdr;
        const std::string with_sp = " " + key + "=";
        if ((pos = r.conninfo.find(with_sp)) != std::string::npos) {
            hdr = with_sp.size();
        } else {
            const std::string at_start = key + "=";
            if (r.conninfo.compare(0, at_start.size(), at_start) == 0) {
                pos = 0;
                hdr = at_start.size();
            } else {
                return 0;
            }
        }
        const std::size_t v_start = pos + hdr;
        std::size_t v_end = v_start;
        while (v_end < r.conninfo.size() &&
               std::isdigit(static_cast<unsigned char>(r.conninfo[v_end]))) {
            ++v_end;
        }
        const int value = std::atoi(
            r.conninfo.substr(v_start, v_end - v_start).c_str());
        r.conninfo.erase(pos, v_end - pos);
        return value;
    };

    r.pool_min = extract("pool_min");
    r.pool_max = extract("pool_max");

    // Mirror Python's defaulting in PostgreSQLConfig: minconn -> 1; maxconn
    // -> 10 if minconn < 5 else minconn * 2.
    if (r.pool_min <= 0) r.pool_min = 1;
    if (r.pool_max <= 0) r.pool_max = (r.pool_min < 5) ? 10 : r.pool_min * 2;
    if (r.pool_max < r.pool_min) r.pool_max = r.pool_min;

    // Trim leading whitespace that may have been left by extraction.
    const std::size_t first = r.conninfo.find_first_not_of(' ');
    if (first == std::string::npos) r.conninfo.clear();
    else if (first > 0)             r.conninfo.erase(0, first);

    return r;
}

//------------------------------------------------------------------------------
// Connection pool
//------------------------------------------------------------------------------

// Thread-safe pool of PGconn handles. acquire() blocks until a connection is
// idle or the pool can grow; release() returns one to the idle queue, or
// closes it if the connection died or is mid-transaction.
class ConnPool {
public:
    ConnPool(std::string conninfo, int min_size, int max_size)
        : conninfo_(std::move(conninfo)),
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
            PQfinish(idle_.front());
            idle_.pop();
        }
    }

    PGconn* acquire() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !idle_.empty() || total_ < max_size_; });
        if (!idle_.empty()) {
            PGconn* c = idle_.front();
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

    void release(PGconn* c) {
        if (!c) return;
        std::lock_guard<std::mutex> lk(mu_);
        bool keep = (PQstatus(c) == CONNECTION_OK);
        if (keep && PQtransactionStatus(c) != PQTRANS_IDLE) {
            // Returned mid-transaction or in error state. Try ROLLBACK so
            // the next borrower gets a clean slate; discard the connection
            // if the rollback itself fails.
            PGresult* r = PQexec(c, "ROLLBACK");
            if (r) PQclear(r);
            keep = (PQstatus(c) == CONNECTION_OK &&
                    PQtransactionStatus(c) == PQTRANS_IDLE);
        }
        if (keep) {
            idle_.push(c);
        } else {
            PQfinish(c);
            --total_;
        }
        cv_.notify_one();
    }

private:
    PGconn* make_conn() {
        PGconn* c = PQconnectdb(conninfo_.c_str());
        if (!c) throw std::runtime_error("PQconnectdb returned null");
        if (PQstatus(c) != CONNECTION_OK) {
            std::string err = "PQconnectdb failed: ";
            err += PQerrorMessage(c);
            PQfinish(c);
            throw std::runtime_error(err);
        }
        return c;
    }

    std::string             conninfo_;
    int                     max_size_;
    int                     total_;
    std::queue<PGconn*>     idle_;
    std::mutex              mu_;
    std::condition_variable cv_;
};

//------------------------------------------------------------------------------

// RAII lease that returns the connection to the pool on scope exit, whether
// execute() returns normally or by exception.
struct ConnLease {
    ConnPool* pool;
    PGconn*   conn;
    ~ConnLease() { if (conn) pool->release(conn); }
};

}  // namespace

//------------------------------------------------------------------------------
// Database
//------------------------------------------------------------------------------

struct Database::Impl {
    std::unique_ptr<ConnPool> pool;
};

//------------------------------------------------------------------------------

Database::Database(const std::string& uri) : impl_(new Impl) {
    ParsedURI p = parse_uri(uri);
    impl_->pool.reset(new ConnPool(std::move(p.conninfo),
                                   p.pool_min, p.pool_max));
}

//------------------------------------------------------------------------------

Database::~Database() = default;

//------------------------------------------------------------------------------

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

//------------------------------------------------------------------------------

namespace {

// Run one statement on an already-held connection. Shared by Database::execute
// (which leases a pooled connection per call) and Transaction (which pins one
// connection for the whole transaction).
Result exec_on_conn(PGconn* conn,
                    const std::string& sql,
                    const std::vector<Value>& params) {
    const std::string translated = translate_placeholders(sql);

    std::vector<std::string> param_storage(params.size());
    std::vector<const char*> param_ptrs(params.size(), nullptr);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const Value& v = params[i];
        if (v.is_null()) {
            param_ptrs[i] = nullptr;
            continue;
        }
        switch (v.type()) {
            case Value::Type::Int:
                param_storage[i] = std::to_string(v.as_int());
                break;
            case Value::Type::Double: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", v.as_double());
                param_storage[i] = buf;
                break;
            }
            case Value::Type::Text:
                param_storage[i] = v.as_text();
                break;
            case Value::Type::Blob:
                param_storage[i] = bytea_encode(v.as_blob());
                break;
            case Value::Type::Null:
                break;
        }
        param_ptrs[i] = param_storage[i].c_str();
    }

    PGresult* res = PQexecParams(
        conn,
        translated.c_str(),
        static_cast<int>(params.size()),
        nullptr,                                                    // paramTypes (inferred)
        params.empty() ? nullptr : param_ptrs.data(),
        nullptr,                                                    // paramLengths (text)
        nullptr,                                                    // paramFormats (text)
        0                                                           // resultFormat: text
    );
    if (!res) {
        std::string err = "PQexecParams returned null: ";
        err += PQerrorMessage(conn);
        throw std::runtime_error(err);
    }

    struct ResultGuard {
        PGresult* r;
        ~ResultGuard() { PQclear(r); }
    } guard{res};

    const ExecStatusType status = PQresultStatus(res);

    if (status == PGRES_TUPLES_OK) {
        Result result;
        const int ncols = PQnfields(res);
        const int nrows = PQntuples(res);
        result.columns.reserve(static_cast<std::size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            const char* name = PQfname(res, c);
            result.columns.emplace_back(name ? name : "");
        }
        result.rows.reserve(static_cast<std::size_t>(nrows));
        for (int r = 0; r < nrows; ++r) {
            std::vector<Value> row;
            row.reserve(static_cast<std::size_t>(ncols));
            for (int c = 0; c < ncols; ++c) {
                if (PQgetisnull(res, r, c)) {
                    row.emplace_back();
                    continue;
                }
                const Oid         t    = PQftype(res, c);
                const char*       text = PQgetvalue(res, r, c);
                const std::size_t len  = static_cast<std::size_t>(PQgetlength(res, r, c));

                switch (t) {
                    case OID_BOOL:
                        row.emplace_back(
                            static_cast<std::int64_t>(text[0] == 't' ? 1 : 0));
                        break;
                    case OID_INT2:
                    case OID_INT4:
                    case OID_INT8:
                        try {
                            row.emplace_back(
                                static_cast<std::int64_t>(std::stoll(text)));
                        } catch (...) {
                            row.emplace_back(std::string(text, len));
                        }
                        break;
                    case OID_FLOAT4:
                    case OID_FLOAT8:
                        try {
                            row.emplace_back(std::stod(text));
                        } catch (...) {
                            row.emplace_back(std::string(text, len));
                        }
                        break;
                    case OID_BYTEA:
                        row.emplace_back(Value::blob(bytea_decode(text, len)));
                        break;
                    default:
                        // numeric, text, varchar, json, timestamp, ...
                        // come back as their server-rendered text form.
                        row.emplace_back(std::string(text, len));
                        break;
                }
            }
            result.rows.emplace_back(std::move(row));
        }
        result.rows_affected = nrows;
        return result;
    }

    if (status == PGRES_COMMAND_OK) {
        Result result;
        const char* tup = PQcmdTuples(res);
        if (tup && *tup) {
            try {
                result.rows_affected = std::stoll(tup);
            } catch (...) {
                // fall back to 0
            }
        }
        // PG has no portable last_insert_id; callers should use RETURNING.
        return result;
    }

    std::string err = "PQexecParams failed: ";
    err += PQresultErrorMessage(res);
    throw std::runtime_error(err);
}

}  // namespace

//------------------------------------------------------------------------------

Result Database::execute(const std::string& sql,
                         const std::vector<Value>& params) {
    ConnLease lease{impl_->pool.get(), impl_->pool->acquire()};
    return exec_on_conn(lease.conn, sql, params);
}

//------------------------------------------------------------------------------
// Transaction
//------------------------------------------------------------------------------

// Holds the pinned connection for the transaction's life. The dtor returns it
// to the pool; ConnPool::release() issues a ROLLBACK if the connection is still
// mid-transaction, so an un-committed Transaction rolls back automatically.
struct Transaction::Impl {
    ConnPool* pool = nullptr;
    PGconn*   conn = nullptr;   // nulled once committed and released
    ~Impl() { if (conn) pool->release(conn); }
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
    PGresult* r = PQexec(impl_->conn, "COMMIT");
    const bool ok = (r && PQresultStatus(r) == PGRES_COMMAND_OK);
    std::string err;
    if (!ok) {
        err = "COMMIT failed: ";
        err += r ? PQresultErrorMessage(r) : PQerrorMessage(impl_->conn);
    }
    if (r) PQclear(r);
    // Hand the connection back regardless; on a failed COMMIT release() rolls
    // it back. Null it so the Impl dtor does not release a second time.
    impl_->pool->release(impl_->conn);
    impl_->conn = nullptr;
    if (!ok) throw std::runtime_error(err);
}

//------------------------------------------------------------------------------

Transaction Database::begin() {
    PGconn* conn = impl_->pool->acquire();
    PGresult* r = PQexec(conn, "BEGIN");
    const bool ok = (r && PQresultStatus(r) == PGRES_COMMAND_OK);
    if (r) PQclear(r);
    if (!ok) {
        std::string err = "BEGIN failed: ";
        err += PQerrorMessage(conn);
        impl_->pool->release(conn);
        throw std::runtime_error(err);
    }
    std::unique_ptr<Transaction::Impl> ti(new Transaction::Impl);
    ti->pool = impl_->pool.get();
    ti->conn = conn;
    return Transaction(std::move(ti));
}

}}
