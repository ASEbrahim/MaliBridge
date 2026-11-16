/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 ASEbrahim and MaliBridge contributors */
/* Fast cores for the threads a game waits on (server_cores.c). */
#ifndef VKB_SERVER_CORES_H
#define VKB_SERVER_CORES_H
#include <stddef.h>
#include <stdint.h>

#define VKB_CORES_MIN_FAST 3
enum { VKB_HOT_AFF = 1, VKB_HOT_NICE = 2 };

void vkb_cores_conn_thread(void);   /* first thing on every connection thread */
void vkb_cores_device_made(void);   /* on the connection thread, after the driver made its device */
void vkb_cores_plain_thread(void);  /* first thing on a background thread a connection thread starts */

uint64_t vkb_cores_fast_mask(const unsigned *cap, int n);
void vkb_cores_mask_str(uint64_t m, char *out, size_t cap);
int vkb_cores_mode(const char *v);
void vkb_cores_test_root(const char *root);
#endif
