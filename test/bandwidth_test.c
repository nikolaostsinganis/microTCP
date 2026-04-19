/*
 * microtcp, a lightweight implementation of TCP for teaching,
 * and academic purposes.
 *
 * Copyright (C) 2015-2017  Manolis Surligas <surligas@gmail.com>
 * ... (License header) ...
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <time.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../lib/microtcp.h"

#define CHUNK_SIZE 4096

/*
 * Helper function to calculate and print transfer statistics.
 * Converts bytes to Megabytes and calculates throughput in MB/s.
 */
static inline void
print_statistics (ssize_t received, struct timespec start, struct timespec end)
{
  /* Calculate elapsed time in seconds */
  double elapsed = end.tv_sec - start.tv_sec
      + (end.tv_nsec - start.tv_nsec) * 1e-9;
  
  /* Convert total bytes received to Megabytes */
  double megabytes = received / (1024.0 * 1024.0);

  printf ("Data received: %f MB\n", megabytes);
  printf ("Transfer time: %f seconds\n", elapsed);
  printf ("Throughput achieved: %f MB/s\n", megabytes / elapsed);
}

/*
 * Standard TCP Server Implementation.
 * This acts as a benchmark to compare the performance of your microTCP implementation.
 * It uses the standard POSIX socket API (socket, bind, listen, accept).
 */
int
server_tcp (uint16_t listen_port, const char *file)
{
  uint8_t *buffer;
  FILE *fp;
  int sock;
  int accepted;
  int received;
  ssize_t written;
  ssize_t total_bytes = 0;
  socklen_t client_addr_len;

  struct sockaddr_in sin;
  struct sockaddr client_addr;
  struct timespec start_time;
  struct timespec end_time;

  /* Allocate memory for the application receive buffer */
  buffer = (uint8_t *) malloc (CHUNK_SIZE);
  if (!buffer) {
    perror ("Allocate application receive buffer");
    return -EXIT_FAILURE;
  }

  /* Open the file where received data will be saved */
  fp = fopen (file, "w");
  if (!fp) {
    perror ("Open file for writing");
    free (buffer);
    return -EXIT_FAILURE;
  }

  /* Create a standard TCP socket (SOCK_STREAM) */
  if ((sock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
    perror ("Opening TCP socket");
    free (buffer);
    fclose (fp);
    return -EXIT_FAILURE;
  }

  memset (&sin, 0, sizeof(struct sockaddr_in));
  sin.sin_family = AF_INET;
  sin.sin_port = htons (listen_port);
  sin.sin_addr.s_addr = INADDR_ANY; /* Listen on all interfaces */

  if (bind (sock, (struct sockaddr *) &sin, sizeof(struct sockaddr_in)) == -1) {
    perror ("TCP bind");
    free (buffer);
    fclose (fp);
    return -EXIT_FAILURE;
  }

  if (listen (sock, 1000) == -1) {
    perror ("TCP listen");
    free (buffer);
    fclose (fp);
    return -EXIT_FAILURE;
  }

  /* Block and wait for an incoming connection */
  client_addr_len = sizeof(struct sockaddr);
  accepted = accept (sock, &client_addr, &client_addr_len);
  if (accepted < 0) {
    perror ("TCP accept");
    free (buffer);
    fclose (fp);
    return -EXIT_FAILURE;
  }

  /*
   * Start processing the received data.
   * We start the timer here to measure the throughput (Bandwidth).
   */
  clock_gettime (CLOCK_MONOTONIC_RAW, &start_time);
  
  /* Receive loop: keeps reading until the peer closes connection (recv returns 0) */
  while ((received = recv (accepted, buffer, CHUNK_SIZE, 0)) > 0) {
    written = fwrite (buffer, sizeof(uint8_t), received, fp);
    total_bytes += received;
    if (written * sizeof(uint8_t) != received) {
      printf ("Failed to write to the file the"
              " amount of data received from the network.\n");
      shutdown (accepted, SHUT_RDWR);
      shutdown (sock, SHUT_RDWR);
      close (accepted);
      close (sock);
      free (buffer);
      fclose (fp);
      return -EXIT_FAILURE;
    }
  }
  
  /* Stop the timer */
  clock_gettime (CLOCK_MONOTONIC_RAW, &end_time);
  
  /* Print the bandwidth results */
  print_statistics (total_bytes, start_time, end_time);

  /* Clean up resources */
  shutdown (accepted, SHUT_RDWR);
  shutdown (sock, SHUT_RDWR);
  close (accepted);
  close (sock);
  fclose (fp);
  free (buffer);

  return 0;
}

/*
 * microTCP Server Implementation.
 * This function uses your custom library (microtcp_*) to receive files.
 */
int
server_microtcp (uint16_t listen_port, const char *file)
{
  uint8_t *buffer;
  FILE *fp;
  microtcp_sock_t sock;
  ssize_t received;
  ssize_t written;
  ssize_t total_bytes = 0;
  struct sockaddr_in sin;
  struct sockaddr client_addr;
  socklen_t client_addr_len;
  struct timespec start_time;
  struct timespec end_time;

  /* Allocate buffer */
  buffer = (uint8_t *) malloc (CHUNK_SIZE);
  if (!buffer) {
    perror ("Allocate application receive buffer");
    return -EXIT_FAILURE;
  }

  /* Open file for writing */
  fp = fopen (file, "w");
  if (!fp) {
    perror ("Open file for writing");
    free (buffer);
    return -EXIT_FAILURE;
  }

  /* * Create a microTCP socket.
   * [cite_start]Note: The underlying socket is UDP (SOCK_DGRAM)[cite: 7].
   */
  sock = microtcp_socket(AF_INET, SOCK_DGRAM, 0);
  if (sock.state == INVALID) {
    fprintf(stderr, "server_microtcp: microtcp_socket failed\n");
    free(buffer);
    fclose(fp);
    return -EXIT_FAILURE;
  }

  memset (&sin, 0, sizeof(struct sockaddr_in));
  sin.sin_family = AF_INET;
  sin.sin_port = htons (listen_port);
  sin.sin_addr.s_addr = INADDR_ANY;

  /* Bind the microTCP socket using your custom function */
  if (microtcp_bind (&sock, (struct sockaddr *) &sin,
                     sizeof(struct sockaddr_in)) == -1) {
    perror("server_microtcp: microtcp_bind");
    free(buffer);
    fclose(fp);
    return -EXIT_FAILURE;
  }

  /* * Accept a connection. 
   * [cite_start]This handles the server-side of the 3-Way Handshake[cite: 91].
   */
  client_addr_len = sizeof(struct sockaddr);
  if (microtcp_accept(&sock, &client_addr, client_addr_len) != 0) {
    fprintf(stderr, "server_microtcp: microtcp_accept failed\n");
    free(buffer);
    fclose(fp);
    return -EXIT_FAILURE;
  }

  /* Start measuring time for bandwidth calculation */
  clock_gettime (CLOCK_MONOTONIC_RAW, &start_time);
  
  while (1) {
    /* Receive data using microtcp_recv (to be implemented in Phase B) */
    received = microtcp_recv(&sock, buffer, CHUNK_SIZE, 0);
    
    if (received < 0) {
      if (sock.state == CLOSING_BY_PEER) {
        /* Connection closed by peer (FIN received) */
        break;
      }
      fprintf(stderr, "server_microtcp: microtcp_recv error\n");
      microtcp_shutdown(&sock, SHUT_RDWR);
      free(buffer);
      fclose(fp);
      return -EXIT_FAILURE;
    }
    if (received == 0) {
      /* Treat as close */
      break;
    }

    /* Write data to file */
    written = fwrite(buffer, sizeof(uint8_t), received, fp);
    total_bytes += received;
    if (written * sizeof(uint8_t) != received) {
      printf("Failed to write to the file the"
             " amount of data received from the network.\n");
      microtcp_shutdown(&sock, SHUT_RDWR);
      free(buffer);
      fclose(fp);
      return -EXIT_FAILURE;
    }
  }
  
  /* Stop measuring time */
  clock_gettime (CLOCK_MONOTONIC_RAW, &end_time);
  
  /* Display results */
  print_statistics(total_bytes, start_time, end_time);

  /* Terminate the microTCP connection */
  microtcp_shutdown(&sock, SHUT_RDWR);
  free(buffer);
  fclose(fp);

  return 0;
}


/*
 * Standard TCP Client Implementation.
 * Reads a local file and sends it to the server using standard TCP.
 */
int
client_tcp (const char *serverip, uint16_t server_port, const char *file)
{
  uint8_t *buffer;
  int sock;
  socklen_t client_addr_len;
  FILE *fp;
  size_t read_items = 0;
  ssize_t data_sent;

  struct sockaddr *client_addr;

  buffer = (uint8_t *) malloc (CHUNK_SIZE);
  if (!buffer) {
    perror ("Allocate application receive buffer");
    return -EXIT_FAILURE;
  }

  /* Open the file to be sent (Read mode) */
  fp = fopen (file, "r");
  if (!fp) {
    perror ("Open file for reading");
    free (buffer);
    return -EXIT_FAILURE;
  }

  /* Create standard TCP socket */
  if ((sock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
    perror ("Opening TCP socket");
    free (buffer);
    fclose (fp);
    return -EXIT_FAILURE;
  }

  struct sockaddr_in sin;
  memset (&sin, 0, sizeof(struct sockaddr_in));
  sin.sin_family = AF_INET;
  sin.sin_port = htons (server_port);
  sin.sin_addr.s_addr = inet_addr (serverip); /* Convert IP string to binary */

  /* Connect to the server */
  if (connect (sock, (struct sockaddr *) &sin, sizeof(struct sockaddr_in)) == -1) {
    perror ("TCP connect");
    exit (EXIT_FAILURE);
  }

  printf ("Starting sending data...\n");
  
  /* Read from file and send loop */
  while (!feof (fp)) {
    read_items = fread (buffer, sizeof(uint8_t), CHUNK_SIZE, fp);
    if (read_items < 1) {
      if (ferror(fp)) {
        perror ("Failed read from file");
        shutdown (sock, SHUT_RDWR);
        close (sock);
        free (buffer);
        fclose (fp);
        return -EXIT_FAILURE;
      }
      break; /* End of file reached */
    }

    data_sent = send (sock, buffer, read_items * sizeof(uint8_t), 0);
    if (data_sent != read_items * sizeof(uint8_t)) {
      printf ("Failed to send the"
              " amount of data read from the file.\n");
      shutdown (sock, SHUT_RDWR);
      close (sock);
      free (buffer);
      fclose (fp);
      return -EXIT_FAILURE;
    }
  }

  printf ("Data sent. Terminating...\n");
  shutdown (sock, SHUT_RDWR);
  close (sock);
  free (buffer);
  fclose (fp);
  return 0;
}

/*
 * microTCP Client Implementation.
 * Reads a local file and sends it to the server using your custom library.
 */
int
client_microtcp (const char *serverip, uint16_t server_port, const char *file)
{
  uint8_t *buffer;
  FILE *fp;
  size_t read_items = 0;
  ssize_t data_sent;
  microtcp_sock_t sock;
  struct sockaddr_in sin;

  buffer = (uint8_t *) malloc (CHUNK_SIZE);
  if (!buffer) {
    perror ("Allocate application buffer");
    return -EXIT_FAILURE;
  }

  fp = fopen (file, "r");
  if (!fp) {
    perror ("Open file for reading");
    free (buffer);
    return -EXIT_FAILURE;
  }

  /* Create the microTCP socket */
  sock = microtcp_socket(AF_INET, SOCK_DGRAM, 0);
  if (sock.state == INVALID) {
    fprintf(stderr, "client_microtcp: microtcp_socket failed\n");
    free(buffer);
    fclose(fp);
    return -EXIT_FAILURE;
  }

  memset(&sin, 0, sizeof(struct sockaddr_in));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(server_port);
  sin.sin_addr.s_addr = inet_addr(serverip);

  /* * Initiate the 3-Way Handshake (sends SYN).
   */
  if (microtcp_connect(&sock, (struct sockaddr*)&sin,
                       sizeof(struct sockaddr_in)) != 0) {
    fprintf(stderr, "client_microtcp: microtcp_connect failed\n");
    free(buffer);
    fclose(fp);
    return -EXIT_FAILURE;
  }

  printf("Starting sending data over microTCP...\n");
  
  /* Read from file and send using microtcp_send */
  while (!feof(fp)) {
    read_items = fread(buffer, sizeof(uint8_t), CHUNK_SIZE, fp);
    if (read_items == 0) {
      if (ferror(fp)) {
        perror("Failed read from file");
        microtcp_shutdown(&sock, SHUT_RDWR);
        free(buffer);
        fclose(fp);
        return -EXIT_FAILURE;
      }
      break; /* End of File */
    }

    /* Send data packet */
    data_sent = microtcp_send(&sock, buffer,
                              read_items * sizeof(uint8_t), 0);
    
    if (data_sent != (ssize_t)(read_items * sizeof(uint8_t))) {
      printf("Failed to send the amount of data read from the file.\n");
      microtcp_shutdown(&sock, SHUT_RDWR);
      free(buffer);
      fclose(fp);
      return -EXIT_FAILURE;
    }
  }

  printf("Data sent. Terminating microTCP connection...\n");
  
  /* Perform connection termination (sends FIN) */
  microtcp_shutdown(&sock, SHUT_RDWR);
  free(buffer);
  fclose(fp);
  return 0;
}


int
main (int argc, char **argv)
{
  int opt;
  int port;
  int exit_code = 0;
  char *filestr = NULL;
  char *ipstr = NULL;
  uint8_t is_server = 0;
  uint8_t use_microtcp = 0;

  /* * Parse command line arguments.
   * -s: Run as Server
   * -m: Use microTCP (default is standard TCP)
   * -f: Filename (input for client, output for server)
   * -p: Port number
   * -a: IP address (client only)
   */
  while ((opt = getopt (argc, argv, "hsmf:p:a:")) != -1) {
    switch (opt)
      {
      /* If -s is set, program runs in server mode */
      case 's':
        is_server = 1;
        break;
      /* if -m is set, use the microTCP library */
      case 'm':
        use_microtcp = 1;
        break;
      case 'f':
        filestr = strdup (optarg);
        /* Note: Always check if file exists or path is valid */
        break;
      case 'p':
        port = atoi (optarg);
        break;
      case 'a':
        ipstr = strdup (optarg);
        break;

      default:
        printf (
            "Usage: bandwidth_test [-s] [-m] -p port -f file"
            "Options:\n"
            "   -s                  If set, the program runs as server. Otherwise as client.\n"
            "   -m                  If set, the program uses the microTCP implementation. Otherwise the normal TCP.\n"
            "   -f <string>         If -s is set the -f option specifies the filename of the file that will be saved.\n"
            "                       If not, is the source file at the client side that will be sent to the server.\n"
            "   -p <int>            The listening port of the server\n"
            "   -a <string>         The IP address of the server. This option is ignored if the tool runs in server mode.\n"
            "   -h                  prints this help\n");
        exit (EXIT_FAILURE);
      }
  }

  /*
   * Dispatch logic:
   * Choose the correct function based on the arguments provided (Server/Client, TCP/microTCP).
   */
  if (is_server) {
    if (use_microtcp) {
      exit_code = server_microtcp (port, filestr);
    }
    else {
      exit_code = server_tcp (port, filestr);
    }
  }
  else {
    /* Client mode */
    if (use_microtcp) {
      exit_code = client_microtcp (ipstr, port, filestr);
    }
    else {
      exit_code = client_tcp (ipstr, port, filestr);
    }
  }

  free (filestr);
  free (ipstr);
  return exit_code;
}