# Architecture

`domain` owns song, track, candidate, and snapshot records plus cancellation and Windows handle wrappers. `library` reads tags through a bounded worker queue. `rym` owns browser/HTTP transports and DOM parsing. `matching` owns immutable exact and fuzzy indexes. `storage` owns cache, whitelist, file locks, and atomic writes. `report` renders Markdown. `cli` coordinates these modules.

## Matching guarantees

Normalize once and decode to Unicode code points. Exact identity contains the title, sorted complete artist list, and an explicit recording qualifier. Do not infer collaborations by splitting punctuation in an artist name. A title such as “Live Forever” has no recording qualifier.

For code-point lengths `m,n`, similarity threshold `a/b`, and unit-cost Levenshtein distance `d`, eligibility is `d <= floor((b-a)*max(m,n)/b)`. Candidate lengths lie between `ceil(a*m/b)` and `floor(b*m/a)` when `a > 0`.

For trigrams, count occurrences rather than only distinct grams. Shared occurrence count must be at least `max(max(m-2,0),max(n-2,0)) - 3*k`, where `k` is the allowed distance. Each edit destroys at most three original trigram occurrences. If this bound cannot prune, scan the length-compatible bucket. Zero thresholds and short strings use safe fallbacks.

Threshold decisions and candidate ordering use integer/rational arithmetic. Dense postings select a length-only fallback; this changes execution cost without excluding qualifying candidates. Distance scoring trims equal prefixes/suffixes, reuses thread-local scratch buffers, and rejects candidates once the edit limit is impossible. The exhaustive test path skips retrieval filters, providing an oracle for candidate recall and ranking. An independent dynamic-programming oracle also checks the bounded distance implementation.

## Ownership and concurrency

Records own their strings and paths. Index keys own their storage; postings contain numeric IDs. Build all indexes before concurrent reads. Views remain within the owner's lifetime and are not stored across asynchronous boundaries or flat-map rehashes.

Boost.JSON parsing resources are local to a command; returned records are copied into independently owned storage before the resource is destroyed. The top-three static vector is never temporarily overfilled.

Local scanning uses bounded queues and joined workers. Web workers share request pacing; browser commands are serialized over the CDP socket while page loading can overlap in separate hidden targets. Headless retrieval targets use `hidden: true, background: true` and never activate a window. A blank bootstrap page initializes Edge before hidden targets are created. Retrieval keeps the browser user agent consistent with ordinary verification so cookies remain usable. Verification alone creates visible UI in ordinary Edge, which closes before further retrieval. The optional private mode owns a fresh temporary profile for both phases and removes it after all browser workers finish. Browser cookies are never copied into the HTTP client.

Non-throwing arithmetic, callbacks, accessors, and resource moves declare `noexcept`; owning records also retain their implicit non-throwing moves. Candidate tie-breaking compares native paths without allocating temporary UTF-8 strings. Allocation, parsing, I/O, synchronization, and cancellable scoring remain throwable. Destructors contain fallible cleanup rather than exposing exceptions across teardown or C callback boundaries.

## Failure and cleanup

Chart rank gaps, conflicting records, unsupported markup, and access failures stop the comparison. Album failures are warnings and result in `Unknown`. Unresolved local files are listed prominently and produce exit code 2.

File locks serialize cache/profile ownership. Writes use sibling temporary files, flushing, and atomic replacement. RAII removes temporary writes and owns handles, sockets, subprocess jobs, and worker lifetimes. Cache pruning touches only application-owned filenames. Forced process termination can leave stale temporary files; cache startup removes its stale temporary writes.

Browser-assisted access is validated against RYM chart and song markup through reduced regression fixtures; upstream changes may require parser updates.
