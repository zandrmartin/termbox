#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "termbox.h"

#include "bytebuffer.inl"
#include "term.inl"
#include "input.inl"

struct cellbuf {
	int width;
	int height;
	struct tb_cell *cells;
};

#define CELL(buf, x, y) (buf)->cells[(y) * (buf)->width + (x)]
#define IS_CURSOR_HIDDEN(cx, cy) (cx == -1 || cy == -1)
#define LAST_COORD_INIT -1

static struct termios orig_tios;

static struct cellbuf back_buffer;
static struct cellbuf front_buffer;
static struct bytebuffer output_buffer;
static struct bytebuffer input_buffer;

static int termw;
static int termh;

static int inputmode = TB_INPUT_ESC;
static int outputmode = TB_OUTPUT_NORMAL;

static int inout;
static int winch_fds[2];

static int lastx = LAST_COORD_INIT;
static int lasty = LAST_COORD_INIT;
static int cursor_x = -1;
static int cursor_y = -1;

static uint16_t background = TB_DEFAULT;
static uint16_t foreground = TB_DEFAULT;

static void write_cursor(int x, int y);
static void write_sgr_fg(uint16_t fg);
static void write_sgr_bg(uint16_t bg);
static void write_sgr(uint16_t fg, uint16_t bg);

static void cellbuf_init(struct cellbuf *buf, int width, int height);
static void cellbuf_resize(struct cellbuf *buf, int width, int height);
static void cellbuf_clear(struct cellbuf *buf);
static void cellbuf_free(struct cellbuf *buf);

static void update_size(void);
static void update_term_size(void);
static void send_attr(uint16_t fg, uint16_t bg);
static void send_char(int x, int y, uint32_t c);
static void send_clear(void);
static void sigwinch_handler(int xxx);
static int wait_fill_event(struct tb_event *event, struct timeval *timeout);

/* may happen in a different thread */
static volatile int buffer_size_change_request;

/* -------------------------------------------------------- */

int tb_init(void)
{
	inout = open("/dev/tty", O_RDWR);
	if (inout == -1) {
		return TB_EFAILED_TO_OPEN_TTY;
	}

	if (init_term() < 0) {
		close(inout);
		return TB_EUNSUPPORTED_TERMINAL;
	}

	if (pipe(winch_fds) < 0) {
		close(inout);
		return TB_EPIPE_TRAP_ERROR;
	}

	struct sigaction sa;
	sa.sa_handler = sigwinch_handler;
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, 0);

	tcgetattr(inout, &orig_tios);

	struct termios tios;
	memcpy(&tios, &orig_tios, sizeof(tios));

	tios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
	tios.c_oflag &= ~OPOST;
	tios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tios.c_cflag &= ~(CSIZE | PARENB);
	tios.c_cflag |= CS8;
	tios.c_cc[VMIN] = 0;
	tios.c_cc[VTIME] = 0;
	tcsetattr(inout, TCSAFLUSH, &tios);

	bytebuffer_init(&input_buffer, 128);
	bytebuffer_init(&output_buffer, 32 * 1024);

	bytebuffer_puts(&output_buffer, funcs[T_ENTER_CA]);
	bytebuffer_puts(&output_buffer, funcs[T_ENTER_KEYPAD]);
	bytebuffer_puts(&output_buffer, funcs[T_HIDE_CURSOR]);
	send_clear();

	update_term_size();
	cellbuf_init(&back_buffer, termw, termh);
	cellbuf_init(&front_buffer, termw, termh);
	cellbuf_clear(&back_buffer);
	cellbuf_clear(&front_buffer);

	return 0;
}

void tb_shutdown(void)
{
	bytebuffer_puts(&output_buffer, funcs[T_SHOW_CURSOR]);
	bytebuffer_puts(&output_buffer, funcs[T_SGR0]);
	bytebuffer_puts(&output_buffer, funcs[T_CLEAR_SCREEN]);
	bytebuffer_puts(&output_buffer, funcs[T_EXIT_CA]);
	bytebuffer_puts(&output_buffer, funcs[T_EXIT_KEYPAD]);
	bytebuffer_flush(&output_buffer, inout);
	tcsetattr(inout, TCSAFLUSH, &orig_tios);

	shutdown_term();
	close(inout);
	close(winch_fds[0]);
	close(winch_fds[1]);

	cellbuf_free(&back_buffer);
	cellbuf_free(&front_buffer);
	bytebuffer_free(&output_buffer);
	bytebuffer_free(&input_buffer);
}

void tb_present(void)
{
	int x,y;
	struct tb_cell *back, *front;

	/* invalidate cursor position */
	lastx = LAST_COORD_INIT;
	lasty = LAST_COORD_INIT;

	if (buffer_size_change_request) {
		update_size();
		buffer_size_change_request = 0;
	}

	for (y = 0; y < front_buffer.height; ++y) {
		for (x = 0; x < front_buffer.width; ++x) {
			back = &CELL(&back_buffer, x, y);
			front = &CELL(&front_buffer, x, y);
			if (memcmp(back, front, sizeof(struct tb_cell)) == 0)
				continue;
			send_attr(back->fg, back->bg);
			send_char(x, y, back->ch);
			memcpy(front, back, sizeof(struct tb_cell));
		}
	}
	if (!IS_CURSOR_HIDDEN(cursor_x, cursor_y))
		write_cursor(cursor_x, cursor_y);
	bytebuffer_flush(&output_buffer, inout);
}

void tb_set_cursor(int cx, int cy)
{
	if (IS_CURSOR_HIDDEN(cursor_x, cursor_y) && !IS_CURSOR_HIDDEN(cx, cy))
		bytebuffer_puts(&output_buffer, funcs[T_SHOW_CURSOR]);

	if (!IS_CURSOR_HIDDEN(cursor_x, cursor_y) && IS_CURSOR_HIDDEN(cx, cy))
		bytebuffer_puts(&output_buffer, funcs[T_HIDE_CURSOR]);

	cursor_x = cx;
	cursor_y = cy;
	if (!IS_CURSOR_HIDDEN(cursor_x, cursor_y))
		write_cursor(cursor_x, cursor_y);
}

void tb_put_cell(int x, int y, const struct tb_cell *cell)
{
	if (x >= back_buffer.width || y >= back_buffer.height)
		return;
	CELL(&back_buffer, x, y) = *cell;
}

void tb_change_cell(int x, int y, uint32_t ch, uint16_t fg, uint16_t bg)
{
	struct tb_cell c = {ch, fg, bg};
	tb_put_cell(x, y, &c);
}

void tb_blit(int x, int y, int w, int h, const struct tb_cell *cells)
{
	if (x+w > back_buffer.width || y+h > back_buffer.height)
		return;

	int sy;
	struct tb_cell *dst = &CELL(&back_buffer, x, y);
	size_t size = sizeof(struct tb_cell) * w;

	for (sy = 0; sy < h; ++sy) {
		memcpy(dst, cells, size);
		dst += back_buffer.width;
		cells += w;
	}
}

int tb_poll_event(struct tb_event *event)
{
	return wait_fill_event(event, 0);
}

int tb_peek_event(struct tb_event *event, int timeout)
{
	struct timeval tv;
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout - (tv.tv_sec * 1000)) * 1000;
	return wait_fill_event(event, &tv);
}

int tb_width(void)
{
	return termw;
}

int tb_height(void)
{
	return termh;
}

void tb_clear(void)
{
	if (buffer_size_change_request) {
		update_size();
		buffer_size_change_request = 0;
	}
	cellbuf_clear(&back_buffer);
}

int tb_select_input_mode(int mode)
{
	if (mode)
		inputmode = mode;
	return inputmode;
}

int tb_select_output_mode(int mode)
{
	if (mode)
		outputmode = mode;
	return outputmode;
}

void tb_set_clear_attributes(uint16_t fg, uint16_t bg)
{
	foreground = fg;
	background = bg;
}

/* -------------------------------------------------------- */

static int convertnum(uint32_t num, char* buf) {
	int i, l = 0;
	int ch;
	do {
		buf[l++] = '0' + (num % 10);
		num /= 10;
	} while (num);
	for(i = 0; i < l / 2; i++) {
		ch = buf[i];
		buf[i] = buf[l - 1 - i];
		buf[l - 1 - i] = ch;
	}
	return l;
}

#define WRITE_LITERAL(X) bytebuffer_append(&output_buffer, (X), sizeof(X)-1)
#define WRITE_INT(X) bytebuffer_append(&output_buffer, buf, convertnum((X), buf))

static void write_cursor(int x, int y) {
	char buf[32];
	WRITE_LITERAL("\033[");
	WRITE_INT(y+1);
	WRITE_LITERAL(";");
	WRITE_INT(x+1);
	WRITE_LITERAL("H");
}

// can only be called in NORMAL output mode
static void write_sgr_fg(uint16_t fg) {
	char buf[32];

	WRITE_LITERAL("\033[3");
	WRITE_INT(fg-1);
	WRITE_LITERAL("m");
}

// can only be called in NORMAL output mode
static void write_sgr_bg(uint16_t bg) {
	char buf[32];

	WRITE_LITERAL("\033[4");
	WRITE_INT(bg-1);
	WRITE_LITERAL("m");
}

static void write_sgr(uint16_t fg, uint16_t bg) {
	char buf[32];

	switch (outputmode) {
	case TB_OUTPUT_256:
	case TB_OUTPUT_216:
	case TB_OUTPUT_GRAYSCALE:
		WRITE_LITERAL("\033[38;5;");
		WRITE_INT(fg);
		WRITE_LITERAL("m");
		WRITE_LITERAL("\033[48;5;");
		WRITE_INT(bg);
		WRITE_LITERAL("m");
		break;
	case TB_OUTPUT_NORMAL:
	default:
		WRITE_LITERAL("\033[3");
		WRITE_INT(fg-1);
		WRITE_LITERAL(";4");
		WRITE_INT(bg-1);
		WRITE_LITERAL("m");
	}
}

static void cellbuf_init(struct cellbuf *buf, int width, int height)
{
	buf->cells = (struct tb_cell*)malloc(sizeof(struct tb_cell) * width * height);
	assert(buf->cells);
	buf->width = width;
	buf->height = height;
}

static void cellbuf_resize(struct cellbuf *buf, int width, int height)
{
	if (buf->width == width && buf->height == height)
		return;

	int oldw = buf->width;
	int oldh = buf->height;
	struct tb_cell *oldcells = buf->cells;

	cellbuf_init(buf, width, height);
	cellbuf_clear(buf);

	int minw = (width < oldw) ? width : oldw;
	int minh = (height < oldh) ? height : oldh;
	int i;

	for (i = 0; i < minh; ++i) {
		struct tb_cell *csrc = oldcells + (i * oldw);
		struct tb_cell *cdst = buf->cells + (i * width);
		memcpy(cdst, csrc, sizeof(struct tb_cell) * minw);
	}

	free(oldcells);
}

static void cellbuf_clear(struct cellbuf *buf)
{
	int i;
	int ncells = buf->width * buf->height;

	for (i = 0; i < ncells; ++i) {
		buf->cells[i].ch = ' ';
		buf->cells[i].fg = foreground;
		buf->cells[i].bg = background;
	}
}

static void cellbuf_free(struct cellbuf *buf)
{
	free(buf->cells);
}

static void get_term_size(int *w, int *h)
{
	struct winsize sz;
	memset(&sz, 0, sizeof(sz));

	ioctl(inout, TIOCGWINSZ, &sz);

	if (w) *w = sz.ws_col;
	if (h) *h = sz.ws_row;
}

static void update_term_size(void)
{
	struct winsize sz;
	memset(&sz, 0, sizeof(sz));

	ioctl(inout, TIOCGWINSZ, &sz);

	termw = sz.ws_col;
	termh = sz.ws_row;
}

static void send_attr(uint16_t fg, uint16_t bg)
{
#define LAST_ATTR_INIT 0xFFFF
	static uint16_t lastfg = LAST_ATTR_INIT, lastbg = LAST_ATTR_INIT;
	if (fg != lastfg || bg != lastbg) {
		bytebuffer_puts(&output_buffer, funcs[T_SGR0]);

		uint16_t fgcol;
		uint16_t bgcol;

		switch (outputmode) {
		case TB_OUTPUT_256:
			fgcol = fg & 0xFF;
			bgcol = bg & 0xFF;
			break;

		case TB_OUTPUT_216:
			fgcol = fg & 0xFF; if(fgcol > 215) fgcol = 7;
			bgcol = bg & 0xFF; if(bgcol > 215) bgcol = 0;
			fgcol += 0x10;
			bgcol += 0x10;
			break;

		case TB_OUTPUT_GRAYSCALE:
			fgcol = fg & 0xFF; if(fgcol > 23) fg = 23;
			bgcol = bg & 0xFF; if(bgcol > 23) bg = 0;
			fgcol += 0xe8;
			bgcol += 0xe8;
			break;

		case TB_OUTPUT_NORMAL:
		default:
			fgcol = fg & 0x0F;
			bgcol = bg & 0x0F;
		}

		if (fg & TB_BOLD)
			bytebuffer_puts(&output_buffer, funcs[T_BOLD]);
		if (bg & TB_BOLD)
			bytebuffer_puts(&output_buffer, funcs[T_BLINK]);
		if (fg & TB_UNDERLINE)
			bytebuffer_puts(&output_buffer, funcs[T_UNDERLINE]);
		if ((fg & TB_REVERSE) || (bg & TB_REVERSE))
			bytebuffer_puts(&output_buffer, funcs[T_REVERSE]);

		switch (outputmode) {
		case TB_OUTPUT_256:
		case TB_OUTPUT_216:
		case TB_OUTPUT_GRAYSCALE:
			write_sgr(fgcol, bgcol);
			break;

		case TB_OUTPUT_NORMAL:
		default:
			if (fgcol != TB_DEFAULT) {
				if (bgcol != TB_DEFAULT)
					write_sgr(fgcol, bgcol);
				else
					write_sgr_fg(fgcol);
			} else if (bgcol != TB_DEFAULT) {
				write_sgr_bg(bgcol);
			}
		}

		lastfg = fg;
		lastbg = bg;
	}
}

static void send_char(int x, int y, uint32_t c)
{
	char buf[7];
	int bw = tb_utf8_unicode_to_char(buf, c);
	buf[bw] = '\0';
	if (x-1 != lastx || y != lasty)
		write_cursor(x, y);
	lastx = x; lasty = y;
	if(!c) buf[0] = ' '; // replace 0 with whitespace
	bytebuffer_puts(&output_buffer, buf);
}

static void send_clear(void)
{
	send_attr(foreground, background);
	bytebuffer_puts(&output_buffer, funcs[T_CLEAR_SCREEN]);
	if (!IS_CURSOR_HIDDEN(cursor_x, cursor_y))
		write_cursor(cursor_x, cursor_y);
	bytebuffer_flush(&output_buffer, inout);

	/* we need to invalidate cursor position too and these two vars are
	 * used only for simple cursor positioning optimization, cursor
	 * actually may be in the correct place, but we simply discard
	 * optimization once and it gives us simple solution for the case when
	 * cursor moved */
	lastx = LAST_COORD_INIT;
	lasty = LAST_COORD_INIT;
}

static void sigwinch_handler(int xxx)
{
	(void) xxx;
	const int zzz = 1;
	write(winch_fds[1], &zzz, sizeof(int));
}

static void update_size(void)
{
	update_term_size();
	cellbuf_resize(&back_buffer, termw, termh);
	cellbuf_resize(&front_buffer, termw, termh);
	cellbuf_clear(&front_buffer);
	send_clear();
}

static int wait_fill_event(struct tb_event *event, struct timeval *timeout)
{
	// ;-)
#define ENOUGH_DATA_FOR_PARSING 64
	fd_set events;
	memset(event, 0, sizeof(struct tb_event));

	// try to extract event from input buffer, return on success
	event->type = TB_EVENT_KEY;
	if (extract_event(event, &input_buffer, inputmode))
		return TB_EVENT_KEY;

	// it looks like input buffer is incomplete, let's try the short path,
	// but first make sure there is enough space
	int prevlen = input_buffer.len;
	bytebuffer_resize(&input_buffer, prevlen + ENOUGH_DATA_FOR_PARSING);
	ssize_t r = read(inout,	input_buffer.buf + prevlen,
		ENOUGH_DATA_FOR_PARSING);
	if (r < 0) {
		// EAGAIN / EWOULDBLOCK shouldn't occur here
		assert(errno != EAGAIN && errno != EWOULDBLOCK);
		return -1;
	} else if (r > 0) {
		bytebuffer_resize(&input_buffer, prevlen + r);
		if (extract_event(event, &input_buffer, inputmode))
			return TB_EVENT_KEY;
	} else {
		bytebuffer_resize(&input_buffer, prevlen);
	}

	// r == 0, or not enough data, let's go to select
	while (1) {
		FD_ZERO(&events);
		FD_SET(inout, &events);
		FD_SET(winch_fds[0], &events);
		int maxfd = (winch_fds[0] > inout) ? winch_fds[0] : inout;
		int result = select(maxfd+1, &events, 0, 0, timeout);
		if (!result)
			return 0;

		if (FD_ISSET(inout, &events)) {
			event->type = TB_EVENT_KEY;
			prevlen = input_buffer.len;
			bytebuffer_resize(&input_buffer,
				prevlen + ENOUGH_DATA_FOR_PARSING);
			r = read(inout, input_buffer.buf + prevlen,
				ENOUGH_DATA_FOR_PARSING);
			if (r < 0) {
				// EAGAIN / EWOULDBLOCK shouldn't occur here
				assert(errno != EAGAIN && errno != EWOULDBLOCK);
				return -1;
			}
			bytebuffer_resize(&input_buffer, prevlen + r);
			if (r == 0)
				continue;
			if (extract_event(event, &input_buffer, inputmode))
				return TB_EVENT_KEY;
		}
		if (FD_ISSET(winch_fds[0], &events)) {
			event->type = TB_EVENT_RESIZE;
			int zzz = 0;
			read(winch_fds[0], &zzz, sizeof(int));
			buffer_size_change_request = 1;
			get_term_size(&event->w, &event->h);
			return TB_EVENT_RESIZE;
		}
	}
}
