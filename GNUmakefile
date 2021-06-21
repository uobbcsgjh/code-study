CFLAGS = -Wall -Wextra -fstack-protector-all -D_FORTIFY_SOURCE=2 -O2

.PHONY: all clean debug lint
.DEFAULT: all

all: server client
clean:
	rm -f $(wildcard *.o) server client TAGS tags

server: server.o sftp.o
client: client.o sftp.o

server.o: server.c server.h sftp.h sftp.o
client.o: client.c client.h sftp.h sftp.o
sftp.o: sftp.c sftp.h

lint:
	rats server.c client.c sftp.c
