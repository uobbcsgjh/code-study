#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "client.h"
#include "sftp.h"

/* Populate the options with default values. */
void default_options(Options *opts) {
  opts->port = DEFAULT_PORT;
  opts->hostname = NULL;
}

/* Get and connect to the first available socket, based on the hints given. */
int get_connect_socket(struct addrinfo *hints,
                       char their_addr[INET6_ADDRSTRLEN]) {
  struct addrinfo *servinfo, *p;
  int err;
  int sock_fd = -1;

  if ((err = getaddrinfo(options.hostname, options.port, hints, &servinfo)) !=
      0) {
    log_error("getaddrinfo: %s\n", gai_strerror(err));
  }

  /* Loop through all the results and bind to the first we can. */
  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) ==
        -1) {
      log_warn("socket: %s", strerror(errno));
      continue;
    }

    if (connect(sock_fd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sock_fd);
      log_warn("connect: %s", strerror(errno));
      continue;
    }

    break;
  }

  if (p == NULL)
    log_error("failed to connect");
  inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
            their_addr, INET6_ADDRSTRLEN);

  freeaddrinfo(servinfo);

  return sock_fd;
}

/* Handle arguments passed and set up the connections before starting the client
   proper.
 */
int main(int argc, char *argv[]) {
  int sock_fd;
  struct addrinfo hints;
  char s[INET6_ADDRSTRLEN];
  int err;

  program_name = argv[0];
  default_options(&options);

  if (argc != 2) {
    fprintf(stderr, "usage: %s hostname\n", program_name);
    exit(EXIT_FAILURE);
  }
  options.hostname = argv[1];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  sock_fd = get_connect_socket(&hints, s);

  log_info("connecting to %s", s);

  err = client(sock_fd);

  close(sock_fd);
  return err;
}

/* Test if a socket is still receiving okay */
bool socket_up(int socket) {
  int err = 0;
  socklen_t len = sizeof(err);
  int ret = getsockopt(socket, SOL_SOCKET, SO_ERROR, &err, &len);
  if (ret != 0 || err != 0)
    return false;
  else
    return true;
}

/* Main client application communicating over a file descriptor fd.  Handle and
   parse commands and run appropriate actions.
 */
int client(int fd) {
  Command c;
  ssize_t err;
  char *input = NULL;
  size_t len = 0;

  while (true) {
    printf("$ ");
    err = getline(&input, &len, stdin);
    if (err <= 0) break;
    
    input = strip(input);

    if (!strcmp(input, ""))
      continue;
    if (!parse_command(input, &c)) {
      log_warn("couldn't parse input");
      continue;
    }

    if (! socket_up(fd)) {
      log_info("Connection closed");
      break;
    }

    if (!do_command(fd, &c))
      break;

    if (! socket_up(fd)) {
      log_info("Connection closed");
      break;
    }
  }
  if (err < 0)
    log_error("client input failed: %s", strerror(errno));

  if (input != NULL)
    free(input);

  return EXIT_SUCCESS;
}

/* Handle the command passed by calling the appropriate do_ method. */
bool do_command(int fd, Command *c) {
  switch (c->type) {
  case DONE:
    return do_done(fd, c);
  case LIST:
    return do_list(fd, c);
  case GET:
    return do_get(fd, c);
  case PUT:
    return do_put(fd, c);
  case ERROR:
    log_warn("unrecognised command: %d", c->type);
  }

  return true;
}

bool do_done(int fd, __attribute__((unused)) Command *c) {
  dzprintf(fd, "DONE");
  return false;
}

bool do_list(int fd, Command *c) {
  char *buffer = NULL;
  ssize_t len;
  unsigned int lines;

  dzprintf(fd, "LIST %s", c->list.path);

  len = recv_all(fd, &buffer);
  if (!strncmp(buffer, "ERROR", 5)) {
    log_warn("%s", buffer + 6);
    goto done;
  }

  sscanf(buffer, "%u", &lines);
  while (lines--) {
    len = recv_all(fd, &buffer);
    if (len < 0)
      log_error("could not receive response: %s", strerror(errno));
    buffer[len] = '\0';

    printf("%s\n", buffer);
  }

done:
  return true;
}

bool do_get(int fd, Command *c) {
  char *msg = NULL;
  char *dest = NULL;
  int dest_fd;
  int err;
  bool ok;
  ssize_t len;
  ssize_t received;
  ssize_t written;
  ssize_t n;
  ssize_t want;
  ssize_t remaining;
  char buf[MAXDATASIZE];

  log_debug("get %s %s", c->get.path, c->get.into);

  dzprintf(fd, "GET %s", c->get.path);

  recv_all(fd, &msg);
  sscanf(msg, "%zd", &len);
  free(msg);
  msg = NULL;

  err = asprintf(&msg, "Okay to receive %luB?", len);
  if (err == -1) log_error("memory allocation failed: asprintf");
  ok = y_or_n_p(msg);
  free(msg);
  msg = NULL;

  if (!ok) {
    dzprintf(fd, "NO");
    goto done;
  }

  dest = c->get.into;
  dest_fd = open(dest, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (dest_fd == -1) {
    log_warn("couldn't open file for getting: %s", strerror(errno));
    dzprintf(fd, "NO");
    goto done;
  }

  log_info("starting the transfer of %uB to %s", len, dest);
  dzprintf(fd, "OK");

  received = 0;
  while (received < len) {
    remaining = len - received;
    want = (MAXDATASIZE < remaining) ? MAXDATASIZE : remaining;
    n = recv(fd, &buf, (size_t)want, MSG_WAITALL);

    if (n < 0)
      log_error("error receiving file: %s", strerror(errno));
    if (n == 0)
      log_error("connection with server ended abruptly");

    received += n;

    written = write(dest_fd, buf, (size_t)want);
    if (written < 0)
      log_error("write failed: %s", strerror(errno));

    if (written != want)
      log_warn("tried to write %uB but only wrote %uB", want, written);
  }
  log_info("transfer completed");

done:
  return true;
}

bool do_put(int fd, Command *c) {
  log_debug("put %s %s", c->put.from, c->put.path);

  char *msg = NULL;
  char buf[MAXDATASIZE];
  int err;
  int to_send = -1;
  off_t len;
  ssize_t n, sent, chunk_sent, chunk_read;
  struct stat fs;

  err = stat(c->put.from, &fs);
  if (err != 0) {
    log_warn("cannot put %s: %s", c->put.from, strerror(errno));
    goto done;
  }
  log_debug("got file size: %lld", fs.st_size);

  to_send = open(c->put.from, O_RDONLY);
  if (to_send < 0) {
    log_warn("cannot open file for sending %s: %s", c->put.from,
             strerror(errno));
    goto done;
  }
  log_debug("opened file for sending");

  dzprintf(fd, "PUT %s", c->put.path);

  recv_all(fd, &msg);
  if (strcmp(msg, "OK") != 0) {
    log_warn("put refused: %s", msg);
    goto done;
  }
  free(msg);
  msg = NULL;
  log_debug("server accepted send in principle");

  len = fs.st_size;
  dzprintf(fd, "%lld", len);
  log_debug("sent file length: %lld", len);

  recv_all(fd, &msg);
  if (strcmp(msg, "OK") != 0) {
    log_warn("put refused: %s", msg);
    goto done;
  }
  free(msg);
  msg = NULL;

  log_info("sending %uB", len);

  sent = 0;
  while (sent < len) {
    chunk_read = read(to_send, &buf, MAXDATASIZE);
    chunk_sent = 0;
    log_debug("read %lldB", chunk_read);
    while (chunk_sent < chunk_read) {
      n = send(fd, buf + chunk_sent, (size_t)(chunk_read - chunk_sent), 0);
      log_debug("sent %lld", n);
      sent += n;
      chunk_sent += n;
    }
    log_debug("sent %u/%uB", sent, len);
  }
  log_info("transfer completed");

done:
  if (msg != NULL)
    free(msg);
  if (to_send > 0)
    close(to_send);
  return true;
}

/* Prompt for a y/n response. */
bool y_or_n_p(char *prompt) {
  char *response = NULL;
  size_t len = 0;
  ssize_t err;
  bool result;
  printf("%s? (y/N): ", prompt);

  err = getline(&response, &len, stdin);
  if (err <= 0)
    return false;

  result = strncasecmp(response, "y", 1) == 0;
  free(response);
  return result;
}

/* Remove leading and trailing space from a string. */
char *strip(char *in) {
  char *start, *end;
  ssize_t i = 0;

  if (in == NULL)
    return NULL;
  if (*in == '\0')
    return in;

  for (end = strchr(in, '\0') - 1; isspace(*end) && in < end; end--)
    ;
  for (start = in; isspace(*start) && start < end; start++)
    ;
  if (start != end)
    for (; i <= end - start; i += 1)
      in[i] = start[i];
  in[i] = '\0';

  return in;
}

/* What are we being asked to do?  Get the first few characters and then
   dispatch to the appropriate command parser. 
*/
bool parse_command(char *input, Command *c) {
  input = strip(input);

  if (!strncasecmp(input, "done", 4))
    return parse_done(input + 4, c);
  if (!strncasecmp(input, "list", 4))
    return parse_list(input + 4, c);
  if (!strncasecmp(input, "get ", 4))
    return parse_get(input + 4, c);
  if (!strncasecmp(input, "put ", 4))
    return parse_put(input + 4, c);

  return false;
}

bool parse_done(char *input, Command *c) {
  input = strip(input);
  if (strcmp(input, "") == 0) {
    c->type = DONE;
    return true;
  } else {
    log_warn("trailing junk in DONE: '%s'", input);
    c->type = ERROR;
    return false;
  }
}

bool parse_list(char *input, Command *c) {
  input = strip(input);

  c->type = LIST;
  /* If path missing, assume ".". */
  if (strcmp(input, "") == 0)
    c->list.path = ".";
  else
    c->list.path = input;
  return true;
}

bool parse_get(char *input, Command *c) {
  size_t len, i, fp;
  input = strip(input);
  len = strlen(input);
  bool escaping = false;

  c->type = GET;

  /* Cannot get an empty path */
  if (len == 0) {
    log_warn("cannot GET empty path");
    c->type = ERROR;
    return false;
  }

  for (i = 0, fp = 0; i < len; i++) {
    if (escaping) {
      switch (input[i]) {
      case ' ':
        input[fp++] = ' ';
        break;
      case 't':
        input[fp++] = '\t';
        break;
      case 'n':
        input[fp++] = '\n';
        break;
      case 'r':
        input[fp++] = '\r';
        break;
      case '\\':
        input[fp++] = '\\';
        break;
      default:
        log_warn("unrecognised escape: \\%c", input[i]);
        return false;
      }
    } else {
      escaping = false;
      if (input[i] == '\0' || isspace(input[i]))
        break;
      else if (input[i] == '\\')
        escaping = true;
      else
        input[fp++] = input[i];
    }
  }
  c->get.path = input;

  /* If only one path provided, save into the basename */
  if (input[i] == '\0') {
    c->get.into = basename(input);
  } else {
    input[fp] = '\0';
    input = strip(input + i + 1);

    for (i = 0, fp = 0; i < len; i++) {
      if (escaping) {
        switch (input[i]) {
        case ' ':
          input[fp++] = ' ';
          break;
        case 't':
          input[fp++] = '\t';
          break;
        case 'n':
          input[fp++] = '\n';
          break;
        case 'r':
          input[fp++] = '\r';
          break;
        case '\\':
          input[fp++] = '\\';
          break;
        default:
          log_warn("unrecognised escape: \\%c", input[i]);
          return false;
        }
        escaping = false;
      } else {
        if (input[i] == '\0')
          break;
        else if (input[i] == '\\')
          escaping = true;
        else {
          escaping = false;
          input[fp++] = input[i];
        }
      }
    }
    input[fp] = '\0';
    c->get.into = input;
  }

  return true;
}

bool parse_put(char *input, Command *c) {
  size_t len, i, fp;
  input = strip(input);
  len = strlen(input);
  bool escaping = false;

  c->type = PUT;

  /* Cannot PUT an empty path */
  if (len == 0) {
    log_warn("cannot PUT empty path");
    c->type = ERROR;
    return false;
  }

  for (i = 0, fp = 0; i < len; i++) {
    if (escaping) {
      switch (input[i]) {
      case ' ':
        input[fp++] = ' ';
        break;
      case 't':
        input[fp++] = '\t';
        break;
      case 'n':
        input[fp++] = '\n';
        break;
      case 'r':
        input[fp++] = '\r';
        break;
      case '\\':
        input[fp++] = '\\';
        break;
      default:
        log_warn("unrecognised escape: \\%c", input[i]);
        return false;
      }
    } else {
      escaping = false;
      if (input[i] == '\0' || isspace(input[i]))
        break;
      else if (input[i] == '\\')
        escaping = true;
      else
        input[fp++] = input[i];
    }
  }
  c->put.from = input;

  /* If only one path provided, save into the basename */
  if (input[i] == '\0') {
    c->put.path = basename(input);
  } else {
    input[fp] = '\0';
    input = strip(input + i + 1);

    for (i = 0, fp = 0; i < len; i++) {
      if (escaping) {
        switch (input[i]) {
        case ' ':
          input[fp++] = ' ';
          break;
        case 't':
          input[fp++] = '\t';
          break;
        case 'n':
          input[fp++] = '\n';
          break;
        case 'r':
          input[fp++] = '\r';
          break;
        case '\\':
          input[fp++] = '\\';
          break;
        default:
          log_warn("unrecognised escape: \\%c", input[i]);
          return false;
        }
        escaping = false;
      } else {
        if (input[i] == '\0')
          break;
        else if (input[i] == '\\')
          escaping = true;
        else {
          escaping = false;
          input[fp++] = input[i];
        }
      }
    }
    input[fp] = '\0';
    c->put.path = input;
  }

  return true;
}
