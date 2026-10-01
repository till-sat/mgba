/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MGBA_FLAGS_H
#define MGBA_FLAGS_H

/* Fixed configuration for the standalone Linux SDL2 player. */
#define BUILD_STATIC
#define MINIMAL_CORE 1
#define M_CORE_GB
#define M_CORE_GBA
#define ENABLE_DIRECTORIES
#define ENABLE_VFS
#define ENABLE_VFS_FD
#define USE_PTHREADS

/* Linux/POSIX libc facilities. strlcpy uses the portable implementation. */
#define HAVE_STRDUP
#define HAVE_STRNDUP
#define HAVE_VASPRINTF
#define HAVE_FREELOCALE
#define HAVE_NEWLOCALE
#define HAVE_SETLOCALE
#define HAVE_USELOCALE
#define HAVE_LOCALE
#define HAVE_FUTIMENS
#define HAVE_LOCALTIME_R
#define HAVE_REALPATH
#define HAVE_PTHREAD_CREATE
#define HAVE_PTHREAD_SETNAME_NP

#endif
