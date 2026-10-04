/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_PROFILE_H
#define AM_PROFILE_H

/* Optional physical proto-soc PC sampling; output is outside the timed loop. */
void am_profile_prepare(void);
void am_profile_start(void);
void am_profile_stop(void);
void am_profile_report(void);

#endif
