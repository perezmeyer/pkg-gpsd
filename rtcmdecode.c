/* $Id$ */
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>

#include "gpsd_config.h"
#include "gpsd.h"

static int verbose = ISGPS_ERRLEVEL_BASE;

void gpsd_report(int errlevel, const char *fmt, ... )
/* assemble command in printf(3) style, use stderr or syslog */
{
    if (errlevel <= verbose) {
	char buf[BUFSIZ];
	va_list ap;

	(void)strlcpy(buf, "rtcmdecode: ", BUFSIZ);
	va_start(ap, fmt) ;
	(void)vsnprintf(buf + strlen(buf), sizeof(buf)-strlen(buf), fmt, ap);
	va_end(ap);
	(void)fputs(buf, stdout);
    }
}

/*@ -compdestroy @*/
static void decode(FILE *fpin, FILE *fpout)
/* RTCM-104 bits on fpin to dump format on fpout */
{
    struct gps_packet_t lexer;
    struct rtcm2_t rtcm;
    char buf[BUFSIZ];

    packet_reset(&lexer);

    for (;;) {
	if (packet_get(fileno(fpin), &lexer) <= 0 && packet_buffered_input(&lexer) <= 0)
	    break;
	else if (lexer.type == RTCM2_PACKET) {
	    rtcm2_unpack(&rtcm, (char *)lexer.isgps.buf);
	    rtcm2_dump(&rtcm, buf, sizeof(buf));
	    (void)fputs(buf, fpout);
	}
    }
}
/*@ +compdestroy @*/

/*@ -compdestroy @*/
static void pass(FILE *fpin, FILE *fpout)
/* dump format on stdin to dump format on stdout (self-inversion test) */
{
    char buf[BUFSIZ];
    struct gps_packet_t lexer;
    struct rtcm2_t rtcm;

    memset(&lexer, 0, sizeof(lexer));
    memset(&rtcm, 0, sizeof(rtcm));
    while (fgets(buf, (int)sizeof(buf), fpin) != NULL) {
	int status;

	/* pass through comment lines without interpreting */
	if (buf[0] == '#') {
	    (void)fputs(buf, fpout);
	    continue;
	} 
	status = rtcm2_undump(&rtcm, buf);

	if (status == 0) {
	    (void)memset(lexer.isgps.buf, 0, sizeof(lexer.isgps.buf));
	    (void)rtcm2_repack(&rtcm, lexer.isgps.buf);
	    (void)rtcm2_unpack(&rtcm, (char *)lexer.isgps.buf);
	    (void)rtcm2_dump(&rtcm, buf, sizeof(buf));
	    (void)fputs(buf, fpout);
	    memset(&lexer, 0, sizeof(lexer));
	    memset(&rtcm, 0, sizeof(rtcm));
	} else if (status < 0) {
	    (void) fprintf(stderr, "rtcmdecode: bailing out with status %d\n", status);
	    exit(1);
	}
    }
}
/*@ +compdestroy @*/

/*@ -compdestroy @*/
static void encode(FILE *fpin, FILE *fpout)
/* dump format on fpin to RTCM-104 on fpout */
{
    char buf[BUFSIZ];
    struct gps_packet_t lexer;
    struct rtcm2_t rtcm;

    memset(&lexer, 0, sizeof(lexer));
    while (fgets(buf, (int)sizeof(buf), fpin) != NULL) {
	int status;

	status = rtcm2_undump(&rtcm, buf);

	if (status == 0) {
	    (void)memset(lexer.isgps.buf, 0, sizeof(lexer.isgps.buf));
	    (void)rtcm2_repack(&rtcm, lexer.isgps.buf);
	    (void)fwrite(lexer.isgps.buf, 
			 sizeof(isgps30bits_t), 
			 (size_t)rtcm.length, fpout);
	    memset(&lexer, 0, sizeof(lexer));
	} else if (status < 0) {
	    (void) fprintf(stderr, "rtcmdecode: bailing out with status %d\n", status);
	    exit(1);
	}
    }
}
/*@ +compdestroy @*/

int main(int argc, char **argv)
{
    char buf[BUFSIZ];
    int c;
    bool striphdr = false;
    enum {doencode, dodecode, passthrough} mode = dodecode;

    while ((c = getopt(argc, argv, "dehpVv:")) != EOF) {
	switch (c) {
	case 'd':
	    mode = dodecode;
	    break;

	case 'e':
	    mode = doencode;
	    break;

	case 'h':
	    striphdr = true;
	    break;

	case 'p':	/* undocumented, used for regression-testing */
	    mode = passthrough;
	    break;

	case 'v':
	    verbose = ISGPS_ERRLEVEL_BASE + atoi(optarg);
	    break;

	case 'V':
	    (void)fprintf(stderr, "SVN ID: $Id$ \n");
	    exit(0);

	case '?':
	default:
	    (void)fputs("rtcmdecode [-v]\n", stderr);
	    exit(1);
	}
    }
    argc -= optind;
    argv += optind;

    /* strip lines with leading # */
    if (striphdr) {
	while ((c = getchar()) == '#')
	    (void)fgets(buf, (int)sizeof(buf), stdin);
	(void)ungetc(c, stdin);
    }

    if (mode == passthrough)
	pass(stdin, stdout);
    else if (mode == doencode)
	encode(stdin, stdout);
    else
	decode(stdin, stdout);
    exit(0);
}

/* rtcmdecode.c ends here */
