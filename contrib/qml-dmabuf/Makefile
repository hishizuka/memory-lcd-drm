PKGS := libdrm gbm egl glesv2

CC ?= cc
PREFIX ?= /usr/local
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC -pthread \
	$(shell pkg-config --cflags $(PKGS))
LDLIBS += -pthread $(shell pkg-config --libs $(PKGS))

.PHONY: all clean install uninstall

all: libsharp_presenter.so

libsharp_presenter.so: sharp_presenter.c sharp_presenter.h
	$(CC) $(CFLAGS) -shared sharp_presenter.c -o $@ $(LDLIBS)

install: libsharp_presenter.so
	install -Dm755 libsharp_presenter.so \
		$(DESTDIR)$(PREFIX)/lib/libsharp_presenter.so
	install -Dm644 sharp_presenter.h \
		$(DESTDIR)$(PREFIX)/include/sharp_presenter.h

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/lib/libsharp_presenter.so
	$(RM) $(DESTDIR)$(PREFIX)/include/sharp_presenter.h

clean:
	$(RM) libsharp_presenter.so
