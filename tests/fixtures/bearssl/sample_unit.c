/* Sample translation unit exercising every tricky case of the
 * amalgam generator's rename pass (tools/make_amalgam.clj). Not
 * compiled anywhere; tests/bearssl_amalgam_test.clj asserts the
 * detection and suffixing behaviour over exactly this text. */

#include "inner.h"
#include <stdint.h>

#define ROTR32(x, n)   (((x) >> (n)) | ((x) << (32 - (n))))
#define TLEN  64

#pragma comment(lib, "advapi32")

typedef struct {
	uint32_t x;
	uint32_t y;
} point_u, *point_ptr;

typedef struct tag_spair {
	uint32_t a[2];
} spair_t;

typedef unsigned char byte_t;

static const unsigned char P[TLEN] = { 0, 1, 2 };

static uint32_t
add32(uint32_t a, uint32_t b)
{
	point_u pt;
	pt.x = a;
	pt.y = b;
	return pt.x + pt.y + ROTR32(a, 3);
}

static uint32_t table[4];

uint32_t sample_public(uint32_t x)
{
	spair_t s;
	(void) s;
	return add32(x, 1) + table[0] + MIN(x, TLEN) + MAX(x, TLEN);
}
