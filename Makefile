# slimterm - a minimal X11 terminal emulator
.POSIX:

include config.mk

SRC = slimterm.c
OBJ = $(SRC:.c=.o)
BIN = slimterm

all: $(BIN)

.c.o:
	$(CC) $(CFLAGS) -c $<

$(OBJ): slimterm.h config.h config.mk

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJ) $(BIN)-$(VERSION).tar.gz

dist: clean
	mkdir -p $(BIN)-$(VERSION)
	cp -R LICENSE Makefile README config.mk config.h slimterm.h $(SRC) \
		$(BIN)-$(VERSION)
	tar -cf - $(BIN)-$(VERSION) | gzip > $(BIN)-$(VERSION).tar.gz
	rm -rf $(BIN)-$(VERSION)

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all clean dist install uninstall
