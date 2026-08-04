/* asm/m4a_asm.s, in C: the MP2K sequencer and the sample mixer.
 *
 * This is the half of the sound driver the decompilation never had a C body
 * for.  src/m4a.c is the outer layer -- the public API, MPlayStart/Stop, the
 * CGB driver, the extended commands -- and it compiles as written.  What lives
 * only as hand-written ARM is the part that actually plays anything: SoundMain,
 * MPlayMain, ply_note and seventeen of the twenty-four base command handlers,
 * plus SoundMainRAM, the mixer itself.  Forty-four exported symbols.
 *
 * Provenance
 * ----------
 * Written from KATAM's own asm/m4a_asm.s, function by function, against the
 * struct layouts in include/gba/m4a.h.  A C reimplementation of this same
 * library does exist in the wild (the pokeemerald PC-port lineage, vendored
 * into a couple of other decompilations), and it was checked -- but nothing in
 * that chain carries a licence.  Not the file, not the repository it sits in,
 * not the repository it came from, not the decompilation upstream of that; the
 * only permission on record is an informal "thanks for letting us use it" in a
 * README, granted to somebody else.  This repository is public, so that is not
 * a thing to copy from.  Working from the game's own assembly is both cleaner
 * and, given the offsets below, not much slower.
 *
 * Confidence in the transcription
 * -------------------------------
 * Every structure offset the assembly uses was checked against the header
 * before a line of this was written, and they all land: SoundChannel is 0x40
 * and CgbChannel is too, deliberately, sharing their first 0x18 bytes and the
 * chain fields at 0x2C/0x30/0x34 -- which is what lets MPlayMain and ply_note
 * walk a track's channel list without caring which kind it is.  Where the
 * assembly is doing something that looks wrong (a word load from track+4
 * landing on four separate fields of the channel; a "repeat" that adds the
 * loop length one at a time), the comment says so.
 *
 * What is deliberately NOT a transcription
 * ----------------------------------------
 * The output stage.  SoundMainRAM packs four 8-bit samples per 32-bit word and
 * accumulates them with a rotate, because it is mixing into a DMA buffer that
 * feeds an 8-bit FIFO.  There is no FIFO here; the browser wants s16 at its own
 * sample rate.  So the envelope, the voice allocation and the phase arithmetic
 * are faithful, and the accumulation is plain s32 into a stereo scratch.  That
 * also removes the 8-bit wraparound the hardware has when several loud channels
 * add up, which is a difference from hardware and an improvement.
 *
 * The scanline budget is not implemented either.  SoundMain on hardware can be
 * asked to render only until VCOUNT passes soundInfo->maxLines and be resumed
 * by a later call.  linker.ld:2 sets `gMaxLines = 0` for this game, so the
 * branch is dead -- and it is what would otherwise force the mixer to be
 * resumable half way through a channel.
 */

#include <string.h>

#include "port/port.h"
#include "port/audio.h"
#include "gba/gba.h"
#include "gba/m4a.h"

/* m4a.c defines this but no header declares it. */
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

/* ------------------------------------------------------------------------- *
 * Tables.
 *
 * These three are the only part of src/m4a_tables.c the port needs in C.  The
 * six pure-data tables in that file (gScaleTable, gFreqTable, ...) are read
 * straight out of the player's ROM through the macros tools/gen_rom_data.py
 * emits.  These three cannot be, because a ROM entry is an ARM code address
 * and calling one in wasm is not merely wrong, it is out of bounds.
 * ------------------------------------------------------------------------- */

/* Command byte 0x80..0xB0 selects a rest length from here; a note command's
 * low bits select its gate time the same way. */
static const u8 sClockTable[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x1C, 0x1E, 0x20, 0x24, 0x28, 0x2A, 0x2C,
    0x30, 0x34, 0x36, 0x38, 0x3C, 0x40, 0x42, 0x44,
    0x48, 0x4C, 0x4E, 0x50, 0x54, 0x58, 0x5A, 0x5C,
    0x60,
};

/* Extended commands (0xCD, "xcmd").  m4a.c defines every handler; only the
 * table has to be rebuilt (under a port name -- see tools/portify.py).
 * Note entries 0 and 3 are the do-nothing ply_xxx,
 * which bounces through gMPlayJumpTable[0] -- normally ply_fine. */
const XcmdFunc gPortXcmdTable[] = {
    ply_xxx, ply_xwave, ply_xtype, ply_xxx,
    ply_xatta, ply_xdeca, ply_xsust, ply_xrele,
    ply_xiecv, ply_xiecl, ply_xleng, ply_xswee,
};

/* The command dispatch table, copied into gMPlayJumpTable by SoundInit.
 * MPlayExtender then overwrites eight of these with the extended versions --
 * that is the whole point of the copy, and why the template is separate. */
static MPlayFunc const sJumpTableTemplate[36] = {
    ply_fine,  ply_goto,  ply_patt,  ply_pend,
    ply_rept,  ply_fine,  ply_fine,  ply_fine,
    ply_fine,  ply_prio,  ply_tempo, ply_keysh,
    ply_voice, ply_vol,   ply_pan,   ply_bend,
    ply_bendr, ply_lfos,  ply_lfodl, ply_mod,
    ply_modt,  ply_fine,  ply_fine,  ply_tune,
    ply_fine,  ply_fine,  ply_fine,  ply_port,
    ply_fine,  ply_endtie,
    /* 30..35 are never reached as commands -- command bytes only cover
     * 0xB1..0xCE, which is indices 0..29 -- but m4a.c and this file call some
     * of them through the table by index. */
    (MPlayFunc)SampleFreqSet, (MPlayFunc)TrackStop,
    (MPlayFunc)FadeOutBody,   (MPlayFunc)TrkVolPitSet,
    (MPlayFunc)RealClearChain, (MPlayFunc)SoundMainBTM,
};

/* Correctly-typed replacements for `(void (*)(u8))nullsub_141`, which is what
 * SoundInit writes before MPlayExtender installs the real CGB driver.  See the
 * patch in tools/portify.py. */
void PortNullCgbOscOff(u8 chanNum) { (void)chanNum; }
u32 PortNullMidiKeyToCgbFreq(u8 a, u8 b, u8 c)
{
    (void)a; (void)b; (void)c;
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Small helpers the assembly reaches by falling through.
 * ------------------------------------------------------------------------- */

u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((u64)multiplier * multiplicand) >> 32);
}

/* gMPlayJumpTable[35]; Clear64byte's worker.  Four `stm` of four registers. */
void SoundMainBTM(void *addr)
{
    memset(addr, 0, 64);
}

void MPlayJumpTableCopy(MPlayFunc *dest)
{
    s32 i;

    /* The assembly filters each entry through a range check that rejects
     * anything not inside the ROM, which is a guard against a corrupted
     * template rather than anything functional.  A C table cannot be
     * corrupted the same way. */
    for (i = 0; i < 36; i++)
        dest[i] = sJumpTableTemplate[i];
}

/* One command byte, advancing the track's program counter.  The assembly
 * routes every such read through the same ROM range check as above; every
 * cmdPtr in this game points into the ROM, so it is a no-op. */
static u8 ReadCmd(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

/* Unlink a channel from the doubly-linked list its track keeps.  The chain
 * fields sit at the same offsets in SoundChannel and CgbChannel, which is what
 * lets one list hold both. */
void RealClearChain(void *x)
{
    struct SoundChannel *chan = (struct SoundChannel *)x;
    struct SoundChannel *next, *prev;

    if (chan->track == NULL)
        return;
    next = (struct SoundChannel *)chan->np;
    prev = (struct SoundChannel *)chan->pp;
    if (prev != NULL)
        prev->np = (u32)next;
    else
        chan->track->chan = next;
    if (next != NULL)
        next->pp = (u32)prev;
    chan->track = NULL;
}

/* ------------------------------------------------------------------------- *
 * The command handlers.
 *
 * Every one of these is called through gMPlayJumpTable as
 * (mplayInfo, track), and most of them are three instructions of ARM.
 * ------------------------------------------------------------------------- */

/* End of track: release every note it still holds and mark it dead. */
void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan = track->chan;

    (void)mplayInfo;
    while (chan != NULL) {
        if (chan->status & 0xC7)
            chan->status |= 0x40;       /* into release */
        RealClearChain(chan);
        chan = (struct SoundChannel *)chan->np;
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    const u8 *p = track->cmdPtr;

    (void)mplayInfo;
    track->cmdPtr = (u8 *)(uintptr_t)((u32)p[0] | ((u32)p[1] << 8)
                                    | ((u32)p[2] << 16) | ((u32)p[3] << 24));
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 level = track->patternLevel;

    /* Three deep, and the fourth call ends the track rather than overflowing
     * patternStack -- which is exactly what the assembly does. */
    if (level >= 3) {
        ply_fine(mplayInfo, track);
        return;
    }
    track->patternStack[level] = track->cmdPtr + 4;
    track->patternLevel = level + 1;
    ply_goto(mplayInfo, track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    if (track->patternLevel != 0) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 *p = track->cmdPtr;
    u32 count = *p;

    if (count == 0) {                   /* 0 means forever */
        track->cmdPtr = p + 1;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN++;
    track->cmdPtr = p + 1;
    if (track->repN < count) {
        ply_goto(mplayInfo, track);
    } else {
        track->repN = 0;
        track->cmdPtr = p + 5;          /* past the count and the address */
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->priority = ReadCmd(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 t = (u32)ReadCmd(track) * 2;

    mplayInfo->tempoD = t;
    mplayInfo->tempoI = (mplayInfo->tempoU * t) >> 8;
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->keyShift = ReadCmd(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 n = ReadCmd(track);

    /* Three word loads in the assembly, which is the whole 12-byte ToneData. */
    track->tone = mplayInfo->tone[n];
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->vol = ReadCmd(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->pan = (s8)(ReadCmd(track) - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bend = (s8)(ReadCmd(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bendRange = ReadCmd(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoDelay = ReadCmd(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 t = ReadCmd(track);

    (void)mplayInfo;
    if (track->modT != t) {
        track->modT = t;
        /* Both, because the modulation target has changed and whichever of
         * pitch and volume it used to drive has to be recomputed. */
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->tune = (s8)(ReadCmd(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

/* The one command that writes sound hardware directly: two bytes, a register
 * offset from NR10 and a value.  In this port that lands in the mapped I/O
 * region, where platform/cgb.c reads it. */
void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 offset = ReadCmd(track);
    u32 value = ReadCmd(track);

    (void)mplayInfo;
    *(vu8 *)(REG_ADDR_NR10 + offset) = (u8)value;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoSpeed = ReadCmd(track);
    if (track->lfoSpeed == 0)
        ClearModM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->mod = ReadCmd(track);
    if (track->mod == 0)
        ClearModM(track);
}

/* End a tie: find the channel still holding this key and put it into release. */
void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;
    u32 key;

    (void)mplayInfo;
    if (*track->cmdPtr < 0x80) {
        key = *track->cmdPtr;
        track->key = key;
        track->cmdPtr++;
    } else {
        key = track->key;
    }

    for (chan = track->chan; chan != NULL;
         chan = (struct SoundChannel *)chan->np) {
        if ((chan->status & 0x83) && !(chan->status & 0x40)
         && chan->mk == key) {
            chan->status |= 0x40;
            break;
        }
    }
}

/* Silence every channel a track owns.  A CGB channel additionally has to be
 * switched off at the oscillator, because unlike a sample channel it keeps
 * making noise until the hardware is told otherwise. */
void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct SoundChannel *chan;

    (void)mplayInfo;
    if (!(track->flags & MPT_FLG_EXIST))
        return;
    for (chan = track->chan; chan != NULL;
         chan = (struct SoundChannel *)chan->np) {
        if (chan->status != 0) {
            u32 cgb = chan->type & 7;
            if (cgb != 0)
                soundInfo->CgbOscOff(cgb);
            chan->status = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

/* Per-channel stereo volume from the track's mixed volume and the note's own
 * velocity and pan.  Called with the channel and track the caller already has
 * in registers, which is why the assembly takes no arguments at all. */
static void ChnVolSet(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    u32 velocity = chan->ve;
    s32 pan = (s8)chan->rp;
    u32 v;

    v = (velocity * (u32)(128 + pan) * track->volMR) >> 14;
    chan->rightVolume = v > 255 ? 255 : v;
    v = (velocity * (u32)(127 - pan) * track->volML) >> 14;
    chan->leftVolume = v > 255 ? 255 : v;
}

/* ------------------------------------------------------------------------- *
 * ply_note -- start a note, which mostly means choosing a channel.
 * ------------------------------------------------------------------------- */

void ply_note(u32 noteCmd, struct MusicPlayerInfo *mplayInfo,
              struct MusicPlayerTrack *track)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    struct ToneData *tone;
    struct SoundChannel *chan;
    u32 cgbType, priority, key;
    s32 rhythmPan = 0;
    s32 finalKey;

    track->gateTime = sClockTable[noteCmd];

    /* Up to three optional bytes: key, velocity, and an extra gate time.  Each
     * is present only if it is below 0x80; the first byte >= 0x80 is the next
     * command and stops the scan. */
    if (*track->cmdPtr < 0x80) {
        track->key = *track->cmdPtr++;
        if (*track->cmdPtr < 0x80) {
            track->velocity = *track->cmdPtr++;
            if (*track->cmdPtr < 0x80)
                track->gateTime += *track->cmdPtr++;
        }
    }

    key = track->key;
    tone = &track->tone;

    /* Key-split and rhythm voices are a second level of tone table: the voice
     * the track selected is really a pointer to a table of voices, indexed by
     * the key (SPL, through a mapping byte array) or used directly (RHY). */
    if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) {
        u32 index;

        if (tone->type & TONEDATA_TYPE_SPL) {
            /* `attack..release` reused as a pointer to the key-split map. */
            const u8 *map = (const u8 *)(uintptr_t)*(u32 *)&tone->attack;
            index = map[key];
        } else {
            index = key;
        }
        /* `wav` reused as the base of the sub-voice table. */
        tone = (struct ToneData *)((u8 *)tone->wav + index * 12);

        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
            return;                     /* no nesting */

        if (track->tone.type & TONEDATA_TYPE_RHY) {
            if (tone->pan_sweep & 0x80)
                rhythmPan = (s8)((tone->pan_sweep - 0xC0) * 2);
            key = tone->key;
        }
    }

    priority = track->priority + mplayInfo->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    cgbType = tone->type & 7;
    if (cgbType != 0) {
        /* A CGB voice has exactly one channel it can use, so the only question
         * is whether the note already on it may be evicted. */
        struct CgbChannel *cgb = soundInfo->cgbChans;

        if (cgb == NULL)
            return;
        cgb += cgbType - 1;
        chan = (struct SoundChannel *)cgb;
        if ((chan->status & 0xC7) && !(chan->status & 0x40)) {
            /* Evict only for a strictly better note, or an equal one from a
             * track at the same or higher address -- so a later track can take
             * the channel from an earlier one but not the reverse. */
            if (chan->pr > priority)
                return;
            if (chan->pr == priority && (uintptr_t)chan->track < (uintptr_t)track)
                return;
        }
    } else {
        /* Direct sound: pick the best of the twelve.  A channel that is free
         * wins outright.  Otherwise prefer one that is already releasing, then
         * the lowest priority, and break ties on the track address -- which is
         * the assembly's way of preferring the later track, so a track cannot
         * starve the ones before it. */
        struct SoundChannel *best = NULL;
        uintptr_t bestTrack = (uintptr_t)track;
        u32 bestPriority = priority;
        int haveReleasing = 0;
        s32 i;

        chan = &soundInfo->chans[0];
        for (i = soundInfo->maxChans; i > 0; i--, chan++) {
            if (!(chan->status & 0xC7))
                goto allocate;          /* free -- take it and stop looking */
            if (chan->status & 0x40) {
                if (!haveReleasing) {
                    haveReleasing = 1;
                    bestPriority = chan->pr;
                    bestTrack = (uintptr_t)chan->track;
                    best = chan;
                    continue;
                }
            } else if (haveReleasing) {
                continue;               /* a releasing candidate always wins */
            }
            if (chan->pr < bestPriority) {
                bestPriority = chan->pr;
                bestTrack = (uintptr_t)chan->track;
                best = chan;
            } else if (chan->pr == bestPriority
                    && (uintptr_t)chan->track >= bestTrack) {
                bestTrack = (uintptr_t)chan->track;
                best = chan;
            }
        }
        if (best == NULL)
            return;                     /* every channel outranks this note */
        chan = best;
    }

allocate:
    ClearChain(chan);
    chan->pp = 0;
    chan->np = (u32)track->chan;
    if (track->chan != NULL)
        track->chan->pp = (u32)chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        ClearModM(track);
    TrkVolPitSet(mplayInfo, track);

    /* One word load from track+4 landing on four separate channel fields.  The
     * pairs line up on purpose: {gateTime, key, velocity, runningStatus} into
     * {gt, mk, ve, pr}.  pr is overwritten immediately -- runningStatus is not
     * a priority and was never meant to end up there; it is the cost of doing
     * it in one instruction. */
    chan->gt = track->gateTime;
    chan->mk = track->key;
    chan->ve = track->velocity;
    chan->pr = priority;
    chan->ky = key;
    chan->rp = (u8)rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->echoVolume = track->echoVolume;
    chan->echoLength = track->echoLength;
    ChnVolSet(chan, track);

    finalKey = (s32)chan->ky + (s8)track->keyM;
    if (finalKey < 0)
        finalKey = 0;

    if (cgbType != 0) {
        struct CgbChannel *cgb = (struct CgbChannel *)chan;
        u32 sweep = tone->pan_sweep;

        cgb->le = tone->length;
        /* 0x80 set, or none of the sweep bits set, means "no sweep" -- and the
         * hardware's no-sweep encoding is 8, not 0. */
        if ((sweep & 0x80) || !(sweep & 0x70))
            sweep = 8;
        cgb->sw = sweep;
        cgb->fr = soundInfo->MidiKeyToCgbFreq(cgbType, finalKey, track->pitM);
    } else {
        chan->freq = MidiKeyToFreq(chan->wav, finalKey, track->pitM);
    }

    chan->status = 0x80;                /* start, picked up by the mixer */
    track->flags &= ~0x0F;
}

/* ------------------------------------------------------------------------- *
 * MPlayMain -- one player's worth of sequencing, once per frame.
 * ------------------------------------------------------------------------- */

void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    struct SoundInfo *soundInfo;
    struct MusicPlayerTrack *track;
    s32 i;
    u32 activeMask;

    if (mplayInfo->ident != ID_NUMBER)
        return;
    mplayInfo->ident++;

    /* The four players are a chain: MPlayOpen hands each new player the
     * previous head, so calling the head runs all of them. */
    if (mplayInfo->func != 0)
        ((void (*)(struct MusicPlayerInfo *))mplayInfo->func)(
            (struct MusicPlayerInfo *)mplayInfo->intp);

    if ((s32)mplayInfo->status < 0)     /* MUSICPLAYER_STATUS_PAUSE */
        goto done;

    soundInfo = SOUND_INFO_PTR;
    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0)
        goto done;

    /* The tempo accumulator.  tempoI is added every frame and a tick is taken
     * every 150 counts, so tempoI == 150 is one tick per frame. */
    mplayInfo->tempoC += mplayInfo->tempoI;

    while (mplayInfo->tempoC >= 150) {
        mplayInfo->tempoC -= 150;
        activeMask = 0;
        track = mplayInfo->tracks;

        for (i = 0; i < mplayInfo->trackCount; i++, track++) {
            struct SoundChannel *chan;
            u32 bit = 1u << i;

            if (!(track->flags & MPT_FLG_EXIST))
                continue;
            activeMask |= bit;

            /* Count each held note down towards its gate time, and release it
             * when it expires.  A channel whose status has already gone to
             * zero (the mixer finished it) is unlinked here. */
            for (chan = track->chan; chan != NULL;
                 chan = (struct SoundChannel *)chan->np) {
                if (chan->status & 0xC7) {
                    if (chan->gt != 0 && --chan->gt == 0)
                        chan->status |= 0x40;
                } else {
                    ClearChain(chan);
                }
            }

            /* A track that has just been started is reset here rather than in
             * MPlayStart, so it happens on the player's own clock -- and then
             * falls straight into command execution, because Clear64byte has
             * left wait at zero. */
            if (track->flags & MPT_FLG_START) {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 64;
                track->lfoSpeed = 22;
                track->tone.type = 1;
            }

            /* Run commands until the track asks to wait. */
            while (track->wait == 0) {
                u8 *p = track->cmdPtr;
                u32 cmd = *p;

                /* Running status, as in MIDI: a byte below 0x80 is an argument
                 * to whatever command last set it. */
                if (cmd < 0x80) {
                    cmd = track->runningStatus;
                } else {
                    track->cmdPtr = p + 1;
                    if (cmd >= 0xBD)
                        track->runningStatus = cmd;
                }

                if (cmd >= 0xCF) {
                    ((void (*)(u32, struct MusicPlayerInfo *,
                               struct MusicPlayerTrack *))soundInfo->plynote)(
                        cmd - 0xCF, mplayInfo, track);
                } else if (cmd > 0xB0) {
                    u32 index = cmd - 0xB1;
                    MPlayFunc *jt = (MPlayFunc *)soundInfo->MPlayJumpTable;

                    mplayInfo->cmd = index;
                    ((void (*)(struct MusicPlayerInfo *,
                               struct MusicPlayerTrack *))jt[index])(
                        mplayInfo, track);
                    /* ply_fine zeroes flags: the track has ended and must not
                     * be asked for another command, or cmdPtr walks off. */
                    if (track->flags == 0)
                        break;
                } else {
                    track->wait = sClockTable[cmd - 0x80];
                }
            }
            if (track->flags == 0)
                continue;

            if (track->wait != 0) {
                track->wait--;

                /* Vibrato/tremolo: a triangle LFO whose output goes into modM,
                 * which TrkVolPitSet folds into pitch or volume depending on
                 * modT. */
                if (track->lfoSpeed != 0 && track->mod != 0) {
                    if (track->lfoDelayC != 0) {
                        track->lfoDelayC--;
                    } else {
                        /* The sum is kept at full width for the fold even
                         * though only its low byte is stored back, which is
                         * what the assembly does and what makes the waveform
                         * come out symmetric. */
                        u32 phase = (u32)track->lfoSpeedC + track->lfoSpeed;
                        s32 depth;

                        track->lfoSpeedC = (u8)phase;
                        if ((s8)(phase - 0x40) < 0)
                            depth = (s8)phase;      /* rising, and the tail */
                        else
                            depth = 128 - (s32)phase;   /* folded back down */
                        depth = (depth * (s32)track->mod) >> 6;
                        if ((u8)depth != (u8)track->modM) {
                            track->modM = depth;
                            track->flags |= (track->modT == 0)
                                          ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                        }
                    }
                }
            }
        }

        mplayInfo->clock++;
        if (activeMask == 0) {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mplayInfo->status = activeMask;
    }

    /* Second pass: push whatever the commands changed down into the channels
     * that are already sounding.  Split from the first pass because a track
     * can take several ticks in one frame and only the final state matters. */
    track = mplayInfo->tracks;
    for (i = 0; i < mplayInfo->trackCount; i++, track++) {
        struct SoundChannel *chan;

        if (!(track->flags & MPT_FLG_EXIST))
            continue;
        if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            continue;

        TrkVolPitSet(mplayInfo, track);
        for (chan = track->chan; chan != NULL;
             chan = (struct SoundChannel *)chan->np) {
            u32 cgbType;

            if (!(chan->status & 0xC7)) {
                ClearChain(chan);
                continue;
            }
            cgbType = chan->type & 7;
            if (track->flags & MPT_FLG_VOLCHG) {
                ChnVolSet(chan, track);
                if (cgbType != 0)
                    ((struct CgbChannel *)chan)->mo |= 1;
            }
            if (track->flags & MPT_FLG_PITCHG) {
                s32 key = (s32)chan->ky + (s8)track->keyM;

                if (key < 0)
                    key = 0;
                if (cgbType != 0) {
                    struct CgbChannel *cgb = (struct CgbChannel *)chan;

                    cgb->fr = SOUND_INFO_PTR->MidiKeyToCgbFreq(
                        cgbType, key, track->pitM);
                    cgb->mo |= 2;
                } else {
                    chan->freq = MidiKeyToFreq(chan->wav, key, track->pitM);
                }
            }
        }
        track->flags &= 0xF0;
    }

done:
    mplayInfo->ident = ID_NUMBER;
}

/* ------------------------------------------------------------------------- *
 * The mixer.
 *
 * This is where the transcription stops being literal; see the file header.
 * The envelope and the phase arithmetic are the assembly's, the accumulation
 * is not.
 * ------------------------------------------------------------------------- */

/* Six blocks of delay is what the hardware happens to have at the rate this
 * game runs at: PCM_DMA_BUF_SIZE / pcmSamplesPerVBlank is 1584/264 == 6 for
 * SOUND_MODE_FREQ_15768, which is what m4aSoundInit selects.  The reverb reads
 * the block it is about to overwrite, so the feedback delay is one trip round
 * that ring -- about 100 ms.  Keeping the number of *frames* rather than the
 * number of samples is what keeps that delay at 100 ms on a 48 kHz device. */
#define PORT_REVERB_BLOCKS 6

static int sRate;
static int sBlockLen;
static int sRingLen;
static int sRingPos;
static u32 sFixedStep;      /* phase step for TONEDATA_TYPE_FIX voices */

static s32 sRingR[PORT_AUDIO_MAX_BLOCK * PORT_REVERB_BLOCKS];
static s32 sRingL[PORT_AUDIO_MAX_BLOCK * PORT_REVERB_BLOCKS];
static s32 sAccR[PORT_AUDIO_MAX_BLOCK];
static s32 sAccL[PORT_AUDIO_MAX_BLOCK];
static s16 sStage[PORT_AUDIO_MAX_BLOCK * 2];
static int sStageFrames;

/* Phase step in the assembly's own format: a 23-bit fraction, so `>> 23` is
 * how many whole samples to advance.
 *
 * The hardware gets here by multiplying the note's frequency by
 * soundInfo->divFreq, which is 2^23/pcmFreq rounded to a whole number.  At the
 * GBA's own rates that rounding is under a cent; at 48 kHz divFreq comes out as
 * 175 against a true 174.76, which is 2.4 cents sharp on every note in the
 * game.  Doing the division in 64 bits here costs one instruction per channel
 * per frame and removes it. */
static u32 StepFor(u32 freq)
{
    u64 step;

    if (sRate == 0)
        return 0;
    step = ((u64)freq << 23) / (u32)sRate;
    if (step > ((u64)127 << 23))        /* the fraction field is 23 bits and
                                           the whole part is 7 */
        step = (u64)127 << 23;
    return (u32)step;
}

void PortMixerInit(int sampleRate, int blockSamples)
{
    sRate = sampleRate;
    sBlockLen = blockSamples;
    sRingLen = blockSamples * PORT_REVERB_BLOCKS;
    sRingPos = 0;
    sStageFrames = 0;
    memset(sRingR, 0, sizeof(sRingR));
    memset(sRingL, 0, sizeof(sRingL));
}

void PortMixerSetBlock(int blockSamples)
{
    if (blockSamples > 0 && blockSamples <= PORT_AUDIO_MAX_BLOCK
     && blockSamples * PORT_REVERB_BLOCKS <= sRingLen)
        sBlockLen = blockSamples;
}

/* The rate a TONEDATA_TYPE_FIX voice expects to be played back at.  Those
 * samples carry no pitch of their own -- on hardware they are read one sample
 * per output sample, so they play at whatever the mixer's rate happens to be.
 * At 48 kHz that would be three times too fast, so the port has to know what
 * the hardware rate *would* have been.  m4a.c's SampleFreqSet supplies it. */
void PortMixerSetFixedRate(int gbaPcmFreq)
{
    sFixedStep = StepFor((u32)gbaPcmFreq);
}

void PortMixerSilence(void)
{
    sStageFrames = 0;
    memset(sRingR, 0, sizeof(s32) * (size_t)sRingLen);
    memset(sRingL, 0, sizeof(s32) * (size_t)sRingLen);
}

static void MixChannel(struct SoundInfo *soundInfo, struct SoundChannel *chan,
                       int frames)
{
    struct WaveData *wav = chan->wav;
    u32 status = chan->status;
    u32 ev = chan->ev;
    u32 vol;
    const s8 *loopPtr = NULL;
    s32 loopLen = 0;
    const s8 *p;
    s32 remain;
    u32 fw, step;
    s32 er, el;
    int i;

    if (!(status & 0xC7))
        return;

    /* --- the envelope, one step per frame -------------------------------- */
    if (status & 0x80) {                        /* a note that just started */
        if (status & 0x40) {                    /* ...and was released the
                                                   same frame */
            chan->status = 0;
            return;
        }
        status = 3;                             /* attack */
        chan->cp = (u32)(uintptr_t)wav->data;
        chan->ct = wav->size;
        chan->fw = 0;
        ev = 0;
        /* The loop flag is the top bits of WaveData::status. */
        if (wav->status & 0xC000)
            status |= 0x10;
        goto attack;
    }

    if (status & 0x04) {                        /* echo tail */
        u32 len = chan->echoLength;

        chan->echoLength = len - 1;
        if (len <= 1) {
            chan->status = 0;
            return;
        }
        goto setvol;
    }

    if (status & 0x40) {                        /* released */
        ev = (chan->release * ev) >> 8;
        if (ev > chan->echoVolume)
            goto setvol;
        goto echo;
    }

    /* The low two bits of status are the ADSR phase: 3 attack, 2 decay,
     * 1 sustain.  Each phase steps down into the next by decrementing it. */
    if ((status & 3) == 2) {
        ev = (chan->decay * ev) >> 8;
        if (ev > chan->sustain)
            goto setvol;
        ev = chan->sustain;
        if (ev == 0)
            goto echo;
        status--;                               /* -> sustain */
        goto setvol;
    }
    if ((status & 3) != 3)
        goto setvol;                            /* sustain: hold */

attack:
    ev += chan->attack;
    if (ev >= 0xFF) {
        ev = 0xFF;
        status--;                               /* -> decay */
    }
    goto setvol;

echo:
    /* The note is done, but m4a keeps a short tail at echoVolume so a release
     * does not click.  No tail configured means the channel simply stops. */
    ev = chan->echoVolume;
    if (ev == 0) {
        chan->status = 0;
        return;
    }
    status |= 4;

setvol:
    chan->status = status;
    chan->ev = ev;
    vol = (ev * (u32)(soundInfo->masterVolume + 1)) >> 4;
    chan->er = (vol * chan->rightVolume) >> 8;
    chan->el = (vol * chan->leftVolume) >> 8;

    if (status & 0x10) {
        loopPtr = (const s8 *)wav->data + wav->loopStart;
        loopLen = (s32)(wav->size - wav->loopStart);
    }

    /* --- the samples ----------------------------------------------------- */
    p = (const s8 *)(uintptr_t)chan->cp;
    remain = (s32)chan->ct;
    fw = chan->fw;
    er = chan->er;
    el = chan->el;
    step = (chan->type & TONEDATA_TYPE_FIX) ? sFixedStep : StepFor(chan->freq);

    /* Scaling: on hardware each channel contributes (er * sample) >> 8 as a
     * signed byte, and that byte is the full range of the DAC.  So one unit of
     * that byte is 256 units of an s16, and `er * sample` is already the
     * channel's contribution in s16 terms -- no gain constant to guess at, and
     * the same loudness as the console. */
    for (i = 0; i < frames; i++) {
        s32 cur = p[0];
        s32 s = cur + ((((s32)p[1] - cur) * (s32)fw) >> 23);
        u32 adv;

        sAccR[i] += er * s;
        sAccL[i] += el * s;

        fw += step;
        adv = fw >> 23;
        if (adv != 0) {
            fw &= 0x7FFFFF;
            remain -= (s32)adv;
            if (remain > 0) {
                p += adv;
            } else if (loopLen == 0) {
                chan->status = 0;
                return;
            } else {
                /* Add the loop length back until there is something left.  A
                 * very short loop played very fast can need this more than
                 * once, which is why the assembly loops rather than taking a
                 * remainder. */
                do {
                    remain += loopLen;
                } while (remain <= 0);
                p = loopPtr + (loopLen - remain);
            }
        }
    }

    chan->cp = (u32)(uintptr_t)p;
    chan->ct = (u32)remain;
    chan->fw = fw;
}

/* Called from SoundMain, once the sequencer has run. */
void PortMixerRender(struct SoundInfo *soundInfo)
{
    int frames = sBlockLen;
    int i, ch;

    if (sRate == 0 || frames <= 0)
        return;

    /* Reverb, exactly as SoundMainRAM does it: the block about to be written
     * is summed with the one after it in the ring -- both holding audio from a
     * full trip round the ring ago -- scaled by soundInfo->reverb, and used as
     * the starting value the channels mix on top of.  With reverb at its
     * maximum of 127 the four taps give a feedback of 508/512, which is why it
     * rings the way it does.  Songs that want none set reverb to 0 and the
     * block is simply cleared. */
    if (soundInfo->reverb != 0) {
        s32 gain = soundInfo->reverb;

        for (i = 0; i < frames; i++) {
            int a = (sRingPos + i) % sRingLen;
            int b = (sRingPos + i + frames) % sRingLen;
            s32 v = ((sRingR[a] + sRingL[a] + sRingR[b] + sRingL[b]) * gain) >> 9;

            sAccR[i] = v;
            sAccL[i] = v;
        }
    } else {
        memset(sAccR, 0, sizeof(s32) * (size_t)frames);
        memset(sAccL, 0, sizeof(s32) * (size_t)frames);
    }

    for (ch = 0; ch < soundInfo->maxChans; ch++)
        MixChannel(soundInfo, &soundInfo->chans[ch], frames);

    /* Back into the ring for the next reverb pass, and out as s16.
     *
     * Hardware wraps here rather than clamping -- the accumulator is a single
     * signed byte -- so a loud passage folds instead of distorting.  Clamping
     * is the one place this deliberately does not reproduce the console. */
    for (i = 0; i < frames; i++) {
        int a = (sRingPos + i) % sRingLen;
        s32 r = sAccR[i];
        s32 l = sAccL[i];

        sRingR[a] = r;
        sRingL[a] = l;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        /* Left first: the port's transport is interleaved L,R, while m4a's own
         * buffer is two halves with DirectSound A (the first) routed right. */
        sStage[i * 2] = (s16)l;
        sStage[i * 2 + 1] = (s16)r;
    }

    sRingPos = (sRingPos + frames) % sRingLen;
    sStageFrames = frames;
}

/* Hand the finished block to the transport.  Returns 0 when SoundMain has not
 * run since the last call, which happens around the boot sequence and whenever
 * the game switches sound off; the caller pushes silence. */
int PortMixerTakeBlock(const s16 **out)
{
    int n = sStageFrames;

    *out = sStage;
    sStageFrames = 0;
    return n;
}

/* ------------------------------------------------------------------------- *
 * SoundMain -- the per-frame driver.
 * ------------------------------------------------------------------------- */

void SoundMain(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;

    /* GameLoop calls this at the tail of its VBlank work, immediately before
     * spinning on `while (REG_DISPSTAT & DISPSTAT_VBLANK);`.  On hardware the
     * mixer's own runtime is part of what ends that window, so this is the
     * backstop that guarantees the spin exits even on a frame that transferred
     * almost nothing.  It has to happen before the ident check, because a
     * frame with sound switched off still has to end. */
    PortVBlankEnd();

    if (soundInfo == NULL || soundInfo->ident != ID_NUMBER)
        return;
    soundInfo->ident++;

    /* soundInfo->maxLines is zero for this game (linker.ld), so the scanline
     * budget the assembly checks here never applies -- see the file header. */

    if (soundInfo->func != 0)
        ((void (*)(struct MusicPlayerInfo *))soundInfo->func)(
            (struct MusicPlayerInfo *)soundInfo->intp);
    soundInfo->CgbSound();

    PortMixerRender(soundInfo);

    soundInfo->ident = ID_NUMBER;
}
