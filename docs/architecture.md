# Architecture

[README](../README.md) explains program use. [Runtime](runtime.md) explains storage and access recovery.

## Subsystems

Headers are under `include/rym_crosscheck/`. Sources follow the same groups under `src/`.

| Group | Responsibility |
| --- | --- |
| `domain` | Own records, cancellation state, and Windows resources. |
| `library` | Scan files, read tags, and manage the worker queue. |
| `matching` | Normalize metadata, build indexes, and rank candidates. |
| `rym` | Manage Edge, access recovery, chart parsing, and album retrieval. |
| `storage` | Manage caches, whitelist entries, locks, and atomic writes. |
| `report` | Manage JSON reports, installation paths, imports, and saves. |
| `cli` | Check options, run comparisons, and communicate with the reviewer. |
| `reviewer` | Host the embedded interface in Win32 and WebView2. |

Boost supplies flat maps, flat sets, JSON, and candidate storage. The project uses C++23.

## Matching terms

| Term | Meaning |
| --- | --- |
| Identity | Normalized title, complete artists, recording version, and included sections. |
| Principal artist | The first artist in a recorded artist list, before sorting. |
| Base title | A comparison copy after removal of balanced annotations. |
| Component | One complete part of a title with slash separators. |
| Review spelling | A comparison copy after removal of Latin diacritics. |
| Candidate | A local recording that qualifies as a review suggestion. |

## Matching algorithm

```text
metadata → shared normalization → identity
                                  ├─ exact hash lookup → Owned
                                  └─ candidate indexes → eligibility → scoring → best three
```

Exact ownership and review matching share one identity builder. `identity_key` omits review preparation.
`identity` also prepares full credits and annotation evidence. Both functions produce the same exact key.
Artist/title whitelist entries use this key.

Exact ownership requires identity equality. Ordinary fuzzy matching requires qualifying artist and title scores.
Review exceptions admit specific differences. Review exceptions never establish ownership.

### Normalization and exact keys

| Field | Common normalization | Additional handling |
| --- | --- | --- |
| Title | Unicode, case, whitespace, typographic punctuation. | Normalize outer quotes, slash spacing, and explicit remasters. |
| Artists | Unicode, case, whitespace, typographic punctuation. | Keep complete names. Sort structured credits. Remove duplicate identical names. |

Artists with punctuation remain complete names. Quote removal keeps title words and internal apostrophes.
Accents remain significant in exact keys. Albums have no role in ownership or candidate eligibility.

Recognized trailing edition labels remain in the version field. Examples include single, album, extended, and radio edit.
Recognized recording labels include live, remix, acoustic, instrumental, and demo. Ordinary words such as “Live” do not establish a recording label.

Included-section parsing separates names from the scoring title. Exact keys retain the ordered section names.
Equivalent quotes and list separators can match exactly. Missing or different section names require review.
Malformed section lists remain title text.

Length-prefixed key fields prevent delimiter ambiguity. Duplicate identities share one candidate.
Each duplicate group keeps every recorded principal and annotation source. The smallest native path selects the representative recording.

### Review exceptions

| Evidence | Required condition |
| --- | --- |
| Credit mismatch | Equal recorded principal artists and qualifying title similarity. Complete credits must differ. |
| Artist overlap | Equal base titles and a contained artist phrase with two meaningful words. |
| Edition or section difference | Eligible artists and qualifying base-title similarity. Protected recording evidence must remain compatible. |
| Annotation difference | Equal complete, nonempty bases after balanced block removal. Artists must remain eligible. |
| Component difference | One complete title equals a complete slash component. Artists must qualify or share a recorded principal. |
| Diacritic difference | Equal complete canonical titles or equal complete credits after Latin accent removal. Recording checks remain active. |

Annotation extraction removes balanced `(...)` and `[...]` blocks from a separate copy.
Nested blocks and unknown labels are valid. Malformed pairs invalidate the entire extraction.
Empty bases cannot qualify. Original metadata remains available.

The component parser handles canonical titles and extracted bases. Annotation removal precedes segmentation of the extracted form.
Outer component quotes keep their words. Components require at least three code points and one letter.

Empty components and unbalanced brackets invalidate the split. Slashes inside parentheses do not split the canonical title.
Internal substrings alone cannot qualify.

Equal complete extracted bases can expose incompatible recording labels for review. Component equality alone cannot override those recording checks.

Artist overlap is symmetric. Leading English articles do not count in this path.
Space and slash boundaries prevent partial-word matches. Word pairs do not span slashes.
At least two meaningful words must occur within one slash component. This rule does not infer arbitrary artist aliases.

Latin spelling uses canonical decomposition through utf8proc. Accented Latin letters become ASCII bases.
The spelling also removes combining diacritics after Latin letters. Other scripts and variation selectors stay intact.
Letters without this decomposition stay intact.

Complete credits undergo spelling conversion separately for each artist. The matcher sorts the converted list again.
Names that collide after conversion remain separate entries. No accent conversion changes ownership or whitelist identities.

### Candidate retrieval

The matcher qualifies full artist credits first. Latin credit equality can also admit an artist for review.
A small eligible pool uses the corresponding track IDs. The current limit is one quarter of the library.
Larger pools use the global title index. Both paths preserve eligible candidates.

The global indexes use code-point lengths and trigram occurrence counts. A trigram contains three consecutive code points.
For threshold `a/b > 0`, lengths satisfy:

```text
ceil(a*m/b) <= n <= floor(b*m/a)
k = floor((b-a) * max(m,n) / b)
```

Here `m` and `n` are the two lengths. The required shared trigram count is:

```text
max(max(m-2,0), max(n-2,0)) - 3*k
```

An edit can destroy at most three trigrams. Short strings, dense postings, and nonpositive overlap bounds use length-compatible scans.
Threshold zero has no finite upper length bound.

Separate sparse indexes store annotation bases, title components, artist word pairs, and changed Latin spellings.
Component postings retain full-form lengths and annotation flags. Reverse queries use existing canonical indexes.
The matcher prepares query forms once. Candidate scoring does not parse local components again.

Component parsing uses inline Boost storage for the usual two components. Bracket stacks also use inline storage.
Both containers grow when necessary. Inline capacity does not limit valid components or nesting.

### Scoring and ranking

Ordinary similarity uses unit-cost Levenshtein distance:

```text
similarity = 1 - distance / max(m,n)
```

Integer fractions preserve threshold boundaries. Distance scoring removes shared prefixes and suffixes first.
It rejects impossible lengths and stops when the edit limit fails. Worker-local row buffers avoid repeated allocation.

Equal complete extracted bases score 100%. Complete component relations use the shorter length divided by the longer length.
This ratio compares the selected complete forms, including extra components. These paths require no distance matrix.

Diacritic candidates keep actual normalized-text scores. An upper edit bound counts unequal aligned characters and the length difference.
A bounded distance pass then computes the exact score. Long strings with a few accents avoid a full-width pass.

Candidate retrieval has no count limit before scoring. Ranking prefers candidates without these flags, in this order:

1. Artist overlap.
2. Credit mismatch.
3. Diacritic difference.
4. Annotation difference.
5. Version difference.
6. Section difference.
7. Component difference.

Ties use mean artist/title similarity, artist similarity, title, and native path.
Native x64 wide products compare rational means without overflow.

The result holds three candidates in `boost::container::static_vector<Candidate, 3>`.
A length bound can reject candidates that cannot improve the current result.
A binary search can tighten the distance limit. These checks include flags, scores, and deterministic ties.
They do not change eligibility or final scores.

Exhaustive scoring omits retrieval and ranking shortcuts. It checks local spellings and components independently.
A separate dynamic-programming oracle checks edit distances. The two checks test retrieval completeness and scoring correctness.

## Ownership and concurrency

Records own strings and paths. Indexes own keys and store numeric IDs.
Index construction finishes before parallel reads. Views stay within their owners' lifetimes.
Views do not point into flat-map elements across rehashes.

Full-credit text and code points exist once per distinct credit. Exact keys move into the hash map.
Ordinary records omit sparse review data. The source track vector must outlive its index.

Local tag readers use a bounded queue and joined workers. Scanning overlaps chart retrieval.
Album workers share pacing and cancellation. A restriction stops queued album requests.

Resource Acquisition Is Initialization (RAII) wrappers and smart pointers own handles, files, sockets, processes, and buffers.
Nonthrowing accessors, arithmetic, callbacks, and moves use `noexcept`. Allocation, parsing, I/O, and cancellable scoring can throw.

## Browser transport

A dedicated persistent Edge profile serves invisible retrieval and visible access recovery.
The access window closes before retrieval continues. Extensions remain disabled.

The local Chrome DevTools Protocol (CDP) transport uses asynchronous WinHTTP with a blocking worker interface.
Connections share a process-wide session. Callback contexts keep buffers until handle closure.
Session cleanup waits for the unload notification.

Each command has one deadline across fragments and unrelated events. The normal deadline is 15 seconds.
Cleanup commands use 2 seconds. Event waits check cancellation.

JSON results keep their resource owners. Results move from replies.
HTML views stay within each reply's lifetime. Browser streams use the Windows null device and explicit inherited handles.

## Report store

Schema-1 JSON keeps all chart entries, ownership, original metadata, candidate values, warnings, counts, and whitelist data.
Excluded chart entries remain available for later whitelist removal.

Optional candidate flags are `version_gap`, `artist_gap`, `section_gap`, `compound_gap`, `annotation_gap`, and `diacritic_gap`.
Older reports default missing flags to false. Raw caches remain compatible because each run rebuilds identities.
Saved ownership decisions require another comparison to change.

An installation lock serializes updates. Comparison publication reads the current whitelist again.
Save replaces the complete proposed whitelist. It does not merge external entries.
Canonical exclusion identities determine differences. Comments, formatting, and order do not count.

The whitelist commits first. Report refresh reads the latest generation.
A refresh failure reports partial success without reversing the whitelist write.
Imports use a worker for checks and require a user decision. Imports copy sources.
JSON imports keep the active TOML whitelist and receive a new local generation.

## Reviewer and launcher

One WebView2 instance runs in a single-threaded apartment (STA). It hosts one embedded shell.
Screens change without navigation reloads.
Results parsing starts only when necessary. Tables contain 100 rows per page.
Collapsed sections delay creation of Document Object Model (DOM) nodes.

Filtering caches normalized search text. Selection changes update existing controls.
Native work runs on a joined worker. The graphical user interface (GUI) thread owns dialogs and Component Object Model (COM) operations.
Weak callback references and modal guards protect closure.

Metadata uses text nodes. Native code checks selections and RYM links.
External links open in the normal browser. The reviewer makes no RYM requests.

Prepared UTF-16 messages leave memory after delivery. The report model remains available for saves.
Minimized windows disable WebView2 visibility and request lower memory use.
Report memory grows with record count. WebView2 has separate runtime costs.

The reviewer starts the fixed CLI with absolute paths and `--no-open`. It uses no shell or terminal.
Only selected handles pass to the child. A private, versioned channel carries progress.
Human diagnostics use a separate buffer with a 256 KiB limit.

A worker supervises the child. Progress updates occur at most four times per second.
Elapsed time updates once per second. Cumulative counters keep chart and local counts during access recovery.

The suspended child enters an owned Job Object before execution. The Job Object removes comparison children after unexpected reviewer termination.
Cancellation uses an inherited event. Stop and close waits for cleanup.
Only a checked launcher channel permits hidden-run access prompts.

Atomic report publication completes the comparison. Later settings, cache, or reviewer start failures give warnings.
[Testing](testing.md) records validation limits. [Microsoft's performance guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/performance) explains WebView2 runtime costs.
