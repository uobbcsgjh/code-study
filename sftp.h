#pragma once

#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Default port to use for the app. */
#define DEFAULT_PORT "49152"

/* Maximum number of bytes we can fetch at once. */
#define MAXDATASIZE BUFSIZ

/* A command */
typedef struct Command_t {
  enum { ERROR = -1, DONE = 0, LIST = 1, GET = 2, PUT = 3 } type;
  union {
    struct {
      char *path;
    } list;
    struct {
      char *path;
      char *into;
    } get;
    struct {
      char *path;
      char *from;
    } put;
  };
} Command;

extern const char *program_name;
extern bool debug;

void *get_in_addr(struct sockaddr *);

int log_info(char *, ...);
int log_warn(char *, ...);
int log_error(char *, ...);
int log_debug(char *, ...);

ssize_t send_all(int, char *, size_t);
ssize_t recv_all(int, char **);
int dzprintf(int, char *, ...);
