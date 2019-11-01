DEBUG = 0

CC = gcc
CPPFLAGS = -D_GNU_SOURCE \
           -DDEBUG=$(DEBUG) \
           -I/usr/include -Iinclude

CFLAGS = -Wall -Werror -g
LDFLAGS = -L/usr/lib -lev  # -Wl,-Bstatic -lev -Wl,-Bdynamic -lm
MKDIR_P = mkdir -p

IDIR = include
OBJDIR=obj
LDIR =lib
BINDIR=bin
SRCDIR=src

_DEPS = restworker.h container_of.h evx.h hash.h json.h list.h n_buf.h string1.h trace.h
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ = restdaemon.o restworker.o hash.o n_buf.o evx_listen.o json.o
OBJ = $(patsubst %,$(OBJDIR)/%,$(_OBJ))


.PHONY: directories clean

all: directories $(BINDIR)/restdaemon

directories: ${OBJDIR} ${BINDIR}

${OBJDIR}:
	${MKDIR_P} ${OBJDIR}

${BINDIR}:
	${MKDIR_P} ${BINDIR}

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(DEPS)
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $< $(CFLAGS)
	$(CC) -MM $(CFLAGS) $(CPPFLAGS) $<  > $(OBJDIR)/.$*.d

$(BINDIR)/restdaemon: $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)
