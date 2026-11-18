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

Identity is read from the organization's `connectionData.authenticatedUser`:
`properties.Account.$value`, then `uniqueName`, then stable `id` for login;
`providerDisplayName` (or `customDisplayName`) for the display name. This
avoids calling the separate global profile API or requesting Profile scope.
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

## Validation with a real organization

- Sign in with the customer's organization PAT; confirm friendly identity,
  revocation/expiry handling and that another organization's PAT is not used.
- Compare active PRs, full descriptions, reviewer votes, labels and required
  build-policy results with Azure's web pages, including a private repo and
  escaped project names.
- Check successful/running/failed pipelines, stages, retries and external CI
  on both recent and older commits; confirm missing build permissions are
  reported accurately.
- Open a failed job and compare its step, issues and tail; check a large log.
- Download both build and pipeline artifacts, including signed blob redirects.
- Verify write actions remain disabled and no authenticated avatar request is
  attempted. No live organization or GUI is needed by the fixture tests.

## API references

- [PAT authentication](https://learn.microsoft.com/en-us/azure/devops/organizations/accounts/use-personal-access-tokens-to-authenticate?view=azure-devops)
- [ConnectionData](https://learn.microsoft.com/en-us/javascript/api/azure-devops-extension-api/connectiondata)
- [Pull requests](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/pull-requests/get-pull-requests?view=azure-devops-rest-7.1)
- [Policy evaluations (7.1-preview.1)](https://learn.microsoft.com/en-us/rest/api/azure/devops/policy/evaluations/list?view=azure-devops-rest-7.1)
- [Builds](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/list?view=azure-devops-rest-7.1)
- [Timelines](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/timeline/get?view=azure-devops-rest-7.1)
- [Build logs](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/builds/get-build-log?view=azure-devops-rest-7.1)
- [Artifacts](https://learn.microsoft.com/en-us/rest/api/azure/devops/build/artifacts/list?view=azure-devops-rest-7.1)
- [Commit statuses](https://learn.microsoft.com/en-us/rest/api/azure/devops/git/statuses/list?view=azure-devops-rest-7.1)
