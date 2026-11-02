# Changelog

What changed in each release of Tortilla. Versions follow
[semantic versioning](https://semver.org); until 1.0, a minor version may
change how things work.

## 0.1.0

The first release: a native git client for Windows and Linux that lives in
your file manager, with a full log and history client, and pull requests and
CI from GitHub, GitLab and Gitea/Forgejo on your branches.

### Native on every desktop

- One application, drawn with each platform's own widgets through
  [rapport](https://github.com/TurkeyMan/rapport): Win32 on Windows; GTK 4,
  GTK 3 or Qt 6 on Linux, picked to match the desktop when it starts
  (`--ui=` to choose, `--which-ui` to see what was detected). The desktop's
  theme, fonts and dark mode apply as they are.
- Sharp at fractional scales (GTK 3 and GTK 4 on Wayland, Qt, per-monitor DPI
  on Windows), and quick: the log opens at once and scrolls through years of
  history at the display's refresh rate; git that may be slow runs off the
  UI thread.
- A repository can wear a colour: its windows carry it in the taco shell of
  their icon, and on Windows get a taskbar button of their own.
- Translatable throughout, with a German translation.

### File manager integration

- **Windows Explorer**: TortoiseGit's context menu on files and folders, an
  icon for every entry, with each entry's placement (top level, submenu or
  hidden) set in Settings > Context Menu. Status badges on icons: Clean,
  Modified, Conflicted and Added, from a per-user status cache
  (`tortilla cache`), and a remembered summary on a repository's folder
  seen from outside.
- **Linux**: the same menu in Nautilus (GNOME Files), Nemo, Caja, Thunar and
  Dolphin through their extension interfaces, and in PCManFM-Qt and PCManFM
  as file manager actions; one package per file manager. Status badges on
  icons aren't there yet on Linux: Explorer only, for now.
- Explorer's Rename, Delete (to the Recycle Bin) and Delete (keep local), Add
  to ignore list, Create repository here, Diff with previous version, and
  Stash changes, Apply and Pop.

### Log and history

- The commit graph with branch, tag and remote labels, filters, date ranges,
  the commit's details and its changed files with TortoiseGit's context
  menus. Drag a label onto a commit to move the branch; double-click to
  switch to it.
- A Show menu for what the log walks: the current branch, all branches,
  local branches, pull requests, My branches, or a hand-picked set; Branch
  tips only, and Only what's not merged. Pinned and hidden branches per
  repository; a branch checked out in another worktree is marked and opens
  there.
- Logs of a file or folder that follow renames, with Show Whole Project;
  a ref selector; author pictures (Gravatar, GitHub's noreply addresses and
  the forges).
- Blame, with a slider through the file's history; RefLog and the stash list;
  Bisect; Worktrees; the Repository Browser; Browse References (a ref manager
  and picker); Export to a .zip or a folder; Format Patch; Edit Notes;
  comparing any two revisions, or a revision with the working tree.
- The log reloads when something else changes the repository.

### Committing and changing history

- Commit: choose the files, amend, set the author and date, Signed-off-by,
  and Restore after commit to split a change across commits.
- Check for modifications, Add, Revert, Resolve, Clean up and Remove, sharing
  one changes list.
- Interactive rebase: pick, squash, edit or skip, reorder by dragging, and add
  commits picked or dragged from a log.
- Create Branch and Tag, Switch/Checkout, Merge and Abort Merge, Reset, Stash,
  and Submodules (Add, Update, Sync), asking the right questions when the
  working tree has changes.
- Edit conflicts in a merge tool; Diff in the built-in unified diff viewer or
  your own tool (WinMerge, Beyond Compare, Meld, KDiff3 and more, detected);
  images in IfritView.

### Remotes

- The log's Fetch button syncs: every remote fetched side by side without
  stopping for a password, the log reloaded as each arrives, branches that
  aren't checked out fast-forwarded, and a hint (Fast-forward or Pull...)
  when yours is behind. Before you click, its badge says what a sync would
  bring, checked in the background every ten minutes (or from the forge's
  pull request poll); a red ! for a remote that failed. Settings > Network
  has a Sync section.
- Clone, Fetch, Pull and Push with progress; Settings > Remotes to manage
  them. Passwords, passphrases and unknown hosts are asked in Tortilla's own
  window. Git LFS works, and Tortilla says when a repository needs it but
  git-lfs isn't installed.

### Forges: GitHub, GitLab, Gitea and Forgejo

- Settings > Accounts: sign in with a token, through the browser (GitHub's
  device flow), or use the GitHub CLI's existing sign-in with nothing to set
  up. Tokens are kept in the system keychain, never in Tortilla's settings;
  each account is checked when the page opens.
- No sign-in at all for public repositories on github.com, gitlab.com and
  codeberg.org: their pull requests, checks and authors' pictures show
  anyway. Settings > Remotes says, for each remote, whether you're signed in
  there and why you might want to be.
- Pull requests on their branches' labels in the log, with a CI mark (passed,
  failed, running) and whether they can be merged; hover for what the mark
  means and each check's result.
- Create Pull Request from a branch, offering only valid targets (the
  branch's own repository and what it's a fork of); Merge and Close from the
  pull request's menu, which says why a merge isn't possible yet.

### Packaging

- Windows: an installer that registers the Explorer menu for every user and
  restarts Explorer rather than asking for a reboot, and a zip. tortilla.exe
  is self-contained: rapport and the C runtime are linked in.
- Linux: .deb (Debian, Ubuntu), .rpm (Fedora) and Arch packages for x86_64
  (and arm64 for .deb and .rpm), split by toolkit and file manager. Tortilla
  carries its own copy of rapport in `<libdir>/tortilla`, so it never
  conflicts with another application's.
