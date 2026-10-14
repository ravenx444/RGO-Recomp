# ROM and game-data handling

Users supply their own supported ROM. The project does not provide ROMs or
game assets. Validate the selected file locally; do not scan unrelated folders
or transmit the file.

## Repository and submissions

Keep ROMs, extracted assets, generated game code, unrestricted memory captures,
save states, and game-data-bearing logs or dumps out of commits and GitHub
attachments, releases, caches, and CI output. Encoding or archiving a file does
not change its contents or classification. Preserve third-party notices and
do not copy material without an applicable permission/license basis.

Project-authored source, synthetic fixtures, schemas, supported-ROM metadata,
and reviewed hashes/aggregate results can be tracked. A recorded hash does
not authorize publishing its underlying input.

The Windows-only setup candidate uses `symbols/native-us-v1` as supported-ROM
metadata. Its entire payload is admitted by the strict field contract in
`scripts/native_generation_metadata.py` and a manifest digest pinned in source.
Only symbol names, numeric addresses, sizes, ownership relationships and audio
extents are included. Instruction words, executable bodies, ROM bytes, audio
bytes, extracted assets, raw ELF/map exports and private paths are excluded.
Repository content scanning still applies without an exemption for this folder.
Changing the metadata requires a fresh facts review, matching source provenance
and regeneration comparison before changing its pinned digest.


For bug reports, attach the launcher's exported support ZIP using the
[playtesting guide](../playtesting.md). Crash/hang dump export is planned;
the current launcher exports filtered text logs and does not produce dumps.
Any future dump format needs an explicit contents/size contract and verified
filtering before it becomes a supported GitHub attachment.

## Local inputs and output

Use ignored locations such as `roms/`, `generated/`, `src/generated/`,
`corpus/private/`, `captures/private/`, and `artifacts/private/`, or an explicit
location outside the checkout. Keep generated bodies and private evidence
separate from source. Avoid printing their contents or machine-specific paths.
Detailed corpus controls are in the [test-corpus policy](test-corpus-policy.md).

Packaging must exclude ROMs, extracted assets, private test bodies, and
unapproved generated output. Diagnostic collection is opt-in and filters
game data, saves, personal paths, and identifiers before export.

## Enforcement

Ignore rules, tracked-path checks, repository content scanning, and synthetic
canaries enforce the boundary. Scan source, packages, debug symbols, and output
before publication; automated scanning does not replace maintainer review.
Boundary changes and release approval remain owner decisions.

Report accidental disclosure through [security reporting](../../SECURITY.md).
Do not paste exposed bytes into an issue as evidence.
