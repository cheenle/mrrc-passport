<p align="right">
  <strong>简体中文</strong> · <a href="provisioning.md">English</a>
</p>

# 在完全没有网络的情况下配网

这台终端没有键盘、没有蓝牙、也没有 NFC 读卡器 —— 机身上的 NTAG213 是被动标签，
手机能读，MCU 看不见。所以手机要把设置交给设备，只有一条路：**设备自己变成热点**。

```
没有凭据，连续两次连接失败，或长按 UP ≥3 秒
        │
        ├─ 设备开 SoftAP  "MRRC-Passport-XXXX"（密码 mrrcconfig）
        ├─ DHCP 提供 192.168.4.1
        ├─ 屏幕显示 SSID、密码和网址
        │
        └─ 手机连上，打开 http://192.168.4.1，填表提交
              ↓
           存入 NVS，设备重启进入 STA 模式
```

那颗被动 NFC 标签在这里终于有了正当用途：把 `http://192.168.4.1`（或 Wi-Fi
Simple Config 记录）写进它的 NDEF，手机连上 AP 后贴一下就能打开页面。它只携带
地址，不携带数据。

## 为什么是 NVS 而不是 Kconfig

`sdkconfig` 是编译期烘死的，配网过的设备不该为此重新编译。`main/mrrc_cfg.c` 读
NVS 的 `cfg` 命名空间，缺键时回落到 `CONFIG_MRRC_*`，于是：

- 全新构建的行为与以前完全一致（用 Kconfig 里的凭据）；
- 配网过的设备在重刷应用镜像后仍保留自己的设置；
- `sdkconfig.defaults.local` 里的备用网络仍作为额外候选参与选择。

## 触发条件

| 条件 | 场景 |
|---|---|
| 完全没有 SSID | 出厂状态 |
| 连续两次连接失败，间隔 30 s | 人移动到了三个配置网络都不存在的地方 |
| 任意时刻长按 UP ≥3 s | 想改 Wi-Fi 或服务器设置、又不想重新编译 |

中间那次重试是为了不让"路由器恰好重启"把设备推进配网模式。

## 保存失败时

Wi-Fi 驱动每次开机都会写一条 `sta.pwr_reset` 和一个 `sta.apinfo` 块，24 KB 的 NVS
在反复烧写之后会满。因此 `cfg_save()` 会打印 NVS 统计，失败时擦除分区重试一次 ——
配网正是唯一可以安全擦除的时刻，因为页面上的凭据本来就要覆盖这里的内容。

## 代价

`esp_http_server` 占 **+80 KB flash**，配网期间约占 **15 KB 堆**。后者之所以可承受，
是因为配网发生在 TLS/WebSocket 栈建立之前：实测配网模式下 `free=73K largest=64K`。
