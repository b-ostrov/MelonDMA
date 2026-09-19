/* mlx_lat — two-machine RDMA WRITE and READ latency, measured the way
 * perftest's ib_write_lat and ib_read_lat measure it.
 *
 * One source builds against stock libibverbs on the Linux peer and against
 * MelonDMA's compatibility layer on the Mac, so both ends of every cell run the
 * same measurement code. The active side posts one operation, busy-polls its
 * completion, and times the span from just before the post to just after the
 * completion is read. The passive side registers a buffer, hands over its
 * address and key, and then sits in a blocking socket read for the whole run:
 * a one-sided operation is served entirely by the passive card and never
 * reaches the passive host's software.
 *
 * That decides which driver a cell measures. In "A -> B WRITE" the software on
 * B is idle, so the figure is A's posting and completion path, both cards and
 * the wire. Only the initiator's stack is being compared.
 *
 * With -t the active side measures goodput instead: -q operations stay
 * outstanding, each on its own slot of the buffer, and the payload completed
 * inside a -t second window after a one-second warm-up is reported. The whole
 * buffer is checked afterwards, on the passive side for WRITE and on the active
 * side for READ, so give both ends the same -s, -q and -t.
 *
 * The handshake is the tree's existing text destination and remote-memory wire
 * format (see mlx_imm_peer.c), so it interoperates with nothing new.
 *
 * The control connection's direction is independent of the RDMA roles: a
 * host argument means connect, none means listen. That lets the Mac connect
 * out in every cell, whichever side initiates, so its firewall never has to
 * admit an inbound connection.
 *
 * Passive: mlx_lat -S [-d dev] [-i port] [-g gid] [-p tcp] [-s size] [-m mtu] [host]
 * Active:  mlx_lat -o write|read [-I] [-R] [-n iters] [same options] [host]
 * Goodput: add -t seconds [-q depth] on both ends
 * Mac:     -l local-ip -a local-mac -r remote-mac   (programs the GID slot)
 * -R registers with PCIe relaxed ordering (Linux only).
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WARMUP 1000
#define GOODPUT_WARMUP_NS 1000000000ull
#define DEST_TEMPLATE "0000:000000:000000:00000000000000000000000000000000"

struct destination { uint32_t qpn, psn; union ibv_gid gid; };
struct remote_memory_wire { uint64_t address_be; uint32_t rkey_be, length_be; };
struct goodput_result { uint64_t ops, posts; };

static uint64_t now_ns(void)
{
#ifdef __APPLE__
    /* CLOCK_MONOTONIC quantises to whole microseconds on macOS. */
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static int full_read(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int full_write(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static void gid_wire(const union ibv_gid *gid, char out[33])
{
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", gid->raw[i]);
}

static int wire_gid(const char *in, union ibv_gid *gid)
{
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(in + i * 2, "%2x", &v) != 1) return -1;
        gid->raw[i] = (uint8_t)v;
    }
    return 0;
}

static uint64_t be64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

static enum ibv_mtu path_mtu_of(uint32_t n)
{
    switch (n) {
    case 256: return IBV_MTU_256;   case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096; default: return 0;
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Nearest-rank percentile on a sorted array, in microseconds. */
static double pct_us(const uint64_t *s, long n, double p)
{
    long i = (long)(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return (double)s[i] / 1000.0;
}

/* What a READ fetches; the passive side lays it down before the run. */
static uint8_t read_pattern(size_t k) { return (uint8_t)(k * 13 + 7); }

/* What a goodput WRITE lays down. It differs from the READ pattern, which the
 * passive buffer starts out holding, so a slot the run never reached fails. */
static uint8_t write_pattern(size_t k) { return (uint8_t)(k * 31 + 11); }

static int connect_peer(const char *host, uint16_t port)
{
    char service[8];
    snprintf(service, sizeof service, "%u", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, service, &hints, &res)) return -1;
    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        for (struct addrinfo *it = res; it; it = it->ai_next) {
            fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
            if (fd >= 0) { close(fd); fd = -1; }
        }
        if (fd < 0) usleep(100 * 1000);   /* the server may still be starting */
    }
    freeaddrinfo(res);
    return fd;
}

static int listen_once(uint16_t port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (lfd < 0) return -1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 1)) { close(lfd); return -1; }
    int fd = accept(lfd, NULL, NULL);
    close(lfd);
    return fd;
}

static int post_slot(struct ibv_qp *qp, struct ibv_sge *slots, uint32_t k, int is_read,
                     uint64_t remote_addr, uint32_t rkey)
{
    struct ibv_send_wr wr = { .wr_id = (uint64_t)k + 1, .sg_list = &slots[k], .num_sge = 1,
        .opcode = is_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED };
    /* Slot k of the local buffer pairs with slot k of the remote one. */
    wr.wr.rdma.remote_addr = remote_addr + (slots[k].addr - slots[0].addr);
    wr.wr.rdma.rkey = rkey;
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(qp, &wr, &bad);
}

/* Keep one operation outstanding on every slot and count those that complete
 * inside the window. Each completion reposts on its own slot until the window
 * closes; then the queue drains. */
static int run_goodput(struct ibv_qp *qp, struct ibv_cq *cq, struct ibv_sge *slots,
                       uint32_t depth, int is_read, uint64_t remote_addr, uint32_t rkey,
                       double seconds, struct goodput_result *res)
{
    const uint64_t window_open = now_ns() + GOODPUT_WARMUP_NS;
    const uint64_t window_close = window_open + (uint64_t)(seconds * 1e9);
    struct ibv_wc wcs[64];
    uint32_t outstanding = 0;
    *res = (struct goodput_result){ 0 };
    for (uint32_t k = 0; k < depth; k++) {
        if (post_slot(qp, slots, k, is_read, remote_addr, rkey)) {
            fprintf(stderr, "post failed on slot %u\n", k);
            return -1;
        }
        outstanding++; res->posts++;
    }
    while (outstanding) {
        int n = ibv_poll_cq(cq, 64, wcs);
        if (n < 0) { fprintf(stderr, "poll failed\n"); return -1; }
        if (n == 0) continue;
        const uint64_t t = now_ns();
        for (int i = 0; i < n; i++) {
            const uint32_t k = (uint32_t)(wcs[i].wr_id - 1);
            if (wcs[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "completion error %d on slot %u\n", wcs[i].status, k);
                return -1;
            }
            outstanding--;
            if (t >= window_open && t < window_close) res->ops++;
            if (t >= window_close) continue;
            if (post_slot(qp, slots, k, is_read, remote_addr, rkey)) {
                fprintf(stderr, "post failed on slot %u\n", k);
                return -1;
            }
            outstanding++; res->posts++;
        }
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "passive: mlx_lat -S [-d dev] [-i port] [-g gid] [-p tcp] [-s size] [-m mtu] [host]\n"
        "active:  mlx_lat -o write|read [-I] [-R] [-n iters] [same options] [host]\n"
        "goodput: add -t seconds [-q depth] on both ends\n"
        "mac:    -l local-ip -a local-mac -r remote-mac\n");
}

int main(int argc, char **argv)
{
    const char *dev_name = NULL, *op_name = "write";
    const char *local_ip = "192.168.200.1", *local_mac = "98:03:9b:80:6a:94";
    const char *remote_mac = NULL;
    int server = 0, want_inline = 0, relaxed = 0, ib_port = 1, gid_index = 0, opt;
    uint16_t tcp_port = 19700;
    uint32_t size = 64, mtu_bytes = 4096, depth = 1;
    long iters = 20000;
    double seconds = 0;
    while ((opt = getopt(argc, argv, "SIRo:d:i:g:p:s:m:n:q:t:l:a:r:")) != -1) switch (opt) {
    case 'S': server = 1; break;
    case 'I': want_inline = 1; break;
    case 'R': relaxed = 1; break;
    case 'o': op_name = optarg; break;
    case 'd': dev_name = optarg; break;
    case 'i': ib_port = atoi(optarg); break;
    case 'g': gid_index = atoi(optarg); break;
    case 'p': tcp_port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 's': size = (uint32_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break;
    case 'n': iters = strtol(optarg, NULL, 0); break;
    case 'q': depth = (uint32_t)strtoul(optarg, NULL, 0); break;
    case 't': seconds = strtod(optarg, NULL); break;
    case 'l': local_ip = optarg; break;
    case 'a': local_mac = optarg; break;
    case 'r': remote_mac = optarg; break;
    default: usage(); return 2;
    }
    const char *host = optind < argc ? argv[optind] : NULL;
    const int is_read = !strcmp(op_name, "read");
    const int goodput = seconds > 0;
    if ((!is_read && strcmp(op_name, "write")) ||
        size < 1 || size > (1u << 20) || iters < 1 || !path_mtu_of(mtu_bytes) ||
        seconds < 0 || depth < 1 || depth > 256 || (!goodput && depth != 1) ||
        (uint64_t)size * depth > (64u << 20)) {
        usage();
        return 2;
    }
    (void)dev_name; (void)local_ip; (void)local_mac; (void)remote_mac; (void)want_inline; (void)relaxed;

    int rc = 1, count = 0, fd = -1;
    uint64_t *lat = NULL, *post = NULL, *waitns = NULL, *polls = NULL, *pollns = NULL;
    struct ibv_sge *slots = NULL;
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_device *device = NULL;
#ifdef __APPLE__
    device = (devices && count > 0) ? devices[0] : NULL;
#else
    for (int i = 0; devices && i < count; i++)
        if (!dev_name || !strcmp(ibv_get_device_name(devices[i]), dev_name)) {
            device = devices[i];
            break;
        }
#endif
    struct ibv_context *ctx = device ? ibv_open_device(device) : NULL;
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, depth * 2 < 64 ? 64 : (int)depth * 2,
                                            NULL, NULL, 0) : NULL;
    long page = sysconf(_SC_PAGESIZE);
    size_t buf_len = (size < 64 ? 64 : size) * (size_t)depth;
    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, (size_t)(page > 0 ? page : 4096), buf_len)) buf = NULL;
    if (buf) memset(buf, 0, buf_len);
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
#ifdef __linux__
    /* perftest registers with relaxed ordering where the device supports it,
     * which lets the card issue its DMA transactions out of order. */
    if (relaxed) access |= IBV_ACCESS_RELAXED_ORDERING;
#endif
    struct ibv_mr *mr = (pd && buf) ? ibv_reg_mr(pd, buf, buf_len, access) : NULL;
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        /* Ask for inline capacity only when inline is used. perftest creates
         * its queue pair with none, and a stock provider may lay the queue out
         * differently when inline capacity is requested. */
        .cap = { .max_send_wr = depth * 2 < 16 ? 16 : depth * 2, .max_recv_wr = 4,
                 .max_send_sge = 1, .max_recv_sge = 1,
                 .max_inline_data = want_inline ? 512 : 0 } };
    struct ibv_qp *qp = (pd && cq) ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !mr || !qp) { fprintf(stderr, "resources unavailable\n"); goto out; }

    union ibv_gid local_gid;
    uint8_t sgid = (uint8_t)gid_index;
#ifdef __APPLE__
    {
        /* The Mac's GID slot belongs to this client and must be programmed
         * with the link address; nothing else on macOS owns the port. */
        struct ibv_mlx5_roce_config roce = { .hop_limit = 1 };
        uint8_t ipv4[4] = {};
        if (!remote_mac) remote_mac = local_mac;   /* loopback */
        if (inet_pton(AF_INET, local_ip, ipv4) != 1 ||
            sscanf(local_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &roce.local_mac[0], &roce.local_mac[1], &roce.local_mac[2],
                   &roce.local_mac[3], &roce.local_mac[4], &roce.local_mac[5]) != 6 ||
            sscanf(remote_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &roce.peer_mac[0], &roce.peer_mac[1], &roce.peer_mac[2],
                   &roce.peer_mac[3], &roce.peer_mac[4], &roce.peer_mac[5]) != 6) {
            fprintf(stderr, "bad local ip or mac\n"); goto out;
        }
        memset(&roce.local_gid, 0, sizeof roce.local_gid);
        roce.local_gid.raw[10] = roce.local_gid.raw[11] = 0xff;
        memcpy(roce.local_gid.raw + 12, ipv4, sizeof ipv4);
        roce.l3_type = 0;
        if (ibv_mlx5_configure_roce(ctx, &roce)) { fprintf(stderr, "configure_roce failed\n"); goto out; }
        local_gid = roce.local_gid;
        struct ibv_gid_entry entries[64] = {};
        uint32_t n = 0, table = 0;
        int found = 0;
        if (!ibv_query_gid_table(ctx, (uint8_t)ib_port, entries, 64, &n, &table))
            for (uint32_t i = 0; i < n; i++)
                if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
                    !memcmp(entries[i].gid.raw, local_gid.raw, 16)) {
                    sgid = (uint8_t)entries[i].gid_index; found = 1; break;
                }
        if (!found) { fprintf(stderr, "no RoCEv2 GID for %s\n", local_ip); goto out; }
    }
#else
    if (ibv_query_gid(ctx, (uint8_t)ib_port, gid_index, &local_gid)) {
        fprintf(stderr, "query_gid %d failed\n", gid_index); goto out;
    }
#endif

    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ib_port,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                      IBV_QP_ACCESS_FLAGS)) { fprintf(stderr, "RST->INIT failed\n"); goto out; }

    const int tcp_connect = host != NULL;
    fd = tcp_connect ? connect_peer(host, tcp_port) : listen_once(tcp_port);
    if (fd < 0) { fprintf(stderr, "control connection failed\n"); goto out; }

    struct destination local = { .qpn = qp->qp_num,
        .psn = ((uint32_t)getpid() * 2654435761u + qp->qp_num) & 0xffffff,
        .gid = local_gid }, remote = {};
    char msg[sizeof DEST_TEMPLATE] = {}, wire[33] = {};
    gid_wire(&local.gid, wire);
    char mine[sizeof DEST_TEMPLATE] = {};
    snprintf(mine, sizeof mine, "%04x:%06x:%06x:%s", 0, local.qpn, local.psn, wire);
    /* Whoever connected speaks first, as in the existing peer protocol. Every
     * later message has an obvious first speaker by RDMA role. */
    if (tcp_connect ? (full_write(fd, mine, sizeof mine) || full_read(fd, msg, sizeof msg))
                    : (full_read(fd, msg, sizeof msg) || full_write(fd, mine, sizeof mine))) {
        fprintf(stderr, "destination exchange failed\n"); goto out;
    }
    unsigned lid = 0;
    if (sscanf(msg, "%x:%x:%x:%32s", &lid, &remote.qpn, &remote.psn, wire) != 4 ||
        wire_gid(wire, &remote.gid)) { fprintf(stderr, "bad destination\n"); goto out; }

    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = path_mtu_of(mtu_bytes),
        .dest_qp_num = remote.qpn, .rq_psn = remote.psn, .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = (uint8_t)ib_port,
        .grh = { .dgid = remote.gid, .sgid_index = sgid, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER)) { fprintf(stderr, "INIT->RTR failed\n"); goto out; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn,
        .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 1 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "RTR->RTS failed\n"); goto out;
    }

    if (server) {
        char done[sizeof "done"];
        if (full_read(fd, done, sizeof done)) goto out;
        for (size_t k = 0; k < buf_len; k++) buf[k] = read_pattern(k);
        struct remote_memory_wire m = { .address_be = be64((uint64_t)(uintptr_t)buf),
            .rkey_be = htonl(mr->rkey), .length_be = htonl((uint32_t)buf_len) };
        if (full_write(fd, &m, sizeof m)) goto out;
        /* Idle for the whole run: a one-sided operation never reaches us. */
        uint64_t last_be = 0;
        if (full_read(fd, &last_be, sizeof last_be)) goto out;
        uint8_t ok = 1;
        if (!is_read && goodput) {
            for (size_t k = 0; k < (size_t)size * depth; k++)
                if (buf[k] != write_pattern(k)) { ok = 0; break; }
        } else if (!is_read && size >= 8) {
            uint64_t got = 0;
            memcpy(&got, buf, sizeof got);
            ok = (be64(got) == be64(last_be)) || (got == be64(last_be));
        }
        if (full_write(fd, &ok, 1)) goto out;
        printf("%s server: op=%s size=%u payload %s\n", goodput ? "GOODPUT" : "LAT",
               op_name, size, ok ? "verified" : "MISMATCH");
        rc = ok ? 0 : 1;
        goto out;
    }

    if (full_write(fd, "done", sizeof "done")) goto out;
    struct remote_memory_wire m = {};
    if (full_read(fd, &m, sizeof m)) goto out;
    const uint64_t remote_addr = be64(m.address_be);
    const uint32_t remote_rkey = ntohl(m.rkey_be);

#ifdef __APPLE__
    struct ibv_mlx5_telemetry tel0 = {}, tel1 = {};
    (void)ibv_mlx5_query_telemetry(ctx, &tel0);
    const int use_inline = want_inline && !is_read && size <= 512 && !goodput;
#else
    const int use_inline = 0;
#endif

    if (goodput) {
        slots = calloc(depth, sizeof *slots);
        if (!slots) goto out;
        for (uint32_t k = 0; k < depth; k++)
            slots[k] = (struct ibv_sge){ .addr = (uintptr_t)buf + (uint64_t)k * size,
                                         .length = size, .lkey = mr->lkey };
        /* READ starts from zeros, so only fetched bytes can pass the check. */
        for (size_t k = 0; k < (size_t)size * depth; k++)
            buf[k] = is_read ? 0 : write_pattern(k);
        struct goodput_result res;
        if (run_goodput(qp, cq, slots, depth, is_read, remote_addr, remote_rkey, seconds, &res))
            goto out;
        int payload_ok = 1;
        if (is_read)
            for (size_t k = 0; k < (size_t)size * depth; k++)
                if (buf[k] != read_pattern(k)) { payload_ok = 0; break; }
        uint64_t unused_stamp = 0;
        uint8_t server_ok = 0;
        if (full_write(fd, &unused_stamp, sizeof unused_stamp) || full_read(fd, &server_ok, 1))
            goto out;
        printf("GOODPUT client: op=%s size=%u depth=%u window=%.1fs mtu=%u\n",
               op_name, size, depth, seconds, mtu_bytes);
        printf("  goodput %.2f Gbit/s  ops/s %.0f  posts %llu\n",
               (double)res.ops * size * 8.0 / seconds / 1e9, (double)res.ops / seconds,
               (unsigned long long)res.posts);
#ifdef __APPLE__
        if (!ibv_mlx5_query_telemetry(ctx, &tel1))
            printf("  blue-flame WQEs %llu of %llu posts\n",
                   (unsigned long long)(tel1.blue_flame_wqes - tel0.blue_flame_wqes),
                   (unsigned long long)res.posts);
#endif
        printf("  payload %s\n", (payload_ok && server_ok) ? "verified on both ends" : "MISMATCH");
        rc = (payload_ok && server_ok) ? 0 : 1;
        goto out;
    }

    lat = calloc((size_t)iters, sizeof *lat);
    post = calloc((size_t)iters, sizeof *post);
    waitns = calloc((size_t)iters, sizeof *waitns);
    polls = calloc((size_t)iters, sizeof *polls);
    pollns = calloc((size_t)iters, sizeof *pollns);
    if (!lat || !post || !waitns || !polls || !pollns) goto out;

    struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = size, .lkey = mr->lkey };
    uint64_t last_stamp = 0;
    for (long i = 0; i < WARMUP + iters; i++) {
        if (!is_read && size >= 8) {          /* outside the timed span */
            last_stamp = (uint64_t)i + 0x5eed;
            memcpy(buf, &last_stamp, sizeof last_stamp);
        }
        struct ibv_send_wr wr = { .wr_id = (uint64_t)i + 1, .sg_list = &sge, .num_sge = 1,
            .opcode = is_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED };
#ifdef __APPLE__
        if (use_inline) wr.send_flags |= IBV_SEND_INLINE;
#endif
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = remote_rkey;
        struct ibv_send_wr *bad = NULL;
        uint64_t t0 = now_ns();
        if (ibv_post_send(qp, &wr, &bad)) { fprintf(stderr, "post failed at %ld\n", i); goto out; }
        uint64_t t1 = now_ns();
        struct ibv_wc wc;
        uint64_t npoll = 0;
        for (;;) {
            int n = ibv_poll_cq(cq, 1, &wc);
            npoll++;
            if (n < 0) { fprintf(stderr, "poll failed\n"); goto out; }
            if (n == 1) break;
        }
        uint64_t t2 = now_ns();
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "completion error %d at %ld\n", wc.status, i); goto out;
        }
        if (i >= WARMUP) {
            long k = i - WARMUP;
            lat[k] = t2 - t0; post[k] = t1 - t0; waitns[k] = t2 - t1;
            /* How many polls one completion took, and what one poll costs.
             * A slow poll delays noticing a completion that is already there,
             * so it is paid inside the latency, not beside it. */
            polls[k] = npoll; pollns[k] = (t2 - t1) / npoll;
        }
    }

    int payload_ok = 1;
    if (is_read)
        for (uint32_t k = 0; k < size; k++)
            if (buf[k] != read_pattern(k)) { payload_ok = 0; break; }
    uint64_t last_be = be64(last_stamp);
    uint8_t server_ok = 0;
    if (full_write(fd, &last_be, sizeof last_be) || full_read(fd, &server_ok, 1)) goto out;

    uint64_t total = 0;
    for (long i = 0; i < iters; i++) total += lat[i];
    qsort(lat, (size_t)iters, sizeof *lat, cmp_u64);
    qsort(post, (size_t)iters, sizeof *post, cmp_u64);
    qsort(waitns, (size_t)iters, sizeof *waitns, cmp_u64);
    qsort(polls, (size_t)iters, sizeof *polls, cmp_u64);
    qsort(pollns, (size_t)iters, sizeof *pollns, cmp_u64);
    printf("LAT client: op=%s size=%u iters=%ld inline=%d relaxed=%d\n",
           op_name, size, iters, use_inline, relaxed);
    printf("  t_min %.2f  t_typical %.2f  t_avg %.2f  p99 %.2f  p99.9 %.2f  t_max %.2f  "
           "post_p50 %.2f  [usec]\n",
           pct_us(lat, iters, 0.0), pct_us(lat, iters, 0.5),
           (double)total / (double)iters / 1000.0, pct_us(lat, iters, 0.99),
           pct_us(lat, iters, 0.999), pct_us(lat, iters, 1.0), pct_us(post, iters, 0.5));
    printf("  wait_p50 %.2f  polls_p50 %llu  one_poll_p50 %.0f ns\n",
           pct_us(waitns, iters, 0.5),
           (unsigned long long)polls[iters / 2],
           (double)pollns[iters / 2]);
#ifdef __APPLE__
    if (!ibv_mlx5_query_telemetry(ctx, &tel1))
        printf("  blue-flame WQEs %llu of %ld posts\n",
               (unsigned long long)(tel1.blue_flame_wqes - tel0.blue_flame_wqes),
               WARMUP + iters);
#endif
    printf("  payload %s\n", (payload_ok && server_ok) ? "verified on both ends" : "MISMATCH");
    rc = (payload_ok && server_ok) ? 0 : 1;

out:
    free(lat);
    free(post);
    free(waitns);
    free(polls);
    free(pollns);
    free(slots);
    if (fd >= 0) close(fd);
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    if (devices) ibv_free_device_list(devices);
    free(buf);
    return rc;
}
