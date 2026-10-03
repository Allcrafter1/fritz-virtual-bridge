/* SPDX-License-Identifier: MIT */
#ifndef FVB_REGISTRY_STORE_H
#define FVB_REGISTRY_STORE_H

#include "device_registry.h"
#include <stddef.h>

/* JSON is the persistent, versioned interchange form.  Returned serialized
 * strings are owned by the caller and must be released with
 * fvb_registry_serialized_free(). */
char *fvb_registry_serialize(const fvb_device_registry *registry);
void fvb_registry_serialized_free(char *serialized);
int fvb_registry_deserialize(const char *serialized,
                             fvb_device_registry *registry,
                             char *error,
                             size_t error_size);

/* save_file writes a sibling temporary file, fsyncs it and atomically renames
 * it over path.  load_file never changes registry when validation fails. */
int fvb_registry_save_file(const char *path,
                           const fvb_device_registry *registry,
                           char *error,
                           size_t error_size);
int fvb_registry_load_file(const char *path,
                           fvb_device_registry *registry,
                           char *error,
                           size_t error_size);

#endif
