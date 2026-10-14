# Windows generation metadata review

The Windows-only release candidate replaces the matching decompilation build
with names and numeric facts in `symbols/native-us-v1`. The source pins match
`dependencies.lock.json` and `symbols/provenance.json`. This local candidate
has not been publicly released.

The payload contains 156 executable sections, 2,909 authoritative function
boundaries, 324 data-symbol sections, 824 alias relationships and 10 validated
size-recovery records. Audio metadata records the ROM extent and branch/overlay
addresses only; all audio instruction bytes come from the player's own ROM.

| File | Bytes | SHA256 |
|---|---:|---|
| bindings.json | 180913 | `53249e8a6a135f30e0b4bde7d1ced9cd91526cd3d428cf79e32a0e92c30f0512` |
| data-symbols.toml | 787430 | `bc4bffe757b604992bb7d2c40526a508fec66808dc71f75b0286ab18f6af8762` |
| functions.toml | 218093 | `adf5e2acf7219b5ba33ec2c3daa5b7a4718caf8b1838ea064a8f7132f545be3c` |

The manifest SHA256 is pinned separately in the metadata loader. The loader
rejects unknown fields, nonnumeric coordinates, executable payload fields,
unexpected inventory counts, changed files and inconsistent source revisions.
Repository scanners found no game bodies, encoded payloads, ROM markers,
credentials, private paths or raw ELF/map exports. No scanner exemption was
added for these files.

A local regeneration comparison admitted the same metadata and reproduced
the patched ROM, function inventory, relocation data and timing hooks. The
complete Windows CPU/normalization/SDK/audio pipeline was compared with the
preserved Linux baseline. All 5,880 code/support files match after CRLF/LF
normalization. The source manifest differs only in the actual normalizer
revision digest. SDK identification is identical. Audio matches after sorting
only contiguous, uniquely keyed, unconditional case-to-label statements;
all other statements and targets remain identical.

These checks establish the factual payload and generation portability. They
do not substitute for fresh game compilation, player setup or gameplay tests.
Never add ROM bytes, instruction words, generated game bodies, assets or
raw linker/ELF dumps to this bundle.
