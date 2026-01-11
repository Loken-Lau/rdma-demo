# RDMA Echo/Write Demo

A minimal example demonstrating **RDMA WRITE** operations using `libibverbs`.
Designed to work with **Soft-RoCE (RXE)** drivers for emulation on standard Ethernet.

## Features
- **Kernel Bypass**: Direct user-space hardware access.
- **Zero Copy**: DMA data transfer directly to remote memory.
- **Out-of-Band Handshake**: Manual exchange of QP info (LID, GID, QPN, RKEY).

## How to Run
1. Setup Soft-RoCE (rxe0) on Linux.
2. Compile: `gcc -o rdma_demo rdma_demo.c -libverbs`
3. Server: `sudo ./rdma_demo server`
4. Client: `sudo ./rdma_demo client`
5. Copy-paste the exchange info.