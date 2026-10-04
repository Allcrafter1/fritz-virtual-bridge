// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FVB_PROVIDER_TRANSPORT_H
#define FVB_PROVIDER_TRANSPORT_H
#include <stddef.h>
#include <sys/types.h>

#pragma GCC visibility push(hidden)
/* Caller serializes frames. The callback must be the original native write,
 * never the interposed hook. Retry EINTR and short progress only; do not wait
 * or spin on EAGAIN. An error after a prefix cannot undo that stream prefix. */
ssize_t provider_write_frame(int fd, const void *data, size_t size,
                             ssize_t (*write_native)(int, const void *, size_t));
#pragma GCC visibility pop
#endif
