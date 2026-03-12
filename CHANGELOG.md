# Changelog - xcespproc

## 0.0.17

- `PktBertPObj::generateAndSendPacket()`: move link-UP check to before TX LFSR
  advancement so the state does not drift when the link is still initializing;
  prevents initial PRBS desync on link-mode startup
- `PktBertPObj::receivePacket()`: fix re-sync in link/network mode — `rxLfsrState`
  now left at end-of-packet (tracking the remote TX stream) instead of being
  overwritten with the local `txLfsrState`; the bogus overwrite caused oscillating
  mismatches where every subsequent packet failed after the first mismatch

## 0.0.16

- `ProcObject`: add `nodePath_` field (loaded from `NODE_PATH` INI key injected by xcespmap);
  exposes the full hierarchical config path (e.g. `domain/main/udpbert/bert-a`)
- `UdpTesterPObj::buildStatusJson()`: include `"node_path"` field in status JSON
- `PktBertPObj::buildStatusJson()`: include `"node_path"` field in status JSON

## 0.0.15

### Node type / instance metadata in status JSON

- `ProcObject`: add `nodeType_` and `nodeInstance_` fields (loaded from `NODE_TYPE` / `NODE_INSTANCE`
  INI keys injected by xcespserver at deploy time); expose via `getNodeType()` / `getNodeInstance()`
- `UdpTesterPObj::buildStatusJson()`: include `"node_type"` and `"node_instance"` fields in output JSON
- `PktBertPObj::buildStatusJson()`: include `"node_type"` and `"node_instance"` fields in output JSON
- `UdpTesterPObj`: add `SHUTDOWN` INI key — when true, `process()` and `buildStatusJson()` are
  no-ops; allows a node to be declared in config without instantiating a real object

### Build

- `makefile`: add `-MMD -MP` to `CXXFLAGS` and `-include $(DEPS)` so header changes automatically
  trigger recompilation of all dependent `.o` files; prevents silent ABI-mismatch crashes

## 0.0.14

### CtrlLink — TCP client to xcespserver

- Add `CtrlLink` class (`src/CtrlLink.h/.cpp`): event-driven TCP client implementing the xcespctrl protocol toward xcespserver
  - `connect()`: blocking TCP connect, sends HELLO (`PROC_ID=N\n`), registers socket with main-thread event loop, starts heartbeat timer
  - Receives `RESET` → `clearAllObjects()` + `ACK CMD=RESET`; `DEPLOY` (INI body) → parse + `deployObjectFromIni()` + `ACK CMD=DEPLOY`; `REMOVE NAME=<n>` → `removeProcObject(n)`; `STATUS_POLL` → `buildStatusReportJson()` + `STATUS_REPORT`
  - Sends periodic `HEARTBEAT PROC_ID=N\n` at `PROC_HB_INTERVAL × PROC_HB_INTERVAL_MULT` ms (default 10 s); SIGUSR1 to xcespwdog is unchanged
  - Reconnect: a repeating retry timer (default 30 s, `CTRL_RETRY_INTERVAL` in ms) fires `doConnect()` whenever `fd_ < 0`; initial connect failure and runtime disconnect both trigger automatic reconnect without restart; retry timer is removed only on clean shutdown

### XCespProc additions

- `deployObjectFromIni(ini, section)`: creates UdpTester or PktBert from a DEPLOY body INI section, calls `init()` + `setLinkRegistry()`, appends to procObjects under procMutex_
- `clearAllObjects()`: queues all object names to pendingRemovals_ for safe deferred destruction by the processing thread
- `buildStatusReportJson()`: returns `{"objects":[...]}` JSON by calling `buildStatusJson()` on each object

### ProcObject additions

- Add `virtual std::string buildStatusJson() const` (base returns `""`)
- `UdpTesterPObj` override: `{"type":"UdpTester","name":"...","status":"ACTIVE/IDLE/ERROR","stats":{"packetsSent":N,"packetsReceived":N}}`
- `PktBertPObj` override: `{"type":"PktBert","name":"...","status":"...","stats":{"goodPackets":N,"badPackets":N,"syncOk":true/false}}`

### IniConfig additions (exsrc/iniconfig)

- Default constructor `IniConfig()` for empty instances
- `loadFromString(content)`: parses INI text from string (replaces table); used by CtrlLink to decode DEPLOY body
- `getSections()`: returns section names in first-occurrence order

### Configuration

- `xcesptest/cfg/xcespproc.ini`: add `CTRL_HOST = 127.0.0.1` and `CTRL_PORT = 9900` to `[PROC]`; standalone `[object.N]` sections commented out
- `XCespProc::init()`: if `CTRL_HOST` set → create and connect CtrlLink; else → INI-batch object loader (backward compatible)

### ProcObject name fix

- `ProcObject::loadConfig()`: use `NAME` INI attribute as runtime object name when present,
  falling back to section name only if `NAME` is absent; fixes status paths showing
  `object.1` instead of the configured instance name (e.g. `lo-tx`)

## 0.0.13

### Pull mechanism (`getPDU` / `onGetPDU`)

- `IPduReceiver` gains two optional methods: `onSendPDUTo(data, len, dstIp, dstPort)` (default falls back to `onSendPDU`) and `onGetPDU()` (default no-op)
- `PduLinkObject`: add `sendPDUTo(sender, data, len, dstIp, dstPort)` — same peer lookup as `sendPDU`, calls `onSendPDUTo` on the peer; add `getPDU(sender)` — same peer lookup, calls `onGetPDU()` on the peer
- When `UdpTesterPObj` has a timer (`INTERVAL_MS > 0`) and is in link mode, `onSendTimer()` now calls `pduLink_->getPDU(this)` instead of self-sending; the peer (PktBert) generates and pushes the packet back synchronously via `sendPDU()`
- `PktBertPObj` implements `onGetPDU()`: calls `generateAndSendPacket()` to fill the PRBS TX buffer and push it to the master via `sendPDU()` or `sendPDUTo()`

### Destination override (`sendPDUTo`)

- `UdpTesterPObj` overrides `onSendPDUTo`: builds a per-call `sockaddr_in` from the supplied `dstIp`/`dstPort` and calls `sendto()` to that address instead of the object's configured `DST_IP:DST_PORT`
- `PktBertConfig`: add `dstIp` (string) and `dstPort` (`uint16_t`) loaded from INI keys `DST_IP` / `DST_PORT` (both default to empty/0 = disabled); when both are non-empty/non-zero, `generateAndSendPacket()` calls `sendPDUTo` instead of `sendPDU`
- Extracted private `generateAndSendPacket()` helper shared by `onTimer()` (push) and `onGetPDU()` (pull)

### PktBert `INTERVAL_MS=0` support

- `INTERVAL_MS=0` is now valid for `PktBertPObj`: no self-timer is registered; object transitions to ACTIVE immediately and responds only to `onGetPDU` pull requests; useful when the master (UdpTester) owns the packet cadence
- Removed the `< 1` clamp on `intervalMs` in `loadConfig()`
- `process()` IDLE: timer registered only when `intervalMs > 0`; goes ACTIVE on `intervalMs == 0` or successful timer registration

### INI config

- `test/xcespproc_alone.ini`: UdpTester objects `INTERVAL_MS=1000` (drive cadence via `getPDU`), PktBert objects `INTERVAL_MS=0` (pull-only); `object.4` adds `DST_IP=127.0.0.1 DST_PORT=5000` to exercise `sendPDUTo`

## 0.0.12

### Link infrastructure

- Add `LinkRole` enum (`ROLE_MASTER`, `ROLE_SLAVE`) in `src/links/LinkRole.h`
- Add abstract `LinkObject` base class (`src/links/LinkObject.h/.cpp`): holds one `ProcObject*` per role as `std::atomic<ProcObject*>` for lock-free `sendPDU` / `unregisterLink` safety; `registerObject()` uses `compare_exchange_strong`; `getState()` returns `UP` when both slots are filled
- Add `IPduReceiver` interface and `PduLinkObject` derived class (`src/links/PduLinkObject.h/.cpp`): `sendPDU(sender, data, len)` snapshots both role pointers with acquire loads, resolves the peer, and calls `peer->onSendPDU(data, len)` via `dynamic_cast<IPduReceiver*>`
- Add `LinkRegistry` pure-virtual interface (`src/LinkRegistry.h`): `registerLink`, `unregisterLink`, `getLink` — decouples objects from `XCespProc`
- `XCespProc` implements `LinkRegistry`; maintains `links_` (`std::map<string, unique_ptr<LinkObject>>`) protected by `linksMutex_` (separate from `procMutex_`); `registerLink` factory creates `PduLinkObject` for class `"PDU"`; link entries are never erased so raw pointers returned by `getLink()` are stable for the application lifetime
- `ProcObject` gains protected `linkRegistry_` pointer and public `setLinkRegistry()` injector; `XCespProc::loadObjectsBatch()` calls `obj->setLinkRegistry(this)` immediately after `obj->init()`
- `makefile`: add `-I src/links` to `CXXFLAGS`, `src/links/*.cpp` to `SRCS`, compile rule for `build/links/`, `$(BUILDDIR)/links` mkdir target

### Object link integration

- `UdpTesterPObj`: new `LINK` INI key — when set, registers as `ROLE_MASTER` of the named `PduLink` during IDLE→ACTIVE transition; `INTERVAL_MS=0` disables self-send timer (object becomes a pure UDP carrier); `onSendPDU(data, len)` sends the PDU payload as a UDP datagram to `DST_IP:DST_PORT` and increments `packetsSent`; `onRecv()` forwards received UDP datagrams back to the link peer via `pduLink_->sendPDU(this, buf, n)`; link unregistered in `closeSocket()` (destructor); implements `IPduReceiver`
- `PktBertPObj`: new `INTERVAL_MS` INI key (default 1000 ms, min 1) controls BERT timer interval replacing the hardcoded 1 s; new `LINK` INI key — when set, registers as `ROLE_SLAVE` during IDLE→ACTIVE transition; in link mode `onTimer()` sends the PRBS packet via `pduLink_->sendPDU()` to the ROLE_MASTER (skips silently if link not UP yet); `onSendPDU(data, len)` receives the returned UDP payload and calls `receivePacket()` for PRBS verification; standalone loopback with `PACKET_LOSS_PPM` drop simulation retained when no LINK is configured; link unregistered in destructor; implements `IPduReceiver`
- Update `test/xcespproc_alone.ini` with two independent `PktBert↔UdpTester` link setups (`link001`, `link002`) for out-of-the-box loopback BERT testing
- Add commented link-mode example to `test/xcespproc.ini`

## 0.0.11

- Add `XCespProc::removeProcObject(name)` — thread-safe removal of a registered `ProcObject` by name
- Removal is deferred: the main thread queues the name in `pendingRemovals_` (protected by `procMutex_`); the processing thread drains the queue at the top of `processingTick()` before batch collection, so no raw pointer in a live batch is ever invalidated
- Returns `true` if the object was found (and queued), `false` if no match exists
- Add two extra `[object.3]` / `[object.4]` `PktBert` entries to `test/xcespproc_alone.ini`

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
