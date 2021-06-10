#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sftp.h"

int vlogger(char *, char *, va_list argp);

const char *program_name;

/* Get the socket address, whether IPv4 or IPv6. */
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET)
    return &(((struct sockaddr_in *)sa)->sin_addr);
  else
    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

/* Generic logging function.  Don't use this, use one of the log_ methods
   below. 
*/
int vlogger(char *level, char *message, va_list argp) {
  struct timespec now;
  struct tm *tmbuf;
  char timestr[16];
  int err;

  clock_gettime(CLOCK_REALTIME, &now);
  tmbuf = gmtime(&now.tv_sec);
  if (tmbuf == NULL)
    strncpy(timestr, "?????????", 9);
  else
    strftime(timestr, sizeof timestr, "%H:%M:%S", tmbuf);

  err = 0;
  err += fprintf(stderr, "%s %s %s: ", program_name, level, timestr);
  err += vfprintf(stderr, message, argp);
  err += fprintf(stderr, "\n");

  return err;
}

int log_info(char *message, ...) {
  va_list argp;
  int err;

  va_start(argp, message);

  err = vlogger("[INFO]   ", message, argp);
  va_end(argp);

  return err;
}

int log_warn(char *message, ...) {
  va_list argp;
  int err;

  va_start(argp, message);

  err = vlogger("[WARNING]", message, argp);
  va_end(argp);

  return err;
}

int log_error(char *message, ...) {
  va_list argp;

  va_start(argp, message);

  vlogger("[ERROR]  ", message, argp);
  va_end(argp);

  exit(EXIT_FAILURE);
}

bool debug = false;
int log_debug(char *message, ...) {
  va_list argp;
  int err = 0;

  va_start(argp, message);

  if (debug)
    err = vlogger("[DEBUG]  ", message, argp);

  va_end(argp);

  return err;
}

/* Send in chunks an entire string.  Essentially handle the case that send
   returns less than the amount you asked it to send.
 */
ssize_t send_all(int fd, char *buf, size_t len) {
  ssize_t sent = 0;
  ssize_t n = 0;

  while ((size_t)sent < len) {
    n = send(fd, buf + sent, len - (size_t)sent, 0);
    if (n == -1)
      break;
    sent += n;
  }

  /* Return a negative number if send failed, else return bytes sent. */
  return n == -1 ? -sent : sent;
}

/* Read byte by byte from fd until a nil is encountered, allocating
   memory for buf as required.  If buf in not null, the memory is freed.
 */
ssize_t recv_all(int fd, char **buf) {
  ssize_t buflen = MAXDATASIZE;
  ssize_t old_buflen;
  ssize_t fp;
  ssize_t len;

  if (*buf != NULL)
    free(*buf);

  if ((*buf = calloc((size_t)buflen, sizeof(char))) == 0)
    log_error("couldn't allocate memory: %s", strerror(errno));
  memset(*buf, 0, (size_t)buflen);

  fp = 0;
  while (1) {
    len = recv(fd, (*buf) + fp, 1, 0);
    if (len < 0)
      log_error("recv: %s", strerror(errno));
    else if (len == 0)
      log_error("connection ended abruptly");

    if ((*buf)[fp] == '\0')
      goto done;

    fp += 1;
    if (fp >= buflen) {
      log_info("receiving long message: over %d bytes", fp);

      old_buflen = buflen;
      buflen *= 2;
      if ((*buf = (char *)realloc(*buf, ((size_t)buflen) * sizeof(char))) == 0)
        log_error("couldn't reallocate memory: %s", strerror(errno));

      memset((*buf) + fp, 0, (size_t)(buflen - old_buflen));
    }
  }

done:
  (*buf)[fp] = '\0';
  log_debug("received %lldB (%lldB)", strlen(*buf), fp);
  return fp;
  /* Unreachable code */
}

/* Printf for a filedescriptor.

   Why not use dprintf() that is already a part of stdio.h?  Because that
   doesn't include the trailing string \0, which we're using to delimit
   commands.
*/
int dzprintf(int fd, char *format, ...) {
  va_list argp;
  int result = -1;
  int err = 0;
  char *buf = NULL;
  va_start(argp, format);

  err = vasprintf(&buf, format, argp);
  if (err >= 0 && buf != NULL) {
    result = send_all(fd, buf, strlen(buf)+1);
    free(buf);
  }
  
  va_end(argp);
  return result;
}
