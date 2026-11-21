#!/usr/bin/env bash
# patch_ws_nodelay.sh — turn on TCP_NODELAY for the /WSterminal socket.
#
# Neither tcp_transport nor esp_websocket_client ever calls setsockopt, so the
# uplink audio socket runs with Nagle enabled.  The operator's 20 ms PCM frames
# are 641-byte writes: Nagle holds each small segment until the previous one is
# ACKed, and the server's delayed ACK adds tens of milliseconds on top.  Measured
# on the board: 100 of 323 uplink sends blocked for more than 30 ms, worst 89 ms.
# That is heard at the far end as chopped audio.
#
# The client does not expose its transport handle, so the flag has to go in at
# the one place that knows both the socket and the state machine.  Idempotent;
# safe to run before every build.  Requires managed_components to exist.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$HERE/managed_components/espressif__esp_websocket_client/esp_websocket_client.c"

[ -f "$WS" ] || { echo "esp_websocket_client not fetched yet; run a build first"; exit 0; }

python3 - "$WS" <<'PY'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()

if "MRRC_TCP_NODELAY" in src:
    print("already patched")
    raise SystemExit(0)

anchor = """            client->state = WEBSOCKET_STATE_CONNECTED;
            client->wait_for_pong_resp = false;
            client->error_handle.error_type = WEBSOCKET_ERROR_TYPE_NONE;"""
patch = """            client->state = WEBSOCKET_STATE_CONNECTED;
            client->wait_for_pong_resp = false;
            /* MRRC_TCP_NODELAY: real-time audio uplink must not wait for the
             * peer's delayed ACK.  See tools/patch_ws_nodelay.sh. */
            {
                int fd = esp_transport_get_socket(client->transport);
                if (fd >= 0) {
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                }
            }
            client->error_handle.error_type = WEBSOCKET_ERROR_TYPE_NONE;"""

if anchor not in src:
    print("anchor not found - esp_websocket_client layout changed", file=sys.stderr)
    raise SystemExit(1)

src = src.replace(anchor, patch, 1)

# lwIP's socket options need the headers; add them next to the existing includes.
if "#include <sys/socket.h>" not in src:
    src = src.replace('#include "esp_transport.h"',
                      '#include "esp_transport.h"\n#include <sys/socket.h>\n#include <netinet/tcp.h>', 1)
open(path, "w", encoding="utf-8").write(src)
print("patched: TCP_NODELAY on websocket connect")
PY
