/* Descent on the m9b SoC: perspective texture mapping by spans (-tmap m9b,
 * see select_tmap() in texmap/scanline.c).
 *
 * D1X's C mapper (c_tmap_scanline_per) divides u and v by z for every
 * pixel: two divisions per pixel, 70 cycles or more on this CPU (a radix-2
 * divider), two thirds of the frame time in the first level. As DOS
 * Descent's assembly mappers did, this one computes the exact texture
 * coordinates every SPAN pixels and steps linearly in between; the error
 * is at most a fraction of a texel on a 320-pixel screen.
 *
 * The coordinates are 16.16 texels, (u << 16) / z (u / z is the texel, as
 * in the original), computed from one reciprocal of z per span end: z
 * reduced to 16 significant bits (zs = z >> k), r = 2^31 / zs with the
 * hardware's 32-bit divide, then (u * r) >> (15 + k) for u and v. The
 * relative error is about 2^-15, a small fraction of a texel.
 */
#include <stdint.h>

#include "fix.h"
#include "gr.h"
#include "texmap.h"
#include "texmapl.h"
#include "m9b_map.h"

#define SPAN      16
#define SPAN_LOG  4
#define TRANSPARENT 255

/* Texel coordinates (16.16) of u and v at depth z (> 0). */
static inline void texel_coords(fix u, fix v, fix z, int *tu, int *tv)
{
	uint32_t zu = (uint32_t)z, zs, r;
	int k = 0, sh;
	/* significant bits beyond 16 (no clz instruction: a few compares) */
	if (zu >= 0x01000000u) { zu >>= 8; k += 8; }
	if (zu >= 0x00100000u) { zu >>= 4; k += 4; }
	if (zu >= 0x00040000u) { zu >>= 2; k += 2; }
	if (zu >= 0x00020000u) { zu >>= 1; k += 1; }
	if (zu >= 0x00010000u) { k += 1; }
	zs = (uint32_t)z >> k;
	r = 0x80000000u / zs;
	sh = 15 + k;
	*tu = (int)(((int64_t)u * r) >> sh);
	*tv = (int)(((int64_t)v * r) >> sh);
}

/* u and v packed in one word, as Doom's span drawer does: u in bits
 * 31..16 and v in 15..0, each 6.10 fixed point (the texture is 64 x 64 and
 * wraps), so one add steps both; the texel is
 * ((pos >> 4) & 0xfc0) | (pos >> 26). */
static inline uint32_t pack_uv(int u, int v)
{
	return ((uint32_t)(u >> 6) << 16) | ((uint32_t)(v >> 6) & 0xffffu);
}

#define TEXEL(p) (((p) >> 4 & 0x0fc0u) | ((p) >> 26))

/* One scanline, lighted (the fade table, l in 8.8 levels) or not,
 * transparent (color 255 skipped) or not. */
static inline void scanline_per(int lighted, int transparent)
{
	unsigned char *dest = write_buffer + fx_xleft + bytes_per_row * fx_y;
	const unsigned char *tex = pixptr, *fade = gr_fade_table;
	int n = fx_xright - fx_xleft + 1;
	fix u = fx_u, v = fx_v, z = fx_z;
	const fix dudx = fx_du_dx, dvdx = fx_dv_dx, dzdx = fx_dz_dx;
	fix l = fx_l >> 8;
	const fix dldx = fx_dl_dx >> 8;
	int u0, v0;

	if (n <= 0)
		return;
	texel_coords(u, v, z, &u0, &v0);
	while (n > 0) {
		int k = n < SPAN ? n : SPAN, u1, v1, du, dv, i;
		uint32_t pos, step;
		u += dudx * k;
		v += dvdx * k;
		z += dzdx * k;
		texel_coords(u, v, z, &u1, &v1);
		if (k == SPAN) {
			du = (u1 - u0) >> SPAN_LOG;
			dv = (v1 - v0) >> SPAN_LOG;
		} else {
			du = (u1 - u0) / k;
			dv = (v1 - v0) / k;
		}
		pos = pack_uv(u0, v0);
		step = pack_uv(du, dv);
		i = k;
		if (!transparent) {
			/* four at a time, the loads first */
			for (; i >= 4; i -= 4) {
				unsigned a = tex[TEXEL(pos)], b = tex[TEXEL(pos + step)];
				unsigned c = tex[TEXEL(pos + 2 * step)], d = tex[TEXEL(pos + 3 * step)];
				pos += 4 * step;
				if (lighted) {
					unsigned la = l & 0x7f00, lb = (l + dldx) & 0x7f00;
					unsigned lc = (l + 2 * dldx) & 0x7f00, ld = (l + 3 * dldx) & 0x7f00;
					a = fade[la + a]; b = fade[lb + b]; c = fade[lc + c]; d = fade[ld + d];
					l += 4 * dldx;
				}
				dest[0] = a; dest[1] = b; dest[2] = c; dest[3] = d;
				dest += 4;
			}
		}
		for (; i > 0; i--) {
			unsigned c = tex[TEXEL(pos)];
			if (!transparent || c != TRANSPARENT)
				*dest = lighted ? fade[(l & 0x7f00) + c] : c;
			dest++;
			pos += step;
			if (lighted)
				l += dldx;
		}
		u0 = u1;
		v0 = v1;
		n -= k;
	}
}

#ifdef M9B_PROFILE
unsigned int m9b_tmap_cycles, m9b_tmap_calls, m9b_tmap_pixels;
#define TMAP_T0 unsigned int t0 = *(volatile unsigned int *)(M9B_TIMING + 0xCu); m9b_tmap_calls++; m9b_tmap_pixels += fx_xright - fx_xleft + 1;
#define TMAP_T1 m9b_tmap_cycles += *(volatile unsigned int *)(M9B_TIMING + 0xCu) - t0;
#else
#define TMAP_T0
#define TMAP_T1
#endif

void m9b_tmap_scanline_per(void)
{
	TMAP_T0
	if (Transparency_on)
		scanline_per(1, 1);
	else
		scanline_per(1, 0);
	TMAP_T1
}

void m9b_tmap_scanline_per_nolight(void)
{
	TMAP_T0
	if (Transparency_on)
		scanline_per(0, 1);
	else
		scanline_per(0, 0);
	TMAP_T1
}
