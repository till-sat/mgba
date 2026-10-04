/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MGBA_FLAGS_H
#define MGBA_FLAGS_H

/* Shared single-threaded core configuration. */
#define BUILD_STATIC
#define MINIMAL_CORE 1
#define M_CORE_GB
#define M_CORE_GBA
#define DISABLE_THREADING
#define DISABLE_ANON_MMAP
#define HAVE_STRDUP
#define HAVE_STRNDUP
#define HAVE_LOCALTIME_R
#define HAVE_FREELOCALE
#define HAVE_NEWLOCALE
#define HAVE_USELOCALE
#define HAVE_LOCALE

#ifdef AM_BAREMETAL
#define HAVE_STRTOF_L
#endif

#ifndef AM_BAREMETAL
#define ENABLE_DIRECTORIES
#define ENABLE_VFS
#define ENABLE_VFS_FD

/* Linux/POSIX libc facilities. strlcpy uses the portable implementation. */
#define HAVE_VASPRINTF
#define HAVE_SETLOCALE
#define HAVE_FUTIMENS
#define HAVE_REALPATH
#endif

#endif
