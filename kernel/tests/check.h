/* Minimal test helpers for the in-VM user-space tests (no gtest in the initramfs). */
#ifndef KCHECK_H
#define KCHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int g_fail;
#define CHECK(cond, ...) do { if (cond) { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } \
	else { g_fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("  [%s:%d errno=%d %s]\n", __FILE__, __LINE__, errno, strerror(errno)); } } while (0)
#endif
