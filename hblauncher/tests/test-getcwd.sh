#!/bin/sh
# Compile the actual SDK VFS with host-only stubs; never connects to a console.
# Usage: sh hblauncher/tests/test-getcwd.sh /path/to/patched/hblauncher-sdk
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK=${1:-"$HERE/../deps/hblauncher"}
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
  -I"$SDK/sdk/linux" "$SDK/sdk/linux/vfs.c" "$HERE/getcwd.c" -o "$TEST_DIR/getcwd-test"
"$TEST_DIR/getcwd-test"
