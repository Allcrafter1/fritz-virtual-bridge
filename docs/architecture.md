# Architecture and user workflow

## Components

The system has three deliberately separate layers.

### Freetz-NG package

The package runs on a dedicated supported FRITZ!Box and contains only project
code:

- a small provider loaded into the existing `aha` process,
- a separate MQTT and persistence process,
- a local control client,
- a Freetz init script and configuration page.

The provider reuses the original FRITZ!OS Smart Home and Mesh stack. It does
not implement DECT radio and does not replace an AVM executable. The init
script starts `aha` with the provider only after verifying the exact supported
binary fingerprint. Stopping the package restarts the original process
without the provider.

### Home Assistant custom integration

One config entry represents one FRITZ! Virtual Bridge. Every mapped Home
Assistant entity is stored as a config subentry. This gives the user normal UI
flows to add, rename, reconfigure and remove mappings without YAML or a second
web interface.

The integration does not create a second controllable copy of the selected
light, switch, cover or climate entity. It translates between the stable
virtual-device endpoint and the original entity's native Home Assistant
actions and state attributes.

### MQTT transport

MQTT carries management, FRITZ-originated commands and confirmed Home
Assistant state. Messages are local, versioned and independent of Home
Assistant's internal storage format. Retained configuration and state allow
both sides to recover after restart in either order.

## Native source layout and ownership

The C provider is split by responsibility. A button press still takes the same
path: the native `send()` hook recognizes a complete AHA command, updates the
selected device under a mutex, and puts a value-only event in a bounded queue.
The worker takes that snapshot and sends JSON to the separate MQTT bridge.
Confirmed HA state returns through the local control socket and the profile
encoder constructs the original AHA status packets.

| Source | Responsibility |
|---|---|
| `virtual_provider.c` | Interposition hooks, transport discovery, immutable identity substitution, device selection, native command handling and local control worker |
| `provider_state.h` | One independent set of values for each device, plus the legacy self-test state |
| `provider_profiles.c` | Exact switch, light, cover and thermostat packet layouts; explicit encoder context and output callback |
| `provider_codec.h` | Unaligned-safe network-order numbers and complete-batch framing checks |
| `provider_transport.c` | Finish short frame writes and retry interrupted writes through the original native function |
| `provider_events.c` | Bounded event queue and checked JSON formatting outside the provider mutex |
| `control_parse.h` | Bounded decimal timer/schedule parsing shared with the MQTT bridge, independent of 32/64-bit `unsigned long` |
| `device_registry.c`, `registry_store.c` | Identity validation/allocation and atomic persistence, respectively |

Device selection changes a pointer to the device's own values. It no longer
copies dozens of globals into and out of a shared scratch space. Selection,
registry changes, encoder use and queue operations all require the same mutex.
The worker copies an event before releasing that mutex, so no borrowed device
pointer escapes into JSON delivery. Profile helpers are private symbols; only
the existing libc hooks interpose on the native process.

Startup creates a nonblocking wake pipe and a private listener before enabling
the hooks. An invalid socket path, failed listener or failed worker creation
leaves the provider inactive and releases the resources it acquired. Discovery
slots are reclaimed when either socketpair endpoint closes, so reconnects do
not permanently exhaust the fixed table. Socket type flags are independent of
the stream type. Closed WATCH clients are retired before accepting another
client, even when no device event arrives to reveal the disconnection.

### Native review boundaries

The refactor keeps the reverse-engineered wire constants, identities, control
commands and MQTT schema. `tests/fixtures/provider-v0.1.4-dynamic.hex` contains
the complete sequence of frames observed by the dynamic integration test using
the original provider at commit `99bd3f2`. The test compares every byte after
zeroing only the clock-derived Function 95/98 and thermostat activation fields.
This includes all supported profiles, state isolation, restore gaps, native
commands, mixed batches and confirmed state feedback. The legacy integration
test remains separate.

Fault tests also cover queue overflow order, undersized JSON buffers, numeric
overflow, startup descriptor ownership, interrupted/short/zero-progress writes
and errors from closing a persistent file. Atomic save never replaces the old
file on those failures and never retries `close()` on a possibly reused fd.

The persistence reader opens one non-symlink descriptor, checks that descriptor
with `fstat`, and reads it with an explicit size limit. It rejects FIFOs without
blocking, truncated/growing files, embedded NUL bytes, decoded JSON NUL escapes,
and trailing non-JSON text. Literal backslash text remains valid. Allocation
fault tests exercise construction and cleanup of both persisted and published
JSON, including children not yet attached to their parent object.

The control CLI handles interrupted/short stdout writes and rejects truncated
SEQPACKET replies or EOF before a reply. Provider control sockets close on exec;
missing native function symbols leave the provider inactive. The MQTT process
checks authentication/will/loop startup errors and validates WATCH acceptance.
Native deletion and flash persistence have monotonic four- and thirty-second
child-process deadlines. The cache tests cover every supported profile/property
combination and prove that deletion compaction keeps feedback attached to its
original UID.

### Compiler and loader assumptions

Freetz can supply `-Ofast`, which otherwise permits the compiler to assume that
NaN and infinity never occur. The package appends `-fno-fast-math` and
`-fno-finite-math-only` after those inherited flags, preserving validation before
floating-point-to-integer conversions. Optimized tests explicitly include NaN
and infinity. Package builds also use strict warnings, a strong stack protector,
full RELRO/eager binding and undefined-symbol link checks.

The ARM shared object was checked against exports from the original AVM
libraries, including `__stack_chk_fail` in libc and `__stack_chk_guard` in the
loader. The pinned uClibc headers explicitly disable `_FORTIFY_SOURCE`; this
build therefore does not claim FORTIFY coverage. Loader symbol checks are an
offline compatibility check, not a substitute for a dedicated-box test.

Remaining operating limits are explicit:

- The event ring retains the newest 31 pending commands and drops the oldest
  on overflow, matching the existing policy. It is bounded, not a durable queue.
- Native output holds the provider mutex to keep frame order. Short positive
  writes and `EINTR` are completed; zero progress, `EAGAIN` and other errors fail
  immediately. A fatal failure after a stream prefix cannot retract those bytes.
  This is not a nonblocking or real-time transport redesign.
- A slow local control client can occupy the worker for its existing one-second
  receive timeout. Watcher sends remain nonblocking; slow watchers disconnect.
- The provider is process-lifetime code: stopping the package restarts `aha`.
  Unloading an active provider with `dlclose()` is unsupported.
- Directory `fsync` after the registry rename remains best-effort. Power-loss
  durability depends on the filesystem and the separate Freetz flash-persist
  step; a successful in-memory/native test does not prove flash durability.
- Helper deadlines supervise the direct child. Arbitrary configured commands
  that daemonize grandchildren are outside that contract. Native device deletion
  and registry persistence also remain separate operations without transactional
  rollback across AVM and the filesystem.
- Offline golden tests and host sanitizers do not prove undocumented AVM thread
  behavior, radio synchronization or 440 pages. A dedicated-box smoke test is
  still required before deploying the refactor or expanding firmware support.

## Device identity and lifecycle

Each mapping receives an immutable random 64-bit identifier. Its FRITZ UID is
`FVB` followed by sixteen hexadecimal characters, which fits the native
19-character field. The following values are allocated once and persisted on
the bridge:

- FRITZ UID,
- provider remote ID,
- device profile,
- HAN-FUN IPUI/unit discriminator where applicable,
- user-visible device name,
- enabled/disabled state.

Renaming or binding a replacement Home Assistant entity keeps the FRITZ UID
and all radio-side identities. Existing 440 assignments therefore survive.
Changing the device profile creates a new virtual device because FRITZ!OS
capabilities and widget layouts are type-specific.

Removing a mapping is authoritative: the bridge calls the local native AHA
deletion path, removes the provider endpoint and then removes the persistent
registry entry. Existing controller assignments for that virtual device are
therefore removed with it. Provider IDs are never reused.

## Management protocol

The initial public protocol uses schema version 1 beneath
`fritzvirtual/<bridge_id>`:

| Topic | Direction | Purpose |
|---|---|---|
| `bridge/availability` | bridge -> HA | retained online/offline status |
| `bridge/info` | bridge -> HA | retained version, capabilities and compatibility |
| `management/request` | HA -> bridge | versioned upsert/rename/prune request |
| `management/response/<request_id>` | bridge -> HA | correlated accepted/error response |
| `device/<uid>/command` | bridge -> HA | commands from FRITZ!OS or the 440 |
| `device/<uid>/<property>/set` | HA -> bridge | confirmed HA state and configuration |

Management JSON is validated and persisted by the separate MQTT process. The
provider inside `aha` receives only a bounded, validated local snapshot and
never parses MQTT credentials or writes persistent files.

Provider restoration carries each persistent remote ID explicitly. Replaying
only device names in insertion order is insufficient after deletions: the
durable allocator retains gaps while an empty provider starts at 456.

For the pinned local 7530 receiver, Function 98's opaque interface array uses
little-endian words; the surrounding network fields remain big-endian. The
local ETSI handler performs another byte reversal on this array. Incorrect
encoding can leave a visible parent device without a controllable unit.
Native unit creation must be checked independently of transport acceptance.

The exact version-1 messages, validation rules and recovery semantics are in
[MQTT protocol version 1](mqtt-protocol.md).

## Supported profiles

The first usable release targets:

| HA source | Virtual FRITZ profile | Functions |
|---|---|---|
| `switch`, `input_boolean` | switch/socket | on/off |
| `light` | dimmable or color-temperature light | on/off, brightness, optional color temperature |
| `cover` | blind | open, close, stop, position |
| `climate` | radiator thermostat | mode, setpoint, Boost/cold timer, optional schedule shadow |

Free HS color remains an experimental capability until the 440 palette and
arbitrary Home Assistant color gamuts have a predictable mapping.

The thermostat schedule adapter is optional. The first adapter supports the
validated Zigbee2MQTT `5+2` workday/holiday schedule entities. The internal
schedule model is generic so later adapters can support other thermostat
integrations without changing the FRITZ protocol.

For the standard Zigbee2MQTT entity naming scheme the integration detects the
`select.*_week`, `text.*_workdays_schedule` and
`text.*_holidays_schedule` siblings automatically. It publishes a rolling
current/next transition once per minute only while the real thermostat uses
the `schedule` preset and the week selector is `5+2`. Editing the virtual
schedule in FRITZ!OS is rejected by the provider and the HA-owned shadow is
restored.

Boost and cold overrides are kept in a private atomic Home Assistant store.
The first override records the previous temperature, HVAC mode and preset.
Extending or switching the override retains that original state. Expiry,
manual cancellation and a Home Assistant restart restore it without visible
input helpers.

## User workflow

1. Install **FRITZ! Virtual Bridge**. Home Assistant creates an integration
   entry automatically from the retained MQTT bridge announcement and shows an
   onboarding notification explaining the otherwise icon-only add action.
2. Open the bridge entry and use the `+` action in the top-right corner. The
   following dialog is titled **Add virtual FRITZ! device**.
3. Select a source entity. The integration proposes the matching profile and
   capabilities; the user can reduce optional capabilities.
4. Choose a FRITZ device name. The immutable endpoint is created and confirmed
   by the bridge.
5. Follow the final wizard page's direct FRITZ!OS link and place or configure
   the device there. The persistent completion notification and the bridge
   device named **Open the FRITZ!OS interface** remain permanent alternatives.
   Screen layout and its short display label remain FRITZ!OS responsibilities.
6. Reconfigure a mapping to bind a replacement HA entity while retaining the
   FRITZ identity and 440 assignments.

When a mapped thermostat has no recognized schedule adapter, setpoint, mode,
Boost and cold continue to work; only the native next-change schedule display
stays disabled.

## Latency topology

The MQTT leg is local push. In the initial validation topology, however, the
440 is paired with a 6690 Mesh master while virtual endpoints originate on a
7530 Mesh repeater. Measured absolute dim commands all reached Home Assistant
with the correct value, but the Mesh path serialized them at roughly 6–7
seconds each. The confirmed Home Assistant state reached the FRITZ entity about
0.6 seconds after the source entity changed.

For interactive control, pair the controller directly with the dedicated
bridge box. The laboratory 440 now uses the 7530 directly. Captured commands
that reached MQTT were forwarded to Zigbee2MQTT within about 0–45 ms; in one
physical test Zigbee2MQTT reported the requested values about 180–210 ms after
the bridge command. These are segment measurements, not button-to-light
latency guarantees or proof of physical bulb response.

Direct-pairing investigation also found a separate provider defect fixed in
0.1.4: FRITZ!OS may combine several protocol frames in a single `send()`.
Requiring the first frame's length to equal the entire buffer silently bypassed
virtual command handling for such batches. The provider now validates the full
batch, processes its frames in order and preserves unrelated traffic. The
regression test fails against the old provider and passes against the fix.
This is independent of MQTT polling and does not require a faster HA poll rate.

## Safety and rollback

- Only explicitly fingerprinted FRITZ!OS/`aha` builds are accepted.
- Provider input lengths, IDs and enum values are bounded before use.
- MQTT credentials remain in root-readable Freetz configuration.
- Unknown management schema versions are rejected.
- Stopping the Freetz service restarts stock `aha` without `LD_PRELOAD`.
- Removing the package requires building/flashing a normal Freetz or AVM
  image; no AVM file is patched in place at runtime.
- The productive Mesh master is never modified by the installation.
