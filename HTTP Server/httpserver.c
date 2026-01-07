#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFFER_SIZE 8192
#define MAX_HEADER_SIZE 2048
#define MAX_QUEUE_SIZE 1024
#define FILE_LOCK_BUCKETS 128

static volatile sig_atomic_t shutdown_requested = 0;

static void signal_handler(int sig) {
  (void)sig;
  shutdown_requested = 1;
}

typedef struct {
  int *fds;
  int front;
  int rear;
  int count;
  int size;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} Queue;

typedef struct {
  int listen_fd;
  Queue *queue;
  int num_threads;
  pthread_t *threads;
  int shutdown;
} ThreadPool;

typedef struct {
  char method[9];
  char uri[65];
  char version[16];
  int has_content_length;
  size_t content_length;
  uint64_t request_id;
} HttpRequest;

typedef struct {
  int code;
  const char *phrase;
  char *body;
  size_t body_len;
} HttpResponse;

static pthread_mutex_t audit_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int global_parallelism = 1;
static pthread_mutex_t file_locks[FILE_LOCK_BUCKETS];
static int file_locks_initialized = 0;
static pthread_mutex_t file_locks_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash_path(const char *path) {
  unsigned h = 5381;
  int c;
  while ((c = (unsigned char)*path++)) {
    h = ((h << 5) + h) + c;
  }
  return h % FILE_LOCK_BUCKETS;
}

static void init_file_locks(void) {
  pthread_mutex_lock(&file_locks_init_mutex);
  if (!file_locks_initialized) {
    for (int i = 0; i < FILE_LOCK_BUCKETS; i++) {
      pthread_mutex_init(&file_locks[i], NULL);
    }
    file_locks_initialized = 1;
  }
  pthread_mutex_unlock(&file_locks_init_mutex);
}

// Helper function for logging requests (used by GET/PUT after file operations)
static void log_request(const HttpRequest *req, int status_code) {
  pthread_mutex_lock(&audit_log_mutex);
  fprintf(stderr, "%s,%s,%d,%" PRIu64 "\n", req->method, req->uri, status_code,
          req->request_id);
  pthread_mutex_unlock(&audit_log_mutex);
}

Queue *queue_init(int size) {
  Queue *q = malloc(sizeof(Queue));
  if (!q)
    return NULL;
  q->fds = malloc(size * sizeof(int));
  if (!q->fds) {
    free(q);
    return NULL;
  }
  q->front = 0;
  q->rear = 0;
  q->count = 0;
  q->size = size;
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  return q;
}

void queue_destroy(Queue *q) {
  if (!q)
    return;
  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->fds);
  free(q);
}

void queue_enqueue(Queue *q, int fd) {
  pthread_mutex_lock(&q->mutex);
  while (q->count >= q->size) {
    pthread_cond_wait(&q->not_full, &q->mutex);
  }
  q->fds[q->rear] = fd;
  q->rear = (q->rear + 1) % q->size;
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);
}

int queue_dequeue(Queue *q) {
  pthread_mutex_lock(&q->mutex);
  while (q->count == 0) {
    pthread_cond_wait(&q->not_empty, &q->mutex);
  }
  int fd = q->fds[q->front];
  q->front = (q->front + 1) % q->size;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return fd;
}

ssize_t writen(int fd, const void *buf, size_t n) {
  size_t total = 0;
  const char *p = buf;
  while (total < n) {
    ssize_t w = write(fd, p + total, n - total);
    if (w <= 0)
      return w;
    total += w;
  }
  return total;
}

void send_response(int fd, HttpResponse *resp) {
  char header[512];
  int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n\r\n",
                            resp->code, resp->phrase, resp->body_len);

  writen(fd, header, header_len);
  if (resp->body && resp->body_len > 0) {
    writen(fd, resp->body, resp->body_len);
  }
}

void send_error(int fd, int code, const char *phrase, const char *body) {
  HttpResponse resp;
  resp.code = code;
  resp.phrase = phrase;
  resp.body = (char *)body;
  resp.body_len = strlen(body);
  send_response(fd, &resp);
}

int parse_request_line(const char *line, HttpRequest *req) {
  if (!line || !req)
    return -1;

  const char *sp1 = strchr(line, ' ');
  if (!sp1)
    return -1;

  size_t method_len = sp1 - line;
  if (method_len < 1 || method_len > 8)
    return -1;

  for (size_t i = 0; i < method_len; i++)
    if (!isalpha((unsigned char)line[i]))
      return -1;

  strncpy(req->method, line, method_len);
  req->method[method_len] = '\0';

  const char *uri_start = sp1 + 1;
  const char *sp2 = strchr(uri_start, ' ');
  if (!sp2)
    return -1;

  size_t uri_len = sp2 - uri_start;
  if (uri_len < 1 || uri_len > 63)
    return -1;

  if (*uri_start != '/')
    return -1;

  for (size_t i = 1; i < uri_len; i++) {
    char c = uri_start[i];
    if (!isalnum((unsigned char)c) && c != '.' && c != '-')
      return -1;
  }

  strncpy(req->uri, uri_start, uri_len);
  req->uri[uri_len] = '\0';

  const char *version_start = sp2 + 1;
  size_t version_len = strcspn(version_start, "\r\n");
  if (version_len != 8)
    return -1;

  if (strncmp(version_start, "HTTP/", 5) != 0)
    return -1;

  if (!isdigit((unsigned char)version_start[5]) || version_start[6] != '.' ||
      !isdigit((unsigned char)version_start[7]))
    return -1;

  strncpy(req->version, version_start, 8);
  req->version[8] = '\0';

  return 0;
}

int parse_headers(char *headers, HttpRequest *req) {
  req->has_content_length = 0;
  req->content_length = 0;
  req->request_id = 0;

  char *line = headers;

  while (*line) {
    char *end = strstr(line, "\r\n");
    if (!end)
      break;
    if (end == line)
      break;

    *end = '\0';

    char *colon = strchr(line, ':');
    if (!colon)
      return -1;

    *colon = '\0';
    char *key = line;
    char *value = colon + 1;

    if (*value != ' ')
      return -1;
    value++;
    while (*value == ' ')
      value++;

    int len = strlen(key);
    if (len < 1 || len > 128)
      return -1;

    for (int i = 0; key[i]; i++) {
      char c = key[i];
      if (!isalnum(c) && c != '.' && c != '-')
        return -1;
    }

    len = strlen(value);
    if (len < 1 || len > 128)
      return -1;

    for (int i = 0; value[i]; i++) {
      if (value[i] < 32 || value[i] > 126)
        return -1;
    }

    if (strcasecmp(key, "Content-Length") == 0) {
      for (int i = 0; value[i]; i++) {
        if (value[i] < '0' || value[i] > '9')
          return -1;
      }
      req->has_content_length = 1;
      req->content_length = strtoull(value, NULL, 10);
    } else if (strcasecmp(key, "Request-Id") == 0) {
      for (int i = 0; value[i]; i++) {
        if (value[i] < '0' || value[i] > '9')
          return -1;
      }
      req->request_id = strtoull(value, NULL, 10);
    }

    line = end + 2;
  }

  return 0;
}

int handle_get(int fd, HttpRequest *req) {
  const char *filename = req->uri + 1;
  pthread_mutex_t *file_lock = NULL;

  if (global_parallelism > 1) {
    init_file_locks();
    unsigned bucket = hash_path(filename);
    file_lock = &file_locks[bucket];
    pthread_mutex_lock(file_lock);
  }

  struct stat st;
  if (stat(filename, &st) < 0) {
    if (file_lock)
      pthread_mutex_unlock(file_lock);

    if (errno == ENOENT) {
      send_error(fd, 404, "Not Found", "Not Found\n");
      log_request(req, 404);
      return 404;
    } else {
      send_error(fd, 403, "Forbidden", "Forbidden\n");
      log_request(req, 403);
      return 403;
    }
  }

  if (S_ISDIR(st.st_mode)) {
    if (file_lock)
      pthread_mutex_unlock(file_lock);
    send_error(fd, 403, "Forbidden", "Forbidden\n");
    log_request(req, 403);
    return 403;
  }

  if (access(filename, R_OK) != 0) {
    if (file_lock)
      pthread_mutex_unlock(file_lock);
    send_error(fd, 403, "Forbidden", "Forbidden\n");
    log_request(req, 403);
    return 403;
  }

  int file_fd = open(filename, O_RDONLY);
  if (file_fd < 0) {
    if (file_lock)
      pthread_mutex_unlock(file_lock);
    send_error(fd, 403, "Forbidden", "Forbidden\n");
    log_request(req, 403);
    return 403;
  }

  off_t size = st.st_size;
  char *file_buf = NULL;

  if (size > 0) {
    file_buf = malloc((size_t)size);
    if (!file_buf) {
      close(file_fd);
      if (file_lock)
        pthread_mutex_unlock(file_lock);
      send_error(fd, 500, "Internal Server Error", "Internal Server Error\n");
      log_request(req, 500);
      return 500;
    }

    size_t off = 0;
    while (off < (size_t)size) {
      ssize_t n = read(file_fd, file_buf + off, (size_t)size - off);
      if (n <= 0) {
        free(file_buf);
        close(file_fd);
        if (file_lock)
          pthread_mutex_unlock(file_lock);
        send_error(fd, 500, "Internal Server Error", "Internal Server Error\n");
        log_request(req, 500);
        return 500;
      }
      off += (size_t)n;
    }
  }

  close(file_fd);

  // Linearization point for this GET
  log_request(req, 200);

  if (file_lock)
    pthread_mutex_unlock(file_lock);

  // Send response AFTER releasing the file lock so a paused client
  // doesn't block conflicting requests.
  char header[512];
  int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Length: %ld\r\n"
                            "\r\n",
                            (long)size);
  writen(fd, header, header_len);
  if (size > 0 && file_buf) {
    writen(fd, file_buf, (size_t)size);
  }
  free(file_buf);

  return 200;
}

int handle_put(int client_fd, HttpRequest *req, char *body, size_t body_len,
               int use_lock) {
  const char *filename = req->uri + 1;

  // No early rejection here: if there is no Content-Length,
  // we just treat it as a zero-length body.

  // 1) Read the entire body into memory WITHOUT holding the file lock.
  size_t total_len = req->content_length;
  char *buf = NULL;

  if (total_len > 0) {
    buf = malloc(total_len);
    if (!buf) {
      send_error(client_fd, 500, "Internal Server Error",
                 "Internal Server Error\n");
      log_request(req, 500);
      return 500;
    }

    size_t copied = 0;
    if (body && body_len > 0) {
      if (body_len > total_len)
        body_len = total_len;
      memcpy(buf, body, body_len);
      copied = body_len;
    }

    while (copied < total_len) {
      ssize_t n = read(client_fd, buf + copied, total_len - copied);
      if (n <= 0) {
        free(buf);
        send_error(client_fd, 400, "Bad Request", "Bad Request\n");
        log_request(req, 400);
        return 400;
      }
      copied += (size_t)n;
    }
  }

  // 2) Now take the per-file lock and write the file atomically.
  pthread_mutex_t *file_lock = NULL;
  if (use_lock) {
    init_file_locks();
    unsigned bucket = hash_path(filename);
    file_lock = &file_locks[bucket];
    pthread_mutex_lock(file_lock);
  }

  int existed = (access(filename, F_OK) == 0);

  int file_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file_fd < 0) {
    if (file_lock)
      pthread_mutex_unlock(file_lock);
    free(buf);

    if (errno == EACCES) {
      send_error(client_fd, 403, "Forbidden", "Forbidden\n");
      log_request(req, 403);
      return 403;
    }
    send_error(client_fd, 500, "Internal Server Error",
               "Internal Server Error\n");
    log_request(req, 500);
    return 500;
  }

  size_t written = 0;
  while (written < total_len) {
    ssize_t w = write(file_fd, buf + written, total_len - written);
    if (w <= 0) {
      close(file_fd);
      if (file_lock)
        pthread_mutex_unlock(file_lock);
      free(buf);
      send_error(client_fd, 500, "Internal Server Error",
                 "Internal Server Error\n");
      log_request(req, 500);
      return 500;
    }
    written += (size_t)w;
  }

  close(file_fd);
  free(buf);

  HttpResponse resp;
  int status;

  if (existed) {
    status = 200;
    resp.code = 200;
    resp.phrase = "OK";
    resp.body = "OK\n";
    resp.body_len = 3;
  } else {
    status = 201;
    resp.code = 201;
    resp.phrase = "Created";
    resp.body = "Created\n";
    resp.body_len = 8;
  }

  // Linearization point for this PUT: we have completely written the file.
  log_request(req, status);

  if (file_lock)
    pthread_mutex_unlock(file_lock);

  // Send response AFTER releasing the per-file lock, so a paused client
  // doesn't block other requests to this file.
  send_response(client_fd, &resp);

  return status;
}

void handle_request(int fd) {
  char buffer[BUFFER_SIZE];
  size_t total_read = 0;
  ssize_t n;
  char *header_end = NULL;

  while (total_read < sizeof(buffer) - 1 && !header_end) {
    n = read(fd, buffer + total_read, sizeof(buffer) - total_read - 1);
    if (n <= 0) {
      if (total_read == 0)
        return;
      break;
    }
    total_read += n;
    buffer[total_read] = '\0';
    header_end = strstr(buffer, "\r\n\r\n");
  }

  if (!header_end) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, ",,400,0\n");
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  char *request_line_end = strstr(buffer, "\r\n");
  if (!request_line_end || request_line_end > header_end) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, ",,400,0\n");
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  size_t request_line_len = request_line_end - buffer;
  char reqline[BUFFER_SIZE];
  if (request_line_len >= sizeof(reqline)) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, ",,400,0\n");
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }
  memcpy(reqline, buffer, request_line_len);
  reqline[request_line_len] = '\0';

  HttpRequest req;
  memset(&req, 0, sizeof(req));

  if (parse_request_line(reqline, &req) < 0) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, ",,400,0\n");
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  char *header_start = request_line_end + 2;
  size_t header_len = 0;

  if (header_end >= header_start) {
    header_len = (size_t)(header_end - header_start + 2);
  }

  if (request_line_len + 2 + header_len > MAX_HEADER_SIZE) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, "%s,%s,400,%" PRIu64 "\n", req.method, req.uri,
            req.request_id);
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  char *body_start = header_end + 4;
  size_t body_in_buffer = 0;
  if ((size_t)(body_start - buffer) < total_read) {
    body_in_buffer = total_read - (body_start - buffer);
  }

  char headers_buf[MAX_HEADER_SIZE + 1];
  if (header_len > MAX_HEADER_SIZE) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, "%s,%s,400,%" PRIu64 "\n", req.method, req.uri,
            req.request_id);
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }
  memcpy(headers_buf, header_start, header_len);
  headers_buf[header_len] = '\0';

  if (parse_headers(headers_buf, &req) < 0) {
    send_error(fd, 400, "Bad Request", "Bad Request\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, "%s,%s,400,%" PRIu64 "\n", req.method, req.uri,
            req.request_id);
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  if (strcmp(req.version, "HTTP/1.1") != 0) {
    send_error(fd, 505, "Version Not Supported", "Version Not Supported\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, "%s,%s,505,%" PRIu64 "\n", req.method, req.uri,
            req.request_id);
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }

  const char *body_ptr = NULL;
  size_t body_len = 0;
  if (strcmp(req.method, "PUT") == 0) {
    if (req.has_content_length && req.content_length > 0) {
      size_t to_copy = body_in_buffer;
      if (to_copy > req.content_length) {
        to_copy = req.content_length;
      }
      body_ptr = body_start;
      body_len = to_copy;
    }
  }

  if (strcmp(req.method, "GET") == 0) {
    handle_get(fd, &req);
    return; // already logged
  } else if (strcmp(req.method, "PUT") == 0) {
    handle_put(fd, &req, (char *)body_ptr, body_len, global_parallelism > 1);
    return; // already logged
  } else {
    send_error(fd, 501, "Not Implemented", "Not Implemented\n");
    pthread_mutex_lock(&audit_log_mutex);
    fprintf(stderr, "%s,%s,501,%" PRIu64 "\n", req.method, req.uri,
            req.request_id);
    pthread_mutex_unlock(&audit_log_mutex);
    return;
  }
}

void *worker_thread(void *arg) {
  ThreadPool *pool = (ThreadPool *)arg;
  Queue *queue = pool->queue;

  while (1) {
    int client_fd = queue_dequeue(queue);
    if (client_fd < 0) {
      if (pool->shutdown)
        break;
      continue;
    }

    handle_request(client_fd);
    close(client_fd);
  }

  return NULL;
}

ThreadPool *thread_pool_init(int listen_fd, int num_threads) {
  ThreadPool *pool = malloc(sizeof(ThreadPool));
  if (!pool)
    return NULL;

  pool->queue = queue_init(MAX_QUEUE_SIZE);
  if (!pool->queue) {
    free(pool);
    return NULL;
  }

  pool->listen_fd = listen_fd;
  pool->num_threads = num_threads;
  pool->shutdown = 0;
  pool->threads = malloc(num_threads * sizeof(pthread_t));
  if (!pool->threads) {
    queue_destroy(pool->queue);
    free(pool);
    return NULL;
  }

  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
      pool->shutdown = 1;
      for (int j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }
      free(pool->threads);
      queue_destroy(pool->queue);
      free(pool);
      return NULL;
    }
  }

  return pool;
}

void thread_pool_destroy(ThreadPool *pool) {
  if (!pool)
    return;

  pool->shutdown = 1;

  for (int i = 0; i < pool->num_threads; i++) {
    queue_enqueue(pool->queue, -1);
  }

  for (int i = 0; i < pool->num_threads; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  free(pool->threads);
  queue_destroy(pool->queue);
  free(pool);
}

int main(int argc, char *argv[]) {
  int port = 0;
  int parallelism = 1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 < argc) {
        parallelism = atoi(argv[i + 1]);
        i++;
      }
    } else {
      port = atoi(argv[i]);
    }
  }

  if (port < 1 || port > 65535) {
    fprintf(stderr, "Invalid Port\n");
    return 1;
  }

  if (parallelism < 1) {
    parallelism = 1;
  }

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    fprintf(stderr, "Invalid Port\n");
    return 1;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "Invalid Port\n");
    close(listen_fd);
    return 1;
  }

  if (listen(listen_fd, 128) < 0) {
    fprintf(stderr, "Invalid Port\n");
    close(listen_fd);
    return 1;
  }

  global_parallelism = parallelism;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  ThreadPool *pool = thread_pool_init(listen_fd, parallelism);
  if (!pool) {
    close(listen_fd);
    return 1;
  }

  while (!shutdown_requested) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd < 0) {
      if (errno == EINTR && shutdown_requested) {
        break;
      }
      continue;
    }

    if (shutdown_requested) {
      close(client_fd);
      break;
    }

    queue_enqueue(pool->queue, client_fd);
  }

  thread_pool_destroy(pool);

  if (file_locks_initialized) {
    for (int i = 0; i < FILE_LOCK_BUCKETS; i++) {
      pthread_mutex_destroy(&file_locks[i]);
    }
  }

  pthread_mutex_destroy(&audit_log_mutex);
  pthread_mutex_destroy(&file_locks_init_mutex);

  close(listen_fd);
  return 0;
}
