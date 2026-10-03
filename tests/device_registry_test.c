/* SPDX-License-Identifier: MIT */
#include "device_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *uid = "FVB00000000000000af";
static void validation(void) {
    char name[81];
    assert(!fvb_registry_valid_uid(NULL));
    assert(!fvb_registry_valid_uid(""));
    assert(!fvb_registry_valid_uid("F"));
    assert(!fvb_registry_valid_uid("FV"));
    assert(!fvb_registry_valid_uid("FVB"));
    assert(fvb_registry_valid_uid(uid));
    assert(!fvb_registry_valid_uid("FVB00000000000000ag"));
    assert(!fvb_registry_valid_uid("FVB00000000000000aff"));
    assert(!fvb_registry_valid_name(NULL)); assert(!fvb_registry_valid_name(""));
    assert(fvb_registry_valid_name("K\xc3\xbc" "che \xf0\x9f\x92\xa1"));
    assert(!fvb_registry_valid_name("line\nfeed"));
    assert(!fvb_registry_valid_name("\x7f"));
    assert(!fvb_registry_valid_name("\xc2\x85"));
    assert(!fvb_registry_valid_name("\xc0\xaf"));
    assert(!fvb_registry_valid_name("\xe0\x80\xaf"));
    assert(!fvb_registry_valid_name("\xed\xa0\x80"));
    assert(!fvb_registry_valid_name("\xf4\x90\x80\x80"));
    assert(!fvb_registry_valid_name("\xe2\x82"));
    assert(!fvb_registry_valid_name("\x80"));
    memset(name, 'a', 79); name[79] = 0; assert(fvb_registry_valid_name(name));
    name[79] = 'a'; name[80] = 0; assert(!fvb_registry_valid_name(name));
    assert(!fvb_profile_name((fvb_device_profile)-1));
}
static void mutations(void) {
    fvb_device_registry r, before; fvb_device identity; uint16_t id = 9;
    fvb_registry_init(&r); assert(r.next_remote_id == 456);
    assert(fvb_registry_add(&r, 1, uid, "Test", FVB_PROFILE_COVER, &id) == FVB_REGISTRY_CONFLICT);
    assert(id == 9 && !r.count);
    assert(fvb_registry_add(&r, 0, uid, "Test", FVB_PROFILE_COVER, &id) == FVB_REGISTRY_OK);
    assert(id == 456 && r.revision == 1 && r.count == 1);
    identity = *fvb_registry_find(&r, uid);
    assert(!strcmp(identity.uid, "FVB00000000000000AF"));
    assert(identity.hanfun.unit_id == 1 && identity.hanfun.discriminator == 456);
    assert(identity.hanfun.ipui[0] == 0 && identity.hanfun.ipui[1] == 0);
    assert(identity.hanfun.ipui[2] == 0 && identity.hanfun.ipui[3] == 0);
    assert(identity.hanfun.ipui[4] == 0xaf);
    assert(fvb_registry_find_remote(&r, id) == fvb_registry_at(&r, 0));
    assert(!fvb_registry_at(&r, 1)); assert(fvb_registry_count(&r) == 1);
    before = r;
    assert(fvb_registry_add(&r, 1, identity.uid, "Other", FVB_PROFILE_SWITCH, &id) == FVB_REGISTRY_DUPLICATE);
    assert(!memcmp(&before, &r, sizeof(r)));
    assert(fvb_registry_rename(&r, 1, uid, "New") == FVB_REGISTRY_OK);
    assert(fvb_registry_rename(&r, 1, uid, "Bad") == FVB_REGISTRY_CONFLICT);
    assert(fvb_registry_rename(&r, 2, uid, "New") == FVB_REGISTRY_OK && r.revision == 2);
    assert(fvb_registry_set_enabled(&r, 2, uid, 0) == FVB_REGISTRY_OK);
    assert(fvb_registry_set_enabled(&r, 3, uid, 0) == FVB_REGISTRY_OK && r.revision == 3);
    assert(fvb_registry_set_enabled(&r, 3, uid, 1) == FVB_REGISTRY_OK);
    assert(!strcmp(r.devices[0].uid, identity.uid));
    assert(r.devices[0].remote_id == identity.remote_id);
    assert(!memcmp(&r.devices[0].hanfun, &identity.hanfun, sizeof(identity.hanfun)));
    assert(r.devices[0].profile == identity.profile);
    before = r;
    assert(fvb_registry_set_enabled(&r, 4, uid, 2) == FVB_REGISTRY_INVALID);
    assert(fvb_registry_rename(&r, 4, uid, "\n") == FVB_REGISTRY_INVALID);
    assert(fvb_registry_rename(&r, 4, "FVB0000000000000000", "Absent") == FVB_REGISTRY_NOT_FOUND);
    assert(!memcmp(&before, &r, sizeof(r)));
    assert(fvb_registry_add(&r, 4, "FVB0000000000000001", "Second", FVB_PROFILE_SWITCH, NULL) == FVB_REGISTRY_OK);
    assert(fvb_registry_remove(&r, 5, uid) == FVB_REGISTRY_OK);
    assert(r.count == 1 && r.revision == 6 && !strcmp(r.devices[0].uid, "FVB0000000000000001"));
    assert(r.next_remote_id == 458 && r.devices[1].uid[0] == 0);
    assert(fvb_registry_remove(&r, 6, uid) == FVB_REGISTRY_NOT_FOUND);
    r.revision = UINT64_MAX;
    assert(fvb_registry_rename(&r, UINT64_MAX, "FVB0000000000000001", "Overflow") == FVB_REGISTRY_EXHAUSTED);
}
static void capacity(void) {
    fvb_device_registry r, before; char key[20]; size_t i;
    fvb_registry_init(&r);
    for (i = 0; i < FVB_REGISTRY_CAPACITY; ++i) {
        snprintf(key, sizeof(key), "FVB%016llX", (unsigned long long)i);
        assert(fvb_registry_add(&r, r.revision, key, "Device", (fvb_device_profile)(i % FVB_PROFILE_COUNT), NULL) == FVB_REGISTRY_OK);
        assert(r.devices[i].remote_id == 456 + i);
    }
    before = r;
    assert(fvb_registry_add(&r, r.revision, uid, "Overflow", FVB_PROFILE_SWITCH, NULL) == FVB_REGISTRY_FULL);
    assert(!memcmp(&before, &r, sizeof(r)));
    fvb_registry_init(&r); r.next_remote_id = UINT16_MAX;
    assert(fvb_registry_add(&r, 0, uid, "Last", FVB_PROFILE_SWITCH, NULL) == FVB_REGISTRY_OK);
    assert(r.devices[0].remote_id == UINT16_MAX && r.next_remote_id == 65536);
    assert(fvb_registry_add(&r, 1, "FVB0000000000000001", "No IDs", FVB_PROFILE_SWITCH, NULL) == FVB_REGISTRY_EXHAUSTED);
}
int main(void) { validation(); mutations(); capacity(); puts("device_registry_test: OK"); return 0; }
