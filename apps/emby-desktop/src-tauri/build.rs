fn main() {
    tauri_build::build();

    // Compile-time build identifier.
    //
    // The controls window has had three "the caption is still there" reports in
    // a row. Before spending another round on the window style it has to be
    // possible to tell, from a screenshot alone, which binary produced it: the
    // short sha is put into the controls window title and into the diagnostics
    // log, so a report that says "still there" also says whether it is our fix
    // or a stale install. No extra dependency: `git` is already required by the
    // build, and an unavailable git (a source tarball) degrades to "unknown"
    // instead of failing the build.
    let sha = std::process::Command::new("git")
        .args(["rev-parse", "--short=7", "HEAD"])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .output()
        .ok()
        .filter(|out| out.status.success())
        .and_then(|out| String::from_utf8(out.stdout).ok())
        .map(|sha| sha.trim().to_string())
        .filter(|sha| !sha.is_empty())
        .unwrap_or_else(|| "unknown".to_string());
    println!("cargo:rustc-env=EMBY_BUILD_SHA={sha}");
    // A new commit must not reuse a cached value.
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=../../../.git/HEAD");
}
