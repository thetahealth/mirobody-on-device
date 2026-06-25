// Python bindings for embedding mirobody into a CPython host.
//
// This is the "host shim" for Python, sitting alongside the Android JNI bridge
// (android_jni.cpp) and the iOS bridge (ios_bridge.mm). Unlike those, which
// implement the narrow C API in mirobody.h, the Python module talks to the C++
// Server/Config classes directly so the wrapper can grow a richer surface
// without first widening the C ABI.
//
// Built only when MIROBODY_BUILD_PYTHON=ON (see CMakeLists.txt); the wheel is
// produced via scikit-build-core driving CMake — see pyproject.toml and
// python/README.md. The wheel defaults to the SQLITE database backend so it is
// self-contained, exactly like the mobile builds.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "database/schema.hpp"
#include "platform/log.hpp"
#include "server/server.hpp"

#include <libwebsockets.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace py = pybind11;

namespace {

std::once_flag g_curl_init_flag;

void ensure_curl_init() {
    std::call_once(g_curl_init_flag, [] {
        mirobody::client::HttpClient::global_init();
    });
}

void lws_log_to_platform(int /*level*/, const char* line) {
    mirobody::platform::log_info("%s", line);
}

// Apply the DDL on a throwaway connection before the server opens its serving
// pool — mirrors main.cpp. The wheel links the SQLITE backend; the other
// branches exist so the module still builds (and migrates) if someone configures
// a different backend at build time.
void apply_migrations(const mirobody::Config& cfg) {
#if defined(MIROBODY_DATABASE_SQLITE)
    mirobody::database::Database db(cfg.sqlite.open());
#elif defined(MIROBODY_DATABASE_PG) || defined(MIROBODY_DATABASE_PG_LEGACY)
    mirobody::database::Database db(cfg.postgresql().open());
#else
#  error "python_bridge: no migration connection for the selected database backend"
#endif
    mirobody::database::apply_schema(db, cfg.sql_dir + "/" MIROBODY_DATABASE_BACKEND_DIR);
}

// Thin RAII wrapper exposed to Python as mirobody._mirobody.Server. Owns the
// underlying mirobody::Server and runs it on a background thread, so the Python
// interpreter (and its event loop) stays responsive.
class PyServer {
public:
    PyServer(std::string config_path,
             std::string data_dir,
             std::string sql_dir,
             std::string listen_addr,
             std::string openai_api_key,
             std::string gemini_api_key,
             int listen_port)
        : config_path_(std::move(config_path)),
          data_dir_(std::move(data_dir)),
          sql_dir_(std::move(sql_dir)),
          listen_addr_(std::move(listen_addr)),
          openai_api_key_(std::move(openai_api_key)),
          gemini_api_key_(std::move(gemini_api_key)),
          listen_port_(listen_port) {}

    ~PyServer() { stop(); }

    PyServer(const PyServer&) = delete;
    PyServer& operator=(const PyServer&) = delete;

    void start() {
        if (server_) {
            throw std::runtime_error("server already started");
        }

        ensure_curl_init();

        int log_mask = LLL_ERR | LLL_WARN;
        lws_set_log_level(log_mask, lws_log_to_platform);

        mirobody::Config cfg;
        try {
            if (!config_path_.empty()) {
                cfg = mirobody::load_config(config_path_);
            } else {
                cfg = mirobody::load_config();
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("config error: ") + e.what());
        }

        if (!openai_api_key_.empty()) cfg.openai.api_key = openai_api_key_;
        if (!gemini_api_key_.empty()) cfg.gemini.api_key = gemini_api_key_;
        if (!listen_addr_.empty())    cfg.listen_addr = listen_addr_;
        if (!sql_dir_.empty())        cfg.sql_dir = sql_dir_;
        if (listen_port_ > 0)         cfg.listen_port = static_cast<std::uint16_t>(listen_port_);
        // data_dir is where the on-device SQLite file lives. Only override the
        // configured path when the caller actually passed one.
        if (!data_dir_.empty())       cfg.sqlite.path = data_dir_ + "/mirobody.db";

        // The C++ side blocks on DB I/O and socket setup; let other Python
        // threads run while it does.
        {
            py::gil_scoped_release release;
            try {
                apply_migrations(cfg);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("database init failed: ") + e.what());
            }

            auto server = std::unique_ptr<mirobody::Server>(new mirobody::Server(std::move(cfg)));
            if (!server->start_in_background()) {
                throw std::runtime_error("server failed to start");
            }
            server_ = std::move(server);
        }
    }

    void stop() {
        if (!server_) return;
        py::gil_scoped_release release;
        server_->stop_and_join();
        server_.reset();
    }

    bool is_running() const {
        return server_ && server_->is_running();
    }

    int listen_port() const {
        if (!server_) return -1;
        return static_cast<int>(server_->config().listen_port);
    }

private:
    std::string config_path_;
    std::string data_dir_;
    std::string sql_dir_;
    std::string listen_addr_;
    std::string openai_api_key_;
    std::string gemini_api_key_;
    int         listen_port_;

    std::unique_ptr<mirobody::Server> server_;
};

}  // namespace

//------------------------------------------------------------------------------

PYBIND11_MODULE(_mirobody, m) {
    m.doc() = "Python bindings for the mirobody on-device server.";

    py::class_<PyServer>(m, "Server")
        .def(py::init<std::string, std::string, std::string, std::string,
                      std::string, std::string, int>(),
             py::arg("config_path") = "",
             py::arg("data_dir") = "",
             py::arg("sql_dir") = "",
             py::arg("listen_addr") = "",
             py::arg("openai_api_key") = "",
             py::arg("gemini_api_key") = "",
             py::arg("listen_port") = 0,
             "Construct a server. All arguments are optional; empty/zero values "
             "fall back to the config file or compiled-in defaults.")
        .def("start", &PyServer::start,
             "Apply migrations and start the server on a background thread. "
             "Raises RuntimeError on failure.")
        .def("stop", &PyServer::stop,
             "Stop the server and join its thread. Safe to call repeatedly.")
        .def("is_running", &PyServer::is_running,
             "Return True while the server is serving.")
        .def("listen_port", &PyServer::listen_port,
             "Return the bound port, or -1 if not started.")
        // Context-manager sugar: `with mirobody.Server(...) as s: ...`
        .def("__enter__", [](PyServer& self) -> PyServer& {
            self.start();
            return self;
        }, py::return_value_policy::reference)
        .def("__exit__", [](PyServer& self, const py::object&, const py::object&,
                            const py::object&) {
            self.stop();
            return false;
        });
}
