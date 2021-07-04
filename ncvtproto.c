#define LIBSSH_STATIC 1
#include "libssh/libssh.h"
#include "notcurses/notcurses.h"
//#include "vt_colors.h"
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
 
struct ncvtctx {	// VT context
	int curmem_x;
	int curmem_y;
	char cbuf[20];	// Carry buffer (stuff that wasn't processed last time)	TODO: arbitrary size!
	size_t cs;	// Carry size
};

struct ncvtsms {	// VT state machine state
	struct ncplane* n;
	struct ncvtctx* vtctx;
	const char* buf;
	size_t* s;
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
		p =- 231;
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
	return (sms->pos >= sms->vtctx->cs + *(sms->s));
}

// Checks how many bytes are availabe in the buffer past pos
static inline size_t vt_ppos(struct ncvtsms* sms) {
	return (sms->vtctx->cs + *(sms->s) - sms->pos - 1);
}

// byte fetch, from cbuf or buf accordingly
static const char* vt_bfetch_p(const struct ncvtsms* s, size_t pos) {
	if (pos < s->vtctx->cs) return (s->vtctx->cbuf + pos);
	else return (s->buf + pos - s->vtctx->cs);
}

// Same but always fetches the byte from current position
static const char* vt_bfetch(const struct ncvtsms* s) {
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
	ssize_t stubp = s->lop;
	char stub[20];		// This sucks so badly
	while (stubp <= s->pos) {
		stub[stubp-s->lop] = *vt_bfetch_p(s, stubp+1);
		stubp++;
	}
	strcpy(s->vtctx->cbuf, stub);
	s->vtctx->cs = s->pos - s->lop - 1;
	return 0;	// Oh jeez...
}

// -------------------- ACTUAL PARSING STATES

// After detecting '\x1b\x5b'
static int vt_csi(struct ncvtsms* s) {
	s->pos++; if (vt_eob(s)) return vt_end(s);

	// Parameter, intermediate and final bytes, as defined for CSI
	int p = 0;
	char pb[30];	// TODO: these arrays may need resizing! 
	int i = 0;
	char ib[30];
	char f;

	while (*vt_bfetch(s) >= 0x30 && *vt_bfetch(s) <=0x3F) {
		pb[p] = *vt_bfetch(s); p++;
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}
	while (*vt_bfetch(s) >= 0x20 && *vt_bfetch(s) <=0x2F) {
		ib[i] = *vt_bfetch(s); i++;
		s->pos++; if (vt_eob(s)) return vt_end(s);
	}
	f = *vt_bfetch(s);

	char * param;	// This cannot be just after a label.

	switch (f) {
		case 0x6D:	// SGR
			param = strtok(pb,";");	i	// TODO: Extract parameters outside case (not only SGRs use them you know)
			while (param != NULL) {		// TODO: Support default parameters if number is missing
				char pc = atoi(param);
				switch (pc) {
					case 0:
						ncplane_set_fg_default(s->n);
						ncplane_set_bg_default(s->n);
						break;
					// TODO support more!
					default:
						if (pc >= 30 && pc <=37) vt_8bpal(s, pc - 30, 1);
						if (pc >= 90 && pc <=97) vt_8bpal(s, pc - 82, 1);
						if (pc >= 40 && pc <=47) vt_8bpal(s, pc - 40, 0);
						if (pc >= 100 && pc <=107) vt_8bpal(s, pc - 92, 0);
				}
				param = strtok(NULL, ";");
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

// Parsing UTF-8 EGCs
static int vt_utf8(struct ncvtsms* s) {
	// nah... some other time. 
	// but anyway, the idea is to check if the whole datapoint is in the buffer
	// If it is, stick it to putegc()
	// if not, call vt_end().
	return vt_pass(s);	

}

// -------------------- PUTVT

ssize_t ncplane_putvt(struct ncplane* n, struct ncvtctx* vtctx, const char* buf, size_t s) {

	ncplane_set_scrolling(n, 1);	// putvt makes sense only in scrollable planes.	
	
	// Initialize state
	struct ncvtsms sms;
	sms.n = n;
	sms.vtctx = vtctx;
	sms.buf = buf;
	sms.s = &s;
	sms.pos = 0;
	sms.lop = -1;

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
				default: r = vt_pass(&sms);
				
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
	t0ctx.cs = 0;	// TODO: more elegant (and complete) vtctx initializer

	FILE *fp;
	char buf[16];

	fp = popen("toilet --gay Dupa", "r");
	if (fp == NULL) {
		printf("Failed to run command\n" );
		exit(1);
	}

	size_t s;
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
