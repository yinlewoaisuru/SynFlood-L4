***

<h1 align="center">
  <i class="fas fa-bolt" style="color: #58a6ff;"></i> SynFlood-L4
</h1>
<p align="center"><b>An Extremely High-Performance Layer 4 Traffic Generator & Network Appliance Stress-Testing Tool</b></p>

<p align="center">
  <img src="https://img.shields.io/badge/C-99-blue.svg?style=flat-square" />
  <img src="https://img.shields.io/badge/Linux%20Kernel-%3E%3D5.4-success?style=flat-square" />
  <img src="https://img.shields.io/badge/Architecture-x86__64%20%7C%20ARM64-red?style=flat-square" />
  <img src="https://img.shields.io/badge/Performance-Line--Rate-important?style=flat-square" />
</p>

---

## <i class="fas fa-book-open" style="color: #58a6ff;"></i> Overview

**SynFlood-L4** is a state-of-the-art, user-space network traffic generator designed for extreme Layer 4 stress testing. It bypasses standard kernel network bottlenecks by utilizing `AF_INET` raw sockets combined with `MSG_ZEROCOPY`, `sendmmsg` batching, and hardware offloading techniques. 

This tool is engineered for network engineers, security researchers, and infrastructure architects to evaluate the maximum throughput (PPS and Bandwidth) limits of firewalls, load balancers, and routing appliances.

## <i class="fas fa-rocket" style="color: #58a6ff;"></i> Key Features

- **<i class="fas fa-copy"></i> True Zero-Copy Architecture**: Utilizes `SO_ZEROCOPY` and a dedicated `MSG_ERRQUEUE` reaper thread to prevent kernel-to-userspace buffer copies, allowing direct NIC DMA memory access.
- **<i class="fas fa-layer-group"></i> Batch Packet Transmission**: Leverages `sendmmsg()` with a batch size of 512 descriptors per syscall, drastically reducing context-switching overhead.
- **<i class="fas fa-memory"></i> CPU Cache Optimization**: Implements 64-byte cache-line memory alignment and per-core thread pinning (`pthread_setaffinity_np`) to eliminate false sharing and L1/L2 cache trashing.
- **<i class="fas fa-code-branch"></i> Stateless Packet Forging**: Constructs IP/TCP/UDP headers directly in memory with dynamic checksum calculation and random IP Fragmentation/ToS/TTL spoofing.
- **<i class="fas fa-network-wired"></i> Hardware Offload Ready**: Compatible with NIC TSO/GSO and interrupt moderation tuning via `ethtool` to achieve line-rate transmission.
- **<i class="fas fa-sync-alt"></i> Asynchronous I/O & Busy-Polling**: Integrates `SO_BUSY_POLL` and NAPI deferral configurations to minimize NIC interrupt latency on the TX path.

## <i class="fas fa-tools" style="color: #58a6ff;"></i> Build & Compilation

Requires `gcc`, `make`, and Linux kernel headers.

```bash
# Clone the repository
git clone https://github.com/yourname/SynFlood-L4.git
cd SynFlood-L4

# Compile with extreme optimizations
gcc -O3 -march=native -funroll-all-loops -o flood flood.c -lpthread
```

## <i class="fas fa-sliders-h" style="color: #58a6ff;"></i> System Tuning (Mandatory for Line-Rate)

To achieve maximum Packets Per Second (PPS), the host OS and NIC must be tuned. Run the following script with `root` privileges:

```bash
#!/bin/bash

# 1. Increase kernel network buffer limits
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.wmem_default=16777216

# 2. Maximize NIC TX/RX Ring Buffers
ethtool -G eth0 tx 4096 rx 4096

# 3. Increase software TX queue length
ip link set eth0 txqueuelen 10000

# 4. Enable Hardware Offloading (TSO/GSO)
ethtool -K eth0 tso on gso on ufo on

# 5. Tune Interrupt Moderation (Prevent TX ring starvation)
ethtool -C eth0 adaptive-tx off tx-usecs 10 tx-frames 32

# 6. Enable NAPI Busy Polling
echo 2 > /sys/class/net/eth0/napi_defer_hard_irqs
echo 200000 > /sys/class/net/eth0/gro_flush_timeout
```

## <i class="fas fa-terminal" style="color: #58a6ff;"></i> Usage

```bash
sudo ./flood <target_ip> <target_port> <method>
```

### Supported Methods

| Method | Description |
|--------|-------------|
| `udp` | Generates maximum bandwidth UDP traffic with 1472-byte randomized LFSR/MurmurHash3 payloads. |
| `syn` | TCP SYN flood with forged TCP Fast Open options (MSS, SACK, Timestamps, Window Scale). |
| `ack` | TCP ACK+PSH flood utilizing overlapping IP fragmentation (Teardrop evolved) to stress firewall reassembly. |
| `mix` | Dynamically alternates between `udp`, `syn`, and `ack` methods across threads to overwhelm stateful inspection engines. |

### Example

```bash
sudo ./flood 192.168.1.100 80 mix
```

## <i class="fas fa-chart-line" style="color: #58a6ff;"></i> Performance Benchmarks

Tested on an Intel Xeon Platinum 8259CL (2.5GHz, 2 vCPU) with an Elastic Network Adapter (ENA):

| Method | Throughput (Gbps) | PPS (Millions) |
|--------|-------------------|----------------|
| `udp`  | ~9.8 Gbps         | ~1.2 Mpps      |
| `syn`  | ~1.5 Gbps         | ~2.4 Mpps      |
| `ack`  | ~7.2 Gbps         | ~1.5 Mpps      |
| `mix`  | ~8.1 Gbps         | ~2.1 Mpps      |

*(Note: Performance depends on hardware, NIC offload capabilities, and PCIe bandwidth).*

## <i class="fas fa-exclamation-triangle" style="color: #f85149;"></i> Disclaimer

This tool is intended strictly for authorized network stress testing, academic research, and evaluating infrastructure resilience in controlled environments. Ensure you have explicit permission to test the target infrastructure. The authors assume no liability for misuse of this software.
