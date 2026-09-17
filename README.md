> [!CAUTION]
> **Do not use this on Warmane.** Their anti-cheat flags performance injectors and
> memory optimization tools as illegal software regardless of intent, and the
> result is a permanent ban on your account.

> [!WARNING]
> **On WoW Circle the DLL gets you disconnected** - Turning on **No Client Patches** in the launcher stops it, and
> also turns every optimization off.   
Or use the `!LuaBoost` addon without the DLL.

# wow_optimize

Performance optimization DLL for World of Warcraft 3.3.5a (WotLK)
Author: SUPREMATIST

**[Download Latest Pre-compiled Release](https://github.com/suprepupre/wow-optimize/releases/latest)**

wow_optimize improves WoW 3.3.5a at the engine and runtime level: memory allocation, Lua VM behavior, Lua library fast paths, timers, file I/O, networking, heap fragmentation, lock contention, the 16-year combat log bug fix, and other low-level bottlenecks.

The current public build is focused on real frametime stability, long-session smoothness, addon-heavy gameplay, and lower Lua/runtime overhead while keeping historically unsafe features disabled.

> Disclaimer: This project is provided as-is for educational purposes. DLL injection may violate the Terms of Service of private servers. Use at your own risk.

---

## Table of Contents
* [What's New in v3.19.3](#whats-new-in-v3193)
* [What's New in v3.19.2](#whats-new-in-v3192)
* [Send me your log](#send-me-your-log)
  * [Measuring rather than reporting](#if-you-want-to-measure-something-rather-than-report-a-bug)
* [Reviews & Acknowledgments](#reviews)
* [Current Feature Set](#current-feature-set)
* [Installation](#installation)
* [Compatibility & Setup](#compatibility--setup)
* [Multi-client Support](#multi-client-support)
* [macOS / Apple Silicon (WoWSilicon)](#macos--apple-silicon-wowsilicon)
* [Building](#building)
* [Core Architecture](#core-architecture)
* [Troubleshooting & Diagnostics](#troubleshooting)

---

## What's New in v3.19.3

### Fixed

* **Entering the world with ReShade loaded.** A hook this DLL placed in front of
  `InitializeCriticalSection` sat in front of every module in the process, not
  just the game, and it kept a player with ReShade at the loading screen. It has
  its own switch now, Critical Section Hook (All Modules), and that switch is off:
  nothing has ever measured a gain from it.
* **Every other hook on a Windows or CRT export answers the game only.** Thirty
  of them cached or rewrote answers - timers, window rectangles, registry reads,
  string conversions - for ReShade, DXVK, overlays and drivers as well. Each one
  now checks who called it and hands any other module the real function. The
  switch is System Hooks: Game Only, default on.
* **MAX PERFORMANCE no longer ticks switches that are not proven.** It set 113 of
  139, including all 48 marked unproven and the critical-section hook above. It
  now leaves every unproven switch at its own default.
* **A normal quit no longer logs an access violation.** The closing report asked
  the Lua state for its memory after the game had already freed it, and the
  exception that followed reached testers as a crash that had not happened.
* **Two clients from one game folder.** They opened the same log file and wrote
  over each other for the whole session; the second one also ran with no
  compiled-script store at all. Each takes the next free name now.
* **The Lua collector no longer stops for the rest of the session after one
  `/reload`.** Manual stepping and the 300 MB emergency collection both hung off
  a flag that a UI reload cleared and nothing set again. A tester session sat at
  257 MB of Lua memory with that protection switched off and no way to know.

### Faster

* **The game's string hash is finally being replaced.** The replacement ran
  alongside the original on every call and then threw its own answer away, so it
  was strictly slower than the function it replaced. It now verifies for 4096
  calls and takes over: a 3.5-hour session answered 102,613,258 of 102,617,562
  calls, with zero disagreements.
* **Eight of the hottest hooks lost their exception frame.** Each built an SEH
  frame and a stack cookie on every call - up to 405 million calls a session -
  to guard a fallback that reads the same bytes. They now run guarded until a
  million calls have passed without catching anything, then without.
* **New SSE2 replacements**, each off by default and each checking itself against
  the game's own routine before it is used: particle vertex fill, UI batch fill,
  ray against triangle (measured 1.89x), collision ray outcode (16.43x),
  quaternion unpack (7.33x).

### Reports

A bug report is only worth the round trip if the log answers the question.

* **The flight recorder keeps the frame you marked.** Every automatic mark in a
  tester session printed the 120 frames before the hitch and stopped short of the
  hitch itself. It now keeps the frames nearest the mark, and the mark waits for
  the recorder to have that frame.
* **A slow frame says what was in it.** Only rare events were traced, so 63 of 65
  spikes in one session read "nothing traced in this window". A Lua compile over
  20 ms and a change in address-space pressure are traced now: the same session
  had a 156 ms compile of a WeakAuras saved-variables file with nothing
  connecting it to a frame.
* **Two modules that had never printed a number in any session now report.** The
  packet-field fast path and the string-hash replacement printed their counters
  only from a shutdown path this process never reaches.
* **A crash dump says which build it came from.** The version in the binary said
  3.19.1 on every 3.19.2 build, which is the one field a dump without a log
  carries.
* **The report measures all of itself.** It costs up to 110 ms of the main thread
  every five minutes and its own accounting covered 8 of those.

## What's New in v3.19.2

### New

* **The launcher is two presets and a switch for bug reports.** MAX PERFORMANCE,
  DEFAULT and EVERYTHING OFF set every switch at once. Save, load and copy-for-
  the-dev move a whole configuration between machines. LOGGING: NORMAL / FULL
  turns on the recorders when something is wrong and off again when it is not.

  Every switch carries a mark and sits under a heading that says what the run
  below it is for: makes it faster, not proven yet, stability and fixes, logging,
  diagnostics that cost frames, changes how it looks or sounds, tried and didn't
  help. Hover any of them to read what was measured.

  MAX PERFORMANCE turns on everything that makes the game faster and leaves off
  everything that only measures it, buys frames by changing how the game looks,
  or was measured against the client and lost. Those are still yours to tick.
* **A bug report is legible from its first screen.** Every report opens with
  `[Wrong]`: the modules that were asked to run and did not, each with the line
  it printed. A switch you left off is not counted there.
* **Loading screens say where they went.** A load report ends with the addresses
  the main thread was actually in during that load, so the time that is neither
  reading, writing nor compiling has a name on it. Needs LOGGING: FULL.
* **Reuse Compiled Scripts Between Sessions.** A measured loading screen spends
  2128 ms inside the game's Lua compiler. Only 260 ms of that is text the session
  had already compiled, which is what Reuse Compiled Scripts removes. The rest is
  text this session had never seen - and saw the last time the game ran. This
  writes the compiled form to `Cache\wow_optimize_bytecode.bin` and reads it back
  on the next launch. Off by default.

  The game can write that form and has no code to read it back, so the reading is
  ours. Every script rebuilt from the file is compared against a real compile of
  the same text, field by field, into every nested function, each constant by
  type, value and addon ownership. That runs for the first 2000 of them and one
  in every 256 after, and the whole store switches off for good the first time
  two of them differ. The file is discarded whenever Wow.exe changes.
* **Model Animation Stride** holds a distant model's skeleton for a frame instead
  of re-solving every bone. Its materials, particles and attached items keep
  animating, and nothing within 45 yards is ever held. The animation family is
  about a fifth of the frame. Off by default.
* **M2 Matrix Slot Copy (SSE2)** replaces three blocks in the model animation
  update that move matrices one float at a time. No arithmetic, so the bytes
  written are the bytes read. Off by default.
* **Draw Call Merging** was built, measured and removed. 2.4% of 293 million draw
  calls could be merged, merging exactly those worked, and the frame rate fell.
  The census keeps its switch.

### Faster

* **Fourteen of the twenty Direct3D hooks stay out of the vtable.** Two of them
  skip redundant work and are kept, along with the four the shadow fix and the
  device lifecycle need. The rest could only count, and what they counted has
  been answered on two clients: 430 million render states, 601 million sampler
  states and 1.2 billion texture binds, none of them redundant. That is about
  five thousand fewer detours a frame. Draw Call Census puts them all back when
  you want the numbers.
* **string.match decides on the pattern before it reads the string.** A pattern
  none of the fast paths handles goes straight to the game, so the subject is no
  longer walked byte by byte, up to four kilobytes of it, to reach a comparison
  that was never going to match.
* **Batch the Game's File Writes** and **Reuse Compiled Scripts** are on by
  default. One measured loading screen ran 1576 ms with 49 ms of it reading
  files; another spent 2470 ms inside 593,557 nine-byte writes, and 2128 ms
  inside the Lua compiler.
* Loading screens report how much of themselves went into compiling Lua, split
  into source seen for the first time and source compiled again.

### Fixed

* **The two animation throttles are marked as tried and failed.** Model Animation
  Stride and Spread Model Animation both hold a model's skeleton and choose what
  to hold by distance, and both were reported stuttering visibly on environment
  animation: the lava in Ironforge for one, the Deeprun Tram tunnels for the
  other. Distance is a poor stand-in for whether a held skeleton is seen, because
  a large animation fills the screen at any range. Neither has produced a
  measured frame gain to weigh against it.
* **Three megabytes of address space came back.** The log ring reserved four
  megabytes for lines that are measured at about a hundred and fifty characters.
  It is sized to what is actually written now. That space sits in the low 2GB,
  which is where the game allocates from, and where running out is what garbles
  SavedVariables names.
* **The matrix hook counter printed a negative number.** Fifteen counters were
  signed 32-bit and the matrix multiply takes about four thousand calls a frame,
  so one of them ran out inside the second hour and took the total with it. The
  count that overflows carries a wrap counter now and the total is summed
  without one.
* **The fault list at the top of each report counted its own output** and called
  two deliberate decisions failures. It also reported an unresolved draw entry
  point on machines where Draw Call Census was simply switched off.
* **The periodic report says which part of it is slow.** It pauses the main
  thread for a tenth of a second on some machines, and could only report that
  the cost was itself. Each of its seventy-odd sections is timed and the slowest
  are named.
* **GetItemInfo caching says why a miss missed** - an empty slot, a different
  item in the slot, or the game returning nothing because the item is not in its
  own cache yet. Only the middle one is a cache that is too small.
* **Shadows that did not refresh and flickered**, reported by prince [SANC] and
  Sicsoo. Setting a render target resets the viewport, and the render state cache
  skipped the `SetViewport` that put it back, so the shadow pass drew into the
  wrong rectangle. On by default, so this was everyone.
* **Wrong vertex layout after a vertex declaration.** Setting a declaration clears
  the FVF; the FVF cache kept skipping the call that put it back.
* **The quality governor pulled your view distance down when you zoned.** It
  read the end of a loading screen as slow gameplay, halved particle density and
  cut farclip, then put both back a minute later. It ignores loading screens now
  and waits for real frames before deciding anything. Off by default.
* **The quality governor reports every interval**, with the values it is holding
  against your own.
* **Two threads could write into one log buffer.** A line would stop mid-message
  with another thread's whole line inside it. A ring slot is claimed before it is
  written now, and lines dropped to a full ring are counted in the report.
* **Two worker threads started every session for a queue nothing writes to.**
  Async Worker Pool checks whether an offload path is actually built in before
  starting them. Each thread reserved a megabyte of stack in the low 2GB, which
  is the half this client allocates from.
* **The largest scripts reach the disk store.** The megabyte cap on the in-memory
  copy of the source sat above the disk lookup, so GlobalStrings.lua, ChatFrame
  and the rest were captured and could never be read back - and those are where
  skipping a compile is worth ten milliseconds rather than twenty microseconds.
* **Reuse Compiled Scripts Between Sessions understands addon ownership.** A
  constant's ownership belongs to the compile that first created it and the
  game's own dump format does not record it, so only scripts with no ownership on
  any constant are kept, and they are served only into a context that has none.
  The report counts what that leaves behind.
* **The draw call census had never installed.** Its hooks came from a module
  compiled out months ago, so ticking that box measured nothing.
* **The animation census stood down whenever Animation LOD was on**, so neither of
  the two numbers ever arrived. It counts from inside the other one now.
* **Thirteen modules counted on hot paths and could not print the number**, seven
  of them counting crashes they had averted.
* **Five launcher options turned on features this build does not contain**, and two
  more gated an install that always fails. All gone.
* **Four of the six render state filters skipped nothing at all** over 206
  million measured calls. They only count now.
* **A locked 64-bit instruction on every Lua allocation the game makes.**
* **The primitive count in the log went down between reports** and ended at 0.8 per
  draw call, which cannot happen. It was a 32-bit counter holding a five billion
  total.
---

## Send me your log

This is the single most useful thing anyone does for this project, and it costs
you about thirty seconds.

After playing, attach `Logs\wow_optimize_<date>_<time>.log` to an
[issue](https://github.com/suprepupre/wow-optimize/issues) or drop it in
[Discussions](https://github.com/suprepupre/wow-optimize/discussions). Nothing
needs to be wrong for a log to be worth sending — a session where everything
worked is just as informative as one where it did not.

**What is in it and why it matters.** The log ends with a frame-time distribution,
a note on whether your client was actually CPU-bound or waiting on the GPU, a
profile of where time went, and which features did work rather than merely being
switched on. Together those answer questions that cannot be answered from here:

- Which optimizations pay off on hardware and addon sets I do not have. Several
  features have been removed after logs showed they did nothing, and a few were
  fixed after a log showed them doing the wrong thing.
- Where the remaining time actually goes. One log turned out to be 94% idle,
  which meant no CPU-side work could have helped that player at all; another was
  genuinely CPU-bound and pointed straight at the hot code.
- Whether a bug is mine. A log carries the exact build hash, so a report can be
  matched to source instead of guessed at.

Please send the whole file rather than an excerpt, and do not trim the first
lines — that is where the build hash and your settings are. If you are reporting
a bug, say what you saw and roughly when; the log has timestamps and the two
together usually locate it.

If you would rather not share it publicly, that is fine — say so in an issue.

### If you want to measure something rather than report a bug

Comparing two sessions compares two different evenings. One session that
alternates a feature on and off compares the same zone, the same addons and the
same machine against itself.

Tick **A/B Test a Feature** under General, and tick the features you want
compared. The harness measures a feature that is switched on, because a feature
registers with it at the moment it installs. These are the ones it can measure:

> UI Layout Relink Shortcut, Model Draw Order Key Cache, Lua Pool Shortcuts,
> Table Lookup Dispatch (SSE2), Bone Rotation Maths (SSE2), Bone Rotation
> Unpack (SSE2), Bone Movement Track (SSE2), Bone Matrix Upload (SSE2),
> Visibility Box Test (SSE2), Box Overlap Test (SSE2), Line-of-Sight Box Test
> (SSE2), M2 Matrix SSE2, M2 Matrix Slot Copy (SSE2), Model Animation Stride,
> Fast SSE2 Memory Clear, SSE2 String Compare, Lua VM: stop the automatic GC,
> and Matrix-Vector SSE2.

Then play somewhere the processor is busy: a raid, a battleground, a crowded
city. Standing in a field the game waits on the graphics card, a saving inside
the frame changes no frame time, and the report says so instead of giving you
numbers.

Each subject is alternated on its own, four on/off pairs of twenty seconds, so
it spends about two minutes and forty seconds on one before moving to the next.
Play for longer than one pass over everything you ticked, and send the log. The
report says, per feature, when there were too few turns for the number to mean
anything.

To spend the whole session on one feature, open `WTF\wow_opt.ini` and put its
ini key under `[General]`:

```ini
AbTestSubject=LayoutRelinkFast
```

and make sure that feature is switched on too. The switch decides whether it
installs; this decides when it does its work. If the name is wrong the report
lists the ones it would have accepted.

`MatrixVectorSse2` is in the list to check the instrument: it is known to be
slower than the code it replaces, so if a report calls it faster, the
measurement is what is wrong.

For counters instead of a comparison - draw calls and how many of them could be
merged, how much Lua the game compiles twice, how long the horizon scans are,
how many tiny file writes there are, and where the main thread is during a
loading screen - press **LOGGING: FULL** and leave A/B Test off. Those cost
frames to collect, which is why they do not share a session with a test that
compares frame times. Twenty minutes of whatever you normally do is enough.
---

## Reviews

<details>
<summary><b>Click to expand community reviews and stability testers list</b></summary>

See what other players say: [Reviews and Testimonials](https://github.com/suprepupre/wow-optimize/discussions/10)

### Stability Testing Team


This project wouldn't exist without the community. Every crash report, every bisection test, every "hey this broke my addon" message directly shaped the release. 

Special thanks to:
Morbent, Darkmoore, Ethodeus, Billy Hoyle, tuan, NoGoodLife, feh_dois, David (`_oldq`), Keoo, UNOB, DarkRockDemon, Raymond, Vandal, Mantork, Falcon, Muus, szopachink17, Shandrax, pathetic-lynx, txtsd, Signalborn Soulweaver, Sicsoo, kojekude, Houmbro

### Code contributions

- **[athei](https://github.com/athei)** (Alexander Theissen) — the macOS cross-compile
  toolchain (`clang-cl` + `lld-link` + `xwin`, [#20](https://github.com/suprepupre/wow-optimize/pull/20)),
  reliable `!LuaBoost` detection across `lua_State` swaps and fast logins
  ([#19](https://github.com/suprepupre/wow-optimize/pull/19)), and the filter that stops
  `ClientExtensions.dll`'s anti-tamper probe from being reported as a crash
  ([#21](https://github.com/suprepupre/wow-optimize/pull/21)).
- **[anzz1](https://github.com/anzz1)** — VS2019 build fix
  ([#2](https://github.com/suprepupre/wow-optimize/pull/2)) and closing dangling thread
  handles ([#7](https://github.com/suprepupre/wow-optimize/pull/7)).
- **[POKOch](https://github.com/POKOch)** — selective rendering, spell visual blocking
  and API caching ([#12](https://github.com/suprepupre/wow-optimize/pull/12)).

### Testing

Every measured item in these notes came out of a log somebody sent in.

- **prince** — Chinese client under DXVK; the WeakAuras talent-switch bug, the
  loading-screen stall, and the crash report that finally pinned an access
  violation to one instruction.
- **[txtsd](https://github.com/txtsd)** — raids on ChromieCraft; the memory growth
  and freeze reports, and the request for per-addon profiling that turned into the
  addon CPU profiler and the Lua compile census.
- **Signalborn Soulweaver**, **Morbent**, **Sicsoo** — early 3.18 logs.
- **Doc.James** — the zone-change stall, with three sessions that made it
  reproducible.
- **kojekude** — boss voice lines going missing in raids and dungeons while every
  other sound kept working, which turned out to be the sound coalescer returning
  "played fine" for sounds it had dropped.
- **nobus** — three sessions with warrior stance-swap crashes, carrying a second
  independent reproduction of a null-callback crash in the client's device
  callback list.
- **[biship](https://github.com/suprepupre/wow-optimize/issues/50)** — read the
  timing switch's code and reported that it gated twelve unrelated things and
  described none of them.

</details>

---

## Current Feature Set

<details>
<summary><b>Click to expand full optimized feature list (Memory, Lua VM, Math, Network, Async, I/O)</b></summary>

### Memory and allocator
- **Large-allocation mimalloc redirect** *(opt-in, default off)* — only main-thread allocations `>= 1 MB` are routed to mimalloc, and only if the returned pointer sits below 2 GB; everything smaller (including all network buffers) and every background-thread allocation stays on WoW's CRT. This is the conservative replacement for the old redirect-everything version, which was removed in v3.16.3 for destabilizing Winsock and breaking connections. Enable in the launcher and confirm you can still connect. `free`/`realloc`/`_msize`/`_recalloc` route mimalloc-owned blocks by region check so nothing is freed on the wrong heap.
- **Adaptive purge delay + memory-pressure governor** — purge aggression scales with VA pressure; forced `mi_collect` under critical pressure (now driven from the main-thread maintenance tick, not a background thread)
- **Direct mimalloc use** — subsystems (aligned-alloc cache, async I/O buffers, prefetch, etc.) call mimalloc directly regardless of the redirect toggle
- Lua allocator replacement *(disabled — corrupted pointers during login)*
- WoW `free`-wrapper fast path (calls WoW's own `free`, skips a redundant `_msize` heap-walk)
- Lua string table pre-sizing to reduce hash resize spikes
- Low Fragmentation Heap (LFH) enabled for process heap and new heaps
- **Deferred Heap Compactor** — defers process heap compaction during loading screens to run once upon screen closure, preventing character login freezes.

### Lua runtime
- adaptive manual Lua GC
- 4-tier GC stepping:
  - normal
  - combat
  - idle
  - loading
- GC step sync with !LuaBoost
- safe Lua stats export to addon
- Lua reload detection and clean reinitialization
- **Reuse Compiled Scripts** *(off by default, experimental)* — keeps the compiled form of a Lua chunk and hands it back when the client compiles the same source under the same name again, so the parse does not run. The client still builds the function object, its environment and its addon ownership. Nothing is kept until a chunk has been compiled twice. `UI_Lua/LuaProtoCache`

### WoW API result cache
- `GetItemInfo` - 8192-slot cache, Direct Memory Access *(disabled - breaks Aux / WCollections / ElvUI)*
- `GetSpellInfo` - disabled (icon corruption, crashes on relog)

### Lua internal caches
- `luaH_getstr` - generation-guarded table string-key lookup cache (8192-slot, SEH-protected)
- `luaH_getstr` inline v2 - safe bucket-index cache with SSE2 prefetch (16384 entries)
- `lua_rawgeti` inline v2 - safe array direct + bucket-index cache (8192 entries)

### Lua fast paths
- Phase 1:
  - `string.format`
- Phase 2 (safe, Lua API based) - **ENABLED**:
  - `string.find` (plain mode)
  - `string.match` (safe partial fast path)
  - `string.rep`
  - `string.gsub` (plain-literal fast path)
  - `type`
  - `math.floor`
  - `math.ceil`
  - `math.abs`
  - `math.max` (2 args)
  - `math.min` (2 args)
  - `math.random`
  - `math.sqrt`
  - `math.fmod`
  - `math.modf`
  - `string.len`
  - `string.byte`
  - `string.char`
  - `tostring`
  - `tonumber`
  - `select`
  - `rawequal`
  - `string.sub`
  - `string.lower`
  - `string.upper`
  - `table.concat` (disabled - direct RawTValue* stack writes caused hangs)
  - `unpack` (disabled - direct RawTValue* stack writes caused hangs)
  - `ipairs` (disabled - closure factory incompatible with WoW iterator pattern)
- C-global fast paths - **ENABLED**:
  - `strjoin`
  - `strtrim`
  - `strsplit`

### Lua VM internals
- **UI Method Object Lookup** *(off by default, experimental)* — the object fetch that starts every one of 674 Lua calls into a frame (`sub_4A81B0`). Four script-engine calls and a push/pop replaced by direct reads; the addon-ownership propagation `lua_rawgeti` performs is reproduced rather than skipped, and anything unusual is handed back to the client. `UI_Lua/LuaThisFast`
- `luaV_concat` and `luaS_newlstr` hooks disabled for public stability
- baseline-safe VM operation with zero overhead
- string table pre-sizing remains active to prevent rehash freezes

### Timers and frame pacing
- PreciseSleep on the main thread
- automatic single-client / multi-client timing behavior
- `GetTickCount` redirected to QPC-based timing
- `timeGetTime` redirected to the same QPC timeline
- QueryPerformanceCounter coalescing cache
- adaptive timer resolution
- hardcoded FPS cap raised from 200 to 999

### File I/O
- MPQ handle tracking
- retroactive MPQ handle scanner
- sequential-scan hints for MPQ access
- adaptive MPQ read-ahead cache
- skip `FlushFileBuffers` for tracked MPQ handles
- `GetFileAttributesA` cache
- `SetFilePointer` redirected to `SetFilePointerEx`

### Threading and synchronization
- SRWLOCK-based file cache locking
- main thread priority ABOVE_NORMAL
- ideal processor assignment
- process priority ABOVE_NORMAL
- CriticalSection spin count and spin-first entry path
- TLS-cached `GetCurrentThreadId` and pseudo-handle fast path

### Networking
- `TCP_NODELAY`
- immediate ACK frequency
- socket buffer tuning
- low-delay TOS
- keepalive (30s idle / 5s interval — tuned to keep NAT warm without dropping the connection on transient network jitter)

### Async loading and prefetching

Features that use worker threads and lock-free queues. Status reflects the current public-safe configuration; individual toggles live in `src/version.h`.

- **Async spell data prefetching** - predictive spell data loading before cast completes, reduces spell cast lag, worker thread with lock-free queue (4096 entries) and cache (4096 entries) *(disabled — placeholder worker with no producers)*
- **Multithreaded addon dispatcher** - parallelizes addon OnUpdate callbacks across worker thread pool (4 threads), reduces main thread CPU in addon-heavy setups, batch processing with lock-free queue (8192 entries) *(disabled - unsynchronized writes to WoW game state)*
- **Predictive MPQ prefetching** - tracks zone transitions and predicts next zone, prefetches textures/models/WMOs into OS cache before teleport, reduces zone loading stutters, worker thread pool (2 threads) with lock-free queue (2048 entries) *(enabled)*
- **Multithreaded combat log parser** - offloads combat log parsing to worker thread, reduces main thread CPU in raids, lock-free queue with async processing *(disabled — placeholder worker with no producers)*
- **Sound prefetching** - predicts and prefetches sound files based on spell casts, zone transitions, combat state, worker thread pool (2 threads) with lock-free queue (1024 entries) *(disabled — placeholder worker with no producers)*
- **Async quest/achievement loading** - async quest log and achievement data loading, worker thread with lock-free queue (512 entries) *(disabled — placeholder worker with no producers)*
- **Multithreaded nameplate renderer** - offloads nameplate rendering to worker threads, reduces main thread CPU in 25-man raids, priority system (Target > Focus > Nearby > Distant) *(disabled - unsynchronized writes to WoW game state)*
- **Model/M2 caching** - synchronous LRU cache (1024 entries) for loaded models, eliminates redundant model loading *(enabled)*
- **Asynchronous Texture Hot-Swapping & Storm VFS** *(enabled)* — detours TexCreateBLP to immediately return a placeholder white texture, background loads the real BLP data, and hot-swaps the underlying Direct3D 9 texture pointer and properties during frame boundaries (OnFrame) without visual stutters.
- **Asynchronous Terrain Mesh Loader & Collision Decoupler** *(enabled)* — offloads ADT terrain file loading and geometry compiling to background threads, decoupling collision checks via player Z height fallback, and detouring CMapGrid::Update to prevent character-select crashes.
- **RCU Client Object Manager Traverser** *(enabled)* — replaces linear linked-list entity traversals with lock-free atomic pointer flat mirror arrays updated on link/unlink events.
- **Addon dispatcher** - lightweight event-driven addon update dispatch *(enabled)*

### Other runtime optimizations
- **Spread Model Animation** *(off by default, experimental)* — posing model skeletons measured at 3.68 ms of a 24.5 ms frame in raid content. Below 96 models on screen nothing changes; above it each model's pose refreshes every 2nd to 4th frame, never slower than a quarter of the frame rate, and never before its first pose. Cannot make animations run slow: the client derives animation time from a clock rather than by counting frames. `Graphics_Sound/AnimLod`
- combat log optimizer - **fixes the 16-year combat log bug** (log retention increased from 300s to 1800s, events no longer lost during extended sessions)
- `CompareStringA` fast ASCII path
- `MultiByteToWideChar` / `WideCharToMultiByte` - SSE2 ASCII fast path (bypasses NLS for pure-ASCII strings on ASCII-compatible codepages)
- `lstrlenA` / `lstrlenW` fast path
- `OutputDebugStringA` no-op when no debugger
- fast `IsBadReadPtr` / `IsBadWritePtr`
- periodic stats dump
- CRT `pow()` integer fast-path (x^2=x*x, sqrt, etc.)
- CRT `strstr` SSE2 Boyer-Moore-Horspool

### SSE2 string/memory fast paths (WoW-internal, active)
Replacements for WoW's own statically-linked CRT routines at verified addresses:
- WoW `strlen` (sub_76EE30) - 16-byte-aligned SSE2 scan, page-safe
- WoW `memset` (0x40BB80, 1108 callers) - full SSE2 + non-temporal ≥2 MB
- WoW `memcpy` (0x40CB10, 719 callers) - SSE2 16–255 B + non-temporal ≥256 KB, overlap-safe
- WoW `_strnicmp` (0x76E780, 1013 callers) - SSE2 ASCII case-insensitive compare
- `strstr` - SSE2 Boyer–Moore–Horspool
- `MultiByteToWideChar` / `WideCharToMultiByte` - SSE2 ASCII fast path

### SSE2 math and geometry (WoW-internal, active)
- SSE2 4×4 matrix multiply — `CMatrix::operator*` (0x4C1F00)
- SSE2 matrix-vector transforms — 3D point × 4x4 matrix (0x4C21B0), 4D vector × 4x4 matrix (0x4C2270), in-place point × 4x4 (0x4C2300)
- SSE2 `C3Vector::Normalize` — 0x4C3420 + 0x4C3600 (full-precision `sqrtss`/`divss`, engine guards replicated)
- SSE2 `CMatrix::Transpose` — 0x4C23D0 (`_MM_TRANSPOSE4_PS`, bit-identical)
- **SSE2 collision box test** *(off by default, experimental)* — the AABB outcode classification in `sub_7C7230`, 3.8% of main-thread time in a corrected profile. Six x87 comparisons per vertex become six packed comparisons per four vertices. Bit-exact: the bounds are plain floats with no arithmetic applied. `Graphics_Sound/CollisionOutcode`
- SSE2 frustum point culling — `CFrustum::IsPointVisible` (0x983D70)
- SSE2 Möller-Trumbore ray-triangle intersection — 32-bit indices (0x9836B0), 16-bit indices (0x983490)
- SSE2 frustum AABB-vs-4-planes cull
- SSE2 BGRA↔ARGB batch swap, premultiplied alpha
- Network GUID SSE2 unpacking — `CDataStore::GetWowGUID` (0x76DC20)
- SSE2 quaternion normalize *(enabled — normalizes in double precision)*
- Particle simulation throttling — `CParticleEmitter::SimulateParticle` (0x981D40) *(disabled — 0x981D40 is the particle spawn/init routine, not a skippable advance; throttling it left particles uninitialized, rendering as colored flashes)*

> The generic msvcrt CRT mem/char SSE2 paths (`crt_mem_fastpath`, `crt_char_fast`) are **disabled** — WoW links its CRT statically, so hooking msvcrt exports had little effect and risked VA exhaustion.

### Lua Event Coalescing *(disabled)*
- Buffers and deduplicates high-frequency UI events per frame
- **Disabled**: suppressing and re-emitting events a frame later changes event timing/ordering and was unvalidated across the in-world → glue teardown where char-switch crashes occur. Stability outranks the dedup win until it can be confirmed in-game.
- The `FrameScript_SignalEvent` (0x81AC90) detour it used to own now belongs to the loading/combat state detector, which is always installed. The dedup queue is a consumer of that detour, so it stays switched off without taking the state tracking down with it.

### Kernel-call caches (38 hooks)
Batch 1-8: `GetSystemTimeAsFileTime` (QPC-based 1ms refresh), `GetACP`, `GetUserDefaultLangID`, `GetProcessHeap`, `CharUpperA/W`, `CharLowerA/W`, `MapVirtualKeyA`, `GetThreadPriority`

Batch 11-20: `GetOEMCP`, `GetDoubleClickTime`, `GetCursorPos`, `GetSysColor`, `GetCaretBlinkTime`, `IsWindow`, `GetDesktopWindow`, `GetFocus`

Batch 21-26: `GetTickCount64` (QPC-backed), `ShowCursor`, `GetVersionExA`, `GetSystemMetrics`, `IsDebuggerPresent` (no-op), `GetSystemInfo`, `RegQueryValueExA`

Batch 31-38: `GetCurrentProcess`, `GetCurrentThread`, `GetCPInfo` and related kernel caches

### Loading screen optimization
- Loading state is detected natively from the client's own event stream (`PLAYER_LEAVING_WORLD` → `PLAYER_ENTERING_WORLD`), so it works with or without the `!LuaBoost` addon. Many subsystems use it as a "bypass this while the world is loading" gate: deferred field updates, the DBC lookup cache, the Lua opcache and the texture unload queue
- Dynamic VA arena: reserves 256MB during loading, releases after. The reservation is skipped on HD clients (>500MB working set), but the loading state itself is always published
- A watchdog force-exits the loading state after 30s, so a missed end event can never pin the process in loading mode
- Sleep hook: bulk Sleep for waits >16ms (less CPU during idle)

### VA Arena (Virtual Address Arena)
- 512MB high-address reserved arena with `MEM_TOP_DOWN`
- Wow.exe caller filtering - only services allocations from WoW executable code
- span tracking for correct multi-page allocation/deallocation
- proper `MEM_DECOMMIT` / `MEM_RELEASE` behavior
- reduces 32-bit address space fragmentation from large WoW allocations

</details>

---

## What Improves In Practice

### You will notice
- smoother frametimes
- fewer random microstutters
- better long-session smoothness
- lower Lua overhead in addon-heavy gameplay
- less allocator fragmentation over time
- better responsiveness during heavy UI and addon workloads
- faster zone transitions and teleports 
- reduced spell cast lag 
- smoother addon-heavy gameplay 

### You may notice
- slightly better minimum FPS in cities and raids
- less "client gets heavier after long play"
- smoother loading transitions
- faster Lua-heavy addon behavior

### You should not expect
- a giant average FPS increase from one hook alone
- visual changes
- magical fixes for broken addons
- gameplay automation

This is an engine and runtime optimization DLL, not a UI overhaul.

---

## Recommended Combo

For best results, use wow_optimize together with [!LuaBoost](https://github.com/suprepupre/LuaBoost).

| Layer | Tool | Purpose |
|------|------|---------| 
| Engine / C / Win32 | `wow_optimize.dll` | allocator, Lua VM, timers, file I/O, networking, runtime overhead reduction |
| Lua / Addons | `!LuaBoost` | GC control, loading helpers, table pool, update dispatcher, diagnostics |

---

## Installation

### Option A - Launcher Dashboard (Recommended)
Copy into your WoW folder:
- `wow_optimize_launcher.exe`
- `version.dll`
- `wow_optimize.dll`

Then run `wow_optimize_launcher.exe` to configure settings, load/save profiles, and click **LAUNCH WOW**.

### Option B - Proxy load (Automatic)
Copy into your WoW folder:
- `version.dll`
- `wow_optimize.dll`

Then launch WoW normally. The proxy DLL will automatically load the optimizer with default settings.

### Option C - Standalone Loader
Copy:
- `wow_optimize.dll`
- your injector

Then inject after WoW starts.

---

## Compatibility & Setup

### Supported Clients & Private Servers
The optimization suite is compatible with any standard or customized WotLK 3.3.5a client (build 12340), including private servers using custom executables:
* **Warmane** (Icecrown, Lordaeron, Onyxia) — **STRICTLY PROHIBITED (WILL RESULT IN A PERMANENT BAN)**
* **Project Ascension** (supporting custom `Ascension.exe` launches)
* **WoW Circle** - **On WoW Circle the DLL gets you disconnected** - Turning on **No Client Patches** in the launcher stops it, and
also turns every optimization off.   
Or use the `!LuaBoost` addon without the DLL.
* **EZ WoW**
* **WoW Sirus** (supporting `Sirus.exe` or custom `run.exe` launches)
* **UWow** / **Firestorm** (supporting `run.exe` launches)
* **ChromieCraft 3.3.5a**


1. Install the `!LuaBoost` addon into `Interface\AddOns\`.
2. **Disable conflicting addons:** Remove or disable any third-party GC optimizers (`GarbageProtector`, `GarbageCollector`, `SmartGC`, etc.) and combat log fixes (`CombatLogFix`, etc.). The DLL handles these natively; running both causes duplicate hooks, memory corruption, or crashes.
3. **Adjust damage meter settings:** Disable built-in garbage collection / memory optimization in your meter addons to prevent double-stepping the Lua GC.
   - **Skada:**
     ![Skada Settings](images/image1_Skada_settings.png)
   - **DBM:**
     ![DBM Settings](images/image2_DBM_settings.png)

---

## Multi-client Support

wow_optimize automatically detects when multiple WoW instances are running.

- Single client:
  - precise sleep
  - 0.5 ms timer
- Multi-client:
  - yield-based sleep
  - 1.0 ms timer
  - reduced working set targets

This reduces CPU pressure compared to forcing aggressive single-client timing on all clients.

---

## macOS / Apple Silicon (WoWSilicon)

wow_optimize works on macOS via [WoWSilicon](https://github.com/WoWSilicon/WoWSilicon), which runs WoW 3.3.5a natively on Apple Silicon using Wine + [rosettax87](https://github.com/athei/x87sidecar) translation.

### DLL load order

In `dlls.txt`, `winerosetta.dll` must be loaded before `libSiliconPatch.dll`:

```
mods/winerosetta.dll
mods/libSiliconPatch.dll
mods/wow_optimize.dll
```

Swapping the first two causes a rosetta error. Without wow_optimize the order does not matter, but with it loaded the translation layer must initialize before any hooks are installed.

### Testing credits

macOS/WoWSilicon compatibility was tested by **David** (`_oldq`).

---

## Building

The build target is always Win32 i386, but you can produce it from either a Windows host (native MSVC) or a macOS host (cross-compile). Both paths drive the same `CMakeLists.txt` and ship binary-equivalent DLLs to within ~30 KB.

### Windows (native MSVC)

Requirements:
- Windows 10 or 11
- Visual Studio with the C++ desktop workload
- CMake
- Win32 / 32-bit build configuration

```bash
git clone https://github.com/suprepupre/wow-optimize.git
cd wow-optimize
build.bat
```

Output:
- `build\Release\wow_optimize.dll`
- `build\Release\version.dll`
- `build\Release\wow_loader.exe`
- `build\Release\wow_optimize_launcher.exe`

### macOS (cross-compile to Win32)

Requirements:
- macOS (Apple Silicon or Intel)
- Homebrew
- [xwin](https://github.com/Jake-Shadle/xwin) for the Windows SDK / MSVC CRT

One-time setup:
```bash
brew install llvm lld cmake ninja
xwin splat --output /opt/xwin
```

Build:
```bash
git clone https://github.com/suprepupre/wow-optimize.git
cd wow-optimize
make
```

Output:
- `build/wow_optimize.dll`
- `build/version.dll`
- `build/wow_loader.exe`

The Makefile drives `clang-cl` (Homebrew `llvm`) and `lld-link` (Homebrew `lld`) through the toolchain file in `cmake/toolchain-clang-msvc-x86.cmake`. `make verify` prints PE headers; `make clean` / `make rebuild` work as expected. Override `LLVM_DIR`, `LLD_DIR`, or `XWIN` on the command line if your paths differ.

---

## Core Architecture

<details>
<summary><b>Click to expand full modules listing</b></summary>

### Main modules
- `dllmain.cpp` - Win32 hooks, allocator, timers, file I/O, networking, threading, VA Arena
- `lua_optimize.cpp` - Lua VM allocator, adaptive GC (with frame-time scaling and VA-pressure override), Lua globals bridge
- `async_terrain_loader.cpp` / `async_terrain_loader.h` - Asynchronous ADT terrain loader, CMapGrid update safety, Z-coordinate collision fallback query.
- `rcu_obj_mgr.cpp` / `rcu_obj_mgr.h` - RCU client object manager traverser for lock-free entity enumeration.
- `lua_fastpath.cpp` - `string.format` and runtime-discovered Phase 2 hooks (24/27 functions)
- `lua_vm_engine.cpp` - Direct-threaded Lua VM interpreter with inline cache
- `lua_getstr_inline.cpp` - Safe bucket-index cache for luaH_getstr (16384 entries)
- `lua_rawgeti_inline.cpp` - Safe array-direct + bucket-index cache for lua_rawgeti (8192)
- `lua_pushnumber_fast.cpp` - Direct TValue stack write for lua_pushnumber
- `lua_gettable_safety.cpp` - TValue type validation crash fix
- `hooks_render.cpp` - 3-tier off-screen animation throttle, backbuffer LockRect skip
- `hooks_simd.cpp` - SSE2 matrix multiply, 4×4 matrix multiply, quaternion normalize, frustum AABB/point cull, ray-triangle intersection, matrix-vector transforms, particle simulation throttle, BGRA↔ARGB, premultiplied alpha
- `hooks_logic.cpp` - Combat text batching, UI layout cache, heartbeat filter, script cache
- `hooks_memory.cpp` - 64B-aligned slab allocator, 16384-entry GUID hash-table
- `hooks_async.cpp` - 2-thread worker pool, particle SSE2, ADT prefetch
- `event_coalescer.cpp` - Lua event coalescing via FrameScript_SignalEvent hook, per-frame deduplication
- `network_guid_sse2.cpp` - SSE2 branchless GUID unpacking for CDataStore::GetWowGUID
- `d3d9_state_manager.cpp` - 15-hook D3D9 vtable patcher + device-reset lifecycle tracker and DXVK implicit-resize handler (active)
- `dxvk_bridge.cpp` - DXVK / Vulkan-translation-layer detection (module, d3d9.dll metadata, env)
- `hot_patch.cpp` - 20 runtime hot-patch optimizations
- `infra_patch.cpp` - 50 infrastructure APIs (pools, caches, dedup, perfmon)
- `hook_prefetch.cpp` - 3 SSE2 prefetch hooks for cleanup/delete/reset paths
- `data_caches.cpp` - 10 game-data lookup caches (spell, M2, FMOD, DBC, etc.)
- `compute_caches.cpp` - 10 compute/transform caches (BZ2, vertex SSE2, regex ext, etc.)
- `crash_dumper.cpp` - Enhanced crash reporter with feature tracking + hook trace
- `lua_internals.cpp` - stable VM baseline (disabled unsafe hooks)
- `combatlog_optimize.cpp` - combat log retention and cleanup behavior
- `combatlog_mt.cpp` - multithreaded combat log parser
- `texture_async.cpp` - async texture loading with worker thread pool
- `spell_prefetch.cpp` - async spell data prefetching
- `addon_dispatcher.cpp` - multithreaded addon update dispatcher
- `model_async.cpp` - model/M2 caching
- `mpq_prefetch.cpp` - predictive MPQ prefetching
- `api_cache.cpp` - `GetItemInfo` cache
- `ui_cache.cpp` - disabled in public-safe build
- `version_proxy.cpp` - proxy loader
- `wow_loader.cpp` - standalone loader executable

</details>

---

## Troubleshooting

### Reporting a problem

Two things make a report actionable, and both are easy to get wrong:

1. **Send `Logs\wow_optimize_<date>_<time>.log`**, not `Logs\wow_optimize.log`. The second one is overwritten on every launch.
2. **Quit the game normally** — not `alt+F4` — after reproducing the problem, so the end of the log reaches disk.

If the game crashed, attach `Crashes\wow_crash_*.dmp` (or the text report written next to it under Wine) as well.

Every crash report, Lua error dump and stutter dump starts with an event trace — the state transitions leading up to the problem, newest first — which is usually the part that explains it:

```
Recent events:
    -    23ms  TID=900   LUA state swap (UI reload) - new VM settling
    - 27810ms  TID=900   LOADING begin (PLAYER_LEAVING_WORLD)
    -110351ms  TID=900   D3D9 device Reset (dev=0x0EB1AA90)
```

The startup banner reports the exact build the log came from (`v3.19.3 (build abc1234)`), so please don't trim the first lines.

If the complaint is stuttering rather than a crash, look for `slow frame` lines — each one names how far past your session's own median that frame ran, and what was happening during it:

```
[FrameBench] slow frame: 102.9 ms (16.3x the 6.30 ms median) - recent events:
```

| Problem | Solution |
|---------|----------|
| Proxy DLL doesn't load (no log file) | Use `wow_loader.exe`, or uncheck **"Disable fullscreen optimizations"** in `Wow.exe` properties:<br>![Wow.exe Properties](images/wow.exe_properties.png) |
| Antivirus flags the DLL | Hooking/injection tools often trigger false positives. Source is open for review. |
| `FATAL: MinHook initialization failed` | Another hook DLL is conflicting. Disable other injectors/overlays. |
| `ERROR: No CRT DLL found` | Non-standard WoW build detected. |
| Socket shows `fail` | Normal on some Windows versions - some network options require admin. |
| Damage meters still broken | Remove `CombatLogFix` or similar addons. Two fixers conflict. |
| No noticeable difference | Expected on high-end PCs with few addons. |
| `[UICache] DISABLED` | Non-standard WoW build - method table not found. |
| High CPU usage with multiple clients | Expected. Each client runs full optimization. Remove `version.dll` from secondary clients if needed. |
| "I use DXVK or Vulkan" | Fully supported. No D3D9 state-cache dependencies. |
| `Large pages: no permission` | Informational only — **not** a crash cause. Large pages are an optional TLB optimization (mimalloc 2&nbsp;MB OS pages); the DLL runs fine on normal 4&nbsp;KB pages without them. To enable them, see [Fixing `Large pages: no permission`](#fixing-large-pages-no-permission) below. |

### Fixing `Large pages: no permission`

<details>
<summary><b>Click to expand step-by-step setup guide</b></summary>

This log line means your Windows account does not hold the **Lock pages in memory**
privilege. The DLL can only *use* the privilege if the account already has it — it
cannot grant it for you. Granting it is optional and only enables the large-page TLB
optimization; everything else works without it.

**1. Grant the privilege**

1. Press `Win+R`, type `secpol.msc`, and run it as Administrator (Local Security Policy).
2. Go to **Local Policies → User Rights Assignment → Lock pages in memory**.
3. Click **Add User or Group**, type your Windows username, click **Check Names**, then **OK**.
4. Log off and back on (or restart) for the change to take effect.

**2. Still says `no permission`? Use a group catch-all**

If running as Administrator still produces the `no permission` log line, your Windows
username may not be mapping correctly inside the policy tool. Add the universal groups
instead of a specific account:

1. Open `secpol.msc` again.
2. Go back to **Local Policies → User Rights Assignment → Lock pages in memory**.
3. Clear out your specific account name.
4. Click **Add User or Group**, type `Administrators` (plural), click **Check Names**, then **OK**.
5. Click **Add User or Group** once more, type `Users` (plural), click **Check Names**, then **OK**.
6. Restart your computer.

After a restart the log should read `Large pages: ENABLED for mimalloc`. If you would
rather not change the policy at all, the line is harmless and can be ignored — or set
`TEST_ENABLE_LARGE_PAGES 0` in `src/version.h` to silence the attempt entirely.

</details>

---

## Project Structure

```text
wow-optimize/
├── src/
│   ├── allocators/           # mimalloc redirect, cache governor, heap compactor
│   ├── core/                 # DLL entry, proxy loader, features config (version.h)
│   ├── diagnostics/          # EIP sampling profiler, crash reporter, CVar watchdog
│   ├── hooks_subsystems/     # D3D9 state manager, CRT string fast-paths, event/data caches
│   ├── launcher/             # C# WinForms configurator & launcher dashboard
│   ├── runtime_vm/           # Lua C-API detour hooks, stack query inline paths, VM engine
│   ├── simd_math/            # SSE2 4x4 matrix, frustum point culling, raycast overrides
│   └── threading/            # Multi-threaded work pool dispatcher
├── CMakeLists.txt            # Build system definition
├── README.md                 # Project overview & documentation
└── LICENSE                   # Project license
```

## License

MIT License - use, modify, and distribute freely.

---

## Also for WoW 3.3.5a

| Project | What it does |
|---|---|
| [LuaBoost](https://github.com/suprepupre/LuaBoost) | Addon-side GC control, loading-screen helpers, shared APIs for addon authors |
| [WA_SafeGuard](https://github.com/suprepupre/WA_SafeGuard) | Backs up WeakAuras so a forced disconnect cannot wipe your auras |
| [DefileAlert](https://github.com/suprepupre/DefileAlert) | Instant Defile target callout for the Lich King encounter |

