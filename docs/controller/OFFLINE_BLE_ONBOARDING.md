# Offline BLE Onboarding Flow

This note describes the planned onboarding flow for the controller when the La Marzocco machine has no Wi-Fi access, or when it should be used in Bluetooth-only local mode.

## Goal

- Pair the controller and machine without a cloud dependency
- Read the BLE token directly from the machine
- Store the serial number, BLE address, and token locally on the controller
- Enable local control for `Coffee Boiler`, `Steam Boiler`, and `Standby` immediately afterwards

## Current State

The current firmware can already control the machine over BLE when the controller already has:

- the machine serial number
- the BLE address
- the BLE auth token

Today those values come from the cloud setup path. A real offline-first workflow still needs an initial pairing flow without cloud access.

## Proposed Approach

Add a dedicated **Offline BLE Setup** path to the controller. In that flow, the user puts the machine into pairing mode, the controller scans for a matching La Marzocco machine, reads the BLE token directly, and stores the result locally in NVS.

## UX Flow

### On The Controller

1. The setup screen exposes a `Pair Over Bluetooth` action.
2. The user starts the pairing flow.
3. The controller shows:
   - `Put the machine into Bluetooth pairing mode`
   - `Then press the ring or tap to scan`
4. The controller scans and lists discovered machines.
5. The user selects the target machine.
6. The controller reads the token and machine metadata.
7. On success, the controller shows:
   - `Machine connected`
   - `Local control ready`

### Optional In The Web Portal

The setup portal could later mirror the same flow:

- `Search For Machine`
- a list of discovered devices
- `Connect`

The first implementation step should still work directly on the controller so the onboarding path stays robust even without a browser.

## Technical Flow

### 1. Preconditions

- Controller Wi-Fi is optional and not required
- The machine is powered on
- The machine is in BLE pairing mode
- The machine is in radio range of the controller

### 2. Scan

The controller scans for La Marzocco devices and accepts:

- known advertisement names such as `MICRA`, `MINI`, and `GS3`
- current names such as `LINEAR_*`
- later, optionally, service-UUID based filtering instead of name-only filtering

For each candidate device the controller stores:

- BLE address
- advertised name
- RSSI

### 3. Read The Token In Pairing Mode

After the user selects a device, the controller connects to the machine and reads the pairing or token characteristic.

Persisted data:

- `machine_serial`
- `machine_ble_address`
- `machine_ble_token`
- optionally `machine_name`
- optionally `machine_model`

If the serial number is not available directly during BLE setup, it can remain empty at first and be backfilled later from a normal status read or a cloud fallback.

### 4. Persist The Result

Store the BLE onboarding data in NVS, separate from Wi-Fi and cloud credentials.

Suggested keys:

- `lm.machine.serial`
- `lm.machine.ble_addr`
- `lm.machine.ble_token`
- `lm.machine.name`
- `lm.machine.model`
- `lm.machine.source = ble_local`

### 5. Verify The Pairing

Immediately after storing the result, run a short local validation step:

1. reconnect over BLE
2. authenticate with the saved token
3. read `machineCapabilities`
4. read `machineMode`

Only mark onboarding as successful if that verification works.

## Failure Cases

### No Machine Found

- Show a hint such as `Put the machine into pairing mode`
- Offer a retry

### Multiple Machines Found

- Show a list with name and signal strength
- Let the user select the correct one manually

### Token Read Failed

- Show a hint such as `Pairing mode expired or is not active`
- Offer another scan

### Verification Failed After Saving

- Do not mark the new data as active
- Do not overwrite previously working data if it exists

## Security Model

The controller stores the BLE token locally so later connections can work without the cloud.

Minimum acceptable security for the next implementation step:

- store the token in NVS
- never show the raw token in the web portal
- mask the token in exports and debug logs

Reasonable later improvements:

- protect stored secrets with ESP32 security features
- add a deliberate `Forget Machine` flow

## UI Proposal

### Setup Screen

Add a new card or entry:

- `Pair Over Bluetooth`
- subtitle: `Connect directly to a machine without Wi-Fi`

### Status States

- `No machine paired`
- `Paired with <Name>`
- `Local control ready`

## Implementation Plan

### Step 1

- Add a `Pair Over Bluetooth` setup action
- Add the BLE scan UI
- Show a device list on the controller

### Step 2

- Read the token in pairing mode
- Persist it in NVS

### Step 3

- Verify with `machineCapabilities` and `machineMode`
- Surface success and failure clearly in the UI

### Step 4

- Optionally mirror the same flow in the web portal

## Scope Boundary

This flow is specifically for the **first-time setup without cloud access**.

Not part of this step:

- cloud login
- machine list loading from the cloud
- schedules or other cloud-only features
- automatic migration from older cloud-derived data

## Open Questions

- Which characteristic is the reliable token source across all supported models in pairing mode?
- Does the machine also expose serial number and model directly during pairing?
- Does pairing mode need to be triggered from the official app, or can it be enabled directly on the machine?
- How long does pairing mode remain open?
- How should the flow be started on hardware that has only touch and the outer ring, but no physical push button?

## Recommendation

This offline path should be implemented next when the goal is true cloud-independent operation. It is the cleanest long-term foundation for local BLE control without depending on machine Wi-Fi or La Marzocco cloud availability.
