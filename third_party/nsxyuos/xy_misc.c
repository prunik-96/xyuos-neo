/* The scheduler, and the odds and ends NetSurf asks the system about.
 *
 * NetSurf does not own a thread and does not block: it asks to be called back
 * in so many milliseconds and gets on with something else. Fetch polling,
 * animated images, the reflow after a stylesheet arrives -- all of it comes
 * back through here. So this list is the browser's heartbeat, and the event
 * loop's job is to run it often enough and then wait exactly as long as the
 * next entry allows, rather than spinning.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/misc.h"

#include "xy_front.h"

#define XY_SCHED_MAX 64

struct entry {
    void (*cb)(void *p);
    void  *p;
    unsigned when;              /* uptime_ms at which it is due */
    bool   live;
};

static struct entry sched[XY_SCHED_MAX];

static nserror xy_schedule(int t, void (*callback)(void *p), void *p) {
    /* A negative interval withdraws the callback. NetSurf leans on this: it
     * cancels and re-arms the same callback constantly, and a scheduler that
     * treated the two as different would grow an entry per reflow. */
    for (int i = 0; i < XY_SCHED_MAX; i++) {
        if (sched[i].live && sched[i].cb == callback && sched[i].p == p) {
            if (t < 0) { sched[i].live = false; return NSERROR_OK; }
            sched[i].when = uptime_ms() + (unsigned)t;
            return NSERROR_OK;
        }
    }
    if (t < 0) return NSERROR_OK;              /* nothing to withdraw */

    for (int i = 0; i < XY_SCHED_MAX; i++) {
        if (!sched[i].live) {
            sched[i].cb = callback;
            sched[i].p = p;
            sched[i].when = uptime_ms() + (unsigned)t;
            sched[i].live = true;
            return NSERROR_OK;
        }
    }
    NSLOG(netsurf, WARNING, "the schedule is full; a callback was dropped");
    return NSERROR_NOSPACE;
}

/* Run everything that is due. An entry is taken off the list before it is
 * called, because a callback very often schedules itself again and would
 * otherwise be overwritten by its own re-arming. */
void xy_schedule_run(void) {
    unsigned now = uptime_ms();

    for (int i = 0; i < XY_SCHED_MAX; i++) {
        if (!sched[i].live) continue;
        /* Unsigned subtraction, so the comparison still holds when the
         * millisecond counter wraps. */
        if ((int)(now - sched[i].when) < 0) continue;

        void (*cb)(void *) = sched[i].cb;
        void *p = sched[i].p;
        sched[i].live = false;
        cb(p);
    }
}

/* How long the loop may wait before something is due. */
int xy_schedule_next_ms(void) {
    unsigned now = uptime_ms();
    int soonest = 1000;                        /* never sleep longer */

    for (int i = 0; i < XY_SCHED_MAX; i++) {
        if (!sched[i].live) continue;
        int left = (int)(sched[i].when - now);
        if (left < 0) left = 0;
        if (left < soonest) soonest = left;
    }
    return soonest;
}

/* --- the rest of the table ----------------------------------------------- */

static nserror xy_warning(const char *message, const char *detail) {
    /* The message line at the top of the window is where a person will look,
     * so that is where this goes as well as into the log. */
    NSLOG(netsurf, WARNING, "%s%s%s", message,
          detail ? ": " : "", detail ? detail : "");
    if (xy_win != NULL)
        snprintf(xy_win->status, sizeof xy_win->status, "%s%s%s", message,
                 detail ? ": " : "", detail ? detail : "");
    return NSERROR_OK;
}

static nserror xy_launch_url(struct nsurl *url) {
    /* There is nothing else to hand a link to: this is the browser. */
    (void)url;
    return NSERROR_NOT_IMPLEMENTED;
}

static nserror xy_cert_verify(struct nsurl *url,
                              const struct ssl_cert_info *certs,
                              unsigned long num,
                              nserror (*cb)(bool proceed, void *pw),
                              void *cbpw) {
    (void)url; (void)certs; (void)num;
    /* This cannot be answered honestly yet. The TLS in this kernel encrypts
     * the connection and does not check who is at the other end, so there is
     * no certificate here to have failed, and no question to put to anyone.
     * Saying no is the only answer that is not a claim. */
    return cb(false, cbpw);
}

struct gui_misc_table xy_misc_table = {
    .schedule    = xy_schedule,
    .warning     = xy_warning,
    .launch_url  = xy_launch_url,
    .cert_verify = xy_cert_verify,
};
