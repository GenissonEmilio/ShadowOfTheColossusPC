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

## Plan

1. Read the command dispatcher of SG2IOPM1.IRX: what each command does in
   terms of SPU2 (voice parameters, key on/off, DMA, streams) and what the
   400-byte status holds.
2. Serve RPC 0x11223344 natively: apply the commands to a voice mixer on the
   host (the ps2xIOP SPU2 mixer can be driven directly at first), fill the
   status, read the stream data from the disc on the host. Then the IOP CPU,
   sg2iop_driver and libsd are no longer run.
3. Measure: the route's audio against the emulated path (PS2X_SPU2_WAV).
