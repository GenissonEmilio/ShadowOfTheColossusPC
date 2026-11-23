# European PAL edition

The launcher accepts the European SCES-53326 v1.00 disc alongside the North
American SCUS-97472 edition. Select the ISO or its 7Z, ZIP, or RAR archive in
the launcher. RecompLauncher Easy Setup takes the ISO.

The importer checks the boot identifier in `SYSTEM.CNF`, verifies the boot
ELF and the KERNEL, MANAGER, and GAMECORE modules, and checks the five contiguous
extents of the European data archive. It copies them into the local `NICO.DAT`
that the game uses for sector reads. The compilation cache is separate from the
North American edition.

PAL uses the OpenGL compatibility renderer and the video mode selected in the
original game. The 50/60 Hz choice controls the guest video signal. To see
how often the game renders a new frame, open F1 and enable **Show FPS**.
The counter shows the game's render rate and the rate of new pictures sent
to the window. Use the memory card to save progress. Frame generation, quick
save states, and the dedicated assist patches require the North American edition.
