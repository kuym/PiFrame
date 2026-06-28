################################################################
#
# PiFrame
# The Networked Digital Picture Frame for Raspberry Pi
#
# Build the two client editions and (optionally) install one of
#   them as a systemd service on the Raspberry Pi:
#
#   make                 build both editions
#   make piframe         build the GTK+ edition only
#   make piframe-fb      build the framebuffer edition only
#
#   sudo make install-fb    install the framebuffer edition as a service
#   sudo make install-gtk   install the GTK+ edition as a service
#   sudo make install       alias for install-fb (recommended for headless)
#
#   sudo make uninstall  stop, disable and remove the service + binaries
#   make clean           remove built binaries
#
# Override the install prefix if desired:  make install-fb PREFIX=/usr
#
################################################################

# --- install locations ------------------------------------- #

PREFIX      ?= /usr/local
BINDIR       = $(PREFIX)/bin
SHAREDIR     = $(PREFIX)/share/piframe
SYSTEMDDIR   = /etc/systemd/system
DEFAULTDIR   = /etc/default

INSTALL     ?= install

# --- toolchain --------------------------------------------- #

CC          ?= gcc
CFLAGS      ?= -O3 -Wall

PKGCONFIG   ?= pkg-config

# GTK+ edition: GTK3 + libcurl.
GTK_CFLAGS   = $(shell $(PKGCONFIG) --cflags gtk+-3.0 libcurl)
GTK_LIBS     = $(shell $(PKGCONFIG) --libs   gtk+-3.0 libcurl)

# Framebuffer edition: libcurl + libpng (pkg-config) and libjpeg (no
#   pkg-config module on most systems) plus the math library.
FB_CFLAGS    = $(shell $(PKGCONFIG) --cflags libcurl libpng)
FB_LIBS      = $(shell $(PKGCONFIG) --libs   libcurl libpng) -ljpeg -lm

# --- build ------------------------------------------------- #

.PHONY: all
all: piframe piframe-fb

piframe: client/main.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS)

piframe-fb: client/main-fb.c
	$(CC) $(CFLAGS) $(FB_CFLAGS) -o $@ $< $(FB_LIBS)

# --- install shared assets (startup image + default config) - #

.PHONY: install-common
install-common:
	$(INSTALL) -d $(DESTDIR)$(SHAREDIR)
	$(INSTALL) -m 0644 client/startup.jpg $(DESTDIR)$(SHAREDIR)/startup.jpg
	$(INSTALL) -d $(DESTDIR)$(DEFAULTDIR)
	# Don't clobber an existing config the user may have edited.
	@if [ -f $(DESTDIR)$(DEFAULTDIR)/piframe ]; then \
		echo "Keeping existing $(DESTDIR)$(DEFAULTDIR)/piframe"; \
	else \
		$(INSTALL) -m 0644 client/piframe.default $(DESTDIR)$(DEFAULTDIR)/piframe; \
	fi

# --- install as a systemd service -------------------------- #
#
# The service unit is generated from the matching template, with @PREFIX@
#   substituted, and installed as the single unit name "piframe.service"
#   (so the two editions are mutually exclusive).  systemctl steps are
#   skipped when DESTDIR is set, so staged/package builds don't touch the
#   live system.

.PHONY: install
install: install-fb

.PHONY: install-fb
install-fb: piframe-fb install-common
ifeq ($(DESTDIR),)
	# Stop any running instance so we don't overwrite a binary that's in use.
	-systemctl stop piframe.service
endif
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 piframe-fb $(DESTDIR)$(BINDIR)/piframe-fb
	sed 's|@PREFIX@|$(PREFIX)|g' client/piframe-fb.service.in \
		| $(INSTALL) -m 0644 /dev/stdin $(DESTDIR)$(SYSTEMDDIR)/piframe.service
ifeq ($(DESTDIR),)
	systemctl daemon-reload
	systemctl enable piframe.service
	@echo
	@echo "Installed the framebuffer edition as a service."
	@echo "Set PIFRAME_URL in $(DEFAULTDIR)/piframe, then start it with:"
	@echo "    sudo systemctl start piframe"
endif

.PHONY: install-gtk
install-gtk: piframe install-common
ifeq ($(DESTDIR),)
	# Stop any running instance so we don't overwrite a binary that's in use.
	-systemctl stop piframe.service
endif
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 piframe $(DESTDIR)$(BINDIR)/piframe
	sed 's|@PREFIX@|$(PREFIX)|g' client/piframe.service.in \
		| $(INSTALL) -m 0644 /dev/stdin $(DESTDIR)$(SYSTEMDDIR)/piframe.service
ifeq ($(DESTDIR),)
	systemctl daemon-reload
	systemctl enable piframe.service
	@echo
	@echo "Installed the GTK+ edition as a service (requires desktop autologin)."
	@echo "Set PIFRAME_URL in $(DEFAULTDIR)/piframe, then start it with:"
	@echo "    sudo systemctl start piframe"
endif

# --- uninstall --------------------------------------------- #

.PHONY: uninstall
uninstall:
ifeq ($(DESTDIR),)
	-systemctl stop piframe.service
	-systemctl disable piframe.service
endif
	rm -f $(DESTDIR)$(SYSTEMDDIR)/piframe.service
	rm -f $(DESTDIR)$(BINDIR)/piframe $(DESTDIR)$(BINDIR)/piframe-fb
ifeq ($(DESTDIR),)
	-systemctl daemon-reload
endif
	@echo "Left $(DEFAULTDIR)/piframe and $(SHAREDIR) in place; remove them manually if desired."

# --- clean ------------------------------------------------- #

.PHONY: clean
clean:
	rm -f piframe piframe-fb
