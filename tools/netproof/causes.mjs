// Every distinct mismatch cause the rig found, how it was localised, and what
// was done.  Kept by hand (a cause is a judgement); the report embeds it.
export const CAUSES = `
1. **The new-menus switch is host state the simulation reads** (first seen at
   frame 5313, 120 ms run, player 1's pause).  Localised by dump diff:
   \`gPauseMenus[0..3].menuId\` (0x0203ACCD + 0x14n) host 8 (SETTINGS) vs guest 1
   (HELP), and the pause screen's IWRAM heap copy.  Writer by reading:
   \`game/src/pause_help.c\` ~l.300-320 only cycles to MENU_PORT_SETTINGS when
   \`PortMenuPoc()\` is on; the host had it on, the guest (which took the image
   and never set it) off.  **Fixed**: the image header carries the switch and
   \`ApplyImage\` sets it (\`platform/lmpnet.c\`).  R5 ablation shows it is needed.

2. **Pause SETTINGS page edits device settings from inside a simulated frame,
   relatively (+1/-1)** (frame 5409, OBJ VRAM 0x06011000+, 205 bytes: the
   value tiles).  \`RowStep\` (\`platform/menu_poc.c\`) wrote \`gPortSettings\`
   (host memory) then applied and persisted it.  A guest that replays the frame
   of a Right press steps again: its settings drift from the host's, and the
   page draws different value tiles.  Two devices with different settings would
   also draw different pages with no rewind at all.  **Fixed**: while an online
   session is on, the page shows and edits a session copy kept in the sim block
   (\`PORT_SIM_LNSET\`, the header's reserved bytes, zero when off), so it rewinds
   with everything else; the engine applies an edit to the device of the player
   who made it once the frame is final (host: at once; guest: when verified).

3. **Pause page saved OBJ tiles in host statics** (frame 5542, 16.5 KB of OBJ
   VRAM 0x06010000-0x06015600, right after the page closes).
   \`PortPocPauseObjSave/Restore\` keep the covered tiles in \`sObjSave\` and clear
   \`sObjSaved\` on restore; a replay crossing the close a second time finds
   nothing to restore and keeps the page tiles.  Joining while the page is open
   is the same bug (the audit agent saw it too).  **Fixed**: menu_poc.c's screen
   state (\`sObjSave\`, \`sObjSaved\`, \`sModel\`, \`sDescShown\`, \`sOwner\`, Game Select
   descriptions, row banner, decoded geometry, description palette line) is saved
   with every keyframe and image and hashed as region "host"
   (\`PortPocHostStateSave/Load\`); the ROM-derived decode caches are rebuilt on
   an image load.  \`tools/check_sim_state.py\` now checks menu_poc.c too (it was
   not in SIM_PLATFORM, which is how this got past it), with each static
   allowlisted by class.

4. **Local-multiplayer switch outside the image** (found by design review and
   confirmed by the R5 ablation and the audit agent): \`sSetting\` in
   \`platform/localmp.c\` gates the player-2 latch in \`platform/inputlog.c\`.
   Carried in the image header.

5. **Harness bug, not a game bug**: the guest's time sync ran a 1200-frame
   burst on the tick its join image arrived (its frame counter was still
   pre-image), running 300 frames ahead.  Fixed in \`tools/netproof/session.mjs\`
   (one frame, then sync).  Also: the time sync's host-clock estimate now uses
   the least-delayed packet of the last second, so jitter alone does not cause
   holds and skips.
`;

export const NOTES = `
### What a real build must defer until a frame is sealed (final)

- **Sound starts.**  Replayed frames' audio is muted by this engine
  (\`PortLnMuteAudio\` in \`platform/audio.c\`; without it every replayed frame
  pushes an audio block).  Sound starts that were heard live but are not in the
  final timeline, and ones that are in the final timeline but were never heard,
  are counted per run (R9, feel table).  The sound engine's state itself is
  replayed exactly; only what reached the speakers can be wrong.  **This is
  the largest feel problem measured**: every action of player 1 reaches the
  guest one lead late, so its sound start falls in a replayed (muted) frame and
  is never heard from its start -- the "never heard live" count is of the same
  order as the number of final sound starts.  A real build needs a sound
  reconciler: after a replay, start the final timeline's sounds that were not
  heard (late, skipping what elapsed) and stop the ones heard that are no
  longer in it.  Not built.
- **SRAM persistence.**  \`WriteSram\` -> \`PortSramMarkDirty\` runs from unsealed
  and replayed frames (R9 counts).  SRAM is in every keyframe and image so the
  simulation stays exact, but the *storage write* must only happen from final
  frames (e.g. persist when the SRAM region hash of a verified frame changes).
  Built for the guest, more simply: a guest's save memory is the host's (the
  session's world), so from its \`PortLnStart\` until the page reloads nothing
  it holds reaches its storage (\`platform/sram.c\` PortSramPersistBlock; the
  page asks \`PortSramPersistAllowedJs\` before every write) and its own save
  is never restored over the host's.  The host persists as usual: it never
  rewinds.  \`tools/netproof/save_test.mjs\` runs the page's own save code in
  both instances: the guest's stored save survives byte for byte.
- **Device settings edited on the pause SETTINGS page.**  Built: the page
  edits a session copy (sim block); each device applies its own player's
  edits when the frame is final.  Same treatment is still needed for the front
  end's Game Select SETTINGS screen if it can be reached in a session.
- **Events (warps, view changes) sealed at the host's current frame always
  reach the guest after it simulated that frame**, so each costs a rewind and,
  for a warp or door, a room cut that arrives one lead late (R10 counts, feel
  table "corrections that changed a room").  Scheduling host events a lead
  into the future (as rollback.c's PortRbSuggestEventFrame does) would remove
  the rewinds.  Not built.
- **Anything else a simulated frame does to the host** (logs, \`PortLog\` from
  localmp carries, storage): nothing else was found by the counters here.
  There is no rumble in this port.

### Device-dependent inputs a real build must make common (not visible to two identical instances)

From the audit (\`tools/netaudit/matrix.py\`, its "in a session" rows), now
handled by \`platform/lnsession.c\` as a session starts: the mixer runs against a
canonical device (47782 Hz x 800 a frame, every frame, device or none;
\`platform/audio.c\`) and only the output is resampled to the device -- R12
runs host and guest with different audio device models, or none, and every
gate holds; \`AudioTopUp\` (\`platform/web/host_web.c\`) is suppressed while
\`PortLnActive()\`; the sim view follows each display unless
\`PortSimViewUseSession\` is used (\`PortLnStart\` calls it; view changes are
sealed VIEW events); the page's widescreen experiment is held at the hardware
view (\`platform/view.c\`); \`PortLmpP2InputAvailable\` is 1 in a session
(\`platform/localmp.c\`); the randomizer's settings travel in the image.

### Not verified / not run

- No real transport, no browser, no phone: the link is a model (no congestion,
  no bandwidth limit on datagrams, symmetric latency, the reliable channel is an
  approximation of SCTP).
- Same wasm build on both sides only (the image holds host pointers: DMA
  table, menu_poc state).  Native<->wasm or mixed builds would need netsim's
  portable image.
- Native gdb watchpoints were not needed: every mismatch was localised by
  image diff to a named symbol and its writer found by reading the code.
- The "save already loaded" flag ablation cannot diverge in node (no stored
  save).  The guest now sets it as its session starts; save_test.mjs checks
  with a stored save that it is never restored after that.
- Host frozen / host at 30 Hz were measured; a host that *migrates* (host
  leaves) is not designed or tested.
- Cell phone: one call at a time; in the scripted part player 2 calls (battery
  3->2) and player 1's later press does not (helpers already summoned).  The
  audit recording localmp_tour covers player 1's call.
`;
