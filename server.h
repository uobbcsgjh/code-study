#pragma once
#include "sftp.h"
#include <netdb.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

/* How big the pending connections queue is. */
#define DEFAULT_BACKLOG 10

typedef struct Options_t {
  char *port;
  int backlog;
  char connection[INET6_ADDRSTRLEN];
} Options;

static Options options;

void sigchld_handler(int);
void default_options(Options *);
int get_bind_socket(struct addrinfo *);
void listen_on(int);
void setup_process_reaping(void);
int accept_connection(int, char[INET6_ADDRSTRLEN]);
int server(int);

bool parse_done(Command *, char *);
bool parse_list(Command *, char *);
bool parse_get(Command *, char *);
bool parse_put(Command *, char *);
bool parse_command(Command *, char *);

bool do_command(int, Command *);
bool do_done(int, Command *);
bool do_list(int, Command *);
bool do_get(int, Command *);
bool do_put(int, Command *);
