#include "notcurses/notcurses.h"
#include <stdlib.h>

// Please create a header file for this!

int vt_4bc(struct ncplane* n, char c) {
	// Here convert 4-bit color code from 'c' into a notcurses call
	// color code table: https://en.wikipedia.org/wiki/ANSI_escape_code#3-bit_and_4-bit
	// Assume VGA palette for now.
	// Return '-1' on failure, and '1' if succeeded.
	
	// ncplane_set_fg_rgb8(n, red, green, blue)
	// ncplane_set_bg_rgb8(n, red, green, blue)
}

int vt_8bc(struct ncplane* n, char c) {
	// Here convert 8-bit color code from 'c' into a notcurses call
	// color code table: https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit
	// Again, assume VGA pallete for lowest 16 codes.
	// Return '-1' on failure, and '1' if succeeded
	// (although in this case there are no invalid color codes, only ncplane calls may fail).
	
	// ncplane_set_fg_rgb8(n, red, green, blue)
	// ncplane_set_bg_rgb8(n, red, green, blue)
}
