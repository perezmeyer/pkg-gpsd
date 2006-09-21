#!/usr/bin/perl
# $Id$
###############################################################################
##									     ##
##	File:     invert-bitfields.pl					     ##
##	Author:   Wolfgang S. Rupprecht <wolfgang@wsrcc.com>		     ##
##	Created:  Mon Jul 25 01:58:12 PDT 2005				     ##
##	Contents: invert the bitfields                                       ##
##									     ##
##	Copyright (c) 2005 Wolfgang S. Rupprecht.			     ##
##	All rights reserved.						     ##
##									     ##
###############################################################################

$inside    = 0;
$linestack = "";

while (<>) {
    if (/^[ 	]+uint[ 	]+parity:6;/) {
        $inside = 1;

        # print ">>> starting inside\n";

        $linestack = $_ . $linestack;
    }
    elsif (/^[ 	]+uint[ 	]+_pad:2;/) {
        $inside    = 0;
        $linestack = $_ . $linestack;
        print $linestack;
        $linestack = "";
        # print ">>> starting outside\n";
    }
    else {
        if ($inside) {
            $linestack = $_ . $linestack;
        }
        else {
            print $_;
        }
    }
}

# end

