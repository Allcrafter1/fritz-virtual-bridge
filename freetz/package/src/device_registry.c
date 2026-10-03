/* SPDX-License-Identifier: MIT */
#include "device_registry.h"
#include <string.h>

static int hex_digit(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static unsigned hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10u;
    return c - 'A' + 10u;
}
int fvb_registry_valid_uid(const char *s) {
    size_t i;
    if (!s || s[0] != 'F' || s[1] != 'V' || s[2] != 'B') return 0;
    for (i = 3; i < 19; ++i) if (!hex_digit((unsigned char)s[i])) return 0;
    return s[19] == '\0';
}
static void canonical_uid(char out[20], const char *s) {
    size_t i;
    for (i = 0; i < 19; ++i)
        out[i] = s[i] >= 'a' && s[i] <= 'f' ? (char)(s[i] - 'a' + 'A') : s[i];
    out[19] = 0;
}
int fvb_registry_valid_name(const char *s) {
    size_t n = 0, i = 0;
    if (!s) return 0;
    while (n < FVB_DEVICE_NAME_BYTES && s[n]) ++n;
    if (!n || n == FVB_DEVICE_NAME_BYTES) return 0;
    while (i < n) {
        uint32_t cp;
        unsigned need, j;
        unsigned char c = (unsigned char)s[i++];
        if (c < 0x80) { cp = c; need = 0; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; need = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; need = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; need = 3; }
        else return 0;
        if (need > n - i) return 0;
        for (j = 0; j < need; ++j) {
            c = (unsigned char)s[i++];
            if ((c & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (c & 63u);
        }
        if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 ||
            (cp >= 0x7f && cp <= 0x9f)) return 0;
    }
    return 1;
}
const char *fvb_profile_name(fvb_device_profile p) {
    static const char *const names[] = { "switch", "dimmable_light",
        "color_temperature_light", "cover", "thermostat" };
    return (unsigned)p < FVB_PROFILE_COUNT ? names[p] : NULL;
}
void fvb_registry_init(fvb_device_registry *r) {
    if (r) { memset(r, 0, sizeof(*r)); r->next_remote_id = FVB_FIRST_REMOTE_ID; }
}
const fvb_device *fvb_registry_find(const fvb_device_registry *r, const char *uid) {
    char canonical[20]; size_t i;
    if (!r || r->count > FVB_REGISTRY_CAPACITY || !fvb_registry_valid_uid(uid)) return NULL;
    canonical_uid(canonical, uid);
    for (i = 0; i < r->count; ++i)
        if (!strcmp(r->devices[i].uid, canonical)) return &r->devices[i];
    return NULL;
}
const fvb_device *fvb_registry_find_remote(const fvb_device_registry *r, uint16_t id) {
    size_t i;
    if (!r || r->count > FVB_REGISTRY_CAPACITY) return NULL;
    for (i = 0; i < r->count; ++i) if (r->devices[i].remote_id == id) return &r->devices[i];
    return NULL;
}
const fvb_device *fvb_registry_at(const fvb_device_registry *r, size_t i) {
    return r && r->count <= FVB_REGISTRY_CAPACITY && i < r->count ? &r->devices[i] : NULL;
}
size_t fvb_registry_count(const fvb_device_registry *r) {
    return r && r->count <= FVB_REGISTRY_CAPACITY ? r->count : 0;
}
static fvb_registry_result check(const fvb_device_registry *r, uint64_t rev) {
    if (!r || r->count > FVB_REGISTRY_CAPACITY || r->next_remote_id < FVB_FIRST_REMOTE_ID)
        return FVB_REGISTRY_INVALID;
    return r->revision == rev ? FVB_REGISTRY_OK : FVB_REGISTRY_CONFLICT;
}
fvb_registry_result fvb_registry_add(fvb_device_registry *r, uint64_t rev,
    const char *uid, const char *name, fvb_device_profile p, uint16_t *id) {
    fvb_device d; fvb_registry_result result = check(r, rev);
    if (result != FVB_REGISTRY_OK) return result;
    if (!fvb_registry_valid_uid(uid) || !fvb_registry_valid_name(name) || !fvb_profile_name(p))
        return FVB_REGISTRY_INVALID;
    if (fvb_registry_find(r, uid)) return FVB_REGISTRY_DUPLICATE;
    if (r->count == FVB_REGISTRY_CAPACITY) return FVB_REGISTRY_FULL;
    if (r->next_remote_id > UINT16_MAX || r->revision == UINT64_MAX)
        return FVB_REGISTRY_EXHAUSTED;
    if (fvb_registry_find_remote(r, (uint16_t)r->next_remote_id)) return FVB_REGISTRY_INVALID;
    memset(&d, 0, sizeof(d)); canonical_uid(d.uid, uid);
    memcpy(d.name, name, strlen(name) + 1);
    d.remote_id = (uint16_t)r->next_remote_id; d.profile = p; d.enabled = 1;
    if (p == FVB_PROFILE_DIMMABLE_LIGHT || p == FVB_PROFILE_COLOR_TEMPERATURE_LIGHT || p == FVB_PROFILE_COVER) {
        size_t i;
        /* The radio-side identity follows the random stable UID rather than
         * the bridge-local allocator, so two bridge boxes do not generate the
         * same HAN-FUN IPUI for their first device. */
        for (i = 0; i < 5; ++i)
            d.hanfun.ipui[i] = (uint8_t)((hex_value((unsigned char)d.uid[9 + i * 2]) << 4) |
                                         hex_value((unsigned char)d.uid[10 + i * 2]));
        d.hanfun.discriminator = d.remote_id; d.hanfun.unit_id = 1;
    }
    d.revision = ++r->revision; r->devices[r->count++] = d; ++r->next_remote_id;
    if (id) *id = d.remote_id;
    return FVB_REGISTRY_OK;
}
fvb_registry_result fvb_registry_rename(fvb_device_registry *r, uint64_t rev,
    const char *uid, const char *name) {
    const fvb_device *found; fvb_device *d; char copy[80];
    fvb_registry_result result = check(r, rev);
    if (result != FVB_REGISTRY_OK) return result;
    if (!fvb_registry_valid_uid(uid) || !fvb_registry_valid_name(name)) return FVB_REGISTRY_INVALID;
    found = fvb_registry_find(r, uid); if (!found) return FVB_REGISTRY_NOT_FOUND;
    if (!strcmp(found->name, name)) return FVB_REGISTRY_OK;
    if (r->revision == UINT64_MAX) return FVB_REGISTRY_EXHAUSTED;
    memcpy(copy, name, strlen(name) + 1); d = &r->devices[found - r->devices];
    memcpy(d->name, copy, strlen(copy) + 1); d->revision = ++r->revision;
    return FVB_REGISTRY_OK;
}
fvb_registry_result fvb_registry_set_enabled(fvb_device_registry *r, uint64_t rev,
    const char *uid, int enabled) {
    const fvb_device *found; fvb_device *d;
    fvb_registry_result result = check(r, rev);
    if (result != FVB_REGISTRY_OK) return result;
    if (!fvb_registry_valid_uid(uid) || (enabled != 0 && enabled != 1)) return FVB_REGISTRY_INVALID;
    found = fvb_registry_find(r, uid); if (!found) return FVB_REGISTRY_NOT_FOUND;
    if (found->enabled == enabled) return FVB_REGISTRY_OK;
    if (r->revision == UINT64_MAX) return FVB_REGISTRY_EXHAUSTED;
    d = &r->devices[found - r->devices]; d->enabled = (uint8_t)enabled;
    d->revision = ++r->revision; return FVB_REGISTRY_OK;
}
