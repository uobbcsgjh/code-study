#pragma once

#include "sftp.h"
#include <netdb.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct Options_t {
  char *port;
  char *hostname;
} Options;

static Options options;

void default_options(Options *);
int get_connect_socket(struct addrinfo *, char[INET6_ADDRSTRLEN]);
bool socket_up(int);
int client(int);

bool do_command(int, Command *);
bool do_done(int, Command *);
bool do_list(int, Command *);
bool do_get(int, Command *);
bool do_put(int, Command *);

bool y_or_n_p(char *);
char *strip(char *);

bool parse_command(char *, Command *);
bool parse_done(char *, Command *);
bool parse_list(char *, Command *);
bool parse_get(char *, Command *);
bool parse_put(char *, Command *);
