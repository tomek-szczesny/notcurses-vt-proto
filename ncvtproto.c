#define LIBSSH_STATIC 1
#include "libssh/libssh.h"
#include "notcurses/notcurses.h"
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
 
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

// After detecting '\x1b\x5b'
static int vt_csi(struct ncvtsms* s) {
	s->pos++; if (vt_eob(s)) return vt_end(s);

	// Parameter, intermediate and final bytes, as defined for CSI
	int p = 0;
	char pb[30];	// TODO: these arrays may need resizing! 
	int  pi[10];	// parameter ints
	int i = 0;	// Intermediate bytes counter
	char ib[30];	// Intermediate bytes
	char f;		// Final byte
	char * delims = ";:,";
	char * temp;	// used by strtoks
	int it = 0;	// iteration counter

	while (*vt_bfetch(s) >= 0x30 && *vt_bfetch(s) <=0x3F) {
		pb[p] = *vt_bfetch(s); p++;
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}
	pb[p] = 0x00;	// limit strtok

	while (*vt_bfetch(s) >= 0x20 && *vt_bfetch(s) <=0x2F) {
		ib[i] = *vt_bfetch(s); i++;
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}

	f = *vt_bfetch(s);
	
	// At this point we are sure the CSI is complete and we may carry on parsing it
	
	temp = strtok(pb, delims);
	p = 0;
	while (temp != NULL) {		// TODO: Support default parameters if number is missing
		pi[p] = atoi(temp);
		p++;
		temp = strtok(NULL, delims);
	}

	// Intermediate bytes not parsed yet - not used by anything supported so far

	switch (f) {
		case 0x6D:	// SGR
			for (it = 0; it < p; it++) {	
				switch (pi[it]) {
					case 0:				// Reset or normal
						ncplane_set_fg_default(s->n);
						ncplane_set_bg_default(s->n);
						break;
					case 38:			// Foreground color 
						it++;
						switch (pi[it]) {
							case 5: 	// 8-bit palette
								it++;
								vt_8bpal(s, pi[it], 1);
								break;
							case 2:		// 24-bit RGB color
								it++;
								ncplane_set_fg_rgb8(s->n, pi[it], pi[it+1], pi[it+2]);
								it += 2;
						}
						break;
					case 48:			// Background color 
						it++;
						switch (pi[it]) {
							case 5: 	// 8-bit palette
								it++;
								vt_8bpal(s, pi[it], 0);
								break;
							case 2:		// 24-bit RGB color
								it++;
								ncplane_set_bg_rgb8(s->n, pi[it], pi[it+1], pi[it+2]);
								it += 2;
						}
						break;
					// TODO support more!

					default:	// 3/4-bit colors
						if (pi[it] >= 30 && pi[it] <=37) vt_8bpal(s, pi[it] - 30, 1);
						if (pi[it] >= 90 && pi[it] <=97) vt_8bpal(s, pi[it] - 82, 1);
						if (pi[it] >= 40 && pi[it] <=47) vt_8bpal(s, pi[it] - 40, 0);
						if (pi[it] >= 100 && pi[it] <=107) vt_8bpal(s, pi[it] - 92, 0);
				}
			}
			s->lop = s->pos; return 1;
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

	fp = popen("cat 8bit.pattern", "r");
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
