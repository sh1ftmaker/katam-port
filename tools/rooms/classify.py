"""classify.py -- every byte that differs between two rooms-mode devices
(tools/rooms/measure.mjs), by class, with its writer and the reason.

Classes:
  V   view state: per screen by design.  Written for the screen's player or
      the screen's room only (the writer named), read only by the display,
      the sound or the screen's own menus.  Never needs to agree.
  VS  view state of the sound: the m4a engine and the sound registers.
      PlaySfx plays only for the screen's room (include/data.h), so its
      state is the screen's.  Its two gameplay-side readers decide music
      only (code_0806F780.c:9401, collection_room.c:1748).
  H   gameplay state that is the same on both devices except where the heap
      put it: tasks and their structs moved by view-only tasks taking slots
      and heap first (warp_star.c:2534, code_0806F780.c:8757/10006, the HUD
      and pause-screen tasks), and pointers to them.  Same objects, same
      values, other addresses (measure.mjs "objects at other addresses",
      the gameplay digest equal).  A whole-image hash cannot see past it.
  P   the screen's player itself (gLocalPlayerId and the display task's
      copy).
A byte no rule matches is UNEXPLAINED and is reported as such."""
import re

SOUND = [(0x02020080, 0x02020EE0, 'gMPlayTrack_0..3'), (0x03000560, 0x030017B0, 'gSoundInfo..gMPlayInfo_3')]

# (symbol regex, field regex or None, class, writer, why)
RULES = [
    ('^gLocalPlayerId$', None, 'P', 'code_080332BC.c:48 (PortLmpLocalPlayer), PortLmpReview',
     "the screen's player: the one thing that differs by design"),
    ('^task sub_08002118$', None, 'P', 'level.c:45 InitLevelTasks, LoadLevelGfx (var->unk0)',
     "the display task: its player (whose room the BG follows) and the saved display registers"),
    ('^(io)$', None, 'V', 'the VBlank flush of the shadow registers (main.c), m4a (sound registers 0x60-0x84)',
     'the display and sound hardware of this screen'),
    ('^(pltt)$', None, 'V', 'VBlank palette flush from gBgPalette/gObjPalette (main.c)', 'the colours on this screen'),
    ('^(vram)$', None, 'V', 'LoadLevelGfx, BG streaming (bg.c), sprite tile uploads (VramMalloc)', "this screen's tiles and maps"),
    ('^(oam)$', None, 'V', 'VBlank OAM flush', "this screen's sprites"),
    ('^gBgPalette$|^gObjPalette$', None, 'V', 'LoadBgPalette*, palette effects, sprite palette loads for the screen room',
     'shadow palettes of this screen'),
    ('^gSourcePalettes\\??$|^gSourcePalettesBackup\\??$', None, 'V', 'LoadLevelPaletteEffectSourcePalettes (kirby.c:13012, local arrival only), BackupPaletteEffectSourcePalettes (pause)',
     "palette-effect sources: the screen room's palettes"),
    ('^gPaletteEffectManager$', None, 'V', 'Create(Hidden)PaletteFade*, CreateRoomPaletteEffect (visible only for the screen player/room)',
     'palette fades of this screen'),
    ('^gUnk_03002EC0$|^gUnk_030039A4$|^gUnk_03006078$', None, 'V', 'bg.c:23 (BG tile upload queue), sprite.c', 'VRAM upload queue of this screen'),
    ('^gUnk_030031C0$|^gOamBuffer$|^gUnk_03002450$|^gUnk_03006080$|^gUnk_030024F0$|^gUnk_030035F0$', None, 'V',
     'sprite.c DrawToOamBuffer and friends', "this screen's OAM, its draw order and counts"),
    ('^gUnk_03002400$', None, 'V', 'bg.c (the BG layer table of the screen)', "this screen's BG layers"),
    ('^gBgScrollRegs$|^gDispCnt$', None, 'V', 'sub_080019F8, LoadLevelGfx', 'shadow display registers'),
    ('^gUnk_03002E20$', None, 'V', 'palette-effect / OBJ palette staging (pause screens)', 'shadow palette staging'),
    ('^gVramHeapState$', None, 'V', 'VramMalloc/VramFree from the draw macros (object.h Macro_08107BA8_*)', 'OBJ VRAM allocator: only the screen room\'s objects hold tiles'),
    ('^gUnk_02022E60$|^gUnk_02022E70$|^gUnk_02022E80$|^gUnk_02022EA0$', None, 'V', 'kirby.c:5720-5840 (sub_0803DF24/sub_0803DFAC, sub_0803E050, sub_0803E458)',
     'OBJ palette-slot and shared-sprite VRAM bookkeeping of this screen'),
    ('^gUnk_0203A9A0$|^gUnk_0203A9B0$|^gUnk_03003A00$', None, 'V', 'code_08020220.c (the frozen OAM while paused)', "this screen's sprites saved over the pause"),
    ('^gUnk_03003670$', None, 'V', 'main.c:263 (gMainFlags copied for the VBlank handler)', 'VBlank-side copy of gMainFlags'),
    ('^gMainFlags$', None, 'V', 'bg.c:37, palette.c:43 (MAIN_FLAG_*_PALETTE_SYNC_ENABLE, bits 0x1/0x2)',
     'palette upload requests of this screen (the pause bit 0x400 never differed)'),
    ('^gUnk_0203AD10$', None, 'V', 'kirby.c:10656/10692 (bit 0x100 while the screen\'s Kirby dies)', 'mutes some sound effects (code_0806F780.c:6082...) -- sound only'),
    ('^gUnk_0203AD18$', None, 'V', 'code_0806F780.c:1059 (shake of the screen\'s room only)', 'screen shake offsets (sprite and BG offsets)'),
    ('^gUnk_02022F50$', None, 'H', 'object_collision.c:596 (collision group slots: pointers to objects)',
     'pointers to the same objects at other heap addresses'),
    ('^gPauseMenus$', None, 'V', 'pause_help.c PauseMenuFetchInputs (keys of whoever paused, into the screen player\'s menu), pause_*.c via gPauseMenus[gLocalPlayerId]',
     "each screen runs the pause menu in its own player's slot; the pauser's keys drive both the same way, the pause starts and ends on the same frame (gMainFlags 0x400 never differed)"),
    ('^gCurLevelInfo$', 'unkC0|unk180|altViewport|unk5FC|unk63C|viewportMod|unk65C|unk662|unk664', 'V',
     'LoadLevelGfx, BgInit/bg.c streaming, code_080023A4.c:1215-2050 (animated tiles and palettes of sub_08002374_2(), the display player), cutscene_trigger.c:404/968 and code_0806F780.c:1059 (shake on gCurLevelInfo[gLocalPlayerId]), sub_08001358 (music)',
     "BG layers, streaming, animation phases, shake and music of the screen's room; the spawn margin read unkC0[1].unk14/16 (fixed: platform/view.c)"),
    ('^gCurLevelInfo$', 'unk1F0', 'H', 'code_080023A4.c sub_08002918/sub_080028CC (object node lists)',
     'per-room object node lists: pointers into the node pool, allocated around view-only spawns'),
    ('^gKirbys$', 'sprite', 'V', 'kirby.c sub_0803F46C (screen position), hud.c DrawHudOtherKirbys (0x80000 hide), Macro_0803E920 (draw order)',
     "the Kirbys' sprites as drawn on this screen"),
    ('^gKirbys$', 'unk6C|unk74|parent', 'H', 'pointers to heap objects (held/attached objects)', 'same object, other address'),
    ('^gInGameInterludeTask$', None, 'H', 'in_game_interlude.c:109 (pointer to the pause task)', 'same task, other slot'),
    ('^gTasks$|^gTaskPtrs$|^gNumTasks$', None, 'H', 'task.c TaskCreate/TaskDestroy, view-only tasks (warp_star.c:2534, code_0806F780.c:8757/10006, HUD/pause/menu tasks)',
     'the task pool: view-only tasks take slots; order of gameplay tasks is kept (insertion by priority)'),
    ('^task (UpdateGameplayHud|PauseMenuMain|HelpMenuMain|AreaMapMain|WorldMapPauseMain|UpdateInGameInterlude|sub_0814A914|sub_08085E60|sub_080852A4|sub_08085834|sub_08087F98|sub_0800DAD8|sub_08085A54|sub_080850C8|sub_08084B1C|sub_080840B8|sub_080714A0)$', None, 'V',
     'hud.c, pause_*.c, in_game_interlude.c, effect tasks created for the screen player only',
     "the screen's HUD, pause screens and view-only effects"),
    ('^task (?!ObjectMain)', '^\\+0x(c|d|e|f|[12][0-9a-f]|3[0-3])$', 'V', 'effect objects (struct EffectObject, sprite at +0xC..+0x33): screen-relative position, tiles, palette, animation sub-state',
     'sprites of effect objects as drawn on this screen'),
    ('^task ', 'sprite', 'V', 'object.h draw macros, object.c:440-480 (screen-relative x/y, tiles and palette taken only in the screen\'s room)',
     'sprites of objects as drawn on this screen'),
    ('^task ', None, 'H', 'TaskCreate/EwramMalloc/IwramMalloc around view-only tasks',
     'heap contents at shifted addresses (whole objects moved); the digest of every live object agrees'),
    ('^g(Ewram|Iwram)Heap\\?$', None, 'H', 'EwramMalloc/IwramMalloc', 'heap blocks between tasks (allocator headers, freed blocks)'),
]


def classify(sym, field, region, addr):
    for lo, hi, n in SOUND:
        if lo <= addr < hi:
            return 'VS', 'm4a engine (m4a.c), PlaySfx gated by room (include/data.h)', n + ': sound of this screen'
    for rs, rf, cls, writer, why in RULES:
        if re.search(rs, sym) and (rf is None or re.search(rf, field or '')):
            return cls, writer, why
    return 'UNEXPLAINED', '', ''
