/* SPDX-License-Identifier: MIT */
#ifndef FVB_DEVICE_REGISTRY_H
#define FVB_DEVICE_REGISTRY_H
#include <stddef.h>
#include <stdint.h>
#define FVB_REGISTRY_CAPACITY 32u
#define FVB_DEVICE_NAME_BYTES 80u
#define FVB_DEVICE_UID_BYTES 20u
#define FVB_FIRST_REMOTE_ID 456u

typedef enum {
    FVB_PROFILE_SWITCH, FVB_PROFILE_DIMMABLE_LIGHT,
    FVB_PROFILE_COLOR_TEMPERATURE_LIGHT, FVB_PROFILE_COVER,
    FVB_PROFILE_THERMOSTAT, FVB_PROFILE_COUNT
} fvb_device_profile;
typedef enum {
    FVB_REGISTRY_OK, FVB_REGISTRY_INVALID, FVB_REGISTRY_CONFLICT,
    FVB_REGISTRY_DUPLICATE, FVB_REGISTRY_FULL, FVB_REGISTRY_NOT_FOUND,
    FVB_REGISTRY_EXHAUSTED
} fvb_registry_result;
typedef struct {
    uint8_t ipui[5];
    uint16_t discriminator;
    uint16_t unit_id;
} fvb_hanfun_identity;
typedef struct {
    char uid[FVB_DEVICE_UID_BYTES];
    char name[FVB_DEVICE_NAME_BYTES];
    uint16_t remote_id;
    fvb_device_profile profile;
    fvb_hanfun_identity hanfun;
    uint8_t enabled;
    uint64_t revision;
} fvb_device;
/* Caller owns storage and serializes access. Read devices through const accessors.
 * Deleted remote IDs are never recycled; the allocator watermark remains durable.
 * Persistence must preserve the entire logical registry, including next_remote_id.
 * init is for a NEW registry only. No pointers or dynamic allocation are stored.
 */
typedef struct {
    uint64_t revision;
    uint32_t next_remote_id;
    size_t count;
    fvb_device devices[FVB_REGISTRY_CAPACITY];
} fvb_device_registry;
void fvb_registry_init(fvb_device_registry *registry);
int fvb_registry_valid_uid(const char *uid);
int fvb_registry_valid_name(const char *name);
const char *fvb_profile_name(fvb_device_profile profile);
const fvb_device *fvb_registry_find(const fvb_device_registry *, const char *uid);
const fvb_device *fvb_registry_find_remote(const fvb_device_registry *, uint16_t id);
const fvb_device *fvb_registry_at(const fvb_device_registry *, size_t index);
size_t fvb_registry_count(const fvb_device_registry *);
/* All mutations require the exact current GLOBAL revision. No-op mutations do
 * not increment revisions. Failed calls leave registry and output ID unchanged.
 * UID is supplied by the owner, canonicalized to uppercase hex. Identity and
 * profile cannot be changed by any update operation. */
fvb_registry_result fvb_registry_add(fvb_device_registry *, uint64_t expected_revision,
    const char *uid, const char *name, fvb_device_profile, uint16_t *remote_id);
fvb_registry_result fvb_registry_rename(fvb_device_registry *, uint64_t expected_revision,
    const char *uid, const char *name);
fvb_registry_result fvb_registry_set_enabled(fvb_device_registry *, uint64_t expected_revision,
    const char *uid, int enabled);
fvb_registry_result fvb_registry_remove(fvb_device_registry *, uint64_t expected_revision,
    const char *uid);
#endif
