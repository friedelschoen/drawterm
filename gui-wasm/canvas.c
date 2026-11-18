#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"

#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include "screen.h"

#undef long

#include <emscripten.h>
#include <emscripten/html5.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern Rectangle screenr;
extern void canvasmouse(int, int, int, ulong);

static uchar *rgbabuf;
static size_t rgbacap;
static int mousebuttons;
static int mousex, mousey;

/* Browser callbacks only queue events. The Plan 9 process consumes them. */
enum {
	Nkeys = 64,
	Nevents = 1024,
	Ikey,
	Imouse,
	Iwheel,
	Iresize,
};

typedef struct Inputevent Inputevent;
struct Inputevent {
	int type;
	int x, y;
	int buttons;
	int key, down;
	int wheel;
	ulong msec;
};

static Inputevent events[Nevents];
static int head, tail, count;
static pthread_mutex_t inputlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t inputcond = PTHREAD_COND_INITIALIZER;

/* Key identity must survive changes to Shift and other modifiers. */
static struct {
	char code[64];
	int key;
} keys[Nkeys];

static void
postevent(Inputevent *e)
{
	int last;

	pthread_mutex_lock(&inputlock);

	/* Retain button transitions, but coalesce motion and resize bursts. */
	if(count > 0 && (e->type == Imouse || e->type == Iresize)){
		last = (tail + Nevents - 1) % Nevents;
		if(events[last].type == e->type &&
		   (e->type == Iresize || events[last].buttons == e->buttons)){
			events[last] = *e;
			pthread_mutex_unlock(&inputlock);
			return;
		}
	}

	if(count == Nevents){
		/* Keep the queue bounded; never block the browser on a full queue. */
		pthread_mutex_unlock(&inputlock);
		return;
	}

	events[tail] = *e;
	tail = (tail + 1) % Nevents;
	count++;
	pthread_cond_signal(&inputcond);
	pthread_mutex_unlock(&inputlock);
}

static void
inputproc(void *arg)
{
	Inputevent e;
	int buttons;

	USED(arg);
	buttons = 0;
	for(;;){
		pthread_mutex_lock(&inputlock);
		while(count == 0)
			pthread_cond_wait(&inputcond, &inputlock);
		e = events[head];
		head = (head + 1) % Nevents;
		count--;
		pthread_mutex_unlock(&inputlock);

		switch(e.type){
		case Ikey:
			kbdkey(e.key, e.down);
			break;
		case Imouse:
			buttons = e.buttons;
			canvasmouse(e.x, e.y, buttons, e.msec);
			break;
		case Iwheel:
			/* Mouse wheel is a momentary Plan 9 button (8 or 16). */
			canvasmouse(e.x, e.y, buttons | e.wheel, e.msec);
			canvasmouse(e.x, e.y, buttons, e.msec + 1);
			break;
		case Iresize:
			screenresize(Rect(0, 0, e.x, e.y));
			break;
		}
	}
}

/* The browser owns a persistent RGBA image and presents at most once per frame. */
static int
canvasinit(int width, int height)
{
	return MAIN_THREAD_EM_ASM_INT({
		const canvas = document.getElementById("canvas");
		if(!canvas)
			return -1;
		canvas.width = $0;
		canvas.height = $1;
		canvas.style.imageRendering = "pixelated";
		const ctx = canvas.getContext("2d", {alpha: false});
		if(!ctx)
			return -1;

		const frame = {};
		frame.ctx = ctx;
		frame.image = ctx.createImageData($0, $1);
		frame.dirty = null;
		frame.scheduled = false;
		frame.present = function() {
			this.scheduled = false;
			const r = this.dirty;
			this.dirty = null;
			if(r)
				this.ctx.putImageData(this.image, 0, 0,
					r[0], r[1], r[2] - r[0], r[3] - r[1]);
		};
		globalThis.drawtermframe = frame;
		return 0;
	}, width, height);
}

void
canvassize(int *width, int *height)
{
	MAIN_THREAD_EM_ASM({
		const r = document.getElementById("canvas").getBoundingClientRect();
		HEAP32[$0 >> 2] = Math.max(1, Math.floor(r.width));
		HEAP32[$1 >> 2] = Math.max(1, Math.floor(r.height));
	}, width, height);
}

/* Copy only the updated rows into the retained full-screen ImageData. */
static void
canvasput(void *pixels, int x, int y, int width, int height)
{
	MAIN_THREAD_EM_ASM({
		const frame = globalThis.drawtermframe;
		const stride = frame.image.width;
		const source = HEAPU8.subarray($0, $0 + $3 * $4 * 4);
		const dest = frame.image.data;

		for(let row = 0; row < $4; row++){
			const start = row * $3 * 4;
			dest.set(source.subarray(start, start + $3 * 4),
				(($2 + row) * stride + $1) * 4);
		}

		const x1 = $1 + $3;
		const y1 = $2 + $4;
		if(frame.dirty){
			frame.dirty[0] = Math.min(frame.dirty[0], $1);
			frame.dirty[1] = Math.min(frame.dirty[1], $2);
			frame.dirty[2] = Math.max(frame.dirty[2], x1);
			frame.dirty[3] = Math.max(frame.dirty[3], y1);
		}else{
			frame.dirty = new Int32Array(4);
			frame.dirty[0] = $1;
			frame.dirty[1] = $2;
			frame.dirty[2] = x1;
			frame.dirty[3] = y1;
		}

		if(!frame.scheduled){
			frame.scheduled = true;
			requestAnimationFrame(() => frame.present());
		}
	}, pixels, x, y, width, height);
}

void
canvasfocus(void)
{
	MAIN_THREAD_EM_ASM({
		document.getElementById("canvas").focus();
	});
}

int
canvasattach(int width, int height)
{
	return canvasinit(width, height);
}

/* XRGB32, little-endian (B,G,R,X), to canvas RGBA. */
void
canvasflush(Memimage *m, Rectangle r)
{
	int x, y, w, h;
	size_t n;
	uchar *src, *dst, *p;

	if(!rectclip(&r, m->r))
		return;
	w = Dx(r);
	h = Dy(r);
	if(w <= 0 || h <= 0)
		return;

	n = (size_t)w * h * 4;
	if(n > rgbacap){
		p = realloc(rgbabuf, n);
		if(p == nil)
			abort();
		rgbabuf = p;
		rgbacap = n;
	}
	for(y = 0; y < h; y++){
		src = byteaddr(m, Pt(r.min.x, r.min.y + y));
		dst = rgbabuf + (size_t)y * w * 4;
		for(x = 0; x < w; x++){
			dst[4*x] = src[4*x+2];
			dst[4*x+1] = src[4*x+1];
			dst[4*x+2] = src[4*x];
			dst[4*x+3] = 255;
		}
	}
	canvasput(rgbabuf, r.min.x, r.min.y, w, h);
}

void
canvasresize(int width, int height)
{
	MAIN_THREAD_EM_ASM({
		const frame = globalThis.drawtermframe;
		const canvas = frame.ctx.canvas;
		canvas.width = $0;
		canvas.height = $1;
		frame.image = frame.ctx.createImageData($0, $1);
		frame.dirty = null;
		/* An already queued frame will present the new image if needed. */
	}, width, height);
}

static int
translatekey(const char *key)
{
	Rune r;
	int n;

	if(strcmp(key, "Enter") == 0) return '\n';
	if(strcmp(key, "Backspace") == 0) return '\b';
	if(strcmp(key, "Tab") == 0) return '\t';
	if(strcmp(key, "Escape") == 0) return Kesc;
	if(strcmp(key, "Delete") == 0) return Kdel;
	if(strcmp(key, "Insert") == 0) return Kins;
	if(strcmp(key, "Home") == 0) return Khome;
	if(strcmp(key, "End") == 0) return Kend;
	if(strcmp(key, "PageUp") == 0) return Kpgup;
	if(strcmp(key, "PageDown") == 0) return Kpgdown;
	if(strcmp(key, "ArrowUp") == 0) return Kup;
	if(strcmp(key, "ArrowDown") == 0) return Kdown;
	if(strcmp(key, "ArrowLeft") == 0) return Kleft;
	if(strcmp(key, "ArrowRight") == 0) return Kright;
	if(strcmp(key, "Control") == 0) return Kctl;
	if(strcmp(key, "Alt") == 0) return Kalt;
	if(key[0] == 'F' && key[1] >= '1' && key[1] <= '9'){
		n = atoi(key + 1);
		if(n >= 1 && n <= 12)
			return KF|n;
	}
	if(key[0] == 0)
		return -1;
	n = chartorune(&r, (char*)key);
	if(n > 0 && key[n] == 0)
		return r;
	return -1;
}

static void
postkey(int key, int down)
{
	Inputevent e;

	memset(&e, 0, sizeof(e));
	e.type = Ikey;
	e.key = key;
	e.down = down;
	postevent(&e);
}

static EM_BOOL
onkeyboard(int type, const EmscriptenKeyboardEvent *e, void *arg)
{
	int i, freeidx, key;

	USED(arg);
	freeidx = -1;
	for(i = 0; i < Nkeys; i++){
		if(keys[i].code[0] && strcmp(keys[i].code, e->code) == 0)
			break;
		if(keys[i].code[0] == 0 && freeidx < 0)
			freeidx = i;
	}
	if(type == EMSCRIPTEN_EVENT_KEYUP){
		if(i == Nkeys)
			return EM_FALSE;
		postkey(keys[i].key, 0);
		keys[i].code[0] = 0;
		return EM_TRUE;
	}
	if(i != Nkeys){
		if(e->repeat)
			postkey(keys[i].key, 1);
		return EM_TRUE;
	}
	key = translatekey(e->key);
	if(key < 0 || freeidx < 0)
		return EM_FALSE;
	if(strlen(e->code) >= sizeof(keys[freeidx].code))
		return EM_FALSE;
	strcpy(keys[freeidx].code, e->code);
	keys[freeidx].key = key;
	postkey(key, 1);
	return EM_TRUE;
}

static EM_BOOL
onblur(int type, const EmscriptenFocusEvent *e, void *arg)
{
	Inputevent m;
	int i;

	USED(e);
	USED(arg);
	for(i = 0; i < Nkeys; i++){
		if(keys[i].code[0] == 0)
			continue;
		postkey(keys[i].key, 0);
		keys[i].code[0] = 0;
	}
	mousebuttons = 0;
	memset(&m, 0, sizeof(m));
	m.type = Imouse;
	m.x = mousex;
	m.y = mousey;
	m.msec = (ulong)emscripten_get_now();
	postevent(&m);
	return EM_FALSE;
}

static int
translatebuttons(unsigned short b)
{
	int buttons;

	buttons = 0;
	if(b & 1) buttons |= 1;
	if(b & 4) buttons |= 2;
	if(b & 2) buttons |= 4;
	return buttons;
}

/* Called on the browser main thread; fetch the canvas rectangle only once. */
static int
mousecoords(int clientx, int clienty, int *x, int *y)
{
	return EM_ASM_INT({
		const r = document.getElementById("canvas").getBoundingClientRect();
		if(r.width <= 0 || r.height <= 0)
			return -1;
		HEAP32[$2 >> 2] = Math.floor(($0 - r.left) * $4 / r.width);
		HEAP32[$3 >> 2] = Math.floor(($1 - r.top) * $5 / r.height);
		return 0;
	}, clientx, clienty, x, y, Dx(screenr), Dy(screenr));
}

static EM_BOOL
onmouse(int type, const EmscriptenMouseEvent *e, void *arg)
{
	Inputevent m;
	int x, y;

	USED(arg);
	if(mousecoords(e->clientX, e->clientY, &x, &y) < 0)
		return EM_FALSE;
	mousex = x;
	mousey = y;
	mousebuttons = translatebuttons(e->buttons);
	memset(&m, 0, sizeof(m));
	m.type = Imouse;
	m.x = x;
	m.y = y;
	m.buttons = mousebuttons;
	m.msec = (ulong)emscripten_get_now();
	postevent(&m);
	if(type == EMSCRIPTEN_EVENT_MOUSEDOWN){
		EM_ASM({ document.getElementById("canvas").focus(); });
	}
	return EM_TRUE;
}

static EM_BOOL
onwheel(int type, const EmscriptenWheelEvent *e, void *arg)
{
	Inputevent m;

	USED(type);
	USED(arg);
	if(e->deltaY == 0)
		return EM_FALSE;
	memset(&m, 0, sizeof(m));
	m.type = Iwheel;
	m.x = mousex;
	m.y = mousey;
	m.wheel = e->deltaY < 0 ? 8 : 16;
	m.msec = (ulong)emscripten_get_now();
	postevent(&m);
	return EM_TRUE;
}

static EM_BOOL
onresize(int type, const EmscriptenUiEvent *e, void *arg)
{
	Inputevent ev;

	USED(type);
	USED(e);
	USED(arg);

	memset(&ev, 0, sizeof(ev));
	ev.type = Iresize;
	canvassize(&ev.x, &ev.y);
	postevent(&ev);
	return EM_TRUE;
}

void
inputinit(void)
{
	pthread_t t;

	kproc("canvasinput", inputproc, nil);

	t = EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD;
	emscripten_set_keydown_callback_on_thread(
		"#canvas", nil, 1, onkeyboard, t);
	emscripten_set_keyup_callback_on_thread(
		EMSCRIPTEN_EVENT_TARGET_WINDOW, nil, 1, onkeyboard, t);
	emscripten_set_blur_callback_on_thread(
		"#canvas", nil, 1, onblur, t);
	emscripten_set_mousemove_callback_on_thread(
		"#canvas", nil, 1, onmouse, t);
	emscripten_set_mousedown_callback_on_thread(
		"#canvas", nil, 1, onmouse, t);
	emscripten_set_mouseup_callback_on_thread(
		EMSCRIPTEN_EVENT_TARGET_WINDOW, nil, 1, onmouse, t);
	emscripten_set_wheel_callback_on_thread(
		"#canvas", nil, 1, onwheel, t);
	emscripten_set_resize_callback_on_thread(
		EMSCRIPTEN_EVENT_TARGET_WINDOW, nil, 0, onresize, t);
}
