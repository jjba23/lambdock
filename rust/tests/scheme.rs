// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The Scheme suite, run by cargo.
//!
//! The Guile modules carry their own tests (`tests/scheme/lambdock_tests.scm`),
//! and they are the half of the configuration engine no Rust test can see. This
//! is all that used to be `tests/run_scheme_tests.sh`: find a Guile interpreter,
//! load the modules straight from source, and let the suite report.
//!
//! No interpreter is a skip, not a failure -- the answer the shell script gave by
//! exiting 77 -- so `cargo test` stays runnable where only the dock is built.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The tree the suite lives in: the crate's parent.
fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the crate lives inside the workspace")
        .to_path_buf()
}

/// A Guile interpreter: the one `GUILE` names, or the first on `PATH`.
fn interpreter() -> Option<PathBuf> {
    if let Some(interpreter) = std::env::var_os("GUILE").filter(|value| !value.is_empty()) {
        return Some(PathBuf::from(interpreter));
    }

    let path = std::env::var_os("PATH")?;

    std::env::split_paths(&path)
        .map(|directory| directory.join("guile"))
        .find(|candidate| candidate.is_file())
}

#[test]
fn the_scheme_modules_pass_their_own_suite() {
    let Some(interpreter) = interpreter() else {
        eprintln!("no Guile interpreter found; skipping the Scheme suite");
        return;
    };

    let root = root();
    let status = Command::new(interpreter)
        // Modules load from source: never write `.go` caches into the tree.
        .env("GUILE_AUTO_COMPILE", "0")
        .arg("-L")
        .arg(root.join("scheme"))
        .arg(root.join("tests/scheme/lambdock_tests.scm"))
        .current_dir(&root)
        .status()
        .expect("the Guile interpreter runs");

    assert!(status.success(), "the Scheme suite failed");
}
