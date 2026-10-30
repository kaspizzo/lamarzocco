# BOOKOO Mini / Ultra bridge beta

Status: implemented from the public protocol and covered by synthetic host
tests; **no BOOKOO hardware test yet**. The controller can now connect to BOOKOO,
forward weight/battery as its virtual Acaia scale, translate tare/timer commands,
and use the existing machine registration/reconnection path. This is a testable
implementation, not confirmed device compatibility or weight-stop accuracy.

## Setup

1. Build the ESP32 controller firmware using the project's normal build/flash
   workflow for your board. Keep a copy of your working firmware for comparison.
2. Put the scale in manual timing mode, displaying grams. On Ultra use
   timing-mode or a manual timing submode; avoid automatic brewing modes and
   charging for this test. Close other apps connected to the scale.
3. In the local portal's **Scale Adapter**, choose **BOOKOO Mini (experimental)**
   or **BOOKOO Ultra (experimental)**. Enable the adapter with its source address
   empty and save, then reload after scanning.
4. Select the scale's exact BLE address and public/random address type, confirm
   manual mode, and save. If the name does not appear in the scan list, enter its
   address/type manually. The advertised name alone cannot distinguish models.
5. Leave the virtual name (normally `LUNAR-C`) and controller position stable.
   Check fresh weight/battery in the portal and the scale icon on the controller.
   The machine should connect to the virtual scale through the existing bridge
   path. Missing registrations are retried when the normal policy allows it;
   another scale's registration is not overwritten.

## What to test and report

- Model, scale firmware, selected scale mode, controller firmware revision/build,
  machine model and firmware.
- Source connection: compare an empty platform, a known object and a negative
  value after removing a tared object with the portal weight and scale display.
- Machine connection: record portal status, source/machine session counts and
  registration attempts. The header icon is dimmed for a source connection;
  full brightness requires fresh measurements and the machine scale link.
- Tare before flow: with a cup on the scale, check that a tare requested through
  the machine makes the physical display return near zero.
- Timer: check physical reset/start/stop, including starting again after a stop.
  The bridge resets before starting. Record any command timeout and whether the
  physical timer actually changed.
- Reconnect: power the scale off/on and record whether fresh data and the
  machine link return. Repeat after controller and machine restarts. Distinguish
  a live connection from persisted machine location calibration.
- Finally try a supervised water-only weight-stop test. Record target weight,
  final scale weight, timing and whether the machine stops. Be ready to stop the
  test manually; automatic stop accuracy has not been verified.

Useful USB log lines have tag `lm_scale`: source connection/weight,
`Source control completion=... (observed state, experimental)`, machine
subscription and any protocol/control timeout. Control numbers are tare=0,
start=1, stop=2 and reset=3. Include the last relevant lines around a failure.
After a protocol/control fault, correct the setup and save the adapter settings
again to retry.

## Completion detection and known limits

BOOKOO does not document a dedicated command ACK in its public protocol. The
driver waits for new checksum-valid live measurements after a queued write:

| Command | Observation used for completion |
| --- | --- |
| Tare | Weight within ±0.10 g of zero |
| Reset | Timer equals zero |
| Start | Timer advances from its pre-command value, allowing 24-bit wrap |
| Stop | Timer stays unchanged across separate notifications for ≥200 ms |

These are state observations, not proof that a specific write executed.
Already-satisfied states and timer quantization can affect detection. Tare while
water is arriving may fail the zero threshold. Controls time out after two
seconds; source readings older than one second invalidate the link. A missing
completion stops the adapter instead of acknowledging a write blindly.

The Ultra's display can use ounces, but its documented payload still carries
grams. Unknown weight signs or malformed packets stop the adapter. Ultra
automatic-mode/powder-weight packets do not count as live readings or ACKs.
No scale calibration, automatic mode selection, shutdown or firmware-dependent
keepalive commands are sent. The source scale may still go to sleep under its
own automatic shutdown setting.

Sources: [Mini protocol](https://github.com/BooKooCode/OpenSource/blob/main/bookoo_mini_scale/protocols.md),
[Ultra protocol](https://github.com/BooKooCode/OpenSource/blob/main/bookoo_ultra_scale/protocols.md).
