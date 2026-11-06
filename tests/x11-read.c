/*
 * x11-read — test helper: read the X11 CLIPBOARD, handling INCR correctly.
 *
 * ICCCM ordering matters here: the owner publishes the property first and only
 * then sends SelectionNotify.  A reader must therefore wait for the
 * SelectionNotify (learning the type) before it starts consuming chunks, and
 * must never treat the INCR marker — one 32-bit item holding the total length —
 * as data.
 *
 * Usage: x11-read [TARGET]   (default UTF8_STRING); writes raw bytes to stdout.
 * Exit: 0 success, 2 refused, 3 timeout.
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned char *g_acc;
static size_t g_len, g_cap;
static int g_chunks;

static void append(const unsigned char *p, size_t n)
{
	if (g_len + n > g_cap) {
		while (g_len + n > g_cap)
			g_cap *= 2;
		g_acc = realloc(g_acc, g_cap);
	}
	memcpy(g_acc + g_len, p, n);
	g_len += n;
}

/* Read the property, optionally deleting it.  Returns 1 if present. */
static int read_prop(Display *d, Window w, Atom prop, Atom *type,
                     int *fmt, unsigned long *n, int del)
{
	unsigned char *p = NULL;
	unsigned long after = 0;
	*type = None;
	if (XGetWindowProperty(d, w, prop, 0, 0x1FFFFFFF, del, AnyPropertyType,
	                       type, fmt, n, &after, &p) != Success)
		return 0;
	if (p)
		XFree(p);
	return *type != None;
}

int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	if (!d)
		return 1;

	g_cap = 1 << 20;
	g_acc = malloc(g_cap);

	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
	Atom clip = XInternAtom(d, "CLIPBOARD", False);
	Atom prop = XInternAtom(d, "CBFIX_RD", False);
	Atom incr = XInternAtom(d, "INCR", False);
	Atom tgt  = XInternAtom(d, argc > 1 ? argv[1] : "UTF8_STRING", False);

	XSelectInput(d, w, PropertyChangeMask);
	XConvertSelection(d, clip, tgt, prop, w, CurrentTime);
	XFlush(d);

	/* ---- phase 1: wait for SelectionNotify, learn the transfer type ---- */
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0;
	int chunked = 0;

	for (int i = 0; i < 2000; i++) {
		XEvent e;
		if (!XPending(d)) { usleep(3000); continue; }
		XNextEvent(d, &e);
		if (e.type != SelectionNotify)
			continue;               /* includes the early marker PropertyNotify */
		if (e.xselection.property == None)
			return 2;
		if (!read_prop(d, w, prop, &type, &fmt, &n, 0))
			return 2;
		if (type == incr) {
			chunked = 1;
			XDeleteProperty(d, w, prop);   /* consume the marker */
			XFlush(d);
		}
		break;
	}
	if (type == None)
		return 3;

	/* ---- phase 2: either direct data, or chunks until zero length ---- */
	if (!chunked) {
		unsigned char *p = NULL;
		unsigned long after = 0;
		if (XGetWindowProperty(d, w, prop, 0, 0x1FFFFFFF, True,
		                       AnyPropertyType, &type, &fmt, &n, &after,
		                       &p) == Success && p) {
			append(p, n);
			XFree(p);
		}
		write(1, g_acc, g_len);
		return 0;
	}

	/* Wall-clock deadline: unrelated events (other clients' selections, the
	 * input method, ...) must never consume a fixed iteration budget. */
	struct timespec t0, tn;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &tn);
		if ((tn.tv_sec - t0.tv_sec) > 20)
			break;
		XEvent e;
		static long spins;
		if (!XPending(d)) { if((++spins % 5000)==0) fprintf(stderr,"[rd] idle spins=%ld pending=0\n",spins); usleep(2000); continue; }
		XNextEvent(d, &e);
		if (e.type != PropertyNotify || e.xproperty.atom != prop ||
		    e.xproperty.state != PropertyNewValue) {
			continue;
		}

		unsigned char *p = NULL;
		unsigned long after = 0;
		Atom t = None;
		int f = 0;
		unsigned long cnt = 0;
		after = 0;
		if (XGetWindowProperty(d, w, prop, 0, 0x1FFFFFFF, True,
		                       AnyPropertyType, &t, &f, &cnt, &after,
		                       &p) != Success)
			continue;

		if (t == incr) {          /* stray marker: ignore */
			XFree(p);
			continue;
		}
		if (cnt == 0) {           /* zero-length chunk ends the transfer */
			XFree(p);
			break;
		}
		append(p, f == 8 ? cnt : cnt * (f == 16 ? 2 : 4));
		g_chunks++;

		XFree(p);
	}


	write(1, g_acc, g_len);
	return 0;
}
