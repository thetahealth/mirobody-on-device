//! Rust binding + runnable example for libmirobody.
//!
//! The binding is a plain `extern "C"` block over the functions in mirobody.h;
//! linking is handled by build.rs. The same shared library every other FFI host
//! loads.
//!
//! Usage: mirobody-demo [dataDir]
//!
//! The listen port comes from config (HTTP_PORT env / config.yml, default 8080);
//! the demo reads the bound port back via mirobody_listen_port. Run from the repo
//! root so the default config's sql_dir ("res/sql") resolves, with mirobody.dll
//! on PATH (a fully-static build needs nothing else). See bindings/README.md.

use std::ffi::{c_char, c_int, c_void, CString};
use std::io::{Read, Write};
use std::ptr;

// Opaque server handle; only passed back to the other calls.
extern "C" {
    fn mirobody_start(config_path: *const c_char, data_dir: *const c_char) -> *mut c_void;
    fn mirobody_stop(handle: *mut c_void);
    fn mirobody_is_running(handle: *mut c_void) -> c_int;
    fn mirobody_listen_port(handle: *mut c_void) -> c_int;
}

/// Minimal HTTP/1.1 GET over a raw socket so the demo needs no HTTP crate.
/// Returns the status code from the response line.
fn http_get(port: i32) -> std::io::Result<u16> {
    let mut stream = std::net::TcpStream::connect(("127.0.0.1", port as u16))?;
    stream.write_all(b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")?;
    let mut buf = Vec::new();
    stream.read_to_end(&mut buf)?;
    let text = String::from_utf8_lossy(&buf);
    let code = text
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|c| c.parse().ok())
        .unwrap_or(0);
    Ok(code)
}

fn main() {
    let data_dir = std::env::args()
        .nth(1)
        .unwrap_or_else(|| std::env::temp_dir().to_string_lossy().into_owned());
    let data_c = CString::new(data_dir).expect("data dir has no interior NUL");

    unsafe {
        // null pointers fall back to the config/defaults; the listen port and LLM
        // keys come from config (HTTP_PORT / OPENAI_API_KEY / GOOGLE_API_KEY).
        let handle = mirobody_start(ptr::null(), data_c.as_ptr());
        if handle.is_null() {
            eprintln!("mirobody_start returned NULL (see native log)");
            std::process::exit(1);
        }

        println!(
            "started: running={} port={}",
            mirobody_is_running(handle),
            mirobody_listen_port(handle)
        );

        std::thread::sleep(std::time::Duration::from_millis(400));
        match http_get(mirobody_listen_port(handle)) {
            Ok(code) => println!("GET / -> HTTP {}", code),
            Err(e) => println!("GET / error: {}", e),
        }

        mirobody_stop(handle);
        println!("stopped: running={}", mirobody_is_running(handle));
    }
}
