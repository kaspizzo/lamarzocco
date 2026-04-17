"""Minimal local Bluetooth proof-of-concept app for La Marzocco machines."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path
from typing import Any

from aiohttp import web

WORKSPACE_ROOT = Path(__file__).resolve().parent
WORKER_SCRIPT = WORKSPACE_ROOT / "bt_poc_worker.py"
INDEX_FILE = WORKSPACE_ROOT / "bt_poc_index.html"


INDEX_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>La Marzocco Bluetooth POC</title>
  <style>
    :root {
      --bg: #f5efe5;
      --panel: rgba(255, 252, 247, 0.92);
      --ink: #1c1a17;
      --muted: #6b6258;
      --line: rgba(28, 26, 23, 0.12);
      --accent: #a33f1f;
      --accent-2: #244b5a;
      --ok: #1d6b43;
      --warn: #8a5a10;
      --error: #9d2d20;
      --shadow: 0 24px 70px rgba(79, 55, 30, 0.12);
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      font-family: "Avenir Next", "Segoe UI", sans-serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(163, 63, 31, 0.16), transparent 32%),
        radial-gradient(circle at top right, rgba(36, 75, 90, 0.14), transparent 26%),
        linear-gradient(180deg, #fcf8f1 0%, var(--bg) 100%);
      min-height: 100vh;
    }

    main {
      width: min(1120px, calc(100% - 32px));
      margin: 32px auto 48px;
    }

    .hero {
      margin-bottom: 24px;
      padding: 28px;
      border: 1px solid var(--line);
      border-radius: 28px;
      background: linear-gradient(135deg, rgba(255, 255, 255, 0.88), rgba(250, 243, 233, 0.95));
      box-shadow: var(--shadow);
    }

    .eyebrow {
      margin: 0 0 10px;
      font-size: 12px;
      letter-spacing: 0.18em;
      text-transform: uppercase;
      color: var(--accent);
      font-weight: 700;
    }

    h1 {
      margin: 0 0 10px;
      font-size: clamp(32px, 6vw, 56px);
      line-height: 0.95;
      letter-spacing: -0.04em;
    }

    .hero p {
      margin: 0;
      color: var(--muted);
      max-width: 760px;
      line-height: 1.5;
      font-size: 16px;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
      gap: 18px;
    }

    .card {
      padding: 20px;
      border: 1px solid var(--line);
      border-radius: 24px;
      background: var(--panel);
      box-shadow: var(--shadow);
      backdrop-filter: blur(8px);
    }

    .card h2 {
      margin: 0 0 14px;
      font-size: 18px;
      letter-spacing: -0.02em;
    }

    .stack {
      display: grid;
      gap: 12px;
    }

    .row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }

    label {
      display: grid;
      gap: 6px;
      font-size: 13px;
      color: var(--muted);
      font-weight: 600;
    }

    input, select, button, textarea {
      font: inherit;
    }

    input, textarea {
      width: 100%;
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 12px 14px;
      background: rgba(255, 255, 255, 0.88);
      color: var(--ink);
    }

    textarea {
      min-height: 180px;
      resize: vertical;
      font-family: "SF Mono", "Menlo", monospace;
      font-size: 13px;
      line-height: 1.4;
    }

    button {
      border: 0;
      border-radius: 999px;
      padding: 12px 16px;
      background: var(--accent);
      color: white;
      cursor: pointer;
      font-weight: 700;
      transition: transform 120ms ease, opacity 120ms ease;
    }

    button.secondary {
      background: var(--accent-2);
    }

    button.ghost {
      background: rgba(28, 26, 23, 0.08);
      color: var(--ink);
    }

    button:hover {
      transform: translateY(-1px);
    }

    button:disabled {
      opacity: 0.55;
      cursor: progress;
      transform: none;
    }

    .button-row {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }

    .status {
      min-height: 48px;
      padding: 12px 14px;
      border-radius: 16px;
      background: rgba(28, 26, 23, 0.05);
      color: var(--muted);
      white-space: pre-wrap;
      line-height: 1.45;
    }

    .status.ok {
      background: rgba(29, 107, 67, 0.1);
      color: var(--ok);
    }

    .status.warn {
      background: rgba(138, 90, 16, 0.12);
      color: var(--warn);
    }

    .status.error {
      background: rgba(157, 45, 32, 0.1);
      color: var(--error);
    }

    .hint {
      margin: 0;
      font-size: 13px;
      color: var(--muted);
      line-height: 1.45;
    }

    ul.device-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: grid;
      gap: 10px;
    }

    ul.device-list li {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      padding: 12px 14px;
      border-radius: 16px;
      background: rgba(28, 26, 23, 0.04);
    }

    code {
      font-family: "SF Mono", "Menlo", monospace;
      font-size: 12px;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      padding: 4px 8px;
      border-radius: 999px;
      background: rgba(36, 75, 90, 0.1);
      color: var(--accent-2);
      font-size: 12px;
      font-weight: 700;
    }

    @media (max-width: 740px) {
      main { width: min(100% - 20px, 1120px); margin-top: 20px; }
      .hero, .card { border-radius: 22px; }
      .row { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <p class="eyebrow">Bluetooth Proof of Control</p>
      <h1>La Marzocco Mini App</h1>
      <p>
        This local app is meant to prove that control works over Bluetooth without
        the cloud path. It scans for nearby La Marzocco devices, can read the BLE
        token in pairing mode, reads live status, and sends coffee temperature,
        power, and steam commands.
      </p>
    </section>

    <section class="grid">
      <article class="card">
        <h2>0. Cloud bootstrap</h2>
        <div class="stack">
          <div class="row">
            <label>
              La Marzocco account email
              <input id="cloud-username" type="email" placeholder="name@example.com">
            </label>
            <label>
              Password
              <input id="cloud-password" type="password" placeholder="App password">
            </label>
          </div>
          <div class="button-row">
            <button id="cloud-bootstrap-button">Fetch machines and BLE tokens</button>
          </div>
          <p class="hint">
            This only uses the cloud to bootstrap account data and retrieve the BLE token.
            The installation key is stored locally in this workspace and reused on later runs.
          </p>
          <label>
            Selected machine serial
            <input id="serial-number" placeholder="Choose a machine below or paste serial number">
          </label>
          <ul class="device-list" id="machine-list"></ul>
        </div>
      </article>

      <article class="card">
        <h2>1. Device</h2>
        <div class="stack">
          <div class="button-row">
            <button id="scan-button">Scan for machines</button>
            <span class="badge" id="device-count">0 found</span>
          </div>
          <ul class="device-list" id="device-list"></ul>
          <label>
            BLE address
            <input id="address" placeholder="MAC address from scan result">
          </label>
          <div class="button-row">
            <button class="secondary" id="read-token-button">Read token in pairing mode</button>
          </div>
          <p class="hint">
            Token reading only works while the machine is in pairing mode. Otherwise,
            scan the QR code inside the machine and paste the BLE token below.
          </p>
        </div>
      </article>

      <article class="card">
        <h2>2. Session</h2>
        <div class="stack">
          <label>
            BLE token
            <input id="token" placeholder="Paste token or read it via pairing mode">
          </label>
          <div class="button-row">
            <button id="status-button">Read status</button>
            <button class="ghost" id="clear-button">Clear output</button>
          </div>
          <div id="status-message" class="status">
            Ready. Scan first, then choose a device and token.
          </div>
        </div>
      </article>

      <article class="card">
        <h2>3. Cloud control</h2>
        <div class="stack">
          <div class="button-row">
            <button id="cloud-status-button">Read cloud dashboard</button>
            <button class="secondary" id="cloud-power-on-button">Cloud power on</button>
            <button class="ghost" id="cloud-power-off-button">Cloud power off</button>
          </div>
          <div class="button-row">
            <button class="secondary" id="cloud-steam-on-button">Cloud steam on</button>
            <button class="ghost" id="cloud-steam-off-button">Cloud steam off</button>
          </div>
          <div class="row">
            <label>
              Cloud coffee target temperature (C)
              <input id="cloud-coffee-temp" type="number" step="0.1" value="93.0">
            </label>
            <label>
              Pre-extraction mode
              <select id="pre-extraction-mode">
                <option value="disabled">Disabled</option>
                <option value="prebrew" selected>Prebrew</option>
                <option value="preinfusion">Preinfusion</option>
              </select>
            </label>
          </div>
          <div class="button-row">
            <button id="cloud-coffee-temp-button">Set cloud coffee temp</button>
            <button class="secondary" id="pre-extraction-mode-button">Set mode</button>
          </div>
          <div class="row">
            <label>
              Prebrew seconds on
              <input id="prebrew-seconds-in" type="number" step="0.1" value="1.0">
            </label>
            <label>
              Prebrew seconds off
              <input id="prebrew-seconds-out" type="number" step="0.1" value="2.0">
            </label>
          </div>
          <div class="button-row">
            <button id="prebrew-times-button">Set prebrew times</button>
          </div>
          <p class="hint">
            This section uses the cloud API and is the current fallback for a working baseline,
            especially for prebrew mode and timing.
          </p>
        </div>
      </article>

      <article class="card">
        <h2>4. Local Bluetooth control</h2>
        <div class="stack">
          <div class="button-row">
            <button id="power-on-button">Power on</button>
            <button class="ghost" id="power-off-button">Power off</button>
          </div>
          <div class="button-row">
            <button class="secondary" id="steam-on-button">Steam on</button>
            <button class="ghost" id="steam-off-button">Steam off</button>
          </div>
          <div class="row">
            <label>
              Coffee target temperature (C)
              <input id="coffee-temp" type="number" step="0.1" value="93.0">
            </label>
            <label>
              Mini R steam level presets
              <select id="steam-level">
                <option value="126">Level 1 (126 C)</option>
                <option value="128" selected>Level 2 (128 C)</option>
                <option value="131">Level 3 (131 C)</option>
              </select>
            </label>
          </div>
          <div class="button-row">
            <button id="coffee-temp-button">Set coffee temp</button>
            <button class="secondary" id="steam-level-button">Set steam level</button>
          </div>
          <p class="hint">
            Prebrew and preinfusion are intentionally not in this app because the current
            library exposes those paths only via cloud, not via Bluetooth.
          </p>
        </div>
      </article>

      <article class="card">
        <h2>5. Output</h2>
        <div class="stack">
          <textarea id="output" readonly></textarea>
        </div>
      </article>
    </section>
  </main>

  <script>
    const addressInput = document.getElementById("address");
    const tokenInput = document.getElementById("token");
    const output = document.getElementById("output");
    const statusMessage = document.getElementById("status-message");
    const deviceCount = document.getElementById("device-count");
    const deviceList = document.getElementById("device-list");
    const machineList = document.getElementById("machine-list");
    const cloudUsernameInput = document.getElementById("cloud-username");
    const cloudPasswordInput = document.getElementById("cloud-password");
    const serialNumberInput = document.getElementById("serial-number");

    function maskSecrets(value) {
      if (Array.isArray(value)) {
        return value.map(maskSecrets);
      }
      if (!value || typeof value !== "object") {
        return value;
      }
      const clone = {};
      for (const [key, entry] of Object.entries(value)) {
        if (entry && typeof entry === "object") {
          clone[key] = maskSecrets(entry);
        } else if (typeof entry === "string" && ["token", "ble_auth_token"].includes(key)) {
          clone[key] = entry.length > 12 ? `${entry.slice(0, 6)}...${entry.slice(-4)}` : "***";
        } else {
          clone[key] = entry;
        }
      }
      return clone;
    }

    function appendOutput(label, data) {
      const safe = typeof data === "string" ? data : maskSecrets(data);
      const pretty = typeof safe === "string" ? safe : JSON.stringify(safe, null, 2);
      output.value = `${new Date().toLocaleTimeString()} ${label}\\n${pretty}\\n\\n${output.value}`;
    }

    function setStatus(kind, message) {
      statusMessage.className = `status ${kind}`;
      statusMessage.textContent = message;
    }

    function currentPayload(extra = {}) {
      return {
        address: addressInput.value.trim(),
        token: tokenInput.value.trim(),
        ...extra,
      };
    }

    function currentCloudPayload() {
      return {
        username: cloudUsernameInput.value.trim(),
        password: cloudPasswordInput.value,
      };
    }

    function currentCloudMachinePayload(extra = {}) {
      return {
        ...currentCloudPayload(),
        serial_number: serialNumberInput.value.trim(),
        ...extra,
      };
    }

    async function callApi(path, payload = {}, method = "POST") {
      const response = await fetch(path, {
        method,
        headers: { "Content-Type": "application/json" },
        body: method === "GET" ? undefined : JSON.stringify(payload),
      });
      const data = await response.json();
      if (!response.ok || data.ok === false) {
        const message = data.error || response.statusText;
        throw new Error(message);
      }
      return data;
    }

    function setBusy(button, busy) {
      button.disabled = busy;
    }

    async function run(button, label, fn) {
      setBusy(button, true);
      setStatus("warn", `${label}...`);
      try {
        const data = await fn();
        setStatus("ok", `${label} succeeded.`);
        appendOutput(label, data);
      } catch (error) {
        setStatus("error", `${label} failed: ${error.message}`);
        appendOutput(`${label} failed`, error.message);
      } finally {
        setBusy(button, false);
      }
    }

    function renderDevices(devices) {
      deviceCount.textContent = `${devices.length} found`;
      deviceList.innerHTML = "";
      for (const device of devices) {
        const item = document.createElement("li");
        const text = document.createElement("div");
        text.innerHTML = `<strong>${device.name}</strong><br><code>${device.address}</code>`;
        const pick = document.createElement("button");
        pick.textContent = "Use";
        pick.className = "ghost";
        pick.addEventListener("click", () => {
          addressInput.value = device.address;
          setStatus("ok", `Selected ${device.name} (${device.address}).`);
        });
        item.appendChild(text);
        item.appendChild(pick);
        deviceList.appendChild(item);
      }
    }

    function renderMachines(machines) {
      machineList.innerHTML = "";
      for (const machine of machines) {
        const item = document.createElement("li");
        const text = document.createElement("div");
        const tokenState = machine.ble_auth_token ? "BLE token available" : "No BLE token in response";
        text.innerHTML = `<strong>${machine.name || machine.model_name}</strong><br><code>${machine.serial_number}</code><br>${machine.model_name} · ${tokenState}`;
        const pick = document.createElement("button");
        pick.textContent = "Use token";
        pick.className = "ghost";
        pick.disabled = !machine.ble_auth_token;
        pick.addEventListener("click", () => {
          serialNumberInput.value = machine.serial_number;
          tokenInput.value = machine.ble_auth_token || "";
          setStatus("ok", `Loaded BLE token for ${machine.name || machine.serial_number}.`);
        });
        const choose = document.createElement("button");
        choose.textContent = "Use machine";
        choose.className = "ghost";
        choose.addEventListener("click", () => {
          serialNumberInput.value = machine.serial_number;
          if (machine.ble_auth_token) {
            tokenInput.value = machine.ble_auth_token;
          }
          setStatus("ok", `Selected ${machine.name || machine.serial_number}.`);
        });
        const actions = document.createElement("div");
        actions.className = "button-row";
        actions.appendChild(choose);
        actions.appendChild(pick);
        item.appendChild(text);
        item.appendChild(actions);
        machineList.appendChild(item);
      }
    }

    document.getElementById("cloud-bootstrap-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud bootstrap", async () => {
        const data = await callApi("/api/cloud/bootstrap", currentCloudPayload());
        renderMachines(data.machines || []);
        return data;
      });
    });

    document.getElementById("cloud-status-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud status", async () => {
        return callApi("/api/cloud/status", currentCloudMachinePayload());
      });
    });

    document.getElementById("cloud-power-on-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud power on", async () => {
        return callApi("/api/cloud/command/power", currentCloudMachinePayload({ enabled: true }));
      });
    });

    document.getElementById("cloud-power-off-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud power off", async () => {
        return callApi("/api/cloud/command/power", currentCloudMachinePayload({ enabled: false }));
      });
    });

    document.getElementById("cloud-steam-on-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud steam on", async () => {
        return callApi("/api/cloud/command/steam", currentCloudMachinePayload({ enabled: true }));
      });
    });

    document.getElementById("cloud-steam-off-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud steam off", async () => {
        return callApi("/api/cloud/command/steam", currentCloudMachinePayload({ enabled: false }));
      });
    });

    document.getElementById("cloud-coffee-temp-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Cloud coffee temp", async () => {
        const temperature = Number(document.getElementById("cloud-coffee-temp").value);
        return callApi("/api/cloud/command/coffee-temp", currentCloudMachinePayload({ temperature }));
      });
    });

    document.getElementById("pre-extraction-mode-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Set pre-extraction mode", async () => {
        const mode = document.getElementById("pre-extraction-mode").value;
        return callApi("/api/cloud/command/pre-extraction-mode", currentCloudMachinePayload({ mode }));
      });
    });

    document.getElementById("prebrew-times-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Set prebrew times", async () => {
        const seconds_in = Number(document.getElementById("prebrew-seconds-in").value);
        const seconds_out = Number(document.getElementById("prebrew-seconds-out").value);
        return callApi("/api/cloud/command/prebrew-times", currentCloudMachinePayload({ seconds_in, seconds_out }));
      });
    });

    document.getElementById("scan-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Scan", async () => {
        const data = await callApi("/api/devices", {}, "GET");
        renderDevices(data.devices);
        return data;
      });
    });

    document.getElementById("read-token-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Read token", async () => {
        const data = await callApi("/api/read-token", currentPayload());
        tokenInput.value = data.token;
        return data;
      });
    });

    document.getElementById("status-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Read status", async () => {
        return callApi("/api/status", currentPayload());
      });
    });

    document.getElementById("power-on-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Power on", async () => {
        return callApi("/api/command/power", currentPayload({ enabled: true }));
      });
    });

    document.getElementById("power-off-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Power off", async () => {
        return callApi("/api/command/power", currentPayload({ enabled: false }));
      });
    });

    document.getElementById("steam-on-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Steam on", async () => {
        return callApi("/api/command/steam", currentPayload({ enabled: true }));
      });
    });

    document.getElementById("steam-off-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Steam off", async () => {
        return callApi("/api/command/steam", currentPayload({ enabled: false }));
      });
    });

    document.getElementById("coffee-temp-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Set coffee temp", async () => {
        const temperature = Number(document.getElementById("coffee-temp").value);
        return callApi("/api/command/coffee-temp", currentPayload({ temperature }));
      });
    });

    document.getElementById("steam-level-button").addEventListener("click", async (event) => {
      await run(event.currentTarget, "Set steam level", async () => {
        const temperature = Number(document.getElementById("steam-level").value);
        return callApi("/api/command/steam-temp", currentPayload({ temperature }));
      });
    });

    document.getElementById("clear-button").addEventListener("click", () => {
      output.value = "";
      setStatus("ok", "Output cleared.");
    });
  </script>
</body>
</html>
"""


def json_response(*, data: Any | None = None, error: str | None = None, status: int = 200) -> web.Response:
    """Return a normalized JSON response."""
    payload: dict[str, Any] = {"ok": error is None}
    if data is not None:
        payload.update(data if isinstance(data, dict) else {"data": data})
    if error is not None:
        payload["error"] = error
    return web.json_response(payload, status=status)


async def parse_json(request: web.Request) -> dict[str, Any]:
    """Parse JSON body, defaulting to an empty object."""
    if request.can_read_body:
        try:
            data = await request.json()
        except json.JSONDecodeError as exc:
            raise ValueError(f"Invalid JSON body: {exc}") from exc
        if not isinstance(data, dict):
            raise ValueError("JSON body must be an object.")
        return data
    return {}


async def run_worker(action: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    """Run a BLE action in a subprocess so worker crashes do not kill the server."""
    process = await asyncio.create_subprocess_exec(
        sys.executable,
        str(WORKER_SCRIPT),
        action,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdin_bytes = (
        json.dumps(payload or {}).encode("utf-8") if process.stdin is not None else None
    )
    stdout, stderr = await process.communicate(stdin_bytes)

    stdout_text = stdout.decode("utf-8", errors="replace").strip()
    stderr_text = stderr.decode("utf-8", errors="replace").strip()

    if process.returncode != 0:
        if stdout_text:
            try:
                error_payload = json.loads(stdout_text)
            except json.JSONDecodeError:
                pass
            else:
                raise ValueError(error_payload.get("error", "Bluetooth worker failed."))

        detail = stderr_text or f"Bluetooth worker exited with code {process.returncode}."
        raise ValueError(detail)

    if not stdout_text:
        raise ValueError("Bluetooth worker returned no output.")

    try:
        result = json.loads(stdout_text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Bluetooth worker returned invalid JSON: {exc}") from exc

    if not isinstance(result, dict):
        raise ValueError("Bluetooth worker returned an invalid payload.")

    return result


async def index(_: web.Request) -> web.Response:
    """Serve the single-page UI."""
    return web.Response(text=INDEX_HTML, content_type="text/html")


async def simulator_index(_: web.Request) -> web.Response:
    """Serve the simulator UI."""
    return web.Response(text=INDEX_FILE.read_text(encoding="utf-8"), content_type="text/html")


async def api_cloud_bootstrap(request: web.Request) -> web.Response:
    """Fetch machines and BLE tokens from the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-bootstrap", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_status(request: web.Request) -> web.Response:
    """Read the cloud dashboard for a selected machine."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-status", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_power(request: web.Request) -> web.Response:
    """Set machine power over the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-power", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_steam(request: web.Request) -> web.Response:
    """Set steam over the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-steam", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_coffee_temp(request: web.Request) -> web.Response:
    """Set coffee target temperature over the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-coffee-temp", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_pre_extraction_mode(request: web.Request) -> web.Response:
    """Set pre-extraction mode over the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-pre-extraction-mode", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_cloud_prebrew_times(request: web.Request) -> web.Response:
    """Set prebrew times over the cloud API."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("cloud-prebrew-times", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_devices(_: web.Request) -> web.Response:
    """Return discovered BLE devices."""
    try:
        return json_response(data=await run_worker("scan"))
    except Exception as exc:
        return json_response(error=str(exc), status=500)


async def api_read_token(request: web.Request) -> web.Response:
    """Read the BLE token in pairing mode."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("read-token", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_status(request: web.Request) -> web.Response:
    """Read current machine state over Bluetooth."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("status", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_power(request: web.Request) -> web.Response:
    """Set machine power over Bluetooth."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("power", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_steam(request: web.Request) -> web.Response:
    """Enable or disable steam boiler over Bluetooth."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("steam", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_coffee_temp(request: web.Request) -> web.Response:
    """Set coffee boiler target temperature over Bluetooth."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("coffee-temp", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


async def api_steam_temp(request: web.Request) -> web.Response:
    """Set steam boiler target over Bluetooth."""
    try:
        data = await parse_json(request)
        return json_response(data=await run_worker("steam-temp", data))
    except Exception as exc:
        return json_response(error=str(exc), status=400)


def build_app() -> web.Application:
    """Construct the aiohttp application."""
    app = web.Application()
    app.router.add_get("/", index)
    app.router.add_get("/simulator", simulator_index)
    docs_dir = WORKSPACE_ROOT / "docs"
    if docs_dir.exists():
        app.router.add_static("/docs/", docs_dir, show_index=False)
    app.router.add_post("/api/cloud/bootstrap", api_cloud_bootstrap)
    app.router.add_post("/api/cloud/status", api_cloud_status)
    app.router.add_post("/api/cloud/command/power", api_cloud_power)
    app.router.add_post("/api/cloud/command/steam", api_cloud_steam)
    app.router.add_post("/api/cloud/command/coffee-temp", api_cloud_coffee_temp)
    app.router.add_post("/api/cloud/command/pre-extraction-mode", api_cloud_pre_extraction_mode)
    app.router.add_post("/api/cloud/command/prebrew-times", api_cloud_prebrew_times)
    app.router.add_get("/api/devices", api_devices)
    app.router.add_post("/api/read-token", api_read_token)
    app.router.add_post("/api/status", api_status)
    app.router.add_post("/api/command/power", api_power)
    app.router.add_post("/api/command/steam", api_steam)
    app.router.add_post("/api/command/coffee-temp", api_coffee_temp)
    app.router.add_post("/api/command/steam-temp", api_steam_temp)
    return app


def main() -> None:
    """Run the development server."""
    parser = argparse.ArgumentParser(description="Run the La Marzocco Bluetooth POC app.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()
    web.run_app(build_app(), host=args.host, port=args.port)


if __name__ == "__main__":
    main()
