/* dgpsip.c -- gather and dispatch DGPS data from DGPSIP servers */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "gpsd.h"

/*@ -branchstate */
int dgpsip_open(struct gps_context_t *context, const char *dgpsserver)
/* open a connection to a DGPSIP server */
{
    char hn[256], buf[BUFSIZ];
    char *colon, *dgpsport = "rtcm-sc104";
    int opts;

    if ((colon = strchr(dgpsserver, ':'))) {
	dgpsport = colon+1;
	*colon = '\0';
    }
    if (!getservbyname(dgpsport, "tcp"))
	dgpsport = "2101";

    context->dsock = netlib_connectsock(dgpsserver, dgpsport, "tcp");
    if (context->dsock >= 0) {
	(void)gethostname(hn, sizeof(hn));
	(void)snprintf(buf,sizeof(buf), "HELO %s gpsd %s\r\nR\r\n",hn,VERSION);
	(void)write(context->dsock, buf, strlen(buf));
    } else
	gpsd_report(1, "Can't connect to DGPS server, netlib error %d\n", context->dsock);
    opts = fcntl(context->dsock, F_GETFL);

    if (opts >= 0)
	(void)fcntl(context->dsock, F_SETFL, opts | O_NONBLOCK);
    return context->dsock;
}
/*@ +branchstate */

void dgpsip_poll(struct gps_context_t *context)
/* poll the DGPSIP server for a correction report */
{
    if (context->dsock > -1) {
	context->rtcmbytes = read(context->dsock, context->rtcmbuf, sizeof(context->rtcmbuf));
	if (context->rtcmbytes < 0 && errno != EAGAIN)
	    gpsd_report(1, "Read from rtcm source failed\n");
	else
	    context->rtcmtime = timestamp();
    }
}

void dgpsip_relay(struct gps_device_t *session)
/* pass a DGPSIP connection report to a session */
{
    if (session->gpsdata.gps_fd !=-1 && session->context->rtcmbytes > -1
			&& session->rtcmtime < session->context->rtcmtime) {
	if (session->device_type->rtcm_writer(session, 
					      session->context->rtcmbuf, 
					      (size_t)session->context->rtcmbytes) == 0)
	    gpsd_report(1, "Write to rtcm sink failed\n");
	else { 
	    session->rtcmtime = timestamp();
	    gpsd_report(2, "<= DGPS: %d bytes of RTCM relayed.\n", session->context->rtcmbytes);
	}
    }
}

void dgpsip_report(struct gps_device_t *session)
/* may be time to ship a usage report to the DGPSIP server */
{
    /*
     * 10 is an arbitrary number, the point is to have gotten several good
     * fixes before reporting usage to our DGPSIP server.
     */
    if (session->context->fixcnt > 10 && !session->context->sentdgps) {
	session->context->sentdgps = true;
	if (session->context->dsock > -1) {
	    char buf[BUFSIZ];
	    (void)snprintf(buf, sizeof(buf), "R %0.8f %0.8f %0.2f\r\n", 
			   session->gpsdata.fix.latitude, 
			   session->gpsdata.fix.longitude, 
			   session->gpsdata.fix.altitude);
	    (void)write(session->context->dsock, buf, strlen(buf));
	    gpsd_report(2, "=> dgps %s", buf);
	}
    }
}

/* maximum distance from DGPS server for it to be useful (meters) */
#define DGPS_THRESHOLD	1600000

void dgpsip_autoconnect(struct gps_context_t *context,
			double lat, double lon,
			const char *serverlist)
/* tell the library to talk to the nearest DGPSIP server */
{
    struct dgps_server_t {
	double lat, lon;
	char server[257];
	double dist;
    } keep, hold;
    char buf[BUFSIZ];
    FILE *sfp = fopen(serverlist, "r");

    if (sfp == NULL) {
	gpsd_report(1, "no DGPS server list found.\n");
	context->dsock = -2;	/* don't try this again */
	return;
    }

    keep.dist = DGPS_THRESHOLD;
    keep.server[0] = '\0';
    while (fgets(buf, (int)sizeof(buf), sfp)) {
	char *cp = strchr(buf, '#');
	if (cp)
	    *cp = '\0';
	if (sscanf(buf,"%lf %lf %256s",&hold.lat, &hold.lon, hold.server)==3) {
	    hold.dist = earth_distance(lat, lon, hold.lat, hold.lon);
	    if (hold.dist < keep.dist)
		memcpy(&keep, &hold, sizeof(struct dgps_server_t));
	}
    }
    (void)fclose(sfp);

    if (keep.server[0] == '\0') {
	gpsd_report(1, "no DGPS server within %d m\n", DGPS_THRESHOLD);
	context->dsock = -2;	/* don't try this again */
	return;
    }

    (void)dgpsip_open(context, keep.server);
}
