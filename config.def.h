/* See LICENSE file for copyright and license details. */

static char font[] = "-*-dejavu sans condensed-bold-r-*-*-0-0-*-*-*-0-*-*";
#define NUMFONTS 30
#define FONTSZ(x) ((int)(100.0 * powf(1.1288, (x)))) /* x in [0, NUMFONTS-1] */

/* how much screen estate is to be used at max for the content */
static float usablewidth = 0.75;
static float usableheight = 0.75;

static Mousekey mshortcuts[] = {
	/* button         function        argument */
	{ Button1,        advance,        {.i = +1} },
	{ Button2,        advance,        {.i = -1} },
};

static Shortcut shortcuts[] = {
	/* keysym         function        argument */
	{ XK_q,           quit,           {0} },
	{ XK_Right,       advance,        {.i = +1} },
	{ XK_Left,        advance,        {.i = -1} },
	{ XK_Return,      advance,        {.i = +1} },
	{ XK_BackSpace,   advance,        {.i = -1} },
	{ XK_Down,        advance,        {.i = +5} },
	{ XK_Up,          advance,        {.i = -5} },
	{ XK_Next,        advance,        {.i = +10} },
	{ XK_Prior,       advance,        {.i = -10} },
};
