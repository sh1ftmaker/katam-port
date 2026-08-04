# Audio plan

Research note, 2026-08-04.  Two parts: what the reference port the user pointed
at actually does, and what this port should do.

Conventions used below:

- **VERIFIED** — read out of a file, with the path and line, or observed from a
  command whose output is quoted.
- **INFERRED** — a conclusion drawn from those facts, not itself observed.

Nothing in this document has been implemented.  No file outside this one was
changed.

---

# Part 1 — how `sh1ftmaker/sm64coopdx` does audio in the browser

## 1.1 Yes, it has a web target

**VERIFIED.**  `gh repo view sh1ftmaker/sm64coopdx` reports the fork's default
branch is **`feature/emscripten-web-port`**, and its parent is `zalo/sm64coopdx`
(the same `zalo` who has the second KATAM port).  The branch carries a complete
emscripten target:

| file | what it is |
|---|---|
| `build_web.sh` | wrapper: sources `~/emsdk/emsdk_env.sh`, then `emmake make TARGET_WEB=1 VERSION=us DEBUG=1` |
| `Makefile:144-158` | `TARGET_WEB=1` block — sets `CC := emcc`, `RENDER_API := GL`, `WINDOW_API := SDL2`, **`AUDIO_API := SDL2`**, `CONTROLLER_API := SDL2` |
| `Makefile:941-955` | the emscripten `LDFLAGS` |
| `src/pc/web/shell.html` | 1800-line custom shell (`--shell-file`) |
| `src/pc/thread_web.c` | the threading stub-out |
| `.github/workflows/build-web.yaml` | builds and deploys to GitHub Pages |
| `src/pc/web/serve.py` | local dev server |

## 1.2 The backend is SDL2, compiled by emscripten's SDL2 port

**VERIFIED.**  `Makefile:144-158` selects `AUDIO_API := SDL2` for the web build,
which compiles `src/pc/audio/audio_sdl2.c` (guarded by `#ifdef AAPI_SDL2`).
The flags are `-s USE_SDL=2` in both `BACKEND_CFLAGS` (`Makefile:872`) and
`LDFLAGS` (`Makefile:943`).  There is no SDL_mixer, no OpenAL, no miniaudio in
the audio path — `src/pc/utils/miniaudio.h` is vendored but is not what
`AUDIO_API` selects.  There is no `-sAUDIO_WORKLET` and no `emscripten_*audio*`
call anywhere in the tree.

The full emscripten link line (`Makefile:941-955`), for comparison with ours:

```
-s USE_SDL=2 -s USE_ZLIB=1 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
-s INITIAL_MEMORY=268435456 -s MAX_WEBGL_VERSION=2 -s MIN_WEBGL_VERSION=1
-s FULL_ES2=1 -s FORCE_FILESYSTEM=1
-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","allocateUTF8"]'
-s ASYNCIFY -s ASYNCIFY_STACK_SIZE=65536 -s STACK_SIZE=2097152
-s EXIT_RUNTIME=0 -lidbfs.js
--shell-file src/pc/web/shell.html
```

Note it is **also** an ASYNCIFY build, like ours.  There is no `-pthread` and no
`-sPTHREAD_POOL_SIZE` anywhere in the file (`grep -n "pthread"` matches only the
Windows/Linux/OSX `LDFLAGS` at `Makefile:958,968,970,976`).

## 1.3 Sample path: push (queue), not a pull callback

**VERIFIED.**  `src/pc/audio/audio_sdl2.c` is 63 lines and is the whole backend:

```c
want.freq = 32000;  want.format = AUDIO_S16SYS;
want.channels = 2;  want.samples = 512;
want.callback = NULL;                       // <- no callback: queue mode
dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
SDL_PauseAudioDevice(dev, 0);
...
static int audio_sdl_buffered(void) { return SDL_GetQueuedAudioSize(dev) / 4; }
static int audio_sdl_get_desired_buffered(void) { return 1100; }
static void audio_sdl_play(const uint8_t *buf, size_t len) {
    if (audio_sdl_buffered() < 6000)        // don't over-fill
        SDL_QueueAudio(dev, buf, len);
}
```

The producer is `buffer_audio()` at `src/pc/pc_main.c:341-361`.  Its shape is the
part worth stealing:

```c
int samplesLeft = audio_api->buffered();
u32 numAudioSamples = samplesLeft < audio_api->get_desired_buffered()
                    ? SAMPLES_HIGH : SAMPLES_LOW;      // 560 : 528
for (s32 i = 0; i < 2; i++)
    create_next_audio_buffer(sAudioBuffer + i*(numAudioSamples*2), numAudioSamples);
audio_api->play((u8 *)sAudioBuffer, 2 * numAudioSamples * 4);
```

That is **queue-depth feedback**: the N64 audio engine is asked for a slightly
longer or shorter block depending on how deep the output queue currently is, so
the producer self-clocks against the audio device instead of against the frame
timer.  `SAMPLES_HIGH`/`SAMPLES_LOW` are 560/528 (`pc_main.c:140-141`), i.e. ±3%
around 544 — two blocks per tick, 32 kHz stereo, so roughly 34 ms produced per
game tick against a ~34 ms tick.

## 1.4 Underneath SDL2: a `ScriptProcessorNode` on the main thread

**VERIFIED**, from SDL2's own emscripten driver
(`libsdl-org/SDL`, branch `SDL2`, `src/audio/emscripten/SDL_emscriptenaudio.c`;
fetched during this research):

- line 342: `SDL2.audio.scriptProcessorNode = SDL2.audioContext['createScriptProcessor']($1, 0, $0);`
- line 355: `.connect(SDL2.audioContext['destination'])`
- line 273: the device's real frequency is taken from `SDL2.audioContext.sampleRate` — **SDL silently overrides the requested 32000 Hz with whatever the browser's AudioContext runs at** (typically 48000).
- line 400 area: `impl->ProvidesOwnCallbackThread = SDL_TRUE;` and
  `LockDevice`/`UnlockDevice` are empty — `/* no threads here */`.

So sm64coopdx's browser audio is: game thread → `SDL_QueueAudio` (a memcpy into
SDL's queue) → `ScriptProcessorNode.onaudioprocess`, which is a **main-thread**
callback → `destination`.  `AudioWorklet` is not involved at any point.

Corroborating evidence on the app side: `src/pc/web/shell.html:673` reaches for
`Module.SDL2.audioContext`, which is the exact object that driver creates.

## 1.5 Autoplay policy — handled twice, in both layers

**VERIFIED, SDL side** (`SDL_emscriptenaudio.c:357-375`): when
`SDL2.audioContext.state === 'suspended'` at open time, SDL creates a silent
buffer and a `setInterval` "fake callback" that (a) keeps calling the app's
audio callback so the app can make progress and does not flood its queue, and
(b) polls `navigator.userActivation.hasBeenActive` and calls
`audioContext.resume()` once the user has interacted.  When the real
`onaudioprocess` finally fires, it `clearInterval`s the fake one.  Line 233 also
installs emscripten's own `autoResumeAudioContext(SDL2.audioContext)`.

**VERIFIED, app side** (`src/pc/web/shell.html:665-702`) — belt and braces on top:

```js
// iOS Safari requires touchstart (not just click) and the AudioContext
// must be created or resumed inside a user gesture handler.
function resumeAudio() {
  var ctx = Module.SDL2 && Module.SDL2.audioContext;
  if (ctx && ctx.state === 'suspended') ctx.resume();
  if (!ctx) { var AC = window.AudioContext || window.webkitAudioContext; ... }
}
// Continuously retry resume — iOS sometimes needs multiple attempts
// because SDL2's AudioContext may not exist yet on first touch
document.addEventListener('click',      resumeAudioRetry);
document.addEventListener('keydown',    resumeAudioRetry);
document.addEventListener('touchstart', resumeAudioRetry);
document.addEventListener('touchend',   resumeAudioRetry);
```

The comments are the useful part: the context may not exist yet on the *first*
gesture, so a one-shot unlock is not enough — it has to be retried on every
gesture until it takes.

## 1.6 Threading — single-threaded; the audio thread is deliberately disabled

**VERIFIED.**  `src/pc/thread_web.c` replaces the whole threading API for
`TARGET_WEB`: `init_thread(handle, entry, arg, ...)` simply calls `entry(arg)`
inline and marks the handle `RUNNING`; every mutex operation is `return 0`.

`src/pc/pc_main.c:889` has the audio-thread spawn **commented out**:

```c
// Initialize the audio thread if possible.
// init_thread_handle(&gAudioThread, audio_thread, NULL, NULL, 0);
```

and `produce_one_frame()` (`pc_main.c:399-401`) plus the web tick loop
(`pc_main.c:656-658`) both fall back to running `buffer_audio()` inline:

```c
if (gAudioThread.state == INVALID)
    CTX_EXTENT(CTX_AUDIO, buffer_audio);
```

No `SharedArrayBuffer`, no `-pthread`.  **INFERRED:** it does not need COOP/COEP
cross-origin isolation and therefore deploys to GitHub Pages without trouble —
which the workflow (`.github/workflows/build-web.yaml`, "Build Web (Emscripten)
& Deploy to GitHub Pages") confirms it does.  The COOP/COEP headers in
`src/pc/web/serve.py:12-13` are for the *local* dev server and are not required
by the audio path.

## 1.7 Buffer size / latency, and what they say about glitching

**VERIFIED numbers:** requested device buffer 512 frames (`audio_sdl2.c:22`,
overridden in practice by the ScriptProcessorNode's chosen size); target queue
depth 1100 frames ≈ 34 ms at 32 kHz (`audio_sdl2.c:37`); hard ceiling 6000
frames ≈ 190 ms before the producer starts dropping (`audio_sdl2.c:41`).

**VERIFIED:** there are **no comments about crackle, underruns or stutter**
anywhere in `src/pc/` — a full case-insensitive grep for
`crackle|underrun|stutter|glitch|choppy` over the C, headers, HTML and docs
returned nothing but false positives on the word "buffer".  The only defensive
comment is `audio_sdl2.c:42`, *"Don't fill the audio buffer too much in case
this happens"*.

**INFERRED:** a `ScriptProcessorNode` callback runs on the main thread, which in
this build is also running the whole game inside a `requestAnimationFrame`
callback under ASYNCIFY.  Any main-thread stall longer than the node's buffer
(512 frames ≈ 11 ms at 48 kHz) produces an audible gap.  The ~34 ms target queue
depth is what buys headroom for that; it is latency deliberately traded for
robustness.  This is the single design point I would *not* copy — see §2.6.

## 1.8 Summary of Part 1

| question | answer |
|---|---|
| web build? | yes, `feature/emscripten-web-port`, deployed to GH Pages |
| backend | SDL2 via `-sUSE_SDL=2` (emscripten's SDL2 port) |
| transport | push: `SDL_QueueAudio` from the game tick |
| browser API underneath | `ScriptProcessorNode` on the **main** thread |
| autoplay | SDL's silent-buffer + `userActivation` poll, plus a shell-level retry-on-every-gesture |
| threading | single-threaded; audio thread commented out; no SAB, no COOP/COEP needed |
| latency | ~34 ms target queue, ~190 ms cap |

The genuinely reusable ideas are **queue-depth feedback** (§1.3) and
**retry the unlock on every gesture, not just the first** (§1.5).  The SDL2
dependency itself is not worth adopting here — see §2.6.

---

# Part 2 — a plan for katam-port

## 2.1 What the port has today

**VERIFIED.**  `platform/audio.c` (92 lines) answers the entire m4a API with
no-ops, and provides real storage for `gMPlayInfo_0..3`, `gSoundInfo` and
`gMPlayMemAccArea` because game code reads them back.  `tools/portify.py:59-65`
drops `m4a.c` and `m4a_tables.c` from the build.  `docs/ARCHITECTURE.md` calls
the omission deliberate.

`platform/audio.c:33-42` also uses `m4aSoundMain` as a load-bearing hook —
it calls `PortVBlankEnd()`, because on hardware the mixer's own runtime is part
of what ends the VBlank window.  **Any real implementation has to keep that
call**, or the `while (REG_DISPSTAT & DISPSTAT_VBLANK);` spin at the end of
`GameLoop` will never exit on a light frame.

## 2.2 What m4a actually needs from the hardware — VERIFIED, and it is much less than expected

Reading `/home/agent-tom/Desktop/katam/src/m4a.c` and `asm/m4a_asm.s`:

**The mixer never touches sound hardware at all.**  `SoundMainRAM` reads sample
data through `struct WaveData *` pointers (into ROM) and writes 8-bit PCM into
`soundInfo->pcmBuffer` — a plain array inside `struct SoundInfo`
(`include/gba/m4a.h:190`, `s8 pcmBuffer[PCM_DMA_BUF_SIZE * 2]`,
`PCM_DMA_BUF_SIZE == 1584`, line 153).  Nothing else.

Every hardware register touch in the engine is **setup that exists solely to get
`pcmBuffer` to the speaker**:

| site | what it does | needed under emulation? |
|---|---|---|
| `src/m4a.c:296-331` `SoundInit` | arms DMA1/DMA2, points `DMA1SAD/DMA2SAD` at `pcmBuffer`, `DMA1DAD/DMA2DAD` at `REG_FIFO_A/B`, sets `SOUNDCNT_H` to FIFO-A/B fed by Timer 0, sets `SOUNDBIAS` | **no** — we read `pcmBuffer` directly |
| `src/m4a.c:333-355` `SampleFreqSet` | `REG_TM0CNT_L = -(280896 / pcmSamplesPerVBlank)` — Timer 0 is the sample clock | **no** |
| `src/m4a.c:433-461` `m4aSoundVSyncOff/On` | re-arms DMA1/2 | **no** |
| `src/m4a.c:234-248` `MPlayExtender`, `708-728` `CgbOscOff`, `770+` `CgbSound` | writes `REG_NR10..NR51`, `SOUNDCNT_L/X` — the 4 PSG channels | **yes** — this is the only genuine hardware dependency |
| `asm/m4a_asm.s:40-46` (inside `SoundMain`) | reads `0x04000006` = `REG_VCOUNT`, but **only if `soundInfo->maxLines != 0`** | **no** for KATAM — see below |

**VERIFIED that the VCOUNT path is dormant in KATAM.**  `src/m4a.c:266` sets
`soundInfo->maxLines = MAX_LINES`, and `MAX_LINES` is `((u32)gMaxLines)` — an
address-as-value linker constant.  `/home/agent-tom/Desktop/katam/linker.ld:2`
reads `gMaxLines = 0;`.  So `maxLines == 0`, the scanline-budget branch is
skipped, and the mixer never reads `REG_VCOUNT`.  (`linker.ld:1` likewise gives
`gNumMusicPlayers = 4`.)

**Conclusion (INFERRED, but tightly):** to make m4a audible we need **zero**
DirectSound FIFO emulation, **zero** DMA1/DMA2 emulation, and **zero** Timer 0
emulation.  Those three exist only to move `pcmBuffer` to the DAC, and in this
port we can just read `pcmBuffer` ourselves.  The register writes that set them
up can be left in place writing to the mapped-but-inert I/O region at
`0x04000000` — harmless.  The **only** hardware that has to be emulated for real
is the 4-channel PSG.

## 2.3 State of `SoundMainRAM` in `/home/agent-tom/Desktop/katam` — VERIFIED, and worse than "just the mixer"

There is **no C version of any of it** in the decomp checkout.  Searched `src/`,
`wip/`, `asm/`, `multi_boot/`:

- `asm/m4a_asm.s` — 26,669 bytes, **44 exported symbols**, all ARM/Thumb.
- `src/m4a.c` — 38,605 bytes of C, but it is only the *outer* layer.
- `wip/` contains no audio work at all (`code_0802B4A8.c`, `code_08032E98.c`,
  `code.c`, `cutscene_trigger.c`, `flamer.c`, `large_star_stone_block.c`,
  `sprite.c`).
- `asm/nonmatching/` has nothing for m4a.
- The only other `SoundMainRAM` hits in the tree are the three `multi_boot/`
  sub-programs, which are more copies of the same assembly.

The exact split matters, because it is not "C plus one asm mixer":

**In `asm/m4a_asm.s` (all 44):**
`umul3232H32`, `__umul3232H32`, `SoundMain`, `SoundMainRAM`,
`SoundMainRAM_Reverb`, `SoundMainRAM_NoReverb`, `SoundMainRAM_ChanLoop`,
`sub_0814F604`, `sub_0814F7EC`, `sub_0814F80A`, `SoundMainBTM`,
`RealClearChain`, `ply_fine`, `MPlayJumpTableCopy`, `sub_0814F890`,
`sub_0814F8AC`, `sub_0814F8AE`, `ply_goto`, `ply_patt`, `ply_pend`, `ply_rept`,
`ply_prio`, `ply_tempo`, `ply_keysh`, `ply_voice`, `ply_vol`, `ply_pan`,
`ply_bend`, `ply_bendr`, `ply_lfodl`, `ply_modt`, `ply_tune`, `ply_port`,
`sub_0814FA34`, `m4aSoundVSync`, `MPlayMain`, `sub_0814FCE0`, `TrackStop`,
`ChnVolSetAsm`, `ply_note`, `ply_endtie`, `clear_modM`, `sub_0814FFC0`,
`ply_lfos`, `ply_mod`.

**In `src/m4a.c` (C):** the `m4a*` public API, `SoundInit`, `SampleFreqSet`,
`m4aSoundMode`, `SoundClear`, `MPlayOpen/Start/Stop`, `FadeOutBody`,
`TrkVolPitSet`, `MPlayExtender`, the whole CGB/PSG driver (`CgbSound`,
`CgbOscOff`, `CgbModVol`, `MidiKeyToCgbFreq`), `ply_memacc`, `ply_xcmd` and the
eleven `ply_x*` extended handlers.

So the assembly holds **the sequencer (`MPlayMain`, `ply_note`, and 17 of the 24
base command handlers) *and* the mixer (`SoundMainRAM`) *and* the per-frame
driver (`SoundMain`, `m4aSoundVSync`)**.  Simply un-dropping `src/m4a.c` gets
you a file that links against 44 missing symbols and produces nothing.

`asm/m4a_asm.s` is also not portable even in principle: `m4aSoundInit`
(`src/m4a.c:63`) does
`CpuCopy32((void *)((s32)SoundMainRAM & ~1), SoundMainRAM_Buffer, 0x400)` —
copying *machine code* into a RAM buffer and executing it, the same
code-as-data pattern that already defeated `src/agb_sram.c` and forced
`platform/sram.c` to exist.  It cannot work in wasm at all.

## 2.4 The decisive prior art: a C reimplementation already exists

This is the finding that settles the (a)-vs-(b) question.

**VERIFIED.**  `SAT-R/sa2` (the Sonic Advance 2 decompilation, which has a
first-class native/SDL2 "PC port" target) carries
`src/platform/shared/audio/m4a_sound_mixer.c` — **940 lines of C that replace
the entire `m4a_asm.s`**, plus `src/platform/shared/audio/cgb_audio.c` (279
lines) which emulates the four PSG channels.  Its provenance is stated in
`include/platform/shared/audio/README.txt`:

```
The source code in this folder is from:
https://github.com/Kurausukun/pokeemerald/tree/pc_port
Thanks to camthesaxman, Kurausukun, Pidgey and NT_x86 for allowing us to use the code!
```

i.e. camthesaxman's pokeemerald PC port.  MP2K is the same MKS4AGB library in
every GBA game that uses it, so this is the same engine KATAM has, not a
lookalike.

**I confirmed the semantic match against KATAM's own assembly.**  The C
`SoundMain()` in `m4a_sound_mixer.c:32-69` is instruction-for-instruction the
shape of `asm/m4a_asm.s:19-60`:

| C (`m4a_sound_mixer.c`) | KATAM asm (`asm/m4a_asm.s`) |
|---|---|
| `if (mixer->lockStatus != ID_NUMBER) return; mixer->lockStatus++;` | ldr `0x68736D53`, compare `[r0]`, `adds r3,#1`, `str` |
| `if (mixer->maxScanlines != 0) { vcount = REG_VCOUNT; ... += 228 if < 160 }` | `ldrb r1,[r0,#0xc]` … `=0x04000006` … `cmp r2,#0xa0` … `adds r2,#0xe4` |
| `if (mixer->MPlayMainHead) mixer->MPlayMainHead(mixer->musicPlayerHead);` | `ldr r3,[r0,#0x20]` / `ldr r0,[r0,#0x24]` / `bl sub_0814F80A` |
| `mixer->CgbSound();` | `ldr r3,[r0,#0x28]` |

Offsets 0x0C / 0x20 / 0x24 / 0x28 land exactly on KATAM's
`maxLines` / `func` / `intp` / `CgbSound` (`include/gba/m4a.h:155-190`).  That is
strong evidence the reimplementation is faithful to this exact build of the
library.

It covers **all** 44 symbols, under renamed identifiers — including the ones the
KATAM asm has that I checked for specifically: `MP2K_event_prio`
(`m4a_sound_mixer.c:435`), `MP2K_event_lfodl` (`:484`), `SoundMainBTM` (`:330`),
`MPlayJumpTableCopy` (`:358`), `umul3232H32` (`:323`), `ChnVolSetAsm` (`:688`),
`MP2KPlayerMain` = `MPlayMain` (`:508`), `MP2K_event_nxx` = `ply_note` (`:704`),
`m4aSoundVSync` (`:910`).

It is also already *ported*, not merely decompiled: it mixes into
`fixed8_24` (s32 8.24 fixed point) instead of `s8`, resamples with a
`float sampleRateReciprocal` so pitch is correct at any host rate, and its
`m4aSoundVSync` (`:910-940`) sums the DirectSound and CGB buffers and hands a
frame of interleaved stereo `s16` to a single platform hook:

```c
fixed8_24 sample = (m4aBuffer[i] + cgbBuffer[i]) >> 3;   // headroom
audioBuffer[i] = sample >> 9;                            // 8.24 -> s16
Platform_QueueAudio(audioBuffer, samplesPerFrame * sizeof(s16));
```

**`Platform_QueueAudio(const s16 *, uint32_t bytes)` is the entire platform
surface the audio engine needs.**  In sa2's SDL build that is 12 lines
(`src/platform/pret_sdl/sdl2.c:520-533`) wrapping `SDL_QueueAudio` with the same
over-fill guard sm64coopdx uses.  In our port it becomes an `EM_JS` shim.

Host sample rate is handled by patching two lines of `m4a.c`
(`SAT-R/sa2` `src/lib/m4a/m4a.c:373-375`):

```c
#else   /* !PLATFORM_GBA */
    soundInfo->samplesPerFrame   = 800;                 /* 800 * 60 = 48000 Hz */
    soundInfo->framesPerDmaCycle = PCM_DMA_BUF_SIZE / soundInfo->samplesPerFrame;
    soundInfo->sampleRate        = 60.0f * soundInfo->samplesPerFrame;
```

with `PCM_DMA_BUF_SIZE` raised from 1584 to 4907 for non-GBA builds
(`include/lib/m4a/m4a_internal.h:95-98`).

**One other high-quality reference, for design only:** NBA's MP2K HLE
(`folium-app/Tomato` `Core/hw/apu/hle/mp2k.cpp`, © 2024 fleroviux) does the same
trick from the emulator side — it detects the call to `SoundMainRAM` and renders
float samples at the host rate instead of executing the ARM code.  It is
**GPLv3**, so it is a design reference, not a source of code.  The
pokeemerald-pc_port lineage above is the one to actually use, but its licence is
"the authors gave permission" rather than a written grant — **that needs
checking with camthesaxman/Kurausukun before any of it is vendored**, and it is
a hard blocker on shipping, not a detail.

## 2.5 Recommendation on (a) vs (b): **(a), decisively — but "restore m4a.c" is not the whole of (a)**

**Recommend option (a): run the game's own m4a engine, with the assembly
replaced by C, and with no sound-hardware emulation underneath except the PSG.**

Reasoning:

1. **Option (b) is a much bigger job than it sounds.**  "Synthesise from the song
   data directly" means writing a MIDI-ish sequencer for the MP2K track format,
   an envelope/LFO/portamento/modulation engine, the key-split and rhythm voice
   types, the priority-based 12-voice allocator, and the CGB channel mapping —
   i.e. reimplementing everything in `MPlayMain` + `ply_note` + `TrkVolPitSet` +
   `CgbSound` anyway, but *without* the decomp's own C as a cross-check, and
   with no way to A/B against hardware except by ear.  You would also have to
   intercept every `m4aSongNumStart`/fade/volume/pitch/pan call and re-implement
   its semantics, because the game drives the engine through that API
   continuously (341 `m4a*` call sites across `src/`).

2. **Option (a)'s hardware burden is nearly nil** (§2.2).  No FIFO, no DMA1/2,
   no Timer 0, no VCOUNT.  One PSG.

3. **The expensive part of (a) is already written** (§2.4): 940 lines of C that
   are a faithful reimplementation of the same 44 assembly functions, already
   adapted to run at a host sample rate and already reduced to a one-function
   platform interface.  This turns the job from "reverse-engineer 42 ARM
   functions" into "rename identifiers and wire up one output hook".

4. **All the sound data is already reachable.**  Songs, tone tables and PCM
   samples live in ROM and are followed by pointer — which works precisely
   because of the port's memory-map decision.  `build/generated/port/rom_data.h`
   already emits `gSongTable` at `0x08B59ED0` (:190), `gMPlayTable` at
   `0x08B59EA0` (:189), `gXcmdTable` (:188), `gPcmSamplesPerVBlankTable` (:183),
   `gScaleTable`/`gFreqTable`/`gCgbScaleTable`/`gCgbFreqTable`/`gNoiseTable`/
   `gCgb3Vol` (:181-187).  **No asset extraction step is needed at all.**  This
   is a large, concrete advantage over (b), which would need its own extractor.

5. **It preserves the port's stated principle.**  `docs/ARCHITECTURE.md` — the
   game's code compiles as written; adaptation happens at the narrowest point
   that works.  Replacing `m4a_asm.s` with the C that the same asm was compiled
   from is exactly that.  Bypassing the engine is the shim-everything approach
   the architecture already rejected once.

**Caveat on the framing:** the question as posed ("restore the decomp's own
`src/m4a.c` and emulate the hardware underneath it") is not quite the available
option.  `src/m4a.c` alone is a stub-magnet — the sequencer *and* the mixer are
both in the assembly (§2.3).  The real option (a) is "restore `src/m4a.c` +
`src/m4a_tables.c`, **and** supply a C `m4a_asm.s` replacement, and emulate
almost nothing".

## 2.6 Which emscripten audio API — VERIFIED constraints, then the call

### The SharedArrayBuffer constraint is real, and I checked it

**VERIFIED, empirically.**  `curl -sI https://sh1ftmaker.github.io/katam-port/`
returns `HTTP/2 200` with `server: GitHub.com` and — checked against the full
response — **no `Cross-Origin-Opener-Policy` and no
`Cross-Origin-Embedder-Policy` header**.  GitHub Pages provides no mechanism to
add them.  Therefore `crossOriginIsolated === false`, `SharedArrayBuffer` is
unavailable, and wasm shared memory cannot be used on that deployment.

**VERIFIED, consequence for emscripten.**  From the local emsdk (emcc 6.0.5),
`upstream/emscripten/src/settings.js:1670-1678`:

```
// If true, enables targeting Wasm Web Audio AudioWorklets. ...
// Note: The setting will implicitly add ``worklet`` to the ENVIRONMENT,
// (i.e. the resulting code and run in a worklet environment) but additionaly
// depends on ``WASM_WORKERS`` and Wasm SharedArrayBuffer to run new Audio
// Worklets.
var AUDIO_WORKLET = 0;
```

and `tools/link.py:188-189, 1605-1607` confirm `AUDIO_WORKLET` pulls in the
worklet environment and `libwebaudio.js`.

**So: `-sAUDIO_WORKLET` is off the table for the GitHub Pages deployment.**
It requires `WASM_WORKERS` + shared memory, which requires cross-origin
isolation, which GH Pages cannot give.

Two further notes on this:
- The port is *also* deployed to **`https://katam-port.pages.dev`** (Cloudflare
  Pages).  Cloudflare Pages **can** send COOP/COEP via a `_headers` file, so
  `-sAUDIO_WORKLET` would be *possible there*.  I recommend against splitting the
  audio backend by host: cross-origin isolation also breaks `?rom=<url>` loading
  from third-party hosts unless they send CORP, and the ROM-from-URL path is a
  documented feature.  One backend, works everywhere.
- Shared memory is independently incompatible with the port's current
  `-sALLOW_MEMORY_GROWTH=0` + fixed 192 MB `INITIAL_MEMORY` layout being simple;
  more importantly it would mean auditing the entire single-threaded port for
  data races on the GBA memory map.  Not worth it for audio.

### Also not recommended: `-sUSE_SDL=2`

It would work, and it is what the reference port does.  But it means adopting
SDL2 into a port that currently links nothing but libc; it gets you a
**main-thread `ScriptProcessorNode`** (§1.4) whose callback competes with an
ASYNCIFY game loop for the same thread; and it takes control of the
`AudioContext` away from `web/shell.html`, which already owns the ROM-loading
UI and every gesture the page receives.  ~60 lines of our own JS replaces the
whole dependency.

### Recommendation: a hand-written `AudioWorklet`, fed by `postMessage`, with a `ScriptProcessorNode` fallback

An `AudioWorkletNode` does **not** require `SharedArrayBuffer` — that is only
needed if the worklet wants to read wasm linear memory *directly*.  Without
SAB you copy: the main thread posts an `Int16Array`/`Float32Array` (transferable)
into the worklet, and the worklet keeps its own ring buffer in its own heap.
A frame of 48 kHz stereo s16 is ~3.2 KB; at 60 Hz that is ~190 KB/s of copying,
which is nothing.

Why this beats the SDL/ScriptProcessor arrangement here specifically:

- **`process()` runs on the browser's dedicated audio render thread.**  It is
  immune to main-thread jank.  This port renders a full GBA scanline PPU in
  software on the main thread inside `PortPresentFrame` (`platform/main.c:197`),
  and then blocks on `requestAnimationFrame` under ASYNCIFY — precisely the
  workload that makes a main-thread `ScriptProcessorNode` crackle.
- **It lets us run a much shorter output queue** than sm64coopdx's 34 ms, because
  a stall on the producer side is absorbed by the ring rather than instantly
  audible.  Start at ~4 frames (~67 ms) of ring and tune down.
- `ScriptProcessorNode` remains as a fallback for anything that lacks
  `AudioWorklet` — in practice nothing current, but it is 15 extra lines.
- The worklet module can be loaded from a **`blob:` URL** built from a string
  inside `web/shell.html`, so the dist stays `index.html` + `katam.js` +
  `katam.wasm` and `scripts/publish-pages.sh` needs no change.  (GitHub Pages
  sends no CSP, verified in the header dump above, so `blob:` worklets load.)

### Autoplay

The shell already has real user gestures to hang the unlock on:
`web/shell.html:1186` (`romfile` change), `:1205` (drop), `:1524` (URL button).
Create the `AudioContext` **inside** one of those handlers.  But the
IndexedDB-remembered-ROM path can start the game with no gesture at all, so
follow sm64coopdx's lesson (§1.5) and **also** attach a `resume()` retry to
`pointerdown`/`keydown`/`touchend` that keeps firing until
`ctx.state === 'running'`, rather than a one-shot unlock.

### Sample rate

Do **not** hard-code 48000.  Read `ctx.sampleRate` after the context exists,
pass it into C, and set `samplesPerFrame = round(sampleRate / 60)` in the
patched `SampleFreqSet`.  The mixer then renders natively at the device rate and
no resampling stage is needed anywhere.  (iOS Safari in particular will pick its
own rate and ignore a requested one.)

## 2.7 Implementation sketch

```
web/shell.html
    portAudio = {
      ctx, node, queuedFrames,
      start(sampleRate)          // called inside a user gesture
      push(int16ArrayCopy)       // -> node.port.postMessage(buf, [buf.buffer])
      depth()                    // frames currently buffered in the worklet
    }
    worklet processor: ring of Int16Array chunks; process() drains into
    output[0]/output[1]; on underrun emits silence and bumps a counter that is
    posted back so the C side can widen its blocks.

platform/audio.c  (rewritten; keeps the exact same exported signatures)
    EM_JS(void, PortAudioPush,  (const s16 *pcm, int frames), {...});
    EM_JS(int,  PortAudioDepth, (void), {...});     // frames queued
    EM_JS(int,  PortAudioRate,  (void), {...});     // ctx.sampleRate, 0 if not started

    void Platform_QueueAudio(const s16 *data, u32 bytes);   // the one hook the
                                                            // mixer calls
    /* keep: m4aSoundMain() must still call PortVBlankEnd() */

platform/m4a_mixer.c   (new; adapted from the pokeemerald pc_port lineage)
    SoundMain, MPlayMain, ply_* (17), ply_note, ply_endtie, TrackStop,
    ChnVolSetAsm, SoundMainBTM, RealClearChain, MPlayJumpTableCopy,
    umul3232H32, m4aSoundVSync           -- i.e. all 44 asm symbols

platform/cgb_audio.c   (new; the 4 PSG channels)

tools/portify.py
    - remove 'm4a.c' and 'm4a_tables.c' from REPLACED_FILES
    - patch src/m4a.c on copy (same mechanism as SRAM_RELOC / TASK_DTOR_CALL):
        * SoundInit:      drop the DMA1/2 + SOUNDCNT + SOUNDBIAS block
        * SampleFreqSet:  samplesPerFrame = round(hostRate / 60); drop REG_TM0*
        * m4aSoundInit:   drop the CpuCopy32 of SoundMainRAM (unportable)
        * CgbSound/CgbOscOff/MPlayExtender: REG_NRxx writes -> cgb_* calls
    - stop emitting the rom_data.h address macros for the tables that
      m4a_tables.c now defines in C (gScaleTable, gFreqTable, gXcmdTable,
      gPcmSamplesPerVBlankTable, gCgbScaleTable, gCgbFreqTable, gNoiseTable,
      gCgb3Vol) or they will collide.
```

Struct field mapping, for whoever does the adaptation (KATAM name → pc_port name):

| `include/gba/m4a.h` | pokeemerald pc_port |
|---|---|
| `struct SoundInfo` | `struct SoundMixerState` |
| `struct SoundChannel` | `struct MixerSource` |
| `struct MusicPlayerInfo` | `struct MP2KPlayerState` |
| `struct MusicPlayerTrack` | `struct MP2KTrack` |
| `ident` | `lockStatus` |
| `pcmDmaCounter` | `dmaCounter` |
| `pcmDmaPeriod` | `framesPerDmaCycle` |
| `pcmSamplesPerVBlank` | `samplesPerFrame` |
| `pcmFreq` | `sampleRate` |
| `divFreq` | `sampleRateReciprocal` (becomes `float`) |
| `maxLines` | `maxScanlines` |
| `func` / `intp` | `MPlayMainHead` / `musicPlayerHead` |
| `s8 pcmBuffer[1584*2]` | `fixed8_24 pcmBuffer[4907*2]` |

**Keep KATAM's names.**  Game code reads `gMPlayInfo_0.status`
(e.g. `src/collection_room.c:1744`) and passes `&gMPlayInfo_N` to
`m4aMPlayFadeOut` in ~20 places, so `struct MusicPlayerInfo`'s layout and name
must stay.  Only `struct SoundInfo`'s layout is free to change, because nothing
outside the engine touches it (the port already owns `gSoundInfo` in
`platform/audio.c:24`).

Also needed as plain C definitions alongside the existing ones in
`platform/audio.c`: `gMPlayTrack_0..3` and `gCgbChans` (currently GBA linker
allocations declared in `sound/music_player_table.inc`), and literal `4` / `0`
for `gNumMusicPlayers` / `gMaxLines` (`linker.ld:1-2`).

## 2.8 Ordered task list

**Step 0 — the smallest thing that makes ANY sound (half a day, no m4a at all).**
Add `PortAudioStart/Push/Depth` to `web/shell.html` and a 30-line
`platform/audio.c` addition that generates a 440 Hz square wave in C and pushes
one frame's worth per `m4aSoundVSync`.  Trigger the `AudioContext` from the
existing `romfile` change handler.  This proves, independently of m4a: the
worklet loads from a blob URL, the autoplay unlock works on desktop *and* iOS
Safari, the once-per-`rAF` push cadence is right, and the queue does not drift.
Ship nothing; just confirm a tone comes out of a phone.

**Step 1 — licence check.**  Before writing any adaptation: confirm with
camthesaxman / Kurausukun / the SAT-R maintainers that the pc_port mixer can be
used here and under what attribution.  This gates everything after it.  If the
answer is no, the fallback is to write the mixer from the KATAM assembly using
the pc_port version and NBA's `mp2k.cpp` as behavioural references — several
weeks rather than several days, but the (a)-vs-(b) recommendation does not
change, because option (b) is still larger.

**Step 2 — DirectSound only, no PSG.**  Port `m4a_sound_mixer.c` to KATAM's
names into `platform/m4a_mixer.c`.  Un-drop `m4a.c` and `m4a_tables.c` in
`tools/portify.py`.  Patch `SoundInit`/`SampleFreqSet`/`m4aSoundInit` as above.
**Skip `MPlayExtender`** initially so `soundInfo->CgbSound` stays `nullsub_141`
and the PSG path is inert.  KATAM's soundtrack is predominantly sample-based, so
this alone should produce recognisable music.  Keep `platform/audio.c`'s
`PortVBlankEnd()` call in `m4aSoundMain`.

**Step 3 — the PSG.**  Add `platform/cgb_audio.c` and patch `CgbSound` /
`CgbOscOff` / `MPlayExtender`'s `REG_NRxx` writes into `cgb_*` calls.  Note the
port's I/O region is plain memory with no write hooks, so a "watch the registers"
design is not available — the writes have to be redirected at the source, which
is exactly what `tools/portify.py` already does for other cases.

**Step 4 — clock stability.**  Adopt sm64coopdx's queue-depth feedback
(§1.3): vary `samplesPerFrame` by ±3% per frame based on `PortAudioDepth()`.
This is what absorbs the rAF-vs-59.7275 Hz mismatch, and it is essential on
120 Hz displays — the port takes one game frame per `rAF`
(`platform/main.c:183-185, 212`), so on a 120 Hz panel the game and therefore
the audio producer run at double rate.  **Check whether that is already true
before writing the audio, because it is a pre-existing bug that audio will make
unmissable.**

**Step 5 — measure ASYNCIFY cost.**  The build has bare `-sASYNCIFY` with no
`ASYNCIFY_ONLY`/`ASYNCIFY_REMOVE` (`Makefile:96-97`).  Asyncify is conservative
about indirect calls, and the m4a engine dispatches every sequencer command
through `gMPlayJumpTable` function pointers — so the new hot mixer loop is
likely to be instrumented.  If the profile shows it, add the mixer's leaf
functions to `-sASYNCIFY_REMOVE`.  Nothing in the mixer can reach
`VBlankIntrWait`, so removing them is sound.

**Step 6 — mobile.**  Verify on iOS Safari (the retry-on-every-gesture unlock,
and the device sample rate not being 48000) and on Android Chrome (worklet
latency under a backgrounded tab).  Tune the ring depth down from 4 frames.

## 2.9 Risks and open questions

- **Licence (blocking).**  §2.4.  The pc_port mixer's terms are informal.
- **`struct SoundInfo` layout drift.**  Changing `pcmBuffer` from `s8` to
  `fixed8_24` and growing `PCM_DMA_BUF_SIZE` to 4907 makes `gSoundInfo` ~40 KB.
  It is a port-owned C global living in the emscripten data region above
  `0x0F000000`, not in the GBA map, so this is fine — but confirm nothing in the
  game does arithmetic on `SOUND_INFO_PTR` (`0x3007FF0`) beyond following it.
- **agbcc struct padding.**  `docs/ARCHITECTURE.md` already flags this.  `struct
  WaveData` and `struct ToneData` are read *out of ROM*, so a layout difference
  between agbcc and clang gives silent garbage audio rather than a crash.  Check
  `sizeof`/`offsetof` against `katam.map` for those two specifically before
  debugging by ear.
- **`gXcmdTable` holds ARM function addresses in ROM.**  Restoring
  `src/m4a_tables.c` gives a real C table and sidesteps the ROM-function-table
  problem entirely — but only if `tools/gen_rom_data.py` stops emitting the
  `#define gXcmdTable ((const XcmdFunc *)0x08B58738)` macro
  (`build/generated/port/rom_data.h:188`) that would otherwise shadow it.  Same
  for the six other table macros at `:181-187`.
- **Reverb.**  KATAM's `m4aSoundInit` (`src/m4a.c:66-69`) does not set
  `SOUND_MODE_REVERB_SET`, so reverb is per-song from `SongHeader.reverb`.  The
  pc_port mixer implements it (`m4a_sound_mixer.c:71-90`); just don't be
  surprised when it engages.
- **Frame rate.**  See Step 4.  Worth confirming independently of audio.
