#!/bin/sh
# Configure, build with every toolkit backend and file-manager extension, and
# run the tests.
set -eu
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr | tee build-ci.log
if grep -E -- '^-- rapport: skipping' build-ci.log | grep -v win32; then
  echo "a toolkit backend or file-manager SDK is missing; install its -dev package in .github/ci/deps-*.sh" >&2
  exit 1
fi
# GTK 3 reads the true fractional scale on Wayland only with wayland-protocols.
if ! grep -q 'GTK 3 fractional scale: on' build-ci.log; then
  echo "GTK 3 fractional scale is off; install wayland-protocols in .github/ci/deps-*.sh" >&2
  exit 1
fi
cmake --build build-ci --parallel "$(nproc)"

# The tests make their own repositories, so git needs an identity.
git config --global user.name "Tortilla CI"
git config --global user.email ci@tortilla.invalid
for t in tortilla-tests tortilla-rebase-tests; do
  scratch=$(mktemp -d)
  "build-ci/bin/$t" "$scratch"
done
build-ci/bin/tortilla-shell-probe .
