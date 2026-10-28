/* Descent on the m9b SoC: don't texture-map what the cockpit covers.
 *
 * In the full cockpit (and the rear view) the 3D view is the top 133 lines
 * of the screen, and the cockpit bitmap, drawn over it every frame, covers
 * about a quarter of them: the pillars down both sides and the dashboard's
 * top. While the view is rendered (game_do_render_frame), m9b_mask_set()
 * takes the cockpit's transparent runs per row, and every scanline the
 * texture mappers draw (texmap/ntmap.c, tmapflat.c, through SCANLINE) is
 * cut to them: each visible piece is drawn with the interpolants (u, v, z,
 * light, all linear across the scanline) advanced to its start.
 *
 * The runs are found once per bitmap (cached on its data and a sample of
 * its pixels, as bitblt.c's span lists) and only used while the view is
 * drawn into the top-left of the screen.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gr.h"
#include "texmap.h"
#include "texmapl.h"

#define MAX_ROWS  240
#define MAX_RUNS  6

static struct {
	const unsigned char *data;
	int w, h;
	unsigned int sample;
	unsigned char n[MAX_ROWS];                /* visible runs per row */
	unsigned short run[MAX_ROWS][MAX_RUNS][2]; /* first, last x */
} mask;
static int mask_active;
#ifdef M9B_PROFILE
unsigned int m9b_mask_in, m9b_mask_out, m9b_mask_sets;   // pixels asked for, drawn; frames masked
#endif

static unsigned int sample_of(grs_bitmap *bm)
{
	unsigned int h = 0, i, n = bm->bm_rowsize * bm->bm_h;
	for (i = 0; i < 32; i++)
		h = h * 31 + bm->bm_data[(n / 32) * i + i];
	return h;
}

/* Makes the transparent (255) runs of bm the visible parts of the rows.
 * A row with too many runs (not the case for Descent's cockpits) is left
 * whole. */
static void build(grs_bitmap *bm, unsigned int sample)
{
	int y, x;
	for (y = 0; y < bm->bm_h && y < MAX_ROWS; y++) {
		const unsigned char *row = bm->bm_data + y * bm->bm_rowsize;
		int n = 0;
		x = 0;
		while (x < bm->bm_w) {
			int x0;
			while (x < bm->bm_w && row[x] != 255) x++;
			x0 = x;
			while (x < bm->bm_w && row[x] == 255) x++;
			if (x > x0) {
				if (n == MAX_RUNS) { n = 1; mask.run[y][0][0] = 0; mask.run[y][0][1] = bm->bm_w - 1; break; }
				mask.run[y][n][0] = x0;
				mask.run[y][n][1] = x - 1;
				n++;
			}
		}
		mask.n[y] = n;
	}
	mask.data = bm->bm_data;
	mask.w = bm->bm_w;
	mask.h = bm->bm_h < MAX_ROWS ? bm->bm_h : MAX_ROWS;
	mask.sample = sample;
}

void m9b_mask_set(grs_bitmap *bm)
{
	unsigned int sample;
	mask_active = 0;
#ifdef M9B_NOMASK
	return;
#endif
	if (!bm || !bm->bm_data || bm->bm_type != BM_LINEAR || (bm->bm_flags & BM_FLAG_RLE))
		return;
	/* the view must be drawn from the screen's top-left, as the cockpit */
	if (grd_curcanv->cv_bitmap.bm_data != grd_curscreen->sc_canvas.cv_bitmap.bm_data)
		return;
	sample = sample_of(bm);
	if (mask.data != bm->bm_data || mask.w != bm->bm_w || mask.sample != sample)
		build(bm, sample);
	mask_active = 1;
#ifdef M9B_PROFILE
	m9b_mask_sets++;
#endif
}

void m9b_mask_clear(void)
{
	mask_active = 0;
}

/* Draws the current scanline (the fx_* globals) with fn, only where the
 * mask shows it. */
void m9b_scanline(void (*fn)(void))
{
	int y = fx_y, xl, xr, i, n;
	fix u, v, z, l;

	if (!mask_active || y < 0 || y >= mask.h) {
		fn();
		return;
	}
#ifdef M9B_PROFILE
	m9b_mask_in += fx_xright - fx_xleft + 1;
#endif
	n = mask.n[y];
	xl = fx_xleft;
	xr = fx_xright;
	/* the common cases: all in one visible run, or all hidden */
	for (i = 0; i < n; i++) {
		int a = mask.run[y][i][0], b = mask.run[y][i][1];
		if (a <= xl && b >= xr) {
#ifdef M9B_PROFILE
			m9b_mask_out += xr - xl + 1;
#endif
			fn();
			return;
		}
		if (b >= xl && a <= xr)
			break;   /* partly visible: cut below */
	}
	if (i == n)
		return;
	u = fx_u; v = fx_v; z = fx_z; l = fx_l;
	for (i = 0; i < n; i++) {
		int a = mask.run[y][i][0], b = mask.run[y][i][1], k;
		if (a < xl) a = xl;
		if (b > xr) b = xr;
		if (a > b)
			continue;
		k = a - xl;
		fx_xleft = a;
		fx_xright = b;
		fx_u = u + fx_du_dx * k;
		fx_v = v + fx_dv_dx * k;
		fx_z = z + fx_dz_dx * k;
		fx_l = l + fx_dl_dx * k;
#ifdef M9B_PROFILE
		m9b_mask_out += b - a + 1;
#endif
		fn();
	}
	fx_xleft = xl;
	fx_xright = xr;
	fx_u = u; fx_v = v; fx_z = z; fx_l = l;
}
