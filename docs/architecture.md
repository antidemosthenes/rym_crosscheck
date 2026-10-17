# Architecture

For CLI usage and report interpretation, see the [README](../README.md). Build commands, cache administration, and test execution are documented in the [build](build.md), [runtime](runtime.md), and [testing](testing.md) guides.

`domain` owns song, track, candidate, and snapshot records plus cancellation and Windows handle wrappers. `library` reads tags through a bounded worker queue. `rym` owns browser retrieval and DOM parsing. `matching` owns immutable exact and fuzzy indexes. `storage` owns cache, whitelist, file locks, and atomic writes. `report` owns the versioned JSON comparison model, portable paths, and whitelist update store. `reviewer` hosts the embedded interface in Win32/WebView2. `cli` coordinates these modules.

## Matching guarantees

Normalize once and decode to Unicode code points. Exact identity contains the title, sorted complete artist list, and an explicit recording qualifier. Do not infer collaborations by splitting punctuation in an artist name. A title such as “Live Forever” has no recording qualifier.

For code-point lengths `m,n`, similarity threshold `a/b`, and unit-cost Levenshtein distance `d`, eligibility is `d <= floor((b-a)*max(m,n)/b)`. Candidate lengths lie between `ceil(a*m/b)` and `floor(b*m/a)` when `a > 0`.

For trigrams, count occurrences rather than only distinct grams. Shared occurrence count must be at least `max(max(m-2,0),max(n-2,0)) - 3*k`, where `k` is the allowed distance. Each edit destroys at most three original trigram occurrences. If this bound cannot prune, scan the length-compatible bucket. Zero thresholds and short strings use safe fallbacks.

Threshold decisions and candidate ordering use integer/rational arithmetic. Dense postings select a length-only fallback; this changes execution cost without excluding qualifying candidates. Distance scoring trims equal prefixes/suffixes, reuses thread-local scratch buffers, and rejects candidates once the edit limit is impossible. The exhaustive test path skips retrieval filters, providing an oracle for candidate recall and ranking. An independent dynamic-programming oracle also checks the bounded distance implementation.

When no credited artist qualifies, ordinary title retrieval is skipped; exact-principal credit-mismatch candidates are still evaluated. This avoids unnecessary title-index work without changing eligibility or top-three ordering.

## Ownership and concurrency

Records own their strings and paths. Index keys own their storage; postings contain numeric IDs. Build all indexes before concurrent reads. Views remain within the owner's lifetime and are not stored across asynchronous boundaries or flat-map rehashes.

Boost.JSON parsing resources are local to a command; returned records are copied into independently owned storage before the resource is destroyed. The top-three static vector is never temporarily overfilled.

Local scanning uses bounded queues and joined workers. Album enrichment defaults to four workers and caps the pool at eight or the number of jobs, whichever is smaller. Workers share page-navigation pacing; browser commands are serialized over the CDP socket while page loading can overlap in separate hidden targets. A detected security gate requests a shared stop token, preventing queued paced navigations from continuing. This pool limit does not cap every browser asset request or represent a published server quota.

Headless retrieval targets use `hidden: true, background: true` and never activate a window. A blank bootstrap page initializes Edge before hidden targets are created. Retrieval keeps the browser user agent consistent with ordinary verification so cookies remain usable. Security verification and manual RYM sign-in create visible UI in ordinary Edge, which closes before further retrieval. Both phases use the same persistent profile; cookies remain inside Edge across runs. Chart acquisition retries only the affected URL, allowing at most one security-check handoff and one sign-in handoff per page. Ordinary navigation errors do not trigger a handoff.

Both browser launches redirect standard streams to the Windows null device using an explicit inherited-handle list. Browser diagnostics do not enter the CLI terminal, and unrelated inheritable handles are excluded. Process/thread handles and launch attributes use RAII; suspended launches still join the owning Windows job before execution. Application warnings and browser-connection failures remain visible.

Browser response objects transfer ownership from the parsed CDP reply instead of deep-copying HTML-bearing results. URL/readiness/HTML views stay inside that reply's lifetime; only returned page content becomes a retained owning string. Polling and challenge classification remain unchanged.

The local CDP connection uses asynchronous WinHTTP operations behind a blocking worker-thread interface. Connections reuse one process-wide session, avoiding repeated session setup and the event-handle growth observed with per-connection sessions. Callback contexts own their buffers until the final handle-closing notification, including after cancellation. Session teardown waits for WinHTTP's unload notification before releasing its notification event. Event waits check cancellation and one deadline shared across command send, fragments, and unrelated events; asynchronous traffic cannot restart that deadline. Normal commands allow 15 seconds and cleanup commands 2 seconds. No unsupported receive-timeout option is applied to the upgraded WebSocket handle. The GUI thread never performs these waits.

Non-throwing arithmetic, callbacks, accessors, and resource moves declare `noexcept`; owning records also retain their implicit non-throwing moves. Candidate tie-breaking compares native paths without allocating temporary UTF-8 strings. Allocation, parsing, I/O, synchronization, and cancellable scoring remain throwable. Destructors contain fallible cleanup rather than exposing exceptions across teardown or C callback boundaries.

## Failure and cleanup

Chart acquisition validates rank completeness and song identity before matching. Album enrichment is independent of ownership eligibility. Unresolved local files remain scan warnings rather than silently entering the ownership index.

File locks serialize cache/profile ownership. Writes use sibling temporary files, flushing, and atomic replacement. RAII removes temporary writes and owns handles, sockets, subprocess jobs, and worker lifetimes. Cache pruning touches only application-owned filenames. Forced process termination can leave stale temporary files; cache startup removes its stale temporary writes.

Browser-assisted access is validated against RYM chart and song markup through reduced regression fixtures; upstream changes may require parser updates.

## Portable report store

Both executables resolve the root viewer, `bin/` CLI, and installation-relative `data/` directory. Shared comparison options provide defaults, validation, normalized absolute GUI paths, and shell-free Windows argument quoting. Explicit CLI path arguments retain working-directory-relative interpretation. The CLI creates a fresh generation with every chart song, exact-ownership flags, owning candidate display records, scan context, and a whitelist snapshot. Suppressed rows allow exclusion removal without another scan. Candidate records contain relative paths and scores, never in-memory IDs. Reports use validated schema version 1. No legacy files are moved or adopted automatically.

Report imports and offline snapshots validate their chart source and timestamp range before use. Cached strings retain their full lengths and reject embedded nulls and invalid Unicode rather than silently truncating metadata. An unreadable loaded whitelist cannot form an authoritative replacement; editing is disabled until valid TOML is loaded, while navigation and recovery remain available.

Whitelist entries retain canonical URL or normalized complete-artist/title identities. Additions use song URLs; removals target canonical entry keys. The loaded view plus staged changes forms the authoritative proposed set. Background departure checks compare canonical keys with current disk entries, ignoring comments, order, and formatting. Replace writes exactly the proposed set; Discard preserves disk; Cancel retains editing state. Saves do not merge unseen external edits. A short installation-specific mutex serializes reads, publication, and atomic updates; a separate CLI-owned run mutex rejects overlapping comparisons before cache clearing or writes. GUI updates also respect the run lock.

The whitelist is committed first by atomic replacement. Report refresh follows independently and reads the latest active generation, so an older loaded document cannot replace a newer comparison. Refresh failure reports partial success without rollback. Comparison publication re-reads the whitelist under the store lock. File imports validate on a worker and require explicit replacement; JSON imports receive a new local generation and retain active TOML authority. Sources are copied, never moved. Imported reports do not become offline chart snapshots.

## Native reviewer

The GUI target uses a Win32 STA message loop, WRL COM pointers, and the static WebView2 loader. Only the small compiled UI shell is passed through `NavigateToString`; comparison data uses Boost.JSON and WebView2 messaging, avoiding the shell API’s 2 MiB limit for report data. The initial internal data navigation is allowed; other navigations and new windows are blocked. Metadata is rendered with DOM text nodes. Native actions validate selections and resolve song links against loaded records before opening them externally.

One WebView2 instance hosts one embedded shell. Home, options, Running, Results, and failure screens switch locally without navigation or additional controls. Report parsing and table creation are deferred until Results. The interface uses 100-row pages and independent searches. Bulk actions cover all filtered records; selections use stable URL/entry keys across pages and filters. Validation, difference checks, display-model construction, JSON serialization, and UTF-16 conversion run on a joined worker. The GUI thread owns native dialogs and WebView2 operations and posts prepared messages. Callbacks use weak lifetime references; controller closure and COM release occur during teardown.

The prepared UTF-16 message buffer is released after delivery instead of being retained alongside the native report model and JavaScript data. The native owning model remains available for authoritative saves and link validation. Incomplete-scan warning details defer file-list DOM creation until expanded; their prominent summary remains visible immediately.

Persistent sidebar navigation uses the same native departure guard as the overview actions. Active navigation and keyboard focus follow the current screen, and navigation is disabled during a comparison. The embedded interface uses system fonts, inline icons, static CSS artwork, and short screen transitions with reduced-motion, transparency, contrast, and dark-mode accommodations; no artwork, fonts, or scripts are fetched. Summary counts are computed when a report is accepted, while selection changes continue to update existing rows.

Search text is normalized once per loaded report; filtered rows are cached and search rendering is coalesced to animation frames. Selection and busy-state changes update existing controls without rebuilding tables. The collapsed whitelist defers row creation until expanded. Selection state is sent with actions that need it; no separate dirty-state notifications are required. Native errors carry the current busy state so a notice cannot unlock controls during file I/O. Store revisions recorded under the shared lock prevent reloading the reviewer's own writes while still detecting later comparisons. Minimized windows mark WebView2 invisible and request its lower memory target when supported; restoring the window restores the normal target.

Native confirmations and file/folder pickers share a modal guard. Repeated-launch Results requests are deferred until the modal closes rather than navigating behind it. CLI completion notifications are also deferred during modal interactions.

The report model and search strings remain in memory, so data memory grows with chart and whitelist size even though table rendering is paginated. WebView2 also carries its browser-engine process and memory overhead; the absence of a frontend framework does not eliminate that cost. See Microsoft's [WebView2 performance guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance).

A named instance mutex and installation-specific window class reuse one maximized window. Report-change notices never reset the displayed whitelist or selections. Whitelist files are read on Results entry and departure checks rather than continuously polled. The viewer uses a separate WebView2 profile under LocalAppData, never the verification profile. It does not implement comparison algorithms, contact RYM, run an HTTP server, or require frontend dependencies.

## Hidden CLI supervision

The viewer starts the fixed CLI with `CreateProcessW`, normalized absolute paths, and `--no-open`, without a shell or terminal. An explicit inherited-handle list shares only null stdin, diagnostic output, a versioned JSON-lines progress pipe, and a synchronize-only cancellation event. Machine events remain separate from human logs. A native supervisor thread drains pipes and bounds diagnostic capture to 256 KiB; it coalesces progress to at most four deliveries per second. Elapsed updates run once per second. Process monitoring is independent of store I/O and never waits on the GUI thread.

Progress writes are serialized across acquisition and local-scan completion. Events carry cumulative counters so coalescing does not lose the local count or confuse it with chart ranks. Local completion updates counters without replacing the current stage, preserving verification instructions while the scan finishes in parallel.

The suspended child joins an owned kill-on-close Job Object before execution, preventing orphans if the viewer dies. Cancellation signals the inherited event; the CLI’s joined watcher sets its existing cancellation flag so scanning, pacing, retrieval, and browser cleanup retain their established behavior. Stop and close waits for process completion; Keep running retains the window. Existing browser jobs remain nested beneath the supervised CLI.

Only a validated launcher channel permits security verification or manual sign-in in a hidden run. Redirected input is not treated as an interactive console. The CLI still owns normal persistent Edge access recovery; GUI runs resume when that window closes, while standalone terminal runs also accept Enter. Stages, counts, verification/sign-in status, and terminal events come from the CLI. Atomic report publication is the completion boundary; later cache, settings, or viewer-launch issues cannot turn a completed comparison into a failure.
