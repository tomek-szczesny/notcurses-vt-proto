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
	int curmem_x;
	int curmem_y;
	char* cbuf;	// Carry buffer (stuff that wasn't processed last time, plus stuff currently to be processed)
	size_t cbs;	// Carry buffer size
	size_t cs;	// Carry size (length of stuff stored in buffer)
};

struct ncvtsms {	// VT state machine state
	struct ncplane* n;
	struct ncvtctx* vtctx;
	//const char* buf;
	//size_t* s;
	ssize_t pos;	// current position
	ssize_t lop;	// position where the last output has been produced
};

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

static int vt_get_csi_param(struct ncvtsms* s) {
	// parses an argument that starts at pos+1
	// missing (default) argument is encoded as -1
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
	return output;

}


static int vt_sgr(struct ncvtsms* s) {
	int c;

	int r, g, b;	// These are needed inside the loop, but one can't declare after the label. :/
	bool fg;

 	c = vt_get_csi_param(s);
	while (c > -2) {	
		if (c == -1) c = 0;			// 0 is the default argument in all SGRs
		switch (c) {
			case 0:				// Reset or normal
				ncplane_set_fg_default(s->n);
				ncplane_set_bg_default(s->n);
				break;
			// TODO support more!
			case 38:			// Foreground color 	
			case 48:			// Background color 
				fg = (c == 38);
 				c = vt_get_csi_param(s);
				switch (c) {
					case 5: 	// 8-bit palette
 						c = vt_get_csi_param(s);
						vt_8bpal(s, c, fg);
						break;
					case 2:		// 24-bit RGB color
 						r = vt_get_csi_param(s);
 						g = vt_get_csi_param(s);
 						b = vt_get_csi_param(s);
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
 		c = vt_get_csi_param(s);
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

	// Jump over param bytes
	s->pos++; if (vt_eob(s)) return vt_end(s);
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

	switch (f) {
		case 0x6D:	// SGR
			return vt_sgr(s);	
		case 0x41:	// Cursor up
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

	ncplane_set_scrolling(n, 1);	// putvt makes sense only in scrollable planes.	
	
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

	fp = popen("cat 24bit.pattern", "r");
	//fp = popen("echo ⢠⠃⠀⡠⠞⠉⠀⠀⠉⠣ \ntoilet --gay Dupa", "r");
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
