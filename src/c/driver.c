#include <stdio.h>

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#include "urweb.h"

int uw_port = 8080;
int uw_backlog = 10;
int uw_bufsize = 1024;

typedef struct node {
  int fd;
  struct node *next;
} *node;

static node front = NULL, back = NULL;

static int empty() {
  return front == NULL;
}

static void enqueue(int fd) {
  node n = malloc(sizeof(struct node));

  n->fd = fd;
  n->next = NULL;
  if (back)
    back->next = n;
  else
    front = n;
  back = n;
}

static int dequeue() {
  int ret = front->fd;

  front = front->next;
  if (!front)
    back = NULL;

  return ret;
}

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

#define MAX_RETRIES 5

static void *worker(void *data) {
  int me = *(int *)data, retries_left = MAX_RETRIES;;
  uw_context ctx = uw_init(1024, 1024);
  
  while (1) {
    failure_kind fk = uw_begin_init(ctx);

    if (fk == SUCCESS) {
      uw_db_init(ctx);
      printf("Database connection initialized.\n");
      break;
    } else if (fk == BOUNDED_RETRY) {
      if (retries_left) {
        printf("Initialization error triggers bounded retry: %s\n", uw_error_message(ctx));
        --retries_left;
      } else {
        printf("Fatal initialization error (out of retries): %s\n", uw_error_message(ctx));
        uw_free(ctx);
        return NULL;
      }
    } else if (fk == UNLIMITED_RETRY)
      printf("Initialization error triggers unlimited retry: %s\n", uw_error_message(ctx));
    else if (fk == FATAL) {
      printf("Fatal initialization error: %s\n", uw_error_message(ctx));
      uw_free(ctx);
      return NULL;
    } else {
      printf("Unknown uw_handle return code!\n");
      uw_free(ctx);
      return NULL;
    }
  }

  while (1) {
    char buf[uw_bufsize+1], *back = buf, *s;
    int sock;

    pthread_mutex_lock(&queue_mutex);
    while (empty())
      pthread_cond_wait(&queue_cond, &queue_mutex);
    sock = dequeue();
    pthread_mutex_unlock(&queue_mutex);

    printf("Handling connection with thread #%d.\n", me);

    while (1) {
      unsigned retries_left = MAX_RETRIES;
      int r = recv(sock, back, uw_bufsize - (back - buf), 0);

      if (r < 0) {
        fprintf(stderr, "Recv failed\n");
        break;
      }

      if (r == 0) {
        printf("Connection closed.\n");
        break;
      }

      printf("Received %d bytes.\n", r);

      back += r;
      *back = 0;
    
      if (s = strstr(buf, "\r\n\r\n")) {
        char *cmd, *path, *inputs;

        *s = 0;

        printf("Read: %s\n", buf);
      
        if (!(s = strstr(buf, "\r\n"))) {
          fprintf(stderr, "No newline in buf\n");
          break;
        }

        *s = 0;
        cmd = s = buf;
      
        if (!strsep(&s, " ")) {
          fprintf(stderr, "No first space in HTTP command\n");
          break;
        }

        if (strcmp(cmd, "GET")) {
          fprintf(stderr, "Not ready for non-get command: %s\n", cmd);
          break;
        }

        path = s;
        if (!strsep(&s, " ")) {
          fprintf(stderr, "No second space in HTTP command\n");
          break;
        }

        if (inputs = strchr(path, '?')) {
          char *name, *value;
          *inputs++ = 0;

          while (*inputs) {
            name = inputs;
            if (inputs = strchr(inputs, '&'))
              *inputs++ = 0;
            else
              inputs = strchr(name, 0);

            if (value = strchr(name, '=')) {
              *value++ = 0;
              uw_set_input(ctx, name, value);
            }
            else
              uw_set_input(ctx, name, "");
          }
        }

        printf("Serving URI %s....\n", path);

        while (1) {
          failure_kind fk;

          uw_write(ctx, "HTTP/1.1 200 OK\r\n");
          uw_write(ctx, "Content-type: text/html\r\n\r\n");
          uw_write(ctx, "<html>");

          fk = uw_begin(ctx, path);
          if (fk == SUCCESS) {
            uw_write(ctx, "</html>");
            break;
          } else if (fk == BOUNDED_RETRY) {
            if (retries_left) {
              printf("Error triggers bounded retry: %s\n", uw_error_message(ctx));
              --retries_left;
            }
            else {
              printf("Fatal error (out of retries): %s\n", uw_error_message(ctx));

              uw_reset_keep_error_message(ctx);
              uw_write(ctx, "HTTP/1.1 500 Internal Server Error\n\r");
              uw_write(ctx, "Content-type: text/plain\r\n\r\n");
              uw_write(ctx, "Fatal error (out of retries): ");
              uw_write(ctx, uw_error_message(ctx));
              uw_write(ctx, "\n");
            }
          } else if (fk == UNLIMITED_RETRY)
            printf("Error triggers unlimited retry: %s\n", uw_error_message(ctx));
          else if (fk == FATAL) {
            printf("Fatal error: %s\n", uw_error_message(ctx));

            uw_reset_keep_error_message(ctx);
            uw_write(ctx, "HTTP/1.1 500 Internal Server Error\n\r");
            uw_write(ctx, "Content-type: text/plain\r\n\r\n");
            uw_write(ctx, "Fatal error: ");
            uw_write(ctx, uw_error_message(ctx));
            uw_write(ctx, "\n");

            break;
          } else {
            printf("Unknown uw_handle return code!\n");

            uw_reset_keep_request(ctx);
            uw_write(ctx, "HTTP/1.1 500 Internal Server Error\n\r");
            uw_write(ctx, "Content-type: text/plain\r\n\r\n");
            uw_write(ctx, "Unknown uw_handle return code!\n");

            break;
          }

          uw_reset_keep_request(ctx);
        }

        uw_send(ctx, sock);

        printf("Done with client.\n\n");
        break;
      }
    }

    close(sock);
    uw_reset(ctx);
  }
}

int main(int argc, char *argv[]) {
  // The skeleton for this function comes from Beej's sockets tutorial.
  int sockfd;  // listen on sock_fd
  struct sockaddr_in my_addr;
  struct sockaddr_in their_addr; // connector's address information
  int sin_size, yes = 1;
  int nthreads, i, *names;

  if (argc < 2) {
    fprintf(stderr, "No thread count specified\n");
    return 1;
  }

  nthreads = atoi(argv[1]);
  if (nthreads <= 0) {
    fprintf(stderr, "Invalid thread count\n");
    return 1;
  }
  names = calloc(nthreads, sizeof(int));

  sockfd = socket(PF_INET, SOCK_STREAM, 0); // do some error checking!

  if (sockfd < 0) {
    fprintf(stderr, "Listener socket creation failed\n");
    return 1;
  }

  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0) {
    fprintf(stderr, "Listener socket option setting failed\n");
    return 1;
  }

  my_addr.sin_family = AF_INET;         // host byte order
  my_addr.sin_port = htons(uw_port);    // short, network byte order
  my_addr.sin_addr.s_addr = INADDR_ANY; // auto-fill with my IP
  memset(my_addr.sin_zero, '\0', sizeof my_addr.sin_zero);

  if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof my_addr) < 0) {
    fprintf(stderr, "Listener socket bind failed\n");
    return 1;
  }

  if (listen(sockfd, uw_backlog) < 0) {
    fprintf(stderr, "Socket listen failed\n");
    return 1;
  }

  sin_size = sizeof their_addr;

  printf("Listening on port %d....\n", uw_port);

  for (i = 0; i < nthreads; ++i) {
    pthread_t thread;    
    names[i] = i;
    if (pthread_create(&thread, NULL, worker, &names[i])) {
      fprintf(stderr, "Error creating worker thread #%d\n", i);
      return 1;
    }
  }

  while (1) {
    int new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);

    if (new_fd < 0) {
      fprintf(stderr, "Socket accept failed\n");
      return 1;
    }

    printf("Accepted connection.\n");

    pthread_mutex_lock(&queue_mutex);
    enqueue(new_fd);
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
  }
}
