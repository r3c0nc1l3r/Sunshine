# Experimental Lanternbox HDR10 capture

This integration adds opt-in capture negotiation, metadata, a separate VAAPI/OpenGL
RGB10-to-P010 converter, HDR-only shaders and HEVC HDR SEI. It is implemented but
has passed a remote release build, Doxygen and eight blind metadata tests.
The three isolated GPU tests passed on an explicitly selected test render node.
A bounded real-capture test also encoded and software-decoded the synthetic HDR
patches as HEVC Main10/BT.2020/PQ with matching static metadata. Broader GPU
coverage and client presentation qualification below remain required. This must not yet be described
as accepted end-to-end HDR streaming.

Use `capture = wlr` and the VAAPI encoder with an HDR10 Lanternbox compositor
session on the same render device. Client `dynamicRangeMode = 1` maps to
`video::config_t::dynamicRange == 1`; there is no separate `request.hdr` field.
HEVC Main10 or supported AV1 10-bit, 4:2:0 and the VRAM path are required.
The existing asset installation includes the additional HDR-only shader files.

The optional `lanternbox_capture_manager_v1` is bound through the capture
connection's registry. After output selection, Sunshine calls `get_hdr10` on
that output, collects all three metadata events and validates `done` before
requesting frames or creating the stream encoder. It retains the contract until
all screencopy requests have been destroyed. Failed, incomplete, duplicate,
late or unsupported metadata fails HDR initialization. HDR requests without
the manager, linux-dmabuf or screencopy v3 fail explicitly for Lanternbox.
The canonical wl-output make, `lanternbox`, identifies the source even if the
HDR manager is missing. It cannot itself authorize HDR. Other wlr sources
without this manager retain their existing SDR behavior, including 10-bit SDR.

Version 1 accepts only generation 1, ABGR2101010 (`AB30`), full RGB range,
BT.2020 primaries and ST2084/PQ transfer. RGB values are already PQ encoded,
normalized to 10000 cd/m². The conversion must not apply PQ again, normalize
to the mastering peak, or reinterpret the source as linear RGB. Mastering
chromaticities are red, green, blue, white xy pairs in units of 1/50000;
mastering minimum is in 0.0001 cd/m² and maximum in cd/m². Reference white is
validated in cd/m² and does not replace content-light metadata. MaxCLL and
MaxFALL retain zero for unknown values. Metadata that cannot be represented
by Moonlight's fields is rejected rather than truncated.

The dedicated high-precision shaders convert full-range PQ RGB10 to P010 using
BT.2020 non-constant-luminance coefficients. EGL imports ABGR2101010 as logical
R, G, B, sampled as `.rgb`; no red/blue swizzle is applied. The shared matrix
already includes range scaling and offsets normalized by 1023. The shaders round
once to 10-bit codes, clamp to 0–1023 and store `code * 64 / 65535` in normalized
16-bit planes. They do not apply PQ, gamut mapping or luminance rescaling.
YCbCr range follows `encoderCscMode` independently from the full-range RGB contract.

Only the HDR encoder context disables GL dithering. P010 targets must export
R16/GR1616 planes that import as normalized 16-bit textures and pass framebuffer
completeness checks even in release builds. RGB10 imports must retain ten bits
in all three channels. Unsupported imports and GL render errors fail conversion.
Black encoder-probe textures use RGB10; letterboxing clears every target to
Y=64 (limited) or Y=0 (full), with Cb=Cr=512, all shifted left six bits.
The image viewport is aligned to the chroma grid. Linear source filtering, crop
copies, and the existing horizontal chroma sampling footprint are retained.

The compositor supplies the cursor already composited in PQ. The HDR converter
rejects external cursor pixels, RGB8 DMA-BUFs, RAM images and non-P010 targets.
It owns transferred FFmpeg frames before validation, including failure paths,
and commits cached source sequences only after successful imports. HEVC VAAPI
enables HDR mastering-display/content-light SEI only for an actual HDR colorspace;
all SDR encoder options, including 10-bit SDR, remain unchanged.

The collector's direct interface for blind tests is `wl::hdr10_metadata_t`
in `src/platform/linux/lanternbox_hdr.h`. Its `format`, `luminance` and
`mastering_primaries` methods take arrays in wire order. `done`, `failed`,
`valid` and `get_metadata` model completion, rejection and export without
requiring Wayland or GPU access. The dedicated VAAPI entry point is
`va::make_hdr10_encode_device(width, height, offset_x, offset_y)`. The wlr VRAM
backend selects it only with a valid HDR contract and a P010 target. Existing
VAAPI factories and SDR shader sources are unchanged.

Mixed SDR/HDR consumers of a Lanternbox HDR-only session are unsupported.
Sunshine's existing encoder capability resolution may downgrade an unsupported
10-bit request before capture creation; that cannot authorize capture from an
HDR-only Lanternbox session. Such a session will reject unnegotiated SDR
capture. Qualify the encoder capabilities rather than relying on fallback.
The initial target requires hardware HEVC Main10; AV1 encoding needs a GPU
that actually supports AV1 10-bit encoding. Frame import, channel order, P010
packing/range, metadata in the bitstream and default SDR pixel parity need
remote GPU qualification and independent blind tests. Real-client HDR display,
nested compositor color handling and end-to-end presentation remain separate
acceptance gates. Source inspection and formatting do not establish these.


## Synthetic qualification scope

Three opt-in GPU checks passed for both ranges, P010 packing, grayscale precision
and unsupported formats. `HdrCaptureEncode.SyntheticWaylandHevcMain10` also passed
on a single-output synthetic setup. It checks the HDR control metadata and
writes an IDR only after a software decoder confirms the four known test patches,
avoiding a false pass from the encoder’s blank startup image. The resulting
1920×1080 stream was Main 10, `yuv420p10le`, full-range BT.2020 NCL/ST2084 with
BT.2020/D65 mastering metadata, 0.005-nit black and the configured 10000-nit peak.
Unknown CLL/FALL remained zero in the control message and absent in the bitstream.
The temporary packet must be removed at the end of the authorized test window.

The test requires the opt-in capture and synthetic-only flags, an explicit render
node, an explicit socket exposing one output, and a private QA output path. It
rejects `WAYLAND_SOCKET`, strips child GoogleTest sharding variables and bounds
the worker/decoder process group. The system software FFmpeg decoder is a test
dependency; it is not a runtime dependency of the HDR conversion implementation.
No Moonlight client or physical/XR presentation was accepted by this test.
