/* slimterm.h - Header for slimterm, a minimal X11 terminal emulator */

#ifndef SLIMTERM_H
#define SLIMTERM_H

#define MAX_COLS 256
#define MAX_ROWS 128
#define SCROLLBACK_SIZE 1000

typedef struct {
    Display *dpy;
    Window win;
    XftDraw *draw;
    XftFont *font;
    XftColor colors[16]; /* 16 colors: 0-7 normal, 8-15 bright */
    int w, h;
    int col, row;
    int border;
    int font_width, font_height;
    /* For double-buffering */
    Pixmap pixmap;
} XWindow;

typedef struct {
    char data[MAX_ROWS][MAX_COLS];
    int fg[MAX_ROWS][MAX_COLS];
    int bg[MAX_ROWS][MAX_COLS];
    char alt_data[MAX_ROWS][MAX_COLS];
    int alt_fg[MAX_ROWS][MAX_COLS];
    int alt_bg[MAX_ROWS][MAX_COLS];
    char scrollback[SCROLLBACK_SIZE][MAX_COLS];
    int scrollback_fg[SCROLLBACK_SIZE][MAX_COLS];
    int scrollback_bg[SCROLLBACK_SIZE][MAX_COLS];
    int row, col;
    int alt_row, alt_col;
    int scroll_top, scroll_bottom;
    int scrollback_pos, scrollback_len;
    int scroll_offset;
    int use_alt_buffer;
    int sel_start_row, sel_start_col;
    int sel_end_row, sel_end_col;
    int selecting;
} Term;

#endif
