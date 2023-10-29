/**
 * @file httpserver.c
 * @brief Simple HTTP server implementation.
 *
 * This file contains the implementation of a basic HTTP server with the
 * capability to serve files from a directory and act as a proxy to another
 * server.
 */

/* Include the arpa/inet.h header for function and structures related to
 * internet operations, such as converting IP addresses. */
#include <arpa/inet.h>

/* Include the dirent.h header for directory entries, primarily for reading
 * directories. */
#include <dirent.h>

/* Include the errno.h header to handle error codes reported by system calls and
 * some libraries functions. */
#include <errno.h>

/* Include the fcntl.h header for file control operations like locking. */
#include <fcntl.h>

/* Include the netdb.h header for definitions used in network database
 * operations. */
#include <netdb.h>

/* include thenetinet/in.h header for internet protocol family, like constant
 * and structures used in sockets. */
#include <netinet/in.h>

/* Include the pthread.h header for POSIX threads, providing multithreading
 * functionality. */
#include <pthread.h>

/* Include the signal.h header for signals handling, which are software
 * interrupts. */
#include <signal.h>

/* Include the stdio.h header for standard I/O operations. */
#include <stdio.h>

/* Include the stdlib.h header for general purpose functions like malloc() and
 * free(). */
#include <stdlib.h>

/* Include the string.h header for string manipulation and handling functions.
 */
#include <string.h>

/* Include the sys/socket.h header for main sockets defintions and data types.
 */
#include <sys/socket.h>

/* Include the sys/stat.h header for file attributes manipulaton, like getting a
 * file's size. */
#include <sys/stat.h>

/* Include the sys/types.h header for basic dervied types. */
#include <sys/types.h>

/* Include the unistd.h header for POSIX standard symbolic constants and types,
 * used for system calls. */
#include <unistd.h>

/* Include the libhttp.h header, a custom library related to HTTP operations. */
#include "libhttp.h"

/* Include the wq.h header, for a work queue used in conjunction with the thread
 * pool. */
#include "wq.h"

/* Define a variable 'work_queue' of type 'wq_t'. It represents a work queue to
 * manage tasks or jobs. */
wq_t work_queue;

/* Define an integer variable 'num_threads' to store the number of threads in
 * the thread pool. */
int num_threads;

/* Define a integer variable 'server_port' to store the port number where the
 * server will listen for incoming connections. */
int server_port;

/* Define a pointer variable 'server_files_directory' of type char. This stores
 * the path to the directory where server files are kept. */
char *server_files_directory;

/* Proxy variable information that is not being used. */
char *server_proxy_hostname;
int server_proxy_port;

/**
 * @brief Handles HTTP file request.
 *
 * This function processes HTTP request for files, responding with the
 * appropriate content and HTTP status codes based on the request and file
 * system state.
 *
 * @param fd The file descriptor associated with teh client making the request
 */
void handle_files_request(int fd) {
  /* Parse the incoming HTTP request */
  struct http_request *request = http_request_parse(fd);

  /* If the request could not be pares, send a 400 Bad Request response. */
  if (!request) {
    http_start_response(fd, 400);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<h1>400 Bad Request</h1>");
    return;
  }

  /* Construct the absolute path to the requested resource. */
  char path[4096];
  snprintf(path, sizeof(path), "%s%s", server_files_directory, request->path);

  /* Check if teh requested path exists and obtain its stats. */
  struct stat path_stat;
  if (stat(path, &path_stat) == -1) {
    http_start_response(fd, 404);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<h1>404 Not Found</h1>");
    return;
  }

  /* If the requested path is a regular file, send its contents. */
  if (S_ISREG(path_stat.st_mode)) {
    http_start_response(fd, 200);
    http_send_header(fd, "Content-Type", "application/octet-stream");
    http_end_headers(fd);
    int file_fd = open(path, O_RDONLY);
    if (file_fd != -1) {
      char buffer[4096];
      ssize_t bytes_read;
      while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        write(fd, buffer, bytes_read);
      }
      close(file_fd);
    }
    /*
     * If the requested path is a directory, send the contents of the index.html
     * file id it exits or a listing of the directory otherwise.
     */
  } else if (S_ISDIR(path_stat.st_mode)) {
    char index_path[4096];
    snprintf(index_path, sizeof(index_path), "%s/index.html", path);
    if (access(index_path, F_OK) != -1) {
      http_start_response(fd, 200);
      http_send_header(fd, "Content-Type", "text/html");
      http_end_headers(fd);
      int file_fd = open(index_path, O_RDONLY);
      if (file_fd != -1) {
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
          write(fd, buffer, bytes_read);
        }
        close(file_fd);
      }
    } else {
      http_start_response(fd, 200);
      http_send_header(fd, "Content-Type", "text/html");
      http_end_headers(fd);
      http_send_string(fd, "<html><body>");
      http_send_string(fd, "<a href=\"../\">Parent directory</a><br>");
      DIR *dir = opendir(path);
      struct dirent *entry;
      http_send_string(fd, "<ul>");
      while ((entry = readdir(dir)) != NULL) {
        http_send_string(fd, "<li><a href=\"");
        http_send_string(fd, entry->d_name);
        http_send_string(fd, "\">");
        http_send_string(fd, entry->d_name);
        http_send_string(fd, "</a></li>");
      }
      http_send_string(fd, "</ul>");
      http_send_string(fd, "</body></html>");
      closedir(dir);
    }
    /* For all other types of paths, send a 403 Forbidden response. */
  } else {
    http_start_response(fd, 403);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<h1>403 Forbidden</h1>");
  }
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname
 * and port=server_proxy_port) and relays traffic to/from the stream fd and
 * the proxy target. HTTP requests from the client (fd) should be sent to
 * the proxy target, and HTTP responses from the proxy target should be sent
 * to the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void handle_proxy_request(int fd) {

  /*
   * The code below does a DNS lookup of server_proxy_hostname and
   * opens a connection to it. Please do not modify.
   */

  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  struct hostent *target_dns_entry =
      gethostbyname2(server_proxy_hostname, AF_INET);

  int client_socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (client_socket_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno,
            strerror(errno));
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    exit(ENXIO);
  }

  char *dns_address = target_dns_entry->h_addr_list[0];

  memcpy(&target_address.sin_addr, dns_address,
         sizeof(target_address.sin_addr));
  int connection_status =
      connect(client_socket_fd, (struct sockaddr *)&target_address,
              sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_start_response(fd, 502);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<center><h1>502 Bad Gateway</h1><hr></center>");
    return;
  }

  /*
   * TODO: Your solution for task 3 belongs here!
   */
}

/**
 * @brief Worker thread function for handling client requests.
 *
 * This function acts as the primary task for worker threads in a threaded
 * server setup. Worker threads contiually fetch client sockets from a shared
 * work queue and process them using the provided request handler.
 *
 * @param arg A pointer to the request handler function. this handler is
 * responsible for processing client requests.
 *
 * @return Always return NULL.
 */
void *worker_thread_func(void *arg) {
  /* Cast the provided argument to a request handler function pointer. */
  void (*request_handler)(int) = (void (*)(int))arg;

  /* Continuously serve requests from the work queue. */
  while (1) {
    /* Fetch the next client socket from the work queue. */
    int client_socket = wq_pop(&work_queue);

    /* Process the client request using the provided handler. */
    request_handler(client_socket);

    /* Close the client socker once done processing. */
    close(client_socket);
  }
  return NULL;
}

/**
 * @brief Initializes a thread pool with the specified number of worker threads.
 *
 * This function initializes a thread pool that will server client requests.
 * Each thread in the pool continuously fetches client sockets from a shared
 * work queue and processes them using the provided request handler. The
 * initialized threads run the 'worker_thread_func' which implements this
 * behavior.
 *
 * @param num_threads The number of worker threads to create in the thread pool.
 * @param request_handler A pointer to the funcition that handles client
 * requests.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {
  /* Initialize the work queue. */
  wq_init(&work_queue);

  /* Create the specified number of worker threads. */
  for (int i = 0; i < num_threads; i++) {
    /* Variable to hold the thread identifier. */
    pthread_t thread;

    /*
     * Create a new thread that runs the worker_thread_func.
     * If the thread creation fails, print an error and exit.
     */
    if (pthread_create(&thread, NULL, worker_thread_func,
                       (void *)request_handler) != 0) {
      perror("Failed to create worker thread");
      exit(errno);
    }
  }
}

/**
 * @brief Continuously serves client requests on the provided socket.
 *
 * This function initializes a server socket, binds it to a specified address
 * and port, and listens for incoming client conntections. The function then
 * spinds up a pool of worker threads that process the incoming requests using
 * the provided request handler. The server runs in an infinite loop,
 * continuously accepting and processing incoming client connections.
 *
 * @param socket_number A pointer to the interger holding the server socket
 * descriptor.
 * @param request_handler A pointer to the function that handles client
 * requests.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {

  /* Define structures for the server and client addresses. */
  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  /* Initialize the length of the client address structure. */
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  /* Set the socket option to allow the reuse of the address. */
  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                 sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  /* Initialize the server address structure. */
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /* Bind the socket to the specified server address. */
  if (bind(*socket_number, (struct sockaddr *)&server_address,
           sizeof(server_address)) == -1) {
    perror("Failed to bind on socket");
    exit(errno);
  }

  /* Listen on the socket for incoming client connections with a backlog of
   * 1024. */
  if (listen(*socket_number, 1024) == -1) {
    perror("Failed to listen on socket");
    exit(errno);
  }

  /* Notify that the server is now listening on the specified port. */
  printf("Listening on port %d...\n", server_port);

  /* Initialize a thread pool with the specified number of worker threads. */
  init_thread_pool(num_threads, request_handler);

  /* Infinite loop to continuously accept and serve incoming client connections.
   */
  while (1) {
    client_socket_number =
        accept(*socket_number, (struct sockaddr *)&client_address,
               (socklen_t *)&client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    /* Notify of the accepted connection from a client */
    printf("Accepted connection from %s on port %d\n",
           inet_ntoa(client_address.sin_addr), client_address.sin_port);

    /* Add the client socket to the work queue for the worker threads to
     * process. */
    wq_push(&work_queue, client_socket_number);
  }

  /* Gracefully shut down the socket for both reading and writing. */
  shutdown(*socket_number, SHUT_RDWR);

  /* Close the server socket. */
  close(*socket_number);
}

/* File descriptor for the server socket used to accept incoming client
 * connections. */
int server_fd;

/**
 * Handles received signals by the server process.
 * This function is typically used to gracefully shut down the server in the
 * event of unexpected termination.
 *
 * @param signum The signal number received.
 */
void signal_callback_handler(int signum) {
  /* Print the caught signal number and its string representation. */
  printf("Caught signal %d: %s\n", signum, strsignal(signum));

  /* Notify that the server socket is being closed. */
  printf("Closing socket %d\n", server_fd);

  /* Attempt to close the server socket. */
  if (close(server_fd) < 0)
    /* Print an error message if closing ther ser socket failed. This error is
     * non-fatal and the process will still terminate. */
    perror("Failed to close server_fd (ignoring)\n");

  /* Terminate the process with a normal exit status. */
  exit(0);
}

/* A string representing the usage instructions for the program. */
char *USAGE =
    "Usage: ./httpserver --files www_directory/ --port 8000 [--num-threads "
    "5]\n"
    "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000 "
    "[--num-threads 5]\n";

/**
 * @brief Display usage instructions and exit the program.
 *
 * This function is called to inform the user of the correct way to use the
 * program when they've provided incorrect or insufficient arguments. It
 * prints the correct usage to the standard error stream and then terminates
 * the program.
 *
 * @note The program exits with a status indicating success.
 */
void exit_with_usage() {
  /* Print the usage instructions to stderr. */
  fprintf(stderr, "%s", USAGE);

  /* Exit the program with a status of success. */
  exit(EXIT_SUCCESS);
}

/**
 * @brief Main entry point for the HTTP server program.
 *
 * This function configures and starts the server based on command-line
 * arguments. Users can specify server configurations such as the port number,
 * directory to server files from, number of worker threads, and proxy settings.
 *
 * @param argc Number of command-linne arguments.
 * @param argv Array of command-line argument strings.
 * @return Returns 'EXIT_SUCCESS' upon successful execution.
 *
 * @notes The server can be run in two modes: file serving mode or proxy mode.
 */
int main(int argc, char **argv) {
  /* Register a signal handler for SIGINT (Ctrl+C) for graceful shutdown. */
  signal(SIGINT, signal_callback_handler);

  /* Default port setting. */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  /* Parse command-line arguments. */
  int i;
  for (i = 1; i < argc; i++) {
    /* If the --files option is provided, set up the server to serve files from
     * the specified directory */
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;

      free(server_files_directory);

      server_files_directory = argv[++i];

      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }

      /* If the --proxy option is provided, set up the server as a proxy to
       * another server. */
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];

      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');

      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {

        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }

      /* Specify custom server port. */
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];

      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }

      server_port = atoi(server_port_string);

      /* Specify the number of worker threads for the server. */
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char *num_threads_str = argv[++i];

      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }

      /* Display usage information. */
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  /* Ensure that one of the modes (file serving or proxy) is selected. */
  if (server_files_directory == NULL && server_proxy_hostname == NULL) {

    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

  /* Start the server. */
  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}