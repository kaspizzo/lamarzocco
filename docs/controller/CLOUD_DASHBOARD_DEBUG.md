# Cloud Dashboard Debug

This helper keeps a reusable path for inspecting raw La Marzocco cloud
dashboard widgets from the desktop side.

It started as a warmup/ETA inspector, but it is meant to stay useful for later
controller work such as:

- warmup ETA fields
- shot-timer hunting
- widget-code discovery
- checking raw widget outputs before adding controller parsing

By default it focuses on the warmup-relevant widgets, but it can also list all
widget codes and dump any selected widget's raw output.

## Requirements

- sibling repo at `/Users/kaspar/Documents/Projekte/pylamarzocco`
- Python virtualenv at `/Users/kaspar/Documents/Projekte/pylamarzocco/.venv312`
- direct La Marzocco cloud login with email and password

The launcher script resolves that environment automatically:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh --help
```

## Suggested flow

Set credentials once for the shell:

```bash
export LM_CLOUD_USERNAME="you@example.com"
export LM_CLOUD_PASSWORD="your-password"
```

List accessible machines and pick the correct serial number:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh bootstrap
```

Store the chosen serial number for the next commands:

```bash
export LM_CLOUD_SERIAL="MI123456"
```

Capture one baseline snapshot while the machine is still in standby:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh snapshot --output /tmp/lm-dashboard-standby.json
```

Switch the machine on through the cloud API:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh power
```

Poll the dashboard during warmup:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh watch --interval 10
```

Stop automatically once a usable ETA appears:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh watch --interval 10 --stop-when-ready
```

Recommended for real testing: avoid several separate commands in quick
succession. The La Marzocco auth endpoint can start returning CloudFront `403`
responses after too many rapid sign-ins. This single-session run is more robust:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh watch --serial-number "$LM_CLOUD_SERIAL" --power-on --interval 5 --count 60
```

## Widget discovery

List every widget code currently present on the selected machine:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh codes
```

Dump one specific widget's raw output:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh --code CMMachineStatus snapshot
```

Dump everything in one shot:

```bash
sh ./tools/debug/run_cloud_dashboard_debug.sh --all-widgets snapshot --output /tmp/lm-dashboard-full.json
```

For something like shot-timer hunting, the usual flow is:

1. `codes`
2. pick suspicious widget codes
3. `snapshot --code ... --output ...`
4. compare standby, heating, brewing, and post-shot snapshots

Current firmware expectation for shot detection:

- `status == "Brewing"` starts a shot
- `brewingStartTime > 0` starts a shot
- `mode == "BrewingMode"` by itself is not enough

So when validating raw dashboard payloads, always compare those fields together instead of treating `BrewingMode` alone as a reliable live-shot signal.

## Output interpretation

Each snapshot includes:

- `selectedCodes`
- `selectedWidgetCount`
- `machineSignals.connected`
- `machineSignals.offlineMode`
- `machineSignals.online`
- `widgets[].output` with the raw normalized widget output
- `widgets[].derived` with a few convenience fields
- `heatSignals.*` when the selected widgets include boiler warmup data

Expected interpretations:

- `heatSignals.assessment == "eta_ready"`
  The cloud already provides both `HeatingUp` and `readyStartTime`.
- `heatSignals.assessment == "heating_without_eta"`
  The cloud reports active heating but not yet a usable ready timestamp.
- `heatSignals.assessment == "no_heating_signal"`
  The tested dashboard snapshot does not currently expose a warmup signal.

Current controller UI semantics map these reachability signals like this:

- solid Wi-Fi icon: the selected machine is reachable through cloud
- crossed Wi-Fi icon: account/network context exists, but there is no live cloud path to the selected machine yet

The default widget subset is still:

- `CMMachineStatus`
- `CMCoffeeBoiler`
- `CMSteamBoilerLevel`
- `CMSteamBoilerTemperature`

`snapshot --output ...` writes a JSON bundle containing the summarized output
plus normalized full-dashboard data for deeper inspection. For connectivity
reverse-engineering, compare `machineSignals.*` across normal, disconnected,
and recovered snapshots before changing controller UI semantics.
