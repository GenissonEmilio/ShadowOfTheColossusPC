# GS provenance observation

This change records state; it does not change a draw or shader and installs no
observer by default. It does not assert that a known target is the main target.

`PS2Memory::setGsProvenanceObserver(Observer*)` installs a caller-owned observer
on the stopped EE/submission thread, after draining existing VU work. Installing,
memory initialization and successful state restore change the observation epoch.
No callback runs on a VU/GIF/render worker. An observer must copy callback data,
must not throw, and must not call guest code or change the observer in a callback.

The ordered callbacks are `begin`, raw GS `write`/`gap`/`compacted`, then `end`.
The token's `frame` is `vif1Chains` **before** a chain increments it; normal-mode
submissions share the counter without creating a frame boundary. `submission`
distinguishes these operations. A synchronous chain's `end` runs before that
increment. `Completion.complete` is scan integrity, not DMA/GPU completion.

`Source` carries physical domain/address, execution tagVisit, span and TTE status.
The walker observes DIRECT bytes where they lie; the compactor observes them on
input and reports separate spans for retained commands and synthetic NOPs.
Noncontiguous qword fragments cannot become a unique RAM kick by guessing an
address. Unsupported GIF formats, unmodelled VU/commands, incomplete data, and
unobserved paths produce explicit gaps. Paths/queues not known idle make the
submission unordered. Deferred/forced VU parsing is explicitly unsupported;
metadata is not silently attached to an unrelated worker frame.

In the current runtime binding, `syncCoreSubsystems` passes Memory's
`mtvuWorkAdded/Done` to `GS::enableGifWorker`. `enqueueGifWork` increments that
same outstanding count before adding a packet to its local staging list; the
GIF worker decrements it only after `processGIFPacketNow`, and publishes staged
raster commands before the final decrement. Therefore `syncVu1` also drains the
separate GIF worker's register writes. Quiescence additionally requires empty
arbiter, masked FIFO and pending transfer lists. It is not inferred from
`arbiter.empty()` alone. The later GSRaster queue receives copied contexts and
VRAM/raster commands; it does not update the GS frontend's register banks.
An alternate embedding must preserve this work tracking or leave submissions
unordered until it provides its own proven barrier.

`ConservativeLedger` is an optional bounded observer: two context banks,
PRIM/PRMODECONT/PRMODE, snapshots at XYZ2/F2 and XYZ3/F3 (including no-kick status),
8192 vertices per submission and four completed submissions. Overflows fail
closed and dropped ledgers are counted. `takeCompleted` transfers owned immutable
records to the frontend. `findKick` reports missing/unique/ambiguous by physical
domain/address **within an explicitly selected submission**. A unique result with
`known=false` is still unsupported. The frontend must also validate frame/epoch,
root descriptor, camera, target, TEST/ZBUF, interpolation and material before
enabling any future depth feature. The ledger does not serialize guessed state
across restore; it starts unknown until actual writes revalidate it.

Validation commands (run only in the principal's protected build window):

```sh
# Compiles ps2_vif1_interpreter.cpp directly and uses its actual feed functions.
# No runtime kit/archive is needed for this parser test.
bash tools/linux/test_gs_provenance.sh

# After building the CURRENT runtime into an isolated output, pass the actual
# fresh archive, then the read-only dependency kit (IOP/raylib/headers only).
bash tools/linux/test_gs_provenance.sh path/in/repo/libps2_runtime.a artifacts/compiler
```

The second command refuses an archive older than the relevant sources/headers
or lacking the provenance implementation symbol. It never defaults to the kit's
runtime archive. It runs real `PS2Memory::hostDmaStart` CALL/RET/NEXT traversal in
both walker and FULL/compactor modes with TTE, frame-counter seal checks, repeated
CALL ambiguity, immutable snapshots, offscreen/restoration and an observer-absent
event control. A link wrapper only enables the native skipRaster condition; it
does not replace a parser, memory traversal or provenance callback.

The runner uses one outer codex-build and a disposable local Docker builder,
2 CPUs, 1600 MiB RAM/swap cap and no network. The default output is
`artifacts/gs-provenance-tests`; the principal preserves logs and cleans binaries.
The final local protected run passed 89 parser assertions, 114 assertions with
the fresh runtime's walker, and 115 with its FULL/compactor path. This includes
IRQ/TIE interruption, vertex overflow, ring eviction and stale callbacks. The
runtime was rebuilt from the changed sources before the integration run. Linux
CI runs the same parser and fresh-runtime checks after building the compiler kit.

These checks use synthetic memory and packets. They do not validate an actual
colossus fight, complete cross-path coverage, a final pixel image, or a future
renderer feature that consumes the metadata.
