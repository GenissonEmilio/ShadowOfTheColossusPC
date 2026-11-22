# European PAL edition

The launcher accepts the European SCES-53326 v1.00 disc alongside the North
American SCUS-97472 edition. Select the ISO or its 7Z, ZIP, or RAR archive in
the launcher. RecompLauncher Easy Setup takes the ISO.

The importer checks the boot identifier in `SYSTEM.CNF`, verifies the boot
ELF and the KERNEL, MANAGER, and GAMECORE modules, and checks the five contiguous
extents of the European data archive. It copies them into the local `NICO.DAT`
that the game uses for sector reads. The compilation cache is separate from the
North American edition.

PAL uses the compatibility graphics path and the video mode selected in the
original game. Use the memory card to save progress. Features that rely on the
North American native renderer and runtime hooks, including frame generation,
quick save states, and the dedicated assist patches, remain edition-specific.
