#!/usr/bin/env python3
"""
portify.py -- turn the KATAM decompilation's agbcc-targeted C into C that a
modern compiler will accept for a non-ARM target (wasm32).

The decomp is written to make `agbcc` emit byte-identical ARM code.  That goal
pulls in constructs that only exist to steer that specific compiler:

  * hard register pins   register s32 x asm("r0");
  * codegen barriers     asm(""); asm("" : "+r"(v)); asm("":::"memory");
  * NAKED asm wrappers   NAKED f(void) { asm(".include \"asm/nonmatching/f.inc\""); }
  * a few real inline asm statements (8 sites)

None of it compiles for wasm.  All of it is mechanical to remove, and none of
it changes program semantics -- the pins and barriers exist to influence
register allocation and scheduling, not behaviour.

This script never edits the decomp in place.  It copies the tree and rewrites
the copy, so the port can track upstream by re-running the copy.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cxxify
import narrow32

# ---------------------------------------------------------------------------
# transforms
# ---------------------------------------------------------------------------

# A hard register pin is an ` asm("r4")` suffix on a declarator.  Removing just
# the suffix handles every form the decomp uses, including several declarators
# pinned in one declaration:
#     register s32 r0 asm("r0"), r4 asm("r4");
#     register struct Foo *p asm("r8") = q;
#     register const s32 *c asm("ip") = gTable;
# The `register` keyword itself is left alone -- it is standard C.
RE_REG_PIN = re.compile(
    r'\s*\basm\s*\(\s*"(?:r\d+|sl|sb|ip|fp|sp|lr)"\s*\)(?=\s*[,;=])')

# whole inline-asm statements
RE_ASM_STMT = re.compile(r'\b(?:__asm__|asm)\s*(?:__volatile__|volatile)?\s*\((?P<body>(?:[^()"]|"(?:[^"\\]|\\.)*"|\((?:[^()"]|"(?:[^"\\]|\\.)*")*\))*)\)\s*;')

RE_OUT_CONSTRAINT = re.compile(r'"[=+][^"]*"\s*\(\s*([^)]+?)\s*\)')
RE_IN_OPERAND = re.compile(r'"r"\s*\(\s*([^)]+?)\s*\)')

# NAKED f(args) { asm(".include ..."); }
RE_NAKED_WRAPPER = re.compile(
    r'NAKED\s+(?P<ret>[A-Za-z_][\w\s\*]*?)\s+(?P<name>\w+)\s*\((?P<args>[^)]*)\)\s*\{'
    r'\s*asm\s*\(\s*"\.include[^;]*?;\s*\}',
    re.S)


# Files the port replaces wholesale rather than compiles.  Each one is a driver
# for hardware the port does not have; the platform layer supplies the same
# symbols.  Keeping them out of the build is what lets the port ignore the
# sound engine entirely.
REPLACED_FILES = {
    # m4a.c is *not* here: the sound driver's C half compiles as written, with
    # the four patches in M4A_PATCHES below.  What has no C anywhere is
    # asm/m4a_asm.s -- the sequencer and the mixer -- and that is
    # platform/m4a_mixer.c.
    #
    # m4a_tables.c stays out because every table in it is already reachable.
    # Six are pure data sitting in the ROM the player supplied, and
    # tools/gen_rom_data.py already emits address macros for them
    # (gScaleTable, gFreqTable, gPcmSamplesPerVBlankTable, gCgbScaleTable,
    # gCgbFreqTable, gNoiseTable, gCgb3Vol).  Compiling the C copies as well
    # would collide with those macros for no gain.  The three that hold
    # function pointers -- gMPlayJumpTableTemplate, gXcmdTable and, in the same
    # spirit, gClockTable -- cannot come from ROM at all, because a ROM entry
    # is an ARM code address and calling one in wasm is out of bounds; they are
    # rebuilt in C in platform/m4a_mixer.c.
    'm4a_tables.c': 'MP2K tables -- data comes from ROM, function tables from '
                    'platform/m4a_mixer.c',
    'multi_boot.c': 'link-cable multiboot -- hand-written ARM asm, stubbed',
    'agb_sram.c':   'SRAM library -- calls its own machine code copied into a '
                    'stack buffer; replaced by platform/sram.c',
}

# Files whose `#else` (NONMATCHING) branch does not compile.  Nothing in the
# decomp's own workflow ever builds those branches, so they rot quietly; this
# one references a variable that only the matching branch declares.  For these
# files the port takes the matching branch instead, which is equivalent C.
# Under CHECK_POINTERS the destructor call in TaskDestroy is routed through
# platform/checks.c, which knows every destructor the game installs and can
# therefore say whether a bad pointer is a mistyped function or a corrupted
# task.  The message wasm gives conflates the two.
TASK_DTOR_CALL = 'task->dtor(task);'
TASK_DTOR_CHECKED = ('''#ifdef PORT_CHECK_POINTERS
                PortCallDtor(task);
#else
                task->dtor(task);
#endif''')

UNDEF_NONMATCHING = {
    'cookin.c': 'NONMATCHING branch uses `obj`, declared only in the other branch',
}

# Local prototypes that disagree with the real definition.  On ARM a wrong
# return type is harmless -- the value sits in r0 and the caller ignores it --
# so these went unnoticed.  wasm validates signatures, and a mismatch links to
# a stub that traps the moment it is called, so each one has to be corrected.
# Casts of a function to a different function-pointer type.  Faithful on ARM,
# where the extra argument sits unread in r0; a trap in wasm, which checks the
# callee's type at every indirect call.  Each is redirected to an adapter in
# platform/adapters.c that has the signature the call site expects.
FNPTR_CASTS = {
    'code.c': [
        ('(TaskDestructor)sub_08002E3C', 'PortDtor_sub_08002E3C'),
    ],
    'code_0802B4A8.c': [
        ('(TaskMain)sub_0802D528', 'PortMain_sub_0802D528'),
        ('(TaskMain)sub_0802D53C', 'PortMain_sub_0802D53C'),
        ('(TaskMain)sub_0802D550', 'PortMain_sub_0802D550'),
    ],
}

# Reads through a pointer built from a field that is not initialised yet.  On
# ARM these land on an address the GBA does not decode, which returns open bus
# and costs nothing -- and in each case here the value is discarded before it
# reaches the screen.  WebAssembly has no open bus: the address is outside
# linear memory and the load traps.
#
# Each read is redirected through a helper in platform/adapters.c that checks
# the pointer before following it.  The helpers carry the full reasoning, and
# docs/DECOMP-REQUESTS.md asks the decompilation for a guard so the port does
# not have to patch the text.
WILD_READS = {
    'code_08032E98.c': (
        [('gUnk_08D6CD0C[gKirbys[gUnk_0203AD3C].base.base.base.roomId]->unk46',
          'PortRoomTilesetIndex(gKirbys[gUnk_0203AD3C].base.base.base.roomId)')],
        'u16 PortRoomTilesetIndex(u16 roomId);\n',
    ),
}

# Dereferences of a pointer that is still null.
#
# These are a different animal from WILD_READS above.  A wild read follows a
# garbage address and lands somewhere undecoded; these follow *zero*, and on
# the GBA address 0x00000002 is the BIOS ROM -- a real, readable address.  The
# hardware returns open bus for a read from outside the BIOS, the value is
# compared against something it cannot equal, and the branch is not taken.
#
# WebAssembly does not trap on this either: address 2 is ordinary low linear
# memory and reads as zero, so the port has been running these all along
# without noticing.  A native process is the first host that cannot do it --
# every operating system reserves page zero and refuses to map it (Linux via
# vm.mmap_min_addr, which is 65536 by default; macOS via __PAGEZERO; Windows
# reserves the first 64 KiB outright), so the load is a segfault.
#
# The guard is written to be exactly what the hardware does, not what would be
# tidier: null means "there is no such task", the comparison against open bus
# was never going to match, and the branch was never taken.
#
# docs/DECOMP-REQUESTS.md asks upstream for the guard so the port does not have
# to patch the text.
NULL_DEREFS = {
    'task.c': [(
        """            if (slow->next == gNextTask->prev) {""",
        """            /* PORT: gNextTask is null until the first TasksExec, and
             * CreateLogo builds a task before then.  On hardware this read
             * lands in the BIOS and returns open bus; in wasm it reads zero;
             * natively page zero cannot exist at all.  Either way the branch
             * was never taken, so say so. */
            if (gNextTask != NULL && slow->next == gNextTask->prev) {""",
    )],
}

# Transfers written by poking the DMA registers directly instead of going
# through the DmaSet macro.
#
# The port emulates DMA in software: PortDmaSet does the copy, and DmaSet is
# redirected to it.  A raw store to 0x040000D4 therefore moves nothing at all --
# it lands in the IO shadow and returns.  There is no way to trap it, because
# the GBA map is ordinary wasm linear memory with no write hook.
#
# There is exactly one of these in the game, and it is the reason no level
# tilemap was ever written: sub_08153184's scrolling-BG row copy.  It is
# open-coded purely to reach a 95.2% codegen match, and the decompilation left
# the equivalent call in place as a comment on the line above -- that comment is
# what this substitutes back in.  The control word it builds, 0x8000 << 16, is
# DMA_ENABLE | DMA_START_NOW | DMA_16BIT | DMA_SRC_INC | DMA_DEST_INC, which is
# exactly what DmaCopy16 expands to, and the count arithmetic is identical.
RAW_DMA = {
    'bg.c': [(
        """                                vu32 *dmaRegs = (vu32 *)(0x4000000 + 0xd4);
                                s32 size;
                                size = r8 - 1;
                                dmaRegs[0] = (vu32)(r2);
                                dmaRegs[1] = (vu32)(r4);
                                dmaRegs[2] = (vu32)((0x8000 | 0x0000 | 0x0000 | 0x0000 | 0x0000) << 16 | (((r6->unk26 - (size)) * sp8)/(16/8)));
                                //dmaRegs[2] = (vu32)((0x8000 | 0x0000 | 0x0000 | 0x0000 | 0x0000) << 16 | (((r6->unk26 - ({ r8 - 1; })) * sp8)/(16/8)));
                                dmaRegs[2];""",
        """                                /* PORT: was an open-coded DMA3 register poke.
                                 * Software DMA cannot see raw register stores,
                                 * so this moved nothing and every scrolling
                                 * background's tilemap stayed at its fill
                                 * value.  This is the decompilation's own
                                 * commented-out equivalent, restored. */
                                DmaCopy16(3, r2, r4, (r6->unk26 - (r8 - 1)) * sp8);""",
    )],
}

CODE_IN_RAM = {
    # The link driver's other copy-code-into-RAM-and-call-it trick.
    #
    # MultiSioInit copies the machine code of MultiSioRecvBufChange into an
    # IWRAM buffer so the routine cannot be delayed by a cartridge waitstate,
    # and MultiSioRecvDataCheck then calls the buffer.  agb_sram.c did the same
    # thing and it was the first thing to take this port down after boot: in
    # wasm, code is not addressable as data and a function pointer is a table
    # index, so both the copy and the call are nonsense.
    #
    # The call is inside the game's own C, so unlike the interrupt-table case
    # -- which platform/main.c catches by recognising the buffer's address --
    # there is nowhere for the platform layer to intervene.  Point it at the
    # function instead.  platform/multi_sio_intr.c defines it.
    'multi_sio.c': [(
        "    u32 (*multiSioRecvBufChangeOnRam)(void) = "
        "(u32 (*)(void))gMultiSioRecvFuncBuf;",
        "    /* PORT: was a call through gMultiSioRecvFuncBuf, the IWRAM copy\n"
        "     * of this routine's machine code.  See tools/portify.py. */\n"
        "    u32 (*multiSioRecvBufChangeOnRam)(void) = MultiSioRecvBufChange;",
    )],
}


def trace_star_states(text, rep):
    """Report every warp-star / goal-star state handler as it runs.

    The star at the end of a level stops advancing: Kirby boards it and it
    never launches, with no trap and no freeze.  Picking a few handlers to
    instrument by reading the code did not narrow it down -- each candidate
    turned out to be reachable and self-driving, and the chain has 32 states
    across two struct views of the same object.

    So trace all of them.  PortTrace reports each distinct tuple once and caps
    per site, so a run produces one line per state actually entered, in order,
    and then goes quiet.  One session then shows the whole path the star took
    and exactly where it stopped -- which is what a state machine that hangs
    somewhere unreachable by a scripted run requires.

    PortTrace's prototype comes from platform/port/prelude.h, which is
    force-included ahead of every translation unit.  It has to come from
    somewhere: warp_star.c includes no port header, so for two builds this
    call was an implicit declaration, wasm-ld resolved `int PortTrace()`
    against `void PortTrace(...)` by pointing the call at a stub whose body is
    `unreachable`, and all 32 handlers below trapped the moment they ran.  The
    instrumentation killed the thing it was measuring and printed nothing.
    Anything injected into the game's own sources needs a prototype there.

    Diagnosis, not code: delete this once the bug is understood.
    """
    pattern = re.compile(
        r'^static void (sub_[0-9A-F]+)\(struct (?:WarpStar|GoalStar) '
        r'\*(\w+)\)\n\{\n', re.M)

    def sub(m):
        name, arg = m.group(1), m.group(2)
        rep.bump('warp-star state traces inserted')
        # Riders, the state's own timer, and Kirby 0's animation -- between
        # them these identify every wait condition in the file.
        return ('%s    PortTrace("star state %s: riders, timer, kirby0Anim",\n'
                '              %s->unk0.unkB5, %s->unkBC,\n'
                '              gKirbys[0].animationIndex);\n'
                % (m.group(0), name, arg, arg))

    return pattern.sub(sub, text)


FNPTR_ADAPTER_DECLS = {
    'code.c': 'void PortDtor_sub_08002E3C(struct Task *);\n',
    'code_0802B4A8.c': ('void PortMain_sub_0802D528(void);\n'
                        'void PortMain_sub_0802D53C(void);\n'
                        'void PortMain_sub_0802D550(void);\n'),
}


DECL_FIXES = {
    'code_0802E57C.c': [
        ('void sub_0802F8D8(struct Unk_0802E57C *',
         'void *sub_0802F8D8(struct Unk_0802E57C *'),
        ('void sub_0802FA40(struct Unk_0802E57C *',
         'void *sub_0802FA40(struct Unk_0802E57C *'),
    ],
}


# Every object in the game reaches its own state through TaskGetStructPtr, so
# it is the one place worth range-checking when a bad pointer is trashing
# memory.  Under CHECK_POINTERS the macro is redirected into platform/checks.c;
# otherwise the original text is used unchanged.
TASK_PTR_ORIGINAL = """#define TaskGetStructPtr(taskp) \\
    ((taskp)->flags & TASK_USE_EWRAM \\
    ? (void *)EWRAM_START + ((taskp)->structOffset << 2) \\
    : (void *)IWRAM_START + (taskp)->structOffset)"""

TASK_PTR_CHECKED = """#ifdef PORT_CHECK_POINTERS
struct Task;
void *PortTaskStruct(struct Task *);
#define TaskGetStructPtr(taskp) PortTaskStruct(taskp)
#else
""" + TASK_PTR_ORIGINAL + """
#endif"""


# Save memory is the only thing that pushes the reserved GBA map past the ROM.
# The hardware puts it at 0x0E000000, which leaves an 80 MiB hole between the
# end of the ROM and the start of it -- and the whole map has to be reserved as
# one contiguous wasm memory, so that hole costs 80 MiB of allocation on every
# device that opens the page.  A phone will refuse an allocation a desktop
# shrugs at.
#
# Nothing about 0x0E000000 matters here: no ROM pointer reaches it and the port
# supplies the SRAM routines itself (platform/sram.c).  Moving it to just above
# the 16 MiB ROM shortens the reservation from 272 MiB to 192 MiB.  The address
# is hardcoded in two places besides the header, which is why this is a text
# rewrite rather than a #define.
PORT_SRAM_BASE = 0x09000000

# The sound driver.  src/m4a.c is the outer half of MP2K -- the public API, the
# song/player bookkeeping, the CGB driver, the extended commands -- and all of
# it compiles as written.  Four things in it do not survive the move, and each
# is patched at the narrowest point that works.  The inner half (the sequencer
# and the mixer, asm/m4a_asm.s) has no C anywhere and is platform/m4a_mixer.c.
#
# The same table is applied to gba/m4a.h, where three prototypes disagree with
# what the assembly actually is.  On ARM that never mattered: an argument the
# callee ignores just sits in a register.  wasm checks the type at every
# indirect call, and each of these *is* reached indirectly -- MPlayMain through
# soundInfo->func, SoundMainBTM through gMPlayJumpTable[35], ply_note through
# soundInfo->plynote -- so a wrong prototype is a trap, not a warning.
M4A_PATCHES = {
    'm4a.c': [
        # --- the common_data globals ---------------------------------------
        # Deleted here and redefined as fixed addresses in platform/port/
        # prelude.h; the reasoning is written out there.  The short version is
        # that gMPlayTable in ROM already holds pointers to these objects, so
        # they have to be at the addresses the ROM names or m4aSoundInit and
        # the game's own &gMPlayInfo_1 end up looking at different memory.
        ("""char SoundMainRAM_Buffer[0x400] __attribute__((section(".bss.code"), aligned(4))) = {};

struct SoundInfo gSoundInfo __attribute__((section("common_data"))) = {};
MPlayFunc gMPlayJumpTable[36] __attribute__((section("common_data"))) = {};
struct CgbChannel gCgbChans[4] __attribute__((section("common_data"))) = {};
struct MusicPlayerInfo gMPlayInfo_0 __attribute__((section("common_data"))) = {};
struct MusicPlayerInfo gMPlayInfo_1 __attribute__((section("common_data"))) = {};
struct MusicPlayerInfo gMPlayInfo_2 __attribute__((section("common_data"))) = {};
u8 gMPlayMemAccArea[0x10] __attribute__((section("common_data"))) = {};
struct MusicPlayerInfo gMPlayInfo_3 __attribute__((section("common_data"))) = {};""",
         """/* PORT: gSoundInfo, gMPlayJumpTable, gCgbChans, gMPlayInfo_0..3 and
 * gMPlayMemAccArea are defined as fixed GBA addresses in
 * platform/port/prelude.h -- gMPlayTable in ROM points at them.  The
 * `common_data` section attribute is a linker-script placement with no
 * meaning for wasm-ld either way. */
char SoundMainRAM_Buffer[0x400];

/* gXcmdTable in ROM is twelve ARM code addresses; gen_rom_data.py's macro for
 * it would compile, and calling through it would take the module out.  The
 * real table is rebuilt in C in platform/m4a_mixer.c -- under a different
 * name, because gen_rom_data.py comments out any `extern ... gXcmdTable[];`
 * it finds, including one added here. */
#undef gXcmdTable
extern const XcmdFunc gPortXcmdTable[];
#define gXcmdTable gPortXcmdTable

void PortSampleRateSet(struct SoundInfo *soundInfo);"""),

        # --- the sample rate ------------------------------------------------
        # Two problems in one function.  The device rate is the browser's to
        # choose, not one of the GBA's twelve; and both VCOUNT spins would hang
        # forever, because VCOUNT is plain memory in this port and nothing
        # advances it from inside this call.  That second one is not a
        # cosmetic difference -- it is a boot hang.
        ("""    soundInfo->pcmSamplesPerVBlank = gPcmSamplesPerVBlankTable[freq - 1];
    soundInfo->pcmDmaPeriod = PCM_DMA_BUF_SIZE / soundInfo->pcmSamplesPerVBlank;
    // LCD refresh rate 59.7275Hz
    soundInfo->pcmFreq = (597275 * soundInfo->pcmSamplesPerVBlank + 5000) / 10000;
    // CPU frequency 16.78Mhz
    soundInfo->divFreq = (0x1000000 / soundInfo->pcmFreq + 1) >> 1;
    // Turn off timer 0.
    REG_TM0CNT_H = 0;
    // cycles per LCD fresh 280896
    REG_TM0CNT_L = -(280896 / soundInfo->pcmSamplesPerVBlank);
    m4aSoundVSyncOn();
    while (*(vu8 *)REG_ADDR_VCOUNT == 159)
        ;
    while (*(vu8 *)REG_ADDR_VCOUNT != 159)
        ;
    REG_TM0CNT_H = TIMER_ENABLE | TIMER_1CLK;""",
         """    /* PORT: the AudioContext picks the rate and the mixer renders natively
     * at it, so none of the twelve fixed rates or Timer 0 applies.  The two
     * VCOUNT spins that followed are gone because they cannot terminate --
     * VCOUNT is ordinary memory here and only the frame loop advances it, and
     * this runs inside m4aSoundInit long before the first frame. */
    PortSampleRateSet(soundInfo);
    m4aSoundVSyncOn();"""),

        # --- three function pointers stored at the wrong type ----------------
        ("""    soundInfo->CgbOscOff = (void (*)(u8))nullsub_141;
    soundInfo->MidiKeyToCgbFreq = (u32 (*)(u8, u8, u8))nullsub_141;""",
         """    /* PORT: nullsub_141 is void(void).  Storing it as void(u8) and
     * u32(u8,u8,u8) is free on ARM -- the arguments sit unread in r0-r2 --
     * and traps in wasm, which checks the callee's type at every indirect
     * call.  MPlayExtender overwrites both a moment later and SoundClear
     * guards on cgbChans being non-null, so neither is reachable today; a
     * wasm trap is unrecoverable and this is one line. */
    soundInfo->CgbOscOff = PortNullCgbOscOff;
    soundInfo->MidiKeyToCgbFreq = PortNullMidiKeyToCgbFreq;"""),

        # --- a BIOS call nothing makes --------------------------------------
        ("""void MusicPlayerJumpTableCopy(void)
{
    asm("swi 0x2A");
}""",
         """void MusicPlayerJumpTableCopy(void)
{
    /* PORT: BIOS MusicPlayerOpen (swi 0x2A).  Nothing in the game calls this
     * -- SoundInit uses the library's own MPlayJumpTableCopy instead. */
}"""),
    ],

    'm4a.h': [
        ("extern u8 gMPlayMemAccArea[0x10];",
         "/* PORT: fixed address, see platform/port/prelude.h */"),
        ("extern MPlayFunc gMPlayJumpTable[36];\n"
         "extern struct MusicPlayerInfo gMPlayInfo_0;\n"
         "extern struct MusicPlayerInfo gMPlayInfo_1;\n"
         "extern struct MusicPlayerInfo gMPlayInfo_2;\n"
         "extern struct MusicPlayerInfo gMPlayInfo_3;",
         "/* PORT: fixed addresses, see platform/port/prelude.h */"),
        ("extern struct CgbChannel gCgbChans[4];",
         "/* PORT: fixed address, see platform/port/prelude.h */"),
        ("extern char gNumMusicPlayers[];\nextern char gMaxLines[];",
         "/* PORT: linker.ld sets these to the literal values 4 and 0, and the\n"
         " * two macros below cast the address to an integer.  prelude.h. */"),

        # MPlayMain is reached only as soundInfo->func, always with the player
        # in r0; the chain of four players in MPlayOpen depends on it.
        ("void MPlayMain(void);",
         "void MPlayMain(struct MusicPlayerInfo *);  /* PORT: takes the player */"),
        # SoundMainBTM is gMPlayJumpTable[35], which Clear64byte calls as
        # void(*)(void *).
        ("void SoundMainBTM(void);",
         "void SoundMainBTM(void *);  /* PORT: it is Clear64byte's worker */"),
        # ply_note is soundInfo->plynote, called as (cmd - 0xCF, player, track).
        ("void ply_note(struct MusicPlayerInfo *, struct MusicPlayerTrack *);",
         "void ply_note(u32, struct MusicPlayerInfo *, struct MusicPlayerTrack *);"),

        ("void CgbOscOff(u8);",
         "void CgbOscOff(u8);\nvoid PortNullCgbOscOff(u8);\n"
         "u32 PortNullMidiKeyToCgbFreq(u8, u8, u8);"),
    ],
}


SRAM_RELOC = {
    'save.c': [
        ('(u8 *)0xE000000', '(u8 *)0x09000000'),
    ],
    'agb_sram.h': [
        ('#define SRAM 0x0E000000', '#define SRAM 0x09000000'),
        ('#define SRAM_ADR                0x0e000000',
         '#define SRAM_ADR                0x09000000'),
    ],
}


# INCBIN in the GBA build is expanded by the decomp's own `preproc` tool, which
# reads the referenced binary and pastes it in as an initialiser.  The port has
# no preproc in its pipeline, so it does the same expansion here.  The assets
# are read out of the decomp checkout at build time and land in build/ -- the
# port itself ships no game data.
RE_INCBIN = re.compile(r'INCBIN_([US](?:8|16|32))\(\s*"([^"]+)"\s*\)')



def load_stub_returns(path):
    """{symbol: literal} from tools/stub_returns.conf.

    A stub that returns 0 can wedge the game when the caller reads 0 as a
    state ("not finished yet").  This file records the cases where the port
    knows a different answer is correct."""
    out = {}
    if path and Path(path).exists():
        for line in Path(path).read_text().splitlines():
            line = line.split('#')[0].strip()
            if line:
                parts = line.split()
                if len(parts) == 2:
                    out[parts[0]] = parts[1]
    return out


class Report:
    def __init__(self):
        self.counts = {}
        self.stubs = []
        self.unhandled = []
        self.missing_assets = []
        self.incbin_bytes = 0

    def bump(self, key, n=1):
        self.counts[key] = self.counts.get(key, 0) + n


def strip_register_pins(text, rep):
    def sub(m):
        rep.bump('register pins removed')
        return ''
    return RE_REG_PIN.sub(sub, text)


def rewrite_asm_statements(text, path, rep):
    """Replace inline asm with an equivalent C statement, or delete it."""

    def sub(m):
        body = m.group('body')

        # ".include" wrappers are handled separately; leave them for now.
        if '.include' in body:
            return m.group(0)

        template = body.split(':')[0].strip()
        outs = RE_OUT_CONSTRAINT.findall(body)
        ins = RE_IN_OPERAND.findall(body)

        # --- pure barriers: asm(""), asm("":::"memory"), asm("":::"r0") ------
        if template in ('""', "''"):
            if not outs:
                rep.bump('barriers removed')
                return '/* PORT: asm barrier removed */'
            # asm("" : "+r"(v))  -- v is read and written, value unchanged.
            if re.search(r'"\+r"', body):
                rep.bump('barriers removed (value preserved)')
                return ' '.join('(void)%s;' % o for o in outs)
            # asm("" : "=r"(v))  -- v takes whatever was in the register.
            # Undefined on real hardware too; make it deterministic.
            rep.bump('undefined-value barriers zeroed')
            return ' '.join('%s = 0;' % o for o in outs)

        # --- the handful of real instructions -------------------------------
        # asm("mov %0, %1" : "=r"(a) : "r"(b))
        if re.match(r'"mov\s*%0,\s*%1"', template) and len(outs) == 1 and len(ins) == 1:
            rep.bump('mov translated')
            return '%s = %s;' % (outs[0], ins[0])

        # asm("mov\t%0, #0" : "=r"(a))
        if re.match(r'"mov\\?t?\s*%0,\s*#0"', template) and len(outs) == 1:
            rep.bump('mov #0 translated')
            return '%s = 0;' % outs[0]

        # asm("ldr %0, [%1, #8]" : "=r"(a) : "r"(b))
        # asm("swi\t3") -- BIOS Halt.  The port has no interrupts to wait for;
        # the platform layer decides what halting means.
        if re.match(r'"swi\\?t?\s*3"', template):
            rep.bump('swi 3 (Halt) translated')
            return 'PortHalt();'

        mldr = re.match(r'"ldr\s*%0,\s*\[%1,\s*#(\d+)\]"', template)
        if mldr and len(outs) == 1 and len(ins) == 1:
            rep.bump('ldr translated')
            return '%s = *(u32 *)((u8 *)(%s) + %s);' % (outs[0], ins[0], mldr.group(1))

        rep.unhandled.append('%s: %s' % (path.name, body.strip()[:90]))
        return '/* PORT: UNHANDLED ASM: %s */' % body.strip()[:90].replace('*/', '* /')

    return RE_ASM_STMT.sub(sub, text)


def stub_naked_wrappers(text, rep, returns=None):
    """A NAKED function whose body is `.include`d reference assembly has no C at
    all.  Replace it with a stub that reports itself the first time it runs."""

    def sub(m):
        ret = ' '.join(m.group('ret').split())
        name = m.group('name')
        args = m.group('args').strip() or 'void'
        rep.stubs.append(name)
        rep.bump('asm wrappers stubbed')
        body = 'PortMissingFunction("%s");' % name
        if ret != 'void':
            body += ' return (%s)%s;' % (ret, (returns or {}).get(name, '0'))
        return '%s %s(%s) { %s }' % (ret, name, args, body)

    return RE_NAKED_WRAPPER.sub(sub, text)


def expand_incbin(text, decomp, rep):
    """Paste referenced binaries in as C initialisers, the way preproc does."""

    def sub(m):
        kind, rel = m.group(1), m.group(2)
        path = decomp / rel
        if not path.exists():
            rep.missing_assets.append(rel)
            return '{ 0 }'

        data = path.read_bytes()
        width = int(kind[1:]) // 8
        signed = kind[0] == 'S'
        if len(data) % width:
            data += b'\0' * (width - len(data) % width)

        values = []
        for i in range(0, len(data), width):
            v = int.from_bytes(data[i:i + width], 'little', signed=signed)
            values.append(hex(v) if v >= 0 else str(v))

        rep.bump('INCBIN files expanded')
        rep.incbin_bytes += len(data)
        return '{ ' + ', '.join(values) + ' }'

    return RE_INCBIN.sub(sub, text)


def drop_static_on_exported(text, exported, rep):
    """Remove `static` where a header also declares the symbol `extern`.

    agbcc (gcc 2.95) accepted a static definition of a symbol that a header had
    already declared extern; clang rejects it outright.  The header is the
    authority here -- other files reference these -- so the definition loses
    its `static`."""

    def sub(m):
        if m.group('name') in exported:
            rep.bump('redundant `static` dropped')
            return m.group(0).replace('static', '/* PORT: static */', 1)
        return m.group(0)

    return re.sub(r'^static\s+(?:const\s+)?[\w\s\*]*?\b(?P<name>\w+)\s*\[',
                  sub, text, flags=re.M)


def apply_m4a_patches(text, name, rep):
    """The sound driver's four source patches and its header's three wrong
    prototypes.  A pattern that stops matching is reported rather than skipped:
    silently not applying the SampleFreqSet patch gives a boot that hangs in a
    VCOUNT spin, which is a long way from the symptom back to this file."""
    for old, new in M4A_PATCHES.get(name, ()):
        if old in text:
            text = text.replace(old, new, 1)
            rep.bump('m4a sites patched')
        else:
            rep.unhandled.append(
                '%s: an M4A_PATCHES pattern no longer matches -- the sound '
                'driver has changed upstream: %s'
                % (name, ' '.join(old.split())[:70]))
    return text


def rewrite_source(text, path, rep, decomp=None, exported=None, returns=None):
    text = strip_register_pins(text, rep)
    text = stub_naked_wrappers(text, rep, returns)
    text = rewrite_asm_statements(text, path, rep)
    if decomp is not None:
        text = expand_incbin(text, decomp, rep)
    if exported:
        text = drop_static_on_exported(text, exported, rep)
    # NAKED itself is left alone: it is a macro, and the defines.h override
    # expands it to nothing.  Rewriting the text would corrupt the `#define
    # NAKED ...` line in the header that declares it.
    return text


# ---------------------------------------------------------------------------
# header overrides
# ---------------------------------------------------------------------------

MACRO_OVERRIDE = '''\
#ifndef GUARD_PORT_GBA_MACRO_H
#define GUARD_PORT_GBA_MACRO_H

/* Generated by tools/portify.py -- do not edit.
 *
 * The original header is preserved as macro_agb.h.  Everything in it is fine
 * on a host except the DMA macros, which work by writing the DMA control
 * register and relying on the hardware to notice.  There is no hardware here,
 * so those three macros are redirected into the platform layer.  Every other
 * Dma* macro in the decomp is built on top of DmaSet, so this catches them
 * all. */

#include "gba/macro_agb.h"
#include "port/dma.h"

#undef DmaSet
#define DmaSet(dmaNum, src, dest, control) \\
    PortDmaSet((dmaNum), (const void *)(src), (void *)(dest), (u32)(control))

#undef DmaStop
#define DmaStop(dmaNum) PortDmaStop(dmaNum)

/* Our DMA is synchronous, so there is never anything to wait for. */
#undef DmaWait
#define DmaWait(dmaNum) ((void)0)

#endif /* GUARD_PORT_GBA_MACRO_H */
'''

DEFINES_OVERRIDE = '''\
#ifndef GUARD_PORT_GBA_DEFINES
#define GUARD_PORT_GBA_DEFINES

/* Generated by tools/portify.py -- do not edit.
 *
 * The original header is preserved as defines_agb.h.  The GBA memory-map
 * constants in it (VRAM, PLTT, OAM, EWRAM_START ...) are kept exactly as they
 * are: the port reserves those addresses inside the wasm linear memory, so
 * `(void *)VRAM` is a genuinely valid pointer.  See docs/ARCHITECTURE.md.
 *
 * Only the two section attributes have to go -- `ewram_data` / `iwram_data`
 * are ARM linker-script sections with no meaning for wasm-ld. */

#include "gba/defines_agb.h"

#undef IWRAM_DATA
#define IWRAM_DATA
#undef EWRAM_DATA
#define EWRAM_DATA
#undef NAKED
#define NAKED

/* The BIOS interrupt vector is the last *word* of IWRAM -- four bytes at
 * 0x3007FFC, with the region ending at 0x3008000.  defines_agb.h spells it
 * `(*(void **)0x3007FFC)`, which on a 64-bit host is an eight-byte store
 * running four bytes past the end of the mapping, and the write segfaults.
 *
 * PTR32 keeps it four bytes there and is a plain `void *` everywhere else, so
 * the ILP32 builds see exactly what they saw before.  This is the same problem
 * as a structure's pointer member and not the same fix, because there is no
 * structure -- it is a naked address, which is why the layout assertions have
 * nothing to say about it.  docs/SIXTYFOUR.md's warning about the ~190
 * linker-placed symbols whose extents nothing asserts is precisely this. */
#undef INTR_VECTOR
#define INTR_VECTOR (*(PTR32(void) *)0x3007FFC)

#endif /* GUARD_PORT_GBA_DEFINES */
'''


def write_overrides(out_include, rep):
    gba = out_include / 'gba'
    (gba / 'macro.h').rename(gba / 'macro_agb.h')
    (gba / 'macro.h').write_text(MACRO_OVERRIDE)
    (gba / 'defines.h').rename(gba / 'defines_agb.h')
    (gba / 'defines.h').write_text(DEFINES_OVERRIDE)
    rep.bump('headers overridden', 2)


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decomp', required=True, type=Path,
                    help='path to the katam decompilation checkout')
    ap.add_argument('--out', required=True, type=Path,
                    help='directory to write the portable source tree into')
    args = ap.parse_args()

    decomp, out = args.decomp, args.out
    if not (decomp / 'include' / 'gba' / 'macro.h').exists():
        sys.exit('error: %s does not look like a katam checkout' % decomp)

    if out.exists():
        shutil.rmtree(out)
    (out / 'src').mkdir(parents=True)
    shutil.copytree(decomp / 'include', out / 'include')

    rep = Report()
    returns = load_stub_returns(Path(__file__).parent / 'stub_returns.conf')

    # The C++ compatibility pass over the headers has to run before anything
    # reads them, because the two things the source pass needs -- the real
    # prototype of every function, and which structures end in a `[0]` array --
    # are read back out of these files.  See tools/cxxify.py for why the port
    # compiles the game as C++ at all; the short version is that a 64-bit build
    # needs 4-byte pointer members and C has no way to spell one.
    #
    # Every transform is valid C as well, so this runs for all four builds
    # rather than only the 64-bit ones.  That is deliberate: it means the ILP32
    # builds are the test of it, and their frame output must not move.
    # Collected before the loop, from the headers as the decompilation wrote
    # them: which typedefs resolve to a pointer is a property of the tree, and
    # reading it back after the rewriting has begun would see a half-rewritten
    # one.
    narrow_typedefs = narrow32.pointer_typedefs(out / 'include')

    for hp in sorted((out / 'include').rglob('*.h')):
        text = hp.read_text(errors='replace')
        # cxxify first, narrow32 second, and the order is load-bearing.
        # cxxify generates a converting constructor per member type for each
        # transparent union; if narrowing has already happened those
        # constructors take a PTR32 instead of the pointer the call sites
        # actually pass, and every such call stops compiling.  Narrowing after
        # leaves the constructor signatures alone and rewrites only the member
        # declarations, which is what both halves want.
        new = cxxify.rewrite_header(text, rep, hp.name)
        new = narrow32.narrow_members(new, rep, name=hp.name,
                                      typedefs=narrow_typedefs)
        if new != text:
            hp.write_text(new)
    cxx_protos = cxxify.header_function_prototypes(out / 'include')
    cxx_flex = cxxify.flex_array_types(out / 'include')

    # Every symbol the headers declare `extern`.  A .c file that defines one of
    # these as `static` is the agbcc-ism handled in drop_static_on_exported.
    exported = set()
    for hp in (out / 'include').rglob('*.h'):
        for m in re.finditer(r'\bextern\b[^;\n]*?\b(\w+)\s*(?:\[|;)',
                             hp.read_text(errors='replace')):
            exported.add(m.group(1))

    # src/ holds finished files; wip/ holds files still being decompiled.  The
    # GBA build keeps wip/ out of the link because a partially-decompiled file
    # collides with its .s counterpart.  The port has no .s counterparts, so it
    # wants both.
    # A file can exist in both trees while it is being decompiled, and neither
    # copy is a superset of the other: src/flamer.c defines 4 functions and
    # wip/flamer.c defines 38 *different* ones, because the GBA build takes the
    # rest from asm/flamer.s.  The port has no assembly at all, so it needs
    # every body it can get -- the two copies are concatenated into one file.
    # They are complementary by construction: the same function cannot be in
    # both, or the GBA build itself would not link.
    #
    # Picking one and dropping the other is what produced the port's nastiest
    # bug so far: dropping wip/flamer.c left `nullsub_125` declared, placed in
    # a dispatch table, and never defined.  wasm-ld resolves an address-taken
    # undefined function to a null table entry rather than failing the link, so
    # nothing complained until the game dispatched through it and died.
    #
    # If the two copies ever do overlap, this is a duplicate-symbol link error
    # -- loud, and far easier to diagnose than the silent version above.
    written = {}
    for tree in ('src', 'wip'):
        srcdir = decomp / tree
        if not srcdir.exists():
            continue
        for path in sorted(srcdir.rglob('*.c')):
            if path.name in REPLACED_FILES:
                rep.bump('files replaced by platform layer')
                continue
            dst = written.get(path.name, out / 'src' / path.relative_to(srcdir))
            dst.parent.mkdir(parents=True, exist_ok=True)
            text = rewrite_source(path.read_text(errors='replace'), path, rep,
                                  decomp=decomp, exported=exported,
                                  returns=returns)
            if path.name == 'task.c' and TASK_DTOR_CALL in text:
                text = text.replace(TASK_DTOR_CALL, TASK_DTOR_CHECKED, 1)
                text = 'void PortCallDtor(struct Task *);\n' + text
                rep.bump('TaskDestroy dtor call made checkable')
            for old, new in SRAM_RELOC.get(path.name, ()):
                if old in text:
                    text = text.replace(old, new)
                    rep.bump('save-memory address relocated')
            text = apply_m4a_patches(text, path.name, rep)
            casts = FNPTR_CASTS.get(path.name, ())
            if casts:
                hit = False
                for old, new in casts:
                    if old in text:
                        text = text.replace(old, new)
                        rep.bump('mis-cast function pointers adapted')
                        hit = True
                if hit:
                    text = FNPTR_ADAPTER_DECLS[path.name] + text
            if path.name == 'warp_star.c':
                text = trace_star_states(text, rep)
            for old, new in RAW_DMA.get(path.name, ()):
                if old in text:
                    text = text.replace(old, new)
                    rep.bump('raw DMA register writes routed through PortDmaSet')
                else:
                    rep.unhandled.append(
                        '%s: RAW_DMA pattern no longer matches -- the tilemap '
                        'blitter has changed upstream' % path.name)
            for old, new in CODE_IN_RAM.get(path.name, ()):
                if old in text:
                    text = text.replace(old, new)
                    rep.bump('calls through copied machine code redirected')
                else:
                    rep.unhandled.append(
                        '%s: a CODE_IN_RAM pattern no longer matches -- a call '
                        'through a RAM copy of a function is left in place and '
                        'will trap: %s' % (path.name, ' '.join(old.split())[:70]))
            for old, new in NULL_DEREFS.get(path.name, ()):
                if old in text:
                    text = text.replace(old, new, 1)
                    rep.bump('null dereferences guarded')
                else:
                    rep.unhandled.append(
                        '%s: a NULL_DEREFS pattern no longer matches -- if the '
                        'decompilation has guarded it upstream, drop the entry; '
                        'if not, the native build will segfault' % path.name)
            wild = WILD_READS.get(path.name)
            if wild:
                reads, decl = wild
                hit = False
                for old, new in reads:
                    if old in text:
                        text = text.replace(old, new)
                        rep.bump('reads through not-yet-valid pointers guarded')
                        hit = True
                if hit:
                    text = decl + text
            for old, new in DECL_FIXES.get(path.name, ()):
                if old in text:
                    text = text.replace(old, new)
                    rep.bump('conflicting prototypes corrected')
            if path.name in UNDEF_NONMATCHING:
                text = ('/* PORT: %s */\n#undef NONMATCHING\n\n'
                        % UNDEF_NONMATCHING[path.name]) + text
                rep.bump('files built from the matching branch instead')
            # Last, and before the src/wip merge rather than after it: the
            # merge concatenates this file with one that has already been
            # through here, and cxxify's site table reports a pattern that no
            # longer matches -- which a second pass over the same text would
            # trip on every time.  Each half is rewritten exactly once.
            text = cxxify.rewrite_source(text, rep, path.name, cxx_protos, cxx_flex)
            # Structures defined in a .c file rather than a header need
            # narrowing every bit as much as the ones in include/, and for a
            # while they did not get it.
            #
            # Nothing about a structure's *location* decides whether its
            # pointer members have to be four bytes.  What decides it is
            # whether anything outside this compiler cares about the layout --
            # and the game's own allocator does, for every one of these:
            #
            #     TaskCreate(ObjectMain, sizeof(struct CutsceneTrigger), ...)
            #
            # A struct that is 48 bytes bigger does not corrupt anything by
            # itself.  It makes the allocation bigger, so the fixed 0x2604-byte
            # IWRAM heap fills sooner, so some later object misses IWRAM and
            # falls back to EWRAM -- which TaskCreate does silently and by
            # design.  The two builds are still computing the same thing; they
            # have stopped keeping it in the same place, and the frame that
            # finally looks different is hundreds of frames downstream.
            #
            # 24 structures and one union were this, all defined in .c files:
            # CutsceneTrigger, AreaMap, WorldMap, Unk_080880AC and the rest.
            # tools/abi_size_diff.py is what found them and is what keeps them
            # found -- it compares every DWARF type size between the ILP32 and
            # LP64 builds, which is the check `make layout-check` cannot make
            # because a type only enters gba_layout.h if it has a known console
            # address to be checked against.
            #
            # Same ordering rule as the headers: cxxify first, narrow second.
            text = narrow32.narrow_members(text, rep, name=path.name,
                                           typedefs=narrow_typedefs)
            # A file-scope array of pointers that something casts to a
            # four-byte-strided type.  Not a structure member, so
            # narrow_members above never sees it -- see ALIASED_ARRAYS.
            text = narrow32.apply_aliased_arrays(text, path.name, rep)
            if path.name in written:
                text = ('%s\n\n/* ---- PORT: merged from %s/%s ---- */\n\n%s'
                        % (dst.read_text(), tree, path.name, text))
                rep.bump('src/wip twins merged into one file')
            else:
                written[path.name] = dst
                rep.bump('C files copied')
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(text)
        # .inc.c fragments and any per-file headers next to the sources
        for path in sorted(list(srcdir.rglob('*.h')) + list(srcdir.rglob('*.inc.c'))):
            rel = path.relative_to(srcdir)
            dst = out / 'src' / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(rewrite_source(path.read_text(errors='replace'), path, rep))

    for path in sorted((out / 'include').rglob('*.h')):
        text = path.read_text(errors='replace')
        new = rewrite_source(text, path, rep)
        for old, repl in SRAM_RELOC.get(path.name, ()):
            if old in new:
                new = new.replace(old, repl)
                rep.bump('save-memory address relocated')
        new = apply_m4a_patches(new, path.name, rep)
        if path.name == 'task.h' and TASK_PTR_ORIGINAL in new:
            new = new.replace(TASK_PTR_ORIGINAL, TASK_PTR_CHECKED)
            rep.bump('TaskGetStructPtr made checkable')
        if new != text:
            path.write_text(new)

    write_overrides(out / 'include', rep)

    (out / 'PORTIFY_REPORT.txt').write_text(format_report(rep))
    print(format_report(rep))


def format_report(rep):
    lines = ['portify.py report', '=' * 40]
    for k in sorted(rep.counts):
        lines.append('%-40s %d' % (k, rep.counts[k]))
    lines.append('%-40s %d KiB' % ('game assets pasted in via INCBIN',
                                   rep.incbin_bytes // 1024))
    if rep.missing_assets:
        lines.append('%-40s %d  <-- these will render as zeros'
                     % ('INCBIN files NOT FOUND', len(rep.missing_assets)))
    lines.append('')
    lines.append('functions stubbed (no C body exists): %d' % len(rep.stubs))
    for name in sorted(rep.stubs):
        lines.append('    %s' % name)
    if rep.unhandled:
        lines.append('')
        lines.append('UNHANDLED inline asm (%d) -- these need hand translation:' % len(rep.unhandled))
        for u in rep.unhandled:
            lines.append('    %s' % u)
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    main()
