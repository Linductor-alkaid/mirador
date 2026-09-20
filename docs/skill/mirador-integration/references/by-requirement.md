# Index By Requirement

| Requirement or concern | Read |
| --- | --- |
| No exceptions; every failure visible as a code | [Quick start](quick-start.md) |
| Cancel or time out a long operation without hangs | [Session and caching](session-and-caching.md) and [backends](backends.md) |
| Do nothing when the screen is unchanged | [Session and caching](session-and-caching.md) |
| Ignore a clock, cursor, or video overlay as non-change | [Session and caching](session-and-caching.md) |
| Invalidate results when the model or weights change | [Backends](backends.md) |
| Share one backend across threads, or keep calls serialized | [Backends](backends.md) |
| Keep one source's state out of another's | [Session and caching](session-and-caching.md) |
| Keep a button's identity across small layout shifts | [Fusion and snapshots](fusion-and-snapshots.md) |
| Refuse an action on a region that has since changed | [Fusion and snapshots](fusion-and-snapshots.md) and [set-of-mark](set-of-mark.md) |
| Mix frame, rotated, and display coordinates safely | [Frames and coordinates](frames-and-coordinates.md) and [fusion and snapshots](fusion-and-snapshots.md) |
| Every budget overrun visible, nothing silently dropped | [Session and caching](session-and-caching.md) and [geometry](geometry.md) |
| Explain why two evidence boxes were merged | [Fusion and snapshots](fusion-and-snapshots.md) |
| Results must not leak pixels, text, or network traffic | [Quick start](quick-start.md) (in-memory only; logging is the caller's) |
| A downstream AI needs to see this skill | [Adoption](adoption.md) |

Choose the smallest capability that satisfies the requirement. A cache hit is not proof the screen is unchanged, similarity is not identity, and proximity alone never merges evidence.
