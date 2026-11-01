# Journal format, version 1

The journal is a concatenation of independent frames. There is no file-level
header. An empty file is a valid journal containing zero events. All integers
are unsigned and little endian. No C++ object layout, padding, or host byte order
is serialized. The implementation requires IEEE-754 binary64 `double`.

## Frame header

The header is exactly 32 bytes:

| Offset | Bytes | Field | Version 1 requirement |
| --- | --- | --- | --- |
| 0 | 8 | Magic | ASCII `STRATA26` |
| 8 | 2 | Version | `1` |
| 10 | 2 | Header bytes | `32` |
| 12 | 4 | Event count | `1..4096` |
| 16 | 4 | Payload bytes | `event_count * 32` |
| 20 | 4 | Reserved | Zero |
| 24 | 4 | Header CRC32C | CRC of bytes `[0, 24)` |
| 28 | 4 | Payload CRC32C | CRC of the entire payload |

CRC32C uses the Castagnoli polynomial, reflected representation `0x82f63b78`,
an initial register of `0xffffffff`, and final bitwise complement. Check values
are written little endian. The ASCII test vector `123456789` has CRC32C
`0xe3069283`; the empty input has CRC zero. Checksums detect accidental
corruption. They provide no authenticity or protection against deliberate
modification by an adversary who can recompute the checksums.

The header checksum is checked before count-derived allocation. Event count is
bounded before multiplication, and the payload length must match it exactly.
Maximum payload size is 131072 bytes; maximum complete frame size is 131104
bytes. A forged count cannot request an arbitrary allocation.

## Event payload

The payload contains `event_count` consecutive 32-byte records:

| Record offset | Bytes | Field | Requirement |
| --- | --- | --- | --- |
| 0 | 8 | Sequence | Preserved unsigned metadata |
| 8 | 4 | Series | Preserved unsigned metadata |
| 12 | 4 | Reserved | Zero |
| 16 | 8 | Value bits | IEEE-754 binary64, encoded as a little-endian `uint64_t` |
| 24 | 8 | Timestamp, nanoseconds | Preserved unsigned metadata |

Values must be finite and have absolute value at most `1e100`. Negative zero
preserves its bit representation. NaN, infinity, out-of-domain values, and
nonzero reserved fields are rejected even when the payload checksum matches.
The codec does not know the runtime's configured series count; any `uint32_t`
series can be encoded. Admission into an engine applies that engine's series
bound separately.

Sequence and timestamp fields do not impose order, uniqueness, or clock-domain
semantics. Encoding duplicate sequence numbers is valid. Replaying the same
frame twice produces its observations twice.

## Public codec contract

`encode_frame(span<const event>)` returns an owned byte vector or an error. It
requires 1..4096 observations and validates all values before encoding. It does
not borrow the caller's data after returning.

`decode_frame(span<const byte>)` returns an owned event vector and `consumed`, the
size of the first frame. Trailing bytes are allowed so a caller can parse a
larger buffer. It never retains the input span. A short header or payload returns
`truncated`; invalid structure or checksums return `corrupt`; a full header with
a valid checksum and an unknown version returns `unsupported`. Malformed
records return `corrupt`. Allocation failures propagate as exceptions.

These byte-span entry points are independent of filesystem I/O, which makes
boundary tests and fuzzing possible without constructing files.

## Writer contract

`journal_writer::open(path)` creates or truncates the target. It does not resume
an existing journal. A writer is noncopyable and nonmovable, and callers must
provide single-threaded ownership or external synchronization.

`append(events)` accepts any span length, emits frames of at most 4096 records,
and treats an empty span as a no-op. It validates the entire input before writing
any frame, so a validation error leaves that call's output unwritten. An I/O
failure can leave complete earlier frames and an incomplete final frame. The
call has no transactional rollback.

Call `flush()` explicitly to observe buffered stream errors. The destructor
closes the stream but cannot report a closing error. Flush does not call `fsync`,
`fdatasync`, or a platform durability equivalent. The format and implementation
make no power-loss, torn-sector, or durable-acknowledgement guarantee.

## Replay and prefix recovery

`replay(path, callback)` reads one header at a time, validates its bounded
lengths, reads one payload, checks and decodes it, and invokes the callback
synchronously. At most one frame is decoded for each callback. The borrowed
`span<const event>` remains valid only for that callback invocation. A callback
must copy observations it needs to retain.

Recovery behavior is precise:

| Condition | Result |
| --- | --- |
| EOF exactly between frames | Success, `truncated_tail = false` |
| EOF after part of the next header | Earlier complete frames returned; `truncated_tail = true` |
| Valid complete header followed by a short final payload | Earlier complete frames returned; `truncated_tail = true` |
| Complete malformed header | Error; no attempt to resynchronize |
| Complete payload with invalid checksum or records | Error; that frame is not delivered |
| Unknown version in a valid complete header | `unsupported` error |
| Non-EOF read failure | `io_error` |
| Empty callback | `invalid_config` |
| Callback error | Propagated immediately; no later frames delivered |

The successful result counts only frames and events delivered to successful
callbacks. Complete earlier callbacks may already have changed application
state if a later frame or callback fails; replay cannot roll them back. Error
offsets are relative to the file. For example, a payload-checksum mismatch points
to that frame's checksum field at `frame_start + 28`.

An incomplete final header is ignored regardless of its contents. Thus prefix
recovery cannot distinguish an interrupted write from arbitrary garbage shorter
than a full header. It neither repairs nor truncates the input file and does not
scan for the next magic marker after corruption. Applications needing durable
append, authenticated input, transactional restore, or recovery beyond the first
bad frame need a stronger storage layer.
