/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MGBA_AM_PLAYER_H
#define MGBA_AM_PLAYER_H
#include <mgba/core/core.h>

/* Configure an initialized core, then run an already loaded ROM. */
void mAMConfigure(struct mCore* core);
int mAMRun(struct mCore* core, bool headless, bool audio, unsigned frame_limit);
#endif
