# Tortilla
A fast, native git client for Windows and Linux.

<p align="center">
  <img src="data/icons/hicolor/256x256/apps/tortilla.png" width="256" alt="Tortilla's mascot: a turtle with a taco for a shell">
</p>

Tortilla puts your repository's history, branches and changes in front of
you, and keeps up with you as you work through them. It's a real desktop
app: Win32 controls on Windows, GTK or Qt on Linux to match your desktop,
and it opens in an instant. Repositories are read directly with libgit2;
`git` itself runs only for the commands that change things.

If you've used TortoiseGit, you'll feel at home: Tortilla follows its
dialogs and menus, and now runs on Linux too. It's built on
[rapport](https://github.com/TurkeyMan/rapport).

It's early days. What works now:

- **Log**: the commit graph with branch and tag labels, filters and date
  ranges, the commit details, and the changed files with TortoiseGit's
  context menus. Drag a label onto a commit to move the branch;
  double-click a label to switch to it. Middle-button autoscroll, F5 to
  refresh, and scrolling that keeps up with a 240 Hz display.
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
