/* config.h - Configuration for slimterm */

/* Terminal type */
#define TERM_TYPE "xterm-256color"

/* Default dimensions */
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Border width */
#define BORDER_WIDTH 20

/* Font */
#define FONT_NAME "JetBrainsMono Nerd Font:size=15"

/* Colors */
const char *colors[] = {
    /* Normal colors (0-7) */
    "#071121", /* Black */
    "#f7768e", /* Red */
    "#73daca", /* Green */
    "#e0af68", /* Yellow */
    "#7aa2f7", /* Blue */
    "#bb9af7", /* Magenta */
    "#7dcfff", /* Cyan */
    "#c0caf5", /* White */
    /* Bright colors (8-15) */
    "#666666", /* Bright Black */
    "#FF6666", /* Bright Red */
    "#66FF66", /* Bright Green */
    "#FFFF66", /* Bright Yellow */
    "#6666FF", /* Bright Blue */
    "#FF66FF", /* Bright Magenta */
    "#66FFFF", /* Bright Cyan */
    "#CCCCCC", /* Bright White */
};

/* Default foreground and background colors (indices into colors array) */
#define DEFAULT_FG 7  /* White */
#define DEFAULT_BG 0  /* Black */

/* Selection colors (indices into colors array) */
#define SELECTION_FG 0  /* Black */
#define SELECTION_BG 7  /* White */

/* Mouse behavior */
#define MOUSE_SCROLL_LINES 3  /* Number of lines to scroll per mouse wheel tick */
