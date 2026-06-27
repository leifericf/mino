/*
 * image_internal.h -- shared declarations for SLAD image save/load.
 *
 * Splits the serializer (image.c) from the deserializer (image_load.c).
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_IMAGE_INTERNAL_H
#define RUNTIME_IMAGE_INTERNAL_H

#include "mino_internal.h"

#include <stdint.h>
#include <stddef.h>

/* Image format constants */
#define IMG_MAGIC       "MINO-IMAGE/1"
#define IMG_VERSION     1
#define IMG_HT_INIT     256
#define IMG_HT_LOAD     75

/* Sanity ceiling on value/env IDs read from an image file. A real image
 * is small (ADR 12: ~30 KB for a player); an ID at this scale signals a
 * truncated or hostile file. Bounding it also bounds the four ID-table
 * allocations in the reader against a malicious huge id. */
#define IMG_MAX_ID      (1u << 24)

/* CRC32 (IEEE 802.3 polynomial 0xEDB88320).
 * Table is computed once on first use. */
uint32_t img_crc32_update(uint32_t crc, const void *data, size_t len);

/* Check if a namespace name belongs to the standard library
 * (reinstalled by mino_install_all). */
int img_is_stdlib_ns(const char *name);

#endif /* RUNTIME_IMAGE_INTERNAL_H */
