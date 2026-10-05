/* SPDX-License-Identifier: MPL-2.0 */
/* The ysyx image is a GBA-only AM target.  Keep the shared mGBA flags while
 * removing the Game Boy core registration; its audio implementation is still
 * compiled separately because GBA PSG audio uses the shared code. */
#undef M_CORE_GB
