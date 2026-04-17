"""Bluetooth worker for the La Marzocco POC web app."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path
from typing import Any

from aiohttp import ClientSession
from bleak import BleakScanner
import uuid

WORKSPACE_ROOT = Path(__file__).resolve().parent
PYLMARZOCCO_ROOT = WORKSPACE_ROOT.parent / "pylamarzocco"
INSTALLATION_KEY_FILE = WORKSPACE_ROOT / ".lm_installation_key.json"

if str(PYLMARZOCCO_ROOT) not in sys.path:
    sys.path.insert(0, str(PYLMARZOCCO_ROOT))

from pylamarzocco import LaMarzoccoBluetoothClient, LaMarzoccoCloudClient  # noqa: E402
from pylamarzocco.const import BoilerType, PreExtractionMode  # noqa: E402
from pylamarzocco.models import PrebrewSettingTimes, SecondsInOut  # noqa: E402
from pylamarzocco.util import InstallationKey, generate_installation_key  # noqa: E402


def serialize(value: Any) -> Any:
    """Convert SDK models into plain data."""
    if hasattr(value, "to_dict"):
        return value.to_dict()
    if isinstance(value, list):
        return [serialize(item) for item in value]
    return value


def parse_bool(value: Any, *, field: str) -> bool:
    """Parse a boolean payload field."""
    if isinstance(value, bool):
        return value
    raise ValueError(f"{field} must be a boolean.")


def require_token(payload: dict[str, Any]) -> str:
    """Read and validate the BLE token."""
    token = str(payload.get("token", "")).strip()
    if not token:
        raise ValueError("BLE token is required for this action.")
    return token


def require_temperature(payload: dict[str, Any]) -> float:
    """Read and validate a temperature value."""
    if "temperature" not in payload:
        raise ValueError("temperature is required.")
    return float(payload["temperature"])


def require_credentials(payload: dict[str, Any]) -> tuple[str, str]:
    """Read and validate cloud credentials."""
    username = str(payload.get("username", "")).strip()
    password = str(payload.get("password", ""))
    if not username:
        raise ValueError("username is required.")
    if not password:
        raise ValueError("password is required.")
    return username, password


def require_serial_number(payload: dict[str, Any]) -> str:
    """Read and validate the machine serial number."""
    serial_number = str(payload.get("serial_number", "")).strip()
    if not serial_number:
        raise ValueError("serial_number is required.")
    return serial_number


def load_or_create_installation_key() -> tuple[InstallationKey, bool]:
    """Load or create the installation key material."""
    if INSTALLATION_KEY_FILE.exists():
        return InstallationKey.from_json(INSTALLATION_KEY_FILE.read_text(encoding="utf-8")), False

    installation_key = generate_installation_key(str(uuid.uuid4()).lower())
    INSTALLATION_KEY_FILE.write_text(installation_key.to_json(), encoding="utf-8")
    return installation_key, True


def parse_pre_extraction_mode(payload: dict[str, Any]) -> PreExtractionMode:
    """Read and validate the pre-extraction mode."""
    raw_mode = str(payload.get("mode", "")).strip()
    if not raw_mode:
        raise ValueError("mode is required.")
    normalized = raw_mode.lower()
    mapping = {
        "disabled": PreExtractionMode.DISABLED,
        "prebrewing": PreExtractionMode.PREBREWING,
        "prebrew": PreExtractionMode.PREBREWING,
        "preinfusion": PreExtractionMode.PREINFUSION,
    }
    if normalized not in mapping:
        raise ValueError("mode must be one of: disabled, prebrew, preinfusion.")
    return mapping[normalized]


def require_seconds(payload: dict[str, Any], key: str) -> float:
    """Read and validate a float seconds field."""
    if key not in payload:
        raise ValueError(f"{key} is required.")
    return float(payload[key])


async def with_cloud_client(payload: dict[str, Any], handler) -> Any:
    """Create a cloud client, run the handler, and close the session."""
    username, password = require_credentials(payload)
    installation_key, registration_required = load_or_create_installation_key()

    async with ClientSession() as session:
        client = LaMarzoccoCloudClient(
            username=username,
            password=password,
            installation_key=installation_key,
            client=session,
        )
        if registration_required:
            await client.async_register_client()
        return await handler(client, registration_required)


async def discover_devices() -> list[Any]:
    """Discover nearby La Marzocco BLE devices."""
    return await LaMarzoccoBluetoothClient.discover_devices()


async def resolve_device(address: str | None) -> Any:
    """Resolve a BLE device from address or discovery."""
    normalized = (address or "").strip()

    if normalized:
        device = await BleakScanner.find_device_by_address(normalized, timeout=10.0)
        if device is None:
            raise ValueError(f"No BLE device found for address {normalized!r}.")
        return device

    devices = await discover_devices()
    if not devices:
        raise ValueError("No La Marzocco Bluetooth device found nearby.")
    if len(devices) > 1:
        raise ValueError("Multiple devices found. Pick one address first.")
    return devices[0]


async def with_client(payload: dict[str, Any], handler) -> Any:
    """Create a Bluetooth client, run the handler, and disconnect."""
    device = await resolve_device(payload.get("address"))
    token = require_token(payload)
    client = LaMarzoccoBluetoothClient(device, token)
    try:
        return await handler(client, device)
    finally:
        await client.disconnect()


async def do_scan(_: dict[str, Any]) -> dict[str, Any]:
    """Scan for nearby La Marzocco BLE devices."""
    devices = await discover_devices()
    return {
        "devices": [
            {"name": device.name or "Unknown", "address": device.address}
            for device in devices
        ]
    }


async def do_cloud_bootstrap(payload: dict[str, Any]) -> dict[str, Any]:
    """Authenticate against cloud and return machines plus BLE tokens."""
    async def handler(client: LaMarzoccoCloudClient, registration_required: bool) -> dict[str, Any]:
        things = await client.list_things()
        return {
            "installation_key_path": str(INSTALLATION_KEY_FILE),
            "registration_required": registration_required,
            "machines": [
                {
                    "name": thing.name,
                    "serial_number": thing.serial_number,
                    "model_name": str(thing.model_name),
                    "model_code": str(thing.model_code),
                    "connected": thing.connected,
                    "offline_mode": thing.offline_mode,
                    "ble_auth_token": thing.ble_auth_token,
                }
                for thing in things
            ],
        }

    return await with_cloud_client(payload, handler)


async def do_cloud_status(payload: dict[str, Any]) -> dict[str, Any]:
    """Read dashboard over the cloud API."""
    serial_number = require_serial_number(payload)

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        dashboard = await client.get_thing_dashboard(serial_number)
        return {
            "serial_number": serial_number,
            "dashboard": serialize(dashboard),
        }

    return await with_cloud_client(payload, handler)


async def do_cloud_power(payload: dict[str, Any]) -> dict[str, Any]:
    """Set power over the cloud API."""
    serial_number = require_serial_number(payload)
    enabled = parse_bool(payload.get("enabled"), field="enabled")

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.set_power(serial_number=serial_number, enabled=enabled)
        return {"serial_number": serial_number, "result": result}

    return await with_cloud_client(payload, handler)


async def do_cloud_steam(payload: dict[str, Any]) -> dict[str, Any]:
    """Set steam over the cloud API."""
    serial_number = require_serial_number(payload)
    enabled = parse_bool(payload.get("enabled"), field="enabled")

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.set_steam(serial_number=serial_number, enabled=enabled)
        return {"serial_number": serial_number, "result": result}

    return await with_cloud_client(payload, handler)


async def do_cloud_coffee_temp(payload: dict[str, Any]) -> dict[str, Any]:
    """Set coffee target temperature over the cloud API."""
    serial_number = require_serial_number(payload)
    temperature = require_temperature(payload)

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.set_coffee_target_temperature(
            serial_number=serial_number,
            target_temperature=temperature,
        )
        return {
            "serial_number": serial_number,
            "target_temperature": round(temperature, 1),
            "result": result,
        }

    return await with_cloud_client(payload, handler)


async def do_cloud_pre_extraction_mode(payload: dict[str, Any]) -> dict[str, Any]:
    """Set pre-extraction mode over the cloud API."""
    serial_number = require_serial_number(payload)
    mode = parse_pre_extraction_mode(payload)

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.change_pre_extraction_mode(
            serial_number=serial_number,
            prebrew_mode=mode,
        )
        return {"serial_number": serial_number, "mode": mode.value, "result": result}

    return await with_cloud_client(payload, handler)


async def do_cloud_prebrew_times(payload: dict[str, Any]) -> dict[str, Any]:
    """Set prebrew in/out times over the cloud API."""
    serial_number = require_serial_number(payload)
    seconds_in = require_seconds(payload, "seconds_in")
    seconds_out = require_seconds(payload, "seconds_out")
    settings = PrebrewSettingTimes(
        times=SecondsInOut(seconds_in=seconds_in, seconds_out=seconds_out)
    )

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.change_pre_extraction_times(
            serial_number=serial_number,
            times=settings,
        )
        return {
            "serial_number": serial_number,
            "seconds_in": round(seconds_in, 1),
            "seconds_out": round(seconds_out, 1),
            "result": result,
        }

    return await with_cloud_client(payload, handler)


async def do_read_token(payload: dict[str, Any]) -> dict[str, Any]:
    """Read the BLE token in pairing mode."""
    device = await resolve_device(payload.get("address"))
    token = await LaMarzoccoBluetoothClient.read_token(device)
    return {
        "name": device.name or "Unknown",
        "address": device.address,
        "token": token,
    }


async def do_status(payload: dict[str, Any]) -> dict[str, Any]:
    """Read live machine state."""

    async def handler(client: LaMarzoccoBluetoothClient, device: Any) -> dict[str, Any]:
        capabilities = await client.get_machine_capabilities()
        mode = await client.get_machine_mode()
        boilers = await client.get_boilers()
        tank_status = await client.get_tank_status()
        return {
            "name": device.name or "Unknown",
            "address": device.address,
            "capabilities": serialize(capabilities),
            "machine_mode": serialize(mode),
            "boilers": serialize(boilers),
            "tank_status": tank_status,
        }

    return await with_client(payload, handler)


async def do_power(payload: dict[str, Any]) -> dict[str, Any]:
    """Toggle machine power."""
    enabled = parse_bool(payload.get("enabled"), field="enabled")

    async def handler(client: LaMarzoccoBluetoothClient, _: Any) -> dict[str, Any]:
        return {"result": serialize(await client.set_power(enabled))}

    return await with_client(payload, handler)


async def do_steam(payload: dict[str, Any]) -> dict[str, Any]:
    """Toggle steam boiler."""
    enabled = parse_bool(payload.get("enabled"), field="enabled")

    async def handler(client: LaMarzoccoBluetoothClient, _: Any) -> dict[str, Any]:
        return {"result": serialize(await client.set_steam(enabled))}

    return await with_client(payload, handler)


async def do_coffee_temp(payload: dict[str, Any]) -> dict[str, Any]:
    """Set coffee target temperature."""
    temperature = require_temperature(payload)

    async def handler(client: LaMarzoccoBluetoothClient, _: Any) -> dict[str, Any]:
        return {
            "result": serialize(await client.set_temp(BoilerType.COFFEE, temperature))
        }

    return await with_client(payload, handler)


async def do_steam_temp(payload: dict[str, Any]) -> dict[str, Any]:
    """Set steam target temperature or Mini R level preset."""
    temperature = require_temperature(payload)

    async def handler(client: LaMarzoccoBluetoothClient, _: Any) -> dict[str, Any]:
        return {
            "result": serialize(await client.set_temp(BoilerType.STEAM, temperature))
        }

    return await with_client(payload, handler)


ACTIONS = {
    "cloud-bootstrap": do_cloud_bootstrap,
    "cloud-status": do_cloud_status,
    "cloud-power": do_cloud_power,
    "cloud-steam": do_cloud_steam,
    "cloud-coffee-temp": do_cloud_coffee_temp,
    "cloud-pre-extraction-mode": do_cloud_pre_extraction_mode,
    "cloud-prebrew-times": do_cloud_prebrew_times,
    "scan": do_scan,
    "read-token": do_read_token,
    "status": do_status,
    "power": do_power,
    "steam": do_steam,
    "coffee-temp": do_coffee_temp,
    "steam-temp": do_steam_temp,
}


def parse_payload() -> dict[str, Any]:
    """Read the JSON payload from stdin."""
    raw = sys.stdin.read().strip()
    if not raw:
        return {}
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise ValueError("Worker payload must be a JSON object.")
    return data


async def run(action: str) -> dict[str, Any]:
    """Run the requested worker action."""
    payload = parse_payload()
    return await ACTIONS[action](payload)


def main() -> int:
    """CLI entrypoint."""
    parser = argparse.ArgumentParser(description="La Marzocco Bluetooth worker")
    parser.add_argument("action", choices=sorted(ACTIONS))
    args = parser.parse_args()

    try:
        result = asyncio.run(run(args.action))
    except Exception as exc:
        print(json.dumps({"error": str(exc)}))
        return 1

    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
