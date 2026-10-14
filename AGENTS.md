# RGO development workflow

The owner confirmed on 2026-10-14 that this fork uses its normal build and
test workflow. The inherited upstream autonomous-investigation supervisor
and its private ledger are not requirements for RGO development. Do not
create or reset an upstream investigation ledger to work on this fork.

- Preserve existing user changes and local helpers.
- Run the relevant tests, project validation, and repository hygiene checks.
- Validate Windows launcher builds locally; automated tests alone do not
  establish gameplay behavior or first-run setup on another PC.
- Preserve previous release artifacts. Package only the launcher and its
  reviewed documentation and license files.
- Never commit or release ROMs, ROM-derived code/assets, or built game output.
- Changes to repository visibility, releases, and upstream projects require
  the owner's authorization. Do not treat a development task as publication
  permission.

# Upstream reference knowledge

For upstream dependency updates or overlay, controller/save, camera, and
audio investigations, consult [the pinned reference review](docs/upstream/reference-review-2026-10-01.md)
and [reference catalog](docs/upstream/reference-catalog.json). The catalog
separates existing behavior, source observations, and unvalidated work
items. It does not authorize runtime changes, reopen shelved investigations,
or override this fork's normal development workflow.

# RGO Git identity

For owner-authorized commits in this fork, use the authenticated owner's
GitHub no-reply identity: `ravenx444` with
`19757470+ravenx444@users.noreply.github.com`. Keep commit timestamps in UTC.
Preserve upstream and contributor authorship; do not rewrite existing history
or use the upstream maintainer's identity for new RGO work.
