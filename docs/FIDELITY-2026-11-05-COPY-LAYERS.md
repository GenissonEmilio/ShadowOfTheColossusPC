# Framebuffer capture order

## Reproduced defect

The renderer sorted draws by display-list layer, but scheduled a framebuffer
capture only at the first submitted `screenCopy` draw. Submission order differs
from display-list order. An underwater draw at layer 19 could therefore run
before its capture when a layer-23 reader had been submitted first.

An opt-in trace of 240 Hydrus underwater frames on the private Linux QA build
`700a0a390f50de13190ed20a` recorded 144 captures at layer 23 and 96 at layer 19.
All 240 underwater draws covered 229,376 samples (512 x 448), with finite
geometry, constant vertex colour/alpha and no GL errors. Down/up sampling
preserved RGB means within 0.204 levels. All 96 layer-19 captures were sampled
in their own frame; in 124 consecutive 23-to-23 frames, layer 19 sampled the
exact hash of the previous frame's reduced copy. Source scheduling, rather than
missing raster coverage or corrupted reduction, was demonstrated directly.

The earlier Gaius-lake replay also showed visible brightness flashes while the
guest underwater flag and tint remained constant. Not every underwater camera
interval flashes: a later 12-second Hydrus interval was already stable before
this correction. Neither that interval nor the synthetic tests establish that
the full Hydrus battle has been validated.

## Correction

Capture before every distinct framebuffer-reading layer, in sorted layer order.
Water and a screen-copy reader at layer 9 share one event. Taking only the
minimum layer would still leave later readers using an outdated picture.

On Linux, frames with several capture layers use chronological rendering even
when prefix redraw is requested. A plain mesh prefix omits intermediate
composites such as depth of field at layer 20. Chronological rendering retains
those effects and captures their result for later readers. Prefix sources are
also associated with their layer, with separate lazily allocated reduced
targets within the existing three-slot prefix budget. The Vita redraw path is
retained; cases exceeding its existing prefix capacity are not validated here.

The bounded Linux diagnostic is disabled unless `SOTC_WATER_COPY_TRACE` is set.
Enabled readbacks synchronize the GPU and are for diagnosis, not benchmarking.

## Regression check

`test_screen_copy_order.sh` uses synthetic colours and real OpenGL, without a
ROM. A red prior frame is followed by a green world, a layer-19 screen reader,
blue geometry on the right at layer 21, and a layer-23 reader. The final picture
must be green on the left and blue on the right. Both submission orders,
reduced/full sources and redraw off/on are covered (eight combinations).

The diagnostic-only baseline fails all eight combinations while its opaque
controls and readbacks pass. Sorted submission exposes the later stale reader;
reversed submission exposes the earlier reader using a previous-frame source.
The corrected build passes all eight combinations. The near-depth GPU check,
renderer-state round trips, and 14 sampler transitions/palette/mipmap checks
also pass. Live corrected replay is still pending.

Electrical effects, weak-point brightness, actor deformation and full battles
against Hydrus and Pelagia remain separate validation work.
