# Azure DevOps (read-only)

For Azure HTTPS remotes, Tortilla automatically consults Git's configured
credential helper (including GCM) when there is no explicitly configured
organization account. A pasted **Azure DevOps** PAT in Settings, server
`dev.azure.com/organization`, overrides the automatic source deterministically.
PAT instructions request Code (Read) and Build (Read); this integration
only reads. Browser OAuth app registration is not implemented; existing GCM
OAuth sign-ins can be reused through Git's credential protocol.

Accounts are per organization, even if a PAT grants access to several.
The existing account setting and keychain key contain the organization
scope; there is no host-wide token fallback. Modern HTTPS, Azure SSH v3,
and legacy visualstudio.com remotes normalize to the same organization.
Each path component is decoded once and independently encoded for API URLs.
Encoded separators and dot segments are rejected. Basic authentication uses
an empty username and PAT password, restricted to that organization's path.
Every authenticated read checks each redirect before it is sent:

- Organization-approved: https://dev.azure.com/{organization}/... or the
  legacy https://{organization}.visualstudio.com. Both the as-sent and
  decoded-dot interpretations must stay in that organization. Rapport keeps
  origin credentials only on the original origin: a transition between
  modern and legacy hosts is followed without them, even for the same org.
- Stopped before the hop, with the error `Azure DevOps redirected outside
  organization X`: another organization on the shared dev.azure.com origin
  (any port) or another organization's legacy host, an encoded slash or
  backslash, a path that climbs out of the organization, and any downgrade
  to http.
- Followed without credentials: any other https host, such as
  `*.blob.core.windows.net`, `*.vsblob.visualstudio.com` or an artifacts CDN,
  where Azure Pipelines artifact and log downloads go with a signed URL.
  Caller Authorization, Cookie, explicit Proxy-Authorization and URL userinfo
  stay off that hop and every later one, even one back at
  dev.azure.com/{organization}. Proxy-configuration credentials remain with
  the proxy; they are not credentials for an origin.

Rapport supplies callback URLs as sent: literal dot segments removed, fragments
dropped and an empty path replaced by `/`; percent escapes remain as sent.
The organization check also handles decoded dot segments conservatively.
Requests without credentials are not checked.

Identity uses only the documented `connectionData.authenticatedUser.id` as
its stable login and `providerDisplayName` (or `customDisplayName`) as the
friendly display name. The arbitrary Identity `properties` bag is not
interpreted: there is no documented `Account.$value` contract. This avoids
the separate global Profile API and Profile scope. Microsoft's SDK
`WebApi.connect()` uses the unversioned `/_apis/connectionData` route;
Tortilla follows that explicit exception to REST 7.1 versioned requests.
Identity avatars are omitted because those endpoints may need credentials
on a different origin.

PR lists include labels, branches, head commit, drafts and reviewer votes.
The selected PR fetches its full description (the list truncates it), reviews
and policy evaluations. Write actions (create, merge, auto-complete, close,
branch deletion and fork targeting) are explicitly unsupported.

Pipelines are found by repository GUID, paging build history and selecting
the newest matching source commit per definition. Azure's Build list has no
sourceVersion query parameter, so a repository with a large build history
may require many requests. Stage/Phase/Job ancestry groups the matrix;
failed task issues and bounded log tails supply failure details. Timeline
GUIDs and build IDs are retained separately from stable process-local numeric
job cache keys. External commit statuses and run artifacts are also shown;
artifact size is unknown until downloading.

## Git credential source

The automatic account carries canonical organization, the decoded username
from the original HTTPS remote (if present), and repository workdir for local
helper/account configuration. No password is copied from a remote URL. This
metadata is generated per repository, not serialized as a saved account.
SSH recognition is unchanged; no additional SSH credential work is added.

Only `git credential fill` reads credentials. Tortilla does not call approve,
reject, GCM list/bind/unbind, Windows Credential/Vault APIs, registry or MSAL
cache files. It does not copy a helper secret into settings or the keychain.
Git Credential Manager itself remains responsible for its cache and refresh.
`fill` is not a cache-only or provider-read-only guarantee: in optional PAT mode,
GCM can create a PAT from an existing Entra session without showing UI.
`GCM_INTERACTIVE=never` prevents interactive prompts, not provider-side token
creation/refresh or helper-managed cache writes. We found no documented
cache-only `fill` capability; Tortilla does not claim one or bypass the helper.
The owner accepts helper-managed creation/refresh as the same credential
behavior as an ordinary Git fetch; Tortilla still issues only `fill`.
See the [GCM provider implementation](https://github.com/git-ecosystem/git-credential-manager/blob/main/src/Microsoft.AzureRepos/AzureReposHostProvider.cs)
and [noninteractive setting](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/environment.md#gcm_interactive).
No helper commands are added to fetch/push/askpass transport.

The stdin protocol begins with `capability[]=authtype`, then `protocol=https`,
`host=dev.azure.com`, `path={organization}`, optional `username={remote user}`,
and a terminating blank line. The path is the organization alone, never
`dev.azure.com/org` or a full project path. Generic, host-specific and exact
organization-specific command-local `credential.useHttpPath=true` prevent Git
from stripping it. `git -C` retains repository-local bindings. Legacy HTTPS
remotes use the same canonical lookup; GCM PATs cached only under a legacy
visualstudio.com URL might not be found. That produces the explicit paste-PAT
fallback rather than a different-host/account probe.

The dedicated child runner supplies stdin directly, caps stdout at 64 KiB,
discards stderr, and has a 10-second deadline. Windows starts hidden/suspended
into its own kill-on-close job with only the intended pipe handles inherited;
POSIX creates its own process group. Timeout/output-limit termination affects
only that child/group/job. The child's environment disables GCM interactivity,
GUI prompts, terminal prompting, both askpass variables and credential tracing;
command-local `core.askPass=` also disables configured askpass. No process-global
Git config/environment is changed. Test execution substitutes a fake runner or
a dedicated dummy executable, never the user's credential helper/store.

Capability-declared Basic/Bearer credentials use the declared HTTP scheme
without guessing from token contents. Legacy username/password preserves the
**exact helper username and password** in Basic authentication. Because that
legacy shape cannot distinguish PAT from OAuth, a read-only request rejected
with HTTP401 or HTML203 may retry the same opaque password **once as Bearer**.
This is a documented compatibility policy, not a claim that Microsoft documents
OAuth-over-Basic for REST. JSON203, 403, 429 and 5xx do not negotiate. Explicit
schemes and pasted PATs do not negotiate. Neither failure causes approve/reject,
credential deletion or a silent switch to another identity. Future write support
must resolve its scheme with a read-only preflight, not by retrying mutations.

Accepted mode is remembered in a maximum-128-entry, 10-minute in-memory cache
keyed by a SHA256 fingerprint scoped to organization/workdir/remote username
and helper credential. It contains no passwords and is never serialized;
a changed credential resets the mode. Actual credential material belongs to a
typed worker-owned RAII context. Existing string-token APIs receive only an
opaque handle; a weak registry bridges them. An internal context marker ensures
concurrent organizations sharing identical credential bytes cannot alias the
scope gate, and is stripped before every real/fake HTTP transport. Context
destruction releases material and removes the opaque binding. Panel and PR
watch workers do not keep helper tokens in their long-lived token caches.

Sources: [Git credential protocol](https://git-scm.com/docs/git-credential),
[credential contexts/useHttpPath/askpass](https://git-scm.com/docs/gitcredentials),
[GCM Azure accounts/tokens](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/azrepos-users-and-tokens.md),
[GCM interactivity and tracing settings](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/configuration.md),
[GCM PAT/OAuth provider source](https://github.com/git-ecosystem/git-credential-manager/blob/main/src/Microsoft.AzureRepos/AzureReposHostProvider.cs),
[Microsoft REST Bearer samples](https://learn.microsoft.com/en-us/azure/devops/integrate/get-started/rest/samples?view=azure-devops).

## Validation and user diagnostics

**Documentation/fixture-validated only; no live Azure organization validation
is available.** The first customer use will be the first live exercise.
Acceptance evidence is offline fixtures, null-backend UI tests and the six
clean exact-head GitHub CI jobs, not an expected future organization check.

Errors retain `Azure DevOps HTTP <status>` and the root JSON `message` when
present. Messages are bounded, control characters are flattened, and raw,
URL-encoded and Basic-encoded PATs are redacted. Raw HTML is never displayed. Azure artifact responses are validated in a
unique sibling staging file; HTTP/JSON/HTML errors never replace or delete
the existing destination content.
HTTP 401 and HTTP 203 with an HTML sign-in page explain invalid/expired
Git credentials (or PAT for pasted tokens), or missing scope; known Git/policy endpoints name Code read access,
and Build/timeline/log/artifact endpoints name Build (Read). HTTP 203 JSON
is an ordinary unexpected HTTP response, not automatically an auth error.
A 403 shows the service message plus the endpoint's likely scope, without
claiming that scope is the only possible cause. A 404 is not silently
converted to an empty list: Azure also uses it for missing permissions.

PR-list errors and selected-row CI/artifact errors are combined in the
always-present log status, outside conditional lower pages. Identical
errors do not rewrite the status; a row change or recovery clears the
selected-row error. Failed-job errors use the existing failure pane;
selected-PR policy errors use the existing PR detail error path. Artifact
errors also appear beside the valid build summary and do not change its
CI verdict. No modal dialogs are added for background polls.

If the user reports an error, request only the visible HTTP status,
sanitized Azure message, endpoint category (PR, policy, build, timeline,
log or artifact), and whether a modern or legacy remote is used. Repo,
project and organization names may be replaced with placeholders. Do not
request tokens, Authorization headers, credential-helper output or raw
HTML. Reproduce the reported response with an offline transport fixture.

## Endpoint → reference → offline fixture checklist

All request construction sites have adjacent exact reference URLs.
Fixture IDs refer to comments in `tests/azure_tests.cpp`; fixture values
are synthetic and use the documented fields/types, not captured customer
responses. D-series cases deliberately inject unsuccessful HTTP responses.

| Endpoint | Exact reference | Fixture / coverage |
| --- | --- | --- |
| `/{org}/_apis/connectionData` (unversioned) | [Official SDK WebApi.connect](https://github.com/microsoft/azure-devops-node-api/blob/master/api/WebApi.ts), [ConnectionData](https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/connectiondata), [Identity](https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/identity) | F3: authenticatedUser.id/providerDisplayName; D1: sign-in error |
| Git `repositories/{repo}` | [Get repository 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/repositories/get-repository?view=azure-devops-rest-7.1) | F5: repository GUID resolution; remotes-page probe shares route |
| Git `pullrequests?searchCriteria.status=active&$top=100` | [List PRs 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1) | F1: list, branch/head/label/draft/fork fields; D1/D2/D3 diagnostics |
| Git `pullrequests/{id}` | [Get PR 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-request?view=azure-devops-rest-7.1) | F1: full description, reviewer list, project GUID; E6 merge statuses |
| Embedded `reviewers[].vote` (no extra reviewers request) | [Reviewers 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-request-reviewers/list?view=azure-devops-rest-7.1) | F1/E5: all five votes and unknown values |
| Git `refs?filter=heads/&$top=100` | [Refs list 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/refs/list?view=azure-devops-rest-7.1) | F11: name/objectId; ref errors propagate |
| Policy `evaluations?artifactId=...` | [Evaluations list, 7.1-preview.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1) | F4/E4: required policy rejection, every status, unknown; permission errors |
| Build `builds?repositoryId=...&repositoryType=TfsGit&queryOrder=queueTimeDescending&$top=100` and continuationToken | [Builds list 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1) | F6/E2: repository filter, pagination, commit match/newest definition, all build statuses/results |
| Build `builds/{id}/timeline` (timelineId optional) | [Timeline get 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1) | F2/E1: Stage/Phase/Job/Task ancestry, UUID cache identity, every timeline state/result, failure issues |
| Git `commits/{sha}/statuses?latestOnly=true` | [Statuses list 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1) | F7/E3: context/state/targetUrl and every status |
| Build `builds/{id}/artifacts` and `resource.downloadUrl` | [Artifacts list 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/artifacts/list?view=azure-devops-rest-7.1) | F8: names/download URL/unknown size, one request per run; 403 scope error; null UI error propagation |
| Build `builds/{id}/logs` | [Logs list 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-logs?view=azure-devops-rest-7.1) | F9: id/lineCount; lookup errors propagate |
| Build `builds/{id}/logs/{logId}?startLine=...` with Accept text/plain | [Get log 7.1](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-log?view=azure-devops-rest-7.1) | F10: plain log tail; bounded startLine request |
| PAT Basic header / token settings page | [PAT authentication](https://learn.microsoft.com/en-us/azure/devops/organizations/accounts/use-personal-access-tokens-to-authenticate?view=azure-devops) | empty-username Base64, scoped gate, legacy/SSH aliases, no OAuth |
| Local `git credential fill` (no Azure write endpoint) | [Git protocol](https://git-scm.com/docs/git-credential), [GCM settings](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/configuration.md) | `azure_credential_tests.cpp`: exact stdin, parser, bounds, source precedence, negotiation/scope/lifetime; `azure_process_tests.cpp`: real isolated dummy child stdin/env/deadline/output cap |
| Threads, refs mutation, PR creation/completion/abandon | No requests in PR1 | mutation-refusal transport assertion; no network or hidden write fallback |

## Mappings, ambiguities and limits

- Unknown enums map to `none`/unknown. Unknown reviewer votes are omitted
  with reviews marked incomplete; unknown PR merge states leave merge
  unknown. Votes 10/5 approve, -10 requests changes, and 0/-5 occupy the
  existing requested/waiting group (which has no separate waiting-for-author
  enum). Future positive votes are never assumed to approve.
- Policies: queued/running → pending, approved → success, rejected/broken →
  failure, notApplicable → none. Blocking rejected/broken/waiting policies
  block merge. Unknown policy values keep `checks_known=false`.
- Timeline: succeeded/succeededWithIssues → success; failed/canceled/abandoned
  → failure; skipped → none. Without a result, pending/inProgress → pending,
  completed/unknown → none. These are distinct from BuildResult.
- Builds: succeeded → success; partiallySucceeded/failed/canceled → failure.
  With none/null result, inProgress/cancelling/postponed/notStarted → pending;
  completed/none/all/unknown → none. `all` is a documented query mask, not a
  concrete outcome. External statuses: succeeded → success, failed/error →
  failure, pending → pending, notSet/notApplicable/unknown → none.
- PR merge notSet/queued/unknown → unknown; conflicts → known conflict;
  succeeded → known; rejectedByPolicy/failure → known blocked. The integration
  remains read-only regardless of those values.
- Active PRs and refs are capped at the first 100, matching the existing
  forge-list contract; a full refs page is marked incomplete. Build history
  follows continuation tokens without an invented sourceVersion parameter,
  selects newest matching run per definition, and rejects repeated tokens.
  External statuses ask for latestOnly; Azure's default page limit applies.
- The PR list truncates descriptions; only the detail request marks the body
  complete. Missing optional labels/avatars/times/links stay empty/zero.
  Missing required list/timeline arrays produce explicit response errors.
- Job cache IDs are negative local IDs distinct from status ID0 and other
  forges' positive IDs. The key includes repository organization/project,
  build ID, timeline UUID and attempt. The process-local map lasts for the
  process; it does not persist IDs or hash GUIDs.
- Log-list lineCount selects the final approximately 100 lines, then the
  existing log-tail helper keeps 80. Each failure retrieval is bounded;
  missing log references leave issues available without fabricating a log.
- Artifact URLs outside the organization receive no PAT (refused before
  sending). Rapport's per-hop `on_redirect` applies `azure::redirect_decision`,
  through `forge_detail`'s guard, to every authenticated request: signed
  downloads on other hosts go on without credentials. There are no authenticated identity-image
  requests. No development/test credential-store probing or live Azure requests are used.

Existing per-row completed/error results remain cached by the forge panel.
PR-watch retries on its existing schedule, but an initial CI lookup failure
without a subsequent PR-placement/source change may require reopening the
log tab after credentials become available. No additional timers or layout
changes are introduced by credential reuse.

## Lower-pane integration

Azure HTTPS remotes enter the existing `forge_remotes(workdir)` discovery path
with a canonical per-organization account. The lower tabs therefore retain
their strip even on a Commit-only row, using the same `forge_possible` predicate
as other providers. PR, Build and Artifacts visibility remains strict per row;
errors use the always-visible log status without fabricating Build presence.
The master sticky-choice and silent-fallback behavior is retained unchanged.
