// Runnable Go example for libmirobody.
//
// On Windows this binds with zero cgo: syscall.NewLazyDLL loads mirobody.dll
// through the OS loader and calls the exported C functions directly. POSIX
// (Linux/macOS) uses cgo or dlopen instead — see bindings/README.md.
//
// Usage: go run . <path-to-mirobody.dll> [dataDir]
//
// The listen port comes from config (HTTP_PORT env / config.yml, default 8080);
// the demo reads the bound port back via mirobody_listen_port. Run from the repo
// root so the default config's sql_dir ("res/sql") and config.yml resolve, with
// the native dependency dir on PATH so the DLL's transitive deps (libcurl,
// openssl, …) load. See bindings/README.md.
package main

import (
	"fmt"
	"net/http"
	"os"
	"syscall"
	"time"
	"unsafe"
)

// cstr returns a pointer to a NUL-terminated copy of s, or 0 for the empty
// string (which the C API treats as "use the configured default").
func cstr(s string) uintptr {
	if s == "" {
		return 0
	}
	b, err := syscall.BytePtrFromString(s)
	if err != nil {
		panic(err)
	}
	return uintptr(unsafe.Pointer(b))
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: demo <library-path> [dataDir]")
		os.Exit(2)
	}
	libPath := os.Args[1]
	dataDir := ""
	if len(os.Args) > 2 {
		dataDir = os.Args[2]
	}

	dll := syscall.NewLazyDLL(libPath)
	start := dll.NewProc("mirobody_start")
	stop := dll.NewProc("mirobody_stop")
	isRunning := dll.NewProc("mirobody_is_running")
	listenPort := dll.NewProc("mirobody_listen_port")

	// mirobody_start(config_path, data_dir) — keys and port come from config.
	h, _, _ := start.Call(cstr(""), cstr(dataDir))
	if h == 0 {
		fmt.Println("mirobody_start returned NULL (see native log)")
		os.Exit(1)
	}

	r, _, _ := isRunning.Call(h)
	p, _, _ := listenPort.Call(h)
	fmt.Printf("started: running=%d port=%d\n", r, int32(p))

	time.Sleep(400 * time.Millisecond)
	if resp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/", int32(p))); err != nil {
		fmt.Println("GET / error:", err)
	} else {
		fmt.Println("GET / -> HTTP", resp.StatusCode)
		resp.Body.Close()
	}

	stop.Call(h)
	r, _, _ = isRunning.Call(h)
	fmt.Printf("stopped: running=%d\n", r)
}
