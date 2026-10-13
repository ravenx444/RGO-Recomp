# Third-Party Dependency Status

The root MIT license covers original project contributions. Existing third-party
copyright and license notices retain their separate scope. It does not relicense
game content, generated ROM-derived output, or third-party code. Distribution of
a combined work must also satisfy every applicable dependency license, including
GPL requirements where GPL-covered components are used.

Third-party font files are vendored under `launcher/ui/fonts`, with their
SIL Open Font License notices and source/hash manifest. The launcher statically
links RmlUi, FreeType and SDL 2.26.3, fetched at the revisions in `launcher/ui/CMakeLists.txt`.
Their retained notices are indexed in [launcher notices](launcher/ui/THIRD-PARTY.txt)
and [notice provenance](launcher/ui/licenses/provenance.json).

Other external repositories are cloned locally under the ignored `tools/`
directory at commits recorded in `dependencies.lock.json`. These local build
and research dependencies are distinct from the components bundled in the
launcher release archive.

The tracked N64Recomp patch series necessarily contains limited context from
the MIT-licensed upstream source. Its required copyright and permission notice
is preserved byte-for-byte from the locked upstream revision in
`patches/n64recomp/LICENSE.upstream`. That notice applies to the upstream
material represented in the patch payload; it does not change the license of
project-authored material.

The RT64/Plume compatibility edits embedded in `CMakeLists.txt` retain
the exact pinned MIT notices in `patches/rt64/LICENSE.upstream` and
`patches/plume/LICENSE.upstream`.

The CIC-NUS-6105 challenge-response algorithm in
`src/runtime/cic_nus_6105.cpp` is conservatively attributed to X-Scale (2011).
The complete upstream copyright, redistribution conditions, disclaimer and
views paragraph are retained both in that source and in
`patches/cic/LICENSE.upstream`. The source was inspected at BizHawk commit
`bdddf4a58aa1a022afb11dc73294a81a5aa7bbd5`, path
`libmupen64plus/mupen64plus-core/src/memory/n64_cic_nus_6105.c`.
The project integration uses C++ types and packed PIF RAM. This notice applies
to the upstream algorithm; the project license does not override upstream rights.

## Components and attribution

| Component | Status |
|---|---|
| N64Recomp | MIT; local build tool; tracked patch context carries `patches/n64recomp/LICENSE.upstream` |
| LLVM/Clang | Apache-2.0 WITH LLVM-exception; ignored local compiler toolchain |
| OpenSSH `ssh-keygen` | BSD-style/component-specific notices; OS-provided signature tool only |
| N64ModernRuntime | GPL-3.0-only; evaluated runtime reference, not the project's native bridge |
| RT64 | MIT; renderer |
| Plume | MIT; embedded compatibility patch context carries `patches/plume/LICENSE.upstream` |
| CIC-NUS-6105 algorithm | X-Scale (2011), permissive two-condition notice retained in source and `patches/cic/LICENSE.upstream` |
| Jet Force Gemini decompilation | No root license found; consulted facts-only for libultra identification (see attribution below) |

## Launcher dependencies

| Component | Retained notice |
|---|---|
| RmlUi 6.3 | [MIT](launcher/ui/licenses/RmlUi-MIT.txt) |
| RmlUi's robin_hood and itlib containers | [MIT notices](launcher/ui/licenses/RmlUi-Containers-MIT.txt) |
| RmlUi Debugger's Courier Prime Code fonts | [OFL notice](launcher/ui/licenses/RmlUi-Debugger-OFL.txt); retained for the aggregate target, including assets that the linker may omit |
| FreeType 2.14.1 | [FreeType License](launcher/ui/licenses/FreeType-FTL.txt) and [component notices](launcher/ui/licenses/FreeType-ThirdParty.txt), including BDF/PCF, The Open Group, hashing, zlib and HarfBuzz-derived source |
| SDL 2.26.3 (static, controller input) | [zlib](launcher/ui/licenses/SDL2-zlib.txt) |
| Barlow | [SIL OFL 1.1](launcher/ui/fonts/Barlow-OFL.txt) |
| Chakra Petch | [SIL OFL 1.1](launcher/ui/fonts/ChakraPetch-OFL.txt) |

The launcher package includes these complete notices under `licenses/`, along
with font and notice provenance manifests. The package builder checks that
every license path referenced by `THIRD-PARTY.txt` exists in the archive inputs.
The launcher uses Win32/DX11; optional RmlUi backends and external FreeType
compression/shaping libraries are not enabled by this build configuration.
FreeType's internal zlib and HarfBuzz-derived source notices are still retained.

## Attribution: Jet Force Gemini decompilation (facts-only consultation)

The Jet Force Gemini decompilation project
(https://github.com/Ryan-Myers/Jet-Force-Gemini) was **consulted** during
Phase 6 for the sole purpose of identifying which addresses in our own
generated set correspond to libultra (N64 SDK) functions. Only factual
symbol data — libultra function names (public SDK API) mapped to ROM
addresses — was used, to guide independently authored HLE implementations.
**No decomp source, headers, or implementation was copied.** The libultra
HLE code in this project is original work authored to the public SDK
contract (see `src/boot/PROVENANCE.md`).

The decomp carries no observed license, so this is a maintainer-approved
research consultation of factual metadata, not a grant of copy rights; it
does not affect the underlying game copyright.

RGO (this fork) also consults the decompilation, the same way, to understand
game functions it hooks or replaces (widescreen, frame rate, input, gore).
Only facts are used: function names, ROM/RAM addresses, structure field
offsets and descriptions of behaviour. RGO's hooks and replacements are
independently written; no decomp source, headers or implementation are
copied. The build also clones the decomp at the commit pinned in
`dependencies.lock.json` and builds it locally on the user's machine to
produce a symbol map; neither the decomp nor its output is distributed.

Pinned dependency records and build-tool inventory are in the
[dependency inventory](docs/legal/dependency-license-inventory.md).

## Original device timing

The instruction/cache timing model in `include/jfg/boot/original_timing_cache.h`
and `include/jfg/boot/original_timing_fast.h`, and the cartridge DMA duration
formula in `include/jfg/boot/original_timing_pi_dma.hpp`, are adapted from ares, as vendored in BizHawk revision
`bdddf4a58aa1a022afb11dc73294a81a5aa7bbd5`,
`waterbox/ares64/ares/ares/n64/cpu/` and `n64/pi/dma.cpp`. The copyright and ISC permission
notice are retained in the source, and the complete upstream notice is in
`patches/ares/LICENSE.upstream`.
