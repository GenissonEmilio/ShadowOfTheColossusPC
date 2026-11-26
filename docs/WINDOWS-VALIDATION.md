# Windows investigation — 2026-11-26

## Baseline and scope

The baseline is **v1.2.6 / `297dae6`**, the published Windows source on
`codex/release-v1.2.6`. The default branch was still at `99888eb` and does not
contain all the desktop changes shipped in that release. Fixes in this change
are evaluated against the Windows release, not that older default branch.

Issues and comments were reviewed across the repository, including closed
reports. Feature requests are distinguished from reproducible defects.

## Preparation failure reported by oeudasio (#38)

The supplied `last-prepare.log` confirms that translating the boot executable
succeeded: 1,425 functions, no decode failures and no errors. The subsequent
link fails for all 45 object files below the Windows profile `Eudásio`.
This is an encoding failure in the toolchain, rather than an ISO validation or
MIPS translation failure.

With the official v1.2.6 kit, an ASCII-path fixture compiles and links. Equivalent
fixtures with accented and CJK characters fail in the assembler/linker. The
GCC driver and C++ compiler already declare UTF-8 in their application manifests;
the assembler, linker and `collect2` do not. The response file contains UTF-8
paths, but that encoding is not used consistently by the tools reading them.
The exact linker failure was reproduced with an existing object and a UTF-8
response file after removing inherited MSYS2 locale variables from the probe.
Those variables can mask the defect in a terminal while a normal GUI launch
still fails. Both the unmodified-kit failure and patched-kit success were
observed locally.

The Windows packager now enables UTF-8 and long-path awareness consistently in
the staged compiler executables. Existing manifest entries and other PE
resources are retained. The installed/source toolchain and Windows policy are
not modified. The configuration helper and manifest template participate in
the compiler cache identity and the prior-kit reuse gate.

Preparation helpers explicitly use Python UTF-8 mode. Cross-volume paths fall
back to absolute paths, and structured module arguments preserve `@` in either
file name. The legacy module format remains accepted for existing callers.
An interrupted object rebuild also invalidates its old key before compilation,
so reverting source or flags cannot make a partial object look cached.

[Report #38](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/38)

## Other reports

| Reports | Evidence and disposition |
| --- | --- |
| [#22](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/22), [#32](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/32), [#33](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/33) | Startup access violations point to Windows exception unwinding. The dispatcher stack-pointer correction was already shipped in v1.2.5. The existing Win64 fixture passes with the released v1.2.6 kit. The new diagnostic also records the loaded ntdll version, RIP and the failing thread's stack limits. Confirmation on each reporter's current installation is still required. |
| [#23](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/23) | All four Windows logs identify the dedicated RX 560X as the OpenGL renderer. One interval has 21.4 new and 78.6 repeated frames/s while the host presents at 100 Hz. High host FPS is not high game FPS. The old load estimate could report a blocked thread as busy; Windows now measures actual thread CPU time, including the native renderer and host. No hardware-independent FPS gain is claimed. |
| [#1](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/1), [#21](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/21) | The historical HUD, pose, palette, defeat-mask and water fixes are present in v1.2.6. The latest detailed #1 reports are Linux reports. The campaign record validates selected fights on Linux; it is not evidence that every fight has been tested on Windows. |
| [#20](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/20) | The old build log predates headless module capture. v1.2.6 captures without host video/audio and logs module exit, byte count and duration. New capture failures remain errors; a partial capture is never accepted. |
| [#34](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/34) | Four-stage progress and per-stage ETA were already added in v1.2.5. The first preparation still performs four local compilations and three captures; stage ETA is not a promise of total remaining time. |
| [#36](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/36) | Closed report of excessive initial preparation time, without logs or timing evidence. Compilation and subsequent cached starts must be measured separately. Generated objects are not reused across stages merely because their source basenames match: their headers and dependencies change. |
| [#37](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/37) | Storage selection is a feature request. The existing `--data-dir` path remains available; this change fixes preparation's cross-volume path handling, not a new graphical storage selector. |
| [#25](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/25), [#27](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/27) | Performance requests overlap #23. New, interpolated and repeated frames must remain separate in measurements. |

Texture-download, language and gameplay-assist requests are not treated as
Windows crash reports. No issue is automatically closed based solely on this
investigation.

## Regression checks

- `tests/windows/test_unicode_prepare.py` uses the shipped recompiler, Python,
  GCC and linker with Unicode data and temporary directories. CI also relocates
  the entire program under an accented/CJK path. It verifies the helper protocol
  for cross-volume module paths containing `@` and repairs an interrupted object
  rebuild after a source rollback, without needing game files.
- `tests/windows/test_compiler_manifest.py` checks UTF-8/UTF-16 XML resources,
  padding and rejection of malformed XML.
- `sotc-thread-load-test` measures both a blocked and an active worker using
  Windows CPU accounting, checks thread replacement/exit and invalid intervals.
- `tools/linux/test_compiler.py` retains the original large-frame/XMM Win64
  unwind and headless-capture tests. Its diagnostic case now uses two threads
  and compares the logged ntdll version with the Windows version API.
- Application-control errors 4551/577 are tested as explicit capture failures.
  A failed final launch logs the original Windows error and preserves the same
  exception. No test disables system protection or suppresses a game crash.

The lightweight checks ran locally with the official kit's GCC 16.2.0. A small
standalone executable was refused by local App Control; the existing full
runtime fixture and real boot/KERNEL captures did execute successfully. This
is recorded per executable, not generalized to every locally built program.

The provided SCUS-97472 ISO and all extracted/generated game data are private.
The full local preparation is a separate, long-running check; its final result
and any gameplay measurements belong in the PR's validation record. They are
not implied by the ROM-free CI or by earlier Linux campaign validation.
