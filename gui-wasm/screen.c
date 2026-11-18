#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include <cursor.h>
#include "screen.h"

Memimage *gscreen;
Memimage *screenimage;
Memimage *backbuf;
Rectangle screenr;
Point mousexy;
ulong chan;
int depth;
char *snarfbuf;

extern int canvasattach(int, int);
extern void canvasflush(Memimage*, Rectangle);
extern void canvasfocus(void);
extern void canvassize(int*, int*);
extern void canvasresize(int, int);
extern void inputinit(void);

/* Bounds of the cursor in the most recently presented framebuffer. */
static Rectangle drawncursor;

static Rectangle
cursorrect(Point p)
{
	p = addpt(p, cursor.offset);
	return Rect(p.x, p.y, p.x+16, p.y+16);
}

static Memimage*
fbattach(int width, int height)
{
	Rectangle r;

	r = Rect(0, 0, width, height);
	chan = XRGB32;
	depth = 4;
	screenr = r;
	screenimage = allocmemimage(r, chan);
	backbuf = allocmemimage(r, chan);
	if(screenimage == nil || backbuf == nil)
		return nil;
	if(canvasattach(width, height) < 0)
		return nil;
	return backbuf;
}

void
flushmemscreen(Rectangle r)
{
	Rectangle cr;
	Point p;
	uchar *dst;
	int x, y, i;

	if(!rectclip(&r, screenimage->r))
		return;

	if(Dx(r) <= 0 || Dy(r) <= 0)
		return;

	assert(!canqlock(&drawlock));

	memimagedraw(screenimage, r, backbuf, r.min, nil, r.min, S);

	p = addpt(mousexy, cursor.offset);
	cr = Rect(p.x, p.y, p.x+16, p.y+16);

	if(rectclip(&cr, r)){
		for(y = cr.min.y; y < cr.max.y; y++){
			for(x = cr.min.x; x < cr.max.x; x++){
				int cx, cy;

				cx = x - p.x;
				cy = y - p.y;
				i = cy*2 + cx/8;

				dst = byteaddr(screenimage, Pt(x, y));

				if(cursor.clr[i] & (0x80 >> (cx%8)))
					*(uint32_t*)dst = 0xFFFFFFFF;

				if(cursor.set[i] & (0x80 >> (cx%8)))
					*(uint32_t*)dst = 0xFF000000;
			}
		}
	}

	canvasflush(screenimage, r);
	if(rectXrect(r, cursorrect(mousexy)))
		drawncursor = cursorrect(mousexy);
}


void
canvasmouse(int x, int y, int buttons, ulong msec)
{
	Rectangle r;
	Point p;

	if(x < screenr.min.x) x = screenr.min.x;
	if(y < screenr.min.y) y = screenr.min.y;
	if(x >= screenr.max.x) x = screenr.max.x-1;
	if(y >= screenr.max.y) y = screenr.max.y-1;
	p = Pt(x, y);
	qlock(&drawlock);
	if(!eqpt(p, mousexy)){
		r = cursorrect(mousexy);
		combinerect(&r, cursorrect(p));
		mousexy = p;
		flushmemscreen(r);
	}
	qunlock(&drawlock);
	absmousetrack(x, y, buttons, msec);
}

void
screensize(Rectangle r, ulong channel)
{
	Memimage *b, *s;
	Memimage *oldb, *olds;

	if(eqrect(r, screenr)){
		gscreen = backbuf;
		gscreen->clipr = ZR;
		return;
	}

	b = allocmemimage(r, channel);
	s = allocmemimage(r, channel);
	if(b == nil || s == nil){
		if(b != nil)
			freememimage(b);
		if(s != nil)
			freememimage(s);
		return;
	}

	oldb = backbuf;
	olds = screenimage;

	backbuf = b;
	screenimage = s;
	gscreen = b;
	screenr = r;
	chan = channel;
	depth = gscreen->depth;

	gscreen->clipr = ZR;
	drawncursor = ZR;

	canvasresize(Dx(r), Dy(r));

	if(oldb != nil)
		freememimage(oldb);
	if(olds != nil)
		freememimage(olds);
}

void
screeninit(void)
{
	int width, height;

	memimageinit();
	canvassize(&width, &height);
	if(fbattach(width, height) == nil)
		panic("cannot initialize canvas");

	screensize(screenr, chan);
	gscreen->clipr = screenr;

	qlock(&drawlock);
	terminit();
	flushmemscreen(gscreen->clipr);
	qunlock(&drawlock);

	/* Start input after the terminal has been initialized. */
	inputinit();
	canvasfocus();
}

Memdata*
attachscreen(Rectangle *r, ulong *channel, int *d, int *width, int *softscreen)
{
	*r = gscreen->clipr;
	*channel = gscreen->chan;
	*d = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;
	gscreen->data->ref++;
	return gscreen->data;
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	ulong v;

	v = cmap2rgb(i);
	*r = (v>>16)&0xFF;
	*g = (v>>8)&0xFF;
	*b = v&0xFF;
}

void
setcolor(ulong i, ulong r, ulong g, ulong b)
{
}

char*
clipread(void)
{
	if(snarfbuf)
		return strdup(snarfbuf);
	return nil;
}

int
clipwrite(char *buf)
{
	free(snarfbuf);
	snarfbuf = strdup(buf);
	return snarfbuf == nil ? -1 : 0;
}

void
guimain(void)
{
	cpubody();
}

void
mouseset(Point p)
{
	Rectangle r;

	qlock(&drawlock);
	r = cursorrect(mousexy);
	combinerect(&r, cursorrect(p));
	mousexy = p;
	flushmemscreen(r);
	qunlock(&drawlock);
}

void
setcursor(void)
{
	Rectangle r;

	qlock(&drawlock);
	r = cursorrect(mousexy);
	if(!badrect(drawncursor) && Dx(drawncursor) > 0 && Dy(drawncursor) > 0)
		combinerect(&r, drawncursor);
	flushmemscreen(r);
	qunlock(&drawlock);
}

void
titlewrite(char *buf)
{
}
