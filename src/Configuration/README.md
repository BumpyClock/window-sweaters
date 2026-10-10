# Configuration contract

`contracts::Configuration` schema 4 contains semantic settings, not target handles,
tile indexes, cache keys, or renderer output:

| Field | Contract |
| --- | --- |
| `revision` | Host-owned committed revision; mutations supply the expected revision |
| `paused` | Pause decoration |
| `border_width_dip` | Finite 2–64 DIP, default 16 |
| `design_selection` | `plain`, `original`, `by_app`, or `shared_zigzag` |
| `design_id` | Exact immutable catalogue theme ID, required only for `original` |
| `stitch_style` | `stockinette`, `rib`, or `garter`; retained plain-stitch preference |
| `material_style` | `plain` or `raised`; default `raised`, retained plain-material preference |
| `stitch_rows` | Finite 1.5–12 rows across the border, default 6; more rows mean smaller stitches |
| `yarn_argb` | Opaque ARGB, default `0xff4e8098` |
| `eligibility` | Default-on plus explicit `AppIdentity`-keyed enable/disable exceptions |

Every catalogue ID (pinned originals and Windows-authored designs) is accepted. Unknown IDs, unsupported enum values,
nonfinite/out-of-range floats, translucent yarn, and noncanonical selection/ID
pairs are rejected. No truncation, clamping, or unsupported-value upgrade occurs.
Original charts determine their own stitch/material from authored flags; plain
preferences remain saved for a later return to plain knit.

`ConfigurationServer::SubmitLocal(bool, float, revision)` retains its existing
pause/width API. The serialized host worker merges only those two values into the
current appearance **after** the expected-revision check. The overload
`SubmitLocal(Configuration const&)` submits the appearance fields and preserves
committed eligibility. `SubmitEligibility(mutation, expected_revision)` changes
only application preferences. Neither operation overwrites the other feature.
`Snapshot()` and mutation acknowledgments include every new field.

## Storage and migration

Schema 4 is length-framed: 12-byte little-endian magic/schema/total-length header,
a bounded protocol-5 configuration record, then CRC-32 of all preceding bytes.
The file is limited to 1 MiB plus its 16-byte envelope. The configuration record
contains appearance plus a tagged eligibility section; no snapshots, PNGs,
target handles or process identities are persisted.

The read-only legacy schema-2 disk record is exactly 120 bytes:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | Magic `0x31435357` |
| 4 | 4 | Schema 2 |
| 8 | 8 | Revision |
| 16 | 4 | Pause, exactly 0 or 1 |
| 20 | 4 | Width float bits |
| 24, 28, 32 | 4 each | Design selection, stitch, material |
| 36, 40, 44 | 4 each | Stitch rows float bits, ARGB, ID byte length |
| 48 | 64 | ID bytes and zero padding |
| 112 | 4 | Reserved zero |
| 116 | 4 | CRC-32 of the preceding 116 bytes |

IDs are at most 63 ASCII bytes (`a`–`z`, `0`–`9`, `-`), a bounded UTF-8 subset
containing the exact authored semantic IDs. Embedded NULs and nonzero padding are
invalid. Reads validate the fixed header and aggregate file limit before allocation.

Valid legacy schema-1/32-byte, schema-2/120-byte, and framed schema-3 records
migrate **in memory only**. Startup and snapshot reads do not rewrite them.
Schema 1 acquires raised stockinette. Schema 2 retains every appearance field;
both acquire default-on eligibility with no exceptions. Schema 3 retains its
appearance and application exceptions. Its supported selection values remain
0–2; a legacy record cannot introduce the new value 3 by changing enum bytes.
A successful validated mutation performs the ordinary staged,
flushed atomic replacement; failed writes retain legacy bytes and committed
state. Corrupt, unsupported old values, or newer-schema files remain untouched.
Only explicit reset preserves their exact bytes in a uniquely named backup and
then replaces the live configuration.
Bounded atomic-replacement retries include `ERROR_UNABLE_TO_REMOVE_REPLACED`
(1175), which leaves both the original and staged files unchanged. This handles
transient filesystem readers without rewriting user intent, acknowledging a
failed save or weakening permanent failed-write retention.

## IPC and acknowledgment

Protocol version 5 retains the first 176 appearance bytes and its typed,
length-framed sections. The byte-mode pipe reads a 12-byte header, validates the
total bound, then reads exactly that frame. The previous header/request/client/
server/revision/schema/pause/width offsets are unchanged. Offset 76 now carries
the actual Win32 storage error in responses (zero in requests), and offset 80
retains the 64-bit client lease. Appearance fields occupy offsets 88–108 in the
same semantic order as above; the bounded 64-byte ID occupies offsets 112–175.
Tag 1 contains eligibility preferences; tag 2 an eligibility mutation; tag 3
an application snapshot; tag 4 the acknowledged operation. Required tags are
operation-specific. Unknown/duplicate tags, unsupported operations, trailing
data, invalid identities, duplicate edits, bad counts and unchecked lengths
are rejected. The 1 MiB aggregate limit applies before payload allocation.
The live endpoint accepts protocol 5 only. Protocol-4/schema-3 decoding exists
solely for validated disk migration, not an alternate live mutation path.
The existing three-second I/O budget covers the complete header/body read;
partial byte progress does not restart the deadline.
Pending mutations retain the existing 32-command limit and an additional
4 MiB aggregate serialized charge; saturation returns `busy` without saving.

Identity keys are validated UTF-16: at most 32767 units for normalized absolute
executable paths or the supported packaged AUMID limit. Preferences have at most
4096 exceptions and 256 KiB aggregate serialized bytes. Host running metadata
retention is separately limited to 256 KiB; excess becomes a non-mutable
resource-limit diagnostic. Snapshots contain at most 8192 rows and 4096 running
windows, with counts, labels, supported-property provenance and failure facts.
They contain no pixel blobs. Publisher authenticity is not claimed.

Only snapshot, appearance mutation, explicit recovery reset, eligibility mutation,
and application snapshot operations exist. All-on/off atomically changes the
default and clears exceptions; default-only changes retain explicit choices.
Snapshots include disabled running owners and saved non-running choices.
Observation generation advances for metadata/lifecycle/count changes even when
the configuration revision does not change.
`EligibilityRevision(identity)` is an internal Host fence, not another IPC
operation or persisted setting. It advances after each durable effective
decision change for an observed owner. Runtime pins contain only the bounded
currently running identity set, so coalesced Host snapshots cannot lose a
disable/re-enable event or invalidate unrelated render work.

Server-instance identity, explicit client admission, bounded 128 client leases,
request-ID deduplication, and expected-revision checks apply to the **entire**
configuration. Retired leases cannot readmit old mutations. Only successful durable
replacement advances the revision or produces `saved`.
Dedup compares the exact canonical framed request, including identity-key spelling.
Its retained requests/results are bounded to 256 records and 4 MiB aggregate
serialized charge. Earlier byte-budget eviction still uses lease high-water
marks: an evicted successful request cannot be applied again.

Settings retain the exact displayed configuration/server/lease with each
coalesced request. Only an exact full-configuration acknowledgment extending an
own revision by one can advance a proven own chain. Foreign refreshes never
rebase it. The bounded 32-entry chain history retains active roots across more
than 70 continuous appearance changes.
