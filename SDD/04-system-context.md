<p align="right">
  <strong>English</strong> · <a href="04-system-context.zh_CN.md">简体中文</a>
</p>

# 04. System Context (APP 011)

## 4.1 Systems and actors

| System / actor | Inside this document's boundary? | What crosses the boundary |
| --- | --- | --- |
| Operator | no (human) | key presses, knob rotation, what the panel shows them |
| Terminal firmware | **yes** | everything in chapters `05`–`15` |
| `mrrc_modern` server | no (separate record) | control JSON, PCM16 audio both ways, spectrum frames, keyer arbitration |
| Radio (FT-710 and other supported models) | no (reached only through the server) | nothing directly: the terminal has no CAT link |
| Phone (provisioning) | no | one HTTP form on the terminal's own SoftAP |
| Access point / internet | no | IPv6 path to `w103d.vlsc.net:8888` (the server is IPv6-only) |
| Battery / charger | no (board) | voltage, charge state, into the battery indicator |

The terminal has exactly **one** network peer that matters (the server) and exactly
**one** input device class that matters (the keys). Everything else it knows arrives
through the server, which is why the server's decisions appear in this document
only as dependencies (chapter `08` AD-004, §13.5).

## 4.2 Interfaces

| # | Interface | Direction | Protocol | Contract | Owner |
| --- | --- | --- | --- | --- | --- |
| I-1 | Session control | terminal → server | WS text, JSON | `stateUpdate` on merge semantics; the terminal never invents state | shared, server-authoritative |
| I-2 | Receive audio | server → terminal | WS binary, channel `0x00` | `[0x00][tag 0x02][int16 LE]`, 16 kHz mono, 20 ms blocks | firmware owns playback only |
| I-3 | Transmit audio | terminal → server | WS binary, channel `0x01` | `[0x01][tag 0x02][int16 LE]`, 641 B per frame, 100 ms write timeout | firmware (AD-004) |
| I-4 | Spectrum | server → terminal | WS binary, channel `0x02` | one frame per radio sweep; the terminal may drop tiers while transmitting | firmware receives only |
| I-5 | Keyer arbitration | terminal ↔ server | WS JSON | the server decides; a refusal is visible, never a silent carrier | server |
| I-6 | Provisioning | phone → terminal | HTTP over SoftAP | one page, SSID + password, save-then-reboot | firmware (AD-006) |
| I-7 | Codec | firmware → ES8311 | I2S0 (shared MCLK/BCLK/WS) + I2C | 16 kHz both directions, configured once | firmware (AD-003) |
| I-8 | Panel | firmware → ST7789 | SPI + LVGL port | 240×320, single 24-line draw buffer, 190 ms refresh | firmware |
| I-9 | Power | board → firmware | ADC + charger status | percent + charging flag for the indicator | board (BSP) |

## 4.3 Network topology and its three switches

The server is reachable over **IPv6 only**, and lwIP is not IPv6-ready by default.
Three things must all be true (`../AGENTS.md` §2) — two of them are configuration
and the third is a call:

```c
CONFIG_LWIP_IPV6=y                    // compile IPv6 into lwIP
CONFIG_LWIP_IPV6_AUTOCONFIG=y         // accept Router Advertisements (SLAAC)
esp_netif_create_ip6_linklocal(netif) // and call it after GOT_IP
```

With only the first two, the interface reports `0 IPv6 address(es)` forever, because
`esp_netif_create_default_wifi_sta()` does not attach a link-local address itself.
This was the root cause of two failed domain switches, and it is why the terminal's
network configuration is described as a three-part fact rather than a setting.

## 4.4 Data flows

```
operator ──keys──▶ terminal ──I-3 TX PCM16──▶ server ──CAT/audio──▶ radio
                     ▲                          │
                     ├──I-2 RX PCM16────────────┤
                     ├──I-4 spectrum────────────┤
                     └──I-1 control JSON────────┘

phone ──I-6 HTTP over SoftAP──▶ terminal (no server involved)
```

Three properties of this picture are load-bearing:

1. **the terminal has no direct path to the radio** — every radio behaviour it shows
   was asserted by the server first;
2. **provisioning is the only flow that does not involve the server**, which is why
   it must work with no network at all (AD-006);
3. **audio and control share one TCP connection**, so a media stall and a control
   stall are the same stall — this is the assumption behind AD-004 and the reason
   media congestion must never be treated as a control failure.

## 4.5 What this document does not own

| Concern | Owner | Where it is recorded |
| --- | --- | --- |
| Session policy, keyer arbitration, downlink suppression, RX/TX gain | server | `mrrc_modern/SDD/` (AD-039, AD-040) |
| CAT commands, filter tables, band limits | server + radio | server record, radio manual |
| Radio TOT | hardware | radio manual; chapter `15` §15.1 treats it as the last stop |
| Cloud hub, remote access, ticketing | `mrrc_hub` | that repository's SDD |
| Build/CI infrastructure | this repository | chapter `03` SC10, `../docs/development/ci/` |
