/*
 * microtcp, a lightweight implementation of TCP for teaching,
 * and academic purposes.
 *
 * Copyright (C) 2015-2017  Manolis Surligas <surligas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * You can use this file to write a test microTCP server.
 * This file is already inserted at the build system.
 */

#include "../lib/microtcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int
main(int argc, char **argv)
{
  microtcp_sock_t sock = microtcp_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  
  if (sock.state == INVALID) {
    printf("Socket creation failed\n");
    return EXIT_FAILURE;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(8888);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (microtcp_bind(&sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    printf("Bind failed\n");
    microtcp_shutdown(&sock, 0);
    return EXIT_FAILURE;
  }

  printf("Server listening on port 8888...\n");

  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  if (microtcp_accept(&sock, (struct sockaddr *)&client_addr, client_addr_len) < 0) {
    printf("Accept failed\n");
    microtcp_shutdown(&sock, 0);
    return EXIT_FAILURE;
  }

  printf("Client connected from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
  microtcp_shutdown(&sock, 0);

  return EXIT_SUCCESS;
}