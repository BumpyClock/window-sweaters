# Native personal imports

This bounded, declarative module supplies the personal-design feature in Settings.
The host's configuration worker validates candidates and commits their owned
contents; this module does not open Settings, execute scripts, write configuration,
elevate, or retain unchecked external references. The immutable `Designs`
catalogue is read-only.

## Provenance and compatibility

The reviewed local reference is Window Sweaters
`a49dff52483dafdbe27f972bd271cd28d37cef1c`, from
<https://github.com/saragordic/window-sweaters>. Relevant source:

- `src/apps.c:103-209` and `src/misc/apps.h`: six hexadecimal RGB digits,
  optional chart/comment, 64 personal rules, longest case-insensitive prefix,
  first source-order rule for equal-length ties, personal rules before authored
  rules. A prefix is source metadata, not a persistent Windows app identity.
- `src/chart.c:283-392` and `src/misc/chart.h`: top-down chart cells, axes 1..256,
  custom PNGs, retained corner/cuff/dot/fitting/yarn flags on known replacements,
  128 effective charts. **128 is a chart-count limit, not a palette limit.**
- `src/knit.c:347-361`: raised yarn has sixteen color path groups; alpha below
  128 selects base yarn, otherwise literal ARGB is made opaque.

GNU GPL v3 applies; see the repository `LICENSE` and `NOTICE.md`, including
the original Window Sweaters/JankyBorders attribution.

Deliberate import-boundary restrictions:

- Invalid noncomment rules reject the entire document with a line diagnostic,
  rather than upstream's partial import that skips invalid lines.
- UTF-8 prefixes use Windows ordinal case-insensitive comparison. ASCII
  behavior agrees with the original; Unicode case matching is a Windows
  extension. Names/prefixes retain their original spelling.
- Semantic chart names are 1..63 UTF-8 token bytes, not paths; reserved
  `none`/`by-app`, whitespace, controls, NUL, and `/\:#=` are rejected. Optional
  chart references must exist when a catalogue candidate is built.
- At most sixteen distinct normalized yarn slots are accepted. Zero/base yarn
  consumes a slot if present. There is **no sixteen-row height restriction**:
  256x256 charts are accepted. Excess yarns are rejected explicitly, never
  dropped, quantized, recolored, or cropped. This avoids the upstream renderer's
  silent seventeenth-color omission.
- WIC converts to **straight** RGBA without dithering, palette reduction,
  premultiplication, or a color-profile transform. Alpha 0..127 becomes zero;
  alpha 128..255 retains literal RGB and becomes opaque. This preserves user
  RGB rather than retaining CoreGraphics' intermediate premultiplied channels.
  Sixteen-bit channels are rejected because they cannot be preserved in the
  eight-bit yarn representation. Valid low-bit grayscale/indexed PNGs and
  Adam7 are supported; animated PNGs are rejected.

## Limits and checked errors

`DecodePng` accepts owned input of at most 2 MiB. Before WIC allocation it
checks signature, chunk extents/CRCs/order, header fields, axis bounds, palette,
transparency, critical chunks, and complete IEND with no trailing content.
WIC receives only the validated image chunks (IHDR/PLTE/IDAT/IEND/tRNS).
Other ancillary metadata is retained in the owned original blob but is not
decoded or semantically interpreted. This blocks unbounded compressed-text,
profile, or EXIF work inside metadata handlers and makes literal-color policy
independent of embedded profiles.
Bounded RFC1950/1951 validation checks stored/fixed/dynamic DEFLATE blocks,
backreferences, exact noninterlaced/Adam7 scanline sizes, filters, and Adler-32.
This extra validation is necessary: local tests measured WIC reporting success
for corrupted compressed bytes with recomputed PNG CRCs. The validator does
not replace WIC image decoding or color conversion.

Maximum decoded cells: 65,536; RGBA/cell buffers: 256 KiB each. DEFLATE output
is bounded by the declared scanline layout before growth. Indexed pixels are
also checked against the actual encoded palette before conversion, so an
undefined index cannot silently become a different color.

`ImportPngFile` accepts canonical absolute drive paths on fixed/removable local
disks with a direct local hard-disk volume mapping only. UNC/device paths,
mapped network drives, SUBST/DOS-device indirection, ADS, traversal, DOS devices,
invalid UTF-16, directories, offline/recalled files, and reparse traversal are
rejected. Each ancestor is opened without following reparse points and held
without write/delete sharing through the read. The ordinary final file is
opened read-only, sized before allocation, bounded-read to EOF, and checked
before/after using volume/file ID and size/write/change metadata. Final handle
location must remain a local drive. Local hard links are ordinary files;
their observed file identity is distinct from imported content identity.

Catalogue candidates are limited to 128 effective charts including built-ins,
and 16 MiB total encoded PNG plus normalized cells. Rules are bounded by
64 KiB, 64 entries, and the original 511-byte physical line buffer. Verified
metadata snapshots are bounded by 4,096 entries/4 MiB retained data, and the
entire resolution output by 4 MiB, including repeated ambiguous identities.

Failures return `ErrorCode`, a static reason, optional rule line, and native
Win32/HRESULT/NTSTATUS evidence. Allocation failure is explicitly reported;
there is no broad exception catch or success fallback in production imports.

## Exact host integration contract

1. Obtain verified `applications::AppMetadata` from the host's metadata worker.
   Call `ParseRules` and `PlanRuleImport` explicitly. Keep every unmatched or
   ambiguous `RuleResolution` visible to settings; do not silently guess an
   identity from a prefix, display name, window title, PID, or a basename shared
   by different executable locations.
2. The plan yields at most one `BoundRule` per verified `AppIdentity`, using
   longest original prefix and first source-order ties. An ambiguous winner
   yields no binding. Translate only these bound values into the existing
   `colorways::PersonalRule`: exact typed application identity, `base_argb`
   override, no synthesized contrast. Persist the bound identity independently
   from the original declarative prefix and unresolved diagnostics.
3. In By App mode, omitted optional chart means `PlainPattern`, **not** a missing
   override that falls through to an authored/icon pattern. A chart name that
   shadows a built-in resolves to the imported asset first; otherwise use its
   immutable authored semantic name. Translate a personal asset into
   `PersonalChart{Id(), ContentRevision()}` only after candidate validation. Keep the
   existing resolver's global explicit-pattern override and personal-color
   precedence; user PNG contrast cells must remain literal.
4. Import a selected local PNG with `ImportPngFile`, or revalidate a bounded
   host-owned content blob with `DecodePng`. Its `LocalFileIdentity` is read
   evidence only. Use `BuildCandidate(committed, AddChart/ReplaceChart/
   RemoveChart/SetRules)`; no call changes `committed`. Duplicate custom names
   require explicit replacement; replacing an immutable known name creates
   an imported shadow and retains source material. Removing a shadow restores
   the original built-in; removing a referenced nonbuilt-in fails validation.
5. New `Id()` is `imported:sha256:<initial-normalized-content-and-material-hash>`, independent
   of path, filename, process, and menu order. Replacement retains that ID.
   `Revision()` is `sha256:<current-normalized-cells+dimensions+material-hash>`;
   same-content retries and presentation renames do not change it. A rename
   cannot bypass known-source material retention. No color is rewritten when
   a source chart's flags/corner/cuff are retained.
   `ContentRevision()` is the native resolver/renderer's persistent `uint64_t`
   token, allocated from **one catalogue-global high-water mark**. Every new
   asset and changed-content replacement receives the next value, not a new
   per-ID counter. Same-content retries/renames retain their existing value.
   Removing any or all assets retains the high-water mark. Thus A(ID,1) → B(ID,2)
   → remove → reimport A(ID,3) → C(ID,4) cannot reuse B's consumer cache key,
   even when restart occurs between remove and reimport. There is no per-ID
   tombstone history: the retained state is one eight-byte scalar. Overflow
   rejects additions/changed-content replacements without wrapping; removal
   and no-content-change edits remain valid.
6. Serialize candidates through the sole-writer host's versioned mutation
   contract with expected configuration revision and request deduplication.
   `StoreCatalogue(candidate)` returns the **complete owned `StoredCatalogue`**:
   original parsed rules, live chart records (ID, semantic name, normalized
   hash revision, numeric content revision, material, validated encoded blob),
   and the required `revision_high_water`. Persist this entire snapshot
   atomically, **including the high-water mark when there are no live charts**.
   Never persist the source path/file ID as the only asset reference.
   `RestoreCatalogue(stored)` requires that complete snapshot; the old
   chart-list-only restore API has been removed because current assets cannot
   reconstruct removed revision history. Missing high-water data is an error,
   not a reason to infer it or default to zero. Restore checks that each live
   revision is nonzero, unique, and at most the stored high-water mark; it also
   redecodes bounded blobs and verifies hashes/material/name uniqueness without
   file-system reads. The host must not omit, roll back, reset, or infer the
   allocator state from the surviving chart list. Zero is valid only for a
   genuinely new catalogue; corrupt/older storage needs explicit recovery
   outside this module, not automatic fallback to a fresh `ImportCatalogue`.
   The host must also persist exact bound identities and reconstruct rule
   diagnostics, rather than opportunistically rebinding on restart.
7. Only after durable storage succeeds should the host swap its committed
   catalogue, acknowledge the saved configuration revision, publish snapshots,
   and invalidate tiles using the asset ID/content revision. The new renderer
   can copy `Width/Height/Cells/SourceMaterial` into its owned chart adapter.
   Its numeric revision is `ContentRevision()`, persisted alongside the full
   normalized hash; do not substitute a menu index or truncate the hash.
   Only the serialized, durably accepted candidate may supply cache keys:
   competing candidates from one committed base are not separate committed
   histories. A failed candidate does not consume the committed high-water mark.
   Reject stale host/metadata generations and failed writes using the existing
   owner contract; retain the previous committed catalogue and rendered chart.

The configuration layer supplies schema-5/protocol-6 serialization, typed
mutations and immutable owned snapshots. Native Settings uses those operations
for rule text, PNG add/select/replace/remove, and explicit application colors.
The renderer consumes copied literal chart cells and persisted content revisions.
Startup behavior remains separate.
Consumers link the SDK's `windowscodecs.lib`, `ole32.lib`, and `bcrypt.lib`,
and the project-reference dependencies `Applications` and `Designs`.

## Focused verification

Visual Studio 2026 v145, C++20, Windows SDK 10.0.26100.0, Windows 11 x64:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' tests\Imports\WindowSweaters.Imports.Tests.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
.\out\x64\Debug\WindowSweaters.Imports.Tests.exe
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' tests\Imports\WindowSweaters.Imports.Tests.vcxproj /m /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
.\out\x64\Release\WindowSweaters.Imports.Tests.exe
```

Run executables from the repository root. Tests generate PNGs locally with
WIC, and use only `tests\Imports\outputs` for disk fixtures, removing their
files and local junction afterward. No third-party dependency, UI launch,
application mutation, elevation, network access, or user-settings write.

Coverage: invalid/truncated/oversized PNGs; CRC/zlib/DEFLATE/Adler/filter errors;
palette indices and yarn limits; alpha thresholds; 1..256 axes and overflow;
all DEFLATE block types and Adam7; rule limits/invalid lines/Unicode/case/ties;
verified packaged/traditional identities, ambiguity, unmatched/missing alias;
duplicate names/IDs; stable content/replacement revisions; exact remove/reimport
sequence and owned-snapshot restore between removal/reimport; production
consumer key uniqueness and old-B/new-C cache lookup; global revision allocation,
missing/reset/behind high-water rejection, overflow, and failed-candidate
preservation; source-material
retention; failed candidate preservation; owned-blob rehydration; effective
catalogue/aggregate/resolution budgets; missing files, directories, ADS,
traversal, UNC/device paths, active writers, hard links, and local junctions.
Deterministic production `Colorways` resolver fixtures establish longest-rule
translation, personal color over automatic/manual proposals, explicit
omitted-chart plain, global-pattern override, preserved PNG color policy,
and the exact stable-ID/numeric-revision adapter. They do not execute an icon
worker or mutate any application.

The module checks establish native codec, local-file, and pure-model behavior.
Separate configuration/Session tests cover authenticated mutations, atomic saves,
restart, acknowledged edit chaining, and persistent revision history. Hidden
Host checks cover target/asset invalidation and icon/manual/personal precedence.
Actual Settings interaction and visual import verification remain separate gates
in `docs\parity.md`.

Measured on Windows x64 build `10.0.26300.0`, Visual Studio v145/MSVC
`14.51.36231`, SDK `10.0.26100.0`: focused Debug and Release builds completed
with warnings treated as errors; after the durable revision correction both
executables passed **1,618 checks**. Before the correction, the new regression
failed on the old remove/reimport cache-key collision.
The isolated output directory was empty after both runs. No user
configuration or desktop applications were changed.
