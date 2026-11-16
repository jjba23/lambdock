// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! The three jobs cargo does not do: install the data files, compile the
//! translation catalogues, write the source archive.
//!
//! `cargo xtask`, from the workspace root. Every argument is used as an argument
//! -- nothing here builds a shell string -- and every step that cannot finish
//! says why and exits non-zero rather than leaving half an install behind.

use std::path::{Path, PathBuf};
use std::process::{exit, Command};

fn main() {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    let command = arguments.first().map(String::as_str).unwrap_or("");

    match command {
        "install" => install(&arguments[1..]),
        "i18n" => i18n(&arguments[1..]),
        "dist" => dist(&arguments[1..]),
        _ => {
            eprintln!(
                "usage: cargo xtask <install|i18n|dist> [--prefix DIR] [--destdir DIR]"
            );
            exit(2);
        }
    }
}

/// The tree the task runs in: the workspace root, whatever the CWD.
fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives inside the workspace")
        .to_path_buf()
}

/// `--name VALUE`, or `default` when the flag is absent.
fn flag(arguments: &[String], name: &str, default: &str) -> String {
    arguments
        .iter()
        .position(|argument| argument == name)
        .and_then(|index| arguments.get(index + 1))
        .cloned()
        .unwrap_or_else(|| default.to_owned())
}

/// Runs a program, and stops the task when it fails: a missing translator or a
/// missing interpreter is not a warning.
fn run(program: &str, arguments: &[String]) {
    let status = Command::new(program)
        .args(arguments)
        .current_dir(root())
        .status()
        .unwrap_or_else(|error| {
            eprintln!("{program} is needed: {error}");
            exit(1);
        });

    if !status.success() {
        eprintln!("{program} failed");
        exit(1);
    }
}

/// Copies one file, creating the directory it needs.
fn install_file(source: &Path, destination: &Path) {
    if let Some(parent) = destination.parent() {
        if let Err(error) = std::fs::create_dir_all(parent) {
            eprintln!("could not create {}: {error}", parent.display());
            exit(1);
        }
    }

    if let Err(error) = std::fs::copy(source, destination) {
        eprintln!(
            "could not install {} to {}: {error}",
            source.display(),
            destination.display()
        );
        exit(1);
    }
}

/// Puts the binary, the desktop entry, the metainfo, the logo, the Scheme modules
/// and the compiled catalogues under the prefix.
fn install(arguments: &[String]) {
    let prefix = PathBuf::from(flag(arguments, "--prefix", "/usr/local"));
    let destdir = PathBuf::from(flag(arguments, "--destdir", ""));
    let root = root();
    let target = destdir.join(prefix.strip_prefix("/").unwrap_or(&prefix));

    let binary = root.join("rust/target/release/lambdock");
    install_file(&binary, &target.join("bin/lambdock"));

    let data = target.join("share");
    install_file(
        &root.join("data/org.jointhefreeworld.lambdock.desktop"),
        &data.join("applications/org.jointhefreeworld.lambdock.desktop"),
    );
    install_file(
        &root.join("data/org.jointhefreeworld.lambdock.metainfo.xml"),
        &data.join("metainfo/org.jointhefreeworld.lambdock.metainfo.xml"),
    );
    install_file(
        &root.join("logo.svg"),
        &data.join("icons/hicolor/scalable/apps/lambdock.svg"),
    );

    let modules = root.join("scheme/lambdock");
    let installed = data.join("guile/site/3.0/lambdock");
    let Ok(entries) = std::fs::read_dir(&modules) else {
        eprintln!("could not read {}", modules.display());
        exit(1);
    };

    for entry in entries.filter_map(Result::ok) {
        let path = entry.path();
        if path.extension().is_some_and(|extension| extension == "scm") {
            if let Some(name) = path.file_name() {
                install_file(&path, &installed.join(name));
            }
        }
    }

    // The catalogues go with them, into the same prefix.
    compile_catalogues(&prefix, &destdir);
}

/// `cargo xtask i18n`: compile the catalogues under the prefix.
fn i18n(arguments: &[String]) {
    compile_catalogues(
        &PathBuf::from(flag(arguments, "--prefix", "/usr/local")),
        &PathBuf::from(flag(arguments, "--destdir", "")),
    );
}

/// Compiles every `po/*.po` into `<lang>/LC_MESSAGES/lambdock.mo` under the
/// prefix, which is what both `install` and `i18n` mean by translating.
fn compile_catalogues(prefix: &Path, destdir: &Path) {
    let target = destdir.join(prefix.strip_prefix("/").unwrap_or(prefix));
    let locale = target.join("share/locale");
    let po = root().join("po");

    let Ok(entries) = std::fs::read_dir(&po) else {
        eprintln!("could not read {}", po.display());
        exit(1);
    };

    for entry in entries.filter_map(Result::ok) {
        let path = entry.path();
        if path.extension().is_none_or(|extension| extension != "po") {
            continue;
        }

        let language = path
            .file_stem()
            .expect("a .po file has a stem")
            .to_string_lossy()
            .into_owned();
        let destination = locale.join(format!("{language}/LC_MESSAGES/lambdock.mo"));

        if let Some(parent) = destination.parent() {
            if let Err(error) = std::fs::create_dir_all(parent) {
                eprintln!("could not create {}: {error}", parent.display());
                exit(1);
            }
        }

        run(
            "msgfmt",
            &[
                path.display().to_string(),
                "-o".to_owned(),
                destination.display().to_string(),
            ],
        );
    }
}

/// Writes the source archive git already knows how to make.
fn dist(arguments: &[String]) {
    let version = flag(arguments, "--version", env!("CARGO_PKG_VERSION"));
    let archive = format!("lambdock-{version}.tar.gz");

    run(
        "git",
        &[
            "archive".to_owned(),
            "--format=tar.gz".to_owned(),
            format!("--prefix=lambdock-{version}/"),
            "-o".to_owned(),
            archive,
            "HEAD".to_owned(),
        ],
    );
}
