# aliensense,nxs-csi2 — the NXS camera face node

The `nxs-csi2` driver serves a sensor head behind an NXS pod. The pod's unit
owns the sensor: it runs the camera personality that brings the head up, and
it applies the gain, the exposure and the frame length the capture stack asks
for, each frame, under the sensor's register hold. The kernel never addresses
the sensor. It talks to the unit's camera face, a flat register map at the
pod's second I²C address, through the control block and `COMMIT` the
host-interface specification's chapter "Camera face" defines (the `nxs`
documentation set, `nxs-host-interface.md`).

## Node

```
nxs_csi2_cam1_a: nxs_csi2_cam1@33 {
    compatible = "aliensense,nxs-csi2";
    reg = <0x33>;
    ...
    mode0 { aliensense_sensors_pool = "framos,imx900;"; ... };
    mode1 { aliensense_sensors_pool = "framos,imx335;"; ... };
};
```

## Properties

The node's `reg` is the camera face's host address: the pod straps `0x31`,
and behind a hub the link's serializer presents it one above the pod's
register map alias (link A `0x33`, link B `0x35`). Each `modeN` child
carries the standard tegracam properties plus `aliensense_sensors_pool`,
as the `aliensense,universal` node does. There is no register width and no
control table: the unit holds the sensor's laws.

The host tool generates the node (`nxs host overlay`) from the personality
the pod carries. The modes are the personality's capture table, the same
rows the universal node would carry for that sensor.

## V4L2 controls

The standard tegracam controls act through the face:

| Control | Face field | What the unit does |
|---|---|---|
| `gain` | `SET_GAIN` | the control value, in the mode's `gain_factor` units, scaled to the sensor's gain register by the personality |
| `exposure` | `SET_EXPOSURE` | lines, from the control's time at the face's `LINE_TIME_NS`; clamped to the face's range |
| `frame_rate` | `SET_FRAME_LENGTH` | lines per frame, from the control's rate at the line time; clamped to the face's range |
| `group_hold` | `COMMIT` | the fields staged inside a hold commit together when it closes; outside a hold each control commits on its own |
| `sensor_mode_id` | — | the mode's rows; a mode switch is the host tool's bring-up, not the face's |

The driver adds the four private controls the FRAMOS Jetson drivers name:

| Control | Values | Effect |
|---|---|---|
| `shutter_mode` | 0 normal, 1 sequential trigger, 2 fast trigger | writes `SET_TRIGGER` with the personality's value for that preset and commits; the unit runs its trigger conversion. A preset the personality lacks is refused with `EINVAL` |
| `test_pattern` | 0 off, N pattern code | writes `SET_TEST_PATTERN` and commits |
| `operation_mode` | 0 master, 1 slave | recorded only: the personality's timing program sets the sync role |
| `black_level` | integer | recorded only: the personality's tables set the black level |

## The commit

One write transaction stages the control block, `SET_EXPOSURE` through
`SET_MASK`, and ends on `COMMIT`. The unit answers `BUSY` in `STATE` while it
applies, and it does not acknowledge a transaction that lands inside the
apply: the driver treats that NAK as busy and repeats the transaction after
1 ms, eight times at most. One commit per frame is the budget.

The capability block (`CAP_VERSION` through `FACE_FLAGS`) is read at probe,
or at the first capture session when the pod is not reachable yet (the hub's
link comes up after boot, under the host tool). A face whose identity or
capability layout the driver does not know refuses the probe. The live
values (`LINE_TIME_NS`, `FRAME_LENGTH`) are read at each session's `set_mode`
and are the numbers the exposure and frame-rate laws divide by; a face still
`INERT` (no personality run yet) leaves the controls inert until the next
session.
