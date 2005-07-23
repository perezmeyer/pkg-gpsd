/*****************************************************************************

This is a decoder for RTCM-104, an obscure and complicated serial
protocol used for broadcasting pseudorange corrections from
differential-GPS reference stations.  The applicable
standard is

RTCM RECOMMENDED STANDARDS FOR DIFFERENTIAL NAVSTAR GPS SERVICE,
RTCM PAPER 194-93/SC 104-STD

Ordering instructions are accessible from <http://www.rtcm.org/>
under "Publications".

Also applicable is ITU-R M.823: "Technical characteristics of
differential transmissions for global navigation satellite systems
from maritime radio beacons in the frequency band 283.5 - 315 kHz in
region 1 and 285 - 325 kHz in regions 2 & 3."

The code was originally by Wolfgang Rupprecht.  ESR severely hacked
it, with Wolfgang's help, in order to separate message analysis from
message dumping.  You are not expected to understand any of it. Here
are Wolfgang's original rather cryptic notes on the decoder stage:

--------------------------------------------------------------------------
1) trim and  bitflip the input.

While syncing the msb of the input gets shifted into lsb of the
assembled word.  
    word <<= 1, or in input >> 5 
    word <<= 1, or in input >> 4
    word <<= 1, or in input >> 3
    word <<= 1, or in input >> 2 
    word <<= 1, or in input >> 1 
    word <<= 1, or in input

At one point it should sync-lock.

----

Shift 6 bytes of rtcm data in as such:

---> (trim-bits-to-5-bits) ---> (end-for-end-bit-flip) ---> 

---> shift-into-30-bit-shift-register
              |||||||||||||||||||||||
	      detector-for-preamble
              |||||||||||||||||||||||
              detector-for-parity
              |||||||||||||||||||||||
--------------------------------------------------------------------------

Wolfgang's decoder was loosely based on one written by John Sanger in
1999 (in particular the dump function emits Sanger's dump format).
Here are John Sanger's original notes:

--------------------------------------------------------------------------
The RTCM decoder prints a legible representation of the input data.
The RTCM SC-104 specification is copyrighted, so I cannot
quote it - in fact, I have never read it! Most of the information
used to develop the decoder came from publication ITU-R M.823.
This is a specification of the data transmitted from LF DGPS
beacons in the 300kHz band. M.823 contains most of those parts of
RTCM SC-104 directly relevant to the air interface (there
are one or two annoying and vital omissions!). Information
about the serial interface format was gleaned from studying
the output of a beacon receiver test program made available on
Starlink's website.
--------------------------------------------------------------------------

*****************************************************************************/

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <endian.h>

#include "gpsd.h"

#define MAG_SHIFT 6u
#define MAG_TAG_DATA (1 << MAG_SHIFT)
#define MAG_TAG_MASK (3 << MAG_SHIFT)

#define PREAMBLE_PATTERN 0x66
#define PREAMBLE_SHIFT 22
#define PREAMBLE_MASK (0xFF << PREAMBLE_SHIFT)

#define W_DATA_MASK	0x3fffffc0u

/*@ +charint @*/
static unsigned char   parity_array[] = {
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0
};

static unsigned int reverse_bits[] = {
    0, 32, 16, 48, 8, 40, 24, 56, 4, 36, 20, 52, 12, 44, 28, 60,
    2, 34, 18, 50, 10, 42, 26, 58, 6, 38, 22, 54, 14, 46, 30, 62,
    1, 33, 17, 49, 9, 41, 25, 57, 5, 37, 21, 53, 13, 45, 29, 61,
    3, 35, 19, 51, 11, 43, 27, 59, 7, 39, 23, 55, 15, 47, 31, 63
};
/*@ -charint @*/

static unsigned int rtcmparity(rtcmword_t th)
{
#define P_30_MASK	0x40000000u

#define	PARITY_25	0xbb1f3480u
#define	PARITY_26	0x5d8f9a40u
#define	PARITY_27	0xaec7cd00u
#define	PARITY_28	0x5763e680u
#define	PARITY_29	0x6bb1f340u
#define	PARITY_30	0x8b7a89c0u
    rtcmword_t        t;
    unsigned int    p;

    /*
    if (th & P_30_MASK)
	th ^= W_DATA_MASK;
    */

    /*@ +charint @*/
    t = th & PARITY_25;
    p = parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
	parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff];
    t = th & PARITY_26;
    p = (p << 1) | (parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
		  parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff]);
    t = th & PARITY_27;
    p = (p << 1) | (parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
		  parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff]);
    t = th & PARITY_28;
    p = (p << 1) | (parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
		  parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff]);
    t = th & PARITY_29;
    p = (p << 1) | (parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
		  parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff]);
    t = th & PARITY_30;
    p = (p << 1) | (parity_array[t & 0xff] ^ parity_array[(t >> 8) & 0xff] ^
		  parity_array[(t >> 16) & 0xff] ^ parity_array[(t >> 24) & 0xff]);
    /*@ -charint @*/

    gpsd_report(RTCM_ERRLEVEL_BASE+2, "parity %u\n", p);
    return (p);
}


#define rtcmparityok(w)	(rtcmparity(w) == ((w) & 0x3f))

#if 0
/* 
 * ESR found a doozy of a bug...
 *
 * Defining the above as a function triggers an optimizer bug in gcc 3.4.2.
 * The symptom is that parity computation is screwed up and the decoder
 * never achieves sync lock.  Something steps on the argument to 
 * rtcmparity(); the lossage appears to be related to the compiler's 
 * attempt to fold the rtcmparity() call into rtcmparityok() in some
 * tail-recursion-like manner.  This happens under -O2, but not -O1, on
 * both i386 and amd64.  Disabling all of the individual -O2 suboptions
 * does *not* fix it.
 *
 * And the fun doesn't stop there! It turns out that even with this fix, bare
 * -O2 generates bad code.  It takes "-O2 -fschedule-insns" to generate good
 * code under 3.4.[23]...which is weird because -O2 is supposed to *imply*
 * -fschedule-insns.
 *
 *  gcc 4.0 does not manifest these bugs.
 */
static bool rtcmparityok(rtcmword_t w)
{
    return (rtcmparity(w) == (w & 0x3f));
}
#endif

void rtcm_init(/*@out@*/struct gps_device_t *session)
{
    session->rtcm.curr_word = 0;
    session->rtcm.curr_offset = 24;	/* first word */
    session->rtcm.locked = false;
    session->rtcm.bufindex = 0;
}

/*
 * Structures for interpreting words in an RTCM-104 message (after
 * parity checking and removing inversion).
 *
 * The RTCM standard is less explicit than it should be about signed-integer
 * representations.  Two's compliment is specified for prc and rrc (msg1wX),
 * but not everywhere.
 */

#define	ZCOUNT_SCALE	0.6	/* sec */
#define	PCSMALL		0.02	/* meters */
#define	PCLARGE		0.32	/* meters */
#define	RRSMALL		0.002	/* meters/sec */
#define	RRLARGE		0.032	/* meters/sec */

#define XYZ_SCALE	0.01	/* meters */
#define DXYZ_SCALE	0.1	/* meters */
#define	LA_SCALE	(90.0/32767.0)	/* degrees */
#define	LO_SCALE	(180.0/32767.0)	/* degrees */
#define	FREQ_SCALE	0.1	/* kHz */
#define	FREQ_OFFSET	190.0	/* kHz */
#define CNR_OFFSET	24	/* dB */
#define TU_SCALE	5	/* minutes */

#pragma pack(1)

struct rtcm_msg_t {
    struct rtcm_msghw1 {			/* header word 1 */
	uint            parity:6;
	uint            refstaid:10;	/* reference station ID */
	uint            msgtype:6;		/* RTCM message type */
	uint            preamble:8;		/* fixed at 01100110 */
	uint            _pad:2;
    } w1;

    struct rtcm_msghw2 {			/* header word 2 */
	uint            parity:6;
	uint            stathlth:3;		/* station health */
	uint            frmlen:5;
	uint            sqnum:3;
	uint            zcnt:13;
	uint            _pad:2;
    } w2;

    union {
	/* msg 1 - differential gps corrections */
	struct rtcm_msg1 {
	    struct b_correction_t {
		struct {			/* msg 1 word 3 */
		    uint            parity:6;
		    int             pc1:16;
		    uint            satident1:5;	/* satellite ID */
		    uint            udre1:2;
		    uint            scale1:1;
		    uint            _pad:2;
		} w3;

		struct {			/* msg 1 word 4 */
		    uint            parity:6;
		    uint            satident2:5;	/* satellite ID */
		    uint            udre2:2;
		    uint            scale2:1;
		    uint            issuedata1:8;
		    int             rangerate1:8;
		    uint            _pad:2;
		} w4;

		struct {			/* msg 1 word 5 */
		    uint            parity:6;
		    int             rangerate2:8;
		    int             pc2:16;
		    uint            _pad:2;
		} w5;

		struct {			/* msg 1 word 6 */
		    uint            parity:6;
		    int             pc3_h:8;
		    uint            satident3:5;	/* satellite ID */
		    uint            udre3:2;
		    uint            scale3:1;
		    uint            issuedata2:8;
		    uint            _pad:2;
		} w6;

		struct {			/* msg 1 word 7 */
		    uint            parity:6;
		    uint            issuedata3:8;
		    int             rangerate3:8;
		    uint            pc3_l:8;		/* NOTE: uint for low byte */
		    uint            _pad:2;
		} w7;
	    } corrections[(RTCM_WORDS_MAX - 2) / 5];
	} type1;

	/* msg 3 - reference station parameters */
	struct rtcm_msg3 {
	    struct {
		uint        parity:6;
		uint	    x_h:24;
		uint        _pad:2;
	    } w3;
	    struct {
		uint        parity:6;
		uint	    y_h:16;
		uint	    x_l:8;
		uint        _pad:2;
	    } w4;
	    struct {
		uint        parity:6;
		uint	    z_h:8;
		uint	    y_l:16;
		uint        _pad:2;
	    } w5;

	    struct {
		uint        parity:6;
		uint	    z_l:24;
		uint        _pad:2;
	    } w6;
	} type3;

	/* msg 4 - reference station datum */
	struct rtcm_msg4 {
	    struct {
		uint        parity:6;
		uint	    datum_alpha_char2:8;
		uint	    datum_alpha_char1:8;
		uint	    spare:4;
		uint	    dat:1;
		uint	    dgnss:3;
		uint        _pad:2;
	    } w3;
	    struct {
		uint        parity:6;
		uint	    datum_sub_div_char2:8;
		uint	    datum_sub_div_char1:8;
		uint	    datum_sub_div_char3:8;
		uint        _pad:2;
	    } w4;
	    struct {
		uint        parity:6;
		uint	    dy_h:8;
		uint	    dx:16;
		uint        _pad:2;
	    } w5;
	    struct {
		uint        parity:6;
		uint	    dz:24;
		uint	    dy_l:8;
		uint        _pad:2;
	    } w6;
	} type4;

	/* msg 5 - constellation health */
	struct rtcm_msg5 {
	    struct b_health_t {
		uint        parity:6;
		uint	    unassigned:2;
		uint	    time_unhealthy:4;
		uint	    loss_warn:1;
		uint	    new_nav_data:1;
		uint	    health_enable:1;
		uint	    cn0:5;
		uint	    data_health:3;
		uint	    issue_of_data_link:1;
		uint	    sat_id:5;
		uint	    reserved:1;
		uint        _pad:2;
	    } health[MAXHEALTH];
	} type5;

	/* msg 6 - null message */

	/* msg 7 - beacon almanac */
	struct rtcm_msg7 {
	    struct b_station_t {
		struct {
		    uint            parity:6;
		    int	    	    lon_h:8;
		    int	            lat:16;
		    uint            _pad:2;
		} w3;
		struct {
		    uint            parity:6;
		    uint	    freq_h:6;
		    uint	    range:10;
		    uint	    lon_l:8;
		    uint            _pad:2;
		} w4;
		struct {
		    uint            parity:6;
		    uint	    encoding:1;
		    uint	    sync_type:1;
		    uint	    mod_mode:1;
		    uint	    bit_rate:3;
		    /*
		     * ITU-R M.823-2 page 9 and RTCM-SC104 v2.1 pages
		     * 4-21 and 4-22 are in conflict over the next two
		     * field sizes.  ITU says 9+3, RTCM says 10+2.
		     * The latter correctly decodes the USCG station
		     * id's so I'll use that one here. -wsr
		     */
		    uint	    station_id:10;
		    uint	    health:2;
		    uint	    freq_l:6;
		    uint            _pad:2;
		} w5;
	    } almanac[(RTCM_WORDS_MAX - 2)/3];
	} type7;

	/* msg 16 - text msg */
	struct rtcm_msg16 {
	    struct {
		uint        parity:6;
		uint	    byte3:8;
		uint	    byte2:8;
		uint	    byte1:8;
		uint        _pad:2;
	    } txt[RTCM_WORDS_MAX-2];
	} type16;

	/* unknown message */
	rtcmword_t	rtcm_msgunk[RTCM_WORDS_MAX-2];
    };
};

static unsigned int tx_speed[] = { 25, 50, 100, 110, 150, 200, 250, 300 };

#if __BYTE_ORDER == __LITTLE_ENDIAN
/* placeholders for field inversion macros */
#define signed8(x)      x
#define signed16(x)     x
#define unsigned2(x)    x
#define unsigned3(x)    x
#define unsigned4(x)    x
#define unsigned5(x)    x
#define unsigned6(x)    x
#define unsigned8(x)    x
#define unsigned10(x)   x
#define unsigned13(x)   x
#define unsigned16(x)   x
#define unsigned24(x)   x
#else

#define signed16(x)     (int16_t)bitreverse(x, 16)
#define signed8(x)      (int8_t)bitreverse(x, 8)
#define unsigned2(x)    bitreverse(x, 2)
#define unsigned3(x)    bitreverse(x, 3)
#define unsigned4(x)    bitreverse(x, 4)
#define unsigned5(x)    bitreverse(x, 5)
#define unsigned6(x)    bitreverse(x, 6)
#define unsigned8(x)    bitreverse(x, 8)
#define unsigned10(x)   bitreverse(x, 10)
#define unsigned13(x)   bitreverse(x, 13)
#define unsigned16(x)   bitreverse(x, 16)
#define unsigned24(x)   bitreverse(x, 24)

static unsigned bitreverse(unsigned x, unsigned w)
{
       unsigned char mask = 1 << (w - 1), result = 0;

       while (value) /* skip most significant bits that are zero */
       {
	   if (value & 1) /* replace mod (machine dependency) */
	       result |= mask;
	   mask  >>= 1;
	   value >>= 1;
       }
       return result;
}

#endif

static void unpack(struct gps_device_t *session)
/* break out the raw bits into the content fields */
{
    int len;
    unsigned int n, w;
    struct rtcm_t *tp = &session->gpsdata.rtcm;
    struct rtcm_msg_t *msg = (struct rtcm_msg_t *)session->rtcm.buf;

    tp->type = unsigned6(msg->w1.msgtype);
    tp->length = unsigned5(msg->w2.frmlen);
    tp->zcount = unsigned13(msg->w2.zcnt) * ZCOUNT_SCALE;
    tp->refstaid = unsigned10(msg->w1.refstaid);
    tp->seqnum = unsigned3(msg->w2.sqnum);
    tp->stathlth = unsigned3(msg->w2.stathlth);

    len = (int)tp->length;
    n = 0;
    switch (tp->type) {
    case 1:
    case 9:
	{
	    struct b_correction_t    *m = &msg->type1.corrections[0];

	    while (len >= 0) {
		if (len >= 2) {
		    tp->ranges.sat[n].ident      = unsigned5(m->w3.satident1);
		    tp->ranges.sat[n].udre       = unsigned2(m->w3.udre1);
		    tp->ranges.sat[n].issuedata  = unsigned8(m->w4.issuedata1);
		    tp->ranges.sat[n].largescale = (bool)m->w3.scale1;
		    tp->ranges.sat[n].rangerr    = signed16(m->w3.pc1) * 
			(m->w3.scale1 ? PCLARGE : PCSMALL);
		    tp->ranges.sat[n].rangerate  = signed8(m->w4.rangerate1) * 
					(m->w3.scale1 ? RRLARGE : RRSMALL);
		    n++;
		}
		if (len >= 4) {
		    tp->ranges.sat[n].ident      = unsigned5(m->w4.satident2);
		    tp->ranges.sat[n].udre       = unsigned2(m->w4.udre2);
		    tp->ranges.sat[n].issuedata  = unsigned8(m->w6.issuedata2);
		    tp->ranges.sat[n].largescale = (bool)m->w4.scale2;
		    tp->ranges.sat[n].rangerr    = signed16(m->w5.pc2) * 
			(m->w4.scale2 ? PCLARGE : PCSMALL);
		    tp->ranges.sat[n].rangerate  = signed8(m->w5.rangerate2) * 
			(m->w4.scale2 ? RRLARGE : RRSMALL);
		    n++;
		}
		if (len >= 5) {
		    tp->ranges.sat[n].ident       = unsigned5(m->w6.satident3);
		    tp->ranges.sat[n].udre        = unsigned2(m->w6.udre3);
		    tp->ranges.sat[n].issuedata   = unsigned8(m->w7.issuedata3);
		    tp->ranges.sat[n].largescale = (bool)m->w6.scale3;
		    /*@ -shiftimplementation @*/
		    tp->ranges.sat[n].rangerr     = ((signed8(m->w6.pc3_h)<<8)|(unsigned8(m->w7.pc3_l))) *
					(m->w6.scale3 ? PCLARGE : PCSMALL);
		    tp->ranges.sat[n].rangerate   = signed8(m->w7.rangerate3) * 
					(m->w6.scale3 ? RRLARGE : RRSMALL);
		    /*@ +shiftimplementation @*/
		    n++;
		}
		len -= 5;
		m++;
	    }
	    tp->ranges.nentries = n;
	}
	break;
    case 3:
        {
	    struct rtcm_msg3    *m = &msg->type3;

	    if ((tp->ecef.valid = len >= 4)) {
		tp->ecef.x = ((unsigned24(m->w3.x_h)<<8)|(unsigned8(m->w4.x_l)))*XYZ_SCALE;
		tp->ecef.y = ((unsigned16(m->w4.y_h)<<16)|(unsigned16(m->w5.y_l)))*XYZ_SCALE;
		tp->ecef.z = ((unsigned8(m->w5.z_h)<<24)|(unsigned24(m->w6.z_l)))*XYZ_SCALE;
	    }
	}
	break;
    case 4:
	if ((tp->reference.valid = len >= 2)){
	    struct rtcm_msg4    *m = &msg->type4;

	    tp->reference.system =
		    (unsigned3(m->w3.dgnss)==0) ? gps :
			    ((unsigned3(m->w3.dgnss)==1) ? glonass : unknown);
	    tp->reference.sense = (m->w3.dat != 0) ? global : local;
	    if (m->w3.datum_alpha_char1){
		tp->reference.datum[n++] = (char)(unsigned8(m->w3.datum_alpha_char1));
	    }
	    if (m->w3.datum_alpha_char2){
		tp->reference.datum[n++] = (char)(unsigned8(m->w3.datum_alpha_char2));
	    }
	    if (m->w4.datum_sub_div_char1){
		tp->reference.datum[n++] = (char)(unsigned8(m->w4.datum_sub_div_char1));
	    }
	    if (m->w4.datum_sub_div_char2){
		tp->reference.datum[n++] = (char)(unsigned8(m->w4.datum_sub_div_char2));
	    }
	    if (m->w4.datum_sub_div_char3){
		tp->reference.datum[n++] = (char)(unsigned8(m->w4.datum_sub_div_char3));
	    }
	    tp->reference.datum[n++] = '\0';
	    if (len >= 4) {
		tp->reference.dx = unsigned16(m->w5.dx) * DXYZ_SCALE;
		tp->reference.dy = ((unsigned8(m->w5.dy_h) << 8) | unsigned8(m->w6.dy_l)) * DXYZ_SCALE;
		tp->reference.dz = unsigned24(m->w6.dz) * DXYZ_SCALE;
	    } else 
		tp->reference.sense = invalid;
	}
	break;
    case 5:
	for (n = 0; n < (unsigned)len; n++) {
	    struct consat_t *csp = &tp->conhealth.sat[n];
	    struct b_health_t *m = &msg->type5.health[n];

	    csp->ident = unsigned5(m->sat_id);
	    csp->iodl = m->issue_of_data_link!=0;
	    csp->health = unsigned3(m->data_health);
	    /*@i@*/csp->snr = (m->cn0?(m->cn0+CNR_OFFSET):SNR_BAD);
	    csp->health_en = unsigned2(m->health_enable);
	    csp->new_data = m->new_nav_data!=0;
	    csp->los_warning = m->loss_warn!=0;
	    csp->tou = unsigned4(m->time_unhealthy)*TU_SCALE;
	}
	tp->conhealth.nentries = n;
	break;
    case 7:
	for (w = 0; w < (unsigned)len; w++) {
	    struct station_t *np = &tp->almanac.station[n];
	    struct b_station_t *mp = &msg->type7.almanac[w];

	    np->latitude = signed16(mp->w3.lat) * LA_SCALE;
	    /*@i@*/np->longitude = ((signed8(mp->w3.lon_h) << 8) | unsigned8(mp->w4.lon_l)) * LO_SCALE;
	    np->range = unsigned10(mp->w4.range);
	    np->frequency = (((unsigned6(mp->w4.freq_h) << 6) | unsigned6(mp->w5.freq_l)) * FREQ_SCALE) + FREQ_OFFSET;
	    np->health = unsigned2(mp->w5.health);
	    np->station_id = unsigned10(mp->w5.station_id),
	    np->bitrate = tx_speed[unsigned3(mp->w5.bit_rate)];
	    n++;
	}
	tp->almanac.nentries = n;
	break;
    case 16:
	/*@ -boolops @*/
	for (w = 0; w < (unsigned)len; w++){
	    if (!msg->type16.txt[w].byte1) {
		break;
	    }
	    tp->message[n++] = (char)(unsigned8(msg->type16.txt[w].byte1));
	    if (!msg->type16.txt[w].byte2) {
		break;
	    }
	    tp->message[n++] = (char)(unsigned8(msg->type16.txt[w].byte2));
	    if (!msg->type16.txt[w].byte3) {
		break;
	    }
	    tp->message[n++] = (char)(unsigned8(msg->type16.txt[w].byte3));
	    len--;
	}
	/*@ +boolops @*/
	tp->message[n++] = '\0';
	break;
    default:
	memcpy(tp->words, msg->rtcm_msgunk, (RTCM_WORDS_MAX-2)*sizeof(rtcmword_t));
	break;
    }
}

#ifdef __UNUSED__
static void repack(struct gps_device_t *session)
/* repack the content fields into the raw bits */
{
    int len, sval;
    unsigned int n, w;
    struct rtcm_t *tp = &session->gpsdata.rtcm;
    struct rtcm_msg_t  *msg = (struct rtcm_msg_t *)session->rtcm.buf;

    memset(session->rtcm.buf, 0, sizeof(session->rtcm.buf));
    msg->w1.msgtype = unsigned6(tp->type);
    msg->w2.frmlen = unsigned5(tp->length);
    msg->w2.zcnt = unsigned13((unsigned)(tp->zcount / ZCOUNT_SCALE));
    msg->w1.refstaid = unsigned10(tp->refstaid);
    msg->w2.sqnum = unsigned3(tp->seqnum);
    msg->w2.stathlth = unsigned3(tp->stathlth);

    len = (int)tp->length;
    n = 0;
    switch (tp->type) {
    case 1:
    case 9:
	{
	    struct b_correction_t    *m = &msg->type1.corrections[0];

	    while (len >= 0) {
		if (len >= 2) {
		    m->w3.satident1 = unsigned5(tp->ranges.sat[n].ident);
		    m->w3.udre1 = unsigned2(tp->ranges.sat[n].udre);
		    m->w4.issuedata1 = unsigned8(tp->ranges.sat[n].issuedata);
		    m->w3.scale1 = (unsigned)tp->ranges.sat[n].largescale;
		    m->w3.pc1 = (int)(signed16(tp->ranges.sat[n].rangerr) / (m->w3.scale1 ? PCLARGE : PCSMALL));
		    m->w4.rangerate1 = (int)(signed8(tp->ranges.sat[n].rangerate) / (m->w3.scale1 ? RRLARGE : RRSMALL));
		    n++;
		}
		if (len >= 4) {
		    m->w4.satident2 = unsigned5(tp->ranges.sat[n].ident);
		    m->w4.udre2 = unsigned2(tp->ranges.sat[n].udre);
		    m->w6.issuedata2 = unsigned8(tp->ranges.sat[n].issuedata);
		    m->w4.scale2 = (unsigned)tp->ranges.sat[n].largescale;
		    m->w5.pc2 = (int)(signed16(tp->ranges.sat[n].rangerr) / (m->w4.scale2 ? PCLARGE : PCSMALL));
		    m->w5.rangerate2 = (int)(signed8(tp->ranges.sat[n].rangerate) / (m->w4.scale2 ? RRLARGE : RRSMALL));
		    n++;
		}
		if (len >= 5) {
		    m->w6.satident3 = unsigned5(tp->ranges.sat[n].ident);
		    m->w6.udre3 = unsigned2(tp->ranges.sat[n].udre);
		    m->w7.issuedata3 = unsigned8(tp->ranges.sat[n].issuedata);
		    m->w6.scale3 = (unsigned)tp->ranges.sat[n].largescale;
		    sval = (int)(tp->ranges.sat[n].rangerr / (m->w6.scale3 ? PCLARGE : PCSMALL));
		    /*@ -shiftimplementation @*/
		    m->w6.pc3_h = signed8(sval >> 8);
		    /*@ +shiftimplementation @*/
		    m->w7.pc3_l = unsigned8((unsigned)sval & 0xff);
		    m->w7.rangerate3 = (int)(signed8(tp->ranges.sat[n].rangerate) / (m->w6.scale3 ? RRLARGE : RRSMALL));
		    n++;
		}
		len -= 5;
		m++;
	    }
	    tp->ranges.nentries = n;
	}
	break;
    case 3:
	if (tp->ecef.valid) {
	    struct rtcm_msg3    *m = &msg->type3;
	    unsigned x = (unsigned)(tp->ecef.x / XYZ_SCALE);
	    unsigned y = (unsigned)(tp->ecef.y / XYZ_SCALE);
	    unsigned z = (unsigned)(tp->ecef.z / XYZ_SCALE);

	    m->w4.x_l = unsigned8(x & 0xff);
	    m->w3.x_h = unsigned24(x >> 8);
	    m->w5.y_l = unsigned8(y & 0xff);
	    m->w4.y_h = unsigned24(y >> 8);
	    m->w6.z_l = unsigned8(z & 0xff);
	    m->w5.z_h = unsigned24(z >> 8);
	}
	break;
    case 4:
	if (tp->reference.valid) {
	    struct rtcm_msg4    *m = &msg->type4;

	    m->w3.dgnss = tp->reference.system;
	    m->w3.dat = (unsigned)(tp->reference.sense == global);
	    /*@ -predboolothers -type @*/
	    if (tp->reference.datum[0])
		m->w3.datum_alpha_char1 = unsigned8(tp->reference.datum[0]);
	    else
		m->w3.datum_alpha_char1 = 0;
	    if (tp->reference.datum[1])
		m->w3.datum_alpha_char2 = unsigned8(tp->reference.datum[1]);
	    else
		m->w3.datum_alpha_char2 = 0;
	    if (tp->reference.datum[2])
		m->w4.datum_sub_div_char1 = unsigned8(tp->reference.datum[2]);
	    else
		m->w4.datum_sub_div_char1 = 0;
	    if (tp->reference.datum[3])
		m->w4.datum_sub_div_char2 = unsigned8(tp->reference.datum[3]);
	    else
		m->w4.datum_sub_div_char2 = 0;
	    if (tp->reference.datum[4])
		m->w4.datum_sub_div_char3 = unsigned8(tp->reference.datum[4]);
	    else
		m->w4.datum_sub_div_char3 = 0;
	    /*@ +predboolothers +type @*/
	    if (tp->reference.system != unknown) {
		m->w5.dx = unsigned16((uint)(tp->reference.dx / DXYZ_SCALE));
		//tp->reference.dy = ((unsigned8(m->w5.dy_h) << 8) | unsigned8(m->w6.dy_l)) * DXYZ_SCALE;
		m->w6.dz = unsigned24((uint)(tp->reference.dz / DXYZ_SCALE));
	    }
	}
	break;
    case 5:
	for (n = 0; n < (unsigned)len; n++) {
	    struct consat_t *csp = &tp->conhealth.sat[n];
	    struct b_health_t *m = &msg->type5.health[n];

	    m->sat_id = unsigned5(csp->ident);
	    m->issue_of_data_link = (unsigned)csp->iodl;
	    m->data_health = unsigned3(csp->health);
	    m->cn0 = (csp->snr == SNR_BAD) ? 0 : (unsigned)csp->snr - CNR_OFFSET;
	    m->health_enable = unsigned2(csp->health_en);
	    m->new_nav_data = (unsigned)csp->new_data;
	    m->loss_warn = (unsigned)csp->los_warning;
	    m->time_unhealthy = unsigned4((unsigned)(csp->tou / TU_SCALE));
	}
	break;
    case 7:
	for (w = 0; w < (RTCM_WORDS_MAX - 2)/ 3; w++) {
	    struct station_t *np = &tp->almanac.station[n];
	    struct b_station_t *mp = &msg->type7.almanac[w];

	    mp->w3.lat = signed16((int)(np->latitude / LA_SCALE));
	    //np->longitude = ((signed8(mp->w3.lon_h) << 8) | unsigned8(mp->w4.lon_l)) * LO_SCALE;
	    mp->w4.range = unsigned10(np->range);
	    //np->frequency = (((unsigned6(mp->w4.freq_h) << 6) | unsigned6(mp->w5.freq_l)) * FREQ_SCALE) + FREQ_OFFSET;
	    mp->w5.health = unsigned2(np->health);
	    mp->w5.station_id = unsigned10(np->station_id);
	    //np->bitrate = tx_speed[unsigned3(mp->w5.bit_rate)];
	}
	tp->almanac.nentries = n;
	break;
    case 16:
	/*@ -boolops @*/
	for (w = 0; w < RTCM_WORDS_MAX - 2; w++){
	    if (!tp->message[n]) {
		break;
	    }
	    msg->type16.txt[w].byte1 = unsigned8((unsigned)tp->message[n++]);
	    if (!tp->message[n]) {
		break;
	    }
	    msg->type16.txt[w].byte2 = unsigned8((unsigned)tp->message[n++]);
	    if (!tp->message[n]) {
		break;
	    }
	    msg->type16.txt[w].byte3 = unsigned8((unsigned)tp->message[n++]);
	}
	/*@ +boolops @*/
	break;
    }

    /* FIXME: must compute parity and inversion here */
}
#endif /* __UNUSED__ */

/*@ -usereleased -compdef @*/
enum rtcmstat_t rtcm_decode(struct gps_device_t *session, unsigned int c)
{
    enum rtcmstat_t res;

    if ((c & MAG_TAG_MASK) != MAG_TAG_DATA) {
	gpsd_report(RTCM_ERRLEVEL_BASE+1, 
		    "word tag not correct, skipping\n");
	return RTCM_SKIP;
    }
    c = reverse_bits[c & 0x3f];

    /*@ -shiftnegative @*/
    if (!session->rtcm.locked) {
	session->rtcm.curr_offset = -5;
	session->rtcm.bufindex = 0;

	while (session->rtcm.curr_offset <= 0) {
	    gpsd_report(RTCM_ERRLEVEL_BASE+2, "syncing\n");
	    session->rtcm.curr_word <<= 1;
	    if (session->rtcm.curr_offset > 0) {
		session->rtcm.curr_word |= c << session->rtcm.curr_offset;
	    } else {
		session->rtcm.curr_word |= c >> -(session->rtcm.curr_offset);
	    }
	    if (((struct rtcm_msghw1 *) & session->rtcm.curr_word)->preamble ==
		PREAMBLE_PATTERN) {
		if (rtcmparityok(session->rtcm.curr_word)) {
		    gpsd_report(RTCM_ERRLEVEL_BASE+1, 
				"preamble ok, parity ok -- locked\n");
		    session->rtcm.locked = true;
		    /* session->rtcm.curr_offset;  XXX - testing */
		    break;
		}
		gpsd_report(RTCM_ERRLEVEL_BASE+1, 
			    "preamble ok, parity fail\n");
	    }
	    session->rtcm.curr_offset++;
	}			/* end while */
    }
    if (session->rtcm.locked) {
	res = RTCM_SYNC;

	if (session->rtcm.curr_offset > 0) {
	    session->rtcm.curr_word |= c << session->rtcm.curr_offset;
	} else {
	    session->rtcm.curr_word |= c >> -(session->rtcm.curr_offset);
	}

	if (session->rtcm.curr_offset <= 0) {
	    /* weird-assed inversion */
	    if (session->rtcm.curr_word & P_30_MASK)
		session->rtcm.curr_word ^= W_DATA_MASK;

	    if (rtcmparityok(session->rtcm.curr_word)) {
#if 0
		/*
		 * Don't clobber the buffer just because we spot
		 * another preamble pattern in the data stream. -wsr
		 */
		if (((struct rtcm_msghw1 *) & session->rtcm.curr_word)->preamble ==
		    PREAMBLE_PATTERN) {
		    gpsd_report(RTCM_ERRLEVEL_BASE+2, 
				"Preamble spotted (index: %u)\n",
				session->rtcm.bufindex);
		    session->rtcm.bufindex = 0;
		}
#endif
		gpsd_report(RTCM_ERRLEVEL_BASE+2,
			    "processing word %u (offset %d)\n",
			    session->rtcm.bufindex, session->rtcm.curr_offset);
		{
		    struct rtcm_msg_t  *msg = (struct rtcm_msg_t *)session->rtcm.buf;

		    /*
		     * Guard against a buffer overflow attack.  Just wait for
		     * the next PREAMBLE_PATTERN and go on from there. 
		     */
		    if (session->rtcm.bufindex >= RTCM_WORDS_MAX){
			session->rtcm.bufindex = 0;
			gpsd_report(RTCM_ERRLEVEL_BASE+1, 
				    "RTCM buffer overflowing -- resetting\n");
			return RTCM_NO_SYNC;
		    }

		    session->rtcm.buf[session->rtcm.bufindex] = session->rtcm.curr_word;

		    if ((session->rtcm.bufindex == 0) &&
			(msg->w1.preamble != PREAMBLE_PATTERN)) {
			gpsd_report(RTCM_ERRLEVEL_BASE+1, 
				    "word 0 not a preamble- punting\n");
			return RTCM_NO_SYNC;
		    }
		    session->rtcm.bufindex++;
		    /* rtcm_print_msg(msg); */

		    if (session->rtcm.bufindex >= 2) {	/* do we have the length yet? */
			if (session->rtcm.bufindex >= msg->w2.frmlen + 2) {
			    /* jackpot, we have an RTCM packet*/
			    res = RTCM_STRUCTURE;
			    session->rtcm.bufindex = 0;
			    unpack(session);
			}
		    }
		}
		session->rtcm.curr_word <<= 30;	/* preserve the 2 low bits */
		session->rtcm.curr_offset += 30;
		if (session->rtcm.curr_offset > 0) {
		    session->rtcm.curr_word |= c << session->rtcm.curr_offset;
		} else {
		    session->rtcm.curr_word |= c >> -(session->rtcm.curr_offset);
		}
	    } else {
		gpsd_report(RTCM_ERRLEVEL_BASE+0, 
			    "parity failure, lost lock\n");
		session->rtcm.locked = false;
	    }
	}
	session->rtcm.curr_offset -= 6;
	gpsd_report(RTCM_ERRLEVEL_BASE+2, "residual %d\n", session->rtcm.curr_offset);
	return res;
    }
    /*@ +shiftnegative @*/

    /* never achieved lock */
    gpsd_report(RTCM_ERRLEVEL_BASE+1, 
		"lock never achieved\n");
    return RTCM_NO_SYNC;
}
/*@ +usereleased +compdef @*/

void rtcm_dump(struct gps_device_t *session, /*@out@*/char buf[], size_t buflen)
/* dump the contents of a parsed RTCM104 message */
{
    unsigned int n;

    (void)snprintf(buf, buflen, "H\t%u\t%u\t%0.1f\t%u\t%u\t%u\n",
	   session->gpsdata.rtcm.type,
	   session->gpsdata.rtcm.refstaid,
	   session->gpsdata.rtcm.zcount,
	   session->gpsdata.rtcm.seqnum,
	   session->gpsdata.rtcm.length,
	   session->gpsdata.rtcm.stathlth);

    switch (session->gpsdata.rtcm.type) {
    case 1:
    case 9:
	for (n = 0; n < session->gpsdata.rtcm.ranges.nentries; n++) {
	    struct rangesat_t *rsp = &session->gpsdata.rtcm.ranges.sat[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "S\t%u\t%u\t%u\t%0.1f\t%0.3f\t%0.3f\n",
			   rsp->ident,
			   rsp->udre,
			   rsp->issuedata,
			   session->gpsdata.rtcm.zcount,
			   rsp->rangerr,
			   rsp->rangerate);
	}
	break;

    case 3:
	if (session->gpsdata.rtcm.ecef.valid)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "R\t%.2f\t%.2f\t%.2f\n",
			   session->gpsdata.rtcm.ecef.x, 
			   session->gpsdata.rtcm.ecef.y,
			   session->gpsdata.rtcm.ecef.z);
	break;

    case 4:
	if (session->gpsdata.rtcm.reference.valid)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "D\t%s\t%1d\t%s\t%.1f\t%.1f\t%.1f\n",
			   (session->gpsdata.rtcm.reference.system==gps) ? "GPS"
			   : ((session->gpsdata.rtcm.reference.system==glonass) ? "GLONASS"
			      : "UNKNOWN"),
			   session->gpsdata.rtcm.reference.sense,
			   session->gpsdata.rtcm.reference.datum,
			   session->gpsdata.rtcm.reference.dx,
			   session->gpsdata.rtcm.reference.dy,
			   session->gpsdata.rtcm.reference.dz);
	break;

    case 5:
	for (n = 0; n < session->gpsdata.rtcm.conhealth.nentries; n++) {
	    struct consat_t *csp = &session->gpsdata.rtcm.conhealth.sat[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   /* FIXME: turn these spaces to tabs someday */
			   "C\t%2u\t%1u  %1u\t%2d\t%1u  %1u  %1u\t%2u\n",
			   csp->ident,
			   (unsigned)csp->iodl,
			   (unsigned)csp->health,
			   csp->snr,
			   (unsigned)csp->health_en,
			   (unsigned)csp->new_data,
			   (unsigned)csp->los_warning,
			   csp->tou);
	}
	break;

    case 6: 			/* NOP msg */
	strcat(buf, "N\n");
	break;

    case 7:
	for (n = 0; n < session->gpsdata.rtcm.almanac.nentries; n++) {
	    struct station_t *ssp = &session->gpsdata.rtcm.almanac.station[n];
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "A\t%.4f\t%.4f\t%u\t%.1f\t%u\t%u\t%u\n",
			   ssp->latitude,
			   ssp->longitude,
			   ssp->range,
			   ssp->frequency,
			   ssp->health,
			   ssp->station_id,
			   ssp->bitrate);
	}
	break;
    case 16:
	(void)snprintf(buf + strlen(buf), buflen - strlen(buf),
		       "T \"%s\"\n", session->gpsdata.rtcm.message);
	break;

    default:
	for (n = 0; n < session->gpsdata.rtcm.length; n++)
	    (void)snprintf(buf + strlen(buf), buflen - strlen(buf),
			   "U 0x%08x\n", session->gpsdata.rtcm.words[n]);
	break;
    }

}

#ifdef __UNUSED__
/*
 * The RTCM words are 30-bit words.  We will lay them into memory into
 * 30-bit (low-end justified) chunks.  To write them out we will write
 * 5 Magnavox-format bytes where the low 6-bits of the byte are 6-bits
 * of the 30-word msg.
 */
void rtcm_output_mag(rtcmword_t * ip)
/* ship an RTCM message to standard output in Magnavox format */
{
    static rtcmword_t w = 0;
    int             len;
    static uint     sqnum = 0;

    len = ((struct rtcm_msg *) ip)->w2.frmlen + 2;
    ((struct rtcm_msg *) ip)->w2.sqnum = sqnum++;
    sqnum &= 0x7;

    while (len-- > 0) {
	w <<= 30;
	w |= *ip++ & W_DATA_MASK;

	w |= rtcmparity(w);

	/* weird-assed inversion */
	if (w & P_30_MASK)
	    w ^= W_DATA_MASK;

	/* msb first */
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 24) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 18) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 12) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w >> 6) & 0x3f]);
	putchar(MAG_TAG_DATA | reverse_bits[(w) & 0x3f]);
    }
}
#endif /* UNUSED */
