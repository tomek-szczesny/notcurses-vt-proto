#define LIBSSH_STATIC 1
#include "libssh/libssh.h"
#include "notcurses/notcurses.h"
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// STUFF SUPPORTED SO FAR:
// UTF-8
// 3/4/8/24 bit colors
//
// TODO:
//
// \e[m			// SGR (TODO: Default argument)
// \e[2J		// Erase in display (args 0-3) 
// \e[J			// Erase in display 0
// \e[2d		// Line Position Absolute (Default 1)
// \e[30X		// Erase 30 characters (Default 1)
// \e[K			// Erase in line, args 0-2 (default 0)
// \e[y;xH		// Move cursor to y,x
// \e]0; ... \007 	// ESC ] = OSC, terminated with BEL (0x07) or ST (0x1b \), or nothing
// \e[?1049h		// Alternative screen buffer
// \e[?1049l		// Disable alternative screen buffer
// \e[1;27r		// Set scrolling region (from, to) (default top, bottom)
// \e[4h		// Set Mode (12 = Send/Receive; 20 = automatic newline; 4 = insert mode; +1)
// \e[4l		// Reset Mode (2 = Keyboard Action Mode, 4 = Replace mode; +2)
// \e[?7h		// Auto wrap mode (DECAWM)
// \e[?25h		// Show cursor
// \e[?25l		// Hide cursor
// \e[?1000h		// Send Mouse X & Y on button press and release. This is the X11 xterm mouse protocol.
// \e[?1000l		// Don't send...
//
// Essential:
// \e[m \e[2j \e[J \e[2d \e[30X \e[K \e[H
//
// WTF SEQUENCES:
// \e=			// Application Keypad (DECKPAM)
// \e[?1h		// Application cursor keys (DECCKM)
// \e[?1l		// Normal Cursor Keys
//
// WON'T IMPLEMENT:
// \e(B			// G0 character set -> USASCII 
// \e[22;0;0t		// Window Manipulation (XTWINOPS)
// \e[?12l		// Start/Stop blinking cursor


 
struct ncvtctx {	// VT context
	struct ncplane* n;
	char* ibuf;	// Input buffer (stuff that wasn't processed last time, plus stuff currently to be processed)
	size_t ibs;	// Input buffer size
	size_t bcs;	// Buffer contents size
	ssize_t pos;	// Current position in buffer (last byte read)
	ssize_t lop;	// Position of last byte that has been processed successfully

	int curmem_x;   // Cursor position memory - used by some esc sequences
	int curmem_y;
};

inline int
vtctx_init(struct ncvtctx* vtctx, struct ncplane* n) {

	vtctx->n = n;
	vtctx->pos = 0;
	vtctx->lop = -1;

	// Initialize internal buffer
	vtctx->ibs = 1;							// Buffer grows as needed, but never shrinks
	vtctx->ibuf = (char*) malloc (vtctx->ibs * sizeof(char)); 	// TODO - needs to be freed someday...
	vtctx->bcs = 0;
	
	return 0;							// TODO: Return something meaningful
}

int
vtctx_put(struct ncvtctx* vtctx, char* in, size_t len) {

	// Expand vtctx buffer if needed
	// Constant input buffer size is expected, but there may be leftovers from previous processing
	if (vtctx->bcs + len > vtctx->ibs) {
		vtctx-icbuf = realloc(vtctx->ibuf, (vtctx->bcs + len) * sizeof(char));
		vtctx->ibs = vtctx->bcs + len;
	}

	// Copy stuff into the vtctx buffer
	memcpy(vtctx->ibuf + vtctx->bcs, in, len);
	vtctx->bcs += len;	

	// TODO: Shrink the vtctx input buffer if less than half (?) is in use
	
	vtctx_process(vtctx);
	vtctx_cleanup(vtctx);

	return 0;							// TODO: Return something meaningful
}

static int	// Process data in internal buffer and produce output
vtctx_process(struct ncvtctx* vtctx) {

	vtctx->pos = -1;
	vtctx->lop = -1;
	
	int b;	// A currently processed byte

	// Bytes are processed as integers, with the following special cases:
	// -1 : End of buffer (returned by fetching/peeking functions)
	// -2 : No number found (returned by 'fetch number')

	while (vtctx_rem(vtctx) > 0) {

		b = vtctx_pb(vtctx);
		if (b < 0) return -1;

		if (b >= 0xC0 && b < 0xFE) {	// UTF-8 EGC
			vtctx_utf8(vtctx);
			continue;
		}

		if (b == 0x1B) {		// ESC sequence
			vtctx->pos++;
			b = vtctx_pb(vtctx);
			if (b == 0x5B) {
				vtctx->pos++;
				vtctx_csi(vtctx);
				continue;
			}
			// if nothing fits, print "\e" and carry on as if it was 1-byte EGC
			ncplane_putstr(vtctx->n, "\\e");
			continue;
		}

		// For everything else, treat it as UTF-8 EGC
		vtctx_utf8(vtctx);

	}

	return 0;

}

static int		// Do things after all buffer data has been processed (preserve interrupted sequences)
vtctx_cleanup(struct ncvtctx* vtctx) {

	// Move unprocessed stuff to the beginning of the buffer
	if (vtctx->lop < vtctx->pos) {
		memmove(vtctx->ibuf, vtctx->ibuf + vtctx->lop + 1, vtctx->bcs - vtctx->lop - 1);
		vtctx->bcs = vtctx->bcs - vtctx->lop - 1;
	}
	else {
		vtctx->bcs = 0;
	}
	return 0;							// TODO: Return something meaningful
}

static inline int	// Remaining bytes for processing
vtctx_rem(struct ncvtctx* vtctx) {
	return vtctx->bcs - vtctx->pos - 1;
}

static int		// Fetch byte; -1 means end of buffer
vtctx_fb(struct ncvtctx* vtctx) {
	if (vtctx_rem(vtctx) <= 0) return -1;
	else {
		vtctx->pos++;
		return *(vtctx->ibuf + vtctx->pos);
	}
}

static int		// Peek byte without pos++; -1 means end of buffer
vtctx_pb(struct ncvtctx* vtctx) {
	if (vtctx_rem(vtctx) <= 0) return -1;
	else {
		return *(vtctx->ibuf + vtctx->pos + 1);
	}
}

static int		// Fetch number
vtctx_fn(struct ncvtctx* vtctx) {
			// Returns -1 on end of buffer
			// Returns -2 if no number found (pos remains unchanged)

	int output = -1;
	int digit = vtctx_pb(vtctx);

	while (digit >= 48 && digit < 58) {
		if (output == -1) output = 0;
		output *= 10;
		output += digit - 48;			// Is this kosher?
		vtctx->pos++;
		digit = vtctx_pb(vtctx);
	}
	
	// If end of buffer has been reached, consider the whole number invalid
	if (digit == -1) return -1;

	// If no single digit has been found, return -2
	if (output == -1) return -2;

	return output;
}


static int // Parsing UTF-8 EGCs from current pos (including 1-byte ASCII)
vt_utf8(struct ncvtctx* vtctx) {
	
	size_t cpl = utf8_codepoint_length(vtctx_pb(vtctx));	// TODO: cast int to u_char

	if (cpl <= vtctx_rem(vtctx)){
		if (ncplane_putegc(vtctx->n, vtctx->ibuf + vtctx->pos + 1, NULL) >= 0) {
			vtctx->pos += cpl;
			vtctx->lop = vtctx->pos; return 1;
		}
		else;		// TODO: Else what?
	}
	else {
		// On failure due to end of buffer, push 'pos' forward anyway to stop the processing loop
		vtctx->pos += cpl;
		return -1;
	}

}

static inline size_t	// TODO: This should be in the library, dunno why I cannot call it. :/
utf8_codepoint_length(unsigned char c){
  if(c <= 0x7f){        // 0x000000...0x00007f
    return 1;
  }else if(c <= 0xc1){  // illegal continuation byte
    return 1;
  }else if(c <= 0xdf){  // 0x000080...0x0007ff
    return 2;
  }else if(c <= 0xef){  // 0x000800...0x00ffff
    return 3;
  }else if(c <= 0xf4){  // c <= 0xf4, 0x100000...0x10ffff
    return 4;
  }else{                // illegal first byte
    return 1;
  }
}

// After detecting '\x1b\x5b'
static int vtctx_csi(struct ncvtctx* vtctx) {

	// CSI structure is a mess:
	// 0x1B 0x5B [[first byte] parameters] [intermediate] final
	// Only first two bytes and final byte are mandatory.
	//
	// First byte of parameters array may or may not have special meaning.
	// For example, '?' means a completely different set of functions, called private CSIs.
	// Internally first byte is treated as a part of parameters list.
	// 
	// parameters are unsigned integers separated by semicolons (usually). Numbers between semicolons
	// may be missing, or even too few arguments may be given, or none given at all. In such
	// cases, each CSI function may assume different default values or behavior.
	//
	// Intermediate bytes are not implemented for now as no supported CSI use them.
	// Final byte is always given and determine the actual function.
	//
	// The strategy here is to fetch first byte, pointer to first byte, and final byte.
	// First byte decides on a subset of functions. Final byte picks a concrete function, and finally
	// parameters are extracted one by one, depending on the function.
	//
	// The following jumps over params just to get final byte - params are parsed later.
	
	int first_byte;
	char* params;
	int last_byte;
	int last_byte_pos;		// position in vtctx internal buffer

	first_byte = vtctx_fb(vtctx);
	params = vtctx->ibuf + vtctx->pos;

	// Jump over remaining param bytes
	int b = vtctx_pb(vtctx);
	while (b >= 0x30 && b <=0x3F) {
		vtctx->pos++;
		b = vtctx_pb(vtctx);
	}

	last_byte = vtctx_fb(vtctx);

	// Check if we're not over the vtctx internal buffer
	if (last_byte == -1) return -1;

	// Now depending on first byte and last byte, we pick a function and execute it

	ssize_t init_pos = s->pos;V
	char f;		// Final byte
	char ft;	// First byte (i.e. '?' for private sequences)

	s->pos++; if (vt_eob(s)) return vt_end(s);
	ft = *vt_bfetch(s);
	// Jump over param bytes
	while (*vt_bfetch(s) >= 0x30 && *vt_bfetch(s) <=0x3F) {
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}

	/*
	while (*vt_bfetch(s) >= 0x20 && *vt_bfetch(s) <=0x2F) {
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}
	*/

	f = *vt_bfetch(s);	// Get final byte
	s->pos = init_pos;	// Rewind pos to initial state, so param parsers don't get confused
	
	// At this point we are sure the CSI is complete and we may carry on interpreting it
	
	if (ft == '?') {	// Private sequences
		s->pos++;
		switch (f) {
			case 'h':
				return 1; // TODO
			break;
			case 'l':
				return 1; // TODO
			break;
			default: return vt_unknown(s);
		}
	}

	switch (f) {
		// Erase functions
		case 'J':	// Erase display
			switch (vt_get_csi_param(s, 0)){
				case 0:
					// Erase without moving cursor?
					return 1;
				case 1:
				// What does case 1 do?
				case 2:
				case 3:
					// Erase and home cursor
					return 1;
				default: return vt_unknown(s);
			}; 
		case 'K':	// Erase line, do not move cursor. Check the args.

			return 1;
		case 'X':
				// erase n(default 1) chars after cursor, don't move the cursor.
			return 1;

		// Cursor moving functions
		case 'd':	// Line position absolute (default 1)
				
			return 1;
		case 'H':	// Move cursor to x, y (y is the first argument)

			return 1;
		

		case 'A': 
		case 0x6D: return vt_sgr(s);	
		default: return vt_unknown(s);
	}
}






// Below the legacy
//
//
//
//
//
//





struct ncvtsms {	// VT state machine state
	struct ncplane* n;
	struct ncvtctx* vtctx;
	ssize_t pos;	// current position
	ssize_t lop;	// position where the last output has been produced
};


int vt_8bpal(struct ncvtsms* s, int p, bool fg) {
	int r, g, b;

	// The following 3/4 bit codes shall not be translated to RGB - that's a temporary solution
	if (p < 8) {
		r = ( p    % 2 ? 170 : 0);
		g = ((p/2) % 2 ? 170 : 0);
		b = ((p/4) % 2 ? 170 : 0);
	}
	else if (p >= 8 && p < 16) {
		r = ( p    % 2 ? 255 : 85);
		g = ((p/2) % 2 ? 255 : 85);
		b = ((p/4) % 2 ? 255 : 85);
	}
	
	// The following 8-bit codes probably should be handled by something else (ncplane_set_.g_palindex())

	else if (p >= 16 && p < 232) {
		p -= 16;
		b = (p % 6) * 51;
		p /= 6;
		g = (p % 6) * 51;
		p /= 6;
		r = (p % 6) * 51;
	}

	else {
		p -= 231;
		r = p * 8;
		g = r;
		b = r;
	}

	if (fg) ncplane_set_fg_rgb8(s->n, r, g, b);
	else    ncplane_set_bg_rgb8(s->n, r, g, b);
	
	return 1; // TODO react to actual ncplane call returns

}




// Checks if current byte is outside the buffer bounds
static inline bool vt_eob(struct ncvtsms* sms) {
	return (sms->pos >= sms->vtctx->cs);
}

// Checks how many bytes are availabe in the buffer past pos
static inline size_t vt_ppos(struct ncvtsms* sms) {
	return (sms->vtctx->cs - sms->pos - 1);
}

// byte fetch, from cbuf
static const inline char* vt_bfetch_p(const struct ncvtsms* s, size_t pos) {
	return (s->vtctx->cbuf + pos);
}

// Same but always fetches the byte from current position
static const inline char* vt_bfetch(const struct ncvtsms* s) {
	return vt_bfetch_p(s, s->pos);
}

// ------------------- A FEW GENERAL STATES
// All states return an integer code that acts as a feedback to the main loop / base state:
//  1 - Parsed OK, the main loop may continue
//  0 - Did not parse because the buffer has ended prematurely (returned by vt_end only)
// negative value - major oopsie (should be returned by vt_error only)
// Thus each state shall return whatever has been returned by the states called by them.

// Writes bytes directly to ncplane, from lop+1 to pos
static int vt_pass(struct ncvtsms* s) {
	// TODO: Do it better
	while (s->lop < s->pos) {
		s->lop++;
		ncplane_putchar(s->n, *vt_bfetch_p(s, s->lop));
	}
	return 1;	// TODO: check if ncplane_putwhatever has succeeded	
}

// Handles unknown state - when parser got unexpected data
static int vt_unknown(struct ncvtsms* s) {
	return vt_pass(s);	// For now we treat invalid codes as normal text
}

// Handles error state - returns error code of putvt 
static int vt_error(struct ncvtsms* s) {
	return -(s->lop);
}

// Handles end of buffer during parsing
static int vt_end(struct ncvtsms* s) {
	if (s->lop < s->pos) {
		memmove(s->vtctx->cbuf, s->vtctx->cbuf + s->lop + 1, s->vtctx->cs - s->lop - 1);
		s->vtctx->cs = s->vtctx->cs - s->lop - 1;
	}
	else {
		s->vtctx->cs = 0;
	}
	return 0;	// Oh jeez...
}

// -------------------- ACTUAL PARSING STATES


static int vt_get_csi_param(struct ncvtsms* s, int def) {
	// parses an argument that starts at pos+1
	// missing (default) argument is replaced with 'def'.
	// if no more params are available, returns -2
	// vt_eob: -3 (should never happen)
	// Don't feed it inconsistent data, or you'll get inconsistent results.
	
	
	if (strchr(";:?[", *vt_bfetch(s)) == NULL) return -2;
	s->pos++; if (vt_eob(s)) return -3;

	int output = -1;
	while (*vt_bfetch(s) >= 48 && *vt_bfetch(s) < 58) {
		if (output < 0) output = 0;
		output *= 10;
		output += *vt_bfetch(s) - 48;		// Is this kosher?
		s->pos++; if (vt_eob(s)) return -3;
	}
	if (output == -1) return def;
	else return output;

}


static int vt_sgr(struct ncvtsms* s) {
	int c;

	int r, g, b;	// These are needed inside the loop, but one can't declare after the label. :/
	bool fg;

 	c = vt_get_csi_param(s, 0);
	while (c > -2) {	
		switch (c) {
			case 0:				// Reset or normal
				ncplane_set_fg_default(s->n);
				ncplane_set_bg_default(s->n);
				break;
			// TODO support more!
			case 38:			// Foreground color 	
			case 48:			// Background color 
				fg = (c == 38);
 				c = vt_get_csi_param(s, 0);
				switch (c) {
					case 5: 	// 8-bit palette
 						c = vt_get_csi_param(s, 0);
						vt_8bpal(s, c, fg);
						break;
					case 2:		// 24-bit RGB color
 						r = vt_get_csi_param(s, 0);
 						g = vt_get_csi_param(s, 0);
 						b = vt_get_csi_param(s, 0);
						if (fg) ncplane_set_fg_rgb8(s->n, r, g, b);
						else    ncplane_set_bg_rgb8(s->n, r, g, b);
				}
				break;

			default:	// 3/4-bit colors
				if (c >= 30 && c <=37) vt_8bpal(s, c - 30, 1);
				if (c >= 90 && c <=97) vt_8bpal(s, c - 82, 1);
				if (c >= 40 && c <=47) vt_8bpal(s, c - 40, 0);
				if (c >= 100 && c <=107) vt_8bpal(s, c - 92, 0);
		}
 		c = vt_get_csi_param(s, 0);
	}
	return 1;	// TODO actual return lol
}
// After detecting '\x1b\x5b'
static int vt_csi(struct ncvtsms* s) {

	// Parameter, intermediate and final bytes, as defined for CSI
	// Intermediate bytes are disabled for now, not supported by any sequence ATM
	// The following jumps over params just to get final byte - params are parsed later.
	
	ssize_t init_pos = s->pos;
	char f;		// Final byte
	char ft;	// First byte (i.e. '?' for private sequences)

	s->pos++; if (vt_eob(s)) return vt_end(s);
	ft = *vt_bfetch(s);
	// Jump over param bytes
	while (*vt_bfetch(s) >= 0x30 && *vt_bfetch(s) <=0x3F) {
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}

	/*
	while (*vt_bfetch(s) >= 0x20 && *vt_bfetch(s) <=0x2F) {
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}
	*/

	f = *vt_bfetch(s);	// Get final byte
	s->pos = init_pos;	// Rewind pos to initial state, so param parsers don't get confused
	
	// At this point we are sure the CSI is complete and we may carry on interpreting it
	
	if (ft == '?') {	// Private sequences
		s->pos++;
		switch (f) {
			case 'h':
				return 1; // TODO
			break;
			case 'l':
				return 1; // TODO
			break;
			default: return vt_unknown(s);
		}
	}

	switch (f) {
		// Erase functions
		case 'J':	// Erase display
			switch (vt_get_csi_param(s, 0)){
				case 0:
					// Erase without moving cursor?
					return 1;
				case 1:
				// What does case 1 do?
				case 2:
				case 3:
					// Erase and home cursor
					return 1;
				default: return vt_unknown(s);
			}; 
		case 'K':	// Erase line, do not move cursor. Check the args.

			return 1;
		case 'X':
				// erase n(default 1) chars after cursor, don't move the cursor.
			return 1;

		// Cursor moving functions
		case 'd':	// Line position absolute (default 1)
				
			return 1;
		case 'H':	// Move cursor to x, y (y is the first argument)

			return 1;
		

		case 'A': 
		case 0x6D: return vt_sgr(s);	
		default: return vt_unknown(s);
	}
}

// Escape state - after detecting '\x1b'
static int vt_esc(struct ncvtsms* s) {
	s->pos++; if (vt_eob(s)) return vt_end(s);

	switch (*vt_bfetch(s)) {
		case 0x5B: return vt_csi(s); break;
		default: return vt_unknown(s);
	}
}

// Parsing UTF-8 EGCs (including 1-byte ASCII)
static int vt_utf8(struct ncvtsms* s) {
	size_t cpl = utf8_codepoint_length(*vt_bfetch(s));
	if (cpl <= vt_ppos(s) + 1){
		int q;
		if (ncplane_putegc(s->n, vt_bfetch(s), NULL) >= 0) {
			s->pos += cpl - 1;
			s->lop = s->pos; return 1;
		}
	}
	return vt_end(s);


}

// -------------------- PUTVT

ssize_t ncplane_putvt(struct ncplane* n, struct ncvtctx* vtctx, const char* buf, size_t s) {

	//ncplane_set_scrolling(n, 1);	// putvt makes sense only in scrollable planes.	
	
	// Initialize state
	struct ncvtsms sms;
	sms.n = n;
	sms.vtctx = vtctx;
	sms.pos = 0;
	sms.lop = -1;

	//fill vtctx with new buffer contents
	if (vtctx->cs + s > vtctx->cbs) {
		vtctx->cbuf = realloc(vtctx->cbuf, (vtctx->cs + s) * sizeof(char));
		vtctx->cbs = vtctx->cs + s;
	}
	memcpy(vtctx->cbuf + vtctx->cs, buf, s);
	vtctx->cs += s;	

	// The 'base' state is case in while loop, to avoid stack overflows with arbitrarily long buffers
	char c;
	int r;	// Return from state machine
	do {
		c = *vt_bfetch(&sms);	

		if (c >= 0xC0 && c < 0xFE) {	// UTF-8 EGC
			r = vt_utf8(&sms);
		}
		else {
			switch (c) {	// Other cases
				case 0x1B: r = vt_esc(&sms); break;
				default: r = vt_utf8(&sms);
				
			}
		}
		if (r == 1){
			sms.pos++; if (vt_eob(&sms)) r = vt_end(&sms);
		}
	}
	while (r == 1);

	return sms.lop;	// Might actually be greater than s, if cbuf wasn't empty.
}

// --------------------- MAIN (proof-of-concept test)

int main()
{

	struct notcurses* nc;
	nc = notcurses_core_init(NULL, stdout);
	setlocale(LC_ALL, "");

	struct ncplane_options defopts = {.y=10, .x=20, .rows=30, .cols=80};
	struct ncplane* t0 = ncplane_create(notcurses_stdplane(nc), &defopts);
	ncplane_set_scrolling(t0, 1);

	ncplane_putstr(t0, "Oto terminal nr 0, woohoo!\n"); 
	notcurses_render(nc);

	struct ncvtctx t0ctx;
	t0ctx.cbs = 1;
	t0ctx.cbuf = (char*) malloc (t0ctx.cbs * sizeof(char)); // TODO - needs to be freed someday...
	t0ctx.cs = 0;	// TODO: more elegant (and complete) vtctx initializer

	FILE *fp;
	char buf[256];
	size_t s;

	//fp = popen("cat 24bit.pattern", "r");
	fp = popen("echo ⢠⠃⠀⡠⠞⠉⠀⠀⠉⠣ \ntoilet --gay Dupa", "r");
	//fp = popen("unbuffer -efq ls /home/mctom", "r");
	if (fp == NULL) {
		printf("Failed to run command\n" );
		exit(1);
	}

	while (s = fread(buf, sizeof(char), sizeof(buf), fp)) {
  		//ncplane_putnstr(t0, s, buf); 
		ncplane_putvt(t0, &t0ctx, buf, s);
		notcurses_render(nc);
	}

	pclose(fp);

	system("sleep 5");	// for some reason putchar() doesn't work here.

	notcurses_stop(nc);

	return 0;

}
