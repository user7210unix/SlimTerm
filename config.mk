# slimterm configuration

# Version
VERSION = 0.1

# Compiler and linker
CC = gcc
PREFIX = /usr/local

# Compiler flags
CFLAGS = -g -Wall -O2 -I. -I/usr/X11R6/include -I/usr/include/freetype2 -DVERSION=\"$(VERSION)\"
LDFLAGS = -g -L/usr/X11R6/lib -lX11 -lXft -lfontconfig
