/* SPDX-License-Identifier: BSD-2-Clause
 * Adapter for the unmodified upstream IPC headers in a FreeBSD kernel build.
 */
#ifdef _KERNEL
#include <sys/stdint.h>
#else
#include_next <stdint.h>
#endif
