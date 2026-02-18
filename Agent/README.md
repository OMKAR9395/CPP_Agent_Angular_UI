# Device Agent (C++ Serial + REST + WebSocket)

Cross-platform local agent (Windows/Linux/macOS) that:

- Enumerates serial devices (`COM*`, `/dev/tty*`, `/dev/cu.*`)
- Connects to multiple devices concurrently
- Keeps ports open exclusively while connected
- Auto-reconnects with backoff on unplug/replug
- Exposes localhost REST APIs and WebSocket streaming

## Tech stack

- CMake
- `libserialport` for serial I/O
- `cpp-httplib` for REST API
- Boost.Beast (Boost.Asio) for WebSocket (`/ws`)
- `nlohmann/json` for JSON

`cpp-httplib` and `nlohmann/json` are pulled automatically via CMake `FetchContent`.

## Project layout

```text
.
|-- CMakeLists.txt
|-- README.md
|-- third_party/
`-- src/
    |-- main.cpp
    |-- api/
    |   |-- HttpServer.h
    |   |-- HttpServer.cpp
    |   |-- WebSocketServer.h
    |   `-- WebSocketServer.cpp
    |-- core/
    |   |-- Backoff.h
    |   |-- Backoff.cpp
    |   |-- EventBus.h
    |   |-- EventBus.cpp
    |   |-- SessionManager.h
    |   `-- SessionManager.cpp
    `-- serial/
        |-- PortEnumerator.h
        |-- PortEnumerator.cpp
        |-- SerialManager.h
        `-- SerialManager.cpp
```

## Dependencies

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  libserialport-dev libboost-system-dev libboost-thread-dev
```

### macOS (Homebrew)

```bash
brew install cmake pkg-config libserialport boost
```

### Windows

Recommended: Visual Studio 2022 + CMake + vcpkg.

```powershell
vcpkg install libserialport boost-system boost-thread
```

Then configure CMake with toolchain:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Run

Environment variables:

- `AGENT_PORT` (default `8080`)
- `AGENT_WS_PORT` (default `AGENT_PORT + 1`)
- `CORS_ORIGINS` (comma-separated allowlist; default `http://localhost:3000,http://127.0.0.1:3000`)
- `AGENT_MOCK_SERIAL` (`1/true/yes` to enable `MOCK0`)

Example:

```bash
AGENT_PORT=8080 AGENT_WS_PORT=8081 CORS_ORIGINS=http://localhost:3000 AGENT_MOCK_SERIAL=1 ./build/device_agent
```

Windows (PowerShell):

```powershell
$env:AGENT_PORT="8080"
$env:AGENT_WS_PORT="8081"
$env:CORS_ORIGINS="http://localhost:3000"
$env:AGENT_MOCK_SERIAL="1"
.\build\Release\device_agent.exe
```

## REST API

Base URL: `http://127.0.0.1:8080`

### 1) List devices

```bash
curl http://127.0.0.1:8080/api/v1/serial/devices
```

### 2) Validate (handshake)

Send `PING\n` and match `PONG`:

```bash
curl -X POST http://127.0.0.1:8080/api/v1/serial/validate \
  -H "Content-Type: application/json" \
  -d '{
    "port":"COM5",
    "baud":9600,
    "validate":{
      "type":"handshake",
      "command_b64":"UElORwo=",
      "expected_regex":"PONG",
      "read_timeout_ms":500,
      "max_read_bytes":1024
    }
  }'
```

If in use by another app, response is `409` with:

```json
{"ok":false,"error":"PORT_IN_USE","message":"Port is being used by another application."}
```

### 3) Connect

```bash
curl -X POST http://127.0.0.1:8080/api/v1/serial/connect \
  -H "Content-Type: application/json" \
  -d '{
    "port":"COM5",
    "baud":9600,
    "mode":{"type":"line","delimiter":"\n"}
  }'
```

Response includes `session_id` and `owner_token`.

### 4) Write

```bash
curl -X POST http://127.0.0.1:8080/api/v1/serial/write \
  -H "Content-Type: application/json" \
  -H "X-Owner-Token: <owner_token>" \
  -d '{"session_id":"<session_id>","data_b64":"SEVMTE8K"}'
```

### 5) Read

```bash
curl "http://127.0.0.1:8080/api/v1/serial/read?session_id=<session_id>&timeout_ms=100&max=512" \
  -H "X-Owner-Token: <owner_token>"
```

### 6) Disconnect

```bash
curl -X POST http://127.0.0.1:8080/api/v1/serial/disconnect \
  -H "Content-Type: application/json" \
  -d '{"session_id":"<session_id>"}'
```

### 7) Status

```bash
curl http://127.0.0.1:8080/api/v1/status
```

## WebSocket API

- URL: `ws://127.0.0.1:8081/ws` (default port shown)
- Client subscribe message:

```json
{"subscribe":["events","serial_data"],"session_id":"optional"}
```

Server emits:

- `device_attached`, `device_detached`
- `serial_connected`, `serial_disconnected`, `serial_reconnecting`
- `serial_data`
- `error` (when published by server components)

### Browser JS demo (prints barcode-like lines)

```html
<script>
  const ws = new WebSocket("ws://127.0.0.1:8081/ws");
  ws.onopen = () => {
    ws.send(JSON.stringify({ subscribe: ["events", "serial_data"] }));
  };
  ws.onmessage = (evt) => {
    const msg = JSON.parse(evt.data);
    if (msg.type === "serial_data") {
      console.log("barcode:", (msg.text || "").trim());
    } else {
      console.log("event:", msg);
    }
  };
</script>
```

## Notes

- Agent binds only to `127.0.0.1`.
- Busy detection and unique ID are best-effort.
- Linux unique ID tries `/dev/serial/by-id/*` symlink mapping.
- On Windows/macOS, `unique_id` may be empty in MVP.
- Reconnect backoff is `0.5s -> 1s -> 2s -> 5s -> 10s (cap)`.
- `AGENT_MOCK_SERIAL=1` adds `MOCK0`, which emits `TEST123\n` every second.
