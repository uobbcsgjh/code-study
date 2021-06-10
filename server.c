#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "server.h"
#include "sftp.h"

/* Handler used to ensure ended sessions die smoothly. */
void sigchld_handler(__attribute__((unused)) int s) {
  int saved_errno = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
  errno = saved_errno;
}

/* Populate the options with default values. */
void default_options(Options *opts) {
  if (opts != NULL) {
    opts->port = DEFAULT_PORT;
    opts->backlog = DEFAULT_BACKLOG;
  }
}

/* Get and bind to the first available socket, based on the hints given. */
int get_bind_socket(struct addrinfo *hints) {
  int sock_fd = -1;
  struct addrinfo *servinfo, *p;
  int yes = 1;
  int err;

  if ((err = getaddrinfo(NULL, options.port, hints, &servinfo)) != 0) {
    log_error("getaddrinfo: %s\n", gai_strerror(err));
  }

  /* Loop through all the results and bind to the first we can. */
  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) ==
        -1) {
      log_warn("socket: %s", strerror(errno));
      continue;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) ==
        -1) {
      log_error("setsockopt: %s", strerror(errno));
    }

    if (bind(sock_fd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sock_fd);
      log_warn("bind: %s", strerror(errno));
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo);

  return sock_fd;
}

/* Try and listen on a file descriptor */
void listen_on(int sock_fd) {
  if (listen(sock_fd, options.backlog) == -1) {
    log_error("listen: %s", strerror(errno));
  }
}

/* Reap all dead processes. */
void setup_process_reaping() {
  struct sigaction sa;
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    log_error("sigaction: %s", strerror(errno));
  }
}

/* Accept a connection and set up a socket for that session to communicate over */
int accept_connection(int sock_fd, char from[INET6_ADDRSTRLEN]) {
  socklen_t sin_size;
  struct sockaddr_storage their_addr;
  int new_fd;

  sin_size = sizeof their_addr;
  new_fd = accept(sock_fd, (struct sockaddr *)&their_addr, &sin_size);
  if (new_fd == -1) {
    log_warn("accept: %s", strerror(errno));
    return -1;
  }

  inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr),
            from, INET6_ADDRSTRLEN);

  return new_fd;
}

/* Set up the server and as connections come in bind them to their own session
   of the file transfer server.
 */
int main(__attribute__((unused)) int argc, char *argv[]) {
  int sock_fd, new_fd;
  struct addrinfo hints;

  program_name = argv[0];
  default_options(&options);

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; /* Use my IP. */

  sock_fd = get_bind_socket(&hints);
  listen_on(sock_fd);
  setup_process_reaping();

  log_info("waiting for connections");

  /* Listen for connections, and set up server instances. */
  for (;;) {
    if ((new_fd = accept_connection(sock_fd, options.connection)) < 0)
      continue;

    log_info("got connection from %s", options.connection);

    if (!fork()) {
      /* Child process: we don't need the listener. */
      close(sock_fd);
      return server(new_fd);
    } else {
      /* Parent process: we don't need the connection. */
      close(new_fd);
    }
  }

  return EXIT_SUCCESS;
}

/* An established server session with a client communicating over the file
 * descriptor fd. */
int server(int fd) {
  char *buffer = NULL;
  Command command;
  bool keep_alive = true;

  while (keep_alive) {
    recv_all(fd, &buffer);

    if (!parse_command(&command, buffer))
      log_error("%s: unparseable command: %s", options.connection, buffer);

    keep_alive = do_command(fd, &command);
  }

  close(fd);
  free(buffer);
  buffer = NULL;
  log_info("%s: session ended", options.connection);
  return EXIT_SUCCESS;
}

bool parse_done(Command *command, char *buffer) {
  if (strncmp("DONE", buffer, 5) == 0) {
    command->type = DONE;
    return true;
  }

  return false;
}

bool parse_list(Command *command, char *buffer) {
  if (strncmp("LIST ", buffer, 5) == 0) {
    command->type = LIST;
    command->list.path = buffer + 5;
    return true;
  }

  return false;
}

bool parse_get(Command *command, char *buffer) {
  if (strncmp("GET ", buffer, 4) == 0) {
    command->type = GET;
    command->list.path = buffer + 4;
    return true;
  }

  return false;
}

bool parse_put(Command *command, char *buffer) {
  if (strncmp("PUT ", buffer, 4) == 0) {
    command->type = PUT;
    command->list.path = buffer + 4;
    return true;
  }

  return false;
}

bool parse_command(Command *command, char *buffer) {
  if (parse_done(command, buffer))
    return true;
  if (parse_list(command, buffer))
    return true;
  if (parse_get(command, buffer))
    return true;
  if (parse_put(command, buffer))
    return true;

  command->type = ERROR;
  return false;
}

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

  return false;
}

bool do_done(__attribute__((unused)) int fd,
             __attribute__((unused)) Command *command) {
  log_info("%s: DONE", options.connection);
  return false;
}

bool do_list(int fd, Command *command) {
  int err;
  struct dirent **list;

  log_info("%s: LIST %s", options.connection, command->list.path);

  err = scandir(command->list.path, &list, NULL, alphasort);
  if (err < 0) {
    log_warn("cannot open directory for reading: %s", command->list.path);
    dzprintf(fd, "ERROR can't open directory: %s", strerror(errno));
  } else {
    log_debug("found %d entries", err);
    dzprintf(fd, "%d", err);

    for (int i = 0; i < err; i++) {
      log_debug("sending dirent %s", list[i]->d_name);
      send_all(fd, list[i]->d_name, strlen(list[i]->d_name) + 1);
      free(list[i]);
      list[i] = NULL;
    }
    free(list);
    list = NULL;
  }

  return true;
}

bool do_get(int fd, Command *command) {
  log_info("%s: GET %s", options.connection, command->get.path);

  struct stat fs;
  int err;
  int to_send = -1;
  char *msg = NULL;
  char buf[MAXDATASIZE];
  off_t len;
  ssize_t n, sent;
  ssize_t chunk_read;
  ssize_t chunk_sent;

  err = stat(command->get.path, &fs);
  if (err != 0)
    goto err;

  to_send = open(command->get.path, O_RDONLY);
  if (to_send < 0)
    goto err;

  len = fs.st_size;
  log_info("%s: checking if okay to receive %lluB", options.connection, len);
  dzprintf(fd, "%llu", len);

  recv_all(fd, &msg);
  if (strcmp(msg, "OK") != 0) {
    log_info("%s: cancelled GET: %s", options.connection, msg);
    goto done;
  }

  log_info("%s: sending %uB", options.connection, len);
  sent = 0;
  while (sent < len) {
    chunk_read = read(to_send, &buf, MAXDATASIZE);
    chunk_sent = 0;
    while (chunk_sent < chunk_read) {
      n = send(fd, buf + chunk_sent, chunk_read - (size_t)chunk_sent, 0);
      sent += n;
      chunk_sent += n;
    }
    log_debug("sent %u/%uB", sent, len);
  }

  goto done;

err:
  log_warn("%s: GET failed: %s", options.connection, strerror(errno));
  dzprintf(fd, "ERROR %s", strerror(errno));

done:
  if (to_send > 0)
    close(to_send);
  if (msg != NULL)
    free(msg);
  return true;
}

bool do_put(int fd, Command *command) {
  char *msg = NULL;
  int dest_fd = -1;
  ssize_t len;
  ssize_t received, remaining, want, n, written;
  char buf[MAXDATASIZE];

  log_info("%s: PUT %s", options.connection, command->put.path);

  dest_fd =
      open(command->put.path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (dest_fd == -1) {
    log_warn("%s: couldn't open file for PUT: %s", options.connection,
             strerror(errno));
    dzprintf(fd, "NO: %s", strerror(errno));
    goto done;
  }
  log_debug("%s: opened file for writing: %s", options.connection,
            command->put.path);

  dzprintf(fd, "OK");
  log_debug("%s: accepted put in principle", options.connection);

  recv_all(fd, &msg);
  sscanf(msg, "%zd", &len);
  free(msg);
  msg = NULL;
  log_info("%s: to receive %luB", options.connection, len);

  // TODO: handle rejecting files based on free space.
  dzprintf(fd, "OK");

  log_debug("%s: awaiting transfer", options.connection);
  received = 0;
  while (received < len) {
    remaining = len - received;
    want = (MAXDATASIZE < remaining) ? MAXDATASIZE : remaining;
    n = recv(fd, &buf, (size_t)want, MSG_WAITALL);

    if (n < 0)
      log_error("error receiving file: %s", strerror(errno));
    if (n == 0) {
      log_warn("connection from client ended abruptly");
      goto done;
    }
    received += n;
    log_debug("%s: read %luB", options.connection, n);

    written = write(dest_fd, buf, (size_t)want);
    log_debug("%s: written %luB", options.connection, written);
    if (written < 0)
      log_error("write failed: %s", strerror(errno));

    if (written != want)
      log_warn("tried to write %uB but only wrote %uB", want, written);
  }
  log_info("transfer completed");

done:
  if (dest_fd > 0)
    close(dest_fd);
  if (msg != NULL)
    free(msg);

  return true;
}
