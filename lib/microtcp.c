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

#include "microtcp.h"
#include "../utils/crc32.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static inline size_t microtcp_adv_window(const microtcp_sock_t *s) {
    return (s->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - s->buf_fill_level);
}

/* Control flag bits definitions */
#define MICROTCP_FLAG_ACK (1 << 13)
#define MICROTCP_FLAG_SYN (1 << 14)
#define MICROTCP_FLAG_FIN (1 << 15)
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static struct sockaddr_in g_peer_addr; /*<-- Stores the connected peer's address */
static socklen_t g_peer_len = 0;


/**
 * Initializes a new microTCP socket structure.
 * Sets up the underlying UDP socket and initializes counters/states.
 */
microtcp_sock_t microtcp_socket (int domain, int type, int protocol)
{
    microtcp_sock_t sock;
    
    /* Zero out the entire structure to avoid garbage data */
    memset(&sock, 0, sizeof(sock));

    /* Create a UDP socket using the system's socket API */
    sock.sd = socket(domain, type, protocol);
    if (sock.sd < 0) {
        perror("microtcp_socket: cannot create UDP socket");
        sock.state = INVALID;
        return sock;
    }

    /* Initialize socket state and window sizes */
    sock.state         = CLOSED;
    sock.init_win_size = MICROTCP_WIN_SIZE;
    sock.curr_win_size = MICROTCP_WIN_SIZE;

    /* Initialize Congestion Control parameters (Phase B) */
    sock.cwnd     = MICROTCP_INIT_CWND;
    sock.ssthresh = MICROTCP_INIT_SSTHRESH;

    /* Initialize statistics counters */
    sock.packets_send     = 0;
    sock.packets_received = 0;
    sock.packets_lost     = 0;
    sock.bytes_send       = 0;
    sock.bytes_received   = 0;
    sock.bytes_lost       = 0;

    /* * Initialize Sequence Number with a random value (security best practice).
     * Initialize ACK Number to 0.
     */
    sock.seq_number = (size_t) rand();
    sock.ack_number = 0;

    /* Receive buffer will be allocated later (on connection) */
    sock.recvbuf        = NULL;
    sock.buf_fill_level = 0;

    return sock;
}


/**
 * Binds the underlying UDP socket to a specific address and port.
 * Essential for the server side to listen for incoming connections.
 */
int microtcp_bind (microtcp_sock_t *socket, const struct sockaddr *address, socklen_t address_len)
{
    /* Sanity check */
    if (!socket || socket->state == INVALID)
        return -1;

    /* Perform the system bind call */
    if (bind(socket->sd, address, address_len) < 0) {
        perror("microtcp_bind: bind failed");
        socket->state = INVALID;
        return -1;
    }

    /* Update state to LISTEN, ready to accept connections */
    socket->state = LISTEN;
    return 0;
}


/**
 * CLIENT SIDE: Initiates the 3-Way Handshake.
 * 1. Sends SYN
 * 2. Receives SYN + ACK
 * 3. Sends ACK
 */
int microtcp_connect (microtcp_sock_t *socket, const struct sockaddr *address, socklen_t address_len)
{
    if (!socket || socket->state == INVALID)
        return -1;

    /* --- STEP 1: Send SYN packet --- */
    microtcp_header_t syn_hdr;
    memset(&syn_hdr, 0, sizeof(syn_hdr));

    /* Prepare SYN header */
    syn_hdr.seq_number = (uint32_t) socket->seq_number; /* Random client ISN */
    syn_hdr.ack_number = 0;
    syn_hdr.control    = MICROTCP_FLAG_SYN; /* Set SYN flag */
    syn_hdr.window     = (uint16_t) microtcp_adv_window(socket);
    syn_hdr.data_len   = 0;
    
    /* Zero unused fields */
    syn_hdr.future_use0 = 0;
    syn_hdr.future_use1 = 0;
    syn_hdr.future_use2 = 0;

    /* Calculate Checksum (must set checksum field to 0 before calculation) */
    syn_hdr.checksum = 0;
    syn_hdr.checksum = crc32((uint8_t*)&syn_hdr, sizeof(syn_hdr));

    /* Send the SYN packet to the server */
    if (sendto(socket->sd, &syn_hdr, sizeof(syn_hdr), 0, address, address_len) < 0) {
        perror("microtcp_connect: send SYN failed");
        return -1;
    }

    
    /* --- STEP 2: Receive SYN+ACK from Server --- */
    microtcp_header_t synack_hdr;
    socklen_t from_len = address_len;

    /* Blocking receive call waiting for server response */
    if (recvfrom(socket->sd, &synack_hdr, sizeof(synack_hdr), 0, (struct sockaddr*)address, &from_len) < 0) {
        perror("microtcp_connect: recv SYN+ACK failed");
        return -1;
    }

    /* Verify Checksum */
    uint32_t orig = synack_hdr.checksum;
    synack_hdr.checksum = 0; /* Reset to 0 to recalculate */
    uint32_t calc = crc32((uint8_t*)&synack_hdr, sizeof(synack_hdr));

    if (calc != orig) {
        fprintf(stderr, "microtcp_connect: SYN+ACK checksum mismatch\n");
        return -1;
    }

    /* Verify Flags: Must contain both SYN and ACK */
    if (!(synack_hdr.control & MICROTCP_FLAG_SYN) || !(synack_hdr.control & MICROTCP_FLAG_ACK)) {
        fprintf(stderr, "microtcp_connect: invalid SYN+ACK flags\n");
        return -1;
    }

    /* Verify ACK Number: Should be Client_Seq + 1 */
    if (synack_hdr.ack_number != (uint32_t)(socket->seq_number + 1)) {
        fprintf(stderr, "microtcp_connect: wrong ACK number in SYN+ACK\n");
        return -1;
    }

    
    /* --- STEP 3: Send Final ACK --- */
    
    /* Update local ACK number (Expected Seq from Server + 1) */
    socket->ack_number = (size_t)synack_hdr.seq_number + 1;

    microtcp_header_t ack_hdr;
    memset(&ack_hdr, 0, sizeof(ack_hdr));

    /* Increment Client Sequence Number (SYN consumes 1 seq number) */
    socket->seq_number += 1; 

    /* Prepare ACK Header */
    ack_hdr.seq_number = (uint32_t)socket->seq_number;
    ack_hdr.ack_number = (uint32_t)socket->ack_number;
    ack_hdr.control    = MICROTCP_FLAG_ACK; /* Only ACK flag set */
    ack_hdr.window     = (uint16_t) microtcp_adv_window(socket);
    ack_hdr.data_len   = 0;

    /* Calculate Checksum */
    ack_hdr.checksum = 0;
    ack_hdr.checksum = crc32((uint8_t*)&ack_hdr, sizeof(ack_hdr));

    /* Send ACK to server */
    if (sendto(socket->sd, &ack_hdr, sizeof(ack_hdr), 0, address, address_len) < 0) {
        perror("microtcp_connect: send final ACK failed");
        return -1;
    }

    /* * Store Peer Address globally.
     * This is useful if we want to filter incoming packets later.
     */
    memset(&g_peer_addr, 0, sizeof(g_peer_addr));
    if (address_len < (socklen_t)sizeof(g_peer_addr)) {
      memcpy(&g_peer_addr, address, address_len);
    } else {
      memcpy(&g_peer_addr, address, (socklen_t)sizeof(g_peer_addr));
    }
    g_peer_len = address_len;

    /* * Connect the UDP socket to the peer address.
     * This simplifies future send/recv calls (don't need sendto/recvfrom anymore).
     */
    if (connect(socket->sd, (struct sockaddr*)&g_peer_addr, g_peer_len) < 0) {
        perror("microtcp_connect: UDP connect failed");
        return -1;
    }

    /* State Transition: Connection Established */
    socket->state = ESTABLISHED;

    /* Allocate Receive Buffer */
    socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN);
    if (!socket->recvbuf) {
        perror("microtcp_connect: malloc recvbuf");
        socket->state = INVALID;
        return -1;
    }
    socket->buf_fill_level = 0;

    socket->curr_win_size = MICROTCP_WIN_SIZE;  // Initial Flow Control window
    socket->cwnd = MICROTCP_INIT_CWND;
    socket->ssthresh = socket->curr_win_size; /* per Phase B: initial ssthresh = flow-control window */       // Initial threshold

    return 0;
}


/**
 * SERVER SIDE: Accepts a new connection.
 * 1. Receives SYN
 * 2. Sends SYN + ACK
 * 3. Receives ACK
 */
int microtcp_accept (microtcp_sock_t *socket, struct sockaddr *address, socklen_t address_len)
{
    if (!socket || socket->state != LISTEN)
        return -1;

    
    /* --- STEP 1: Receive SYN packet --- */
    microtcp_header_t syn_hdr;
    socklen_t from_len = address_len;

    /* Blocking wait for SYN */
    if (recvfrom(socket->sd, &syn_hdr, sizeof(syn_hdr), 0, address, &from_len) < 0) {
        perror("microtcp_accept: recv SYN failed");
        return -1;
    }

    /* Store peer advertised window for flow control */
    socket->curr_win_size = (size_t)syn_hdr.window;
    socket->init_win_size = socket->curr_win_size;
    socket->ssthresh = socket->curr_win_size;
    socket->cwnd = MICROTCP_INIT_CWND;

    
    /* Verify Checksum */
    uint32_t orig = syn_hdr.checksum;
    syn_hdr.checksum = 0;
    uint32_t calc = crc32((uint8_t*)&syn_hdr, sizeof(syn_hdr));

    if (calc != orig) {
        fprintf(stderr, "microtcp_accept: SYN checksum mismatch\n");
        return -1;
    }

    /* Verify SYN Flag */
    if (!(syn_hdr.control & MICROTCP_FLAG_SYN)) {
        fprintf(stderr, "microtcp_accept: received non-SYN packet\n");
        return -1;
    }

    /* Store Client's Sequence Number. Next expected byte is Seq + 1 */
    socket->ack_number = (size_t)syn_hdr.seq_number + 1;

    
    /* --- STEP 2: Send SYN+ACK packet --- */
    microtcp_header_t synack_hdr;
    memset(&synack_hdr, 0, sizeof(synack_hdr));

    /* Initialize Server's Sequence Number with a random value */
    socket->seq_number = (size_t)rand();

    /* Prepare Header */
    synack_hdr.seq_number = (uint32_t)socket->seq_number;
    synack_hdr.ack_number = (uint32_t)socket->ack_number; /* Acknowledging client's SYN */
    synack_hdr.control    = MICROTCP_FLAG_SYN | MICROTCP_FLAG_ACK; /* Set both flags */
    synack_hdr.window     = (uint16_t) microtcp_adv_window(socket);
    synack_hdr.data_len   = 0;

    /* Calculate Checksum */
    synack_hdr.checksum = 0;
    synack_hdr.checksum = crc32((uint8_t*)&synack_hdr, sizeof(synack_hdr));

    /* Send SYN+ACK */
    if (sendto(socket->sd, &synack_hdr, sizeof(synack_hdr), 0, address, from_len) < 0) {
        perror("microtcp_accept: send SYN+ACK failed");
        return -1;
    }

    
    /* --- STEP 3: Receive Final ACK --- */
    microtcp_header_t ack_hdr;
    if (recvfrom(socket->sd, &ack_hdr, sizeof(ack_hdr), 0, address, &from_len) < 0) {
        perror("microtcp_accept: recv final ACK failed");
        return -1;
    }

    /* Verify Checksum */
    orig = ack_hdr.checksum;
    ack_hdr.checksum = 0;
    calc = crc32((uint8_t*)&ack_hdr, sizeof(ack_hdr));

    if (calc != orig) {
        fprintf(stderr, "microtcp_accept: ACK checksum mismatch\n");
        return -1;
    }

    /* Verify ACK Flag */
    if (!(ack_hdr.control & MICROTCP_FLAG_ACK)) {
        fprintf(stderr, "microtcp_accept: final ACK missing\n");
        return -1;
    }

    /* Verify ACK Number: Should be Server_Seq + 1 */
    if (ack_hdr.ack_number != (uint32_t)(socket->seq_number + 1)) {
        fprintf(stderr, "microtcp_accept: wrong final ACK number\n");
        return -1;
    }

    /* Increment Server Sequence Number (SYN+ACK consumes 1 seq) */
    socket->seq_number++;

    /* Store Peer Address globally */
    memset(&g_peer_addr, 0, sizeof(g_peer_addr));
    if (from_len < (socklen_t)sizeof(g_peer_addr)) {
      memcpy(&g_peer_addr, address, from_len);
    } else {
      memcpy(&g_peer_addr, address, (socklen_t)sizeof(g_peer_addr));
    }
    g_peer_len = from_len;

    /* Connect UDP socket to the client */
    if (connect(socket->sd, (struct sockaddr*)&g_peer_addr, g_peer_len) < 0) {
        perror("microtcp_accept: UDP connect failed");
        return -1;
    }

    /* State Transition: Connection Established */
    socket->state = ESTABLISHED;

    /* Allocate Receive Buffer */
    socket->recvbuf = malloc(MICROTCP_RECVBUF_LEN);
    if (!socket->recvbuf) {
        perror("microtcp_accept: malloc recvbuf");
        socket->state = INVALID;
        return -1;
    }
    socket->buf_fill_level = 0;

    return 0;
}


/**
 * Initiates connection termination.
 * NOTE: Currently implements a simplified termination.
 * Real TCP uses a 4-way handshake (FIN -> ACK -> FIN -> ACK).
 */
int microtcp_shutdown (microtcp_sock_t *socket, int how)
{
    (void)how; 

    if (!socket){
        return -1;
    }
        
    microtcp_header_t hdr;
    struct sockaddr_in remote_addr; // Could be cached in the socket
    socklen_t addr_len = sizeof(remote_addr); 
    // Note: If we've called connect() on the UDP socket, recv/send works 
    // without needing remote_addr each time, but here for completeness:

    /* ==========================================================
       SCENARIO 1: Active Close (We initiate the termination)
       ========================================================== */
    if (socket->state == ESTABLISHED) {
        
        /* 1. Send FIN */
        memset(&hdr, 0, sizeof(hdr));
        hdr.seq_number = (uint32_t)socket->seq_number;
        hdr.ack_number = (uint32_t)socket->ack_number;
        hdr.control    = MICROTCP_FLAG_FIN | MICROTCP_FLAG_ACK;
        hdr.window     = (uint16_t) microtcp_adv_window(socket);
        
        hdr.checksum = 0;
        hdr.checksum = crc32((uint8_t*)&hdr, sizeof(hdr));
        
        if (send(socket->sd, &hdr, sizeof(hdr), 0) < 0) {
            perror("shutdown: send FIN failed");
        }

        /* 2. Wait for ACK */
        // Here we should normally have timeout/retransmission like in send
        // For simplicity, we wait blocking for the ACK.
        
        microtcp_header_t recv_hdr;
        while (1) {
            if (recv(socket->sd, &recv_hdr, sizeof(recv_hdr), 0) < 0) break;
            
            // Checksum check...
            uint32_t rec_sum = recv_hdr.checksum;
            recv_hdr.checksum = 0;
            if (crc32((uint8_t*)&recv_hdr, sizeof(recv_hdr)) != rec_sum) continue;

            if (recv_hdr.control & MICROTCP_FLAG_ACK) {
                // We received the ACK for our FIN.
                socket->state = CLOSING_BY_HOST; // [cite: 126]
                break;
            }
        }

        /* 3. Wait for Peer's FIN */
        while (1) {
            if (recv(socket->sd, &recv_hdr, sizeof(recv_hdr), 0) < 0) break;
            
            uint32_t rec_sum = recv_hdr.checksum;
            recv_hdr.checksum = 0;
            if (crc32((uint8_t*)&recv_hdr, sizeof(recv_hdr)) != rec_sum) continue;

            if (recv_hdr.control & MICROTCP_FLAG_FIN) {
                // We received the FIN from the peer.
                socket->ack_number = (size_t)recv_hdr.seq_number + 1;
                break;
            }
        }

        /* 4. Send Final ACK */
        memset(&hdr, 0, sizeof(hdr));
        hdr.seq_number = (uint32_t)socket->seq_number + 1; // FIN consumes sequence
        hdr.ack_number = (uint32_t)socket->ack_number;
        hdr.control    = MICROTCP_FLAG_ACK;
        
        hdr.checksum = 0;
        hdr.checksum = crc32((uint8_t*)&hdr, sizeof(hdr));
        
        send(socket->sd, &hdr, sizeof(hdr), 0);
    }

    /* ==========================================================
       SCENARIO 2: Passive Close (The other side closed first)
       ========================================================== */
    else if (socket->state == CLOSING_BY_PEER) {
        // We already received FIN and sent ACK inside microtcp_recv.
        // Now we need to send our own FIN.

        /* 1. Send FIN */
        memset(&hdr, 0, sizeof(hdr));
        hdr.seq_number = (uint32_t)socket->seq_number;
        hdr.ack_number = (uint32_t)socket->ack_number;
        hdr.control    = MICROTCP_FLAG_FIN | MICROTCP_FLAG_ACK;
        hdr.window     = (uint16_t) microtcp_adv_window(socket);
        
        hdr.checksum = 0;
        hdr.checksum = crc32((uint8_t*)&hdr, sizeof(hdr));
        
        send(socket->sd, &hdr, sizeof(hdr), 0);

        /* 2. Wait for Final ACK */
        microtcp_header_t recv_hdr;
        while (1) {
            if (recv(socket->sd, &recv_hdr, sizeof(recv_hdr), 0) < 0) break;
            
            uint32_t rec_sum = recv_hdr.checksum;
            recv_hdr.checksum = 0;
            if (crc32((uint8_t*)&recv_hdr, sizeof(recv_hdr)) != rec_sum) continue;

            if (recv_hdr.control & MICROTCP_FLAG_ACK) {
                break; // Done
            }
        }
    }

    /* Free Resources & Close Socket */
    if (socket->recvbuf) {
        free(socket->recvbuf);
        socket->recvbuf = NULL;
    }

    if (socket->sd >= 0) {
        close(socket->sd);
        socket->sd = -1;
    }

    socket->state = CLOSED;
    return 0;
}


ssize_t microtcp_send (microtcp_sock_t *socket, const void *buffer, size_t length, int flags)
{
    (void) flags; // Flags not used

    if (!socket || socket->state != ESTABLISHED) {
        return -1;
    }

    size_t total_sent = 0;
    size_t remaining = length;
    const uint8_t *data_ptr = (const uint8_t *)buffer;

    /* Set Timeout for recvfrom (RTO)*/
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = MICROTCP_ACK_TIMEOUT_US;
    if (setsockopt(socket->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("microtcp_send: setsockopt failed");
        return -1;
    }

    /* Main loop: Send until all bytes are gone */
    while (remaining > 0) {

        /* 1. Calculate Sending Window  */
        size_t flow_win = socket->curr_win_size;
        size_t cong_win = socket->cwnd;
        size_t window = MIN(flow_win, cong_win);

        /* If window is 0, wait */
        if (window == 0) {
            usleep(rand() % MICROTCP_ACK_TIMEOUT_US); // [cite: 263]
            /* Try to receive an ACK that might open the window */
             microtcp_header_t ack_probe;
             socklen_t addr_len = 0;
             // Non-blocking or small timeout receive 
             // In the simple implementation, retry on the next loop
             continue; 
        }

        /* 2. Calculate Bytes to send in this cycle */
        size_t bytes_to_send = MIN(window, remaining);
        
        /* Calculate number of chunks (packets) */
        int chunks = bytes_to_send / MICROTCP_MSS;
        if (bytes_to_send % MICROTCP_MSS != 0) {
            chunks++;
        }

        /* Save initial position in case of Retransmission */
        size_t seq_start_of_burst = socket->seq_number;
        const uint8_t *data_start_of_burst = data_ptr;

        /* 3. Sending Loop */
        size_t bytes_in_burst = 0;
        for (int i = 0; i < chunks; ++i) {
            size_t payload_len = MIN(MICROTCP_MSS, bytes_to_send - bytes_in_burst);
            
            /* Header */
            microtcp_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.seq_number = (uint32_t)socket->seq_number;
            hdr.ack_number = (uint32_t)socket->ack_number;
            hdr.control    = MICROTCP_FLAG_ACK; // All packets after handshake have ACK
            hdr.window     = (uint16_t) microtcp_adv_window(socket); // Flow control update
            hdr.data_len   = (uint32_t)payload_len;
            
            /* Checksum */
            size_t packet_size = sizeof(hdr) + payload_len;
            uint8_t *packet = malloc(packet_size);
            if (!packet) return -1;

            memcpy(packet, &hdr, sizeof(hdr));
            memcpy(packet + sizeof(hdr), data_ptr + bytes_in_burst, payload_len);
            
            ((microtcp_header_t*)packet)->checksum = 0;
            ((microtcp_header_t*)packet)->checksum = crc32(packet, packet_size);

            /* Sending */
            if (send(socket->sd, packet, packet_size, 0) < 0) {
                perror("microtcp_send: send error");
                free(packet);
                return -1;
            }
            free(packet);

            /* Update local variables for next chunk */
            socket->seq_number += payload_len;
            socket->packets_send++;
            socket->bytes_send += payload_len;
            
            bytes_in_burst += payload_len;
        }

        /* 4. ACK Wait Loop (Wait for ACKs)  */
        /* must receive confirmation for all chunks sent */
        size_t acked_bytes_in_burst = 0;
        int retransmit_needed = 0;

        while (acked_bytes_in_burst < bytes_in_burst) {
            microtcp_header_t ack_hdr;
            
            /* Blocks until the timeout we set with setsockopt */
            ssize_t res = recv(socket->sd, &ack_hdr, sizeof(ack_hdr), 0);

            if (res < 0) {
                /* TIMEOUT DETECTED*/
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Congestion Control: Timeout handling
                    socket->ssthresh = socket->cwnd / 2;
                    socket->cwnd = MIN(MICROTCP_MSS, socket->ssthresh);
                    
                    retransmit_needed = 1; 
                    break; // Exit wait loop to retransmit
                } else {
                    perror("microtcp_send: recv error");
                    return -1;
                }
            }

            /* Checksum verification */
            uint32_t received_sum = ack_hdr.checksum;
            ack_hdr.checksum = 0;
            if (crc32((uint8_t*)&ack_hdr, sizeof(ack_hdr)) != received_sum) {
                // Corrupted ACK, ignore
                continue;
            }

            /* Duplicate ACK check */
            /* If the ACK number is old */
            if (ack_hdr.ack_number == (uint32_t)seq_start_of_burst + acked_bytes_in_burst) {
                socket->dup_ack_count++;
                if (socket->dup_ack_count >= 3) {
                    /* FAST RETRANSMIT [cite: 287-290] */
                    socket->ssthresh = socket->cwnd / 2;
                    socket->cwnd = socket->ssthresh + MICROTCP_MSS;
                    
                    socket->dup_ack_count = 0; // Reset
                    retransmit_needed = 1;
                    break;
                }
            } else if (ack_hdr.ack_number > (uint32_t)seq_start_of_burst) {
                /* NEW ACK (Valid) */
                socket->dup_ack_count = 0; // Reset duplicates
                
                size_t new_bytes_acked = ack_hdr.ack_number - (seq_start_of_burst + acked_bytes_in_burst);
                acked_bytes_in_burst += new_bytes_acked;

                /* Congestion Control Updates */
                if (socket->cwnd < socket->ssthresh) {
                    // Slow Start: Add MSS for every ACK
                    socket->cwnd += MICROTCP_MSS; 
                } else {
                    // Congestion Avoidance: Add (MSS * MSS) / cwnd
                    // increase by ~1 MSS per RTT. Here per ACK:
                    socket->cwnd += (MICROTCP_MSS * MICROTCP_MSS) / socket->cwnd;
                }
                
                /* Update Peer Window */
                socket->curr_win_size = ack_hdr.window;
            }
        }

        /* Retransmission handling */
        if (retransmit_needed) {
            /* Go-Back-N Logic: Return variables to the start of the burst */
            socket->seq_number = seq_start_of_burst;
            // Don't advance data_ptr and remaining
            // The loop will re-run from the beginning with new cwnd/ssthresh values
            continue; 
        }

        /* Successful burst transmission */
        data_ptr += bytes_in_burst;
        remaining -= bytes_in_burst;
        total_sent += bytes_in_burst;
    }

    return total_sent;
}


ssize_t microtcp_recv (microtcp_sock_t *socket, void *buffer, size_t length, int flags)
{
    (void) flags;

    if (!socket || socket->state == CLOSED || socket->state == INVALID || !buffer || length == 0)
        return -1;

    /* If already have buffered data, return it immediately. */
    if (socket->buf_fill_level > 0) {
        size_t to_deliver = (socket->buf_fill_level < length) ? socket->buf_fill_level : length;
        memcpy(buffer, socket->recvbuf, to_deliver);
        memmove(socket->recvbuf, socket->recvbuf + to_deliver, socket->buf_fill_level - to_deliver);
        socket->buf_fill_level -= to_deliver;

        /* Update advertised window */
        socket->curr_win_size = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);
        return (ssize_t)to_deliver;
    }

    uint8_t packet_buf[sizeof(microtcp_header_t) + MICROTCP_MSS];
    microtcp_header_t hdr;
    ssize_t received;
    struct sockaddr_in remote_addr;
    socklen_t addr_len = sizeof(remote_addr);

    while (1) {
        received = recvfrom(socket->sd, packet_buf, sizeof(packet_buf), 0,
                            (struct sockaddr *)&remote_addr, &addr_len);

        if (received < 0) {
            perror("microtcp_recv: recv failed");
            return -1;
        }
        if ((size_t)received < sizeof(microtcp_header_t)) {
            /* Ignore undersized packets */
            continue;
        }

        memcpy(&hdr, packet_buf, sizeof(hdr));

        /* Validate checksum (header + payload) */
        uint32_t received_sum = hdr.checksum;
        ((microtcp_header_t*)packet_buf)->checksum = 0;
        uint32_t calc_sum = crc32(packet_buf, (size_t)received);
        if (calc_sum != received_sum) {
#ifdef MICROTCP_DEBUG
            fprintf(stderr, "[DEBUG] Dropping due to checksum error\n");
#endif
            /* Corrupted segment: send duplicate ACK (same ack_number) */
            goto send_dup_ack;
        }

        /* FIN handling: ACK it and return -1 while setting proper state */
        if (hdr.control & MICROTCP_FLAG_FIN) {
#ifdef MICROTCP_DEBUG
            fprintf(stderr, "[DEBUG] Received FIN\n");
#endif
            socket->state = CLOSING_BY_PEER;
            socket->ack_number = (size_t)hdr.seq_number + 1;
            goto send_ack_fin;
        }

        /* In-order delivery only: if out-of-order, send dup ACK */
        if (hdr.seq_number != (uint32_t)socket->ack_number) {
#ifdef MICROTCP_DEBUG
            fprintf(stderr, "[DEBUG] Out of order! Got %u, expected %lu\n",
                    hdr.seq_number, (unsigned long)socket->ack_number);
#endif
            goto send_dup_ack;
        }

        /* Determine payload length */
        size_t data_len = hdr.data_len;
        if (data_len > (size_t)received - sizeof(microtcp_header_t))
            data_len = (size_t)received - sizeof(microtcp_header_t);

        /* Flow control: accept only if space in internal buffer */
        size_t avail_win = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);
        if (data_len > avail_win) {
            /* Receiver buffer full: advertise window=0 and dupACK */
            goto send_dup_ack;
        }

        if (data_len > 0) {
            memcpy(socket->recvbuf + socket->buf_fill_level, packet_buf + sizeof(microtcp_header_t), data_len);
            socket->buf_fill_level += data_len;

            socket->ack_number += data_len;
            socket->packets_received++;
            socket->bytes_received += data_len;
        }

        /* Update advertised window and send ACK */
        socket->curr_win_size = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);

        microtcp_header_t ack_hdr;
        memset(&ack_hdr, 0, sizeof(ack_hdr));
        ack_hdr.seq_number = (uint32_t)socket->seq_number;
        ack_hdr.ack_number = (uint32_t)socket->ack_number;
        ack_hdr.control    = MICROTCP_FLAG_ACK;
        ack_hdr.window     = (uint16_t) microtcp_adv_window(socket);
        ack_hdr.checksum   = 0;
        ack_hdr.checksum   = crc32((uint8_t*)&ack_hdr, sizeof(ack_hdr));
        sendto(socket->sd, &ack_hdr, sizeof(ack_hdr), 0, (struct sockaddr *)&remote_addr, addr_len);

        /* Deliver up to requested length from internal buffer */
        if (socket->buf_fill_level > 0) {
            size_t to_deliver = (socket->buf_fill_level < length) ? socket->buf_fill_level : length;
            memcpy(buffer, socket->recvbuf, to_deliver);
            memmove(socket->recvbuf, socket->recvbuf + to_deliver, socket->buf_fill_level - to_deliver);
            socket->buf_fill_level -= to_deliver;

            socket->curr_win_size = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);
            return (ssize_t)to_deliver;
        }
        return 0;

send_ack_fin:
        /* ACK FIN */
        socket->curr_win_size = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);
        microtcp_header_t fin_ack;
        memset(&fin_ack, 0, sizeof(fin_ack));
        fin_ack.seq_number = (uint32_t)socket->seq_number;
        fin_ack.ack_number = (uint32_t)socket->ack_number;
        fin_ack.control    = MICROTCP_FLAG_ACK;
        fin_ack.window     = (uint16_t) microtcp_adv_window(socket);
        fin_ack.checksum   = 0;
        fin_ack.checksum   = crc32((uint8_t*)&fin_ack, sizeof(fin_ack));
        sendto(socket->sd, &fin_ack, sizeof(fin_ack), 0, (struct sockaddr *)&remote_addr, addr_len);
        return -1;

send_dup_ack:
        /* Duplicate ACK for out-of-order/corrupt/zero-window scenarios */
        socket->curr_win_size = (socket->buf_fill_level >= MICROTCP_WIN_SIZE) ? 0 : (MICROTCP_WIN_SIZE - socket->buf_fill_level);
        microtcp_header_t dup_hdr;
        memset(&dup_hdr, 0, sizeof(dup_hdr));
        dup_hdr.seq_number = (uint32_t)socket->seq_number;
        dup_hdr.ack_number = (uint32_t)socket->ack_number;
        dup_hdr.control    = MICROTCP_FLAG_ACK;
        dup_hdr.window     = (uint16_t) microtcp_adv_window(socket);
        dup_hdr.checksum   = 0;
        dup_hdr.checksum   = crc32((uint8_t*)&dup_hdr, sizeof(dup_hdr));
        sendto(socket->sd, &dup_hdr, sizeof(dup_hdr), 0, (struct sockaddr *)&remote_addr, addr_len);
        /* keep waiting for an in-order segment */
    }
}
