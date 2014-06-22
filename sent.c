/* See LICENSE for licence details. */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "arg.h"

char *argv0;

/* macros */
#define LEN(a)     (sizeof(a) / sizeof(a)[0])
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)

typedef struct {
	char* text;
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
} XWindow;

/* Drawing Context linked list*/
struct DC{
	XFontStruct *font;
	GC gc;
	struct DC *next;
};

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

/* function definitions used in config.h */
static void advance(const Arg *);
static void quit(const Arg *);

/* config.h for applying patches and the configuration. */
#include "config.h"

static Bool xfontisscalable(char *name);
static XFontStruct *xloadqueryscalablefont(char *name, int size);
static struct DC *getfontsize(char *str, size_t len, int *width, int *height);
static void cleanup(struct DC *cur);
static void eprintf(const char *, ...);
static void load(FILE *fp);
static void advance(const Arg *arg);
static void quit(const Arg *arg);
static void run();
static void usage();
static void xdraw();
static void xhints();
static void xinit();
static void xloadfonts(char *);

static void bpress(XEvent *);
static void cmessage(XEvent *);
static void expose(XEvent *);
static void kpress(XEvent *);
static void resize(XEvent *);

/* Globals */
static Slide *slides = NULL;
static int idx = 0;
static int slidecount = 0;
static XWindow xw;
static struct DC dc;
static int running = 1;
static char *opt_font = NULL;

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = bpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[Expose] = expose,
	[KeyPress] = kpress,
};


Bool xfontisscalable(char *name)
{
	int i, field;

	if (!name || name[0] != '-')
		return False;

	for (i = field = 0; name[i] != '\0'; i++) {
		if (name[i] == '-') {
			field++;
			if ((field == 7) || (field == 8) || (field == 12))
				if ((name[i+1] != '0') || (name[i+2] != '-'))
					return False;
		}
	}
	return field == 14;
}

XFontStruct *xloadqueryscalablefont(char *name, int size)
{
	int i, j, field;
	char newname[500];
	int resx, resy;

	if (!name || name[0] != '-')
		return NULL;
	/* calculate our screen resolution in dots per inch. 25.4mm = 1 inch */
	resx = DisplayWidth(xw.dpy, xw.scr)/(DisplayWidthMM(xw.dpy, xw.scr)/25.4);
	resy = DisplayHeight(xw.dpy, xw.scr)/(DisplayHeightMM(xw.dpy, xw.scr)/25.4);
	/* copy the font name, changing the scalable fields as we do so */
	for (i = j = field = 0; name[i] != '\0' && field <= 14; i++) {
		newname[j++] = name[i];
		if (name[i] == '-') {
			field++;
			switch (field) {
				case 7:  /* pixel size */
				case 12: /* average width */
					/* change from "-0-" to "-*-" */
					newname[j] = '*';
					j++;
					if (name[i+1] != '\0') i++;
					break;
				case 8:  /* point size */
					/* change from "-0-" to "-<size>-" */
					sprintf(&newname[j], "%d", size);
					while (newname[j] != '\0') j++;
					if (name[i+1] != '\0') i++;
					break;
				case 9:  /* x-resolution */
				case 10: /* y-resolution */
					/* change from an unspecified resolution to resx or resy */
					sprintf(&newname[j], "%d", (field == 9) ? resx : resy);
					while (newname[j] != '\0') j++;
					while ((name[i+1] != '-') && (name[i+1] != '\0')) i++;
					break;
			}
		}
	}
	newname[j] = '\0';
	return (field != 14) ? NULL : XLoadQueryFont(xw.dpy, newname);
}

struct DC *getfontsize(char *str, size_t len, int *width, int *height)
{
	XCharStruct info;
	int unused;
	struct DC *pre = &dc;
	struct DC *cur = &dc;

	do {
		XTextExtents(cur->font, str, len, &unused, &unused, &unused, &info);
		if (info.width > usablewidth * xw.w
				|| info.ascent + info.descent > usableheight * xw.h)
			break;
		pre = cur;
	} while ((cur = cur->next));

	XTextExtents(pre->font, "o", 1, &unused, &unused, &unused, &info);
	*height = info.ascent;
	*width = XTextWidth(pre->font, str, len);
	return pre;
}

void cleanup(struct DC *cur)
{
	XFreeFont(xw.dpy, cur->font);
	XFreeGC(xw.dpy, cur->gc);

	if (cur->next) {
		cleanup(cur->next);
		cur->next = NULL;
	}

	if (cur != &dc) {
		free(cur);
		return;
	}

	XDestroyWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);
	XCloseDisplay(xw.dpy);
	if (slides) {
		free(slides);
		slides = NULL;
	}
}

void eprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] != '\0' && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}
	exit(EXIT_FAILURE);
}

void load(FILE *fp)
{
	static size_t size = 0;
	char buf[BUFSIZ], *p;
	size_t i;

	/* read each line from stdin and add it to the item list */
	for (i = slidecount; fgets(buf, sizeof(buf), fp); i++) {
		if ((i+1) * sizeof(*slides) >= size)
			if (!(slides = realloc(slides, (size += BUFSIZ))))
				eprintf("cannot realloc %u bytes:", size);
		if ((p = strchr(buf, '\n')))
			*p = '\0';
		if (!(slides[i].text = strdup(buf)))
			eprintf("cannot strdup %u bytes:", strlen(buf)+1);
	}
	if (slides)
		slides[i].text = NULL;
	slidecount = i;
}

void advance(const Arg *arg)
{
	int new_idx = idx + arg->i;
	LIMIT(new_idx, 0, slidecount-1);
	if (new_idx != idx) {
		idx = new_idx;
		xdraw();
	}
}

void quit(const Arg *arg)
{
	running = 0;
}

void run()
{
	XEvent ev;

	/* Waiting for window mapping */
	while (1) {
		XNextEvent(xw.dpy, &ev);
		if (ev.type == ConfigureNotify) {
			xw.w = ev.xconfigure.width;
			xw.h = ev.xconfigure.height;
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
	eprintf("sent " VERSION " (c) 2014 markus.teich@stusta.mhn.de\n" \
	"usage: sent [-f font] FILE1 [FILE2 ...]", argv0);
}

void xdraw()
{
	int line_len = strlen(slides[idx].text);
	int height;
	int width;
	struct DC *dc = getfontsize(slides[idx].text, line_len, &width, &height);

	XClearWindow(xw.dpy, xw.win);
	XDrawString(xw.dpy, xw.win, dc->gc, (xw.w - width)/2, (xw.h + height)/2,
			slides[idx].text, line_len);
}

void xhints()
{
	XClassHint class = {.res_name = "sent", .res_class = "presenter"};
	XWMHints wm = {.flags = InputHint, .input = True};
	XSizeHints *sizeh = NULL;

	if (!(sizeh = XAllocSizeHints()))
		eprintf("sent: Could not alloc size hints");

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
		eprintf("Can't open display.");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);
	xw.w = DisplayWidth(xw.dpy, xw.scr);
	xw.h = DisplayHeight(xw.dpy, xw.scr);

	xw.attrs.background_pixel = WhitePixel(xw.dpy, xw.scr);
	xw.attrs.bit_gravity = CenterGravity;
	xw.attrs.event_mask = KeyPressMask | ExposureMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask;

	xw.win = XCreateWindow(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, 0,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput, xw.vis,
			CWBackPixel | CWBitGravity | CWEventMask, &xw.attrs);

	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xloadfonts(opt_font ? opt_font : font);

	XStringListToTextProperty(&argv0, 1, &prop);
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, False);
}

void xloadfonts(char *fontstr)
{
	int count = 0;
	int i = 0;
	XFontStruct *fnt;
	XGCValues gcvalues;
	struct DC *cur = &dc;
	char **fstr = XListFonts(xw.dpy, fontstr, 42, &count);

	while (count-- && !xfontisscalable(fstr[count]))
		; /* nothing, just get first scalable font result */

	if (count < 0)
		eprintf("sent: could not find a scalable font matching %s", fontstr);

	memset(&gcvalues, 0, sizeof(gcvalues));

	do {
		if (!(fnt = xloadqueryscalablefont(fstr[count], FONTSZ(i)))) {
			i++;
			continue;
		}

		cur->gc = XCreateGC(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, &gcvalues);
		cur->font = fnt;
		XSetFont(xw.dpy, cur->gc, fnt->fid);
		XSetForeground(xw.dpy, cur->gc, BlackPixel(xw.dpy, xw.scr));
		cur->next = (++i < NUMFONTS) ? malloc(sizeof(struct DC)) : NULL;
		cur = cur->next;
	} while (cur && i < NUMFONTS);

	if (cur == &dc)
		eprintf("sent: could not load fonts.");

	XFreeFontNames(fstr);
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

void resize(XEvent *e)
{
	xw.w = e->xconfigure.width;
	xw.h = e->xconfigure.height;
	xdraw();
}

int main(int argc, char *argv[])
{
	int i;
	FILE *fp = NULL;

	ARGBEGIN {
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'v':
	default:
		usage();
	} ARGEND;

	for (i = 0; i < argc; i++) {
		if ((fp = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin)) {
			load(fp);
			fclose(fp);
		} else {
			eprintf("could not open file %s for reading:", argv[i]);
		}
	}

	if (!slides || !slides[0].text)
		usage();

	xinit();
	run();

	cleanup(&dc);
	return EXIT_SUCCESS;
}
