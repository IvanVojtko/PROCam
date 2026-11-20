/* SPDX-FileCopyrightText: 2026 ProCam Contributors
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * quirc (src/3rdparty/quirc, ISC, licenses/quirc-ISC.txt) as one unit,
 * built without its assertions: they check the flood fill's internal
 * state, and a failed one must not take the camera down with it.
 */
#ifndef NDEBUG
#define NDEBUG
#endif
/* Room for every candidate region of a busy frame (the default, 254, runs
 * out on textured backgrounds before the scan reaches the code); 16-bit
 * region labels, so 2 bytes per pixel. Internal to quirc: quirc.h is the
 * same either way. */
#define QUIRC_MAX_REGIONS 65534
/* Its sign comparisons and unused callback parameters are harmless. */
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "3rdparty/quirc/quirc.c"
#include "3rdparty/quirc/identify.c"
#include "3rdparty/quirc/decode.c"
#include "3rdparty/quirc/version_db.c"
