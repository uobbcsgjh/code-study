CFLAGS  = -Wall -Wextra -fstack-protector-all -std=gnu18 -D_FORTIFY_SOURCE=2 -O2
TAGS = ctags

.PHONY: all clean debug lint
.DEFAULT: all

all: server client package.tar.gz
clean:
	rm -f $(wildcard *.o) server client TAGS tags

debug: CFLAGS += -g -pedantic
HOST := $(shell uname)
ifneq ($(HOST), OpenBSD)
debug: LDFLAGS += -fsanitize=address,undefined,thread,leak
endif
debug: clean | server client

tags: $(wildcard *.c) $(wildcard *.h)
	$(TAGS) $(^)

TAGS: $(wildcard *.c) $(wildcard *.h)
	$(TAGS) -e -f $@ $(^)

server: server.o sftp.o
client: client.o sftp.o

server.o: server.c server.h sftp.h sftp.o
client.o: client.c client.h sftp.h sftp.o
sftp.o: sftp.c sftp.h

package.tar.gz: $(wildcard *.c) $(wildcard *.h)
	@mkdir -p package
	rm -rf package/*
	cp -r logs server.c server.h sftp.c sftp.h client.c client.h GNUmakefile package/
	tar czf $@ package 

lint:
	rats server.c client.c sftp.c