/* See LICENSE for licence details. */
#include <sys/types.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include "arg.h"
#include "drw.h"

char *argv0;

/* macros */
#define LEN(a)         (sizeof(a) / sizeof(a)[0])
#define LIMIT(x, a, b) (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define MAXFONTSTRLEN  128

typedef enum {
	NONE = 0,
	LOADED = 1,
	SCALED = 2,
	DRAWN = 4
} imgstate;

typedef struct {
	unsigned char *buf;
	unsigned int bufwidth, bufheight;
	imgstate state;
	XImage *ximg;
	int fd;
	int numpasses;
} Image;

typedef struct {
	char *regex;
	char *bin;
} Filter;

typedef struct {
	unsigned int linecount;
	char **lines;
	Image *img;
} Slide;

/* Purely graphic info */
typedef struct {
	Display *dpy;
	Window win;
	Atom wmdeletewin, netwmname;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int w, h;
	int uw, uh; /* usable dimensions for drawing text and images */
} XWindow;

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int b;
	void (*func)(const Arg *);
	const Arg arg;
} Mousekey;

typedef struct {
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

static Image *ffopen(char *filename);
static void fffree(Image *img);
static int ffread(Image *img);
static int ffprepare(Image *img);
static void ffscale(Image *img);
static void ffdraw(Image *img);

static void getfontsize(Slide *s, unsigned int *width, unsigned int *height);
static void cleanup();
static void eprintf(const char *, ...);
static void die(const char *, ...);
static void load(FILE *fp);
static void advance(const Arg *arg);
static void quit(const Arg *arg);
static void resize(int width, int height);
static void run();
static void usage();
static void xdraw();
static void xhints();
static void xinit();
static void xloadfonts();

static void bpress(XEvent *);
static void cmessage(XEvent *);
static void expose(XEvent *);
static void kpress(XEvent *);
static void configure(XEvent *);

/* config.h for applying patches and the configuration. */
#include "config.h"

/* Globals */
static Slide *slides = NULL;
static int idx = 0;
static int slidecount = 0;
static XWindow xw;
static Drw *d = NULL;
static Scm *sc;
static Fnt *fonts[NUMFONTSCALES];
static int running = 1;

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = bpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = configure,
	[Expose] = expose,
	[KeyPress] = kpress,
};

int
filter(int fd, const char *cmd)
{
	int fds[2];

	if (pipe(fds) < 0)
		eprintf("pipe:");

	switch (fork()) {
	case -1:
		eprintf("fork:");
	case 0:
		dup2(fd, 0);
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		execlp(cmd, cmd, (char *)0);
		eprintf("execlp %s:", cmd);
	}
	close(fds[1]);
	return fds[0];
}

Image *ffopen(char *filename)
{
	unsigned char hdr[16];
	char *bin;
	regex_t regex;
	Image *img;
	size_t i;
	int tmpfd, fd;

	for (bin = NULL, i = 0; i < LEN(filters); i++) {
		if (regcomp(&regex, filters[i].regex,
		            REG_NOSUB | REG_EXTENDED | REG_ICASE))
			continue;
		if (!regexec(&regex, filename, 0, NULL, 0)) {
			if (!(bin = filters[i].bin))
				return NULL;
			break;
		}
	}

	if ((fd = open(filename, O_RDONLY)) < 0) {
		eprintf("Unable to open file %s:", filename);
		return NULL;
	}

	tmpfd = fd;
	fd = filter(fd, bin);
	if (fd < 0)
		eprintf("Unable to filter %s:", filename);
	close(tmpfd);

	if (read(fd, hdr, 16) != 16)
		return NULL;

	if (memcmp("farbfeld", hdr, 8))
		return NULL;

	img = calloc(1, sizeof(Image));
	img->fd = fd;
	img->bufwidth = ntohl(*(uint32_t *)&hdr[8]);
	img->bufheight = ntohl(*(uint32_t *)&hdr[12]);

	return img;
}

void fffree(Image *img)
{
	free(img->buf);
	if (img->ximg)
		XDestroyImage(img->ximg);
	free(img);
}

int ffread(Image *img)
{
	uint32_t y, x;
	uint16_t *row;
	uint8_t opac;
	uint8_t fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;
	size_t rowlen, off, nbytes;
	ssize_t count;

	if (!img)
		return 0;

	if (img->state & LOADED)
		return 2;

	if (img->buf)
		free(img->buf);
	/* internally the image is stored in 888 format */
	if (!(img->buf = malloc(3 * img->bufwidth * img->bufheight)))
		return 0;

	/* scratch buffer to read row by row */
	rowlen = img->bufwidth * 2 * strlen("RGBA");
	row = malloc(rowlen);
	if (!row) {
		free(img->buf);
		img->buf = NULL;
		return 0;
	}

	/* extract window background color channels for transparency */
	bg_r = (sc->bg.pix >> 16) % 256;
	bg_g = (sc->bg.pix >>  8) % 256;
	bg_b = (sc->bg.pix >>  0) % 256;

	for (off = 0, y = 0; y < img->bufheight; y++) {
		nbytes = 0;
		while (nbytes < rowlen) {
			count = read(img->fd, (char *)row + nbytes, rowlen - nbytes);
			if (count < 0)
				eprintf("Unable to read from pipe:");
			nbytes += count;
		}
		for (x = 0; x < rowlen / 2; x += 4) {
			fg_r = ntohs(row[x + 0]) / 256;
			fg_g = ntohs(row[x + 1]) / 256;
			fg_b = ntohs(row[x + 2]) / 256;
			opac = ntohs(row[x + 3]) / 256;
			/* blend opaque part of image data with window background color to
			 * emulate transparency */
			img->buf[off++] = (fg_r * opac + bg_r * (255 - opac)) / 256;
			img->buf[off++] = (fg_g * opac + bg_g * (255 - opac)) / 256;
			img->buf[off++] = (fg_b * opac + bg_b * (255 - opac)) / 256;
		}
	}

	free(row);
	close(img->fd);
	img->state |= LOADED;

	return 1;
}

int ffprepare(Image *img)
{
	int depth = DefaultDepth(xw.dpy, xw.scr);
	int width = xw.uw;
	int height = xw.uh;

	if (xw.uw * img->bufheight > xw.uh * img->bufwidth)
		width = img->bufwidth * xw.uh / img->bufheight;
	else
		height = img->bufheight * xw.uw / img->bufwidth;

	if (depth < 24) {
		eprintf("Display depths <24 not supported.");
		return 0;
	}

	if (!(img->ximg = XCreateImage(xw.dpy, CopyFromParent, depth, ZPixmap, 0,
				NULL, width, height, 32, 0))) {
		eprintf("Unable to create XImage.");
		return 0;
	}

	if (!(img->ximg->data = malloc(img->ximg->bytes_per_line * height))) {
		eprintf("Unable to alloc data section for XImage.");
		XDestroyImage(img->ximg);
		img->ximg = NULL;
		return 0;
	}

	if (!XInitImage(img->ximg)) {
		eprintf("Unable to init XImage.");
		free(img->ximg->data);
		XDestroyImage(img->ximg);
		img->ximg = NULL;
		return 0;
	}

	ffscale(img);
	img->state |= SCALED;
	return 1;
}

void ffscale(Image *img)
{
	unsigned int x, y;
	unsigned int width = img->ximg->width;
	unsigned int height = img->ximg->height;
	char* newBuf = img->ximg->data;
	unsigned char* ibuf;
	unsigned int jdy = img->ximg->bytes_per_line / 4 - width;
	unsigned int dx = (img->bufwidth << 10) / width;

	for (y = 0; y < height; y++) {
		unsigned int bufx = img->bufwidth / width;
		ibuf = &img->buf[y * img->bufheight / height * img->bufwidth * 3];

		for (x = 0; x < width; x++) {
			*newBuf++ = (ibuf[(bufx >> 10)*3+2]);
			*newBuf++ = (ibuf[(bufx >> 10)*3+1]);
			*newBuf++ = (ibuf[(bufx >> 10)*3+0]);
			newBuf++;
			bufx += dx;
		}
		newBuf += jdy;
	}
}

void ffdraw(Image *img)
{
	int xoffset = (xw.w - img->ximg->width) / 2;
	int yoffset = (xw.h - img->ximg->height) / 2;
	XPutImage(xw.dpy, xw.win, d->gc, img->ximg, 0, 0,
	          xoffset, yoffset, img->ximg->width, img->ximg->height);
	XFlush(xw.dpy);
	img->state |= DRAWN;
}

void getfontsize(Slide *s, unsigned int *width, unsigned int *height)
{
	int i, j;
	unsigned int curw, imax;
	float lfac = linespacing * (s->linecount - 1) + 1;

	/* fit height */
	for (j = NUMFONTSCALES - 1; j >= 0; j--)
		if (fonts[j]->h * lfac <= xw.uh)
			break;
	LIMIT(j, 0, NUMFONTSCALES - 1);
	drw_setfontset(d, fonts[j]);

	/* fit width */
	*width = 0;
	for (i = 0; i < s->linecount; i++) {
		curw = drw_fontset_getwidth(d, s->lines[i]);
		if (curw >= *width)
			imax = i;
		while (j > 0 && curw > xw.uw) {
			drw_setfontset(d, fonts[--j]);
			curw = drw_fontset_getwidth(d, s->lines[i]);
		}
		if (imax == i)
			*width = curw;
	}
	*height = fonts[j]->h * lfac;
	*width += fonts[j]->h;
}

void cleanup()
{
	unsigned int i, j;

	for (i = 0; i < NUMFONTSCALES; i++)
		drw_fontset_free(fonts[i]);
	drw_scm_free(sc);
	drw_free(d);

	XDestroyWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);
	XCloseDisplay(xw.dpy);
	if (slides) {
		for (i = 0; i < slidecount; i++) {
			for (j = 0; j < slides[i].linecount; j++)
				free(slides[i].lines[j]);
			free(slides[i].lines);
			if (slides[i].img)
				fffree(slides[i].img);
		}
		free(slides);
		slides = NULL;
	}
}

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	eprintf(fmt, ap);
	va_end(ap);

	exit(1);
}

void eprintf(const char *fmt, ...)
{
	va_list ap;

	fputs("sent: ", stderr);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] != '\0' && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
}

void load(FILE *fp)
{
	static size_t size = 0;
	size_t blen, maxlines;
	char buf[BUFSIZ], *p;
	Slide *s;

	/* read each line from fp and add it to the item list */
	while (1) {
		/* eat consecutive empty lines */
		while ((p = fgets(buf, sizeof(buf), fp)))
			if (strcmp(buf, "\n") != 0 && buf[0] != '#')
				break;
		if (!p)
			break;

		if ((slidecount+1) * sizeof(*slides) >= size)
			if (!(slides = realloc(slides, (size += BUFSIZ))))
				die("Unable to realloc %u bytes:", size);

		/* read one slide */
		maxlines = 0;
		memset((s = &slides[slidecount]), 0, sizeof(Slide));
		do {
			if (buf[0] == '#')
				continue;

			/* grow lines array */
			if (s->linecount >= maxlines) {
				maxlines = 2 * s->linecount + 1;
				if (!(s->lines = realloc(s->lines, maxlines * sizeof(s->lines[0]))))
					die("Unable to realloc %u bytes:", maxlines * sizeof(s->lines[0]));
			}

			blen = strlen(buf);
			if (!(s->lines[s->linecount] = strdup(buf)))
				die("Unable to strdup:");
			if (s->lines[s->linecount][blen-1] == '\n')
				s->lines[s->linecount][blen-1] = '\0';

			/* only make image slide if first line of a slide starts with @ */
			if (s->linecount == 0 && s->lines[0][0] == '@') {
				memmove(s->lines[0], &s->lines[0][1], blen);
				s->img = ffopen(s->lines[0]);
			}

			if (s->lines[s->linecount][0] == '\\')
				memmove(s->lines[s->linecount], &s->lines[s->linecount][1], blen);
			s->linecount++;
		} while ((p = fgets(buf, sizeof(buf), fp)) && strcmp(buf, "\n") != 0);
		slidecount++;
		if (!p)
			break;
	}
}

void advance(const Arg *arg)
{
	int new_idx = idx + arg->i;
	LIMIT(new_idx, 0, slidecount-1);
	if (new_idx != idx) {
		if (slides[idx].img)
			slides[idx].img->state &= ~(DRAWN | SCALED);
		idx = new_idx;
		xdraw();
		if (slidecount > idx + 1 && slides[idx + 1].img && !ffread(slides[idx + 1].img))
			die("Unable to read image %s", slides[idx + 1].lines[0]);
		if (0 < idx && slides[idx - 1].img && !ffread(slides[idx - 1].img))
			die("Unable to read image %s", slides[idx - 1].lines[0]);
	}
}

void quit(const Arg *arg)
{
	running = 0;
}

void resize(int width, int height)
{
	xw.w = width;
	xw.h = height;
	xw.uw = usablewidth * width;
	xw.uh = usableheight * height;
	drw_resize(d, width, height);
}

void run()
{
	XEvent ev;

	/* Waiting for window mapping */
	while (1) {
		XNextEvent(xw.dpy, &ev);
		if (ev.type == ConfigureNotify) {
			resize(ev.xconfigure.width, ev.xconfigure.height);
		} else if (ev.type == MapNotify) {
			break;
		}
	}

	while (running) {
		XNextEvent(xw.dpy, &ev);
		if (handler[ev.type])
			(handler[ev.type])(&ev);
	}
}

void usage()
{
	die("sent " VERSION " (c) 2014-2015 markus.teich@stusta.mhn.de\n" \
	"usage: sent FILE1 [FILE2 ...]", argv0);
}

void xdraw()
{
	unsigned int height, width, i;
	Image *im = slides[idx].img;

	getfontsize(&slides[idx], &width, &height);
	XClearWindow(xw.dpy, xw.win);

	if (!im) {
		drw_rect(d, 0, 0, xw.w, xw.h, 1, 1);
		for (i = 0; i < slides[idx].linecount; i++)
			drw_text(d,
			         (xw.w - width) / 2,
			         (xw.h - height) / 2 + i * linespacing * d->fonts->h,
			         width,
			         d->fonts->h,
			         slides[idx].lines[i],
			         0);
		drw_map(d, xw.win, 0, 0, xw.w, xw.h);
	} else if (!(im->state & LOADED) && !ffread(im)) {
		eprintf("Unable to read image %s", slides[idx].lines[0]);
	} else if (!(im->state & SCALED) && !ffprepare(im)) {
		eprintf("Unable to prepare image %s for drawing", slides[idx].lines[0]);
	} else if (!(im->state & DRAWN)) {
		ffdraw(im);
	}
}

void xhints()
{
	XClassHint class = {.res_name = "sent", .res_class = "presenter"};
	XWMHints wm = {.flags = InputHint, .input = True};
	XSizeHints *sizeh = NULL;

	if (!(sizeh = XAllocSizeHints()))
		die("Unable to alloc size hints.");

	sizeh->flags = PSize;
	sizeh->height = xw.h;
	sizeh->width = xw.w;

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
	XFree(sizeh);
}

void xinit()
{
	XTextProperty prop;

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("Unable to open display.");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);
	resize(DisplayWidth(xw.dpy, xw.scr), DisplayHeight(xw.dpy, xw.scr));

	xw.attrs.bit_gravity = CenterGravity;
	xw.attrs.event_mask = KeyPressMask | ExposureMask | StructureNotifyMask |
	                      ButtonMotionMask | ButtonPressMask;

	xw.win = XCreateWindow(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, 0,
	                       xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr),
	                       InputOutput, xw.vis, CWBitGravity | CWEventMask,
	                       &xw.attrs);

	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	if (!(d = drw_create(xw.dpy, xw.scr, xw.win, xw.w, xw.h)))
		die("Unable to create drawing context.");
	sc = drw_scm_create(d, fgcol, bgcol);
	drw_setscheme(d, sc);
	XSetWindowBackground(xw.dpy, xw.win, sc->bg.pix);

	xloadfonts();

	XStringListToTextProperty(&argv0, 1, &prop);
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, False);
}

void xloadfonts()
{
	int i, j;
	char *fstrs[LEN(fontfallbacks)];

	for (j = 0; j < LEN(fontfallbacks); j++) {
		if (!(fstrs[j] = malloc(MAXFONTSTRLEN)))
			die("Unable to malloc fstrs.");
	}

	for (i = 0; i < NUMFONTSCALES; i++) {
		for (j = 0; j < LEN(fontfallbacks); j++) {
			if (MAXFONTSTRLEN < snprintf(fstrs[j], MAXFONTSTRLEN, "%s:size=%d", fontfallbacks[j], FONTSZ(i)))
				die("Font string too long.");
		}
		if (!(fonts[i] = drw_fontset_create(d, (const char**)fstrs, LEN(fstrs))))
			die("Unable to load any font for size %d.", FONTSZ(i));
	}

	for (j = 0; j < LEN(fontfallbacks); j++)
		if (fstrs[j])
			free(fstrs[j]);
}

void bpress(XEvent *e)
{
	unsigned int i;

	for (i = 0; i < LEN(mshortcuts); i++)
		if (e->xbutton.button == mshortcuts[i].b && mshortcuts[i].func)
			mshortcuts[i].func(&(mshortcuts[i].arg));
}

void cmessage(XEvent *e)
{
	if (e->xclient.data.l[0] == xw.wmdeletewin)
		running = 0;
}

void expose(XEvent *e)
{
	if (0 == e->xexpose.count)
		xdraw();
}

void kpress(XEvent *e)
{
	unsigned int i;
	KeySym sym;

	sym = XkbKeycodeToKeysym(xw.dpy, (KeyCode)e->xkey.keycode, 0, 0);
	for (i = 0; i < LEN(shortcuts); i++)
		if (sym == shortcuts[i].keysym && shortcuts[i].func)
			shortcuts[i].func(&(shortcuts[i].arg));
}

void configure(XEvent *e)
{
	resize(e->xconfigure.width, e->xconfigure.height);
	if (slides[idx].img)
		slides[idx].img->state &= ~(DRAWN | SCALED);
	xdraw();
}

int main(int argc, char *argv[])
{
	int i;
	FILE *fp = NULL;

	ARGBEGIN {
	case 'v':
	default:
		usage();
	} ARGEND;

	for (i = 0; i < argc; i++) {
		if ((fp = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin)) {
			load(fp);
			fclose(fp);
		} else {
			eprintf("Unable to open '%s' for reading:", argv[i]);
		}
	}

	if (!slidecount)
		usage();

	xinit();
	run();

	cleanup();
	return 0;
}
