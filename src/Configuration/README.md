# Configuration contract

`contracts::Configuration` schema 2 contains semantic settings, not target handles,
tile indexes, cache keys, or renderer output:

| Field | Contract |
| --- | --- |
| `revision` | Host-owned committed revision; mutations supply the expected revision |
| `paused` | Pause decoration |
| `border_width_dip` | Finite 2–64 DIP, default 16 |
| `design_selection` | `plain`, `original`, or `by_app` |
| `design_id` | Exact immutable catalogue theme ID, required only for `original` |
| `stitch_style` | `stockinette`, `rib`, or `garter`; retained plain-stitch preference |
| `material_style` | `plain` or `raised`; default `raised`, retained plain-material preference |
| `stitch_rows` | Finite 1.5–12 rows across the border, default 6; more rows mean smaller stitches |
| `yarn_argb` | Opaque ARGB, default `0xff4e8098` |

All 37 original catalogue IDs are accepted. Unknown IDs, unsupported enum values,
nonfinite/out-of-range floats, translucent yarn, and noncanonical selection/ID
pairs are rejected. No truncation, clamping, or unsupported-value upgrade occurs.
Original charts determine their own stitch/material from authored flags; plain
preferences remain saved for a later return to plain knit.

`ConfigurationServer::SubmitLocal(bool, float, revision)` retains its existing
pause/width API. The serialized host worker merges only those two values into the
current appearance **after** the expected-revision check. The overload
`SubmitLocal(Configuration const&)` submits a complete semantic snapshot.
`Snapshot()` and mutation acknowledgments include every new field.

## Storage and migration

The little-endian schema-2 disk record is exactly 120 bytes:

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
invalid. Reads never allocate according to an external file length.

Valid legacy schema-1/32-byte records migrate **in memory only**, preserving
revision/pause/width and using appearance defaults. Startup and snapshot reads do
not rewrite them. New settings, explicit reset, and legacy migration use raised
stockinette; existing schema-2 records retain their explicit material unchanged.
A successful validated mutation performs the ordinary staged,
flushed atomic replacement; failed writes retain legacy bytes and committed
state. Corrupt, unsupported old values, or newer-schema files remain untouched.
Only explicit reset preserves their exact bytes in a uniquely named backup and
then replaces the live configuration.

## IPC and acknowledgment

Protocol version 3 uses exactly 176 bytes. The previous header/request/client/
server/revision/schema/pause/width offsets are unchanged. Offset 76 now carries
the actual Win32 storage error in responses (zero in requests), and offset 80
retains the 64-bit client lease. Appearance fields occupy offsets 88–108 in the
same semantic order as above; the bounded 64-byte ID occupies offsets 112–175.
Invalid frame size/version/length/values/padding is rejected, not upgraded.

Server-instance identity, explicit client admission, bounded 128 client leases,
request-ID deduplication, and expected-revision checks apply to the **entire**
appearance. Retired leases cannot readmit old mutations. Only successful durable
replacement advances the revision or produces `saved`.

Settings retain the exact displayed configuration/server/lease with each
coalesced request. Only an exact full-configuration acknowledgment extending an
own revision by one can advance a proven own chain. Foreign refreshes never
rebase it. The bounded 32-entry chain history retains active roots across more
than 70 continuous appearance changes.
