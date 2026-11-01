/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 */
#pragma once

/* The fault handler that lets server_guard.c fail a compile the driver crashed in (layer_crash.cpp);
 * once per process, chained to the handler that was there before. */
void layer_crash_install();
