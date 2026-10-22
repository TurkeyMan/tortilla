# tortilla
A portable TortoiseGit inspired git client

Tortilla brings TortoiseGit's dialogs (the log, rebase, diff and their
context menus) to Windows and Linux as a fast native app. On Windows it uses
real Win32 controls; on Linux, GTK or Qt to match your desktop. It's built on
[rapport](https://github.com/TurkeyMan/rapport) and reads repositories
directly with libgit2, running `git` only for the commands that change
things.

It's early days. What works now:

- **Log**: the commit graph with branch and tag labels, filters and date
  ranges, the commit details, and the changed files with TortoiseGit's
  context menus. Drag a label onto a commit to move the branch;
  double-click a label to switch to it. Middle-button autoscroll, F5 to
  refresh, and scrolling that keeps up with a 165 Hz display.
- **Rebase**: interactive rebase with pick/squash/edit/skip and reordering.
- **Diff**: a unified diff viewer with find and save, or your own diff tool
  (WinMerge, Beyond Compare, Meld, KDiff3, ...), set in Settings.
- Branches, tags, stash, reset, merge and switching, with the right
  questions asked when the working tree has changes.

## Running

```sh
tortilla [log] [path]           # the log for the repository holding path
tortilla rebase [path] [upstream]
tortilla diff [path] [rev] [rev2]
tortilla settings [path]
```

`--all` starts the log with all branches; `--ui=gtk3`, `--ui=qt6` and so on
pick the toolkit (`--which-ui` lists what was detected).

## Building

You need CMake 3.25, a C++23 compiler, libgit2, and the development files
for GTK 3, GTK 4 and/or Qt 6 (Linux) or nothing extra (Windows).

```sh
git clone --recursive https://github.com/TurkeyMan/tortilla.git
cd tortilla
cmake -S . -B build -G Ninja
cmake --build build
build/bin/tortilla log /path/to/a/repo
```

## Licence

GPLv3: see [LICENSE](LICENSE). Tortilla follows TortoiseGit's design;
rapport, the UI library, is MIT.
