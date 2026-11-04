# Where Tortilla runs git

Every place src/ starts a git process (`run_git`, `git_process::start`,
`revision_file`), what it is for, and whether it moves to libgit2. Starting
git costs tens of milliseconds on Windows (more over `\\wsl.localhost`, and
more again when git runs a script), so reads on click paths run in process.

Line numbers are as of the inventory commit (master 8b13a6c); the decision
column says what became of each call.

Classes:

- **click**: a read the UI thread, or a click, waits for (a menu or dialog
  opening, a check before an action, a list refreshing).
- **worker**: a read on a worker thread, nothing waits on it directly.
- **status**: working-tree status and the untracked/ignored lists. They stay
  on git while git is installed: git has fsmonitor, the untracked cache and
  split index, libgit2 none of them.
- **walk**: big history walks, where git's commit-graph matters.
- **write**: writes refs, the index, the working tree, config or objects.
- **net/hooks/filters**: network, hooks, filter drivers (LFS), signing,
  git svn, or state the user may continue on the command line (merge,
  rebase, cherry-pick, am, bisect).

Decisions:

- **libgit2**: in process, through a helper in src/git, with git as the
  fallback when libgit2 can't open the repository or doesn't know the
  syntax (and parity tests in tests/log_model_test.cpp).
- **git**: stays on git; the reason is given.
- **other agent**: belongs to the in-process diff tool work (branch
  `fast-difftool`): src/ui/compare.cpp, the diff and merge tool launch
  paths, `revision_file`.

The **no git** column says what the feature does when git isn't installed:
**works** (libgit2 alone), **greyed** (the action is greyed with "git isn't
installed"), **fallback** (libgit2's answer, git's when it's there), or
**off** (silently skipped).

## Summary

Of the rows below (a row may stand for a few call sites of one command):
56 reads moved to libgit2 (37 that a click waits for, 18 on workers, and
LFS detection); 5 more were libgit2 already (git/remotes.cpp); 10 status
reads, 2 big walks, 7 worker reads and 2 click reads stay on git (each with
its reason); 20 writes and 5 network calls stay on git; 5 belong to the
in-process diff tool work. git's view of a folder's owner (safe.directory)
and the committer identity check before a rebase stay git's own.

Every in-process read falls back to git when libgit2 can't open the
repository (an extension it refuses: reftable, sha256) or doesn't know the
syntax; config reads fall back when a config uses includeIf hasconfig:,
which libgit2 skips. Opened through `git_repository_open_ext`, as the log
always has been: GIT_DIR and GIT_WORK_TREE in the environment are not
followed (git's are, where git still runs).

Counting: `TORTILLA_GIT_TRACE=<file>` writes a line for each git started.

Processes started per click path, before and after (strace -f -e execve,
offscreen GTK 4 on Linux, a clone of this repository: 341 commits, 64 refs):

| Path | Before | After |
|---|---|---|
| Opening the log, to the Working tree row | 13: rev-parse, for-each-ref, git lfs version (and the 4 it starts), status, diff --numstat, for-each-ref --contains, symbolic-ref, rev-parse @{u}, rev-list --count | 2: status, diff --numstat |
| The log's sync check (no network part) | 4: remote, config, for-each-ref x2 | 0 |
| The log reloading after a fetch | 7 | 2: status, diff --numstat |
| Switch/Checkout dialog | 3 for-each-ref | 0 |
| Browse References | 2: for-each-ref, config | 0 |
| Commit dialog | 2: status, diff --numstat | 2 (kept); ticking Amend 2 -> 0 |
| Push, Settings | 0 | 0 |
| The log's context menu | 0 to open; Branches containing 1, dragging a label 2, moving an annotated tag 1, the stash offer after a switch 1 | 0 to open; 0 each (the stash offer keeps its status) |
| Explorer's context menu | 0 (badges come from the cache's status, kept on git) | 0 |

The log's status started 2.18 s after launch before and 1.81 s after, on a
loaded Linux machine; on Windows each process saved is tens of
milliseconds more. Without git on PATH the log opens with no process
started at all.

## Log window (opening, refreshing, the sync check, forge data)

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `rev-parse --git-common-dir` | log/log_window.cpp:742 | click (log opens) | libgit2: `repo::common_dir()` | works |
| `rev-parse --verify -q <rev>^{commit}` / `rev-parse -q a..b` | log/log_window.cpp:791 | click (every reload) | libgit2: revparse, git fallback for syntax libgit2 lacks | works |
| `show --format=fuller` / `diff HEAD` (View Patch) | log/log_window.cpp:1805 | worker | git: the text is git's patch format; libgit2 patch when git is missing | fallback |
| `for-each-ref` (labels, upstreams, worktrees, symrefs) | git/repo.cpp:234 (`repo::refs`, twice per load) | worker (log load, refresh_refs) | libgit2: one ref iteration and one config snapshot | works |
| `rev-list --parents [--full-history] -- <paths>` / `log --follow` | git/repo.cpp:570 | walk (log limited to paths) | git: history simplification and --follow are git's; libgit2 has neither | off (path-limited log needs git) |
| `status --porcelain -z`, `diff HEAD --numstat -z` | log/log_model.cpp:137, 140 | status (Working tree row) | git while installed; `git_status_list` without it | fallback |
| `config --get-all user.email` | log/branch_filter.cpp:138 | worker (My branches) | libgit2: config multivar | works |
| `branch -a --contains <id>` | log/log_actions.cpp:900 | click (Branches containing) | libgit2: refs + descendant_of | works |
| `rev-list --count to..from` | log/log_actions.cpp:997 | click (dragging a label back) | libgit2: revwalk count | works |
| `log --format=%h %s --max-count=6 to..from` | log/log_actions.cpp:999 | click (same) | libgit2: revwalk + summaries | works |
| `tag -l --format=%(contents) <tag>` | log/log_actions.cpp:1015 | click (moving an annotated tag) | libgit2: tag message | greyed (the move writes) |
| `status --porcelain --untracked-files=no` | log/log_actions.cpp:1145, 1230 | status (dirty check before switch / stash apply) | git: status | greyed (switch writes) |
| `stash list --format=%gd%x09%P%x09%gs` | log/log_actions.cpp:1211 | click (stash apply from the log) | libgit2: stash foreach | greyed (apply writes) |
| `format-patch --stdout -1` | log/log_actions.cpp:1353 | worker | git: format-patch's output is git's | greyed |
| ref commands (branch, tag, update-ref ...) | log/log_actions.cpp:166 | write | git | greyed |
| `log -1 --format=%s <id>` | log/reflog_window.cpp:375 | click (reflog action) | libgit2: commit summary | works |
| `stash drop` / `reflog delete` | log/reflog_window.cpp:421 | write | git | greyed |
| `stash list --format=%gd  %s` | log/reflog_window.cpp:438 | click (Clear stash confirm) | libgit2: stash foreach | greyed (clears) |
| `stash clear` | log/reflog_window.cpp:468 | write | git | greyed |
| `worktree lock/unlock` | log/worktree_window.cpp:449 | write | git | greyed |
| `update-ref` (fast-forward) | log/log_sync.cpp:216 | write | git | off |
| `ls-remote --heads` | log/log_sync.cpp:334 | net | git | off |
| `fetch --prefetch`-style | log/log_sync.cpp:419 | net | git | off |
| `for-each-ref <prefix><remote>/` (`local_tips`, x2) | git/sync.cpp:165 | worker (sync check) | libgit2: ref iteration | works |
| `worktree list --porcelain` | git/sync.cpp:251 | worker (fast-forward targets) | libgit2: worktree HEADs | works |
| `for-each-ref refs/heads refs/remotes` (tips, symrefs) | git/sync.cpp:259 | worker | libgit2: ref iteration | works |
| `for-each-ref ... %(upstream:track) %(HEAD)` | git/sync.cpp:275 | worker | libgit2: upstream + ahead/behind | works |
| `symbolic-ref -q --short HEAD` | git/sync.cpp:330 | worker (branch hint) | libgit2 | works |
| `rev-parse --abbrev-ref --symbolic-full-name @{u}` | git/sync.cpp:334 | worker (branch hint) | libgit2: branch upstream | works |
| `rev-list --left-right --count HEAD...@{u}` | git/sync.cpp:338 | worker (branch hint) | libgit2: `git_graph_ahead_behind` | works |
| `status --porcelain --untracked-files=no` | git/sync.cpp:346 | status | git (only when behind) | fallback |
| `remote` | git/sync.cpp:398 | worker (sync check) | libgit2: remote list | works |
| `config --type=bool --get-regexp skipfetchall` | git/sync.cpp:400 | worker | libgit2: config bool | works |
| `version` | git/sync.cpp:426 | worker | git: asks git itself (once) | off |
| `fetch` | git/sync.cpp:447 | net | git | greyed |
| `for-each-ref ... %(committerdate:unix)` (`leading_remotes`) | forge/pr_watch.cpp:81 | worker (log load, avatars, sync) | libgit2: refs + commit times | works |
| `rev-list --max-count=n head ^base` | forge/pr_details.cpp:200 | worker | libgit2: revwalk | works |
| `for-each-ref --contains <sha> refs/remotes/<r>/` | forge/pr_details.cpp:223 | worker (forge panel, log load) | libgit2: refs + descendant_of | works |
| `ls-files -- :(attr:filter=lfs)` (`uses_lfs`) | git/lfs.cpp:19 | click (log opens) | libgit2: .gitattributes in the index and info/attributes, cached per repository | works |
| `lfs version` (`lfs_installed`, 5 processes) | git/lfs.cpp:25 | click (log opens) | in process: git-lfs looked for on PATH and in git's exec path, asked only when the repository uses LFS | works |

## Commit, changes, file actions

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `log -1 --format=%B`, `log -1 --format=%an <%ae>` | changes/commit_window.cpp:375, 378 | click (Amend ticked) | libgit2: HEAD's message and author | greyed (Commit) |
| `check-ref-format --branch` | changes/commit_window.cpp:423 | click (Commit to new branch) | libgit2: `git_branch_name_is_valid` | greyed |
| `hash-object -w --no-filters` | changes/commit_window.cpp:586 | write | git | greyed |
| `cat-file blob` | changes/commit_window.cpp:609 | click (restore after commit) | libgit2: blob read | greyed |
| `rev-parse --verify --quiet refs/stash` | changes/changes_window.cpp:350 | click (window opens) | libgit2: ref lookup | works |
| status/diff/ls-files (`working_tree`) | changes/working_tree.cpp:28 | status | git while installed; `git_status_list` without it | fallback |
| `ls-files -u -z` (conflict stages) | changes/changes_view.cpp:329 | worker | git: index with split-index/untracked cache; same pass as status | fallback |
| `cat-file --filters` (`revision_file`) | changes/changes_view.cpp:672, 715, 891, 923 | net/hooks/filters (smudge) | other agent | greyed |
| `diff --name-only`, `ls-files --others` | changes/revert.cpp:95, 98 | status | git | greyed (Revert writes) |
| `clean -n` listing | changes/cleanup_dialog.cpp:137 | status | git | greyed |
| `rev-parse --git-path info/exclude` | changes/ignore_dialog.cpp:115 | click | libgit2: common dir + info/exclude | works (writes a file) |
| `ls-files -- <paths>` (any tracked?) | changes/ignore_dialog.cpp:138 | click | libgit2: index lookup by path and prefix | works |
| `rm --cached` | changes/ignore_dialog.cpp:211 | write | git | greyed |
| `ls-files -s` then `update-index` | changes/file_action_window.cpp:373 | write (feeds update-index) | git | greyed |
| `rm`, `mv`, `add`, `rm -f` | changes/remove.cpp:91, rename_dialog.cpp:85, conflict_editor.cpp:63, 132 | write | git | greyed |

## Branches, tags, references

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `show-ref --verify --quiet` | branch/create_ref_dialog.cpp:30 | click | libgit2: ref lookup | greyed (Create writes) |
| `rev-parse --is-bare-repository` | branch/create_ref_dialog.cpp:60 | click (dialog opens) | libgit2: `repo::bare()` | greyed |
| `check-ref-format refs/{heads,tags}/<n>` | branch/create_ref_dialog.cpp:153 | click | libgit2: `git_reference_name_is_valid` | greyed |
| `branch`/`tag` create, `config branch.<b>.description` | branch/create_ref_dialog.cpp:213, 223, 237 | write (signing) | git | greyed |
| `check-ref-format --branch` | branch/switch_dialog.cpp:150 | click | libgit2 | greyed |
| `show-ref --verify --quiet refs/heads/<n>` | branch/switch_dialog.cpp:154 | click | libgit2 | greyed |
| `for-each-ref` (chooser lists) | branch/ref_chooser.cpp:19 | click (dialog opens) | libgit2: ref iteration | works |
| `show-ref --verify --quiet` | branch/ref_chooser.cpp:36 | click | libgit2 | works |
| `rev-parse --symbolic-full-name --verify` | branch/ref_chooser.cpp:142 | click (dialog opens) | libgit2: dwim ref | works |
| `merge-base HEAD <rev>` | branch/merge_dialog.cpp:261 | click (after merge, offer to delete) | libgit2: merge base | greyed (merge) |
| `branch -D` | branch/merge_dialog.cpp:276 | write | git | greyed |
| command list (delete, rename ...) | branch/browse_refs.cpp:851 | write | git | greyed |
| `branch --set-upstream-to` | branch/browse_refs.cpp:1029 | write | git | greyed |
| `for-each-ref` (every field) | git/ref_list.cpp:64 | click (Browse References opens) | libgit2: refs, tag/commit metadata, upstream:track | works |
| `config --null --get-regexp branch.*.description` | git/ref_list.cpp:107 | click | libgit2: config iterator | works |

## Remotes, pull requests, sync dialogs

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `config --get`, `rev-parse`, `symbolic-ref` (`query`) | git/remotes.cpp:36 | click | libgit2 already, git fallback | works |
| `remote` | git/remotes.cpp:85 | click | libgit2 already, git fallback | works |
| `for-each-ref refs/heads` | git/remotes.cpp:150 | click | libgit2 already, git fallback | works |
| `merge-base --is-ancestor` | git/remotes.cpp:176 | click | libgit2 already, git fallback | works |
| `diff --name-only --diff-filter=U` | git/remotes.cpp:199 | click | libgit2 already, git fallback | works |
| `config ...` / `remote remove` | ui/remotes_page.cpp:272, 300 | write | git | greyed |
| `for-each-ref refs/remotes/<r>/` | ui/pull_request_dialog.cpp:251 | click (dialog) | libgit2 | works |
| `symbolic-ref --short refs/remotes/<r>/HEAD` | ui/pull_request_dialog.cpp:266 | click | libgit2 | works |
| `log --format=%s%x1f%b%x1e base..tip` | ui/pull_request_dialog.cpp:299 | click | libgit2: revwalk + messages | works |
| progress runner (clone, fetch, pull, push, submodule ...) | ui/progress_dialog.cpp:57 | net / write | git | greyed |

## Rebase, history edits, merge tools

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `ls-files --others --exclude-standard` | rebase/rebase_engine.cpp:57 | status | git | greyed |
| `ls-tree -r --name-only --full-tree <onto>` | rebase/rebase_engine.cpp:61 | worker (before rebasing) | libgit2: tree walk | greyed |
| `config --bool core.ignorecase` | rebase/rebase_engine.cpp:65 | worker | libgit2: config bool | greyed |
| rebase steps | rebase/rebase_engine.cpp:116 | write (hooks, continuable) | git | greyed |
| `cherry <u> <b>` | rebase/rebase_window.cpp:356 | worker | git: patch-id equivalence is git's | greyed |
| `stash push/pop` | rebase/rebase_window.cpp:601, 686, 707 | write | git | greyed |
| `var GIT_COMMITTER_IDENT` | rebase/rebase_window.cpp:617 | click (Start rebase) | git: git's identity rules (env, useConfigOnly, EMAIL, host name) are its own, and the rebase that follows runs git anyway | greyed |
| `status --porcelain --untracked-files=no` | rebase/rebase_window.cpp:655 | status | git | greyed |
| `stash list --format=%H` | git/history_edit.cpp:531 | worker | libgit2: stash foreach | works (read) |
| `stash apply/drop/push`, `read-tree -m -u` | git/history_edit.cpp:601, 607, 612, 632 | write | git | greyed (when the edit ends in a checkout) |
| `rev-parse --verify refs/stash` | git/history_edit.cpp:614 | worker (right after a stash push) | libgit2: ref lookup | greyed |
| `ls-files -u`, `checkout-index --temp` | git/merge_tool.cpp:114, 215, 301 | click (merge tool) | other agent (tool launch path) | greyed |
| compare / diff tool | ui/compare.cpp, git/diff_tool.cpp | (various) | other agent | other agent |

## Explorer and the shell

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `log -2 --format=%H -- <path>` (prevdiff) | main.cpp:1015 | click (diff tool launch) | other agent (diff tool path); a path-limited walk besides | greyed |
| `status --porcelain -z` | git/badges.cpp:121 | status (badge cache) | git while installed; `git_status_list` without it | fallback |
| `ls-files -z` | git/badges.cpp:131 | status (badge cache, same pass) | git while installed; libgit2 index without it | fallback |
| `rev-parse --absolute-git-dir` | git/repo_operation.cpp:83 | click (what's in progress) | libgit2: `git_repository_path` | works |
| `svn --version` | git/svn.cpp:20 | net (git svn) | git | off |
| `rev-parse --git-dir` (safe.directory check) | ui/trust_repository.cpp:53 | click | git: this asks git's own opinion of the folder's owner | works (libgit2 opens it) |
| `config --global --add safe.directory` | ui/trust_repository.cpp:88 | write | git | off |
| `init` | ui/create_repo_dialog.cpp:90 | write | git | greyed |
| `check-ref-format --branch` | remote/clone_dialog.cpp:184, ui/submodule_dialogs.cpp:111 | click | libgit2: `git_branch_name_is_valid` | greyed |
| `apply -z --numstat`, `apply --check` | ui/review_patch.cpp:196, 219 | worker | git: git apply's own rules | greyed |
| `apply` (patch serial) | git/patch_serial.cpp:169 | write | git | greyed |
| `status`, `config --get svn.*`, `rev-parse`, `merge-base`, svn ignore | ui/svn_dialogs.cpp:65-385 | git svn | git (svn is git's) | off |
| `cat-file --filters` (repo browser Open / Save) | ui/repo_browser.cpp:551, 578, 602 | net/hooks/filters | other agent (`revision_file`) | not yet: fails until `revision_file` reads blobs in process (fast-difftool) |
| blame history, blame | blame/blame_window.cpp:329, 388 | walk | git: blame and `--follow` are git's | not yet: the Blame command needs git (libgit2 blame to come) |
| diff window | ui/diff_window.cpp:198 | worker | git: git's patch text; libgit2 patch when git is missing | fallback |

## Not git

| Call | Where | Class | Decision | No git |
|---|---|---|---|---|
| `gh auth token --hostname <h>` | forge/forge.cpp:425 | worker (log opens, twice) | cached per session | off |
