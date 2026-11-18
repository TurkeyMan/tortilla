# Azure DevOps (read-only)

Add an **Azure DevOps** account in Settings with server
`dev.azure.com/organization` and a pasted Personal Access Token. The token
instructions request Code (Read & Write) and Build (Read); this integration
only reads. Browser OAuth and importing Git Credential Manager credentials
are not implemented.

Accounts are per organization, even if a PAT grants access to several.
The existing account setting and keychain key contain the organization
scope; there is no host-wide token fallback. Modern HTTPS, Azure SSH v3,
and legacy visualstudio.com remotes normalize to the same organization.
Each path component is decoded once and independently encoded for API URLs.
Encoded separators and dot segments are rejected. Basic authentication uses
an empty username and PAT password, restricted to that organization's path.
Redirects retain Rapport's existing origin protections. Signed artifact URLs
outside the organization are fetched without the PAT.

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

Git Credential Manager can cache PATs or Entra/OAuth credentials, so its
stored entry is not necessarily a reusable PAT. Import needs a separately
designed account/token lifecycle and is deliberately outside this change.
No credential-manager commands or user secrets were inspected.
See [GCM Azure credential types](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/configuration.md#credentialazreposcredentialtype).

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
HTTP 401 and HTTP 203 with an HTML sign-in page explain an invalid/expired
PAT or missing scope; known Git/policy endpoints name Code read access,
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
- Signed artifact URLs outside the organization receive no PAT. Rapport
  retains its origin-based redirect protection; it does not expose a
  per-path redirect callback. There are no authenticated identity-image
  requests. No credential-helper probing or live Azure requests are used.
