/* GPS speedometer as a wrapper around an Athena widget Tachometer
 * - Derrick J Brashear <shadow@dementia.org>
 * Tachometer widget from Kerberometer (xklife)
 */
#include "config.h"

#include <unistd.h>
#include <stdlib.h>
#if defined(HAVE_GETOPT_H)
#include <getopt.h>
#endif /* HAVE_GETOPT_H */
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/Xaw/Box.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/Command.h>
#include <X11/Xaw/Paned.h>
#include <Tachometer.h>

#include "xgpsspeed.icon"
#include "gps.h"
#include "display.h"

static XrmOptionDescRec options[] = {
{"-rv",		"*reverseVideo",	XrmoptionNoArg,		"TRUE"},
{"-nc",         "*needleColor",         XrmoptionSepArg,        NULL},
{"-needlecolor","*needleColor",         XrmoptionSepArg,        NULL},
};

String fallback_resources[] = {NULL};

static struct gps_data_t *gpsdata;
static Widget tacho;

static void update_display(char *buf UNUSED)
{
  int new = rint(gpsdata->speed * KNOTS_TO_MPH);
  if (new > 100)
    new = 100;
  TachometerSetValue(tacho, new);
}

static void handle_input(XtPointer client_data UNUSED,
			 int *source UNUSED,
			 XtInputId * id UNUSED)
{
    gps_poll(gpsdata);
}

int main(int argc, char **argv)
{
    Arg             args[10];
    XtAppContext app;
    int option;
    char *colon, *server = NULL;
    char *port = DEFAULT_GPSD_PORT;
    Widget toplevel, base;

    toplevel = XtVaAppInitialize(&app, "xpsspeed.ad", 
				 options, XtNumber(options),
				 &argc, argv, fallback_resources, NULL);
    while ((option = getopt(argc, argv, "hv")) != -1) {
	switch (option) {
	case 'v':
	    printf("xgpsspeed %s\n", VERSION);
	    exit(0);
	case 'h': case '?':
	default:
	    fputs("usage: gps [-h] [-rv] [-nc] [-needlecolor] [server[:port]]\n", stderr);
	    exit(1);
	}
    }
    if (optind < argc) {
	server = strdup(argv[optind]);
	colon = strchr(server, ':');
	if (colon != NULL) {
	    server[colon - server] = '\0';
	    port = colon + 1;
	}
    }

   /**** Shell Widget ****/
    XtSetArg(args[0], XtNiconPixmap,
	     XCreateBitmapFromData(XtDisplay(toplevel),
				   XtScreen(toplevel)->root, (char*)xgps_bits,
				   xgps_width, xgps_height));
    XtSetValues(toplevel, args, 1);
    
    /**** Form widget ****/
    base = XtCreateManagedWidget("pane", panedWidgetClass, toplevel, NULL, 0);

    /**** Label widget (Title) ****/
    XtSetArg(args[0], XtNlabel, "GPS Speedometer");
    XtCreateManagedWidget("title", labelWidgetClass, base, args, 1);

    /**** Label widget ****/
    XtSetArg(args[0], XtNlabel, "Miles per Hour");
    XtCreateManagedWidget("name", labelWidgetClass, base, args, 1);
    
    /**** Tachometer widget ****/
    tacho = XtCreateManagedWidget("meter",
				  tachometerWidgetClass, base, NULL, 0);    
    XtRealizeWidget(toplevel);

    if (!(gpsdata = gps_open(server, DEFAULT_GPSD_PORT))) {
	fprintf(stderr, "xgpsspeed: no gpsd running or network error (%d).\n", errno);
	exit(2);
    }

    XtAppAddInput(app, gpsdata->gps_fd, (XtPointer) XtInputReadMask,
		  handle_input, NULL);
    
    gps_set_raw_hook(gpsdata, update_display);
    gps_query(gpsdata, "w+x\n");

    XtAppMainLoop(app);

    gps_close(gpsdata);
    return 0;
}
