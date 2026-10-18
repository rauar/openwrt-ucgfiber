// SPDX-License-Identifier: GPL-2.0-only
/*
 * ucgfiber-lcd: show the WAN download and upload rate on the front panel
 * display of the Ubiquiti UCG-Fiber.
 *
 * The panel is a DRM device (panel-mipi-dbi). The program draws into a dumb
 * buffer with plain DRM ioctls, so it needs no library. Two rows, each with
 * an arrow, the rate of the last second and a bar that is filled by the rate
 * relative to the fastest known WAN link speed.
 *
 * With -o FILE the frame is written to FILE (raw RGB565, 160x80) instead of
 * the display; this is for testing on a host.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

#define COL_BG		RGB(8, 16, 100)		/* deep blue, as on stock */
#define COL_TEXT	RGB(255, 255, 255)
#define COL_FRAME	RGB(255, 255, 255)
#define COL_DOWN	RGB(40, 80, 225)
#define COL_UP		RGB(0, 150, 235)

#define DEFAULT_MAX_MBIT	1000

struct screen {
	uint16_t *px;
	unsigned int w, h, pitch;	/* pitch in pixels */
};

struct drm_out {
	int fd;
	uint32_t fb_id;
	size_t size;
	struct screen scr;
};

static volatile sig_atomic_t stop;

/*
 * Font at native size, as on stock: digits and capitals 7 pixels wide and 10
 * high. A glyph is up to 12 rows of '#' and '.'; the width is the length of
 * the first row. Row 9 is the baseline, 'p' reaches down to row 11.
 */
struct glyph {
	char c;
	const char *row[12];
};

static const struct glyph font[] = {
	{ '0', { ".#####.", "##...##", "##...##", "##...##", "##...##",
		 "##...##", "##...##", "##...##", "##...##", ".#####." } },
	{ '1', { "...##..", "..###..", ".####..", "...##..", "...##..",
		 "...##..", "...##..", "...##..", "...##..", ".######" } },
	{ '2', { ".#####.", "##...##", ".....##", ".....##", "....##.",
		 "...##..", "..##...", ".##....", "##.....", "#######" } },
	{ '3', { ".#####.", "##...##", ".....##", ".....##", "..####.",
		 ".....##", ".....##", ".....##", "##...##", ".#####." } },
	{ '4', { "....##.", "...###.", "..####.", ".##.##.", "##..##.",
		 "#######", "....##.", "....##.", "....##.", "....##." } },
	{ '5', { "#######", "##.....", "##.....", "##.....", "######.",
		 ".....##", ".....##", ".....##", "##...##", ".#####." } },
	{ '6', { "..####.", ".##....", "##.....", "##.....", "######.",
		 "##...##", "##...##", "##...##", "##...##", ".#####." } },
	{ '7', { "#######", ".....##", ".....##", "....##.", "....##.",
		 "...##..", "...##..", "..##...", "..##...", "..##..." } },
	{ '8', { ".#####.", "##...##", "##...##", "##...##", ".#####.",
		 "##...##", "##...##", "##...##", "##...##", ".#####." } },
	{ '9', { ".#####.", "##...##", "##...##", "##...##", ".######",
		 ".....##", ".....##", ".....##", "....##.", ".####.." } },
	{ '.', { "..", "..", "..", "..", "..", "..", "..", "..", "##", "##" } },
	{ '-', { ".....", ".....", ".....", ".....", "#####", "#####" } },
	{ ' ', { "..." } },
	{ 'K', { "##...##", "##..##.", "##.##..", "####...", "###....",
		 "####...", "##.##..", "##..##.", "##...##", "##...##" } },
	{ 'M', { "##...##", "###.###", "#######", "#######", "##.#.##",
		 "##...##", "##...##", "##...##", "##...##", "##...##" } },
	{ 'G', { ".#####.", "##...##", "##.....", "##.....", "##.####",
		 "##...##", "##...##", "##...##", "##...##", ".#####." } },
	{ 'b', { "##.....", "##.....", "##.....", "##.###.", "###..##",
		 "##...##", "##...##", "##...##", "###..##", "##.###." } },
	{ 'p', { ".......", ".......", ".......", "##.###.", "###..##",
		 "##...##", "##...##", "##...##", "###..##", "##.###.",
		 "##.....", "##....." } },
	{ 's', { ".......", ".......", ".......", ".#####.", "##...##",
		 "##.....", ".#####.", ".....##", "##...##", ".#####." } },
	{ '^', { ".........", ".........", ".........", "....#....", "...###...",
		 "..#####..", ".#######.", "#########", ".........", "........." } },	/* up triangle */
	{ 'v', { ".........", ".........", ".........", "#########", ".#######.",
		 "..#####..", "...###...", "....#....", ".........", "........." } },	/* down triangle */
};

static void fill_rect(struct screen *s, int x, int y, int w, int h, uint16_t c)
{
	int i, j;

	for (j = y; j < y + h; j++) {
		if (j < 0 || j >= (int)s->h)
			continue;
		for (i = x; i < x + w; i++)
			if (i >= 0 && i < (int)s->w)
				s->px[j * s->pitch + i] = c;
	}
}

/* draw a glyph, return the advance to the next one (width and a 1 pixel gap) */
static int put_glyph(struct screen *s, int x, int y, char c, uint16_t col)
{
	unsigned int n, r, b;

	for (n = 0; n < sizeof(font) / sizeof(font[0]); n++) {
		if (font[n].c != c)
			continue;
		for (r = 0; r < 12 && font[n].row[r]; r++)
			for (b = 0; font[n].row[r][b]; b++)
				if (font[n].row[r][b] == '#')
					fill_rect(s, x + b, y + r, 1, 1, col);
		return (int)strlen(font[n].row[0]) + 1;
	}
	return 0;
}

static void put_text(struct screen *s, int x, int y, const char *t, uint16_t col)
{
	for (; *t; t++)
		x += put_glyph(s, x, y, *t, col);
}

static int text_width(const char *t)
{
	unsigned int n;
	int w = 0;

	for (; *t; t++)
		for (n = 0; n < sizeof(font) / sizeof(font[0]); n++)
			if (font[n].c == *t)
				w += (int)strlen(font[n].row[0]) + 1;
	return w ? w - 1 : 0;
}

/* rate as value and unit: 0 bps, 999 bps, 1.50 Kbps, 12.3 Mbps, 940 Mbps, 1.50 Gbps */
static void fmt_rate(double bps, char *val, size_t vn, char *unit, size_t un)
{
	static const char * const names[] = { "bps", "Kbps", "Mbps", "Gbps" };
	int u = 0;
	double v = bps;

	while (v >= 999.5 && u < 3) {
		v /= 1000;
		u++;
	}
	if (u == 0)
		snprintf(val, vn, "%.0f", v);
	else if (v < 9.995)
		snprintf(val, vn, "%.2f", v);
	else if (v < 99.95)
		snprintf(val, vn, "%.1f", v);
	else
		snprintf(val, vn, "%.0f", v);
	snprintf(unit, un, "%s", names[u]);
}

static int in_rrect(int x, int y, int rx, int ry, int rw, int rh, int r)
{
	int cx, cy, dx, dy;

	if (x < rx || x >= rx + rw || y < ry || y >= ry + rh)
		return 0;
	cx = x < rx + r ? rx + r : (x >= rx + rw - r ? rx + rw - 1 - r : x);
	cy = y < ry + r ? ry + r : (y >= ry + rh - r ? ry + rh - 1 - r : y);
	dx = x - cx;
	dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

/*
 * One bar, 90 percent of the screen width: white frame, fill in the colour
 * of the KPI up to the fraction. Triangle at the left; the unit is right
 * aligned at the end of the bar in a field as wide as "Mbps", the value is
 * right aligned in front of it, so nothing moves when the numbers change.
 */
static void draw_bar(struct screen *s, int row, char mark, const char *val,
		     const char *unit, double frac, uint16_t col)
{
	int bx = s->w / 20, bw = s->w - 2 * bx;
	int bh = 24, r = 10, gap = 11, by = (s->h - (2 * bh + gap)) / 2 + row * (bh + gap);
	int fx = bx + 3, fw = bw - 6, fill, x, y;
	int ty = by + (bh - 10) / 2, rx = bx + bw - 12;

	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	fill = (int)(fw * frac);
	if (frac > 0 && fill < 1)
		fill = 1;	/* any traffic shows */

	for (y = by; y < by + bh; y++) {
		for (x = bx; x < bx + bw; x++) {
			uint16_t c;

			if (in_rrect(x, y, bx, by, bw, bh, r) &&
			    !in_rrect(x, y, bx + 1, by + 1, bw - 2, bh - 2, r - 1))
				c = COL_FRAME;
			else if (x < fx + fill &&
				 in_rrect(x, y, bx + 3, by + 3, bw - 6, bh - 6, r - 3))
				c = col;
			else
				continue;
			s->px[y * s->pitch + x] = c;
		}
	}
	put_glyph(s, bx + 9, ty, mark, COL_TEXT);
	put_text(s, rx - text_width(unit), ty, unit, COL_TEXT);
	put_text(s, rx - text_width("Mbps") - 5 - text_width(val), ty, val, COL_TEXT);
}

static void draw_frame(struct screen *s, int valid, double down, double up, double max_bps)
{
	char v[16], u[8];

	fill_rect(s, 0, 0, s->w, s->h, COL_BG);
	if (valid) {
		fmt_rate(down, v, sizeof(v), u, sizeof(u));
	} else {
		strcpy(v, "-");
		u[0] = 0;
	}
	draw_bar(s, 0, 'v', v, u, valid ? down / max_bps : 0, COL_DOWN);
	if (valid) {
		fmt_rate(up, v, sizeof(v), u, sizeof(u));
	} else {
		strcpy(v, "-");
		u[0] = 0;
	}
	draw_bar(s, 1, '^', v, u, valid ? up / max_bps : 0, COL_UP);
}

/* ---- WAN counters and link speed ---- */

static int read_u64(const char *path, uint64_t *v)
{
	char b[32];
	FILE *f = fopen(path, "r");
	int ok;

	if (!f)
		return -1;
	ok = fgets(b, sizeof(b), f) != NULL;
	fclose(f);
	if (!ok)
		return -1;
	errno = 0;
	*v = strtoull(b, NULL, 10);
	return errno ? -1 : 0;
}

static int read_counters(const char *cls, const char *dev, uint64_t *rx, uint64_t *tx)
{
	char p[256];

	snprintf(p, sizeof(p), "%s/%s/statistics/rx_bytes", cls, dev);
	if (read_u64(p, rx))
		return -1;
	snprintf(p, sizeof(p), "%s/%s/statistics/tx_bytes", cls, dev);
	return read_u64(p, tx);
}

static double rate_bps(uint64_t cur, uint64_t prev, double dt)
{
	return (cur >= prev && dt > 0) ? (double)(cur - prev) * 8.0 / dt : 0;
}

/* fastest link of the device and of its bridge ports, in bit/s; 0 if unknown */
static double link_speed_bps(const char *cls, const char *dev)
{
	char p[640], dir[256];
	struct dirent *e;
	uint64_t v, best = 0;
	DIR *d;

	snprintf(p, sizeof(p), "%s/%s/speed", cls, dev);
	if (!read_u64(p, &v) && v < 1000000)
		best = v;
	snprintf(dir, sizeof(dir), "%s/%s", cls, dev);
	d = opendir(dir);
	if (!d)
		return best * 1e6;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "lower_", 6))
			continue;
		snprintf(p, sizeof(p), "%s/%s/speed", dir, e->d_name);
		if (!read_u64(p, &v) && v < 1000000 && v > best)
			best = v;
	}
	closedir(d);
	return best * 1e6;
}

/* ---- output: DRM dumb buffer ---- */

static int drm_open_panel(const char *path, struct drm_out *o)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector con = { 0 };
	struct drm_mode_get_encoder enc = { 0 };
	struct drm_mode_create_dumb cd = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_fb_cmd2 fb = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	struct drm_mode_modeinfo *modes = NULL;
	uint32_t *crtcs = NULL, *conns = NULL, *encs = NULL, conn_id, crtc_id = 0;
	void *map;
	int fd, i;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		goto fail;
	if (!res.count_crtcs || !res.count_connectors)
		goto fail;
	crtcs = calloc(res.count_crtcs, sizeof(*crtcs));
	conns = calloc(res.count_connectors, sizeof(*conns));
	if (!crtcs || !conns)
		goto fail;
	res.count_fbs = 0;
	res.count_encoders = 0;
	res.fb_id_ptr = res.encoder_id_ptr = 0;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		goto fail;

	/* first connector that has a mode */
	for (i = 0; i < (int)res.count_connectors; i++) {
		memset(&con, 0, sizeof(con));
		con.connector_id = conns[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0 || !con.count_modes)
			continue;
		modes = calloc(con.count_modes, sizeof(*modes));
		encs = calloc(con.count_encoders ? con.count_encoders : 1, sizeof(*encs));
		if (!modes || !encs)
			goto fail;
		con.count_props = 0;
		con.props_ptr = con.prop_values_ptr = 0;
		con.modes_ptr = (uintptr_t)modes;
		con.encoders_ptr = (uintptr_t)encs;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0 || !con.count_modes)
			goto fail;
		break;
	}
	if (!modes)
		goto fail;
	conn_id = conns[i];

	/* CRTC: the one the encoder uses now, else the first it can use */
	if (con.encoder_id) {
		enc.encoder_id = con.encoder_id;
	} else if (con.count_encoders) {
		enc.encoder_id = encs[0];
	}
	if (enc.encoder_id && ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
		crtc_id = enc.crtc_id;
		for (i = 0; !crtc_id && i < (int)res.count_crtcs; i++)
			if (enc.possible_crtcs & (1u << i))
				crtc_id = crtcs[i];
	}
	if (!crtc_id)
		crtc_id = crtcs[0];

	cd.width = modes[0].hdisplay;
	cd.height = modes[0].vdisplay;
	cd.bpp = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		goto fail;
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		goto fail;
	map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (map == MAP_FAILED)
		goto fail;

	fb.width = cd.width;
	fb.height = cd.height;
	fb.pixel_format = DRM_FORMAT_RGB565;
	fb.handles[0] = cd.handle;
	fb.pitches[0] = cd.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
		goto fail;

	crtc.crtc_id = crtc_id;
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = modes[0];
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
		goto fail;

	o->fd = fd;
	o->fb_id = fb.fb_id;
	o->size = cd.size;
	o->scr.px = map;
	o->scr.w = cd.width;
	o->scr.h = cd.height;
	o->scr.pitch = cd.pitch / 2;
	free(crtcs);
	free(conns);
	free(encs);
	free(modes);
	return 0;
fail:
	free(crtcs);
	free(conns);
	free(encs);
	free(modes);
	close(fd);
	return -1;
}

static void drm_present(struct drm_out *o)
{
	struct drm_mode_fb_dirty_cmd d = { .fb_id = o->fb_id };

	if (ioctl(o->fd, DRM_IOCTL_MODE_DIRTYFB, &d) < 0 && errno != ENOSYS) {
		static int warned;

		if (!warned++)
			fprintf(stderr, "ucgfiber-lcd: DIRTYFB: %s\n", strerror(errno));
	}
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ucgfiber-lcd [-d DEV] [-m MBIT] [-D DRMDEV] [-C CLASSDIR]\n"
		"                    [-o FILE] [-n FRAMES] [-r DOWN,UP]\n"
		"  -d DEV      WAN network device (default br-wan)\n"
		"  -m MBIT     rate for a full bar in Mbit/s (default: fastest WAN link, else %d)\n"
		"  -D DRMDEV   DRM device (default /dev/dri/card0)\n"
		"  -C CLASSDIR network class directory (default /sys/class/net)\n"
		"  -o FILE     write the frame (raw RGB565 160x80) to FILE, not the display\n"
		"  -n FRAMES   stop after FRAMES frames (default: run until stopped)\n"
		"  -r DOWN,UP  show fixed rates in bit/s instead of reading counters\n",
		DEFAULT_MAX_MBIT);
}

int main(int argc, char **argv)
{
	const char *dev = "br-wan", *drm = "/dev/dri/card0", *cls = "/sys/class/net";
	const char *outfile = NULL;
	double fixed_down = -1, fixed_up = -1, max_bps = 0;
	unsigned long frames = 0, n = 0;
	struct drm_out out = { .fd = -1 };
	struct screen file_scr = { 0 };
	struct screen *scr;
	struct timespec next, now, prev_t = { 0 };
	uint64_t prx = 0, ptx = 0;
	int have_prev = 0, c, warned = 0;

	while ((c = getopt(argc, argv, "d:m:D:C:o:n:r:h")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'm': max_bps = atof(optarg) * 1e6; break;
		case 'D': drm = optarg; break;
		case 'C': cls = optarg; break;
		case 'o': outfile = optarg; break;
		case 'n': frames = strtoul(optarg, NULL, 10); break;
		case 'r':
			if (sscanf(optarg, "%lf,%lf", &fixed_down, &fixed_up) != 2) {
				usage();
				return 2;
			}
			break;
		default:
			usage();
			return c == 'h' ? 0 : 2;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	if (outfile) {
		file_scr.w = 160;
		file_scr.h = 80;
		file_scr.pitch = 160;
		file_scr.px = calloc(160 * 80, sizeof(uint16_t));
		if (!file_scr.px)
			return 1;
		scr = &file_scr;
	} else {
		while (drm_open_panel(drm, &out) < 0) {
			if (stop)
				return 1;
			if (!warned++)
				fprintf(stderr, "ucgfiber-lcd: waiting for %s\n", drm);
			sleep(2);
		}
		scr = &out.scr;
	}

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!stop) {
		double down = 0, up = 0, dt, speed, mx;
		uint64_t rx, tx;
		int valid = 0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (fixed_down >= 0) {
			down = fixed_down;
			up = fixed_up;
			valid = 1;
		} else if (!read_counters(cls, dev, &rx, &tx)) {
			dt = (now.tv_sec - prev_t.tv_sec) + (now.tv_nsec - prev_t.tv_nsec) / 1e9;
			if (have_prev) {
				down = rate_bps(rx, prx, dt);
				up = rate_bps(tx, ptx, dt);
				valid = 1;
			}
			prx = rx;
			ptx = tx;
			prev_t = now;
			have_prev = 1;
		} else {
			have_prev = 0;
		}

		mx = max_bps;
		if (mx <= 0 && (speed = link_speed_bps(cls, dev)) > 0)
			mx = speed;
		if (mx <= 0)
			mx = DEFAULT_MAX_MBIT * 1e6;

		draw_frame(scr, valid, down, up, mx);
		if (outfile) {
			FILE *f = fopen(outfile, "wb");

			if (!f || fwrite(scr->px, sizeof(uint16_t), 160 * 80, f) != 160 * 80)
				return 1;
			fclose(f);
		} else {
			drm_present(&out);
		}

		if (frames && ++n >= frames)
			break;
		next.tv_sec++;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return 0;
}
