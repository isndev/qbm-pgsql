# Changelog

All notable changes to the qbm-pgsql module are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the module tracks the qb framework's
[Semantic Versioning](https://semver.org/). Framework-wide policy is in the qb
[VERSIONING](https://github.com/isndev/qb/blob/main/VERSIONING.md) document.

## [Unreleased]

### Added

- **The connection's side of the reconnect contract (Huly QB-111; `qb/io/reconnect.h`).** A connection reports where
  it stands and why it went down in qb-io's vocabulary, the one qbm-redis uses: `state()` (`idle`, `connecting`,
  `handshaking`, `ready`, `reconnecting`, `down`), `down_reason()` (`user`, `peer`, `protocol`, `retries_exhausted`,
  `handshake_refused`), and three hooks, empty until set and read only on those transitions -- `on_connected()` each
  time the connection becomes ready, `on_disconnected(down_reason)` once for every ready connection lost (the two
  strictly alternate), `on_down(down_reason)` when it reaches `down`. `enable_auto_reconnect(RetryPolicy)` arms a
  reconnection run for a ready connection the server or the network takes away (`disable_auto_reconnect()`,
  `is_reconnecting()`); `connect_with_retry(policy)` makes the policy's attempts up front; `qb::pg::RetryPolicy` is
  `qb::io::reconnect_policy`, the type of `qb::redis::RetryPolicy` and `qb::http::RetryPolicy`. An explicit
  `disconnect()` is terminal whatever the policy, and ends the session. Every connection, the first one included, runs
  the whole handshake before it is ready: authentication and the first ReadyForQuery, then the hook installed with
  `set_handshake(hook)` -- a `task<bool>(qb::pg::handshake_session &)` whose session queues on the link only while
  that handshake runs (a copy kept past it fails with `"handshake over"`), so no other coroutine can slip a query past
  the gate -- then the replay of the listened channels. Work submitted outside `ready` is refused, as before (Huly
  QB-917: `connection is not established`); qbm-redis holds it instead. A refusal -- SQLSTATE class 28 (credentials,
  authorization) or 3D000 (no such database), a server proof that does not verify, a build without OpenSSL asked for
  SCRAM or MD5, or the hook returning `false` -- is terminal (`handshake_refused`, never retried); any other startup
  failure, a link lost during the handshake or a handshake past the attempt's deadline is one more attempt. What was in
  flight when a ready link dropped fails once, with the server's reason or `database disconnected`, and is never
  replayed. `stats()` counts `reconnects` (ready connections reached after a lost one) and `gaps` (gap markers
  emitted), the names qbm-redis uses. Pinned by `integration/connection/reconnect-contract.cpp`.

- **LISTEN survives a reconnection, and the stream says where it has a hole (Huly QB-111).** `listen()`, `unlisten()`
  and `unlisten_all()` maintain the connection's listened set (`listened_channels()`) when the server confirms them;
  every new session replays it at AuthenticationOk, before any other query. A `LISTEN` written as SQL is not tracked
  (the connection says so once, at DEBUG). An explicit `disconnect()` ends the session: the set is forgotten, and a
  later `connect()` is a fresh session -- nothing replayed, no gap. PostgreSQL does not replay the NOTIFYs a session
  missed, so a session that follows an earlier ready one opens a hole, reported when that session is ready -- in
  qbm-redis's order: the hole, then the notifications the session received while it was handshaking (held until
  then), then `on_connected` -- so whoever learns of the hole can resynchronise on the connection at once:
  `notify_co_consumer` places a gap marker in its stream (`notification::kind == notification_kind::gap`,
  `gap.reason == qb::io::gap_reason::reconnect`), `notify_cb_consumer` calls the new `on_gap(qb::io::gap_info)`. A full co-consumer stream is a hole too: the NOTIFYs that do not fit still reach
  `on_notify_dropped` and `stats().notifications_dropped`, and ONE marker (`overflow`, `gap.lost` = the drops,
  coalesced) precedes the next notification the stream takes, or ends what the reader drained. Markers are always on.
  The stream ends only at `down`: `receive()` yields what it holds, then `std::nullopt`. Pinned by the
  `CoConsumer_ARunReplaysTheListenedSetAndMarksTheGap`, `CbConsumer_OnGapComesBeforeTheNextNotify`,
  `CoConsumer_ANotifyDuringTheHookIsHeldBehindTheMarker`, `CbConsumer_ADisconnectFromOnGapStopsTheRelease`,
  `CoConsumer_TheStreamEndsOnlyAtDown`, `CoConsumer_AnOverflowIsOneCoalescedGapMarker`,
  `AnExplicitDisconnectEndsTheListeningSession` and `ARawListenIsNotReplayed` cases of
  `integration/notify/listen-notify.cpp`. ABI: `qb::pg::notification` grows by `kind` and `gap` and `qb::pg::stats` by
  two counters (additive member growth in qbm types, allowed under the 3.3.0 ABI reading and pinned by a
  `static_assert` against a mirror layout) -- consumers recompile with the train.

- **A server error keeps every field the server sent, and notices reach a handler (Huly QB-108).** The client
  decoded all the ErrorResponse fields and kept four (message, severity, SQLSTATE, detail): `constraint_name` -- the
  field a handler of 23505 wants -- `table_name`, `schema_name`, `column_name`, `data_type_name`, `hint`, `position`,
  `internal_query`, `internal_position`, `where`, `file_name`, `line` and `routine` were dropped, and `V`, the
  non-localized severity, was not even decoded. They now live in one public `qb::pg::server_error_fields`, shared by the
  error's copies (`db_error::server_fields()`, null for an error the client raised) and read through `std::string_view`
  accessors (`db_error::constraint_name()`, ...), empty when absent. `database::on_notice(fn)` receives every
  NoticeResponse with the same fields (`qb::pg::notice`); without a handler a notice is logged at INFO, as before; an
  exception escaping the handler is contained. The decoder's `qb::pg::detail::notice_message` now derives from the
  public struct, so one definition of the fields remains. ABI: `db_error` -- and so every `Reply<T>`, which embeds one
  -- grows by one `std::shared_ptr` (16 bytes on a 64-bit target, allocated only for a server error, pinned by a
  `static_assert`); additive member growth in a qbm class, allowed under the 3.3.0 ABI reading -- consumers recompile
  with the train.

- **`database::stats()` and `database::on_query_complete()` (Huly QB-115).** `stats()` returns a `qb::pg::stats`
  snapshot by value: monotonic `std::uint64_t` counts since construction — `queries`, `errors`, `bytes_sent`,
  `bytes_received` (PostgreSQL protocol bytes, the handshake included, TLS framing excluded), `notifications_received`,
  `notifications_dropped` (what a `notify_co_consumer`'s bounded channel refused), `prepared_hits`, `prepared_misses`,
  `prepared_evicted` — and `last_query_duration`. `on_query_complete(fn)` calls `fn` once per completed query with a
  `qb::pg::query_info` (`kind` — `qb::pg::query_kind` — `name`, `sql`, `duration`, `ok`, `error`), on the connection's
  thread, before the query's own callback or awaiter; an exception escaping it is contained and logged. A query is one
  round trip of the serial queue; one that failed before it was sent (refused locally, drained by a disconnect) is
  counted and reported with a zero duration. Without a hook a completion pays one predictable branch; the duration
  costs one clock read per query on a busy connection (the ReadyForQuery instant ends one query and stamps the next).
  New header `qbm/pgsql/stats.h`. ABI: `Database` gains the counters, the hook and the clock state (additive member
  growth in a qbm class, allowed under the 3.3.0 ABI reading); the internal `ISqlQuery` gains the pure virtual
  `describe()`, and `Transaction::fail_all_pending` an optional `drained_query_observer *` — consumers recompile with
  the train.

- **The prepared-statement cache has a public bound, and counts its hits and misses (Huly QB-207).**
  `db.prepared_cache_capacity(n)` sets the LRU's bound (100 by default; `0` restores it) and
  `db.prepared_cache_capacity()` reads it. `PreparedStorage::hits()` / `misses()` count the named executions found
  in the cache / refused because the name was not cached, beside `evicted_count()`; the inline `query(sql, args...)`
  path's unnamed statement is not counted. ABI: `PreparedStorage` gains three members (the queue of owed Closes and
  the two counters) — additive member growth in a qbm class, allowed under the 3.3.0 ABI reading; consumers
  recompile with the train.

- **UUID, timestamptz, NUMERIC, JSON and JSONB arrays have typed result codecs (Huly QB-206).**
  Read one-dimensional arrays as `std::vector<T>` or `std::vector<std::optional<T>>` for NULL elements.
  Prepared queries request binary for all five array OIDs; simple-query text arrays decode too.
  Timestamp and JSON/JSONB array text quotes and escapes each element, preserving JSON `null`
  separately from SQL NULL. The existing binary parameter path and seven earlier element types
  keep their contracts; multi-dimensional arrays remain unsupported. `results::json()` exports
  these binary arrays as text with NULL elements and lower bounds preserved. JSON/JSONB elements
  retain their wire text, including long decimals, without converting through a JSON DOM.

- **A coroutine parked on PostgreSQL says so in qb's `CoroutineScheduler::dump()` (Huly QB-71).** `pg_awaiter` (every
  `pg_reply_awaiter<T>`: a query, an execute, a prepare, a transaction step) and `connect_awaiter` begin their
  `await_suspend` with `qb::io::async::track_suspension(h, ...)`: with suspension tracking on, the dump shows the
  coroutine waiting on `"pgsql"` or `"pgsql connect"`, for how long. An awaiter without the call would leave the
  coroutine with the record of its previous wait, ageing. Off -- the default -- the call is one predictable branch.
  Pinned by `TheDumpSaysACoroutineWaitsOnAConnectThenAQuery` in `tests/integration/api/coro-api.cpp`; qb's
  `scripts/check-awaiter-tracking.py`, run over this module from the superproject, refuses an awaiter without it.

### Changed

- **Qualified timestamp converters share the canonical binary decoder.** Direct
  `const`/reference-qualified `TypeConverter` instantiations now delegate to the
  existing `qb::wall_time` specialization. Raw and legacy-prefixed representations,
  negative fractional timestamps and malformed-buffer diagnostics are unchanged.

- **Behaviour change -- `connect()` resolves, and `is_connected()` is true, only once the handshake ran (Huly
  QB-111).** The connected flag used to be set at AuthenticationOk, before the first ReadyForQuery, and
  `is_connected()` was the io base's flag -- true on a database that had never connected. `connect()` and
  `connect_with_retry()` now resolve when the handshake succeeded -- authentication, the first ReadyForQuery, the hook
  installed with `set_handshake()` and the LISTEN replay -- (`true`) or the attempt failed, was refused or superseded
  (`false`); a connect while a connection, a handshake or a run is in progress resolves `false` at once, and one on a
  ready connection `true` at once. `is_connected()` is `state() == ready`. A server that refuses the session after
  AuthenticationOk -- 3D000, the database does not exist -- now fails the connect: it used to resolve it.
  `connect(dsn)` and `connect(connection_options)` change the stored options only while the connection is idle or
  down, so a live connection, and the run that may recover it, keep theirs. `prepare_reconnect()` does nothing while a
  connection, a handshake or a run owns the link: it used to close the live socket under the connection.

- **Behaviour change -- a `notify_co_consumer`'s stream ends only at `down`, and it carries gap markers (Huly
  QB-111).** A lost link used to close `receive()`'s channel; with a reconnection run armed it now stays open across
  the loss, and the new session's notifications follow a gap marker, queued when it is ready. Without a run, or after `disconnect()`, the loss is
  terminal and the stream ends as before. A reader distinguishes a marker by `notification::kind` -- a loop that only
  reads `channel` and `payload` sees an element with both empty where the stream has a hole.

- **Source change -- `notify_cb_consumer` is a callback-only consumer (Huly QB-1092).** It was an alias of
  `notify_co_consumer`: every NOTIFY went to `on_notify` AND into `receive()`'s bounded queue (8192 by default), which
  a callback-only program never reads -- the queue kept up to 8192 payloads alive, then every later NOTIFY was counted
  in `stats().notifications_dropped` and logged as dropped although the callback had handled it. It is now its own
  class: `on_notify` only, nothing queued, nothing dropped; an exception escaping the callback is contained (a
  non-`std` one too). A program that called `receive()`, `on_notify_dropped()` or `notify_channel_capacity()` on a
  `notify_cb_consumer` uses `notify_co_consumer`, which is unchanged. Pinned by `CbConsumer_RetainsNothingAndDropsNothing`
  (8192 + 8 notifications, all delivered, none dropped) and `CbConsumer_OnNotifyCallbackThrows_StillDelivers`.

- **NUMERIC, timestamp and array rendering retain their contracts (Huly QB-206).**
  NUMERIC formats its limbs with integer conversion instead of `printf`, transfers decoded text into its owning
  carrier, and renders that carrier directly inside arrays. UTC timestamp text uses the existing qb calendar
  and a stack buffer for years 1970–9999; other years keep the previous formatter. JSON/JSONB array projection
  borrows element text only while rendering.
  The final result owns its text; public types, scale, special values, NULLs and wire-format selection are unchanged.
  Completed JSON row objects move into the output array instead of copying their keys and strings.
  Repeated exports preserve the source result and each returned projection remains independently owned.

- **`results::json()` writes a binary array's literal in place (Huly QB-1087).** QB-206 asks for `uuid[]`,
  `timestamptz[]`, `numeric[]`, `json[]` and `jsonb[]` results in binary, so the export re-renders the
  literal the text format would have carried. It decoded the array into a vector of typed values, built a string
  per UUID, timestamp and NUMERIC element (two for a NUMERIC with a fraction), grew the literal from an
  eight-bytes-per-element guess, escaped quoted text with a call per escaped byte, and copied the whole
  literal again behind its `[lower:upper]=` bounds. It now validates the framing once, writes the bounds first
  and appends every element into one string reserved from the wire size: UUID and timestamp text through
  `append_text`, NUMERIC limbs decoded straight into the output (`append_pg_numeric_text`, which
  `decode_pg_numeric` and `from_binary` share), quoted text escaped without a branch per byte. callgrind, g++-14
  Release, one 32-element array with 16 NULLs (`BM_JsonExportExtendedArray`), instructions per export:
  uuid 10692 -> 5715, timestamptz 18935 -> 10217, numeric 35492 -> 12685, json 20899 -> 12874, jsonb
  21028 -> 12938, int4 7634 -> 5416, text 9042 -> 5535; on json, the branch simulator's mispredictions fall from
  173 to 26.
  Every spelling is unchanged, pinned by a test that also passes on the previous renderer, and NUMERIC by a
  differential test against the string-building decoder over 20 000 random headers. `render_pg_array_text`
  writes its elements through the same appenders. A malformed array that is wrong in two places may report the
  framing fault before the element fault; both throw.

- **`encode_pg_numeric` writes its limbs in place (Huly QB-1088).** Encoding a NUMERIC parameter built the
  integer and fraction digits in strings, padded each into a new one by concatenation, collected the limbs in
  a `std::vector<int>` and grew the output with an insert per 16-bit word. type_converter.cpp exhausts g++'s
  `inline-unit-growth` budget, so whether those string helpers were inlined into the function depended on the
  rest of the TU: under `-march=native`, the benchmarks preset's, train 3's rewrite of the TU left two
  `std::string` move constructors out of line (1641 -> 1411 instructions, +28 instructions per call), while
  the portable `-march=x86-64` build was unaffected. The encoder now reads each limb straight from the digits
  into one output buffer, the only allocation: 858 instructions whose only calls are the buffer's, and per
  `BM_EncodeWideNumeric` call (callgrind, g++-14) 3892 -> 696 instructions with `-march=native` and
  3927 -> 698 with `-march=x86-64`. Its bytes and rejections are those of the string-building encoder, held
  by a differential test over 20000 random decimal texts and the edge cases.

- **Typed JSONB decoding combines precision validation and DOM construction where supported (Huly QB-945).**
  Audited nlohmann versions and configurations reuse its DOM builder and numeric formatter.
  Other configurations keep the checked public parser and its position metadata.
  Value/error contracts, public API and carrier layout remain unchanged. A later rejected
  number can follow allocation of a DOM prefix, which is destroyed during unwinding.

- **A multi-statement simple query whose statements return rows of different shapes now fails instead of
  mixing them (Huly QB-122).** `execute("SELECT …; SELECT …", …)` collected every statement's rows into ONE
  result under the LAST RowDescription, undocumented: the earlier statement's rows were then decoded against
  the later statement's columns -- a `field_type_mismatch` or an `out_of_range` at best, a wrong value in
  silence at worst. Statements of the same shape (column count, types, formats) still merge into one result
  as before; a different shape arriving after rows were collected fails the query with a `client_error`, once
  the server has finished with it, so the connection stays usable -- exactly the queries that returned wrong
  data. An earlier statement that returned no rows is superseded, as before. Documented in
  `readme/results.md` and the llm gotchas.

### Fixed

- **Primitive-reader test comments name the current branches (Huly QB-1102).** Correct the
  binary auto-detection reference and replace retired module/SQLSTATE/boolean-reader coordinates
  with their actual symbols. No decoder, query path or test assertion changes.

- **`connect(dsn, raw_io)` runs the handshake on the transport it is given (Huly QB-1096).** The overload was
  documented as adopting an already-connected transport, but it stored the transport and then dialled `dsn` itself:
  the transport was never used (and stayed open in the client until the next connect). It now adopts it for the FIRST
  connection of the lifecycle -- the whole handshake (authentication, the hook, the LISTEN replay) runs on it -- while
  `dsn` still names the session and the address a reconnection run or a later `connect()` dials. On
  `tcp::ssl::database` the caller must already have run the PostgreSQL TLS negotiation on it (the SSLRequest, the
  server's `'S'`, the TLS handshake). A transport that is not open fails the attempt (`down`, `down_reason::peer`);
  on a busy or ready connection the call resolves `false` and closes the moved-in transport unused, since the
  connection already has its link. Pinned daemon-free by `system/connection/adopted-transport.cpp`: adoption, a
  reconnection dialling the DSN, a closed transport, a ready client, and a TLS-negotiated transport on a secure
  database.

- **A bare reconnect starts with an empty prepared-statement cache (Huly QB-1095).** Prepared statements belong to
  the backend session, but only `prepare_reconnect()` cleared the cache: `connect()` after `disconnect()` -- a
  supported path -- kept it, so an `execute(name)` of a statement the old backend held passed the client's check (a
  cache hit), was sent, and failed on the server with 26000 (`invalid_sql_statement_name`) instead of being refused
  client-side. Every connect path now resets the per-backend state where it installs the new transport: the statement
  cache (and the protocol `Close` messages still owed to it), the ParameterStatus cache and the transaction status.
  Pinned by `ABareReconnectStartsWithAnEmptyStatementCache`.

- **A prepared statement the cache evicts is closed on the server (Huly QB-207).** The LRU's eviction was local
  bookkeeping only: the backend kept every statement the connection ever prepared — 101 after 101 prepares into the
  default 100-slot cache, without bound on a long-lived session — and preparing an evicted name again, the recovery the
  client's "not registered" error asks for, failed on the server with 42P05 (`prepared statement "…" already
  exists`). A Parse now makes room before it is written and sends `Close('S', name)` for the statement it pushes out
  ahead of itself in the same message (no round trip is added; the existing CloseComplete handler absorbs the reply).
  Every other eviction — a shrink of the bound — queues its Closes for the next Parse or Bind. The unnamed statement is
  never closed. Pinned by `LruEvictionClosesTheEvictedStatementOnTheServer` and
  `PreparedCacheCapacityBoundsTheServerSideStatements` against a live server (`pg_prepared_statements`) and
  `EvictingParseCarriesTheCloseInItsOwnMessage` on the wire bytes.
- **Shrinking the prepared-statement cache keeps exactly the new bound (Huly QB-207).** `PreparedStorage::set_max_size`
  applied the pre-insertion rule (make room for one more) and left a shrunk cache one entry below the capacity it had
  just been given. Pinned by `SetMaxSizeShrinksToTheNewBoundExactly`.
- **`Database::disconnect()` ends the server backend (with qb's Huly QB-1060).** qb-io's standalone
  `dispose()` left the socket untouched, so the backend lived until the `Database` was destroyed. With
  qb's fix the sending side is shut down at teardown, the server reads EOF and the backend leaves
  `pg_stat_activity`; the descriptor is released at destruction or the next `connect()`. Pinned by
  `ConnectionLifecycle.DisconnectEndsTheServerBackend`.
- **A row difference holds any two row ordinals (Huly QB-986).** `resultset::difference_type` -- and with it
  `row::difference_type`, the type of `row_a - row_b` -- was a 32-bit `integer` while its own row iterator already
  measured distances in `std::ptrdiff_t`. Row ordinals are 32-bit unsigned, so past `INT32_MAX` they converted to
  negative values and the subtraction overflowed. The container's `difference_type` is now the iterator's
  `std::ptrdiff_t`, which a `static_assert` requires to be wider than an ordinal; widening is source compatible, and
  the iterators, whose `advance()` already took `std::ptrdiff_t`, are unchanged.
- **The PostgreSQL guides use the binary NUMERIC marker and document installed package consumption (Huly QB-689, QB-694).**
  The prepared-statement recipe binds `qb::pg::detail::numeric` for OID 1700 and decodes the result with the matching
  converter; a plain decimal string is not binary NUMERIC. The integration page covers source and installed CMake
  targets, including the dependency on an installed qb package.
- **System and integration test fixtures keep workers and coroutine state alive on fatal exits (Huly QB-406, QB-412).**
  CancelRequest workers join before listener state dies; the resilience fixture destroys
  suspended frames before releasing the database and its result flags.
- **The connect timeout suite distinguishes a real handshake deadline from native errors (Huly QB-439).**
  A silent loopback backend accepts StartupMessage and stays open until the configured timer
  ends the attempt. Bad TLS material now targets a local listener that detects fallback TCP.
- **API documentation checks reject comment-only names and invalid later citations (Huly QB-561, QB-562).**
  The shipped guard indexes C++ code without comments or literals and examines every
  citation on a line before accepting it.
- **Scheduled tests and documentation checks select their intended inputs (Huly QB-558–560, QB-621).**
  Nightly uses paired `develop` branches, doc-lint triggers on checker and workflow edits,
  the pull request template points to the public superbuild recipe, and clang-tidy includes
  module headers.
- **NOTIFY rejects payloads at the server's byte boundary (Huly QB-996).** Both callback
  and coroutine forms accept 7,999 bytes and reject 8,000 bytes or more locally, before
  queuing SQL. The local `client_error` retains its existing code `"00000"`; the connection
  remains usable and no notification is published for the rejected payload.
- **Async cancellation waits for the socket direction TLS needs (Huly QB-1001).** A native
  TLS write that needs peer input now waits for readability alone instead of repeatedly
  waking on a writable socket. Plain writes wait for writability; custom secure transports
  without native TLS state keep the combined read/write fallback.
- **Present optional field reads check SQL NULL once (Huly QB-646).** Scalar and optional
  reads share the format- and OID-aware conversion after the public NULL decision.
  Empty text stays present; SQL NULL, nested optionals and typed errors keep their contract.

- **Concurrent unverified TLS connects emit one warning without a data race (Huly QB-628).**
  The warning flag is shared by each TLS client specialization and can be reached from different
  actor cores; its one-time decision is now atomic. Verified TLS and plain connections do not
  enter the warning branch, and query execution pays no extra cost.
- **`notify()` preserves payload text with either PostgreSQL string setting (Huly QB-630).**
  Both callback and coroutine forms now use a dollar-quoted literal whose first closing match
  follows the entire payload, including when its suffix overlaps the closing tag. This leaves
  backslashes, apostrophes, and multibyte characters such as SJIS `表` untouched by SQL escaping,
  while preserving the callback's single simple-query command.
  Channel identifiers remain quoted; empty payloads still omit the payload clause.
- **Copies of `PreparedStorage` own their LRU index (Huly QB-649).** A copied cache previously
  kept list iterators from the source. Copy construction rebuilds the index, and copy assignment
  leaves the destination intact if building the replacement fails. The database still owns a
  single cache per connection; this corrects direct use of the storage value type. Move assignment
  also guards self-move, which otherwise left a populated map with an emptied LRU list.
- **A const prepared-storage lookup updates recency without casting away const (Huly QB-993).**
  The LRU list is mutable cache bookkeeping, so `get()` can promote an entry on a genuinely
  const storage object without modifying a const subobject through `const_cast`.
- **Prepared-storage hits preserve their LRU nodes (Huly QB-994).** `get()` and an existing-name
  `push()` now relink the node in place. Promotion no longer allocates a replacement node or
  leaves a dangling map iterator if an allocation fails.
- **Named row extraction rejects too few column names before touching outputs (Huly QB-647).**
  Variadic `row.to({names...}, targets...)` now throws the same `db_error` as the tuple overload
  when names run out, instead of reading beyond the initializer list. Zero or one name for
  two targets leaves both targets unchanged.
- **Row ordinals remain correct past 32767 rows (Huly QB-648).** `row.row_index()` now returns
  the result-set row size type rather than the 16-bit column count type; row differences across
  that boundary no longer wrap.
- **Input field-buffer seeks from end use the position after the last byte (Huly QB-662).**
  `seekoff(0, end)` now reaches EOF, `seekoff(-1, end)` reaches the last byte, and out-of-range
  seeks fail before pointer arithmetic, including on an empty range.
- **Callback savepoint failures stay within their savepoint (Huly QB-625).** Once `SAVEPOINT` succeeds,
  a failed child rolls back to it and releases it before the outer callback block continues. The
  savepoint error callback runs once after cleanup, including when the body callback throws;
  a failed create or cleanup still aborts the outer block. A failure already recorded earlier
  in the same fluent chain keeps its original error after a later savepoint recovers.
- **A root `then` callback exception is visible (Huly QB-661).** It becomes a client error for
  the following `error` callback and `await()` status; already completed SQL is not undone.
- **Invalid `execute` success callbacks fail compilation (Huly QB-624).** Simple and prepared
  callback calls accept `(Transaction&)` or `(Transaction&, resultset)`; an incompatible
  signature can no longer compile while silently omitting its query.
- **Reconnect guidance matches the supported client path (Huly QB-202).** Calling
  ordinary `connect()` after `disconnect()` on the same object performs a fresh handshake;
  `prepare_reconnect()` is an optional explicit fd and per-backend state reset.
  The README, connection guide, public reference, and header now describe that contract.
- **Typed JSONB reads reject decimal value changes (Huly QB-945).**
  `field.as<qb::jsonb>()` now throws `client_error` if nlohmann would round a
  PostgreSQL JSONB number, including nested decimals and integers outside its
  integer ranges and `1e400` beyond the DOM floating range. Malformed JSON
  keeps its separate parse error; equal decimal values with different spellings remain valid.
  `field.jsonb_text()` exposes the server's canonical text without parsing or
  copying, with OID/format/version checks and backing-row lifetime;
  `jsonb_text_copy()` keeps an owning copy. The `qb::jsonb` layout is unchanged.
- **Overlapping inline parameterized queries keep their own SQL (Huly QB-106).**
  `query(sql, args...)` now keeps Parse/Describe and Bind/Execute in one queued command.
  Two coroutines on one connection can no longer replace the unnamed statement between
  those phases and return another query's plausible result. The two server round-trips
  and per-column result formats remain as before.
- **Overlapping COPY calls keep their data with the admitted command (Huly QB-629).** A second
  `copy_out` or `copy_in` on one connection now fails explicitly before sending SQL or invoking
  its sink/source. Previously it replaced the first command's callback: the first COPY OUT
  delivered its rows to the second sink, while the first COPY IN read the second source and
  loaded the wrong table. A plain COPY query queued ahead cannot borrow a later `copy_in`
  source. Destroying an awaiting coroutine detaches its callback immediately while its
  command keeps the COPY reservation until completion; sequential COPY and connection
  reuse still work. COPY OUT keeps its active sink and owned captures alive through each
  invocation, including when that sink destroys its own waiter or disconnects the connection;
  later chunks cannot invoke the detached sink. Ordinary chunk delivery borrows the operation
  and sink without copying either shared owner; cancellation or completion during the call
  retains the needed ownership in the outermost invocation until it returns, including nested calls.
  If a source destroys its own awaiting coroutine inside a callback,
  the returned chunk is discarded and the command sends one `CopyFail` before any
  further source call.
- **Present empty text and binary numeric values stay correct through `field::as<std::optional<T>>()` (Huly QB-646).**
  SQL NULL metadata now decides whether the optional is empty; a present value is decoded through `T` with
  its column format and OID. `results::json()` keeps an empty string as `""` rather than JSON null, and
  binary `int8` read as `optional<double>` converts numerically instead of reinterpreting its bytes.
- **Binary JSON and JSONB decoding keeps arrays of pairs as arrays (Huly QB-944).**
  `field.as<qb::jsonb>()` and the direct JSON binary decoder used to reinterpret
  `[[1,2]]` as an object despite PostgreSQL sending ordinary JSON text with no
  object marker. Both now parse that text without changing its structure; binary
  version checks and parse errors still fail loudly.
- **`results::json()` decodes binary result columns before stringifying them (Huly QB-943).**
  Prepared `int8`, `float8` and other binary columns used to become JSON strings containing
  PostgreSQL wire bytes. The export keeps its existing string-valued contract: scalar
  values use the column OID's text formatter, JSONB keeps PostgreSQL's canonical text
  exactly (including pair arrays and arbitrary-precision decimals), SQL NULL is JSON
  null, and empty TEXT is `""`. BYTEA is `\x` hex; one-dimensional arrays preserve
  NULL elements and non-1 lower bounds as `[lower:upper]=...`. Unsupported binary OIDs,
  JSONB versions and overflowing array bounds fail loudly instead of exporting bytes
  or a changed value.
- **Fragmented `ReadyForQuery` completes as soon as its final byte arrives (Huly QB-626).** The
  framer no longer asks for another five bytes after it has already consumed the header; a
  six-byte response split 5+1 or 1+4+1 no longer leaves the next query waiting.
- **A local prepared-statement error leaves the connection ready (Huly QB-659).** A fluent
  `.error(...)` branch with no query no longer clears readiness permanently; a following
  valid query is sent without waiting for a server response that cannot arrive.
- **`disconnect()` from a query callback no longer re-enters a completed query (Huly QB-627).**
  The active query is removed before its callback, and queued failures wait until that callback
  returns so its transaction owner stays alive. The same lifetime guard covers fluent
  `.then(...)` / `.error(...)` callbacks invoked while a transaction command is popped;
  disconnect there cannot destroy the parent before the traversal finishes. Success and
  error callbacks complete once; queued work fails once.
- **Callback commands on a disconnected database fail immediately (Huly QB-917).** The
  common command queue rejects callback `execute`, prepared `execute` and `prepare` before
  serialization, delivers their error callback once, and records a failed status for
  `await()`, matching the coroutine overloads' closed-handle behavior.
- **`notify_co_consumer::receive()` serves the next connection (Huly QB-252).** A disconnect closes the consumer's
  queue, and a channel closes for good: after a reconnect every NOTIFY was dropped -- and logged as "buffer full" --
  and `receive()` yielded `std::nullopt` for ever, so the documented advice was to build a new consumer. The queue
  is now replaced on the first `receive()` or NOTIFY of the next connection, carrying over in order what was
  received and not yet read. LISTEN still has to be re-issued: the subscription belongs to the session.
- **`disconnect()` is safe from a coroutine (Huly QB-253).** It closed the socket and then ran one `EVRUN_NOWAIT`
  loop pass to observe the close, and from a coroutine body that pass re-entered the coroutine scheduler: an abort in
  a debug build, which the documentation answered with "call it from a callback or `main()`" and the
  `07-listen-notify` example by not calling it. It now completes the teardown in the call through qb-io's
  `disconnect_now()`, as qbm-redis's `disconnect()` does -- no loop pass, so no other watcher, deferred callback or
  coroutine runs under the caller -- and still fails every in-flight and queued query first.
- **A reconnection without `prepare_reconnect()` no longer sends the previous connection's queries first (Huly
  QB-202).** `disconnect()` fails the queued queries, but their bytes stayed in the client's output buffer, and the
  bare reconnect path -- `connect()` after `disconnect()`, supported and tested -- sent them ahead of the
  StartupMessage: the server refused the connection. Every connect path now clears both buffers, with the protocols,
  where it installs the new transport (qb-io's `reset_for_reconnect()`).

### Documentation

- **Connection and quickstart guidance now follows the public API and coroutine lifetime rules (Huly QB-690, QB-691, QB-693, QB-695, QB-622).** The connect reference names `AuthenticationOk` as the success transition, removes an undeclared `qb::pg::init`, gives the cancel watchdog a named coroutine frame, points `main.cpp` at the complete standalone program, and keeps the support policy version-independent.

- **The stale `@todo COPY` comment in `pgsql.h` is gone (Huly QB-120)** -- it said COPY was "not a product
  feature yet" while `copy_in` / `copy_out` ship and are tested; the comment now says what is supported and
  that CopyBoth is not.
- **`readme/types.md` no longer says vectors of other element types fail to bind** -- binding works for any
  element type with a PostgreSQL array companion (`uuid`, `numeric`, `json` / `jsonb`, the date and time types);
  decoding them back is what is still limited to the seven listed.
- **Two `readme/transaction.md` citations re-derived (Huly QB-254).** The `begin` and `savepoint` ranges of
  `commands.h` started one line early, on the blank line above each; found by the strengthened
  `scripts/cite-check.py`.

## [3.2.1] - 2026-09-24

Lockstep release with the qb 3.2.1 train; no change in this repository (the version says compatible, this section says unchanged).

## [3.2.0] - 2026-09-21

### Added

- **`cancel_async()` -- the out-of-band CancelRequest without blocking the event loop, TLS
  included (Huly QB-113).** `cancel()` opens its second socket with a BLOCKING connect + send,
  capped at `min(connect_timeout, 2 s)`; fired from a timer on the core's loop -- the natural place
  -- it parked every actor of that core for the duration, and it sent the request in plaintext even
  on a secure database, which a `hostssl`-only server rejects. It was the one blocking call in the
  client, listed as such in both llm docs and three readme pages. `cancel_async()` (`task<bool>`)
  is the same request driven by the async connector the session itself connects through:
  `qb::io::async::tcp::connect` for a plain database, the STARTTLS connector with
  `postgres_ssl_negotiator` -- SSLRequest, the server's `'S'`, the TLS handshake -- for a secure
  one, the coroutine suspended meanwhile; then the 16 bytes written non-blocking (a would-block
  parks the coroutine on the socket's readiness). A secure database never falls back to
  plaintext: a server that declines SSL on the cancel connection fails the cancel, the rule the
  session's connect applies. Same connect budget as `cancel()`, same verdict (57014 on the
  awaiting caller, the connection survives). `cancel()` stays, unchanged on the wire: the two share
  `cancel_request_packet()`, and the TLS context builder is shared with the session's connect
  (`make_client_tls_context()`). Tests: a fake backend that hands out the BackendKeyData and reads
  the second connection (`system/connection/cancel-request-wire.cpp`: the exact 16 bytes, the
  loop's next turn running BEFORE the cancel completes, `cancel()` sending the same bytes, both
  false on a never-connected database), the coroutine twin of `CancelInFlightQuery` and a TLS twin
  in `connection-ssl` whose session is asserted TLS through `pg_stat_ssl`.

### Fixed

- **Array serde is fail-loud (Huly QB-109).** A NULL element decoded to a default-constructed
  element (`{1,NULL,3}` read as `{1,0,3}`, `{'a',NULL,'c'}` as `{"a","","c"}`); it throws
  `value_is_null` now, naming the way out, unless the column is read as
  `std::vector<std::optional<T>>` -- supported for every array type on both the result and the
  parameter side, where a `nullopt` element binds as the -1 length with the has-null flag raised.
  A multi-dimensional array was flattened row-major; it throws `field_type_mismatch` (unnest it
  in SQL, or read the column as text). A malformed value decoded to an empty or PARTIAL vector; it
  throws `client_error`. And the TEXT format (the simple query protocol) is parsed and rendered --
  `from_text` returned `{}` and `to_text` `""` for every array -- with quoting and escapes, the
  bare `NULL`, `{}`, the `[lo:hi]=` decoration and whitespace-inside-a-bare-element refused as
  `array_in` refuses it; the multi-dimensional literal is refused like its binary twin.
  `decode_pg_array` / `encode_pg_array` / `parse_pg_array_text` / `render_pg_array_text` are the
  four functions, `TypeConverter<std::vector<std::optional<T>>>` the new specialisations.

## [3.1.0] - 2026-08-30

Lockstep release with the qb 3.1.0 train. In this repository only two `qbFunctions.cmake`
citations re-keyed (a comment grew upstream).

## [3.0.1] - 2026-08-29

Lockstep patch with the qb 3.0.1 train (cut for qb-examples, Huly QB-4). In this repository only
tooling: a nightly CI run against qb's `develop` of the day, and `scripts/gen-llms-txt.py` strips
HTML comments by scanning rather than by regex.

## [3.0.0] - 2026-08-20

Tracks changes not yet part of a tagged release. Since 2026-08-11 that is **both** branches:
`main` was fast-forwarded to `develop` for the release, so the module version is **3.0.0** on either,
in lockstep with the qb framework; see the qb CHANGELOG for what makes that release major.

### Removed

- **BREAKING — `resultset.inl`, `transaction.inl` and `transaction_coro.inl` no longer exist.**
  Every definition moved verbatim into the header that already included it, at exactly the position
  the `#include` occupied, so none changed and none was dropped. The counts are identical on both
  axes before and after — 43 `template<…>` headers and 22 `inline` definitions in total:

  | was | now | `template<` | `inline` |
  |---|---|---|---|
  | `resultset.inl` (204 L) | tail of `resultset.h` (was included from [`resultset.h` L839](https://github.com/isndev/qbm-pgsql/blob/0b4b9ee3516a08c0b456ab5ca2551bb7e3a61e45/src/resultset.h#L839)) | 18 → 18 | 5 → 5 |
  | `transaction.inl` (528 L) | tail of `commands.h` (was included from [`commands.h` L703](https://github.com/isndev/qbm-pgsql/blob/0b4b9ee3516a08c0b456ab5ca2551bb7e3a61e45/src/commands.h#L703)) | 25 → 25 | 17 → 17 |
  | `transaction_coro.inl` (262 L) | tail of `commands.h`, inside `namespace qb::pg::detail` (was included from [`transaction.inl` L526](https://github.com/isndev/qbm-pgsql/blob/0b4b9ee3516a08c0b456ab5ca2551bb7e3a61e45/src/transaction.inl#L526)) | *(counted with `commands.h` above)* | |

  (`transaction.inl` is 528 lines, not the 527 `wc -l` reports — it ships without a trailing
  newline.)

  The bodies land in `commands.h`, **not** `transaction.h`, because `commands.h` is the header that
  closes the declaration cycle: `transaction.h` only declares `Transaction`, while the bodies need
  the complete command types that `commands.h` defines (and `commands.h` includes `transaction.h`
  at `:26`). This is the same structural constraint that put `qb::Actor`'s bodies at the tail of
  `VirtualCore.h`. `transaction.cpp`, which used to include `transaction.inl` directly, now reaches
  them through `commands.h`.

  Verified by comparing the preprocessed token stream of `commands.h` and `resultset.h` before and
  after: both are identical (`commands.h` differs by exactly one blank line and zero tokens). A
  control confirmed the comparison can fail. [`resultset.h` L1-L838](https://github.com/isndev/qbm-pgsql/blob/8abbbd2964f393de77011d9e0416ea3a2a962284/src/qbm/pgsql/resultset.h#L1-L838) and [`commands.h` L1-L702](https://github.com/isndev/qbm-pgsql/blob/8abbbd2964f393de77011d9e0416ea3a2a962284/src/qbm/pgsql/commands.h#L1-L702) are
  unchanged, so existing citations into either class still land.

  This only breaks a consumer who included a fragment **directly** — `#include
  <qbm/pgsql/resultset.inl>` and friends. None was a supported spelling: two of the three could not
  compile alone and qb's installed-header gate carried them as named "by-design fragment"
  exclusions. Use `<qbm/pgsql/resultset.h>`, `<qbm/pgsql/commands.h>`, or the `<qbm/pgsql/pgsql.h>`
  umbrella.

### Fixed

- **`CMakeLists.txt` had no `cmake_minimum_required()`, and a standalone configure died on an
  unhelpful error.** CMake reported `No cmake_minimum_required command is present` alongside
  `Unknown CMake command "qb_status_message"` — which reads like a missing include rather than
  "wrong entry point". This module is built from the qb-dev superproject, which loads qb's CMake
  helpers first; an *installed* qb ships none of them (`lib/cmake/qb/` carries only `qbConfig`,
  `qbConfigVersion`, `qbTargets` and the `Find` modules), so pointing `CMAKE_PREFIX_PATH` at one
  does not help — the exact mistake the old error invited. There is now a
  `cmake_minimum_required(VERSION 3.24)` and a guard that names the constraint and points at the
  superproject root and the `package` preset.

- **`transaction_coro.inl` ran seven `#include` directives inside `namespace qb::pg::detail`.**
  It was spliced into [`transaction.inl` L526](https://github.com/isndev/qbm-pgsql/blob/0b4b9ee3516a08c0b456ab5ca2551bb7e3a61e45/src/transaction.inl#L526), which is *between* that namespace's braces, so its own
  `#include <cctype> <filesystem> <fstream> <sstream> <string>` and two local includes were
  processed in there. They were harmless only because `transaction.inl`'s block, at namespace scope,
  had already pulled the same headers in first — the fragment silently depended on its includer to
  neutralise its own includes. Deleting `<fstream>` from that block was measured to reparse
  `<fstream>` inside the namespace, declaring `qb::pg::detail::std` and producing 20 errors led by
  `no template named 'basic_streambuf'; did you mean '::std::basic_streambuf'?`.

  The merge hoists all seven to namespace scope, which makes that unreachable. Confirmed free: the
  preprocessed token stream is unchanged, because the includes were no-ops at both positions.

### Changed

- **Logging call sites use qb's prefixed `QB_LOG_*` macros** (65 sites). qb 3.0.0 renamed
  `LOG_DEBUG` / `LOG_VERB` / `LOG_INFO` / `LOG_WARN` / `LOG_CRIT` to `QB_LOG_*` because the
  unprefixed spellings — three of which are also POSIX `<syslog.h>` names — reached every consumer
  of this module's umbrella header and silently replaced a consumer's own. qb still defines the
  unprefixed names as `#ifndef`-guarded aliases, and that guard is exactly why these call sites had
  to move: a consumer who defines `LOG_INFO` first now keeps their definition, and this module's
  headers would otherwise have started logging through *it*.

- **BREAKING — the public include prefix is now `<qbm/pgsql/...>`** (was `<pgsql/...>`). Every consumer
  edits its `#include` lines: `#include <pgsql/pgsql.h>` becomes `#include <qbm/pgsql/pgsql.h>`. The CMake
  target is unchanged (`qbm::pgsql`), and so is the installed location `<prefix>/include/qbm/pgsql/`.
  The old spelling existed only because `qb_register_module` made this module's include root its
  PARENT directory — the superproject's `qbm/`, which does not exist in this repository at all — and
  mirrored it with `<prefix>/include/qbm` on the consumer's include path. That put the maximally
  generic top-level name `pgsql` in every consumer's include namespace. Now the module's own `src/`
  IS the include root and is copied verbatim to `<prefix>/include`, so `<qbm/pgsql/...>` is the same
  string in this tree and in an installed prefix, and the two cannot drift.
- **The source tree moved to `src/qbm/pgsql/`** — one pure `git mv`, 100 % rename detection, zero
  content change, so `git blame` and every line-numbered citation survive intact. The module's old inner `src/` is gone as a *level*, not as content: its 42 files now sit directly
  beside the umbrella they implement, so `pgsql.h`'s `./src/…` includes became `./…` and every header
  that used to install under `include/qbm/pgsql/src/` now installs directly under
  `include/qbm/pgsql/`. (The three `.inl` files this once described are **gone** — see *Removed*
  above; nothing named `.inl` ships in 3.0.)
  `tests/`, `readme/` and `scripts/` live BESIDE `src/`, never inside it, which is what
  makes a stray `#include <tests/fixture.h>` impossible rather than merely unlikely.
  The test suite now includes the shipped spelling instead of resolving `"../pgsql.h"` by string
  concatenation onto a `-I <mod>/tests` flag.
- **`project(qbm-pgsql VERSION ...)` is now `3.0.0`**, tracking `QB_FRAMEWORK_VERSION`. It had been
  left at `2.6.0` while the framework moved on. The module is not standalone-configurable (it calls
  `qb_register_module` / `qb_add_test`, which an installed qb does not ship), so its version can only
  ever mean "the qb this was built against" — and the structural breaks queued for 3.0.0 land hardest
  in the modules, where a package still claiming `2.6.0` would be actively misleading.
- **`scripts/doc-lint.sh` now validates the *value* of the `Verified-against:` markers**, not just
  their presence. It previously checked only that the marker existed, which is how every page in this
  module sat at `qb 2.6.0` across two version bumps unnoticed. The expected version is read from
  `project(qbm-pgsql VERSION ...)` — the one authoritative version available when this repo is checked
  out alone, as it is in its own CI — and cross-checked against `QB_FRAMEWORK_VERSION` whenever a qb
  tree is reachable. A version it cannot determine is a hard stop, never a skip.

## [2.6.0] - 2026-08-02

Aligned with the qb 2.6.0 framework release.

### Changed

- Test suite restructured into tiered `unit/` / `system/` / `integration/` / `benchmark/` directories.

### Fixed

- Reconnect after a failed transaction no longer surfaces a stale `55P02`.
- Empty `bytea` values round-trip as an empty buffer instead of being decoded as NULL.

### Documentation

- Narrative and reference docs overhauled; every page carries code-verified `src:` citations enforced by
  `scripts/doc-lint.sh`.

## [2.0.0]

Aligns qbm-pgsql with the qb 2.0 framework (C++20 baseline) and hardens the PostgreSQL wire-protocol paths.

### Changed

- Time handling migrated to the canonical chrono model: connect, statement, and transaction timeouts are
  `qb::duration`; the `timestamptz` type maps to `qb::wall_time` (OID 1184, integer-microsecond
  round-tripping). The PostgreSQL wire epoch (microseconds since 2000-01-01, day counts, tz offsets) is kept
  native by design. The retired `qb::Timestamp` / `qb::Duration` types are gone.
- Adapted to the qb C++20 baseline (`QB_CXX_STANDARD=20`, optional 23).

### Fixed

- Uninitialized column count on a malformed `RowDescription`.
- Undefined behavior passing a plain `char` to `std::isspace` / `std::isdigit`.
- `QueryParams` forwarding constructor no longer hijacks copy/move.
- Removed an uncompilable success-only `prepare()` overload and dead, never-compiled tuple-conversion files.
- All queued queries now fail on disconnect, so no coroutine awaiter hangs.
- The connection-deadline timer is owned by the connection and cannot outlive the `Database`.

### Security

- Contain handler exceptions at the `noexcept` `onMessage` boundary (pre-authentication denial-of-service).
- Reject a `PARSE` with more than 32767 parameter types (Bind/Parse symmetry).
- Drop the connection via `not_ok()` on a malformed frame instead of attempting a reconnect.
- Fixed a heap over-read in the timestamp binary decoder for 9–11 byte fields.

[Unreleased]: https://github.com/isndev/qbm-pgsql/compare/v3.2.1...HEAD
[3.2.1]: https://github.com/isndev/qbm-pgsql/compare/v3.2.0...v3.2.1
[3.2.0]: https://github.com/isndev/qbm-pgsql/compare/v3.1.0...v3.2.0
[3.1.0]: https://github.com/isndev/qbm-pgsql/compare/v3.0.1...v3.1.0
[3.0.1]: https://github.com/isndev/qbm-pgsql/compare/v3.0.0...v3.0.1
[3.0.0]: https://github.com/isndev/qbm-pgsql/compare/v2.6.0...v3.0.0
[2.6.0]: https://github.com/isndev/qbm-pgsql/releases/tag/v2.6.0
