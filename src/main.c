// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

/* The only C left in lambdock.
 *
 * The executable still needs a `main` the C runtime can enter, and the Rust
 * standard library is started by one: a `#[no_mangle] main` in a static library
 * would run before std is initialized, which is the sort of thing that works
 * until the first thread or the first `env` access. So this file is the entry
 * point and nothing else -- every line of behaviour is `lambdock_main` in
 * rust/src/main_app.rs. */

int lambdock_main(int argc, char **argv);

int main(int argc, char **argv) {
  return lambdock_main(argc, argv);
}
