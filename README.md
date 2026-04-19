# microTCP - Reliable Transport Protocol over UDP

This project implements **microTCP**, a parameterizable and lightweight transport protocol library built on top of UDP in C. It is designed to provide reliable data transfer for resource-constrained IoT devices where a full TCP stack is computationally expensive. The library replicates core TCP mechanisms such as connection management, flow control, and congestion avoidance in user-space.

## Architecture Overview

* **microtcp.c / microtcp.h**: The core implementation of the protocol. It manages the socket states (from CLOSED to ESTABLISHED), implements the 3-way handshake, and handles the reliable transmission/reception logic including ACKs and sequence numbering.
* **Slave Interface Logic**: Provided through `microtcp_send()` and `microtcp_recv()` functions, which abstract the underlying UDP complexity and provide a TCP-like API to the application layer.
* **Retransmission & Recovery**: Implements a timeout-based retransmission mechanism alongside a **Fast Retransmit** algorithm (triggered by 3 duplicate ACKs) to maximize throughput in lossy network conditions.
* **Control Mechanisms**: Includes a sliding window algorithm for **Flow Control** and dynamic window adjustment (**Slow Start & Congestion Avoidance**) for robust **Congestion Control**.
* **Integrity Layer**: Uses a CRC32 checksum (implemented in `crc32.h`) to ensure data integrity across the network.

## Features

* **Reliable Data Transfer**: Guaranteed and in-order packet delivery using sequence and acknowledgment numbers.
* **Connection Management**: Robust 3-way handshake for connection setup and graceful teardown.
* **Advanced Congestion Control**: Dynamic `cwnd` and `ssthresh` management to prevent network collapse.
* **Throughput Benchmarking**: Includes tools to compare performance against standard kernel-level TCP.
* **Data Integrity**: Integrated CRC32 verification and MD5 validation support.

## File List

* **`microtcp.c` / `microtcp.h`** – Core protocol logic and API definitions.
* **`bandwidth_test.c`** – Benchmarking tool for performance measurement.
* **`traffic_generator.cpp`** – Load testing utility using Poisson distribution for packet arrivals.
* **`test_microtcp_client.c` / `test_microtcp_server.c`** – Reference implementations of a microTCP-based application.
* **`crc32.h` / `log.h`** – Utility headers for checksum calculation and system logging.
* **`CMakeLists.txt`** – Build configuration file.

## Usage

### Build
To compile the library and the included test utilities, use the CMake build system:
```bash
mkdir build
cd build
cmake ..
make

# Run as server
./bandwidth_test -m -s -p [port] -f [output_file]

# Run as client
./bandwidth_test -m -p [port] -a [server_ip] -f [input_file]
