# Security policy

Security fixes target the default branch. Development previews do not have a
long-term support commitment; reports should identify the exact affected build.

## Report a vulnerability

Use the repository's [private vulnerability reporting](https://github.com/ravenx444/RGO-Recomp/security).
Keep exploit details out of ordinary issues and pull requests. If private reporting
is unavailable, use an existing private maintainer channel or retain the report
locally until private reporting is available.

Include the affected commit/component, impact, required attacker capabilities,
and minimal reproduction steps using synthetic data. Omit credentials, personal
information, game data, and unrestricted memory or log contents.

Maintainers aim to acknowledge reports within three business days and provide
initial triage within seven. These are response goals, not guarantees. Coordinate
public disclosure with the maintainer after a fix or an agreed disclosure date.

For ordinary crashes and gameplay problems, use the [playtesting guide](docs/playtesting.md).

## Development requirements

- Treat ROMs, saves, configuration, mods, and imported files as untrusted input.
  Validate bounds, archive paths, and integer arithmetic before allocation/access.
- Keep save writes atomic and recoverable. New mod execution requires a reviewed
  threat and permission model.
- Do not expose secrets to fork code, unreviewed changes, or agent workers.
  Keep workflow permissions minimal and Actions/dependencies pinned.
- Preserve the [data-handling rules](docs/legal/rom-and-assets-policy.md) and
  [isolated corpus-runner requirements](docs/legal/test-corpus-policy.md).
- Diagnostic collection is explicit and user-controlled; filter sensitive data
  before export and do not silently upload files.
- Dependency, workflow, packaging, and data-boundary changes need maintainer review.
  Release packages need clean-environment validation, an inventory, and notices.
- Publishing source, history, packages, or releases requires owner authorization.

For a disclosure incident, stop affected distribution, contact maintainers
privately, restrict/remove affected artifacts, rotate exposed credentials, and
verify remediation. Preserve incident metadata without copying exposed content.
