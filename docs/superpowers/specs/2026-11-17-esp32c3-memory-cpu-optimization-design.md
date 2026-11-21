<p align="right">
  <a href="2026-11-17-esp32c3-memory-cpu-optimization-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# ESP32-C3 Memory and CPU Optimization Design

## Context

The MRRC terminal now uses one `/WSterminal` connection for control, downlink
audio, and uplink PCM. Removing the second persistent audio TLS/WebSocket was a
useful memory optimization, but the new 240×54 RGB565 spectrum waterfall has
consumed much of that benefit.

The implementation must retain the complete spectrum and waterfall
presentation. The server protocol, single-socket architecture, 16 kHz codec, and
PTT behaviour are outside the scope of this optimization. There is no codec
library in the image: `main/mrrc_audio.c` passes Int16 PCM straight to the
ES8311, so the micro-opus pseudostack this document used to budget for no longer
exists.

## Status: what actually shipped (2026-11-20, v1.2.0)

All four stages landed, and two findings from the device contradict assumptions
made when this plan was written.

| Stage | Outcome |
| --- | --- |
| 1. Deterministic buffer reuse | Landed. Downlink decimates in place; uplink builds one tagged packet buffer. |
| 2. Task and socket memory | Landed differently: `AUD_TASK_STACK` is 8,192 B, but the `/WSterminal` buffer went **up** 4,096 → 8,192 B. A 4 KB receive buffer back-pressured the socket: the server measured a 5.8 s pump stall. Smaller was not better. |
| 3. 8-bit indexed waterfall | Landed (`main/mrrc_scope.c`), 240×54 with the full Jet palette. |
| 4. Refresh and fragmentation | Landed. `BSP_LVGL_DRAW_BUFFER_LINES` is **24**, not 8: the buffer is not the binding constraint once LVGL uses the system heap. |

Two corrections for anyone reading the rest of this document:

- **The board runs ESP-IDF 5.5.3, not 5.4.2.**
- **`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` must stay off.** `lvgl/lvgl` is
  constrained to `^9.5.0`, which resolves to 9.6.x; 9.6 applies
  `LV_ATTRIBUTE_FAST_MEM` to far more draw code, and with that option on it
  moved ~117 KB into SRAM. On the C3, IRAM and DRAM are one 400 KB pool, so DRAM
  went 170 KB → 287 KB and the boot heap 201 KB → 88 KB, which made
  `esp_wifi_init()` fail with `ESP_ERR_NO_MEM` and reboot-loop the device.

## Current measured state

Measured on the real board in November 2026, ESP-IDF 5.5.3, `-O2`, with
`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` off:

| Metric | Value |
| --- | ---: |
| DRAM used / total | 179,022 / 321,296 B (55.7%) |
| Flash code | 1,175,482 B |
| Boot heap `free` / `largest` | 201 KB / 112 KB |
| Heap after starting the network | 119 KB / 104 KB |
| Steady-state `free` / `largest` | 46–49 KB / 30–32 KB |
| Audio ring / WS receive buffer | 16,384 B / 8,192 B |
| LVGL draw buffer | 24 lines (11,520 B) |
| Steady-state audio drops | ~2% (`err=0`), still open |

## Baseline when this plan was written

The numbers below are from the original plan and are kept only as history; they
describe an Opus-era build that no longer exists.

Latest measured baseline:

| Metric | Current |
| --- | ---: |
| Runtime `free` | 34 KB |
| Runtime `largest` | 11 KB |
| Audio counters | `err=0 drop=39` |
| Audio task stack | 12,288 B allocated, 9,668 B minimum free |
| `libmain.a` `.bss` | 53,364 B |
| Waterfall framebuffer | 25,920 B |
| LVGL draw buffer | 2 lines / 960 B |

The working tree is actively changing. Since this baseline was captured, the
8 KB audio-stack and eight-line LVGL experiments have already appeared in the
worktree, alongside unrelated network-configuration work. Before implementing
any stage, re-read `git diff` and capture a fresh runtime baseline. Preserve all
concurrent work and do not duplicate or overwrite experiments already in
progress.

The previous pre-spectrum runtime measurement was `free=41K`, `largest=17K`,
and `drop=2`. This is a regression signal, not proof that any single change is
the cause. All performance claims must therefore be verified with A/B runtime
measurements rather than inferred from MAP alone.

## Goals

- Preserve the complete spectrum and waterfall presentation.
- Reduce steady-state RAM by approximately 20 KiB without changing the server.
- Reduce waterfall copying and LVGL flush overhead.
- Restore audio reliability to the pre-spectrum drop rate under comparable
  network conditions.
- Improve `largest` and the PTT minimum heap, not only total `free`.
- Keep each risky change independently testable and reversible.

## Non-goals

- No server or protocol changes.
- No codec sample-rate changes and no runtime codec reconfiguration.
- No reduction in waterfall dimensions, palette, or spectrum content.
- No new audio codec; the wire format is PCM16 both ways.
- No spectrum/waterfall removal or move to a separate page.
- No wholesale rewrite of cJSON handling in the first implementation stage.

## Selected approach

Use staged optimization. First reclaim deterministic memory and measure the
result. Then replace the waterfall storage and rendering with an 8-bit indexed
ring buffer. Restore the LVGL draw buffer to eight lines after enough memory
has been reclaimed, and compare audio drops and rendering cost against the
current two-line implementation.

### Expected memory budget

| Change | Expected internal-RAM effect |
| --- | ---: |
| In-place 48 kHz to 16 kHz decimation | +1,920 B |
| Single aligned TX capture/upsample/packet buffer | about +2,559 B |
| 8-bit waterfall instead of RGB565 | +12,960 B |
| Double 240-column spectrum receive buffers | about +610 B |
| Audio task stack 12,288 B to 8,192 B | +4,096 B, conditional on testing |
| `/WSterminal` buffer 4096 to 2048 | +2,048 B, conditional on testing |
| Restore LVGL draw buffer from 2 to 8 lines | −2,880 B |
| **Expected net effect** | **about +21.3 KB** |

These are source/MAP estimates, not guaranteed runtime results. Alignment and
heap layout can change the final numbers.

## Stage 1: deterministic buffer reuse

### Downlink decimation

`main/mrrc_audio.c` currently owns both `s_pcm48` and `s_pcm16`. Decimate in
place into `s_pcm48`: the output index never exceeds the input index, so this
cannot overwrite samples that have not been read. Pass the same buffer and the
decoded output length to `bsp_audio_write()` and remove `s_pcm16`.

### Uplink packet construction

`main/mrrc_tx.c` currently uses separate `mic`, `up`, and tagged `frame`
arrays. Replace them with one aligned `int16_t` buffer:

1. Read 320 mono samples into the start of the buffer.
2. Expand backwards from sample 319 to sample 0 so each input sample becomes
   three output samples without overwriting unread input.
3. Move the 1,920 payload bytes by one byte and insert the transport tag.
4. Send exactly 1,921 bytes and count a frame only when the complete packet was
   accepted.

Use a temporary host-side test program outside the repository to verify all
320 input positions, negative samples, boundary values, and the final tagged
byte layout. Do not add a new permanent test framework to this repository.

## Stage 2: conditional task and socket memory

Reduce `AUD_TASK_STACK` from 12,288 to 8,192 bytes as a separate experiment.
The current runtime high-water mark shows only about 2.6 KB used, but an older
8 KB overflow report must be treated as a conflicting result. Do not reduce it
further to 6 KB. Exercise normal decode, PLC, PTT, reconnect, and spectrum
rendering while recording stack high-water marks.

Reduce the single `/WSterminal` buffer from 4096 to 2048 bytes only after
confirming all current traffic fits:

- uplink packet: 1,921 B plus protocol overhead;
- spectrum payload: approximately 852 B;
- large control JSON: reassembled by the existing accumulator.

A partial send or a send failure must not be counted as a successful audio
frame.

## Stage 3: 8-bit indexed waterfall

### Storage

Replace `uint16_t s_wf_buf[240 * 54]` with
`uint8_t s_wf_buf[240 * 54]`. Each byte is an index into the existing 64-entry
Jet palette. The palette itself remains RGB565 and costs only 128 bytes.

The display still renders every pixel in RGB565. Only the persistent history
representation changes.

### Ring layout

Keep a newest-row index instead of moving 25 KB on every update:

- each refresh writes one 240-byte row of palette indices;
- the newest physical row advances modulo 54;
- the renderer maps display row `y` to
  `(newest_row + 1 + y) % 54`, producing oldest-to-newest order;
- no full-waterfall `memmove` remains.

Remove the additional waterfall shift currently performed when a new spectrum
frame arrives. Each refresh tick writes exactly one interpolated row.

### LVGL rendering

Use a dedicated scope module rather than adding more rendering code to
`mrrc_ui.c`:

- `main/mrrc_scope.c` owns spectrum downsampling, ring storage, palette lookup,
  and the custom waterfall draw callback;
- `main/mrrc_scope.h` exposes initialization, spectrum ingestion, and refresh;
- `main/mrrc_ui.c` creates the scope objects and calls the module;
- `main/CMakeLists.txt` registers the new source file.

The waterfall uses a custom LVGL object draw event. During `LV_EVENT_DRAW_MAIN`
it converts only pixels inside the current clip area from 8-bit palette indices
to RGB565 in the LVGL layer. It must respect the layer stride and clipping
rectangle and must not allocate a full RGB565 intermediate framebuffer.

The direct path is valid only for an RGB565 layer. If LVGL supplies another
format, use an LVGL-supported colour write path rather than performing an
unchecked cast. No persistent allocation is allowed in the draw callback.

The spectrum trace continues to use the existing point array. The current LVGL
version stores the point pointer without copying it, so no second point buffer
is needed.

### Spectrum ingestion

Downsample 850 bins to 240 columns in the WebSocket callback and publish into
one of two 240-column buffers. Swap buffers only after a complete frame is
written. This removes the 850-byte persistent raw array, saves approximately
610 B net, and prevents the UI from reading a mixture of two spectrum frames.

The WebSocket callback must not access LVGL objects.

## Stage 4: refresh and fragmentation control

Restore `BSP_LVGL_DRAW_BUFFER_LINES` to 8 after the preceding memory savings.
Compare 2-line and 8-line builds for audio drops, UI responsiveness, and CPU.
Do not retain 2 lines merely for its 2,880 B saving if drops remain elevated.

Reduce unnecessary LVGL string allocation:

- redraw the battery label only when the displayed percentage changes;
- bump the main UI sequence only when a displayed radio field actually changes;
- avoid replacing labels whose formatted text is unchanged.

Do not replace cJSON in the first implementation pass. If `largest` remains
below 17 KB after the above work, profile cJSON allocation counts and then
consider a small allocation-free parser for the known control schema.

## Verification

### Build and static measurements

- Build the current source with ESP-IDF 5.5.3 and `-O2`.
- Compare `libmain.a` `.bss`, total DRAM, and firmware size against baseline.
- Confirm no credentials or local `sdkconfig` values enter tracked files.

### Temporary host checks

Use disposable host programs outside the repository for:

- in-place decimation;
- backwards in-place 3× upsampling and tag layout;
- waterfall ring-row mapping at wraparound;
- double-buffer spectrum publication.

### Device checks

Capture 60-second runtime records for each risky stage:

```text
free / largest / min / dma_free
audio frames / err / drop
audio stack high-water mark
PTT keys / frames / sendfail / readfail / last event
```

Acceptance criteria:

- `err=0` and no unexplained increase in drops.
- Back-to-back comparable listening tests return to the pre-spectrum drop
  behaviour; the target is no more than 2 drops over a comparable six-minute
  window, not an absolute guarantee across changing network conditions.
- No Guru Meditation, task watchdog, or partial-send accounting.
- Audio task stack remains above a 2 KB high-water margin after stress tests.
- Runtime `largest` is at least 17 KB where possible and never below the
  repository minimum of 8 KB.
- PTT is heard clearly once by the user, with normal receive audio after unkey.
- Waterfall remains 240×54 with the complete Jet palette and correct row order.

After the staged checks pass, run a 30-minute receive test and ten PTT cycles.
Record RX-idle, TX-ready, and post-unkey `free/largest/min` for every cycle.

## Rollback boundaries

- Revert the audio stack to 12 KB on any stack regression.
- Revert the WebSocket buffer to 4096 on any send/receive regression.
- Revert only the custom waterfall renderer if clipping, colour, or draw-time
  behaviour is wrong; retain the independent buffer-reuse improvements.
- Restore LVGL to eight lines even if this requires leaving another optional
  optimization out.

## Final implementation constraint

No firmware will be flashed without explicit approval at the time of flashing.
Successful compilation and MAP improvements are not device validation.
