# Installation on a dedicated FRITZ!Box 7530

The first public target is deliberately narrow: classic FRITZ!Box 7530
(HW236), German FRITZ!OS 8.25, and the exact `aha` fingerprint listed in the
compatibility document. Do not use the build on another model or firmware.

## Before building

Use a separate laboratory/bridge box. Export its FRITZ!Box configuration and
have the official AVM recovery procedure available. Disable automatic FRITZ!OS
updates after installation because a new `aha` build will fail the runtime
fingerprint check until it has been analysed and supported explicitly.

You need:

- an x86-64 Linux build machine with Git and the normal Freetz-NG build
  prerequisites;
- the dedicated FRITZ!Box 7530;
- an MQTT broker reachable by both the box and Home Assistant;
- Home Assistant with its MQTT integration.

The dedicated 7530 must already be a Mesh Repeater/IP client of the FRITZ!Box
whose Smart Home UI and 440 you use. Establish and verify that relationship on
stock FRITZ!OS before modifying the bridge box. The Mesh master remains
unmodified; the provider enters virtual endpoints through the repeater's
existing Smart Home/Mesh path.

[Freetz-NG documents its supported build hosts, dependencies and installation
methods](https://freetz-ng.github.io/freetz-ng/wiki/10_Beginner/install.en/).
The script intentionally does not run `sudo` or install host packages. Missing
host tools such as `bison` or `flex` are reported by the Freetz prerequisite
check and must be installed through the build host's package manager.

## One-command local build

From this repository run:

```sh
./tools/build-firmware.sh
```

The script checks out the pinned Freetz-NG revision below `.build`, installs
the source package, selects the validated 7530 profile and builds locally.
Neither this repository nor its release downloads offers a ready-made firmware
image. The resulting image under `.build/freetz-ng/images` is for the person
who built it and must not be redistributed.

To inspect or add ordinary Freetz options before compiling:

```sh
./tools/build-firmware.sh --menuconfig
```

Advanced users can add the package to an existing checkout without replacing
its `.config`:

```sh
./tools/install-freetz-package.sh /path/to/freetz-ng
```

Then enable **Packages → F → FRITZ! Virtual Bridge** in `make menuconfig`.

## Flash and first setup

1. Export the 7530 configuration and note its fixed LAN address. Flash the
   locally built image using the [current Freetz-NG installation
   method](https://freetz-ng.github.io/freetz-ng/INSTALL/) for the exact target.
   Confirm that LAN access and the Mesh connection are healthy before enabling
   the package. A 7530 that already runs Freetz can normally be updated through
   the Freetz web interface; follow Freetz-NG's instructions for first install.
2. Open the Freetz web interface, then **Packages → FRITZ! Virtual Bridge**.
3. Enter the existing MQTT broker address, credentials and a unique bridge ID
   containing 3–32 lowercase letters, digits, `_` or `-`. Give this MQTT user
   read/write access only to `fritzvirtual/<bridge-id>/#` when the broker
   supports per-topic ACLs.
4. Enable the service and apply the Freetz configuration. Its fingerprint guard
   refuses an unsupported `/usr/bin/aha` instead of preloading code into an
   unknown build. The MQTT broker should now contain retained `bridge/info` and
   `bridge/availability` messages.
5. In HACS open **Custom repositories**, add this repository URL as category
   **Integration**, install **FRITZ! Virtual Bridge**, and restart Home
   Assistant. Until the first GitHub release, a manual copy of
   `custom_components/fritz_virtual_bridge` is the development alternative.
6. Home Assistant discovers the retained bridge announcement and creates its
   integration entry automatically. Open it in **Settings → Devices &
   services**, then use the `+` action labelled **Add virtual FRITZ! device**.
   Manual setup uses the same bridge ID and the default MQTT topic root
   `fritzvirtual`.
7. Select one HA entity per virtual device. The integration infers the narrowest
   matching profile; a color-temperature light may deliberately be reduced to
   a dimmable light. Assign the resulting device to a
   440 position in the normal FRITZ!OS Smart Home UI.

The bridge permits one persistent virtual device per Home Assistant entity.
One FRITZ device can be placed on several 440 controllers, so duplicate
mappings are unnecessary. Reconfiguring a mapping can bind a replacement HA
entity or change the FRITZ name without changing its FRITZ identity. A device
type change creates a new mapping and requires a new 440 assignment.

Do not keep an older MQTT automation active for the same virtual endpoint.
During migration, validate the new integration with a new device first, then
disable the corresponding old automation. Existing helpers and automations can
be removed only after their consumers have been checked and the new path has
survived a Home Assistant restart.

For a Zigbee2MQTT thermostat, the integration automatically enables the
schedule shadow when the climate entity has sibling entities named
`select.<name>_week`, `text.<name>_workdays_schedule`, and
`text.<name>_holidays_schedule`. Other thermostats retain all ordinary and
timed controls without schedule mirroring.

The bridge uses the central MQTT broker; it does not run another broker on the
FRITZ!Box. Home Assistant uses the credentials already stored by its MQTT
integration. The FRITZ!Box is a separate MQTT client: enter its broker account
under **Freetz → Packages → FRITZ! Virtual Bridge**. Freetz writes those values
to the root-only runtime file `/var/run/fritzvirtual/mqtt.conf` (mode `0600`).
An empty user and password are supported only if the broker allows anonymous
clients. Prefer a dedicated account restricted to
`fritzvirtual/<bridge-id>/#`.

## Updating

Automatic FRITZ!OS updates must remain disabled on the bridge box. Every new
firmware can change the private `aha` ABI and is unsupported until its binary
has been analysed and explicitly allowlisted. Updating this project means
building a new local image from the supported version and applying it through
Freetz. The persistent registry under `/tmp/flash/fritzvirtual/registry.json`
keeps device identities across ordinary restarts and compatible image updates;
export the box configuration before every firmware change.

Home Assistant integration updates follow the normal HACS flow. The bridge
replays all desired mappings whenever its MQTT connection returns, and the HA
side does the same when it sees the bridge come online, so startup order does
not matter.

## Diagnostics

On the bridge, the Freetz service status and these two logs are the first
places to check:

```text
/var/log/fritzvirtual-aha.log
/var/log/fritzvirtual-mqtt.log
```

The retained `bridge/info` document reports `ready`, the registry revision and
supported profiles. `online` with `ready: false` means MQTT works but the local
provider has not finished connecting or reprovisioning. A fingerprint error in
the service log means the installed FRITZ!OS build is deliberately unsupported.

## Rollback

Disable the service in Freetz. The stop action terminates only the bridge MQTT
process and the modified `aha` instance, then starts stock `aha` again without
`LD_PRELOAD`. Existing virtual entries may remain unavailable in FRITZ!OS;
they can be removed separately after checking 440 assignments. Reflashing an
unmodified image removes the package completely.
