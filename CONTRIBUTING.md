# Contributing

Changes are welcome under the repository's MIT license. Keep contributions
source-only: never commit an AVM binary, original or modified firmware image,
FRITZ!Box backup, credential, serial number or unredacted support archive.

Before opening a change:

```sh
ruff check .
pytest
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -Ifreetz/package/src freetz/package/src/device_registry.c \
  tests/device_registry_test.c -o /tmp/device_registry_test
/tmp/device_registry_test
```

Provider changes also require both isolated provider self-test modes and a
fresh Freetz cross-build. A new FRITZ!OS fingerprint needs offline comparison
and a live test on a dedicated box before it can enter the compatibility
allowlist. Describe the device/profile tested and whether 440 commands and HA
state feedback were both verified.

Protocol changes must remain bounded, versioned and backward compatible for a
released schema. Persisted device UID, remote ID, profile and HAN-FUN identity
are immutable. A profile change creates a new device.
