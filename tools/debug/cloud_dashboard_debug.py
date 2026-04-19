#!/usr/bin/env python3
"""Inspect La Marzocco cloud dashboard widgets for controller integration work."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PYLMARZOCCO_ROOT = REPO_ROOT.parent / "pylamarzocco"
PYLMARZOCCO_ROOT = Path(
    os.environ.get("PYLMARZOCCO_ROOT", str(DEFAULT_PYLMARZOCCO_ROOT))
).resolve()
LOCAL_INSTALLATION_KEY_FILE = Path(__file__).resolve().with_name(
    ".lm_cloud_installation_key.json"
)
ACCESS_TOKEN_CACHE_FILE = Path(__file__).resolve().with_name(
    ".lm_cloud_access_token.json"
)
DEFAULT_WIDGET_CODES = (
    "CMMachineStatus",
    "CMCoffeeBoiler",
    "CMSteamBoilerLevel",
    "CMSteamBoilerTemperature",
)
BOILER_CODES = (
    "CMCoffeeBoiler",
    "CMSteamBoilerLevel",
    "CMSteamBoilerTemperature",
)

if str(PYLMARZOCCO_ROOT) not in sys.path:
    sys.path.insert(0, str(PYLMARZOCCO_ROOT))

try:
    from aiohttp import ClientSession
    from pylamarzocco import LaMarzoccoCloudClient
    from pylamarzocco.exceptions import RequestNotSuccessful
    from pylamarzocco.models import AccessToken
    from pylamarzocco.util import InstallationKey, generate_installation_key
except ModuleNotFoundError as exc:  # pragma: no cover - exercised manually
    raise SystemExit(
        "Could not import pylamarzocco dependencies. "
        "Set PYLMARZOCCO_ROOT to the sibling repo and run this script with the "
        "matching Python environment."
    ) from exc


def enum_value(value: Any) -> Any:
    """Return an enum's stable value without depending on its concrete type."""
    if hasattr(value, "value"):
        return value.value
    return value


def iso_timestamp(value: datetime | None) -> str | None:
    """Format datetimes in UTC for stable log output."""
    if value is None:
        return None
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def epoch_ms(value: datetime | None) -> int | None:
    """Convert datetimes to Unix epoch milliseconds."""
    if value is None:
        return None
    return int(value.timestamp() * 1000)


def remaining_seconds(ready_time: datetime | None) -> int | None:
    """Compute seconds until the reported ready timestamp."""
    if ready_time is None:
        return None
    return int(ready_time.timestamp() - time.time())


def json_dump(data: Any) -> str:
    """Serialize structured output in a readable, stable format."""
    return json.dumps(data, indent=2, sort_keys=True)


def save_json(path: Path, data: Any) -> None:
    """Persist a JSON blob with a trailing newline."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json_dump(data) + "\n", encoding="utf-8")


def require_value(
    cli_value: str | None, env_name: str, label: str, allow_empty: bool = False
) -> str:
    """Resolve one required value from CLI args or environment."""
    value = cli_value if cli_value is not None else os.environ.get(env_name)
    if value is None:
        raise SystemExit(
            f"Missing {label}. Pass --{label.replace('_', '-')} or set {env_name}."
        )
    if not allow_empty and str(value).strip() == "":
        raise SystemExit(f"{label} must not be empty.")
    return str(value)


def installation_key_file_path() -> Path:
    """Resolve the preferred installation key file path."""
    env_path = os.environ.get("LM_CLOUD_INSTALLATION_KEY_FILE")
    if env_path:
        return Path(env_path).expanduser().resolve()
    return LOCAL_INSTALLATION_KEY_FILE


def load_or_create_installation_key() -> tuple[InstallationKey, bool, Path]:
    """Reuse the same installation key across runs to avoid re-registering."""
    key_file = installation_key_file_path()
    if key_file.exists():
        return InstallationKey.from_json(
            key_file.read_text(encoding="utf-8")
        ), False, key_file

    installation_key = generate_installation_key(str(uuid.uuid4()).lower())
    key_file.parent.mkdir(parents=True, exist_ok=True)
    key_file.write_text(installation_key.to_json(), encoding="utf-8")
    return installation_key, True, key_file


def load_cached_access_token(username: str) -> AccessToken | None:
    """Load a previously cached access token for this account."""
    if not ACCESS_TOKEN_CACHE_FILE.exists():
        return None

    try:
        payload = json.loads(ACCESS_TOKEN_CACHE_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None

    if not isinstance(payload, dict) or payload.get("username") != username:
        return None

    token_payload = payload.get("token")
    if not isinstance(token_payload, dict):
        return None

    try:
        token = AccessToken.from_dict(token_payload)
    except Exception:
        return None

    if token.expires_at <= time.time():
        return None
    return token


def save_cached_access_token(username: str, token: AccessToken | None) -> None:
    """Persist the most recently used access token for follow-up commands."""
    if token is None:
        return

    payload = {
        "username": username,
        "token": token.to_dict(),
    }
    ACCESS_TOKEN_CACHE_FILE.write_text(json_dump(payload) + "\n", encoding="utf-8")


@asynccontextmanager
async def cloud_client_context(username: str, password: str):
    """Yield one reusable authenticated cloud client."""
    installation_key, registration_required, _ = load_or_create_installation_key()

    async with ClientSession() as session:
        client = LaMarzoccoCloudClient(
            username=username,
            password=password,
            installation_key=installation_key,
            client=session,
        )
        cached_token = load_cached_access_token(username)
        if cached_token is not None:
            client._access_token = cached_token
        if registration_required:
            await client.async_register_client()
        try:
            yield client, registration_required
        finally:
            save_cached_access_token(username, client._access_token)


async def with_cloud_client(username: str, password: str, handler) -> Any:
    """Create an authenticated cloud client for one operation."""
    async with cloud_client_context(username, password) as (
        client,
        registration_required,
    ):
        return await handler(client, registration_required)


def normalize_value(value: Any) -> Any:
    """Convert dashboard model values to JSON-friendly plain Python data."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, datetime):
        return iso_timestamp(value)
    if isinstance(value, (list, tuple)):
        return [normalize_value(entry) for entry in value]
    if isinstance(value, dict):
        return {str(key): normalize_value(entry) for key, entry in value.items()}
    if hasattr(value, "value"):
        return normalize_value(value.value)
    if hasattr(value, "to_dict"):
        return normalize_value(value.to_dict())
    return str(value)


def summarize_machine(thing: Any) -> dict[str, Any]:
    """Extract the small machine inventory used to pick the serial number."""
    return {
        "name": thing.name,
        "serialNumber": thing.serial_number,
        "modelName": str(thing.model_name),
        "modelCode": str(thing.model_code),
        "connected": bool(thing.connected),
        "offlineMode": bool(thing.offline_mode),
        "bleAuthTokenAvailable": bool(thing.ble_auth_token),
    }


def summarize_next_status(next_status: Any) -> dict[str, Any] | None:
    """Normalize the optional next scheduled state."""
    if next_status is None:
        return None

    start_time = getattr(next_status, "start_time", None)
    return {
        "status": enum_value(getattr(next_status, "status", None)),
        "startTime": epoch_ms(start_time),
        "startTimeIso": iso_timestamp(start_time),
    }


def summarize_widget(widget: Any) -> dict[str, Any]:
    """Normalize one dashboard widget and keep its raw output for later digging."""
    output = getattr(widget, "output", None)
    ready_time = getattr(output, "ready_start_time", None)

    return {
        "code": enum_value(widget.code),
        "output": normalize_value(output),
        "derived": {
            "status": enum_value(getattr(output, "status", None)),
            "mode": enum_value(getattr(output, "mode", None)),
            "readyStartTime": epoch_ms(ready_time),
            "readyStartTimeIso": iso_timestamp(ready_time),
            "remainingSeconds": remaining_seconds(ready_time),
            "targetTemperature": getattr(output, "target_temperature", None),
            "targetLevel": enum_value(getattr(output, "target_level", None)),
            "nextStatus": summarize_next_status(getattr(output, "next_status", None)),
        },
    }


def summarize_machine_signals(dashboard: Any) -> dict[str, Any]:
    """Expose root-level machine reachability fields for offline/recovery diffs."""
    connected = getattr(dashboard, "connected", None)
    offline_mode = getattr(dashboard, "offline_mode", None)
    online = None

    if isinstance(connected, bool):
        online = connected
    if isinstance(offline_mode, bool):
        online = (online if online is not None else True) and not offline_mode

    return {
        "connected": connected if isinstance(connected, bool) else None,
        "offlineMode": offline_mode if isinstance(offline_mode, bool) else None,
        "online": online,
    }


def resolve_selected_codes(args: argparse.Namespace) -> tuple[str, ...] | None:
    """Resolve the widget code filter for snapshot and watch commands."""
    if getattr(args, "all_widgets", False):
        return None
    if args.code:
        return tuple(dict.fromkeys(args.code))
    return DEFAULT_WIDGET_CODES


def matching_widgets(
    dashboard: Any, selected_codes: Iterable[str] | None
) -> list[Any]:
    """Return widgets filtered by the requested exact code list."""
    widgets = list(dashboard.widgets)
    if selected_codes is None:
        return widgets

    wanted = set(selected_codes)
    return [widget for widget in widgets if enum_value(widget.code) in wanted]


def summarize_dashboard(
    dashboard: Any,
    selected_codes: Iterable[str] | None,
) -> dict[str, Any]:
    """Extract selected dashboard widget state plus warmup signal hints."""
    widgets = [
        summarize_widget(widget)
        for widget in matching_widgets(dashboard, selected_codes)
    ]

    heating = any(
        widget["code"] in BOILER_CODES
        and widget["derived"]["status"] == "HeatingUp"
        for widget in widgets
    )
    ready_start_time_present = any(
        widget["code"] in BOILER_CODES
        and widget["derived"]["readyStartTime"] is not None
        for widget in widgets
    )

    if heating and ready_start_time_present:
        assessment = "eta_ready"
    elif heating:
        assessment = "heating_without_eta"
    else:
        assessment = "no_heating_signal"

    return {
        "capturedAt": datetime.now(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        "serialNumber": dashboard.serial_number,
        "selectedCodes": list(selected_codes) if selected_codes is not None else "ALL",
        "selectedWidgetCount": len(widgets),
        "machineSignals": summarize_machine_signals(dashboard),
        "heatSignals": {
            "heating": heating,
            "readyStartTimePresent": ready_start_time_present,
            "etaUsable": heating and ready_start_time_present,
            "assessment": assessment,
        },
        "widgets": widgets,
    }


async def fetch_dashboard(username: str, password: str, serial_number: str) -> Any:
    """Read one dashboard snapshot."""

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> Any:
        return await fetch_dashboard_with_client(client, serial_number)

    return await with_cloud_client(username, password, handler)


async def fetch_dashboard_with_client(
    client: LaMarzoccoCloudClient,
    serial_number: str,
) -> Any:
    """Read one dashboard snapshot with an already-open client."""
    return await client.get_thing_dashboard(serial_number)


async def cmd_bootstrap(args: argparse.Namespace) -> int:
    """List accessible machines for the given account."""
    username = require_value(args.username, "LM_CLOUD_USERNAME", "username")
    password = require_value(args.password, "LM_CLOUD_PASSWORD", "password")

    async def handler(
        client: LaMarzoccoCloudClient,
        registration_required: bool,
    ) -> dict[str, Any]:
        things = await client.list_things()
        return {
            "installationKeyPath": str(installation_key_file_path()),
            "accessTokenCachePath": str(ACCESS_TOKEN_CACHE_FILE),
            "registrationRequired": registration_required,
            "machines": [summarize_machine(thing) for thing in things],
        }

    print(json_dump(await with_cloud_client(username, password, handler)))
    return 0


async def cmd_codes(args: argparse.Namespace) -> int:
    """List dashboard widget codes currently exposed for the selected machine."""
    username = require_value(args.username, "LM_CLOUD_USERNAME", "username")
    password = require_value(args.password, "LM_CLOUD_PASSWORD", "password")
    serial_number = require_value(
        args.serial_number, "LM_CLOUD_SERIAL", "serial_number"
    )

    dashboard = await fetch_dashboard(username, password, serial_number)
    codes = sorted({enum_value(widget.code) for widget in dashboard.widgets})
    print(
        json_dump(
            {
                "capturedAt": datetime.now(timezone.utc)
                .isoformat()
                .replace("+00:00", "Z"),
                "serialNumber": serial_number,
                "widgetCodeCount": len(codes),
                "widgetCodes": codes,
            }
        )
    )
    return 0


async def cmd_snapshot(args: argparse.Namespace) -> int:
    """Fetch one dashboard snapshot and optionally persist it."""
    username = require_value(args.username, "LM_CLOUD_USERNAME", "username")
    password = require_value(args.password, "LM_CLOUD_PASSWORD", "password")
    serial_number = require_value(
        args.serial_number, "LM_CLOUD_SERIAL", "serial_number"
    )

    dashboard = await fetch_dashboard(username, password, serial_number)
    summary = summarize_dashboard(dashboard, resolve_selected_codes(args))
    print(json_dump(summary))

    if args.output is not None:
        bundle = {
            "summary": summary,
            "dashboard": normalize_value(dashboard),
        }
        save_json(Path(args.output).expanduser(), bundle)

    return 0


async def cmd_power(args: argparse.Namespace) -> int:
    """Switch the machine to brewing mode or standby through the cloud API."""
    username = require_value(args.username, "LM_CLOUD_USERNAME", "username")
    password = require_value(args.password, "LM_CLOUD_PASSWORD", "password")
    serial_number = require_value(
        args.serial_number, "LM_CLOUD_SERIAL", "serial_number"
    )
    enabled = not args.off

    async def handler(client: LaMarzoccoCloudClient, _: bool) -> dict[str, Any]:
        result = await client.set_power(serial_number=serial_number, enabled=enabled)
        return {
            "serialNumber": serial_number,
            "enabled": enabled,
            "result": result,
        }

    print(json_dump(await with_cloud_client(username, password, handler)))
    return 0


async def cmd_watch(args: argparse.Namespace) -> int:
    """Poll the dashboard until interrupted or the requested limit is reached."""
    username = require_value(args.username, "LM_CLOUD_USERNAME", "username")
    password = require_value(args.password, "LM_CLOUD_PASSWORD", "password")
    serial_number = require_value(
        args.serial_number, "LM_CLOUD_SERIAL", "serial_number"
    )

    if args.interval <= 0:
        raise SystemExit("--interval must be greater than 0.")
    if args.count is not None and args.count <= 0:
        raise SystemExit("--count must be greater than 0 when provided.")

    selected_codes = resolve_selected_codes(args)
    poll_index = 0
    try:
        async with cloud_client_context(username, password) as (client, _):
            if args.power_on:
                power_result = await client.set_power(
                    serial_number=serial_number, enabled=True
                )
                print(
                    json_dump(
                        {
                            "event": "powerOn",
                            "serialNumber": serial_number,
                            "result": power_result,
                        }
                    )
                )
                print("")

            while args.count is None or poll_index < args.count:
                if poll_index > 0:
                    await asyncio.sleep(args.interval)

                dashboard = await fetch_dashboard_with_client(client, serial_number)
                summary = summarize_dashboard(dashboard, selected_codes)
                summary["pollIndex"] = poll_index + 1
                print(json_dump(summary))
                print("")

                if args.stop_when_ready and summary["heatSignals"]["etaUsable"]:
                    break

                poll_index += 1
    except KeyboardInterrupt:  # pragma: no cover - manual stop path
        return 130

    return 0


def build_parser() -> argparse.ArgumentParser:
    """Construct the CLI interface."""
    parser = argparse.ArgumentParser(
        description="Inspect La Marzocco cloud dashboard widgets and keep raw outputs for later analysis."
    )
    parser.add_argument(
        "--username",
        help="La Marzocco account email. Falls back to LM_CLOUD_USERNAME.",
    )
    parser.add_argument(
        "--password",
        help="La Marzocco account password. Falls back to LM_CLOUD_PASSWORD.",
    )
    parser.add_argument(
        "--serial-number",
        help="Machine serial number. Falls back to LM_CLOUD_SERIAL when needed.",
    )
    parser.add_argument(
        "--code",
        action="append",
        help="Exact widget code to include. Repeatable. Default: warmup-related codes.",
    )
    parser.add_argument(
        "--all-widgets",
        action="store_true",
        help="Include every dashboard widget instead of a filtered subset.",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser(
        "bootstrap",
        help="List machines accessible to the configured account.",
    ).set_defaults(handler=cmd_bootstrap)

    subparsers.add_parser(
        "codes",
        help="List all dashboard widget codes exposed by the selected machine right now.",
    ).set_defaults(handler=cmd_codes)

    snapshot = subparsers.add_parser(
        "snapshot",
        help="Fetch one dashboard snapshot for the selected widget codes.",
    )
    snapshot.add_argument(
        "--output",
        help="Optional path for the full captured bundle (summary plus normalized dashboard data).",
    )
    snapshot.set_defaults(handler=cmd_snapshot)

    power = subparsers.add_parser(
        "power",
        help="Switch the machine power state through the cloud API.",
    )
    power.add_argument(
        "--off",
        action="store_true",
        help="Send StandBy instead of BrewingMode.",
    )
    power.set_defaults(handler=cmd_power)

    watch = subparsers.add_parser(
        "watch",
        help="Poll the dashboard repeatedly for the selected widget codes.",
    )
    watch.add_argument(
        "--interval",
        type=float,
        default=10.0,
        help="Seconds between dashboard polls. Default: 10.",
    )
    watch.add_argument(
        "--count",
        type=int,
        help="Optional maximum number of polls before exiting.",
    )
    watch.add_argument(
        "--power-on",
        action="store_true",
        help="Send a cloud power-on command in the same authenticated session before polling.",
    )
    watch.add_argument(
        "--stop-when-ready",
        action="store_true",
        help="Exit as soon as warmup-related widgets expose HeatingUp and readyStartTime.",
    )
    watch.set_defaults(handler=cmd_watch)

    return parser


async def main_async(argv: list[str] | None = None) -> int:
    """Parse CLI arguments and run the selected command."""
    args = build_parser().parse_args(argv)
    return await args.handler(args)


def main(argv: list[str] | None = None) -> int:
    """CLI entrypoint."""
    try:
        return asyncio.run(main_async(argv))
    except RequestNotSuccessful as exc:
        message = str(exc)
        if "status code 403" in message:
            print(
                "Cloud auth was blocked with HTTP 403. "
                "This often happens after several quick sign-ins. "
                "Wait a minute and retry with a single-session run such as "
                "`watch --power-on`.",
                file=sys.stderr,
            )
            print(message, file=sys.stderr)
            return 1
        raise


if __name__ == "__main__":
    raise SystemExit(main())
