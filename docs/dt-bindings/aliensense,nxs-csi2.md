# aliensense,nxs-csi2 — the NXS camera map node

The `nxs-csi2-tegra` driver serves a camera that speaks the NXS map: a sensor
head behind an NXS pod, whose unit runs the camera personality and applies
every request under the sensor's register hold, or a smart camera that
serves the map from its own logic. The kernel never addresses a sensor. It
reads the map at the camera door, a flat register map behind a 16-bit
register pointer, and commits the parameter rows in their units through
`MASK` and `COMMIT`, as the NXS host-interface specification's register map
and procedures chapters define (the `nxs` documentation set,
`nxs-host-interface.md`). The map client is `nxs-csi2-core`, the module the
driver shares with the mainline `nxs-csi2` driver; the constants are
`nxs-csi2-map.h`, generated from the map's one source.

## Node

```
nxs_csi2_cam1_a: nxs_csi2_cam1@33 {
    compatible = "aliensense,nxs-csi2";
    reg = <0x33>;
    ...
    mode0 { ... };
    mode1 { ... };
};
```

## Properties

The node's `reg` is the camera door's host address: `0x31` on a port the
device shares with the host, and behind a hub the alias the link's
serializer maps it to, one above the pod's unit door (link A `0x33`, link B
`0x35`). Each `modeN` child carries the standard tegracam properties. The
driver reads `active_w`, `active_h`, `max_framerate` and `framerate_factor`
of every mode node at probe and builds its frame-format table from them,
since the map answers only once the hub's link is up. There is no register
width and no control table: the device holds the sensor's laws.

| Property | Type | Meaning |
|---|---|---|
| `aliensense,passive` | boolean | The stream reaches this host while another host owns the hub's control bus. The driver never transfers on I²C, builds its formats from the mode nodes, creates no row control, and a session changes nothing on the device |

The host tool generates the node (`nxs switch`) from the personality the
device carries. The mode nodes are the personality's mode table, and the
map serves the same table: at the first session the map answers, the
driver checks every mode node against the map's rows by width, height and
bit depth, and refuses the session with a log line naming the mode when
the tree names one the map does not serve. The generator writes a passive
node for a port whose hub another host owns, with `aliensense,passive`,
the same mode nodes and the same port wiring as an active node, and no
other difference.

## V4L2 controls

The standard tegracam controls act through the map's rows:

| Control | Row | What the driver commits |
|---|---|---|
| `gain` | `GAIN_MDB` | the control value in the mode's `gain_factor` units, as millidecibels |
| `exposure` | `EXPOSURE_US` | the control value in the mode's `exposure_factor` units, as microseconds. Under the fast trigger a row the pulse width sets takes no value |
| `frame_rate` | `FRAME_PERIOD_US` | the period of the control's rate at the mode's `framerate_factor`, as microseconds. Under a trigger the pulse period is the frame period, and the rate is left alone |
| `group_hold` | `COMMIT` | the rows staged inside a hold commit together when it closes; outside a hold each control commits on its own |
| `sensor_mode_id` | `MODE` | the map's row of the mode node's width, height and bit depth, run in standby when the map runs another |

The core adds a control for each row the framework does not map, with one
id on every host (`V4L2_CID_USER_BASE + 0x2000` upward):

| Control | Row | Values |
|---|---|---|
| `Trigger Mode` (`0x00982900`) | `TRIGGER` | a menu of the row's trigger codes: Free Run 0, Fast Trigger 1, Sequential Trigger 2, a code the row lacks skipped. It reads the conversion in effect back, so one the host tool made shows |
| `Test Pattern` (`V4L2_CID_TEST_PATTERN`) | `TEST_PATTERN` | a menu of the row's codes, Disabled first |
| `Black Level` (`0x00982901`) | `BLACK_LEVEL_DN` | an integer at the row's range |
| `Temperature` (`0x00982902`) | `TEMPERATURE_DC` | a volatile read-only integer, deci-degrees Celsius |
| the row's name (`0x00982910` plus the row index) | a custom row | an integer at the row's range, or an integer menu of its values; read-only when the row is |

The stream gate is the host tool's: `nxs <port> on` starts the sensor and
`off` parks it, so a capture session commits no `STREAM` row.

## The commit

A commit is one write from the staged cell of the highest row it names
through `MASK` and `COMMIT`. The driver reads `STATE` under the host bus
lock first, since a `COMMIT` written while BUSY or INERT is ignored, and
keeps the lock until `STATE` leaves BUSY. After the write it sends nothing
for the time the map painted for the kind, `APPLY_RUN_MS` after an apply
and `MODE_RUN_MS`, `GATE_RUN_MS` or `TRIGGER_RUN_MS` after a run, then
polls `STATE` every `APPLY_POLL_MS` for `APPLY_POLL_MAX_MS` past the
painted time, or every `MODE_POLL_MS` for `MODE_SWITCH_TIMEOUT_MS`, a NAK
read as busy. Outside a window a NAK is the device inside one of its own
transfers: the transfer repeats after `BUSY_RETRY_MS`, `BUSY_RETRIES`
times. One commit per frame is the budget.

The map is read at probe where the pod answers, else at the first capture
session. A map whose identity byte or layout version the driver does not
know refuses the probe. A map still INERT, with no personality run yet,
leaves the controls inert until the next session; a control set meanwhile
waits and commits at the next session that finds the map configured.
