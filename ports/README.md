# vcpkg overlay ports

Ports that replace vcpkg's own for the Windows build (vcpkg-configuration.json
names this folder). Each is vcpkg's port at the baseline, plus a patch of ours.

- `libgit2`: vcpkg's 1.9.7 port, plus `strcasecmp-own.diff`. On Windows libgit2
  sorted the index with the C runtime's `_stricmp` but walked the working tree
  with its own `git__strcasecmp`; on ARM64 the two order bytes from 0x80
  differently, so with `core.ignorecase` a non-ASCII file showed as deleted and
  untracked at once. The patch makes both its own. Drop the overlay once
  libgit2 does this itself.
