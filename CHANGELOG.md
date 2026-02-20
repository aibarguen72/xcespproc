# Changelog - xcespproc

## 0.0.10

- Add `PktBertPObj` — loopback PRBS packet BERT processing object
- Configuration: `PACKET_SIZE` (bytes, default 64), `PRBS_TYPE` (7/11/15, default 7), `PACKET_LOSS_PPM` (0–1 000 000, default 0)
- Status: `syncOk` (true when last received packet matched PRBS sequence)
- Stats: `goodPackets` / `badPackets` counters
- PRBS generator: Fibonacci left-shift LFSR with ITU-T taps for degree 7, 11, and 15; lock-up state 0 forced to 1
- Drop simulation: `std::mt19937` RNG; packet dropped when `rng() % 1000000 < PACKET_LOSS_PPM`; receiver not called on drop, causing `rxLfsrState` to fall behind; next non-dropped packet detected as bad then LFSR re-synced
- 1-second repeating timer drives TX+RX; `syncSnapshot()` called from `process()` publishes status/stats to main-thread snapshot
- Factory: `TYPE=PktBert` recognised in `loadObjectsBatch()`; sample `[object.3]` added to `test/xcespproc.ini`

## 0.0.9

- Triple-buffer lock-free status/stats snapshot: each `ProcObject` carries a primary struct (process-thread-owned) and two read buffers for the main thread; `syncSnapshot()` copies primary → inactive buffer then atomically flips the index (`std::atomic<int>` with release/acquire ordering); `getStatus()`/`getStats()` return the latest snapshot — no mutex required
- `ProcObject`: add `virtual void syncSnapshot() {}` and `virtual void clearStats() {}` — default no-ops, overridden by concrete subclasses
- `UdpTesterPObj`: implement `syncSnapshot()` (flips `snapIdx_` double-buffer) and `clearStats()` (resets `stats_` to zero-initialised `UdpTesterStats{}`); `process()` calls `syncSnapshot()` as its last statement
- `UdpTesterPObj`: `getStatus()` and `getStats()` now return from the published snapshot, making them safe to call from the main thread at any time

## 0.0.8

- Scalable scheduling: three new [PROC] INI keys — `PROC_HB_INTERVAL` (ms), `PROC_MAX_OBJECTS_PER_HB`, `PROC_HB_INTERVAL_MULT` — drive rate-limited round-robin processing of up to 10 000 objects
- Deferred batch loading: objects are loaded from INI sections gradually by a main-thread timer (`loadObjectsBatch`) instead of all at startup; supports objects appearing during lifetime
- Processing thread uses epoll backend (`setBackend(EvBackend::Epoll)`) to eliminate O(n) `poll()` fd-array rebuild at high object counts
- `UdpTesterPObj`: pre-allocate send payload buffer once in `loadConfig()` — no per-send heap allocation
- `UdpTesterPObj`: move `socketFd` from `UdpTesterStatus` (public) to `UdpTesterLocalStatus` (internal)
- `ProcObject`: add `processIteration_` (uint32_t) with getter/setter for scheduler round tracking
- Rebuild against logservice 0.0.4 (thread-safe `LogManager` mutex)
- Add `test/xcespproc_alone.ini` sample configuration for standalone (no watchdog) testing

## 0.0.7

- When `-s` heartbeat is active, record PPID at startup (`originalPpid_`)
- `mainTick()` (10 s timer) calls `getppid()` and invokes `stopLoop()` with `LOG_FATAL` if the parent process has changed (PPID became 1 or any other value)
- Prevents xcespproc zombie processes when the supervising parent exits

## 0.0.6

- Move `name_` into `ProcObject` base; `getName()` is now concrete non-virtual
- Add `ProcObject::init()` to supply event loop, LogManager, and app tag; builds composite `logTag_` (e.g. `"xcespproc-1:object.1"`) used for all log calls in derived objects
- Add virtual `getConfig()`, `getStatus()`, `getStats()` returning `const void*` (default nullptr); callers static_cast to the concrete struct type
- Add `UdpTesterLocalStatus` with pre-computed `sockaddr_in dst` to avoid per-send address rebuild
- Replace chrono-based polling send/recv in `process()` with EvApplication timer (`sendTimerCallback`) and socket callback (`recvCallback`); `process()` is now a minimal IDLE→ACTIVE socket-open guard
- Add `LOG_INFO`/`LOG_DEBUG`/`LOG_WARNING` calls in `UdpTesterPObj` tagged with `logTag_`
- Update smoke tests: typed accessor casts via `static_cast<const T*>`; rewrite Test 4 for event-driven model

## 0.0.5

- Add `-i` / `--id <N>` argument (default 1) to set the process instance ID
- ID is parsed before the banner so the banner shows the full tag (e.g. `xcespproc-3 v0.0.5`)
- All `logManager.log()` calls use `logTag` (`"xcespproc-<N>"`) instead of bare `PRJNAME`

## 0.0.4

- Add SIGUSR1 heartbeat to parent process: `-s <seconds>` enables periodic `kill(ppid, SIGUSR1)` via `setParentHeartbeat()`
- Heartbeat interval defaults to 0 (disabled); parent PID is logged at startup when enabled

## 0.0.3

- Add 10 s repeating main-thread heartbeat timer (`mainTimerCallback` → `mainTick()`)
- `mainTick()` logs a `LOG_DEBUG` heartbeat message each tick

## 0.0.2

- Replace syslog listener/servers with local forwarding to xcespwdog
- xcespproc no longer listens on a UDP syslog port; it forwards its own logs via `SyslogWriter` to `127.0.0.1:LOG_LOCAL_PORT` (default 1514 — xcespwdog's listener)
- `LOG_LOCAL_ENABLE` (default True) gates forwarding; `LOG_LOCAL_PORT` sets the destination port; `-p` argument overrides at runtime
- Remove `SyslogReader` dependency, `syslogSocketCallback`, and all `LOG_SYSLOG_SERVER*` INI keys

## 0.0.1

- Initial two-thread XCESP processing application built on EvApplication
- Control thread: parses args (`ArgConfig`), loads INI (`IniConfig`), sets up logging, runs EvApplication event loop
- Processing thread: 100 ms repeating timer calls `process()` on each registered `ProcObject`
- `UdpTesterPObj`: sends UDP packets at a configurable interval with `SO_REUSEADDR` bind and non-blocking receive
- Smoke tests: `loadConfig`, IDLE→ACTIVE transition, interval enforcement, zero-interval immediate send
