<p align="right">
  <a href="transport-tuning.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Transport tuning: why the terminal stuttered (2026-11-21)

Everything below was measured on the board and on the server, not inferred. The
symptom was steady stutter in both directions; it turned out to be five
independent causes, each of which had to be fixed separately.

Measurement points, for anyone repeating this:

- **Device**: `CONFIG_MRRC_PROFILE=y` prints once a minute
  `link: rx=… underruns=… wr_max=… rb_used_max=… rebuf=… rssi=…`,
  `ws: total=…`, `txlink: send_max=… slow=… loop_max=… stalls=…`,
  and a `tx done:` note per transmission.
- **Server** (`MRRC-BG1SB`, `/opt/mrrc_modern/logs/server-stdout.log`):
  `TX session: frames=… fed=… written=… queue_drops=… peak=…` per
  transmission, and `Terminal media losses: … max pump stall …` per connection.
- **Reference client**: a second `/WSterminal` client on the same Wi-Fi, so
  "what the server is sending" and "what the device receives" can be compared
  in the same window.

## 1. Downlink: the device was the ceiling, not the link

| Same window | Audio | Spectrum | Total |
|---|---|---|---|
| Reference client | 31.3 KB/s | 17.8 KB/s | 50.2 KB/s |
| Device, before | 25.6–28.6 KB/s | — | — |
| Device, after | 31.3 KB/s | — | 49.2 KB/s |

Ruled out first: the Wi-Fi link (the reference client gets 50 KB/s over the same
access point, device RSSI −22 dBm), the CPU (IDLE still 38–46%), and the TCP
window alone (5760 and 16384 measured the same).

What was actually full were the queues behind the window:

| Setting | Was | Now |
|---|---|---|
| `LWIP_TCP_WND_DEFAULT` | 5760 | 11520 |
| `LWIP_TCP_RECVMBOX_SIZE` | 6 | 8 |
| `LWIP_TCP_OOSEQ_MAX_PBUFS` | 4 | 8 |
| `ESP_WIFI_RX_BA_WIN` | 6 | 16 |

## 2. Downlink: playback started from an empty ring

The server does not send at a smooth 50 frames/s. Arrival gaps measured from the
reference client: p50 0 ms, p95 67–74 ms, p99 230–299 ms, about 0.6 gaps over
100 ms per second. Playback used to start on the first frame, so the ring sat at
whatever the last burst left behind — a high-water mark of 5.8 KB out of 16 KB —
and every gap starved the I2S DMA, which `auto_clear_after_cb` filled with
silence. Measured 3–5 ring-empty wakeups per second.

Fix: hold playback until 160 ms is queued (`AUD_PREBUF_BYTES` 5120) and rebuild
that cushion if the ring stays dry for 200 ms (`AUD_REBUFFER_US`). Underruns went
to 0–2/s and re-buffers stopped growing.

Trap: the first version spun in the accumulate path at priority 8 and tripped the
task watchdog. The `vTaskDelay` in that loop is required, not defensive.

## 3. Server side: backpressure had gone away

`max pump stall` in the server's per-connection line fell from **5791 ms** to
**3 ms** once the device's receive queues were raised. That number is the clearest
single indicator that the device is keeping up: it is how long the server's write
to the terminal blocked.

## 4. Uplink: Nagle plus a four-MSS send buffer

First measurement, 6.5 s of PTT: `frames 323`, `sendfail 0`, `readfail 0`,
`loop_max 260 us`, `stalls 0` — capture never missed a frame, the loop never
stalled — yet **100 of 323 sends (31%) blocked more than 30 ms, worst 89 ms**.

Cause: neither `tcp_transport` nor `esp_websocket_client` ever calls
`setsockopt`, so the socket ran with Nagle on. A 641-byte write every 20 ms waits
for the previous unacknowledged segment, and the server's delayed ACK adds tens of
milliseconds. The client does not expose its transport handle, so
`tools/patch_ws_nodelay.sh` sets `TCP_NODELAY` where the state machine knows both
the socket and the connection; `tools/build_on_mac.sh` applies it with the other
managed-component patches.

| Configuration | Sends > 30 ms | Worst send |
|---|---|---|
| Nagle on, SND_BUF 5760 | 100 / 323 = 31% | 89 ms |
| + `TCP_NODELAY` | 28 / 412 = 6.8% | 84 ms |
| + SND_BUF 11520 | 0–3 / 389–497 | 9–17 ms |

The server confirms the other end of it: `frames == fed == written`,
`queue_drops=0`, `write_err=0` on every session.

## 5. Uplink: clipping, and where it actually happened

The server reported `peak=100%` and 5% of TX samples clipped:

```
audio levels: TX gain +12 dB is clipping the signal (57/320 samples ...)
```

Two gains were stacked: the device's ES8311 PGA at **+30 dB** and the server's
software `tx_mic_db` at **+12 dB**. Lowering the server to +3 dB did *not* stop
it (5.0% → 5.8% clipped), which is the useful negative result: the peaks were
already near full scale before the server's gain. Device capture peaked at
27522/32768 = 84%, and 84% × 1.41 (+3 dB) is 118%.

Setting `tx_mic_db` to **0** — the identity, as `audio_levels.py` documents —
gave `peak=40%` and **zero new clipped samples** over a full transmission.

Guidance for later: the analog PGA is the right place to buy level (it is before
the ADC and improves SNR); the server's software gain only rescales samples that
are already quantised. If a more sensitive microphone or a closer mic position
pushes the device peak past ~90%, drop the MIC step to 24 dB rather than
compensating in software.

## 6. Transmit: the downlink was strangling the uplink (2026-11-22)

**Symptom**: press PTT and it stops by itself after a few seconds. The obvious
suspect is the key, and it is innocent — the button log shows one press held for
16.5 s with no release until it was let go.

**What actually happened**:

```
356863  OK LONG_PRESS -> PTT ON
359660  esp_transport_write() returned 0, timeout_ms=1000
        -> /WSterminal error -> disconnected -> "Failed to send the buffer"
361069  tx: frames=78 sendfail=71          <- 7 frames out of 78 got out
363870  reconnected ... and the same thing two seconds later
373062  OK RELEASE (held=16.5 s)
```

**Root cause**: the server's terminal media pump wrote the radio's RX audio to
every subscribed socket without looking at PTT. During transmission that socket
therefore carried **32 KB/s up** (PCM16 microphone) *and* **30 KB/s down** (audio
the device muted and discarded anyway) on one TCP connection. On a mobile link the
downlink saturates the window, the uplink write blocks for a second, and
`esp_websocket_client` treats a failed write as fatal and tears the connection
down — the TX light goes out and it looks like the key released itself.

**Two fixes**:

1. **Server**: while a socket holds the PTT (`_ptt_key_ws is ws`), skip its
   downlink audio entirely. A half-duplex radio is not receiving, so those frames
   are 100% waste. Not counted as drops — the radio produced no audio to lose.
2. **Device**: uplink send timeout 500 ms -> 100 ms. A frame that cannot be
   written in 100 ms cannot be followed by the next four either; failing fast
   costs one frame instead of turning the TX loop into 2 frames/s while the
   microphone keeps producing 50.

**Measured after**, one 30.5 s transmission:

| | before | after |
|---|---:|---:|
| uplink frames | 78 in 4 s (~20/s) | **1500 in 30.5 s = 50.0/s** |
| `sendfail` | 71 / 78 | **0 / 1500** |
| worst send | 500 ms (timeout) | **27.9 ms**, `slow=0` |
| WS disconnects during TX | every ~2 s | **0** |

**A second finding from the same log**: `peak=32768` — the ADC is at full scale.
That is device-side clipping, not the server's gain (which is 0 dB): the ES8311
PGA was at 36 dB. Aim for `peak` between 20000 and 27000 (60–80% of full scale);
30 dB measured 27522 on this board.
