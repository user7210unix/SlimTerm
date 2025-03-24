/* slimterm.c - A minimal X11 terminal emulator with Xft */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>

#if defined(__linux)
#include <pty.h>
#endif

#include "slimterm.h"
#include "config.h"

#define BUFSIZE 1024
#define DEFAULT_SHELL "/bin/bash"

/* Utility macros */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Configuration globals */
char *termname = TERM_TYPE;
unsigned int defaultfg = DEFAULT_FG;
unsigned int defaultbg = DEFAULT_BG;
unsigned int selection_fg = SELECTION_FG;
unsigned int selection_bg = SELECTION_BG;
XWindow xw;
Term term;

/* Global variables */
static int master_fd = -1; /* Master side of the PTY */
static pid_t child_pid = -1; /* PID of the shell process */
static char escape_buf[BUFSIZE];
static int escape_len = 0;
static int in_escape = 0;
static int current_fg = DEFAULT_FG; /* Default foreground color */
static int current_bg = DEFAULT_BG; /* Default background color */
static int saved_row = 0; /* For \033[s and \033[u */
static int saved_col = 0;
static int wrap = 1; /* Line wrapping enabled by default */
static int mouse_enabled = 0; /* Mouse reporting disabled by default */
static int mouse_mode = 0; /* Mouse tracking mode */

/* Error handling and termination */
void die(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    exit(1);
}

/* Memory allocation with error checking */
void *xmalloc(size_t len) {
    void *p = malloc(len);
    if (!p) die("malloc failed");
    return p;
}

void *xrealloc(void *p, size_t len) {
    p = realloc(p, len);
    if (!p) die("realloc failed");
    return p;
}

char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("strdup failed");
    return p;
}

/* Write to a file descriptor with error checking */
ssize_t xwrite(int fd, const char *buf, size_t len) {
    ssize_t ret = write(fd, buf, len);
    if (ret < 0) die("write failed");
    return ret;
}

/* Initialize the terminal buffer */
static void term_init(void) {
    memset(term.data, 0, sizeof(term.data));
    memset(term.fg, defaultfg, sizeof(term.fg));
    memset(term.bg, defaultbg, sizeof(term.bg));
    memset(term.alt_data, 0, sizeof(term.alt_data));
    memset(term.alt_fg, defaultfg, sizeof(term.alt_fg));
    memset(term.alt_bg, defaultbg, sizeof(term.alt_bg));
    memset(term.scrollback, 0, sizeof(term.scrollback));
    memset(term.scrollback_fg, defaultfg, sizeof(term.scrollback_fg));
    memset(term.scrollback_bg, defaultbg, sizeof(term.scrollback_bg));
    term.row = 0;
    term.col = 0;
    term.scroll_top = 0;
    term.scroll_bottom = xw.row - 1;
    term.scrollback_pos = 0;
    term.scrollback_len = 0;
    term.scroll_offset = 0;
    term.use_alt_buffer = 0;
    term.alt_row = 0;
    term.alt_col = 0;
    term.sel_start_row = -1;
    term.sel_start_col = -1;
    term.sel_end_row = -1;
    term.sel_end_col = -1;
    term.selecting = 0;
}

/* Clear a line in the terminal buffer */
static void term_clear_line(int r, char data[][MAX_COLS], int fg[][MAX_COLS], int bg[][MAX_COLS]) {
    memset(data[r], 0, MAX_COLS);
    for (int c = 0; c < MAX_COLS; c++) {
        fg[r][c] = defaultfg;
        bg[r][c] = defaultbg;
    }
}

/* Clear from the current cursor position to the end of the line */
static void term_clear_to_eol(void) {
    char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
    int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
    int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
    int row = term.use_alt_buffer ? term.alt_row : term.row;
    int col = term.use_alt_buffer ? term.alt_col : term.col;

    for (int c = col; c < xw.col; c++) {
        data[row][c] = 0;
        fg[row][c] = defaultfg;
        bg[row][c] = defaultbg;
    }
}

/* Clear from the cursor down */
static void term_clear_below(void) {
    char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
    int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
    int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
    int row = term.use_alt_buffer ? term.alt_row : term.row;

    term_clear_to_eol();
    for (int r = row + 1; r < xw.row; r++) {
        term_clear_line(r, data, fg, bg);
    }
}

/* Clear from the cursor up */
static void term_clear_above(void) {
    char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
    int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
    int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
    int row = term.use_alt_buffer ? term.alt_row : term.row;
    int col = term.use_alt_buffer ? term.alt_col : term.col;

    /* Clear from cursor to beginning of the current line */
    for (int c = 0; c <= col; c++) {
        data[row][c] = 0;
        fg[row][c] = defaultfg;
        bg[row][c] = defaultbg;
    }
    /* Clear all lines above the cursor */
    for (int r = 0; r < row; r++) {
        term_clear_line(r, data, fg, bg);
    }
}

/* Add a line to the scrollback buffer */
static void term_add_scrollback(int r) {
    if (term.scrollback_len < SCROLLBACK_SIZE) {
        term.scrollback_len++;
    } else {
        /* Shift scrollback buffer up */
        for (int i = 0; i < SCROLLBACK_SIZE - 1; i++) {
            memcpy(term.scrollback[i], term.scrollback[i + 1], MAX_COLS);
            memcpy(term.scrollback_fg[i], term.scrollback_fg[i + 1], MAX_COLS * sizeof(int));
            memcpy(term.scrollback_bg[i], term.scrollback_bg[i + 1], MAX_COLS * sizeof(int));
        }
        term.scrollback_pos = SCROLLBACK_SIZE - 1;
    }
    memcpy(term.scrollback[term.scrollback_pos], term.data[r], MAX_COLS);
    memcpy(term.scrollback_fg[term.scrollback_pos], term.fg[r], MAX_COLS * sizeof(int));
    memcpy(term.scrollback_bg[term.scrollback_pos], term.bg[r], MAX_COLS * sizeof(int));
    term.scrollback_pos = (term.scrollback_pos + 1) % SCROLLBACK_SIZE;
}

/* Scroll the terminal buffer up */
static void term_scroll_up(void) {
    if (term.use_alt_buffer) {
        for (int r = term.scroll_top; r < term.scroll_bottom; r++) {
            memcpy(term.alt_data[r], term.alt_data[r + 1], MAX_COLS);
            memcpy(term.alt_fg[r], term.alt_fg[r + 1], MAX_COLS * sizeof(int));
            memcpy(term.alt_bg[r], term.alt_bg[r + 1], MAX_COLS * sizeof(int));
        }
        term_clear_line(term.scroll_bottom, term.alt_data, term.alt_fg, term.alt_bg);
    } else {
        term_add_scrollback(term.scroll_top);
        for (int r = term.scroll_top; r < term.scroll_bottom; r++) {
            memcpy(term.data[r], term.data[r + 1], MAX_COLS);
            memcpy(term.fg[r], term.fg[r + 1], MAX_COLS * sizeof(int));
            memcpy(term.bg[r], term.bg[r + 1], MAX_COLS * sizeof(int));
        }
        term_clear_line(term.scroll_bottom, term.data, term.fg, term.bg);
    }
}

/* Add a character to the terminal buffer */
static void term_putc(char c) {
    /* Debug: Print each character being processed */
    fprintf(stderr, "term_putc: %c (row=%d, col=%d, alt=%d)\n", c, 
            term.use_alt_buffer ? term.alt_row : term.row, 
            term.use_alt_buffer ? term.alt_col : term.col, 
            term.use_alt_buffer);

    if (in_escape) {
        escape_buf[escape_len++] = c;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '?' || c == '@') {
            escape_buf[escape_len] = '\0';
            in_escape = 0;
            /* Handle ANSI escape sequences */
            if (strcmp(escape_buf, "[2J") == 0) { /* Clear screen */
                char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
                int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
                int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
                for (int r = 0; r < xw.row; r++) {
                    term_clear_line(r, data, fg, bg);
                }
                if (term.use_alt_buffer) {
                    term.alt_row = 0;
                    term.alt_col = 0;
                } else {
                    term.row = 0;
                    term.col = 0;
                }
            } else if (strcmp(escape_buf, "[H") == 0) { /* Move to top-left */
                if (term.use_alt_buffer) {
                    term.alt_row = 0;
                    term.alt_col = 0;
                } else {
                    term.row = 0;
                    term.col = 0;
                }
            } else if (strcmp(escape_buf, "[K") == 0) { /* Clear to end of line */
                term_clear_to_eol();
            } else if (strcmp(escape_buf, "[J") == 0) { /* Clear below cursor */
                term_clear_below();
            } else if (strcmp(escape_buf, "[1J") == 0) { /* Clear above cursor */
                term_clear_above();
            } else if (strcmp(escape_buf, "[?7h") == 0) { /* Enable line wrapping */
                wrap = 1;
            } else if (strcmp(escape_buf, "[?7l") == 0) { /* Disable line wrapping */
                wrap = 0;
            } else if (strcmp(escape_buf, "[?25h") == 0) { /* Show cursor */
                /* No-op for now */
            } else if (strcmp(escape_buf, "[?25l") == 0) { /* Hide cursor */
                /* No-op for now */
            } else if (strcmp(escape_buf, "[?1000h") == 0) { /* Enable mouse reporting (normal tracking) */
                mouse_enabled = 1;
                mouse_mode = 1000;
            } else if (strcmp(escape_buf, "[?1000l") == 0) { /* Disable mouse reporting */
                mouse_enabled = 0;
                mouse_mode = 0;
            } else if (strcmp(escape_buf, "[?1002h") == 0) { /* Enable mouse button press/release */
                mouse_enabled = 1;
                mouse_mode = 1002;
            } else if (strcmp(escape_buf, "[?1002l") == 0) { /* Disable mouse button press/release */
                mouse_enabled = 0;
                mouse_mode = 0;
            } else if (strcmp(escape_buf, "[?1003h") == 0) { /* Enable mouse any event */
                mouse_enabled = 1;
                mouse_mode = 1003;
            } else if (strcmp(escape_buf, "[?1003l") == 0) { /* Disable mouse any event */
                mouse_enabled = 0;
                mouse_mode = 0;
            } else if (strcmp(escape_buf, "[?1049h") == 0) { /* Switch to alternate screen buffer */
                term.use_alt_buffer = 1;
                for (int r = 0; r < xw.row; r++) {
                    term_clear_line(r, term.alt_data, term.alt_fg, term.alt_bg);
                }
                term.alt_row = 0;
                term.alt_col = 0;
            } else if (strcmp(escape_buf, "[?1049l") == 0) { /* Switch back to normal screen buffer */
                term.use_alt_buffer = 0;
                term.row = 0;
                term.col = 0;
            } else if (strcmp(escape_buf, "[?1h") == 0) { /* Enable application cursor keys */
                /* No-op for now */
            } else if (strcmp(escape_buf, "[?1l") == 0) { /* Disable application cursor keys */
                /* No-op for now */
            } else if (strcmp(escape_buf, "7") == 0) { /* Save cursor position (DECSC) */
                if (term.use_alt_buffer) {
                    saved_row = term.alt_row;
                    saved_col = term.alt_col;
                } else {
                    saved_row = term.row;
                    saved_col = term.col;
                }
            } else if (strcmp(escape_buf, "8") == 0) { /* Restore cursor position (DECRC) */
                if (term.use_alt_buffer) {
                    term.alt_row = saved_row;
                    term.alt_col = saved_col;
                    if (term.alt_row < 0) term.alt_row = 0;
                    if (term.alt_row >= xw.row) term.alt_row = xw.row - 1;
                    if (term.alt_col < 0) term.alt_col = 0;
                    if (term.alt_col >= xw.col) term.alt_col = xw.col - 1;
                } else {
                    term.row = saved_row;
                    term.col = saved_col;
                    if (term.row < 0) term.row = 0;
                    if (term.row >= xw.row) term.row = xw.row - 1;
                    if (term.col < 0) term.col = 0;
                    if (term.col >= xw.col) term.col = xw.col - 1;
                }
            } else if (escape_buf[1] >= '0' && escape_buf[1] <= '9') {
                /* Handle cursor movement: \033[<n>C (move right) */
                if (escape_buf[escape_len - 1] == 'C') {
                    int n = atoi(escape_buf + 2);
                    if (term.use_alt_buffer) {
                        term.alt_col += n;
                        if (term.alt_col >= xw.col) term.alt_col = xw.col - 1;
                    } else {
                        term.col += n;
                        if (term.col >= xw.col) term.col = xw.col - 1;
                    }
                }
                /* Handle cursor movement: \033[<n>A (move up) */
                else if (escape_buf[escape_len - 1] == 'A') {
                    int n = atoi(escape_buf + 2);
                    if (term.use_alt_buffer) {
                        term.alt_row -= n;
                        if (term.alt_row < 0) term.alt_row = 0;
                    } else {
                        term.row -= n;
                        if (term.row < 0) term.row = 0;
                    }
                }
                /* Handle cursor movement: \033[<n>B (move down) */
                else if (escape_buf[escape_len - 1] == 'B') {
                    int n = atoi(escape_buf + 2);
                    if (term.use_alt_buffer) {
                        term.alt_row += n;
                        if (term.alt_row >= xw.row) term.alt_row = xw.row - 1;
                    } else {
                        term.row += n;
                        if (term.row >= xw.row) term.row = xw.row - 1;
                    }
                }
                /* Handle cursor movement: \033[<n>D (move left) */
                else if (escape_buf[escape_len - 1] == 'D') {
                    int n = atoi(escape_buf + 2);
                    if (term.use_alt_buffer) {
                        term.alt_col -= n;
                        if (term.alt_col < 0) term.alt_col = 0;
                    } else {
                        term.col -= n;
                        if (term.col < 0) term.col = 0;
                    }
                }
                /* Handle cursor position: \033[<row>;<col>H */
                else if (escape_buf[escape_len - 1] == 'H') {
                    int row = 1, col = 1;
                    sscanf(escape_buf + 2, "%d;%d", &row, &col);
                    if (term.use_alt_buffer) {
                        term.alt_row = row - 1;
                        term.alt_col = col - 1;
                        if (term.alt_row < 0) term.alt_row = 0;
                        if (term.alt_row >= xw.row) term.alt_row = xw.row - 1;
                        if (term.alt_col < 0) term.alt_col = 0;
                        if (term.alt_col >= xw.col) term.alt_col = xw.col - 1;
                    } else {
                        term.row = row - 1;
                        term.col = col - 1;
                        if (term.row < 0) term.row = 0;
                        if (term.row >= xw.row) term.row = xw.row - 1;
                        if (term.col < 0) term.col = 0;
                        if (term.col >= xw.col) term.col = xw.col - 1;
                    }
                }
                /* Handle color sequences like \033[44;100m */
                else if (escape_buf[escape_len - 1] == 'm') {
                    char *ptr = escape_buf + 2;
                    while (*ptr) {
                        int code = 0;
                        sscanf(ptr, "%d", &code);
                        if (code == 0) { /* Reset */
                            current_fg = defaultfg;
                            current_bg = defaultbg;
                        } else if (code >= 30 && code <= 37) { /* Foreground color */
                            current_fg = code - 30;
                        } else if (code >= 40 && code <= 47) { /* Background color */
                            current_bg = code - 40;
                        } else if (code >= 90 && code <= 97) { /* Bright foreground */
                            current_fg = (code - 90) + 8;
                        } else if (code >= 100 && code <= 107) { /* Bright background */
                            current_bg = (code - 100) + 8;
                        }
                        /* Move to the next code */
                        while (*ptr && *ptr != ';') ptr++;
                        if (*ptr == ';') ptr++;
                    }
                }
                /* Handle scroll region: \033[<top>;<bottom>r */
                else if (escape_buf[escape_len - 1] == 'r') {
                    int top = 1, bottom = xw.row;
                    sscanf(escape_buf + 2, "%d;%d", &top, &bottom);
                    term.scroll_top = top - 1;
                    term.scroll_bottom = bottom - 1;
                    if (term.scroll_top < 0) term.scroll_top = 0;
                    if (term.scroll_bottom >= xw.row) term.scroll_bottom = xw.row - 1;
                }
                /* Handle insert character: \033[<n>@ */
                else if (escape_buf[escape_len - 1] == '@') {
                    int n = atoi(escape_buf + 2);
                    if (n <= 0) n = 1;
                    char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
                    int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
                    int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
                    int row = term.use_alt_buffer ? term.alt_row : term.row;
                    int col = term.use_alt_buffer ? term.alt_col : term.col;
                    /* Shift characters right */
                    for (int c = xw.col - 1; c >= col + n; c--) {
                        data[row][c] = data[row][c - n];
                        fg[row][c] = fg[row][c - n];
                        bg[row][c] = bg[row][c - n];
                    }
                    /* Clear the inserted space */
                    for (int c = col; c < col + n && c < xw.col; c++) {
                        data[row][c] = 0;
                        fg[row][c] = defaultfg;
                        bg[row][c] = defaultbg;
                    }
                }
            }
            escape_len = 0;
        }
        return;
    }

    if (c == '\033') { /* ESC */
        in_escape = 1;
        escape_len = 0;
        escape_buf[escape_len++] = c;
        return;
    }

    char (*data)[MAX_COLS] = term.use_alt_buffer ? term.alt_data : term.data;
    int (*fg)[MAX_COLS] = term.use_alt_buffer ? term.alt_fg : term.fg;
    int (*bg)[MAX_COLS] = term.use_alt_buffer ? term.alt_bg : term.bg;
    int *row_ptr = term.use_alt_buffer ? &term.alt_row : &term.row;
    int *col_ptr = term.use_alt_buffer ? &term.alt_col : &term.col;

    if (c == '\n') {
        (*row_ptr)++;
        *col_ptr = 0;
        if (*row_ptr > term.scroll_bottom) {
            term_scroll_up();
            *row_ptr = term.scroll_bottom;
        }
    } else if (c == '\r') {
        *col_ptr = 0;
    } else if (c == '\b') { /* Backspace */
        if (*col_ptr > 0) {
            (*col_ptr)--;
            data[*row_ptr][*col_ptr] = ' ';
            fg[*row_ptr][*col_ptr] = defaultfg;
            bg[*row_ptr][*col_ptr] = defaultbg;
        }
    } else if (c >= 32 && c <= 126) { /* Printable characters */
        if (*row_ptr < xw.row && *col_ptr < xw.col) {
            data[*row_ptr][*col_ptr] = c;
            fg[*row_ptr][*col_ptr] = current_fg;
            bg[*row_ptr][*col_ptr] = current_bg;
            (*col_ptr)++;
            if (*col_ptr >= xw.col && wrap) {
                (*row_ptr)++;
                *col_ptr = 0;
                if (*row_ptr > term.scroll_bottom) {
                    term_scroll_up();
                    *row_ptr = term.scroll_bottom;
                }
            }
        }
    }
}


/* Execute the shell in the slave PTY */
static void exec_shell(const char *cmd, char **args) {
    const char *shell = cmd ? cmd : DEFAULT_SHELL;

    /* Set TERM environment variable */
    setenv("TERM", termname, 1);
    /* Set a simple prompt */
    setenv("PS1", "$ ", 1);

    /* Ensure HOME is set and change to that directory */
    const char *home = getenv("HOME");
    if (!home) {
        /* Fallback to a default home directory if HOME is unset */
        home = "/root"; /* Adjust this based on your system */
        setenv("HOME", home, 1);
    }
    if (chdir(home) != 0) {
        fprintf(stderr, "exec_shell: Failed to change directory to %s: %s\n", home, strerror(errno));
    }

    /* Start the shell in interactive mode */
    if (!args) {
        if (strcmp(shell, "/bin/bash") == 0) {
            char *default_args[] = {(char *)shell, "-i", NULL}; /* -i for interactive mode */
            execvp(default_args[0], default_args);
        } else {
            char *default_args[] = {(char *)shell, NULL};
            execvp(default_args[0], default_args);
        }
    } else {
        execvp(args[0], args);
    }
    die("execvp failed");
}


/* Handle SIGCHLD from the child process */
static void sigchld_handler(int sig) {
    int status;
    pid_t pid = waitpid(child_pid, &status, WNOHANG);
    if (pid == child_pid) {
        if (WIFEXITED(status)) {
            exit(WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            exit(128 + WTERMSIG(status));
        }
    }
}

/* Create a new PTY and fork the shell */
int ptynew(const char *cmd, char **args) {
    int master, slave;
    struct winsize ws = {DEFAULT_ROWS, DEFAULT_COLS, 0, 0};

    if (openpty(&master, &slave, NULL, NULL, &ws) < 0) {
        die("openpty failed");
    }

    switch (child_pid = fork()) {
    case -1:
        die("fork failed");
        break;
    case 0:
        close(master);
        setsid();
        if (ioctl(slave, TIOCSCTTY, NULL) < 0) die("ioctl TIOCSCTTY failed");
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        exec_shell(cmd, args);
        break;
    default:
        close(slave);
        master_fd = master;
        signal(SIGCHLD, sigchld_handler);
        return master_fd;
    }
    return -1;
}

/* Read from the PTY and update the terminal buffer */
size_t ttyread(void) {
    static char buf[BUFSIZE];
    ssize_t n = read(master_fd, buf, BUFSIZE);
    if (n <= 0) {
        if (n < 0) die("read from PTY failed");
        exit(0);
    }
    /* Debug: Print raw PTY output */
    fprintf(stderr, "ttyread: ");
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\033') {
            fprintf(stderr, "\\033");
        } else {
            fprintf(stderr, "%c", buf[i]);
        }
    }
    fprintf(stderr, "\n");
    for (ssize_t i = 0; i < n; i++) {
        term_putc(buf[i]);
    }
    return n;
}

/* Write to the PTY */
void ttywrite(const char *s, size_t n) {
    xwrite(master_fd, s, n);
    /* Debug: Print what we're writing to the PTY */
    fprintf(stderr, "ttywrite: ");
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\033') {
            fprintf(stderr, "\\033");
        } else {
            fprintf(stderr, "%c", s[i]);
        }
    }
    fprintf(stderr, "\n");
}

/* Resize the PTY and terminal buffer */
void ttyresize(int col, int row) {
    struct winsize ws = {(short)row, (short)col, 0, 0};
    if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0) {
        fprintf(stderr, "ioctl TIOCSWINSZ failed: %s\n", strerror(errno));
    }
    xw.col = col;
    xw.row = row;
    xw.w = col * xw.font_width + 2 * xw.border;
    xw.h = row * xw.font_height + 2 * xw.border;
    XResizeWindow(xw.dpy, xw.win, xw.w, xw.h);
    /* Resize the pixmap for double-buffering */
    if (xw.pixmap) {
        XFreePixmap(xw.dpy, xw.pixmap);
    }
    xw.pixmap = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, DefaultScreen(xw.dpy)));
    XftDrawChange(xw.draw, xw.pixmap);
    term.scroll_bottom = xw.row - 1;
    /* Adjust cursor position */
    if (term.use_alt_buffer) {
        if (term.alt_row >= xw.row) term.alt_row = xw.row - 1;
        if (term.alt_col >= xw.col) term.alt_col = xw.col - 1;
    } else {
        if (term.row >= xw.row) term.row = xw.row - 1;
        if (term.col >= xw.col) term.col = xw.col - 1;
    }
}

/* Copy selected text to clipboard */
static void copy_selection(void) {
    if (term.sel_start_row == -1 || term.sel_end_row == -1) return;

    int start_row = MIN(term.sel_start_row, term.sel_end_row);
    int end_row = MAX(term.sel_start_row, term.sel_end_row);
    int start_col = term.sel_start_row < term.sel_end_row ? term.sel_start_col : term.sel_end_col;
    int end_col = term.sel_start_row < term.sel_end_row ? term.sel_end_col : term.sel_start_col;

    char *sel_text = xmalloc(MAX_COLS * (end_row - start_row + 1) + 1);
    int pos = 0;

    for (int r = start_row; r <= end_row; r++) {
        int src_row;
        char *data;
        if (r < term.scrollback_len) {
            src_row = (term.scrollback_pos - term.scrollback_len + r + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
            data = term.scrollback[src_row];
        } else {
            src_row = r - term.scrollback_len;
            if (src_row >= xw.row) break;
            data = term.use_alt_buffer ? term.alt_data[src_row] : term.data[src_row];
        }

        int c_start = (r == start_row) ? start_col : 0;
        int c_end = (r == end_row) ? end_col : xw.col - 1;
        for (int c = c_start; c <= c_end; c++) {
            if (data[c]) {
                sel_text[pos++] = data[c];
            }
        }
        if (r < end_row) sel_text[pos++] = '\n';
    }
    sel_text[pos] = '\0';

    Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", False);
    XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
    XChangeProperty(xw.dpy, xw.win, clipboard, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)sel_text, strlen(sel_text));
    free(sel_text);
}

/* Initialize X11 window */
void xinit(void) {
    xw.border = BORDER_WIDTH;
    xw.col = DEFAULT_COLS;
    xw.row = DEFAULT_ROWS;

    xw.dpy = XOpenDisplay(NULL);
    if (!xw.dpy) die("XOpenDisplay failed");

    int screen = DefaultScreen(xw.dpy);
    Visual *visual = DefaultVisual(xw.dpy, screen);
    Colormap colormap = DefaultColormap(xw.dpy, screen);

    /* Load font */
    xw.font = XftFontOpenName(xw.dpy, screen, FONT_NAME);
    if (!xw.font) die("XftFontOpenName failed");

    /* Get font dimensions */
    XGlyphInfo extents;
    XftTextExtentsUtf8(xw.dpy, xw.font, (FcChar8 *)"M", 1, &extents);
    xw.font_width = extents.xOff;
    xw.font_height = xw.font->ascent + xw.font->descent;

    /* Create window */
    Window root = RootWindow(xw.dpy, screen);
    xw.win = XCreateSimpleWindow(xw.dpy, root, 0, 0, 100, 100, 0,
                                 BlackPixel(xw.dpy, screen), BlackPixel(xw.dpy, screen));

    /* Create pixmap for double-buffering */
    xw.w = xw.col * xw.font_width + 2 * xw.border;
    xw.h = xw.row * xw.font_height + 2 * xw.border;
    xw.pixmap = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, screen));

    /* Create Xft draw context */
    xw.draw = XftDrawCreate(xw.dpy, xw.pixmap, visual, colormap);
    if (!xw.draw) die("XftDrawCreate failed");

    /* Load colors */
    for (int i = 0; i < 16; i++) {
        if (!XftColorAllocName(xw.dpy, visual, colormap, colors[i], &xw.colors[i])) {
            die("XftColorAllocName failed for color %d", i);
        }
    }

    /* Set window size based on font and terminal dimensions */
    XResizeWindow(xw.dpy, xw.win, xw.w, xw.h);

    XSelectInput(xw.dpy, xw.win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapWindow(xw.dpy, xw.win);
    XFlush(xw.dpy);

    term_init();
}


/* Draw the terminal buffer */
void xdraw(void) {
    /* Clear the pixmap (background) */
    XftDrawRect(xw.draw, &xw.colors[defaultbg], 0, 0, xw.w, xw.h);

    /* Determine selection boundaries */
    int sel_start_row = -1, sel_end_row = -1, sel_start_col = -1, sel_end_col = -1;
    if (term.sel_start_row != -1 && term.sel_end_row != -1) {
        sel_start_row = MIN(term.sel_start_row, term.sel_end_row);
        sel_end_row = MAX(term.sel_start_row, term.sel_end_row);
        sel_start_col = term.sel_start_row < term.sel_end_row ? term.sel_start_col : term.sel_end_col;
        sel_end_col = term.sel_start_row < term.sel_end_row ? term.sel_end_col : term.sel_start_col;
    }

    /* Draw scrollback and current buffer */
    int start_row = term.scroll_offset;
    int display_rows = MIN(xw.row, term.scrollback_len + xw.row);
    for (int r = 0; r < display_rows; r++) {
        int x = xw.border;
        int y = xw.border + (r + 1) * xw.font_height - xw.font->descent;
        int src_row;
        char *data;
        int *fg, *bg;

        if (r + start_row < term.scrollback_len) {
            /* Draw from scrollback */
            src_row = (term.scrollback_pos - term.scrollback_len + r + start_row + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
            data = term.scrollback[src_row];
            fg = term.scrollback_fg[src_row];
            bg = term.scrollback_bg[src_row];
        } else {
            /* Draw from current buffer */
            src_row = r + start_row - term.scrollback_len;
            if (src_row >= xw.row) break;
            data = term.use_alt_buffer ? term.alt_data[src_row] : term.data[src_row];
            fg = term.use_alt_buffer ? term.alt_fg[src_row] : term.fg[src_row];
            bg = term.use_alt_buffer ? term.alt_bg[src_row] : term.bg[src_row];
        }

        int actual_row = r + start_row - term.scrollback_len;
        int is_selected_row = (actual_row >= sel_start_row && actual_row <= sel_end_row);

        for (int c = 0; c < xw.col; c++) {
            int is_selected = 0;
            if (is_selected_row) {
                if (actual_row == sel_start_row && actual_row == sel_end_row) {
                    is_selected = (c >= sel_start_col && c <= sel_end_col);
                } else if (actual_row == sel_start_row) {
                    is_selected = (c >= sel_start_col);
                } else if (actual_row == sel_end_row) {
                    is_selected = (c <= sel_end_col);
                } else {
                    is_selected = 1;
                }
            }

            /* Draw background */
            XftDrawRect(xw.draw, &xw.colors[is_selected ? selection_bg : (bg[c] % 16)],
                        x, y - xw.font->ascent, xw.font_width, xw.font_height);
            if (data[c]) {
                /* Draw character */
                char ch[2] = {data[c], 0};
                XftDrawStringUtf8(xw.draw, &xw.colors[is_selected ? selection_fg : (fg[c] % 16)], xw.font,
                                  x, y, (FcChar8 *)ch, 1);
            }
            x += xw.font_width;
        }
    }

    /* Copy the pixmap to the window */
    XCopyArea(xw.dpy, xw.pixmap, xw.win, DefaultGC(xw.dpy, DefaultScreen(xw.dpy)), 0, 0, xw.w, xw.h, 0, 0);
    XFlush(xw.dpy);
}

/* Handle X11 events */
void xevent(void) {
    XEvent ev;
    while (XPending(xw.dpy)) {
        XNextEvent(xw.dpy, &ev);
        switch (ev.type) {
        case Expose:
            xdraw();
            break;
        case ConfigureNotify:
            {
                XConfigureEvent *cev = &ev.xconfigure;
                int new_cols = (cev->width - 2 * xw.border) / xw.font_width;
                int new_rows = (cev->height - 2 * xw.border) / xw.font_height;
                if (new_cols != xw.col || new_rows != xw.row) {
                    ttyresize(new_cols, new_rows);
                    xdraw();
                }
            }
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button4) { /* Scroll up */
                term.scroll_offset -= MOUSE_SCROLL_LINES;
                if (term.scroll_offset < -term.scrollback_len) {
                    term.scroll_offset = -term.scrollback_len;
                }
                xdraw();
            } else if (ev.xbutton.button == Button5) { /* Scroll down */
                term.scroll_offset += MOUSE_SCROLL_LINES;
                if (term.scroll_offset > 0) {
                    term.scroll_offset = 0;
                }
                xdraw();
            } else if (ev.xbutton.button == Button1) { /* Start selection */
                term.selecting = 1;
                term.sel_start_row = (ev.xbutton.y - xw.border) / xw.font_height + term.scrollback_len - term.scroll_offset;
                term.sel_start_col = (ev.xbutton.x - xw.border) / xw.font_width;
                term.sel_end_row = term.sel_start_row;
                term.sel_end_col = term.sel_start_col;
                if (mouse_enabled && mouse_mode >= 1000) {
                    /* Send mouse press event to the application */
                    int x = term.sel_start_col + 1;
                    int y = term.sel_start_row + 1 - term.scrollback_len + term.scroll_offset;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\033[M %c%c%c", 32, x + 32, y + 32);
                    ttywrite(buf, strlen(buf));
                }
                xdraw();
            }
            break;
        case ButtonRelease:
            if (ev.xbutton.button == Button1) {
                if (term.selecting) {
                    term.selecting = 0;
                    copy_selection();
                }
                if (mouse_enabled && mouse_mode >= 1000) {
                    /* Send mouse release event to the application */
                    int x = term.sel_end_col + 1;
                    int y = term.sel_end_row + 1 - term.scrollback_len + term.scroll_offset;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\033[M!%c%c", x + 32, y + 32);
                    ttywrite(buf, strlen(buf));
                }
                xdraw();
            }
            break;
        case MotionNotify:
            if (term.selecting) {
                term.sel_end_row = (ev.xmotion.y - xw.border) / xw.font_height + term.scrollback_len - term.scroll_offset;
                term.sel_end_col = (ev.xmotion.x - xw.border) / xw.font_width;
                if (mouse_enabled && mouse_mode >= 1002) {
                    /* Send mouse motion event to the application */
                    int x = term.sel_end_col + 1;
                    int y = term.sel_end_row + 1 - term.scrollback_len + term.scroll_offset;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\033[M\"%c%c", x + 32, y + 32);
                    ttywrite(buf, strlen(buf));
                }
                xdraw();
            }
            break;
        case KeyPress:
            {
                char buf[32];
                KeySym keysym;
                int len = XLookupString(&ev.xkey, buf, sizeof(buf), &keysym, NULL);
                int shift = ev.xkey.state & ShiftMask;
                int ctrl = ev.xkey.state & ControlMask;

                if (shift && ctrl && keysym == XK_C) { /* Ctrl+Shift+C */
                    copy_selection();
                    xdraw();
                } else if (shift && ctrl && keysym == XK_V) { /* Ctrl+Shift+V */
                    /* Request clipboard contents */
                    Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", False);
                    XConvertSelection(xw.dpy, clipboard, XA_STRING, clipboard, xw.win, CurrentTime);
                } else if (shift && (keysym == XK_Up || keysym == XK_Down)) {
                    /* Scrollback navigation */
                    if (keysym == XK_Up) {
                        term.scroll_offset--;
                        if (term.scroll_offset < -term.scrollback_len) {
                            term.scroll_offset = -term.scrollback_len;
                        }
                    } else if (keysym == XK_Down) {
                        term.scroll_offset++;
                        if (term.scroll_offset > 0) {
                            term.scroll_offset = 0;
                        }
                    }
                    xdraw();
                } else if (ctrl && keysym == XK_c) { /* Ctrl+C */
                    ttywrite("\003", 1);
                    xdraw();
                } else if (ctrl && keysym == XK_v) { /* Ctrl+V */
                    /* Request clipboard contents */
                    Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", False);
                    XConvertSelection(xw.dpy, clipboard, XA_STRING, clipboard, xw.win, CurrentTime);
                } else if (len > 0) {
                    ttywrite(buf, len);
                    xdraw();
                } else {
                    /* Handle special keys */
                    switch (keysym) {
                    case XK_Up:
                        ttywrite(shift ? "\033[1;2A" : "\033[A", shift ? 6 : 3);
                        break;
                    case XK_Down:
                        ttywrite(shift ? "\033[1;2B" : "\033[B", shift ? 6 : 3);
                        break;
                    case XK_Right:
                        ttywrite(shift ? "\033[1;2C" : "\033[C", shift ? 6 : 3);
                        break;
                    case XK_Left:
                        ttywrite(shift ? "\033[1;2D" : "\033[D", shift ? 6 : 3);
                        break;
                    case XK_Return:
                        ttywrite("\r", 1);
                        break;
                    case XK_BackSpace:
                        ttywrite("\b", 1);
                        break;
                    case XK_Tab:
                        ttywrite("\t", 1);
                        break;
                    }
                    xdraw();
                }
            }
            break;
        case SelectionNotify:
            {
                XSelectionEvent *sev = &ev.xselection;
                if (sev->property != None) {
                    Atom type;
                    int format;
                    unsigned long len, bytes_left;
                    unsigned char *data;
                    XGetWindowProperty(xw.dpy, xw.win, sev->property, 0, 0, False,
                                       AnyPropertyType, &type, &format, &len, &bytes_left, &data);
                    if (bytes_left > 0) {
                        XGetWindowProperty(xw.dpy, xw.win, sev->property, 0, bytes_left,
                                           False, AnyPropertyType, &type, &format, &len, &bytes_left, &data);
                        ttywrite((char *)data, len);
                        XFree(data);
                    }
                }
            }
            break;
        }
    }
}


/* Free X11 resources */
void xfree(void) {
    for (int i = 0; i < 16; i++) {
        XftColorFree(xw.dpy, DefaultVisual(xw.dpy, DefaultScreen(xw.dpy)),
                     DefaultColormap(xw.dpy, DefaultScreen(xw.dpy)), &xw.colors[i]);
    }
    XftDrawDestroy(xw.draw);
    XFreePixmap(xw.dpy, xw.pixmap);
    XftFontClose(xw.dpy, xw.font);
    XDestroyWindow(xw.dpy, xw.win);
    XCloseDisplay(xw.dpy);
}

/* Main event loop */
void run(void) {
    fd_set rfds;
    int xfd = ConnectionNumber(xw.dpy);
    int max_fd = master_fd > xfd ? master_fd : xfd;

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(master_fd, &rfds);
        FD_SET(xfd, &rfds);

        if (select(max_fd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            die("select failed");
        }

        if (FD_ISSET(master_fd, &rfds)) {
            ttyread();
            xdraw();
        }

        if (FD_ISSET(xfd, &rfds)) {
            xevent();
        }
    }
}

int main(int argc, char *argv[]) {
    const char *cmd = (argc > 1) ? argv[1] : NULL;
    char **args = (argc > 1) ? &argv[1] : NULL;

    xinit();
    ptynew(cmd, args);
    ttyresize(xw.col, xw.row);
    run();

    xfree();
    if (master_fd >= 0) close(master_fd);
    return 0;
}
