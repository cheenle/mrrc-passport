<p align="right">
  <a href="provisioning.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Provisioning without a network of any kind

The terminal has no keyboard, no Bluetooth and no NFC reader — the NTAG213 on
the case is a passive tag that a phone can read and the MCU cannot see. So there
is exactly one way for a phone to hand the device its settings: the device has to
become the access point.

```
no credentials, two failed connection attempts, or a 3 s UP hold
        │
        ├─ device raises SoftAP  "MRRC-Passport-XXXX"  (password mrrcconfig)
        ├─ DHCP serves 192.168.4.1
        ├─ the panel shows the SSID, the password and the URL
        │
        └─ phone joins it, opens http://192.168.4.1, fills the form
              ↓
           saved to NVS, device reboots into station mode
```

The passive NFC tag finally has a job here: write `http://192.168.4.1` (or the
Wi-Fi Simple Config record) into its NDEF and tapping the case opens the page
once the phone has joined the AP. It carries the address, never the data.

## Why NVS over Kconfig

`sdkconfig` is baked at build time; a provisioned device must not need a rebuild.
`main/mrrc_cfg.c` reads the `cfg` namespace in NVS and falls back to
`CONFIG_MRRC_*` when a key is absent, so:

- a fresh build behaves exactly as before (Kconfig credentials),
- a provisioned unit keeps its own settings across reflashes of the app image,
- the alternates from `sdkconfig.defaults.local` still act as extra candidates.

## Trigger conditions

| Condition | Why |
|---|---|
| no SSID at all | factory state |
| two failed connection attempts, 30 s apart | the operator moved somewhere none of the configured networks exist |
| UP held ≥3 s at any time | the operator wants to change Wi-Fi or server settings without rebuilding |

The one-retry delay exists so a router that is merely rebooting does not drop the
device into setup mode.

## If the save fails

The Wi-Fi driver writes an `sta.pwr_reset` entry and an `sta.apinfo` blob per
boot; 24 KB of NVS fills up after enough flashes. `cfg_save()` therefore logs the
NVS statistics, and on failure erases the partition and retries once — setup is
the one moment where wiping is safe, because the credentials on the page are
about to replace what is there anyway.

## Cost

+80 KB flash for `esp_http_server`, and about 15 KB of heap while setup runs. The
latter is affordable precisely because setup happens before the TLS/WebSocket
stack exists: measured `free=73K largest=64K` in setup mode.
