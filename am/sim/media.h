/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_SIM_MEDIA_H
#define AM_SIM_MEDIA_H

/* Simulation-only extension. These addresses are not part of proto-soc v3.
 * All registers, framebuffer pixels and stereo sample pairs use LE32 accesses.
 * Commands complete synchronously; publish data before issuing a command. */
#define AM_SIM_BASE          0x40000000u
#define AM_SIM_SIZE          0x00100000u
#define AM_SIM_ID            0x00u
#define AM_SIM_MAGIC         0x414d5301u
#define AM_SIM_CONTROL       0x04u
#define AM_SIM_STATUS        0x08u
#define AM_SIM_WIDTH         0x0cu
#define AM_SIM_HEIGHT        0x10u
#define AM_SIM_PRESENT       0x14u
#define AM_SIM_QUIT          0x18u
#define AM_SIM_TIME_LO       0x1cu
#define AM_SIM_TIME_HI       0x20u
#define AM_SIM_SLEEP_US      0x24u
#define AM_SIM_AUDIO_RATE    0x28u
#define AM_SIM_AUDIO_CAP     0x2cu
#define AM_SIM_AUDIO_QUEUED  0x30u
#define AM_SIM_AUDIO_SUBMIT  0x34u
#define AM_SIM_AUDIO_WRITTEN 0x38u
#define AM_SIM_VIDEO         1u
#define AM_SIM_AUDIO         2u
#define AM_SIM_ERROR         0x80000000u
#define AM_SIM_FB            0x1000u
#define AM_SIM_MAX_WIDTH     256u
#define AM_SIM_MAX_HEIGHT    256u
#define AM_SIM_PCM           0x41000u
#define AM_SIM_PCM_FRAMES    2048u

#endif
