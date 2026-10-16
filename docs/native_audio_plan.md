# Native audio plan

## How SotC makes sound (2026-10-16)

- The sound engine (SG2, about 240 `Sg2*` functions) runs on the EE: voices
  (`Sg2Slot*`), the sequence players (`Sg2Sep*`, `Sg2Smfp*`), sample banks
  (`Sg2Vab*`), streams (`Sg2ExStAdpcm*`, `Sg2StAdpcm*`). It is recompiled
  code already.
- It talks to the IOP through one channel: `Sg2RemoteSend` (0x1d2010) puts
  commands of four words (cmd, a, b, c) in a ring of 512 (head 0x287500,
  tail 0x287504, data 0x287540); each tick `Sg2RemoteTickProc` (0x1d21e8,
  called through a pointer) sends the pending ones with one SIF RPC to
  server 0x11223344 (function 0) and gets 400 bytes of status back at
  0x28b540. `Sg2RemoteSendDmaCom` queues the transfers of sample data.
- On the IOP, `SG2IOPM1.IRX` ("sg2iop_driver") applies them through
  `LIBSD.IRX` to the SPU2 (voices, DMA to sound memory, ADPCM streaming from
  the disc). Both run on the emulated IOP; the SPU2 is emulated in ps2xIOP.

## Commands seen on the test route (tools/native_run.sh, SOTC_LOG_SG2=1)

About two per tick: 1 (2328), 2 (823), 3 (823), 4 (1084), 7 (660), 0x1a (5862),
0x18, 0x19, 0x1b, 0x1c, 0x1d (tens), 0x9 (94), and set-up ones once or twice
(5, 6, 8, a, b, d, e, f, 0x10, 0x11, 0x16, 0x1e, 0x21).

## The IOP driver's commands (SG2IOPM1.IRX)

RPC 0x11223344 function 0 (handler 0x3f08) queues each 16-byte command in
a ring (0x4c70, 512 entries); a loop (0x3d74) runs table[cmd] (0x4ba0 in
.data) with the current command at *0x4c40; then 0x400c fills the 400-byte
reply (0x7088). The driver is built without optimisation (frame pointer,
everything on the stack): easy to read. Voice v in 0..47 is core v / 24,
voice v % 24; libsd parameters are core | voice << 1 | register.

| cmd | what it does |
|---|---|
| 0 | nothing |
| 1 | voice VOLL / VOLR (0x000 / 0x100); negative values + 0x7fff |
| 2 | voice ADSR1 / ADSR2 (0x300 / 0x400) |
| 3 | voice SSA (SetAddr 0x2040) |
| 4 | voice PITCH (0x200) from a note: table at 0x46a0 (12 x 16 fine steps), octave shift, bend; x 441 / 480 (44.1 to 48 kHz) |
| 5 | core attributes (SetCoreAttr) |
| 6 | MVOLL / MVOLR (0x980 / 0xa80) |
| 7 | KON core 0 / 1 (SetSwitch 0x1500 / 0x1501, bit masks) |
| 8 | KOFF (0x1600 / 0x1601) |
| 9 | VMIXEL and VMIXER (0x1900, 0x1b00: reverb send) |
| a | NON (noise, 0x1400) |
| b | EEA (effect end, SetAddr 0x1d00) |
| c - 10 | stream slot state (0x4c4c + i x 0x14): file, volume, ... ; c starts (0x120), d sets and kicks (0x380 op 4) |
| 11 | init: sceSdInit, master and effect volumes 0, core attributes 2 and 3 on |
| 12 | transfer interrupt handler off |
| 13, 14 | stream ops 1, 2 (0x380) |
| 15 | EVOLL / EVOLR (0xb80 / 0xc80: effect return) |
| 16 | clear the voice stream tables (0x7218, 0x78d8) |
| 18 | streamed voice set-up: SSA, ADSR from fixed values (0x8080) |
| 19 | streamed voice release |
| 1a | VOLL / VOLR for every voice in two 24-bit masks (core 0 / 1) |
| 1b | PITCH for every voice in two masks: (rate << 12) / 48000 |
| 1c, 1d | streamed voices: start / step (0x183c) |
| 1e - 23 | PCM input to a core (AVOL 0xf80 / 0x1080, BlockTrans of 0x800-byte halves): streamed PCM (cutscenes) |

## Data, transfers and the reply

- No disc reading in the driver: the EE puts sample and stream data in IOP
  memory (SIF DMA) and asks for copies to sound memory. `Sg2DmaTransVoice`
  is command 0x13 (EE ring of DMA commands 0x28b740, one in flight;
  `Sg2RemoteDmaTransStatus` reads it): the driver queues op 1 for its thread
  (0x474), which does sceSdVoiceTrans(channel, write, IOP address, sound
  address, size), waits, and marks the transfer done (0x7080 = 2 -> reply).
  Op 2 is the same on the other DMA channel, op 4 the effect set-up
  (0x120), op 8 a transfer flagged in 0x4c30.
- Streamed voices (commands 0x18 / 0x19 / 0x1c / 0x1d): double-buffered in
  sound memory; every reply the driver reads each one's NAX (0x2240) and,
  when it has crossed into the other half, queues a refill of that half from
  IOP memory (0x2204, then 0x2620 runs the queue: 0x1adc, 0x1d38, 0x1f64).
- The reply (400 bytes, IOP 0x7088 -> EE 0x28b540): +0x000 ENVX of the 48
  voices (u32 each), +0x0c0 each voice's stream position (0x7230 of its
  slot), +0x180 the PCM input position (0xc8f4), +0x184 transfer busy
  (1 while op 1 runs, EE 0x28b6c4).

## Native driver (2026-10-16)

`ps2xIOP/src/emulator/services/iop_sg2.cpp` (`Sg2Driver`) stands in for
SG2IOPM1.IRX: when the game loads the IRX from its buffer
(`sceSifLoadModuleBuffer`, the image's .iopmod name is "sg2iop_driver") the
load returns a module id without running anything, and RPC 0x11223344 is
answered on the host. Each command goes straight to the SPU2 registers and
sound RAM with libsd's encodings (voice parameters, switches, addresses,
core attributes); transfers (0x13 / 0x14) are copies done at once, so the
busy flag reads 0 in the same reply; the streamed voices keep the driver's
NAX polling, refill queue (one op per reply) and loop flags; the PCM input
(0x20) feeds core 0's sound data input through a refill callback from the
SPU2 mixer at each consumed block, as the transfer interrupt did. The
reply is built in the driver's order (ENVX, stream positions, then the
stream bookkeeping).

- On by default with SOTC_NATIVE=1; `PS2X_NATIVE_SG2=1/0` forces it.
  `PS2X_SG2_TRACE=1` prints the commands (not 1 and 0x1a).
- Pitch table: floor(0x1000 * 2^((i - 0xd0) / 192)), 608 entries, with the
  32 entries where the driver's table differs (rounding, two typos).
- Reverb: only "studio C" (libsd mode 4, what SotC sets) is known: the 22
  offsets and 10 coefficients captured from the emulated libsd
  (`PS2X_SPU2_DUMPREGS=1`), work area 0x37f0 words below EEA. Another mode
  prints a warning and uses it; mode 0 zeroes the coefficients.
  `sceSdInit` is reduced to the registers the mix reads, with the values
  libsd leaves (MMIX 0xff0 / 0xffc, VMIXL/R all, AVOL of core 1 0x7fff).
- Measured on the route (tools/native_run.sh, PS2X_SPU2_WAV): loudness per
  second follows the emulated driver's (same peaks, timing drift of a
  second or two); CPU unchanged (55 s user both: the IRX was cheap).
  Not exercised on the route: the PCM input (cutscene audio, command
  0x20), command 0x14, reverb modes other than 4.

## Native mixer (2026-10-16)

`ps2xIOP/src/native/sound_mixer.cpp` (`SoundMixer`) replaces the SPU2 for
the native driver: 48 voices (ADPCM from 2 MiB of sound memory, cubic
interpolation, ADSR), dry and reverb sends, the routing bits, core 0 into
core 1, the sound data input and the reverb, with the same arithmetic as
the emulated SPU2 (the recordings match it: same loudness per second on the
route). The driver calls it with typed setters (volume, pitch, ADSR,
addresses, key on/off, sends, volumes, reverb preset and area); there is no
register file, IOP DMA or interrupt, and the emulated SPU2 stays idle once
the native driver is loaded. Voices are mixed in blocks of 64 frames, a
voice at a time, skipping silent ones.

- Clock: by default the mixer follows the emulated IOP time (deterministic
  for recordings and tests). `PS2X_AUDIO_PULL=1` mixes in the host audio
  thread when the device asks (the audio length follows wall-clock time):
  the mode for the Vita, where it takes the mix off the game thread.
- Cost on the Mac: about 2-3 % of the game thread (half of it the reverb),
  as the emulated SPU2.
- `PS2X_SPU2_WAV` records the native mix too.

Left on the PS2 side: libsd and the other IRXs still load on the emulated
IOP (nothing calls libsd for sound now). The emulated SPU2 code stays for
the emulated path (`PS2X_NATIVE_SG2=0`).

## Plan

1. Read the command dispatcher of SG2IOPM1.IRX: what each command does in
   terms of SPU2 (voice parameters, key on/off, DMA, streams) and what the
   400-byte status holds.
2. Serve RPC 0x11223344 natively: apply the commands to a voice mixer on the
   host (the ps2xIOP SPU2 mixer can be driven directly at first), fill the
   status, read the stream data from the disc on the host. Then the IOP CPU,
   sg2iop_driver and libsd are no longer run.
3. Measure: the route's audio against the emulated path (PS2X_SPU2_WAV).
