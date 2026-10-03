/* SPDX-License-Identifier: MIT OR Apache-2.0 */
#include "device_registry.h"
#include "registry_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #value); \
        ++failures; \
    } \
} while (0)

int main(void) {
    fvb_device_registry original, loaded, unchanged;
    uint16_t remote_id = 0;
    char error[160], path[128], *json;
    const fvb_device *device;
    fvb_registry_init(&original);
    CHECK(fvb_registry_add(&original, 0, "FVB0123456789abcdef", "Morgenlicht",
                           FVB_PROFILE_COLOR_TEMPERATURE_LIGHT, &remote_id) == FVB_REGISTRY_OK);
    CHECK(remote_id == FVB_FIRST_REMOTE_ID);
    CHECK(fvb_registry_add(&original, original.revision, "FVB1111111111111111", "Heizung Büro",
                           FVB_PROFILE_THERMOSTAT, NULL) == FVB_REGISTRY_OK);
    CHECK(fvb_registry_set_enabled(&original, original.revision,
                                   "FVB1111111111111111", 0) == FVB_REGISTRY_OK);

    json = fvb_registry_serialize(&original);
    CHECK(json != NULL);
    CHECK(json && strstr(json, "\"schema_version\":1"));
    CHECK(json && fvb_registry_deserialize(json, &loaded, error, sizeof(error)));
    if (json) fvb_registry_serialized_free(json);
    CHECK(loaded.count == 2);
    CHECK(loaded.revision == original.revision);
    CHECK(loaded.next_remote_id == original.next_remote_id);
    device = fvb_registry_find(&loaded, "FVB0123456789ABCDEF");
    CHECK(device != NULL);
    CHECK(device && !strcmp(device->name, "Morgenlicht"));
    CHECK(device && device->hanfun.unit_id == 1);
    device = fvb_registry_find(&loaded, "FVB1111111111111111");
    CHECK(device && !device->enabled);

    unchanged = loaded;
    CHECK(!fvb_registry_deserialize(
        "{\"schema_version\":2,\"revision\":0,\"next_remote_id\":456,\"devices\":[]}",
        &loaded, error, sizeof(error)));
    CHECK(!memcmp(&loaded, &unchanged, sizeof(loaded)));
    CHECK(!fvb_registry_deserialize(
        "{\"schema_version\":1,\"revision\":1,\"next_remote_id\":457,\"devices\":["
        "{\"uid\":\"FVB0123456789ABCDEF\",\"name\":\"x\",\"remote_id\":456,"
        "\"profile\":\"cover\",\"enabled\":true,\"revision\":1,"
        "\"hanfun\":{\"ipui\":\"not-hexxxx\",\"discriminator\":1,\"unit_id\":1}}]}",
        &loaded, error, sizeof(error)));

    snprintf(path, sizeof(path), "/tmp/fvb-registry-test-%ld.json", (long)getpid());
    unlink(path);
    CHECK(fvb_registry_save_file(path, &original, error, sizeof(error)));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(fvb_registry_load_file(path, &loaded, error, sizeof(error)));
    CHECK(loaded.count == original.count);
    CHECK(loaded.revision == original.revision);
    unlink(path);

    if (failures) return 1;
    puts("registry_store_test: OK");
    return 0;
}
