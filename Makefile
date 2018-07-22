DISTNAME = sp-fetchmail
DISTDIR =  dist
VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifndef VERSION
	VERSION = $(shell head -n 1 VERSION)
endif

RM = rm -vf
RM_R = rm -vrf
MKDIR = mkdir -p
TAR = $(shell which tar)

DESTDIR =
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

CFLAGS  ?= -Os -g -W -Wall 
BUILDDIR = .

INSTALL ?= install
LIBS = -lspcommon-util -lspcommon-mime -lspcommon-mailstorage -lspcommon-mailid -lspcommon-web -lspuma-common-license -letpan -lresolv

OBJ = $(BUILDDIR)/helper_functions.o
OBJ += $(BUILDDIR)/syncmode.o
OBJ += $(BUILDDIR)/main.o
OBJ += $(BUILDDIR)/configfile.o
OBJ += $(BUILDDIR)/exchange_helpers.o
OBJ += $(BUILDDIR)/parse_helper.o

BIN = $(BUILDDIR)/sp_fetchmail

CFLAGS += -DVERSION=\"$(VERSION)\" -D_GNU_SOURCE -I ../common

all: $(BUILDDIR) $(BIN)

debug: CFLAGS += $(DFLAGS)
debug: all

$(BUILDDIR):
	$(VERBOSE)mkdir -p $(BUILDDIR)

$(BIN): $(OBJ)
	 $(CC) $(CFLAGS) -o $(BIN) $(OBJ) $(LIBS) $(LDFLAGS)

install: all
	$(INSTALL) -d -m 755 $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(BIN) $(DESTDIR)$(BINDIR)

uninstall:
	$(VERBOSE)rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	$(VERBOSE)rm -f $(OBJ) $(BIN)
	$(VERBOSE)rm -rf $(DISTDIR)

dist: clean
	$(MKDIR) $(DISTDIR)
	$(TAR)	--exclude-vcs \
		--exclude=dist \
		--exclude=*.swp \
		--exclude=*~ \
		--owner=root \
		--group=root \
		--transform="s,^\.,$(DISTNAME)-$(VERSION)," \
		--show-transformed-names \
		-cjf $(DISTDIR)/$(DISTNAME)-$(VERSION).tar.bz2 .

distclean:
	$(VERBOSE)rm -rf $(BUILDDIR)

INDENT = indent -nbad -nbap -nbbb -nbc -bl -c33 -ncdb -ce -cli1 -d0 -di0 -ndj -nfc1 -i2 -l78 -nlp -npcs -npsl -sc -sob -nut -saf -sai -saw -br
INDENTSRC   = $(wildcard *.[ch])
indent: $(INDENTSRC)
	for each in $(INDENTSRC); do $(INDENT) $$each; rm *~; done

$(BUILDDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

