/*
 * cbfix-bridge — a lossless CLIPBOARD bridge between the Wayland compositor
 * clipboard and the X11 (Xwayland) CLIPBOARD selection.
 *
 * Why this program exists
 * -----------------------
 * On Wayland, wl_data_device.set_selection() is only honoured by the compositor
 * when the requesting client holds a recent keyboard-focus serial.  A
 * background bridge — and xwayland-satellite's own X11->Wayland path — has no
 * such serial, so it cannot publish to the Wayland clipboard at all; the
 * request is silently dropped.  ext-data-control-v1 exists precisely to give
 * privileged clients (clipboard managers, X11 bridges, ...) a serial-free way
 * to read *and* write the selection.  This program is built on that protocol.
 *
 * The program holds CLIPBOARD on its own X11 window, so it never fights another
 * X11 client for ownership.
 *
 * Copyright (c) 2026.  MIT licensed; see LICENSE.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */

#define DEFAULT_INTERVAL_MS  120
#define MAX_PAYLOAD          (64u * 1024u * 1024u)
#define DIRECT_PROP_LIMIT    0x3FFFFu   /* stay below the X11 request limit */
#define INCR_CHUNK           0x20000u
#define TRANSFER_TIMEOUT_MS  4000
#define TARGET_TIMEOUT_MS    600
#define INCR_IDLE_TIMEOUT_MS 30000
#define MAX_INCR             8

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static int g_verbose;

/* Set by the X error handler; lets a probe tell a dead window from a live one
 * without dying itself. */
static int           g_xerr;
static unsigned long g_xerr_resource;

static void logf_(const char *lvl, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "cbfix-bridge[%d] %-5s ", (int)getpid(), lvl);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

#define INFO(...)  logf_("info", __VA_ARGS__)
#define WARN(...)  logf_("warn", __VA_ARGS__)
#define ERRF(...)  logf_("error", __VA_ARGS__)
#define DBG(...)   do { if (g_verbose) logf_("debug", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* buffers                                                             */
/* ------------------------------------------------------------------ */

struct buf {
	char    *data;
	size_t   len;
	char    *mime;
	uint64_t hash;
	bool     valid;
};

static void buf_clear(struct buf *b)
{
	free(b->data);
	free(b->mime);
	b->data = NULL;
	b->mime = NULL;
	b->len = 0;
	b->hash = 0;
	b->valid = false;
}

static void buf_set(struct buf *b, const void *data, size_t len,
                    const char *mime, uint64_t hash)
{
	char *copy = malloc(len ? len : 1);
	char *mcopy = mime ? strdup(mime) : NULL;
	if (!copy) {
		ERRF("out of memory");
		return;
	}
	if (len)
		memcpy(copy, data, len);
	buf_clear(b);
	b->data = copy;
	b->len = len;
	b->mime = mcopy;
	b->hash = hash;
	b->valid = true;
}

static uint64_t hash_of(const void *data, size_t len)
{
	const unsigned char *p = data;
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	h ^= (uint64_t)len;
	h *= 1099511628211ull;
	return h;
}

static bool mime_is_text(const char *mime)
{
	if (!mime)
		return true;
	return strncmp(mime, "image/", 6) != 0;
}

/* ------------------------------------------------------------------ */
/* global state                                                        */
/* ------------------------------------------------------------------ */

struct outbox {
	struct outbox *next;
	int     fd;
	char   *data;
	size_t  len, off;
};

/* One in-flight INCR transfer.  Several readers may ask for the same large
 * payload, and a reader that walks away must not wedge the bridge, so each
 * transfer carries its own offset and a liveness deadline. */
struct incr_slot {
	bool   active;
	Window requestor;
	Atom   property;
	Atom   type;
	size_t off;
	struct timespec deadline;
	/* The payload is *pinned* for the lifetime of the transfer.  The live
	 * buffer can be replaced at any moment (a new Wayland selection arriving
	 * mid-transfer); without a snapshot the reader would be truncated at an
	 * arbitrary offset. */
	char  *data;
	size_t len;
	/* Consecutive recovery attempts after a lost PropertyDelete. */
	int    recoveries;
};

static struct {
	/* --- X11 --- */
	Display *dpy;
	Window   win, root;
	Atom     CLIPBOARD, TARGETS, TIMESTAMP, UTF8_STRING, TEXT, INCR,
	         TEXT_PLAIN_UTF8, SEL_PROP, IMAGE_PNG;
	int      xfixes_ev;
	bool     have_xfixes;
	Window   tracked_owner;
	bool     x11_dirty;
	struct incr_slot incr[MAX_INCR];

	/* --- Wayland --- */
	struct wl_display                  *wl;
	struct wl_registry                 *registry;
	struct wl_seat                     *seat;
	struct ext_data_control_manager_v1 *dcm;
	struct ext_data_control_device_v1  *dcdev;
	struct ext_data_control_source_v1  *dcsrc;
	struct ext_data_control_offer_v1   *dc_offer;
	char                              **offer_mimes;
	size_t                              n_offer_mimes;
	struct ext_data_control_offer_v1   *pending_offer;
	char                              **pending_mimes;
	size_t                              n_pending_mimes;
	bool                                wl_sel_dirty;
	/* While a receive() is in flight the offer it uses must stay alive;
	 * the compositor may replace the selection mid-transfer. */
	struct ext_data_control_offer_v1   *in_use_offer;
	struct ext_data_control_offer_v1   *retire_offer;
	bool                                fetch_in_flight;

	/* --- content we serve on each side ---
	 * serve_x  : what X11 clients get (fed from the Wayland clipboard)
	 * serve_wl : what Wayland peers get (fed from the X11 clipboard)   */
	struct buf serve_x;
	struct buf serve_wl;
	/* Hash of the content we last pushed to the other side.  Any incoming
	 * selection change carrying that same payload is our own echo. */
	uint64_t   pushed_hash;
	bool       have_pushed;

	/* --- runtime --- */
	struct outbox *out;
	bool  running, configured;
	int   interval_ms;
	int   stat_x2wl, stat_wl2x;
} st;

static void pump_outbox(void);
static void x_serve_request(XSelectionRequestEvent *rq, const struct buf *b);
static void x_incr_step(struct incr_slot *slot, const struct buf *b);
static void x_drain_events(void);
static void fetch_done(void);

/* ------------------------------------------------------------------ */
/* time helpers                                                        */
/* ------------------------------------------------------------------ */

static struct timespec now_mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t;
}

static void deadline_set(struct timespec *d, int ms)
{
	*d = now_mono();
	d->tv_sec += ms / 1000;
	d->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (d->tv_nsec >= 1000000000L) {
		d->tv_sec++;
		d->tv_nsec -= 1000000000L;
	}
}

static long ms_until(const struct timespec *deadline)
{
	struct timespec n = now_mono();
	return (deadline->tv_sec - n.tv_sec) * 1000 +
	       (deadline->tv_nsec - n.tv_nsec) / 1000000;
}

/* ------------------------------------------------------------------ */
/* X11 INCR serving                                                    */
/* ------------------------------------------------------------------ */

static struct incr_slot *incr_find(Window requestor, Atom property)
{
	for (int i = 0; i < MAX_INCR; i++)
		if (st.incr[i].active && st.incr[i].requestor == requestor &&
		    st.incr[i].property == property)
			return &st.incr[i];
	return NULL;
}

static void incr_end(struct incr_slot *slot, bool terminate)
{
	if (!slot->active)
		return;
	if (terminate && slot->off < slot->len)
		ERRF("PREMATURE terminator: reader=0x%lx off=%zu len=%zu",
		     (unsigned long)slot->requestor, slot->off, slot->len);
	if (terminate) {
		/* a zero-length property ends the transfer */
		XChangeProperty(st.dpy, slot->requestor, slot->property,
		                slot->type, 8, PropModeReplace, NULL, 0);
		XFlush(st.dpy);
	}
	DBG("INCR transfer to 0x%lx ended at %zu bytes",
	    (unsigned long)slot->requestor, slot->off);
	free(slot->data);
	memset(slot, 0, sizeof(*slot));
}

/* Reclaim or recover transfers whose reader seems to have stopped asking.
 *
 * A missing PropertyDelete does not always mean a dead reader: sync_pass()
 * round-trips to the X server, which can pull the notification into Xlib's
 * internal event queue where the poll() loop never sees it.  Before dropping
 * anything, probe the requestor's property to find out what actually happened:
 *   - property still holds data -> the reader just has not consumed it yet;
 *     extend the deadline and leave it alone.
 *   - property is empty         -> the chunk WAS consumed but the delete
 *     notification was lost; re-send the next chunk (idempotent: the reader
 *     is waiting for exactly this).
 *   - the window is gone        -> reader died; drop the transfer.
 * A transfer is abandoned only after several consecutive recoveries make no
 * progress. */
static void incr_recover(void)
{
	for (int i = 0; i < MAX_INCR; i++) {
		struct incr_slot *slot = &st.incr[i];
		if (!slot->active || ms_until(&slot->deadline) >= 0)
			continue;

		unsigned char *prop = NULL;
		Atom type = None;
		int fmt = 0;
		unsigned long n = 0, after = 0;

		g_xerr = 0;
		XGetWindowProperty(st.dpy, slot->requestor, slot->property,
		                   0, 0, False, AnyPropertyType, &type, &fmt,
		                   &n, &after, &prop);
		if (prop)
			XFree(prop);

		if (g_xerr) {
			/* BadWindow & co: the reader is gone */
			WARN("INCR reader 0x%lx vanished; dropping transfer",
			     (unsigned long)slot->requestor);
			incr_end(slot, false);
			continue;
		}

		if (type != None && (n > 0 || after > 0)) {
			/* chunk not consumed yet: reader is merely slow */
			DBG("INCR reader 0x%lx slow; extending deadline",
			    (unsigned long)slot->requestor);
			deadline_set(&slot->deadline, INCR_IDLE_TIMEOUT_MS);
			continue;
		}

		/* Property is empty: the chunk was consumed but the notification
		 * got lost.  Resume the stream from where we are. */
		slot->recoveries++;
		if (slot->recoveries > 8) {
			WARN("INCR reader 0x%lx unresponsive; dropping transfer",
			     (unsigned long)slot->requestor);
			incr_end(slot, false);
			continue;
		}
		WARN("INCR notification lost for 0x%lx; resuming at %zu/%zu",
		     (unsigned long)slot->requestor, slot->off, slot->len);
		x_incr_step(slot, NULL);
	}
}

static void x_incr_step(struct incr_slot *slot, const struct buf *unused)
{
	(void)unused;
	if (!slot || !slot->active)
		return;
	if (!slot->data) {
		incr_end(slot, true);
		return;
	}
	size_t remain = slot->len - slot->off;
	if (remain == 0) {
		incr_end(slot, true);
		return;
	}
	size_t chunk = remain > INCR_CHUNK ? INCR_CHUNK : remain;
	XChangeProperty(st.dpy, slot->requestor, slot->property, slot->type, 8,
	                PropModeReplace,
	                (const unsigned char *)slot->data + slot->off, (int)chunk);
	slot->off += chunk;
	slot->recoveries = 0;
	deadline_set(&slot->deadline, INCR_IDLE_TIMEOUT_MS);
	XFlush(st.dpy);
	DBG("INCR chunk %zu/%zu to 0x%lx", slot->off, slot->len,
	    (unsigned long)slot->requestor);
}

/* ------------------------------------------------------------------ */
/* X11 event handling                                                  */
/* ------------------------------------------------------------------ */

/* Wait up to `timeout_ms` for an event.  SelectionRequests and INCR
 * property-deletions are serviced inline: we are usually the selection owner,
 * and letting either queue up would deadlock the peer. */
static bool x_wait_event(XEvent *ev, int timeout_ms)
{
	struct timespec t0 = now_mono();

	for (;;) {
		if (XPending(st.dpy)) {
			XNextEvent(st.dpy, ev);
			if (ev->type == SelectionRequest) {
				x_serve_request(&ev->xselectionrequest, &st.serve_x);
				continue;
			}
			if (ev->type == PropertyNotify) {
				struct incr_slot *slot =
				        incr_find(ev->xproperty.window,
				                  ev->xproperty.atom);
				if (slot && ev->xproperty.state == PropertyDelete) {
					x_incr_step(slot, &st.serve_x);
					continue;
				}
			}
			return true;
		}
		struct timespec t1 = now_mono();
		long spent = (t1.tv_sec - t0.tv_sec) * 1000 +
		             (t1.tv_nsec - t0.tv_nsec) / 1000000;
		if (spent >= timeout_ms)
			return false;

		struct pollfd p = { .fd = ConnectionNumber(st.dpy), .events = POLLIN };
		int r = poll(&p, 1, (int)(timeout_ms - spent));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
	}
}

/* Read a property, following an INCR transfer if the owner started one. */
static char *x_read_prop(Window w, Atom prop, size_t *out_len, Atom *out_type)
{
	Atom type = None;
	int  fmt = 0;
	unsigned long n = 0, after = 0;
	unsigned char *data = NULL;
	*out_len = 0;

	if (XGetWindowProperty(st.dpy, w, prop, 0, 0, False, AnyPropertyType,
	                       &type, &fmt, &n, &after, &data) != Success)
		return NULL;
	if (data) { XFree(data); data = NULL; }

	if (type == st.INCR) {
		size_t cap = INCR_CHUNK, len = 0;
		char *acc = malloc(cap);
		if (!acc)
			return NULL;
		XDeleteProperty(st.dpy, w, prop);
		XFlush(st.dpy);
		for (;;) {
			XEvent ev;
			if (!x_wait_event(&ev, TRANSFER_TIMEOUT_MS)) {
				WARN("INCR read timed out");
				free(acc);
				return NULL;
			}
			if (ev.type != PropertyNotify || ev.xproperty.atom != prop ||
			    ev.xproperty.state != PropertyNewValue)
				continue;
			unsigned char *chunk = NULL;
			Atom t2 = None; int f2 = 0; unsigned long n2 = 0, a2 = 0;
			if (XGetWindowProperty(st.dpy, w, prop, 0, 0x1FFFFFFF, True,
			                       AnyPropertyType, &t2, &f2, &n2, &a2,
			                       &chunk) != Success || !chunk)
				continue;
			size_t bytes = f2 == 8 ? n2 : (f2 == 16 ? n2 * 2 : n2 * 4);
			if (bytes == 0) { XFree(chunk); break; }
			if (len + bytes > MAX_PAYLOAD) {
				XFree(chunk);
				free(acc);
				return NULL;
			}
			if (len + bytes > cap) {
				cap = (len + bytes) * 2;
				char *tmp = realloc(acc, cap);
				if (!tmp) { XFree(chunk); free(acc); return NULL; }
				acc = tmp;
			}
			memcpy(acc + len, chunk, bytes);
			len += bytes;
			XFree(chunk);
		}
		*out_len = len;
		return acc;
	}

	if (type == None)
		return NULL;

	if (XGetWindowProperty(st.dpy, w, prop, 0, 0x1FFFFFFF, True,
	                       AnyPropertyType, &type, &fmt, &n, &after,
	                       &data) != Success || !data)
		return NULL;
	size_t bytes = fmt == 8 ? n : (fmt == 16 ? n * 2 : n * 4);
	char *copy = malloc(bytes + 1);
	if (!copy) { XFree(data); return NULL; }
	memcpy(copy, data, bytes);
	copy[bytes] = '\0';
	XFree(data);
	*out_len = bytes;
	if (out_type)
		*out_type = type;
	return copy;
}

/* Ask the current CLIPBOARD owner for `target`.
 * If we are the owner, answer from what we serve: converting to ourselves would
 * generate a SelectionRequest on our own window, and for a large payload that
 * turns into a self-referential INCR transfer that competes with real readers. */
static char *x_convert(Atom target, size_t *out_len, Atom *out_type)
{
	if (XGetSelectionOwner(st.dpy, st.CLIPBOARD) == st.win)
		return NULL;

	XDeleteProperty(st.dpy, st.win, st.SEL_PROP);
	XConvertSelection(st.dpy, st.CLIPBOARD, target, st.SEL_PROP, st.win,
	                  CurrentTime);
	XFlush(st.dpy);

	for (;;) {
		XEvent ev;
		if (!x_wait_event(&ev, TARGET_TIMEOUT_MS))
			return NULL;
		if (ev.type != SelectionNotify || ev.xselection.selection != st.CLIPBOARD)
			continue;
		if (ev.xselection.property == None)
			return NULL;
		return x_read_prop(st.win, st.SEL_PROP, out_len, out_type);
	}
}

/* Snapshot the X11 CLIPBOARD: prefer real image data, then text. */
static bool x_fetch_selection(struct buf *outp)
{
	size_t len = 0;
	Atom type = None;

	char *tl = x_convert(st.TARGETS, &len, &type);
	bool has_png = false;
	if (tl) {
		Atom *a = (Atom *)tl;
		for (size_t i = 0; i < len / sizeof(Atom); i++)
			if (a[i] == st.IMAGE_PNG)
				has_png = true;
		free(tl);
	}

	if (has_png) {
		char *d = x_convert(st.IMAGE_PNG, &len, &type);
		if (d && len > 0) {
			buf_set(outp, d, len, "image/png", hash_of(d, len));
			free(d);
			return true;
		}
		free(d);
	}

	Atom order[4] = { st.UTF8_STRING, st.TEXT_PLAIN_UTF8, st.TEXT, XA_STRING };
	for (int i = 0; i < 4; i++) {
		char *d = x_convert(order[i], &len, &type);
		if (d && len > 0) {
			buf_set(outp, d, len, "text/plain;charset=utf-8",
			        hash_of(d, len));
			free(d);
			return true;
		}
		free(d);
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* output queue (non-blocking writes into Wayland pipe fds)            */
/* ------------------------------------------------------------------ */

static void pump_outbox(void)
{
	struct outbox **pp = &st.out;
	while (*pp) {
		struct outbox *o = *pp;
		while (o->off < o->len) {
			ssize_t n = write(o->fd, o->data + o->off, o->len - o->off);
			if (n > 0) { o->off += (size_t)n; continue; }
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
			if (n < 0 && errno == EINTR) continue;
			o->off = o->len;      /* peer went away */
			break;
		}
		if (o->off >= o->len) {
			*pp = o->next;
			close(o->fd);
			free(o->data);
			free(o);
		} else {
			pp = &o->next;
		}
	}
}

static void outbox_push(int fd, const char *data, size_t len)
{
	struct outbox *o = calloc(1, sizeof(*o));
	if (!o) { close(fd); return; }
	o->fd = fd;
	o->data = malloc(len ? len : 1);
	if (!o->data) { free(o); close(fd); return; }
	if (len)
		memcpy(o->data, data, len);
	o->len = len;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	o->next = st.out;
	st.out = o;
	pump_outbox();
}

/* ------------------------------------------------------------------ */
/* X11: answering selection requests                                   */
/* ------------------------------------------------------------------ */

static void x_send_notify(XSelectionRequestEvent *rq, Atom prop)
{
	XSelectionEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SelectionNotify;
	ev.display = rq->display;
	ev.requestor = rq->requestor;
	ev.selection = rq->selection;
	ev.target = rq->target;
	ev.time = rq->time;
	ev.property = prop;
	XSendEvent(st.dpy, rq->requestor, False, NoEventMask, (XEvent *)&ev);
	XFlush(st.dpy);
}

static void x_serve_request(XSelectionRequestEvent *rq, const struct buf *b)
{
	Atom prop = rq->property ? rq->property : rq->target;

	if (!b || !b->valid) {
		x_send_notify(rq, None);
		return;
	}

	if (rq->target == st.TARGETS) {
		Atom list[8];
		int n = 0;
		list[n++] = st.TARGETS;
		list[n++] = st.TIMESTAMP;
		if (mime_is_text(b->mime)) {
			list[n++] = st.UTF8_STRING;
			list[n++] = st.TEXT;
			list[n++] = st.TEXT_PLAIN_UTF8;
			list[n++] = XA_STRING;
		} else {
			list[n++] = XInternAtom(st.dpy, b->mime ? b->mime : "image/png",
			                        False);
		}
		XChangeProperty(st.dpy, rq->requestor, prop, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)list, n);
		x_send_notify(rq, prop);
		return;
	}

	if (rq->target == st.TIMESTAMP) {
		long ts = (long)CurrentTime;
		XChangeProperty(st.dpy, rq->requestor, prop, XA_INTEGER, 32,
		                PropModeReplace, (unsigned char *)&ts, 1);
		x_send_notify(rq, prop);
		return;
	}

	bool text = mime_is_text(b->mime);
	bool ok = text
	          ? (rq->target == st.UTF8_STRING || rq->target == st.TEXT ||
	             rq->target == st.TEXT_PLAIN_UTF8 || rq->target == XA_STRING)
	          : (b->mime && rq->target == XInternAtom(st.dpy, b->mime, False));
	if (!ok) {
		x_send_notify(rq, None);
		return;
	}

	if (b->len <= DIRECT_PROP_LIMIT) {
		XChangeProperty(st.dpy, rq->requestor, prop, rq->target, 8,
		                PropModeReplace, (const unsigned char *)b->data,
		                (int)b->len);
		x_send_notify(rq, prop);
		return;
	}

	/* Too large for one request: INCR handshake. */
	/* ICCCM: each SelectionRequest begins a fresh transfer, so a repeat from
	 * the same reader restarts its stream from offset 0. */
	struct incr_slot *slot = incr_find(rq->requestor, prop);
	if (slot) {
		incr_end(slot, false);
		slot = NULL;
	}
	for (int i = 0; i < MAX_INCR; i++)
		if (!st.incr[i].active) { slot = &st.incr[i]; break; }
	if (!slot) {
		WARN("too many concurrent INCR transfers; refusing request");
		x_send_notify(rq, None);
		return;
	}

	XSelectInput(st.dpy, rq->requestor, PropertyChangeMask);
	/* ICCCM/Xt convention: the INCR marker is a 32-bit property holding the
	 * total length of the transfer.  Clients that follow Xt (the JDK among
	 * them) reject a zero-length marker with "INCR data is missed." */
	long total_len = (long)b->len;
	XChangeProperty(st.dpy, rq->requestor, prop, st.INCR, 32,
	                PropModeReplace, (unsigned char *)&total_len, 1);
	x_send_notify(rq, prop);

	slot->data = malloc(b->len ? b->len : 1);
	if (!slot->data) {
		WARN("out of memory starting INCR transfer");
		x_send_notify(rq, None);
		return;
	}
	memcpy(slot->data, b->data, b->len);
	slot->len = b->len;
	slot->active = true;
	slot->requestor = rq->requestor;
	slot->property = prop;
	slot->type = rq->target;
	slot->off = 0;
	deadline_set(&slot->deadline, INCR_IDLE_TIMEOUT_MS);
	DBG("INCR serve of %zu bytes started for 0x%lx", b->len,
	    (unsigned long)rq->requestor);
}

static void x_claim(void)
{
	if (XGetSelectionOwner(st.dpy, st.CLIPBOARD) != st.win) {
		XSetSelectionOwner(st.dpy, st.CLIPBOARD, st.win, CurrentTime);
		XFlush(st.dpy);
		if (XGetSelectionOwner(st.dpy, st.CLIPBOARD) != st.win)
			WARN("could not take X11 CLIPBOARD ownership");
	}
}

/* ------------------------------------------------------------------ */
/* Wayland: publishing                                                 */
/* ------------------------------------------------------------------ */

static void dc_src_send(void *data, struct ext_data_control_source_v1 *src,
                        const char *mime, int32_t fd)
{
	(void)src;
	const struct buf *b = data;
	if (!b || !b->valid) { close(fd); return; }
	DBG("wayland peer asked for '%s' (%zu bytes)", mime, b->len);
	outbox_push(fd, b->data, b->len);
}

static void dc_src_cancelled(void *data, struct ext_data_control_source_v1 *src)
{
	(void)data;
	DBG("wayland source superseded");
	ext_data_control_source_v1_destroy(src);
	if (st.dcsrc == src)
		st.dcsrc = NULL;
}

static const struct ext_data_control_source_v1_listener dc_src_listener = {
	.send = dc_src_send,
	.cancelled = dc_src_cancelled,
};

/* Publish st.serve_wl on the Wayland clipboard. */
static void wl_publish(void)
{
	const struct buf *b = &st.serve_wl;
	if (!st.dcm || !st.dcdev || !b->valid)
		return;

	struct ext_data_control_source_v1 *src =
	        ext_data_control_manager_v1_create_data_source(st.dcm);
	if (!src) { ERRF("cannot create a data source"); return; }
	ext_data_control_source_v1_add_listener(src, &dc_src_listener, (void *)b);

	if (mime_is_text(b->mime)) {
		ext_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
		ext_data_control_source_v1_offer(src, "text/plain");
		ext_data_control_source_v1_offer(src, "UTF8_STRING");
		ext_data_control_source_v1_offer(src, "TEXT");
		ext_data_control_source_v1_offer(src, "STRING");
	} else {
		ext_data_control_source_v1_offer(src, b->mime ? b->mime : "image/png");
	}

	/* The previous source is superseded by set_selection; the compositor
	 * answers with `cancelled`, which is where we free it. */
	st.dcsrc = src;
	ext_data_control_device_v1_set_selection(st.dcdev, src);
	wl_display_flush(st.wl);
}

/* ------------------------------------------------------------------ */
/* Wayland: receiving                                                  */
/* ------------------------------------------------------------------ */

static void mimes_add(char ***arr, size_t *n, const char *mime)
{
	char **tmp = realloc(*arr, (*n + 1) * sizeof(char *));
	if (!tmp)
		return;
	tmp[*n] = strdup(mime);
	*arr = tmp;
	(*n)++;
}

static void mimes_free(char ***arr, size_t *n)
{
	for (size_t i = 0; i < *n; i++)
		free((*arr)[i]);
	free(*arr);
	*arr = NULL;
	*n = 0;
}

static void dc_offer_offer(void *data, struct ext_data_control_offer_v1 *offer,
                           const char *mime)
{
	(void)data;
	if (offer == st.pending_offer)
		mimes_add(&st.pending_mimes, &st.n_pending_mimes, mime);
}

static const struct ext_data_control_offer_v1_listener dc_offer_listener = {
	.offer = dc_offer_offer,
};

static void dc_dev_data_offer(void *data, struct ext_data_control_device_v1 *dev,
                              struct ext_data_control_offer_v1 *id)
{
	(void)data; (void)dev;
	st.pending_offer = id;
	mimes_free(&st.pending_mimes, &st.n_pending_mimes);
	ext_data_control_offer_v1_add_listener(id, &dc_offer_listener, NULL);
}

static void dc_dev_selection(void *data, struct ext_data_control_device_v1 *dev,
                             struct ext_data_control_offer_v1 *id)
{
	(void)data; (void)dev;

	if (st.dc_offer && st.dc_offer != id) {
		if (st.fetch_in_flight) {
			/* do not pull the rug out from under receive() */
			st.retire_offer = st.dc_offer;
		} else {
			ext_data_control_offer_v1_destroy(st.dc_offer);
		}
		mimes_free(&st.offer_mimes, &st.n_offer_mimes);
	}
	st.dc_offer = id;

	if (id && id == st.pending_offer) {
		mimes_free(&st.offer_mimes, &st.n_offer_mimes);
		st.offer_mimes = st.pending_mimes;
		st.n_offer_mimes = st.n_pending_mimes;
		st.pending_mimes = NULL;
		st.n_pending_mimes = 0;
		st.pending_offer = NULL;
	} else if (!id) {
		mimes_free(&st.offer_mimes, &st.n_offer_mimes);
	}
	st.wl_sel_dirty = true;
	DBG("wayland selection changed (%zu mime types)", st.n_offer_mimes);
}

static void dc_dev_finished(void *data, struct ext_data_control_device_v1 *dev)
{
	(void)data; (void)dev;
	WARN("compositor invalidated our data-control device; exiting");
	st.running = false;
}

static void dc_dev_primary(void *data, struct ext_data_control_device_v1 *dev,
                           struct ext_data_control_offer_v1 *id)
{
	(void)data; (void)dev;
	if (id)
		ext_data_control_offer_v1_destroy(id);
}

static const struct ext_data_control_device_v1_listener dc_dev_listener = {
	.data_offer = dc_dev_data_offer,
	.selection = dc_dev_selection,
	.finished = dc_dev_finished,
	.primary_selection = dc_dev_primary,
};

static const char *wl_pick_mime(void)
{
	static const char *prio[] = {
		"image/png",
		"text/plain;charset=utf-8",
		"text/plain;charset=UTF-8",
		"UTF8_STRING",
		"text/plain",
		"TEXT",
		"STRING",
		"text/plain;charset=us-ascii",
	};
	for (size_t p = 0; p < sizeof(prio) / sizeof(prio[0]); p++)
		for (size_t i = 0; i < st.n_offer_mimes; i++)
			if (st.offer_mimes[i] &&
			    strcasecmp(st.offer_mimes[i], prio[p]) == 0)
				return st.offer_mimes[i];
	for (size_t i = 0; i < st.n_offer_mimes; i++)
		if (st.offer_mimes[i] &&
		    strncasecmp(st.offer_mimes[i], "text/", 5) == 0)
			return st.offer_mimes[i];
	return NULL;
}

/* Service the Wayland connection for up to `timeout_ms`.
 * wl_display_dispatch_pending() alone never reads the socket, so a plain
 * dispatch_pending loop would never observe new events. */
static void wl_pump(int timeout_ms)
{
	if (!st.wl)
		return;
	struct pollfd p = { .fd = wl_display_get_fd(st.wl), .events = POLLIN };
	if (poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN)) {
		if (wl_display_dispatch(st.wl) < 0) {
			int err = wl_display_get_error(st.wl);
			ERRF("wayland dispatch failed: %s",
			     err == EPROTO ? "protocol error" : strerror(err));
			st.running = false;
			return;
		}
	} else {
		wl_display_dispatch_pending(st.wl);
	}
	wl_display_flush(st.wl);
}

/* Advance in-flight INCR transfers without taking on new work.  This is what
 * wl_fetch() is allowed to do on the X connection: starting a new transfer or
 * answering a fresh SelectionRequest here would interleave with the transfer
 * already in progress. */
static void x_step_incr_only(void)
{
	XEvent ev;
	while (XPending(st.dpy)) {
		XNextEvent(st.dpy, &ev);
		if (ev.type == PropertyNotify) {
			struct incr_slot *slot =
			        incr_find(ev.xproperty.window, ev.xproperty.atom);
			if (slot && ev.xproperty.state == PropertyDelete)
				x_incr_step(slot, &st.serve_x);
		}
	}
}

static bool wl_fetch(struct buf *outp)
{
	if (!st.dc_offer)
		return false;
	const char *mime = wl_pick_mime();
	if (!mime)
		return false;

	int fds[2];
	if (pipe(fds) != 0)
		return false;

	st.in_use_offer = st.dc_offer;
	st.fetch_in_flight = true;
	ext_data_control_offer_v1_receive(st.dc_offer, mime, fds[1]);
	close(fds[1]);
	wl_display_flush(st.wl);

	size_t cap = 65536, len = 0;
	char *acc = malloc(cap);
	if (!acc) { close(fds[0]); return false; }

	struct timespec t0 = now_mono();
	for (;;) {
		/* Watch the pipe AND the X connection.  A slow or stalled Wayland
		 * peer must never stop us answering X11 selection requests: an
		 * in-flight INCR transfer would deadlock the reading application. */
		struct pollfd pfds[2] = {
			{ .fd = fds[0],             .events = POLLIN },
			{ .fd = ConnectionNumber(st.dpy), .events = POLLIN },
		};
		int r = poll(pfds, 2, 200);
		if (r > 0 && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)))
			x_step_incr_only();
		/* POLLHUP means the writer closed: there may still be buffered bytes,
		 * so drain the pipe and then stop. */
		if (r > 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			if (len + 65536 > cap) {
				cap *= 2;
				char *t = realloc(acc, cap);
				if (!t) { free(acc); close(fds[0]); return false; }
				acc = t;
			}
			ssize_t n = read(fds[0], acc + len, cap - len);
			if (n > 0) { len += (size_t)n; continue; }
			if (n == 0) break;                       /* clean EOF */
			if (errno == EINTR) continue;
			if (errno == EAGAIN) {
				if (pfds[0].revents & (POLLHUP | POLLERR))
					break;
				continue;
			}
			break;
		}
		/* Keep dispatching Wayland events.  A selection event arriving here
		 * replaces st.dc_offer, but the offer this receive() reads from is
		 * pinned via fetch_in_flight/retire_offer and only destroyed in
		 * fetch_done() — safe.  Dispatching is also REQUIRED for a
		 * self-source round-trip (--selftest): nobody else will answer the
		 * `send` request on our own data source. */
		wl_pump(20);
		pump_outbox();

		struct timespec t1 = now_mono();
		long ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		          (t1.tv_nsec - t0.tv_nsec) / 1000000;
		if (ms > TRANSFER_TIMEOUT_MS) {
			WARN("timed out reading the wayland selection");
			free(acc);
			close(fds[0]);
			return false;
		}
	}
	close(fds[0]);

	if (len == 0 || len > MAX_PAYLOAD) {
		free(acc);
		fetch_done();
		return false;
	}
	const char *tag = mime_is_text(mime) ? "text/plain;charset=utf-8"
	                                     : (strcasecmp(mime, "image/bmp") == 0
	                                            ? "image/bmp" : "image/png");
	buf_set(outp, acc, len, tag, hash_of(acc, len));
	free(acc);
	fetch_done();
	return true;
}

static void fetch_done(void)
{
	st.fetch_in_flight = false;
	st.in_use_offer = NULL;
	if (st.retire_offer) {
		ext_data_control_offer_v1_destroy(st.retire_offer);
		st.retire_offer = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Wayland: registry                                                   */
/* ------------------------------------------------------------------ */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_seat_interface.name) == 0 && !st.seat) {
		st.seat = wl_registry_bind(reg, name, &wl_seat_interface,
		                           version < 5 ? version : 5);
	} else if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0 &&
	           !st.dcm) {
		st.dcm = wl_registry_bind(reg, name,
		                          &ext_data_control_manager_v1_interface, 1);
		INFO("bound ext_data_control_manager_v1");
	}
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

/* A reader may vanish mid-transfer (BadWindow on ChangeProperty), and a
 * concurrent owner may vanish mid-read.  Neither should kill the daemon, so
 * errors are logged at debug level and otherwise swallowed. */
static int x_error_handler(Display *dpy, XErrorEvent *e)
{
	char text[160] = "";
	g_xerr = e->error_code;
	g_xerr_resource = (unsigned long)e->resourceid;
	XGetErrorText(dpy, e->error_code, text, sizeof(text));
	DBG("X error: %s (request %d.%d, resource 0x%lx)", text,
	    e->request_code, e->minor_code, g_xerr_resource);
	return 0;
}

static bool x_setup(void)
{
	st.dpy = XOpenDisplay(NULL);
	if (!st.dpy) {
		ERRF("cannot open the X display (DISPLAY=%s)",
		     getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		return false;
	}
	XSetErrorHandler(x_error_handler);
	st.root = DefaultRootWindow(st.dpy);
	st.win = XCreateSimpleWindow(st.dpy, st.root, 0, 0, 1, 1, 0, 0, 0);
	XSelectInput(st.dpy, st.win, PropertyChangeMask | StructureNotifyMask);
	XStoreName(st.dpy, st.win, "cbfix-bridge");
	INFO("bridge X11 window = 0x%lx", (unsigned long)st.win);

	int eb = 0, errb = 0;
	st.have_xfixes = XFixesQueryExtension(st.dpy, &eb, &errb);
	if (st.have_xfixes)
		st.xfixes_ev = eb;
	XFlush(st.dpy);
	return true;
}

static bool wl_setup(void)
{
	st.wl = wl_display_connect(NULL);
	if (!st.wl) {
		ERRF("cannot connect to the Wayland compositor (WAYLAND_DISPLAY=%s)",
		     getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
		return false;
	}
	st.registry = wl_display_get_registry(st.wl);
	wl_registry_add_listener(st.registry, &reg_listener, NULL);
	wl_display_roundtrip(st.wl);

	if (!st.dcm) {
		ERRF("the compositor does not offer ext-data-control-v1.");
		ERRF("That protocol is required: without it a background client cannot");
		ERRF("write the Wayland clipboard, because wl_data_device.set_selection");
		ERRF("needs a recent keyboard-focus serial.");
		ERRF("niri >= 25.05, sway >= 1.10, Hyprland >= 0.40, KWin >= 6.3 ship it.");
		return false;
	}
	if (!st.seat) {
		ERRF("no wl_seat available");
		return false;
	}
	st.dcdev = ext_data_control_manager_v1_get_data_device(st.dcm, st.seat);
	ext_data_control_device_v1_add_listener(st.dcdev, &dc_dev_listener, NULL);
	wl_display_roundtrip(st.wl);
	return true;
}

static bool configure(void)
{
	if (st.configured)
		return true;
	if (!x_setup())
		return false;

	st.CLIPBOARD = XInternAtom(st.dpy, "CLIPBOARD", False);
	st.TARGETS = XInternAtom(st.dpy, "TARGETS", False);
	st.TIMESTAMP = XInternAtom(st.dpy, "TIMESTAMP", False);
	st.UTF8_STRING = XInternAtom(st.dpy, "UTF8_STRING", False);
	st.TEXT = XInternAtom(st.dpy, "TEXT", False);
	st.INCR = XInternAtom(st.dpy, "INCR", False);
	st.TEXT_PLAIN_UTF8 = XInternAtom(st.dpy, "text/plain;charset=utf-8", False);
	st.SEL_PROP = XInternAtom(st.dpy, "CBFIX_SELECTION", False);
	st.IMAGE_PNG = XInternAtom(st.dpy, "image/png", False);

	if (st.have_xfixes)
		XFixesSelectSelectionInput(st.dpy, st.win, st.CLIPBOARD,
		                           XFixesSetSelectionOwnerNotifyMask |
		                           XFixesSelectionWindowDestroyNotifyMask |
		                           XFixesSelectionClientCloseNotifyMask);
	XFlush(st.dpy);

	if (!wl_setup())
		return false;
	st.configured = true;
	return true;
}

static void cleanup(void)
{
	if (st.dpy) {
		XEvent ev;
		int guard = 0;
		while (XPending(st.dpy) && guard++ < 200) {
			XNextEvent(st.dpy, &ev);
			if (ev.type == SelectionRequest)
				x_serve_request(&ev.xselectionrequest, &st.serve_x);
		}
	}
	pump_outbox();
	while (st.out) {
		struct outbox *o = st.out;
		st.out = o->next;
		close(o->fd);
		free(o->data);
		free(o);
	}
	if (st.dcsrc) ext_data_control_source_v1_destroy(st.dcsrc);
	if (st.dc_offer) ext_data_control_offer_v1_destroy(st.dc_offer);
	if (st.dcdev) ext_data_control_device_v1_destroy(st.dcdev);
	if (st.dcm) ext_data_control_manager_v1_destroy(st.dcm);
	if (st.wl) { wl_display_flush(st.wl); wl_display_disconnect(st.wl); }
	if (st.dpy) XCloseDisplay(st.dpy);
	mimes_free(&st.offer_mimes, &st.n_offer_mimes);
	mimes_free(&st.pending_mimes, &st.n_pending_mimes);
	buf_clear(&st.serve_x);
	buf_clear(&st.serve_wl);
	INFO("stopped (%d X11->WL, %d WL->X11)", st.stat_x2wl, st.stat_wl2x);
}

/* ------------------------------------------------------------------ */
/* synchronisation                                                     */
/* ------------------------------------------------------------------ */

static void x_drain_events(void)
{
	XEvent ev;
	while (XPending(st.dpy)) {
		XNextEvent(st.dpy, &ev);
		switch (ev.type) {
		case SelectionRequest:
			DBG("SelectionRequest from 0x%lx target=%lu prop=%lu",
			    (unsigned long)ev.xselectionrequest.requestor,
			    (unsigned long)ev.xselectionrequest.target,
			    (unsigned long)ev.xselectionrequest.property);
			x_serve_request(&ev.xselectionrequest, &st.serve_x);
			break;
		case SelectionClear:
			if (ev.xselectionclear.selection == st.CLIPBOARD)
				DBG("lost X11 CLIPBOARD ownership");
			break;
		case PropertyNotify: {
			struct incr_slot *slot =
			        incr_find(ev.xproperty.window, ev.xproperty.atom);
			DBG("PN win=0x%lx atom=%lu state=%s slot=%s",
			    (unsigned long)ev.xproperty.window,
			    (unsigned long)ev.xproperty.atom,
			    ev.xproperty.state == PropertyNewValue ? "New" : "Del",
			    slot ? "found" : "none");
			if (slot && ev.xproperty.state == PropertyDelete)
				x_incr_step(slot, &st.serve_x);
			break;
		}
		case SelectionNotify:
			DBG("SelectionNotify requestor-win=0x%lx prop=%lu",
			    (unsigned long)ev.xselection.requestor,
			    (unsigned long)ev.xselection.property);
			break;
		default:
			if (st.have_xfixes &&
			    ev.type == st.xfixes_ev + XFixesSelectionNotify) {
				XFixesSelectionNotifyEvent *x =
				        (XFixesSelectionNotifyEvent *)&ev;
				if (x->selection == st.CLIPBOARD &&
				    x->subtype == XFixesSetSelectionOwnerNotify &&
				    x->owner != st.win) {
					DBG("xfixes: CLIPBOARD owner is now 0x%lx",
					    (unsigned long)x->owner);
					st.x11_dirty = true;
				}
			}
			break;
		}
	}
}

static void sync_pass(void)
{
	Window owner = XGetSelectionOwner(st.dpy, st.CLIPBOARD);

	if (owner != st.tracked_owner) {
		if (owner != None)
			st.x11_dirty = true;
		st.tracked_owner = owner;
	}

	/* ---- X11 -> Wayland ---- */
	if (st.x11_dirty) {
		st.x11_dirty = false;
		if (owner != None && owner != st.win) {
			struct buf got;
			memset(&got, 0, sizeof(got));
			if (x_fetch_selection(&got)) {
				if (!(st.have_pushed && got.hash == st.pushed_hash)) {
					buf_set(&st.serve_wl, got.data, got.len,
					        got.mime, got.hash);
					wl_publish();
					st.pushed_hash = got.hash;
					st.have_pushed = true;
					st.stat_x2wl++;
					INFO("X11 -> Wayland : %zu bytes (%s)", got.len,
					     mime_is_text(got.mime) ? "text" : "image");
				} else {
					DBG("X11 change was our own echo; ignored");
				}
			} else {
				WARN("could not read the X11 CLIPBOARD");
			}
			buf_clear(&got);
		}
	}

	/* ---- Wayland -> X11 ---- */
	/* No deferral while an X11 INCR transfer is in flight: wl_fetch() now
	 * services X events (x_step_incr_only) and the pinned per-slot payload
	 * makes mid-transfer buffer swaps safe.  Deferring here instead froze all
	 * clipboard updates whenever a client read via INCR and never deleted the
	 * terminating property (the JDK does exactly that). */
	if (st.wl_sel_dirty) {
		st.wl_sel_dirty = false;   /* consumed only because we read now */
		struct buf got;
		memset(&got, 0, sizeof(got));
		if (wl_fetch(&got)) {
			if (!(st.have_pushed && got.hash == st.pushed_hash)) {
				buf_set(&st.serve_x, got.data, got.len,
				        got.mime, got.hash);
				x_claim();
				st.tracked_owner = st.win;
				st.pushed_hash = got.hash;
				st.have_pushed = true;
				st.stat_wl2x++;
				INFO("Wayland -> X11 : %zu bytes (%s)", got.len,
				     mime_is_text(got.mime) ? "text" : "image");
			} else {
				DBG("Wayland change was our own echo; ignored");
			}
		}
		buf_clear(&got);
	}

	/* stay sticky: if we hold content and X11 lost its owner, reclaim */
	owner = XGetSelectionOwner(st.dpy, st.CLIPBOARD);
	if (st.serve_x.valid && owner == None) {
		x_claim();
		st.tracked_owner = st.win;
	}
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

static bool wl_self_roundtrip(void)
{
	const char *msg = "cbfix-selftest-wayland";
	buf_set(&st.serve_wl, msg, strlen(msg), "text/plain;charset=utf-8",
	        hash_of(msg, strlen(msg)));

	/* The compositor delivers an initial (possibly empty) selection event when
	 * the device is bound; clear the flag so we wait for the one we cause. */
	st.wl_sel_dirty = false;
	wl_publish();

	for (int i = 0; i < 200 && !(st.wl_sel_dirty && st.dc_offer); i++) {
		wl_pump(50);
		pump_outbox();
	}

	struct buf rb;
	memset(&rb, 0, sizeof(rb));
	bool ok = wl_fetch(&rb) && rb.len == strlen(msg) &&
	          memcmp(rb.data, msg, rb.len) == 0;
	buf_clear(&rb);
	return ok;
}

/* Read the selection back through a completely separate X11 client. */
static bool x_second_client_roundtrip(const char *msg)
{
	buf_set(&st.serve_x, msg, strlen(msg), "text/plain;charset=utf-8",
	        hash_of(msg, strlen(msg)));
	XSetSelectionOwner(st.dpy, st.CLIPBOARD, st.win, CurrentTime);
	XFlush(st.dpy);
	if (XGetSelectionOwner(st.dpy, st.CLIPBOARD) != st.win)
		return false;

	Display *d2 = XOpenDisplay(NULL);
	if (!d2)
		return false;

	Window w2 = XCreateSimpleWindow(d2, DefaultRootWindow(d2), 0, 0, 1, 1, 0, 0, 0);
	Atom prop = XInternAtom(d2, "CBFIX_TEST", False);
	Atom utf8 = XInternAtom(d2, "UTF8_STRING", False);
	XSelectInput(d2, w2, PropertyChangeMask);
	XConvertSelection(d2, st.CLIPBOARD, utf8, prop, w2, CurrentTime);
	XFlush(d2);

	bool ok = false;
	for (int i = 0; i < 300 && !ok; i++) {
		XEvent e;
		while (XPending(st.dpy)) {
			XNextEvent(st.dpy, &e);
			if (e.type == SelectionRequest)
				x_serve_request(&e.xselectionrequest, &st.serve_x);
		}
		XFlush(st.dpy);
		while (XPending(d2))
			XNextEvent(d2, &e);

		Atom t; int f; unsigned long n, a;
		unsigned char *d3 = NULL;
		if (XGetWindowProperty(d2, w2, prop, 0, 4096, True, AnyPropertyType,
		                       &t, &f, &n, &a, &d3) == Success && d3 && n > 0) {
			ok = (n == strlen(msg) && memcmp(d3, msg, n) == 0);
			XFree(d3);
		}
		if (!ok)
			usleep(10000);
	}
	XCloseDisplay(d2);
	return ok;
}

static int selftest(void)
{
	int fails = 0;
	printf("cbfix-bridge selftest\n");
	printf("  DISPLAY         : %s\n", XDisplayName(NULL));
	printf("  WAYLAND_DISPLAY : %s\n",
	       getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");

	if (!configure()) {
		printf("  [FAIL] environment setup\n");
		return 1;
	}
	printf("  [ OK ] X11 connected (XFixes %s)\n", st.have_xfixes ? "yes" : "no");
	printf("  [ OK ] Wayland connected, ext-data-control-v1 bound\n");

	bool a = wl_self_roundtrip();
	printf("  [%s] Wayland publish -> read back\n", a ? " OK " : "FAIL");
	if (!a) fails++;

	bool b = x_second_client_roundtrip("cbfix-selftest-x11");
	printf("  [%s] X11 second client reads our selection\n", b ? " OK " : "FAIL");
	if (!b) fails++;

	printf("%s\n", fails ? "SELFTEST FAILED" : "selftest passed");
	return fails ? 1 : 0;
}

static int status_cmd(void)
{
	if (!configure())
		return 1;

	Window owner = XGetSelectionOwner(st.dpy, st.CLIPBOARD);
	printf("X11 CLIPBOARD owner : 0x%lx%s\n", (unsigned long)owner,
	       owner == None ? "  (none)" : "");
	printf("Wayland mime types  : ");
	if (!st.dc_offer) {
		printf("(no offer)\n");
	} else {
		for (size_t i = 0; i < st.n_offer_mimes; i++)
			printf("%s%s", i ? ", " : "", st.offer_mimes[i]);
		printf("\n");
	}
	struct buf b;
	memset(&b, 0, sizeof(b));
	if (wl_fetch(&b))
		printf("Wayland content     : %zu bytes (%s)\n", b.len,
		       mime_is_text(b.mime) ? "text" : "image");
	else
		printf("Wayland content     : (unreadable)\n");
	buf_clear(&b);
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	printf("Usage: %s [options]\n"
	       "  --interval MS   fallback X11 owner polling interval (default %d)\n"
	       "  --verbose, -v   verbose logging\n"
	       "  --selftest      check the environment and exit\n"
	       "  --status        print the current clipboard state and exit\n"
	       "  --help, -h      show this help\n",
	       argv0, DEFAULT_INTERVAL_MS);
}

static void on_signal(int sig)
{
	(void)sig;
	st.running = false;
}

int main(int argc, char **argv)
{
	st.interval_ms = DEFAULT_INTERVAL_MS;
	bool do_selftest = false, do_status = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
			g_verbose = 1;
		} else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
			st.interval_ms = atoi(argv[++i]);
			if (st.interval_ms < 20)
				st.interval_ms = 20;
		} else if (!strcmp(argv[i], "--selftest")) {
			do_selftest = true;
		} else if (!strcmp(argv[i], "--status")) {
			do_status = true;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (!configure())
		return 1;

	if (do_selftest)
		return selftest();
	if (do_status)
		return status_cmd();

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	st.running = true;

	INFO("bridge running (poll interval %d ms)", st.interval_ms);

	int wlfd = wl_display_get_fd(st.wl);
	int xfd = ConnectionNumber(st.dpy);

	/* Adopt the Wayland clipboard first so the X11 echo check is armed. */
	st.wl_sel_dirty = true;
	sync_pass();

	while (st.running) {
		/* Drain FIRST, poll second.  sync_pass() makes synchronous
		 * round-trips to the X server (XGetSelectionOwner, selection
		 * conversions), which silently pull already-arrived events into
		 * Xlib's internal queue.  The socket then never becomes readable
		 * again, so a poll()-driven loop that only drains on POLLIN sleeps
		 * forever with events stuck in the queue — observed as an INCR
		 * transfer frozen mid-stream.  XPending() is the authority on
		 * whether X work exists, not poll(). */
		x_drain_events();

		struct pollfd fds[64];
		int nf = 0;
		fds[nf].fd = wlfd; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++;
		fds[nf].fd = xfd;  fds[nf].events = POLLIN; fds[nf].revents = 0; nf++;
		for (struct outbox *o = st.out; o && nf < 64; o = o->next) {
			fds[nf].fd = o->fd;
			fds[nf].events = POLLOUT;
			fds[nf].revents = 0;
			nf++;
		}

		int r = poll(fds, nf, st.interval_ms);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			ERRF("poll: %s", strerror(errno));
			break;
		}

		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_dispatch(st.wl) < 0) {
				ERRF("wayland connection lost");
				break;
			}
		}
		if (fds[1].revents & (POLLIN | POLLERR | POLLHUP))
			x_drain_events();

		if (g_verbose) {
			static long iter;
			if ((++iter % 40) == 0) {
				int act = 0;
				for (int i = 0; i < MAX_INCR; i++) act += st.incr[i].active;
				DBG("loop iter=%ld r=%d xfd=0x%x active_incr=%d",
				    iter, r, fds[1].revents, act);
			}
		}
		pump_outbox();
		incr_recover();
		sync_pass();
	}

	cleanup();
	return 0;
}
