#!/bin/sh
# Installs inkharness: builds it with cargo and puts it in ~/.cargo/bin. Arguments go to
# `cargo install` (--features local for the in-process speech models).
#
#   inkharness/install.sh          from a checkout: the files you have
#   sh install.sh                  anywhere else: the repository's default branch
set -eu

if ! command -v cargo >/dev/null 2>&1; then
  echo "inkharness is built with Rust's cargo: install it from https://rustup.rs, then run this again." >&2
  exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
if [ -f "$here/inkharness/Cargo.toml" ] && [ -f "$here/Cargo.lock" ]; then
  cargo install --locked --path "$here/inkharness" "$@"
else
  # Through git, which has your GitHub credentials (the repository is private).
  CARGO_NET_GIT_FETCH_WITH_CLI=true cargo install --locked \
    --git https://github.com/onionhammer/INKDECK inkharness "$@"
fi

echo
inkharness --version
config="${INKHARNESS_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}/inkharness}/inkharness.toml"
if [ ! -f "$config" ]; then
  echo "Next: inkharness init (writes $config), then run inkharness in any folder."
fi
