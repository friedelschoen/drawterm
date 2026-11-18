/* Browser WebSocket backend for drawterm's #I device.
 *
 * The generic devip.c presents /net/ws and /net/wss.  Each socket is a
 * binary WebSocket exposed as a blocking byte stream to Plan 9 callers.
 *
 * Prototype limitations: outbound connections only; no TCP or UDP;
 * fixed number of handles per page load, limited receive buffering.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <emscripten/websocket.h>
#include <emscripten/html5.h>

#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"
#include "ip.h"
#include "devip.h"

#undef listen
#undef accept
#undef bind

enum {
	Maxws = 64,
	Maxrecv = 4*1024*1024,
	WSclosed = 0,
	WSconnecting,
	WSopen,
};

typedef struct Ws Ws;
typedef struct Msg Msg;
struct Msg {
	Msg *next;
	size_t len;
	size_t pos;
	uchar data[];
};
struct Ws {
	pthread_mutex_t lk;
	pthread_cond_t cv;
	EMSCRIPTEN_WEBSOCKET_T sock;
	Msg *head;
	Msg *tail;
	size_t queued;
	int state;
	int used;
	int secure;
};

static Ws conns[Maxws];
static pthread_mutex_t alloclk = PTHREAD_MUTEX_INITIALIZER;

static Ws*
getws(int fd)
{
	if(fd < 1 || fd > Maxws || !conns[fd-1].used)
		error("bad websocket handle");
	return &conns[fd-1];
}

static void
freequeue(Ws *w)
{
	Msg *m, *next;
	for(m = w->head; m != nil; m = next){
		next = m->next;
		free(m);
	}
	w->head = w->tail = nil;
	w->queued = 0;
}

static EM_BOOL
onopen(int type, const EmscriptenWebSocketOpenEvent *ev, void *arg)
{
	Ws *w = arg;
	pthread_mutex_lock(&w->lk);
	if(w->state == WSconnecting)
		w->state = WSopen;
	pthread_cond_broadcast(&w->cv);
	pthread_mutex_unlock(&w->lk);
	return EM_TRUE;
}

static EM_BOOL
onclose(int type, const EmscriptenWebSocketCloseEvent *ev, void *arg)
{
	Ws *w = arg;
	pthread_mutex_lock(&w->lk);
	w->state = WSclosed;
	pthread_cond_broadcast(&w->cv);
	pthread_mutex_unlock(&w->lk);
	return EM_TRUE;
}

static EM_BOOL
onerror(int type, const EmscriptenWebSocketErrorEvent *ev, void *arg)
{
	Ws *w = arg;
	pthread_mutex_lock(&w->lk);
	w->state = WSclosed;
	pthread_cond_broadcast(&w->cv);
	pthread_mutex_unlock(&w->lk);
	return EM_TRUE;
}

static EM_BOOL
onmessage(int type, const EmscriptenWebSocketMessageEvent *ev, void *arg)
{
	Ws *w = arg;
	Msg *m;

	pthread_mutex_lock(&w->lk);
	if(w->state != WSopen)
		goto Done;
	if(ev->isText || ev->numBytes > Maxrecv - w->queued){
		w->state = WSclosed;
		pthread_cond_broadcast(&w->cv);
		goto Done;
	}
	m = malloc(sizeof(*m) + ev->numBytes);
	if(m == nil){
		w->state = WSclosed;
		pthread_cond_broadcast(&w->cv);
		goto Done;
	}
	m->next = nil;
	m->len = ev->numBytes;
	m->pos = 0;
	memcpy(m->data, ev->data, ev->numBytes);
	if(w->tail != nil)
		w->tail->next = m;
	else
		w->head = m;
	w->tail = m;
	w->queued += ev->numBytes;
	pthread_cond_broadcast(&w->cv);
Done:
	pthread_mutex_unlock(&w->lk);
	return EM_TRUE;
}

void
osipinit(void)
{
	kstrdup(&sysname, "wasm");
	if(!emscripten_websocket_is_supported())
		error("WebSocket API unavailable");
}

int
so_socket(int type, unsigned char *addr)
{
	Ws *w;
	int i;

	if(type != S_WS && type != S_WSS)
		error("TCP/UDP unavailable in browser");

	pthread_mutex_lock(&alloclk);
	for(i = 0; i < Maxws; i++)
		if(!conns[i].used)
			break;
	if(i == Maxws){
		pthread_mutex_unlock(&alloclk);
		error("too many WebSockets");
	}
	w = &conns[i];
	w->used = 1;
	w->secure = type == S_WSS;
	w->state = WSclosed;
	w->sock = 0;
	w->head = w->tail = nil;
	w->queued = 0;
	pthread_mutex_init(&w->lk, nil);
	pthread_cond_init(&w->cv, nil);
	pthread_mutex_unlock(&alloclk);
	return i+1;
}

void
so_connectws(int fd, char *hostpath, int secure)
{
	Ws *w;
	EmscriptenWebSocketCreateAttributes attr;
	char url[1024];
	char *path;
	EMSCRIPTEN_RESULT r;
	int ok;

	w = getws(fd);
	if(w->secure != secure)
		error("WebSocket protocol mismatch");
	path = strchr(hostpath, '!');
	if(path == nil || path == hostpath)
		error("bad WebSocket address");
    *path = '\0';
	if(strlen(hostpath) > 500 || strlen(path) > 500)
		error("WebSocket address too long");
    do {
        path++;
    }while(path[0] == '/');
	snprintf(url, sizeof(url), "%s://%s/%s", secure ? "wss" : "ws", hostpath, path);

	memset(&attr, 0, sizeof(attr));
	attr.url = url;
	attr.createOnMainThread = EM_TRUE;

	pthread_mutex_lock(&w->lk);
	w->state = WSconnecting;
	pthread_mutex_unlock(&w->lk);
	w->sock = emscripten_websocket_new(&attr);
	if(w->sock <= 0)
		error("cannot create WebSocket");

	r = emscripten_websocket_set_onopen_callback_on_thread(
		w->sock, w, onopen, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
	if(r != EMSCRIPTEN_RESULT_SUCCESS)
		error("cannot register WebSocket open callback");
	r = emscripten_websocket_set_onclose_callback_on_thread(
		w->sock, w, onclose, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
	if(r != EMSCRIPTEN_RESULT_SUCCESS)
		error("cannot register WebSocket close callback");
	r = emscripten_websocket_set_onerror_callback_on_thread(
		w->sock, w, onerror, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
	if(r != EMSCRIPTEN_RESULT_SUCCESS)
		error("cannot register WebSocket error callback");
	r = emscripten_websocket_set_onmessage_callback_on_thread(
		w->sock, w, onmessage, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
	if(r != EMSCRIPTEN_RESULT_SUCCESS)
		error("cannot register WebSocket message callback");

	pthread_mutex_lock(&w->lk);
	while(w->state == WSconnecting)
		pthread_cond_wait(&w->cv, &w->lk);
	ok = w->state == WSopen;
	pthread_mutex_unlock(&w->lk);
	if(!ok)
		error("WebSocket connection failed");
}

int
so_recv(int fd, void *d, int n, int flags)
{
	Ws *w;
	Msg *m;
	size_t count;
	if(n <= 0)
		return 0;
	w = getws(fd);
	pthread_mutex_lock(&w->lk);
	while(w->head == nil && w->state == WSopen)
		pthread_cond_wait(&w->cv, &w->lk);
	if(w->head == nil){
		pthread_mutex_unlock(&w->lk);
		return 0;
	}
	m = w->head;
	count = m->len-m->pos;
	if(count > (size_t)n)
		count = n;
	memcpy(d, m->data+m->pos, count);
	m->pos += count;
	w->queued -= count;
	if(m->pos == m->len){
		w->head = m->next;
		if(w->head == nil)
			w->tail = nil;
		free(m);
	}
	pthread_mutex_unlock(&w->lk);
	return count;
}

int
so_send(int fd, void *d, int n, int flags)
{
	Ws *w;
	EMSCRIPTEN_RESULT r;
	if(n <= 0)
		return 0;
	w = getws(fd);
	pthread_mutex_lock(&w->lk);
	if(w->state != WSopen){
		pthread_mutex_unlock(&w->lk);
		return -1;
	}
	/* No blocking on browser thread: do not hold lock across proxied send. */
	pthread_mutex_unlock(&w->lk);
	r = emscripten_websocket_send_binary(w->sock, d, (uint32_t)n);
	return r == EMSCRIPTEN_RESULT_SUCCESS ? n : -1;
}

void
so_close(int fd)
{
	Ws *w;
	EMSCRIPTEN_WEBSOCKET_T sock;
	if(fd < 1 || fd > Maxws || !conns[fd-1].used)
		return;
	w = &conns[fd-1];
	pthread_mutex_lock(&w->lk);
	w->state = WSclosed;
	pthread_cond_broadcast(&w->cv);
	sock = w->sock;
	w->sock = 0;
	freequeue(w);
	pthread_mutex_unlock(&w->lk);
	if(sock > 0){
		/* Stable slot lifetime avoids use-after-free in late callbacks. */
		emscripten_websocket_close(sock, 1000, nil);
		emscripten_websocket_delete(sock);
	}
}

void
so_connect(int fd, unsigned char *addr, unsigned short port)
{
	error("TCP unavailable in browser");
}

void
so_getsockname(int fd, unsigned char *addr, unsigned short *port)
{
	memset(addr, 0, IPaddrlen);
	*port = 0;
}

void
so_bind(int fd, int restricted, unsigned short port, unsigned char *addr)
{
	error("WebSocket bind unsupported");
}

void
so_listen(int fd)
{
	error("WebSocket listen unsupported");
}

int
so_accept(int fd, unsigned char *addr, unsigned short *port)
{
	error("WebSocket accept unsupported");
	return -1;
}

int
so_getservbyname(char *service, char *net, char *port)
{
	return -1;
}

int
so_gethostbyname(char *host, char **hostv, int n)
{
	return 0;
}
