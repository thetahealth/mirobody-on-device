// Link against libmirobody. On MSVC this resolves mirobody.lib (the DLL's import
// library) at link time; mirobody.dll is loaded at runtime. Point the search at
// build-shared/ (where build-shared.cmd writes the artifacts). Override with the
// MIROBODY_LIB_DIR environment variable.
use std::path::PathBuf;

fn main() {
    let dir = std::env::var("MIROBODY_LIB_DIR").map(PathBuf::from).unwrap_or_else(|_| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("build-shared")
    });
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=mirobody");
    println!("cargo:rerun-if-env-changed=MIROBODY_LIB_DIR");
}
