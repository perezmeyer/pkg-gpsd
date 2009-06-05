/* 
 * Driver for AIS/AIVDM messages.
 *
 * See the file AIVDM.txt on the GPSD website for documentation and references.
 *
 * Decodings of message types 11 and 21 have not yet been tested against 
 * known-good data.
 *
 * The decoder for message type 18 does not yet grok the ITU-1371-3 flag bits. 
 *
 * Message type 21 decoding does not yet handle the Name Extension field.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#include "gpsd_config.h"
#include "gpsd.h"
#if defined(AIVDM_ENABLE)

#include "bits.h"

/**
 * Parse the data from the device
 */

static void from_sixbit(char *bitvec, uint start, int count, char *to)
{
    /*@ +type @*/
#ifdef S_SPLINT_S
    /* the real string causes a splint internal error */
    const char sixchr[] = "abcd";
#else
    const char sixchr[64] = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^- !\"#$%&`()*+,-./0123456789:;<=>?";
#endif /* S_SPLINT_S */
    int i;

    /* six-bit to ASCII */
    for (i = 0; i < count-1; i++)
	to[i] = sixchr[ubits(bitvec, start + 6*i, 6U)];
    to[count-1] = '\0';
    /* trim spaces on right end */
    for (i = count-2; i >= 0; i--)
	if (to[i] == ' ' || to[i] == '@')
	    to[i] = '\0';
	else
	    break;
    /*@ -type @*/
}

/*@ +charint @*/
bool aivdm_decode(char *buf, size_t buflen, struct aivdm_context_t *ais_context)
{
    char *sixbits[64] = {
	"000000", "000001", "000010", "000011", "000100",
	"000101", "000110", "000111", "001000", "001001",
	"001010", "001011", "001100", "001101",	"001110",
	"001111", "010000", "010001", "010010", "010011",
	"010100", "010101", "010110", "010111",	"011000",
	"011001", "011010", "011011", "011100",	"011101",
	"011110", "011111", "100000", "100001",	"100010",
	"100011", "100100", "100101", "100110",	"100111",
	"101000", "101001", "101010", "101011",	"101100",
	"101101", "101110", "101111", "110000",	"110001",
	"110010", "110011", "110100", "110101",	"110110",
	"110111", "111000", "111001", "111010",	"111011",
	"111100", "111101", "111110", "111111",    
    };
    int nfields = 0;    
    unsigned char *data, *cp = ais_context->fieldcopy;
    struct ais_t *ais = &ais_context->decoded;
    unsigned char ch;
    int i;

    if (buflen == 0)
	return false;

    /* we may need to dump the raw packet */
    gpsd_report(LOG_PROG, "AIVDM packet length %ld: %s", buflen, buf);

    /* extract packet fields */
    (void)strlcpy((char *)ais_context->fieldcopy, 
		  (char*)buf,
		  buflen);
    ais_context->field[nfields++] = (unsigned char *)buf;
    for (cp = ais_context->fieldcopy; 
	 cp < ais_context->fieldcopy + buflen;
	 cp++)
	if (*cp == ',') {
	    *cp = '\0';
	    ais_context->field[nfields++] = cp + 1;
	}
    ais_context->await = atoi((char *)ais_context->field[1]);
    ais_context->part = atoi((char *)ais_context->field[2]);
    data = ais_context->field[5];
    gpsd_report(LOG_PROG, "await=%d, part=%d, data=%s\n",
		ais_context->await,
		ais_context->part, 
		data);

    /* assemble the binary data */
    if (ais_context->part == 1) {
	(void)memset(ais_context->bits, '\0', sizeof(ais_context->bits));
	ais_context->bitlen = 0;
    }

    /* wacky 6-bit encoding, shades of FIELDATA */
    /*@ +charint @*/
    for (cp = data; cp < data + strlen((char *)data); cp++) {
	ch = *cp;
	ch -= 48;
	if (ch >= 40)
	    ch -= 8;
	gpsd_report(LOG_RAW, "%c: %s\n", *cp, sixbits[ch]);
	/*@ -shiftnegative @*/
	for (i = 5; i >= 0; i--) {
	    if ((ch >> i) & 0x01) {
		ais_context->bits[ais_context->bitlen / 8] |= (1 << (7 - ais_context->bitlen % 8));
	    }
	    ais_context->bitlen++;
	}
	/*@ +shiftnegative @*/
    }
    /*@ -charint @*/

    /* time to pass buffered-up data to where it's actually processed? */
    if (ais_context->part == ais_context->await) {
	size_t clen = (ais_context->bitlen + 7)/8;
	gpsd_report(LOG_INF, "AIVDM payload is %zd bits, %zd chars: %s\n",
		    ais_context->bitlen, clen,
		    gpsd_hexdump_wrapper(ais_context->bits,
					 clen, LOG_INF));


#define UBITS(s, l)	ubits((char *)ais_context->bits, s, l)
#define SBITS(s, l)	sbits((char *)ais_context->bits, s, l)
#define UCHARS(s, to)	from_sixbit((char *)ais_context->bits, s, sizeof(to), to)
	ais->id = UBITS(0, 6);
	ais->ri = UBITS(6, 2);
	ais->mmsi = UBITS(8, 30);
	gpsd_report(LOG_INF, "AIVDM message type %d, MMSI %09d:\n", 
		    ais->id, ais->mmsi);
	switch (ais->id) {
	case 1:	/* Position Report */
	case 2:
	case 3:
	    ais->type123.status = UBITS(38, 4);
	    ais->type123.rot = SBITS(42, 8);
	    ais->type123.sog = UBITS(50, 10);
	    ais->type123.accuracy = (bool)UBITS(60, 1);
	    ais->type123.longitude = SBITS(61, 28);
	    ais->type123.latitude = SBITS(89, 27);
	    ais->type123.cog = UBITS(116, 12);
	    ais->type123.heading = UBITS(128, 9);
	    ais->type123.utc_second = UBITS(137, 6);
	    ais->type123.maneuver = UBITS(143, 2);
	    ais->type123.spare = UBITS(145, 3);
	    ais->type123.raim = UBITS(148, 1)!=0;
	    ais->type123.radio = UBITS(149, 20);
	    gpsd_report(LOG_INF,
			"Nav=%d ROT=%d SOG=%d Q=%d Lon=%d Lat=%d COG=%d TH=%d Sec=%d\n",
			ais->type123.status,
			ais->type123.rot,
			ais->type123.sog, 
			(uint)ais->type123.accuracy,
			ais->type123.longitude, 
			ais->type123.latitude, 
			ais->type123.cog, 
			ais->type123.heading, 
			ais->type123.utc_second);
	    break;
	case 4: 	/* Base Station Report */
	case 11:	/* UTC/Date Response */
	    ais->type4.year = UBITS(38, 14);
	    ais->type4.month = UBITS(52, 4);
	    ais->type4.day = UBITS(56, 5);
	    ais->type4.hour = UBITS(61, 5);
	    ais->type4.minute = UBITS(66, 6);
	    ais->type4.second = UBITS(72, 6);
	    ais->type4.accuracy = (bool)UBITS(78, 1);
	    ais->type4.longitude = SBITS(79, 28);
	    ais->type4.latitude = SBITS(107, 27);
	    ais->type4.epfd = UBITS(134, 4);
	    ais->type4.spare = UBITS(138, 10);
	    ais->type4.raim = UBITS(148, 1)!=0;
	    ais->type4.radio = UBITS(149, 19);
	    gpsd_report(LOG_INF,
			"Date: %4d:%02d:%02dT%02d:%02d:%02d Q=%d Lat=%d  Lon=%d epfd=%d\n",
			ais->type4.year,
			ais->type4.month,
			ais->type4.day,
			ais->type4.hour,
			ais->type4.minute,
			ais->type4.second,
			(uint)ais->type4.accuracy,
			ais->type4.latitude, 
			ais->type4.longitude,
			ais->type4.epfd);
	    break;
	case 5: /* Ship static and voyage related data */
	    ais->type5.ais_version  = UBITS(38, 2);
	    ais->type5.imo_id       = UBITS(40, 30);
	    UCHARS(70, ais->type5.callsign);
	    UCHARS(112, ais->type5.vessel_name);
	    ais->type5.ship_type    = UBITS(232, 8);
	    ais->type5.to_bow       = UBITS(240, 9);
	    ais->type5.to_stern     = UBITS(249, 9);
	    ais->type5.to_port      = UBITS(258, 6);
	    ais->type5.to_starboard = UBITS(264, 6);
	    ais->type5.epfd         = UBITS(270, 4);
	    ais->type5.month        = UBITS(274, 4);
	    ais->type5.day          = UBITS(278, 5);
	    ais->type5.hour         = UBITS(283, 5);
	    ais->type5.minute       = UBITS(288, 6);
	    ais->type5.draught      = UBITS(294, 8);
	    UCHARS(302, ais->type5.destination);
	    ais->type5.dte          = UBITS(422, 1);
	    ais->type5.spare        = UBITS(423, 1);
	    gpsd_report(LOG_INF,
			"AIS=%d callsign=%s, name=%s destination=%s\n",
			ais->type5.ais_version,
			ais->type5.callsign,
			ais->type5.vessel_name,
			ais->type5.destination);
	    break;
        case 6: /* Addressed Binary Message */
	    ais->type6.seqno          = UBITS(38, 2);
	    ais->type6.dest_mmsi      = UBITS(40, 30);
	    ais->type6.retransmit     = (bool)UBITS(70, 1);
	    ais->type6.spare          = UBITS(71, 1);
	    ais->type6.application_id = UBITS(72, 16);
	    ais->type6.bitcount       = ais_context->bitlen - 88;
	    (void)memcpy(ais->type6.bitdata, 
			 (char *)ais_context->bits+11,
			 (ais->type6.bitcount + 7) / 8);
	    gpsd_report(LOG_INF, "seqno=%d, dest=%u, id=%u, cnt=%u\n",
			ais->type6.seqno,
			ais->type6.dest_mmsi,
			ais->type6.application_id,
			ais->type6.bitcount);
	    break;
        case 7: /* Binary acknowledge */
	    for (i = 0; i < sizeof(ais->type7.mmsi)/sizeof(ais->type7.mmsi[0]); i++)
		if (ais_context->bitlen > 40 + 32*i)
		    ais->type7.mmsi[i] = UBITS(40 + 32*i, 30);
	        else
		    ais->type7.mmsi[i] = 0;
	    gpsd_report(LOG_INF, "\n");
	    break;
        case 8: /* Binary Broadcast Message */
	    ais->type8.spare          = UBITS(38, 2);
	    ais->type8.application_id = UBITS(40, 16);
	    ais->type8.bitcount       = ais_context->bitlen - 56;
	    (void)memcpy(ais->type8.bitdata, 
			 (char *)ais_context->bits+7,
			 (ais->type8.bitcount + 7) / 8);
	    gpsd_report(LOG_INF, "id=%u, cnt=%u\n",
			ais->type8.application_id,
			ais->type8.bitcount);
	    break;
	case 9: /* Standard SAR Aircraft Position Report */
	    ais->type9.altitude = UBITS(38, 12);
	    ais->type9.sog = UBITS(50, 10);
	    ais->type9.accuracy = (bool)UBITS(60, 1);
	    ais->type9.longitude = SBITS(61, 28);
	    ais->type9.latitude = SBITS(89, 27);
	    ais->type9.cog = UBITS(116, 12);
	    ais->type9.utc_second = UBITS(128, 6);
	    ais->type9.regional = UBITS(134, 8);
	    ais->type9.dte = UBITS(142, 1);
	    ais->type9.spare = UBITS(143, 3);
	    ais->type9.assigned = UBITS(144, 1)!=0;
	    ais->type9.raim = UBITS(145, 1)!=0;
	    ais->type9.radio = UBITS(146, 22);
	    gpsd_report(LOG_INF,
			"Alt=%d SOG=%d Q=%d Lon=%d Lat=%d COG=%d Sec=%d\n",
			ais->type9.altitude,
			ais->type9.sog, 
			(uint)ais->type9.accuracy,
			ais->type9.longitude, 
			ais->type9.latitude, 
			ais->type9.cog, 
			ais->type9.utc_second);
	    break;
        case 10: /* UTC/Date inquiry */
	    ais->type10.spare          = UBITS(38, 2);
	    ais->type10.dest_mmsi      = UBITS(40, 30);
	    ais->type10.spare2         = UBITS(70, 2);
	    gpsd_report(LOG_INF, "dest=%u\n", ais->type10.dest_mmsi);
	    break;
        case 12: /* Safety Related Message */
	    ais->type12.seqno          = UBITS(38, 2);
	    ais->type12.dest_mmsi      = UBITS(40, 30);
	    ais->type12.retransmit     = (bool)UBITS(70, 1);
	    ais->type12.spare          = UBITS(71, 1);
	    from_sixbit((char *)ais_context->bits, 
			72, ais_context->bitlen-72,
			ais->type12.text);
	    gpsd_report(LOG_INF, "seqno=%d, dest=%u\n",
			ais->type12.seqno,
			ais->type12.dest_mmsi);
	    break;
        case 13: /* Safety Related Acknowledge */
	    for (i = 0; i < sizeof(ais->type13.mmsi)/sizeof(ais->type13.mmsi[0]); i++)
		if (ais_context->bitlen > 40 + 32*i)
		    ais->type13.mmsi[i] = UBITS(40 + 32*i, 30);
	        else
		    ais->type13.mmsi[i] = 0;
	    gpsd_report(LOG_INF, "\n");
	    break;
        case 14: /* Safety Related Broadcast Message */
	    ais->type14.spare          = UBITS(38, 2);
	    from_sixbit((char *)ais_context->bits, 
			40, ais_context->bitlen-40,
			ais->type14.text);
	    gpsd_report(LOG_INF, "\n");
	    break;
	case 18:	/* Standard Class B CS Position Report */
	    ais->type18.reserved = UBITS(38, 8);
	    ais->type18.sog = UBITS(46, 10);
	    ais->type18.accuracy = (bool)UBITS(56, 1)!=0;
	    ais->type18.longitude = SBITS(57, 28);
	    ais->type18.latitude = SBITS(85, 27);
	    ais->type18.cog = UBITS(112, 12);
	    ais->type18.heading = UBITS(124, 9);
	    ais->type18.utc_second = UBITS(133, 6);
	    ais->type18.regional = UBITS(139, 2);
	    ais->type18.cs_flag = UBITS(141, 1)!=0;
	    ais->type18.display_flag = UBITS(142, 1)!=0;
	    ais->type18.dsc_flag = UBITS(143, 1)!=0;
	    ais->type18.band_flag = UBITS(144, 1)!=0;
	    ais->type18.msg22_flag = UBITS(145, 1)!=0;
	    ais->type18.assigned = UBITS(146, 1)!=0;
	    ais->type18.raim = UBITS(147, 1)!=0;
	    ais->type18.radio = UBITS(148, 20);
	    gpsd_report(LOG_INF,
			"reserved=%x SOG=%d Q=%d Lon=%d Lat=%d COG=%d TH=%d Sec=%d\n",
			ais->type18.reserved,
			ais->type18.sog, 
			(uint)ais->type18.accuracy,
			ais->type18.longitude, 
			ais->type18.latitude, 
			ais->type18.cog, 
			ais->type18.heading, 
			ais->type18.utc_second);
	    break;	
	case 19:	/* Extended Class B CS Position Report */
	    ais->type19.reserved = UBITS(38, 8);
	    ais->type19.sog = UBITS(46, 10);
	    ais->type19.accuracy = (bool)UBITS(56, 1)!=0;
	    ais->type19.longitude = SBITS(57, 28);
	    ais->type19.latitude = SBITS(85, 27);
	    ais->type19.cog = UBITS(112, 12);
	    ais->type19.heading = UBITS(124, 9);
	    ais->type19.utc_second = UBITS(133, 6);
	    ais->type19.regional = UBITS(139, 4);
	    UCHARS(143, ais->type19.vessel_name);
	    ais->type19.ship_type    = UBITS(263, 8);
	    ais->type19.to_bow       = UBITS(271, 9);
	    ais->type19.to_stern     = UBITS(280, 9);
	    ais->type19.to_port      = UBITS(289, 6);
	    ais->type19.to_starboard = UBITS(295, 6);
	    ais->type19.epfd         = UBITS(299, 4);
	    ais->type19.raim = UBITS(302, 1)!=0;
	    ais->type19.dte = UBITS(305, 1)!=0;
	    ais->type19.assigned = UBITS(306, 1)!=0;
	    ais->type19.spare = UBITS(307, 5);
	    gpsd_report(LOG_INF,
			"reserved=%x SOG=%d Q=%d Lon=%d Lat=%d COG=%d TH=%d Sec=%d name=%s\n",
			ais->type19.reserved,
			ais->type19.sog, 
			(uint)ais->type19.accuracy,
			ais->type19.longitude, 
			ais->type19.latitude, 
			ais->type19.cog, 
			ais->type19.heading, 
			ais->type19.utc_second,
		        ais->type19.vessel_name);
	    break;
	case 21:	/* Aid-to-Navigation Report */
	    ais->type21.type = UBITS(38, 5);
	    UCHARS(43, ais->type21.name);
	    ais->type21.accuracy     = UBITS(163, 163);
	    ais->type21.longitude    = UBITS(164, 28);
	    ais->type21.latitude     = UBITS(192, 27);
	    ais->type21.to_bow       = UBITS(219, 9);
	    ais->type21.to_stern     = UBITS(228, 9);
	    ais->type21.to_port      = UBITS(237, 6);
	    ais->type21.to_starboard = UBITS(243, 6);
	    ais->type21.epfd         = UBITS(249, 4);
	    ais->type21.utc_second   = UBITS(253, 6);
	    ais->type21.off_position = UBITS(259, 1)!=0;
	    ais->type21.regional     = UBITS(260, 8);
	    ais->type21.raim         = UBITS(268, 1)!=0;
	    ais->type21.virtual_aid  = UBITS(269, 1)!=0;
	    ais->type21.assigned     = UBITS(270, 1)!=0;
	    ais->type21.spare        = UBITS(271, 1)!=0;
	    /* TODO: figure out how to handle Name Extension field */
	    gpsd_report(LOG_INF,
			"name=%s Q=%d Lon=%d Lat=%d Sec=%d\n",
			ais->type21.name,
			(uint)ais->type19.accuracy,
			ais->type19.longitude, 
			ais->type19.latitude, 
			ais->type19.utc_second);
	    break;
	case 24:	/* Type 24 - Class B CS Static Data Report */
	    ais->type24.part = UBITS(38, 2);
	    switch (ais->type24.part) {
	    case 0:
		UCHARS(40, ais->type24.a.vessel_name);
		ais->type24.a.spare	= UBITS(160, 8);
		break;
	    case 1:
		ais->type24.b.ship_type = UBITS(40, 8);
		UCHARS(48, ais->type24.b.vendor_id);
		UCHARS(90, ais->type24.b.callsign);
		if (AIS_AUXILIARY_MMSI(ais->mmsi))
		    ais->type24.b.mothership_mmsi   = UBITS(132, 30);
		else {
		    ais->type24.b.dim.to_bow        = UBITS(132, 9);
		    ais->type24.b.dim.to_stern      = UBITS(141, 9);
		    ais->type24.b.dim.to_port       = UBITS(150, 6);
		    ais->type24.b.dim.to_starboard  = UBITS(156, 6);
		}
		ais->type24.b.spare	    = UBITS(162, 8);
		break;
	    }
	    break;
	default:
	    gpsd_report(LOG_INF, "\n");
	    gpsd_report(LOG_ERROR, "Unparsed AIVDM message type %d.\n",ais->id);
	    break;
	} 
#undef UCHARS
#undef SBITS
#undef UBITS

	/* data is fully decoded */
	return true;
    }

    /* we're still waiting on another sentence */
    return false;
}
/*@ -charint @*/

void  aivdm_dump(struct ais_t *ais, bool scaled, bool json, FILE *fp)
{
    static char *nav_legends[] = {
	"Under way using engine",
	"At anchor",
	"Not under command",
	"Restricted manoeuverability",
	"Constrained by her draught",
	"Moored",
	"Aground",
	"Engaged in fishing",
	"Under way sailing",
	"Reserved for HSC",
	"Reserved for WIG",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Not defined",
    };
    static char *epfd_legends[] = {
	"Undefined",
	"GPS",
	"GLONASS",
	"Combined GPS/GLONASS",
	"Loran-C",
	"Chayka",
	"Integrated navigation system",
	"Surveyed",
	"Galileo",
    };

    static char *type_legends[100] = {
	"Not available",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Reserved for future use",
	"Wing in ground (WIG) - all ships of this type",
	"Wing in ground (WIG) - Hazardous category A",
	"Wing in ground (WIG) - Hazardous category B",
	"Wing in ground (WIG) - Hazardous category C",
	"Wing in ground (WIG) - Hazardous category D",
	"Wing in ground (WIG) - Reserved for future use",
	"Wing in ground (WIG) - Reserved for future use",
	"Wing in ground (WIG) - Reserved for future use",
	"Wing in ground (WIG) - Reserved for future use",
	"Wing in ground (WIG) - Reserved for future use",
	"Fishing",
	"Towing",
	"Towing: length exceeds 200m or breadth exceeds 25m",
	"Dredging or underwater ops",
	"Diving ops",
	"Military ops",
	"Sailing",
	"Pleasure Craft",
	"Reserved",
	"Reserved",
	"High speed craft (HSC) - all ships of this type",
	"High speed craft (HSC) - Hazardous category A",
	"High speed craft (HSC) - Hazardous category B",
	"High speed craft (HSC) - Hazardous category C",
	"High speed craft (HSC) - Hazardous category D",
	"High speed craft (HSC) - Reserved for future use",
	"High speed craft (HSC) - Reserved for future use",
	"High speed craft (HSC) - Reserved for future use",
	"High speed craft (HSC) - Reserved for future use",
	"High speed craft (HSC) - No additional information",
	"Pilot Vessel",
	"Search and Rescue vessel",
	"Tug",
	"Port Tender",
	"Anti-pollution equipment",
	"Law Enforcement",
	"Spare - Local Vessel",
	"Spare - Local Vessel",
	"Medical Transport",
	"Ship according to RR Resolution No. 18",
	"Passenger - all ships of this type",
	"Passenger - Hazardous category A",
	"Passenger - Hazardous category B",
	"Passenger - Hazardous category C",
	"Passenger - Hazardous category D",
	"Passenger - Reserved for future use",
	"Passenger - Reserved for future use",
	"Passenger - Reserved for future use",
	"Passenger - Reserved for future use",
	"Passenger - No additional information",
	"Cargo - all ships of this type",
	"Cargo - Hazardous category A",
	"Cargo - Hazardous category B",
	"Cargo - Hazardous category C",
	"Cargo - Hazardous category D",
	"Cargo - Reserved for future use",
	"Cargo - Reserved for future use",
	"Cargo - Reserved for future use",
	"Cargo - Reserved for future use",
	"Cargo - No additional information",
	"Tanker - all ships of this type",
	"Tanker - Hazardous category A",
	"Tanker - Hazardous category B",
	"Tanker - Hazardous category C",
	"Tanker - Hazardous category D",
	"Tanker - Reserved for future use",
	"Tanker - Reserved for future use",
	"Tanker - Reserved for future use",
	"Tanker - Reserved for future use",
	"Tanker - No additional information",
	"Other Type - all ships of this type",
	"Other Type - Hazardous category A",
	"Other Type - Hazardous category B",
	"Other Type - Hazardous category C",
	"Other Type - Hazardous category D",
	"Other Type - Reserved for future use",
	"Other Type - Reserved for future use",
	"Other Type - Reserved for future use",
	"Other Type - Reserved for future use",
	"Other Type - no additional information",
    };

#define TYPE_DISPLAY(n) (((n) < (sizeof(type_legends)/sizeof(type_legends[0]))) ? type_legends[n] : "INVALID SHIP TYPE")

    if (json)
	(void)fprintf(fp, "{'type'=%u,'ri'=%u,'MMSI'=%09u,", ais->id, ais->ri, ais->mmsi);
    else
	(void)fprintf(fp, "%u,%u,%09u,", ais->id, ais->ri, ais->mmsi);
    /*@ -formatconst @*/
    switch (ais->id) {
    case 1:	/* Position Report */
    case 2:
    case 3:
#define TYPE123_UNSCALED_CSV "%u,%d,%u,%u,%d,%d,%u,%u,%u,%x,%d,%x\n"
#define TYPE123_UNSCALED_JSON   "'st'=%u,'ROT'=%u,'SOG'=%u,'fq'=%u,'lon'=%d,'lat'=%d,'cog'=%u,'hd'=%u,'sec'=%u,'reg'=%x,'sp'=%d,'radio'=%x}\n"
#define TYPE123_SCALED_CSV "%s,%s,%.1f,%u,%.4f,%.4f,%u,%u,%u,%x,%d,%x\n"
#define TYPE123_SCALED_JSON   "'st'=%s,'ROT'=%s,'SOG'=%.1f,'fq'=%u,'lon'=%.4f,'lat'=%.4f,'cog'=%u,'hd'=%u,'sec'=%u,'reg'=%x,'sp'=%d,'radio'=%x}\n"
	if (scaled) {
	    char rotlegend[10];

	    /* 
	     * Express ROT as nan if not available, 
	     * "fastleft"/"fastright" for fast turns.
	     */
	    if (ais->type123.rot == -128)
		(void) strlcpy(rotlegend, "nan", sizeof(rotlegend));
	    else if (ais->type123.rot == -127)
		(void) strlcpy(rotlegend, "fastleft", sizeof(rotlegend));
	    else if (ais->type123.rot == 127)
		(void) strlcpy(rotlegend, "fastright", sizeof(rotlegend));
	    else
		(void)snprintf(rotlegend, sizeof(rotlegend),
			       "%.0f",
			       ais->type123.rot * ais->type123.rot / 4.733);

	    (void)fprintf(fp,
			  (json ? TYPE123_SCALED_JSON : TYPE123_SCALED_CSV),
			      
			  nav_legends[ais->type123.status],
			  rotlegend,
			  ais->type123.sog / 10.0, 
			  (uint)ais->type123.accuracy,
			  ais->type123.longitude / AIS_LATLON_SCALE, 
			  ais->type123.latitude / AIS_LATLON_SCALE, 
			  ais->type123.cog, 
			  ais->type123.heading, 
			  ais->type123.utc_second,
			  ais->type123.maneuver,
			  ais->type123.raim,
			  ais->type123.radio);
	} else {
	    (void)fprintf(fp,
			  (json ? TYPE123_UNSCALED_JSON : TYPE123_UNSCALED_CSV),
			  ais->type123.status,
			  ais->type123.rot,
			  ais->type123.sog, 
			  (uint)ais->type123.accuracy,
			  ais->type123.longitude,
			  ais->type123.latitude,
			  ais->type123.cog, 
			  ais->type123.heading, 
			  ais->type123.utc_second,
			  ais->type123.maneuver,
			  ais->type123.raim,
			  ais->type123.radio);
	}
#undef TYPE123_UNSCALED_CSV
#undef TYPE123_UNSCALED_JSON
#undef TYPE123_SCALED_CSV
#undef TYPE123_SCALED_JSON
	break;
    case 4:	/* Base Station Report */
    case 11:	/* UTC/Date Response */
#define TYPE4_UNSCALED_CSV "%04u:%02u:%02uT%02u:%02u:%02uZ,%u,%d,%d,%u,%u,%x\n"
#define TYPE4_UNSCALED_JSON "%4u:%02u:%02uT%02u:%02u:%02uZ,'q'=%u,'lon'=%d,'lat'=%d,'epfd'=%u,'sp'=%u,'radio'=%x}\n"
#define TYPE4_SCALED_CSV	"%4u:%02u:%02uT%02u:%02u:%02uZ,%u,%.4f,%.4f,%s,%u,%x\n"
#define TYPE4_SCALED_JSON "%4u:%02u:%02uT%02u:%02u:%02uZ,'q'=%u,'lon'=%.4f,'lat'=%.4f,'epfd'=%s,'sp'=%u,'radio'=%x}\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE4_SCALED_JSON : TYPE4_SCALED_CSV),
			  ais->type4.year,
			  ais->type4.month,
			  ais->type4.day,
			  ais->type4.hour,
			  ais->type4.minute,
			  ais->type4.second,
			  (uint)ais->type4.accuracy,
			  ais->type4.latitude / AIS_LATLON_SCALE, 
			  ais->type4.longitude / AIS_LATLON_SCALE,
			  epfd_legends[ais->type4.epfd],
			  ais->type4.raim,
			  ais->type4.radio);
	} else {
	    (void)fprintf(fp,
			  (json ? TYPE4_UNSCALED_JSON : TYPE4_UNSCALED_CSV),
			  ais->type4.year,
			  ais->type4.month,
			  ais->type4.day,
			  ais->type4.hour,
			  ais->type4.minute,
			  ais->type4.second,
			  (uint)ais->type4.accuracy,
			  ais->type4.latitude, 
			  ais->type4.longitude,
			  ais->type4.epfd,
			  ais->type4.raim,
			  ais->type4.radio);
	}
#undef TYPE4_UNSCALED_CSV
#undef TYPE4_UNSCALED_JSON
#undef TYPE4_SCALED_CSV
#undef TYPE4_SCALED_JSON
	break;
    case 5: /* Ship static and voyage related data */
#define TYPE5_SCALED_JSON "'ID'=%u,'AIS'=%u,'callsign'=%s,'name'=%s,'type'=%s,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%s,'eta'=%02u-%02uT%02u:%02uZ,'draught'=%.1f,'dest'=%s,'dte'=%u,'sp'=%u}\n"
#define TYPE5_SCALED_CSV "%u,%u,%s,%s,%s,%u,%u,%u,%u,%s,%02u-%02uT%02u:%02uZ,%.1f,%s,%u,%u\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE5_SCALED_JSON : TYPE5_SCALED_CSV),
			  ais->type5.imo_id,
			  ais->type5.ais_version,
			  ais->type5.callsign,
			  ais->type5.vessel_name,
			  TYPE_DISPLAY(ais->type5.ship_type),
			  ais->type5.to_bow,
			  ais->type5.to_stern,
			  ais->type5.to_port,
			  ais->type5.to_starboard,
			  epfd_legends[ais->type5.epfd],
			  ais->type5.month,
			  ais->type5.day,
			  ais->type5.hour,
			  ais->type5.minute,
			  ais->type5.draught / 10.0,
			  ais->type5.destination,
			  ais->type5.dte,
			  ais->type5.spare);
	} else {
#define TYPE5_UNSCALED_JSON "'ID'=%u,'AIS'=%u,'callsign'=%s,'name'=%s,'type'=%u,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%u,'eta'=%02u-%02uT%02u:%02uZ,'draught'=%u,'dest'=%s,'dte'=%u,'sp'=%u}\n"
#define TYPE5_UNSCALED_CSV "%u,%u,%s,%s,%u,%u,%u,%u,%u,%u,%02u-%02uT%02u:%02uZ,%u,%s,%u,%u\n"
	    (void)fprintf(fp,
			  (json ? TYPE5_UNSCALED_JSON : TYPE5_UNSCALED_CSV),
			  ais->type5.imo_id,
			  ais->type5.ais_version,
			  ais->type5.callsign,
			  ais->type5.vessel_name,
			  ais->type5.ship_type,
			  ais->type5.to_bow,
			  ais->type5.to_stern,
			  ais->type5.to_port,
			  ais->type5.to_starboard,
			  ais->type5.epfd,
			  ais->type5.month,
			  ais->type5.day,
			  ais->type5.hour,
			  ais->type5.minute,
			  ais->type5.draught,
			  ais->type5.destination,
			  ais->type5.dte,
			  ais->type5.spare);
	}
#undef TYPE5_UNSCALED_CSV
#undef TYPE5_UNSCALED_JSON
#undef TYPE5_SCALED_CSV
#undef TYPE5_SCALED_JSON
	break;
    case 6:	/* Binary Message */
#define TYPE6_CSV	"%u,%u,%u,%u,%u:%s\n"
#define TYPE6_JSON	"'seq'=%u,'dst'=%u,'rexmit'=%u,'appid'=%u,'data'=%u:%s}\n"
	    (void)fprintf(fp,
			  (json ? TYPE6_JSON : TYPE6_CSV),
			  ais->type6.seqno,
			  ais->type6.dest_mmsi,
			  ais->type6.retransmit,
			  ais->type6.application_id,
			  ais->type6.bitcount,
			  gpsd_hexdump(ais->type6.bitdata, 
				       (ais->type6.bitcount+7)/8));
#undef TYPE6_CSV
#undef TYPE6_JSON
	break;
    case 7:	/* Binary Acknowledge */
#define TYPE7_CSV  "%u,%u,%u,%u\n"
#define TYPE7_JSON	"'mmsi1'=%u,'mmsi2'=%u,'mmsi3'=%u,'mmsi4'=%u}\n"
	    (void)fprintf(fp,
			  (json ? TYPE7_JSON : TYPE7_CSV),
			  ais->type7.mmsi[0],
			  ais->type7.mmsi[1],
			  ais->type7.mmsi[2],
			  ais->type7.mmsi[3]);
#undef TYPE7_CSV
#undef TYPE7_JSON
	break;
    case 8:	/* Binary Broadcast Message */
#define TYPE8_CSV	"%u,%u:%s\n"
#define TYPE8_JSON	"'appid'=%u,'data'=%u:%s}\n"
	    (void)fprintf(fp,
			  (json ? TYPE8_JSON : TYPE8_CSV),
			  ais->type8.application_id,
			  ais->type8.bitcount,
			  gpsd_hexdump(ais->type8.bitdata, 
				       (ais->type8.bitcount+7)/8));
#undef TYPE8_CSV
#undef TYPE8_JSON
	break;
    case 9:
#define TYPE9_UNSCALED_CSV "%u,%u,%u,%d,%d,%u,%u,%x,%u,%d,%x\n"
#define TYPE9_UNSCALED_JSON   "'alt'=%u,'SOG'=%u,'fq'=%u,'lon'=%d,'lat'=%d,'cog'=%u,'sec'=%u,'reg'=%x,'dte'=%u,'sp'=%d,'radio'=%x}\n"
#define TYPE9_SCALED_CSV "%u,%u,%u,%.4f,%.4f,%.1f,%u,%x,%u,%d,%x\n"
#define TYPE9_SCALED_JSON   "'alt'=%u,'SOG'=%u,'fq'=%u,'lon'=%.4f,'lat'=%.4f,'cog'=%.1f,'sec'=%u,'reg'=%x,'dte'=%u,'sp'=%d,'radio'=%x}\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE9_SCALED_JSON : TYPE9_SCALED_CSV),
			      
			  ais->type9.altitude,
			  ais->type9.sog, 
			  (uint)ais->type9.accuracy,
			  ais->type9.longitude / AIS_LATLON_SCALE, 
			  ais->type9.latitude / AIS_LATLON_SCALE, 
			  ais->type9.cog / 10.0, 
			  ais->type9.utc_second,
			  ais->type9.regional,
			  ais->type9.dte, 
			  ais->type9.raim,
			  ais->type9.radio);
	} else {
	    (void)fprintf(fp,
			  (json ? TYPE9_UNSCALED_JSON : TYPE9_UNSCALED_CSV),
			  ais->type9.altitude,
			  ais->type9.sog, 
			  (uint)ais->type9.accuracy,
			  ais->type9.longitude,
			  ais->type9.latitude,
			  ais->type9.cog, 
			  ais->type9.utc_second,
			  ais->type9.regional,
			  ais->type9.dte, 
			  ais->type9.raim,
			  ais->type9.radio);
	}
#undef TYPE9_UNSCALED_CSV
#undef TYPE9_UNSCALED_JSON
#undef TYPE9_SCALED_CSV
#undef TYPE9_SCALED_JSON
	break;
    case 10:	/* UTC/Date Inquiry */
#define TYPE10_CSV  "%x,%u,%x\n"
#define TYPE10_JSON	"'sp'=%x,'dst'=%u,'sp2'=%x}\n"
	    (void)fprintf(fp,
			  (json ? TYPE10_JSON : TYPE10_CSV),
			  ais->type10.spare,
			  ais->type10.dest_mmsi,
			  ais->type10.spare2);
#undef TYPE10_CSV
#undef TYPE10_JSON
	break;
    case 12:	/* Safety Related Message */
#define TYPE12_CSV  "%u,%u,%u,%s\n"
#define TYPE12_JSON	"'seq'=%u,'dst'=%u,'rexmit'=%u,'text'=%s}\n"
	    (void)fprintf(fp,
			  (json ? TYPE12_JSON : TYPE12_CSV),
			  ais->type12.seqno,
			  ais->type12.dest_mmsi,
			  ais->type12.retransmit,
			  ais->type12.text);
#undef TYPE12_CSV
#undef TYPE12_JSON
	break;
    case 13:	/* Safety Related Acknowledge */
#define TYPE13_CSV  "%u,%u,%u,%u\n"
#define TYPE13_JSON	"'mmsi1'=%u,'mmsi2'=%u,'mmsi3'=%u,'mmsi4'=%u}\n"
	    (void)fprintf(fp,
			  (json ? TYPE13_JSON : TYPE13_CSV),
			  ais->type13.mmsi[0],
			  ais->type13.mmsi[1],
			  ais->type13.mmsi[2],
			  ais->type13.mmsi[3]);
#undef TYPE13_CSV
#undef TYPE13_JSON
	break;
    case 14:	/* Safety Related Broadcast Message */
#define TYPE14_CSV  "%s\n"
#define TYPE14_JSON	"'text'=%s}\n"
	    (void)fprintf(fp,
			  (json ? TYPE14_JSON : TYPE14_CSV),
			  ais->type14.text);
#undef TYPE14_CSV
#undef TYPE14_JSON
	break;
    case 18:
#define TYPE18_UNSCALED_CSV "%u,%u,%u,%d,%d,%u,%u,%u,%x,%u,%u,%u,%u,%u,%d,%x\n"
#define TYPE18_UNSCALED_JSON   "'res'=%u,'SOG'=%u,'fq'=%u,'lon'=%d,'lat'=%d,'cog'=%u,'hd'=%u,'sec'=%u,'reg'=%x,'cs'=%u,'disp'=%u,'dsc'=%u,'band'=%u,'msg22'=%u,'raim'=%u,'radio'=%x}\n"
#define TYPE18_SCALED_CSV "%u,%.1f,%u,%.4f,%.4f,%.1f,%u,%u,%x,%u,%u,%u,%u,%u,%u,%x\n"
#define TYPE18_SCALED_JSON   "'res'=%u,'SOG'=%.1f,'fq'=%u,'lon'=%.4f,'lat'=%.4f,'cog'=%.1f,'hd'=%u,'sec'=%u,'reg'=%x,'cs'=%u,'disp'=%u,'dsc'=%u,'band'=%u,'msg22'=%u,'raim'=%u,'radio'=%x}\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE18_SCALED_JSON : TYPE18_SCALED_CSV),
			      
			  ais->type18.reserved,
			  ais->type18.sog / 10.0, 
			  (uint)ais->type18.accuracy,
			  ais->type18.longitude / AIS_LATLON_SCALE, 
			  ais->type18.latitude / AIS_LATLON_SCALE, 
			  ais->type18.cog / 10.0,
			  ais->type18.heading,
			  ais->type18.utc_second,
			  ais->type18.regional,
			  ais->type18.cs_flag,
			  ais->type18.display_flag,
			  ais->type18.dsc_flag,
			  ais->type18.band_flag,
			  ais->type18.msg22_flag,
			  ais->type18.raim,
			  ais->type18.radio);
	} else {
	    (void)fprintf(fp,
			  (json ? TYPE18_UNSCALED_JSON : TYPE18_UNSCALED_CSV),
			  ais->type18.reserved,
			  ais->type18.sog, 
			  (uint)ais->type18.accuracy,
			  ais->type18.longitude,
			  ais->type18.latitude,
			  ais->type18.cog, 
			  ais->type18.heading,
			  ais->type18.utc_second,
			  ais->type18.regional,
			  ais->type18.cs_flag,
			  ais->type18.display_flag,
			  ais->type18.dsc_flag,
			  ais->type18.band_flag,
			  ais->type18.msg22_flag,
			  ais->type18.raim,
			  ais->type18.radio);
	}
#undef TYPE18_UNSCALED_CSV
#undef TYPE18_UNSCALED_JSON
#undef TYPE18_SCALED_CSV
#undef TYPE18_SCALED_JSON
	break;
    case 19:
#define TYPE19_UNSCALED_CSV "%u,%u,%u,%d,%d,%u,%u,%u,%x,%s,%u,%u,%u,%u,%u,%u,%d,%x\n"
#define TYPE19_UNSCALED_JSON   "'res'=%u,'SOG'=%u,'fq'=%u,'lon'=%d,'lat'=%d,'cog'=%u,'hd'=%u,'sec'=%u,'reg'=%x,'name'=%s,'type'=%u,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%u,'raim'=%d,'assigned'=%x}\n"
#define TYPE19_SCALED_CSV "%u,%.1f,%u,%.4f,%.4f,%.1f,%u,%u,%x,%s,%s,%u.%u.%u.%u,%s,%d,%x\n"
#define TYPE19_SCALED_JSON   "'res'=%u,'SOG'=%.1f,'fq'=%u,'lon'=%.4f,'lat'=%.4f,'cog'=%.1f,'hd'=%u,'sec'=%u,'reg'=%x,'name'=%s,'type'=%s,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%s,'raim'=%d,'assigned'=%x}\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE19_SCALED_JSON : TYPE19_SCALED_CSV),
			      
			  ais->type19.reserved,
			  ais->type19.sog / 10.0, 
			  (uint)ais->type19.accuracy,
			  ais->type19.longitude / AIS_LATLON_SCALE, 
			  ais->type19.latitude / AIS_LATLON_SCALE, 
			  ais->type19.cog / 10.0,
			  ais->type19.heading,
			  ais->type19.utc_second,
			  ais->type19.regional,
			  ais->type19.vessel_name,
			  TYPE_DISPLAY(ais->type19.ship_type),
			  ais->type19.to_bow,
			  ais->type19.to_stern,
			  ais->type19.to_port,
			  ais->type19.to_starboard,
			  epfd_legends[ais->type19.epfd],
			  ais->type19.raim,
			  ais->type19.assigned);
	} else {
	    (void)fprintf(fp,
			  (json ? TYPE19_UNSCALED_JSON : TYPE19_UNSCALED_CSV),
			  ais->type19.reserved,
			  ais->type19.sog, 
			  (uint)ais->type19.accuracy,
			  ais->type19.longitude,
			  ais->type19.latitude,
			  ais->type19.cog, 
			  ais->type19.heading,
			  ais->type19.utc_second,
			  ais->type19.regional,
			  ais->type19.vessel_name,
			  ais->type19.ship_type,
			  ais->type19.to_bow,
			  ais->type19.to_stern,
			  ais->type19.to_port,
			  ais->type19.to_starboard,
			  ais->type19.epfd,
			  ais->type19.raim,
			  ais->type19.assigned);
	}
#undef TYPE19_UNSCALED_CSV
#undef TYPE19_UNSCALED_JSON
#undef TYPE19_SCALED_CSV
#undef TYPE19_SCALED_JSON
	break;
    case 21: /* Ship static and voyage related data */
#define TYPE21_SCALED_JSON "'type'=%u,'name'=%s,'lon'=%.4f,'lat'=%.4f,'accuracy'=%u,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%s,'Sec'=%u,'regional'=%x,'raim'=%u,'virt'=%u,'sp'=%x}\n"
#define TYPE21_SCALED_CSV "%u,%s,%.4f,%.4f,%u,%u,%u,%u,%u,%s,%u,%x,%u,%u,%x\n"
	if (scaled) {
	    (void)fprintf(fp,
			  (json ? TYPE21_SCALED_JSON : TYPE21_SCALED_CSV),
			  ais->type21.type,
			  ais->type21.name,
			  ais->type21.longitude / AIS_LATLON_SCALE, 
			  ais->type21.latitude / AIS_LATLON_SCALE, 
			  ais->type21.accuracy,
			  ais->type21.to_bow,
			  ais->type21.to_stern,
			  ais->type21.to_port,
			  ais->type21.to_starboard,
			  epfd_legends[ais->type21.epfd],
			  ais->type21.utc_second,
			  ais->type21.regional,
			  ais->type21.raim,
			  ais->type21.virtual_aid,
			  ais->type21.spare);
	} else {
#define TYPE21_UNSCALED_JSON "'type'=%u,'name'=%s,'lon'=%d,'lat'=%d,'accuracy'=%u,'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u,'epsd'=%u,'Sec'=%u,regional-%x,'raim'=%u,'virt'=%u,'sp'=%x}\n"
#define TYPE21_UNSCALED_CSV "%u,%s,%d,%d,%u,%u,%u,%u,%u,%u,%x,%u,%u,%x\n"
	    (void)fprintf(fp,
			  (json ? TYPE21_UNSCALED_JSON : TYPE21_UNSCALED_CSV),
			  ais->type21.type,
			  ais->type21.name,
			  ais->type21.accuracy,
			  ais->type21.longitude,
			  ais->type21.latitude,
			  ais->type21.to_bow,
			  ais->type21.to_stern,
			  ais->type21.to_port,
			  ais->type21.to_starboard,
			  ais->type21.epfd,
			  ais->type21.utc_second,
			  ais->type21.regional,
			  ais->type21.raim,
			  ais->type21.virtual_aid,
			  ais->type21.spare);
	}
#undef TYPE21_UNSCALED_CSV
#undef TYPE21_UNSCALED_JSON
#undef TYPE21_SCALED_CSV
#undef TYPE21_SCALED_JSON
	break;
    case 24: /* Class B CS Static Data Report */
	(void)fprintf(fp, "%u,", ais->type24.part);
	if (ais->type24.part == 0) {
	    (void)fprintf(fp, json ? "'name'=%s,'spare'=%x\n" : "%s,%x\n", 
			  ais->type24.a.vessel_name, 
			  ais->type24.a.spare);
	} else if (ais->type24.part == 1) {
	    if (scaled) {
		(void)fprintf(fp, json ? "'type'=%s," : "%s,", 
			      TYPE_DISPLAY(ais->type24.b.ship_type));
	    } else {
		(void)fprintf(fp, json ? "'type'=%u," : "%u,",
			      ais->type24.b.ship_type);
	    }
	    (void)fprintf(fp, json ? "vendor_'id'=%s,'callsign'=%s," : "%s,%s,",
			  ais->type24.b.vendor_id,
			  ais->type24.b.callsign);
	    if (AIS_AUXILIARY_MMSI(ais->mmsi)) {
		(void)fprintf(fp, json ? "mothership_'mmsi'=%u}\n" : "%u\n",
			      ais->type24.b.mothership_mmsi);
	    } else {
		(void)fprintf(fp, json ? "'bow'=%u,'stern'=%u,'port'=%u,'starboard'=%u}\n" : "%u,%u,%u,%u\n",
			      ais->type24.b.dim.to_bow,
			      ais->type24.b.dim.to_stern,
			      ais->type24.b.dim.to_port,
			      ais->type24.b.dim.to_starboard);
	    }
	} else
	    (void)fprintf(fp, "illegal part value %u.\n", ais->type24.part);
	break;
    default:
	(void)fprintf(fp, "unknown AIVDM message content.\n");
	break;
    }
    /*@ +formatconst @*/
}

#endif /* defined(AIVDM_ENABLE) */
