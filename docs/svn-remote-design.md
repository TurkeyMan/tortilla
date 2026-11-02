# SVN remotes as restricted remotes (parked)

A design agreed in discussion and deliberately not built: SVN's user base is
small and shrinking, and plain git-svn with Tortilla's basic SVN dialogs
(DCommit, Rebase, Fetch, Import Ignore) serves it. Build this only if users
ask.

The principle: enforce SVN's rules at the moment of each action, so a
branch is always ready to dcommit, rather than letting it drift and
repairing it at push time.

## SVN-bound branches

A local branch whose upstream is a git-svn branch. Their labels, and the SVN
remote branches (svn/trunk...), wear a small "svn" mark in the log.

## SVN as a remote

| Action | For an SVN remote |
|---|---|
| Fetch / the Fetch button | `git svn fetch`; slow, so on click, or a cheap `svn info` revision probe in the background |
| Pull | always `git svn rebase` |
| Push | only `git svn dcommit`, to the branch's own SVN branch |
| Settings > Remotes | lists `svn-remote.*`, read-only |

## Rules on SVN-bound branches

- Merging a git-only branch in: only as a squash or a rebase onto, never a
  true merge.
- Merging another SVN branch in: allowed; `svn.pushmergeinfo=true` so
  dcommit records `svn:mergeinfo`.
- Commits already in SVN (`git-svn-id`): immutable. Reword, drop, combine,
  reset past and rebase across them are greyed "(in SVN)". Unsent commits
  are free.
- Pushing an SVN-bound branch to a git remote warns: dcommit renumbers those
  commits.
- New SVN branches or tags only by explicit actions (`git svn branch`,
  `git svn tag`), warned as public and permanent on the server; greyed when
  the clone has no branches path (no stdlayout).
- A push containing a merge of git-only work asks: send it flattened as one
  revision, or create an SVN branch for it (name editable) and merge with
  mergeinfo, or cancel; remembered per repository.
- After dcommit, local branches built on the sent commits are rebased onto
  their rewritten versions.

Local topic branches made from an SVN branch stay ordinary git branches; the
rules apply as their work comes back into the SVN branch.
