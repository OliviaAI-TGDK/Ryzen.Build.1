/*
 * infinity.c
 *
 * TGDK Steel Ox Proxy
 * Hardened POSIX TCP proxy with:
 * - nonblocking sockets
 * - epoll event loop
 * - bidirectional relay
 * - connection caps
 * - idle timeouts
 * - basic metrics
 * - graceful shutdown
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 infinity.c -o infinity
 *
 * Example:
 *   ./infinity --listen 0.0.0.0:9000 --upstream 127.0.0.1:8080
 *
 * Notes:
 * - Linux / epoll target
 * - single upstream target per process
 * - safe, generic reverse TCP proxy core
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define INF_EPOLL_MAX_EVENTS 128
#define INF_BUFFER_CAP       65536
#define INF_DEFAULT_MAX_CONN 4096
#define INF_DEFAULT_IDLE_SEC 120
#define INF_DEFAULT_BACKLOG  512
#define INF_FD_MAP_CAP       65536


typedef struct {
    char card[32];
    char path[PATH_MAX];
    char campaign_id[128];
    char advertiser_id[128];
    int interval_sec;
    bool once;
    bool include_xattrs;
    bool include_stat;
    bool include_gpu;

    /* blockchain / geth compatibility */
    bool blockchain_mode;
    bool emit_jsonrpc_stub;
    char chain_namespace[64];     /* default ".MNTN" */
    char chain_id[64];            /* placeholder string */
    char geth_rpc_url[512];       /* optional */
    char contract_address[128];   /* optional */
} Config;

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (mkdir(path, 0700) == 0) return 0;
    return -1;
}

static void session_destroy(Session *s, int epfd, const ProxyConfig *cfg) {
    if (!s) return;

    if (cfg) {
        char artifact_path[PATH_MAX];
        if (session_emit_artifact(cfg, s, artifact_path, sizeof(artifact_path)) == 0) {
            run_delta_link_sidecar(cfg, artifact_path);
        }
    }

    if (s->client.fd >= 0) {
        epoll_del(epfd, s->client.fd);
        unregister_fd_ctx(s->client.fd);
        close(s->client.fd);
        s->client.fd = -1;
    }
    if (s->upstream.fd >= 0) {
        epoll_del(epfd, s->upstream.fd);
        unregister_fd_ctx(s->upstream.fd);
        close(s->upstream.fd);
        s->upstream.fd = -1;
    }

    session_unlink(s);
    free(s);
    g_metrics.closed++;
    if (g_metrics.active > 0) g_metrics.active--;
}

static void run_delta_link_sidecar(const ProxyConfig *cfg, const char *artifact_path) {
    if (!cfg->delta_link_on_close) return;

    char cmd[PATH_MAX * 2];
    snprintf(
        cmd, sizeof(cmd),
        "luajit -e 'local dl=require(\"delta_link_ffi\"); "
        "local m=assert(dl.manifest(\"%s\",4096,true)); "
        "local j=dl.json(m); "
        "local f=assert(io.open(\"%s.delta.json\",\"wb\")); f:write(j); f:close()'",
        artifact_path, artifact_path
    );

    (void)system(cmd);
}

static int write_artifact_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int session_emit_artifact(const ProxyConfig *cfg, const Session *s, char *out_path, size_t out_path_sz) {
    if (ensure_dir(cfg->spool_dir) != 0) return -1;

    snprintf(out_path, out_path_sz, "%s/session-%llu.bin",
             cfg->spool_dir, (unsigned long long)s->id);

    /*
     * Emit what remains in the relay buffers as a compact artifact.
     * You can change this to emit a full transcript later.
     */
    Buffer merged = {0};

    size_t a = buffer_size(&s->to_upstream);
    size_t b = buffer_size(&s->to_client);
    if ((a + b) > INF_BUFFER_CAP) {
        a = (a > INF_BUFFER_CAP / 2) ? INF_BUFFER_CAP / 2 : a;
        b = (b > (INF_BUFFER_CAP - a)) ? (INF_BUFFER_CAP - a) : b;
    }

    if (a) {
        memcpy(merged.data + merged.end, s->to_upstream.data + s->to_upstream.start, a);
        merged.end += a;
    }
    if (b) {
        memcpy(merged.data + merged.end, s->to_client.data + s->to_client.start, b);
        merged.end += b;
    }

    return write_artifact_file(out_path, merged.data, merged.end);
}

static void config_default(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->card, sizeof(cfg->card), "card0");
    snprintf(cfg->path, sizeof(cfg->path), ".");
    cfg->interval_sec = 30;
    cfg->once = false;
    cfg->include_xattrs = true;
    cfg->include_stat = true;
    cfg->include_gpu = true;

    cfg->blockchain_mode = false;
    cfg->emit_jsonrpc_stub = false;
    snprintf(cfg->chain_namespace, sizeof(cfg->chain_namespace), ".MNTN");
    snprintf(cfg->chain_id, sizeof(cfg->chain_id), "mntn-placeholder-1");
    cfg->geth_rpc_url[0] = '\0';
    cfg->contract_address[0] = '\0';
}

static void parse_args(Config *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--card")) {
            if (++i >= argc) die("missing value for --card");
            snprintf(cfg->card, sizeof(cfg->card), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--path")) {
            if (++i >= argc) die("missing value for --path");
            snprintf(cfg->path, sizeof(cfg->path), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--campaign")) {
            if (++i >= argc) die("missing value for --campaign");
            snprintf(cfg->campaign_id, sizeof(cfg->campaign_id), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--advertiser")) {
            if (++i >= argc) die("missing value for --advertiser");
            snprintf(cfg->advertiser_id, sizeof(cfg->advertiser_id), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--interval")) {
            if (++i >= argc) die("missing value for --interval");
            cfg->interval_sec = atoi(argv[i]);
            if (cfg->interval_sec < 1) cfg->interval_sec = 1;
        } else if (!strcmp(argv[i], "--once")) {
            cfg->once = true;
        } else if (!strcmp(argv[i], "--no-xattrs")) {
            cfg->include_xattrs = false;
        } else if (!strcmp(argv[i], "--no-stat")) {
            cfg->include_stat = false;
        } else if (!strcmp(argv[i], "--no-gpu")) {
            cfg->include_gpu = false;
        } else if (!strcmp(argv[i], "--blockchain")) {
            cfg->blockchain_mode = true;
        } else if (!strcmp(argv[i], "--emit-jsonrpc-stub")) {
            cfg->emit_jsonrpc_stub = true;
        } else if (!strcmp(argv[i], "--chain-namespace")) {
            if (++i >= argc) die("missing value for --chain-namespace");
            snprintf(cfg->chain_namespace, sizeof(cfg->chain_namespace), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--chain-id")) {
            if (++i >= argc) die("missing value for --chain-id");
            snprintf(cfg->chain_id, sizeof(cfg->chain_id), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--geth-rpc-url")) {
            if (++i >= argc) die("missing value for --geth-rpc-url");
            snprintf(cfg->geth_rpc_url, sizeof(cfg->geth_rpc_url), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--contract-address")) {
            if (++i >= argc) die("missing value for --contract-address");
            snprintf(cfg->contract_address, sizeof(cfg->contract_address), "%s", argv[i]);
        } else {
            usage(argv[0]);
            exit(1);
        }
    }

    if (!cfg->campaign_id[0]) die("missing --campaign");
    if (!cfg->advertiser_id[0]) die("missing --advertiser");
}

static uint64_t fnv1a64(const unsigned char *data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void append_blockchain_envelope(StrBuf *sb, const Config *cfg, const char *payload_json) {
    uint64_t anchor_hash = fnv1a64((const unsigned char *)payload_json, strlen(payload_json));

    sb_appendf(sb, "\"blockchain\":{");
    sb_appendf(sb, "\"enabled\":%s,", cfg->blockchain_mode ? "true" : "false");
    sb_appendf(sb, "\"namespace\":");
    json_escape_append(sb, cfg->chain_namespace);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"chain_id\":");
    json_escape_append(sb, cfg->chain_id);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"anchor_mode\":\"geth-compatible\",");
    sb_appendf(sb, "\"anchor_hash_fnv64\":\"0x%016llx\",", (unsigned long long)anchor_hash);
    sb_appendf(sb, "\"signing_mode\":\"ethereum_personal_sign_compatible\",");
    sb_appendf(sb, "\"rpc_url\":");
    json_escape_append(sb, cfg->geth_rpc_url);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"contract_address\":");
    json_escape_append(sb, cfg->contract_address);
    sb_appendf(sb, "}");
}

static void append_jsonrpc_stub(StrBuf *sb, const Config *cfg, const char *payload_json) {
    uint64_t anchor_hash = fnv1a64((const unsigned char *)payload_json, strlen(payload_json));

    sb_appendf(sb, "\"jsonrpc_stub\":{");
    sb_appendf(sb, "\"jsonrpc\":\"2.0\",");
    sb_appendf(sb, "\"id\":1,");
    sb_appendf(sb, "\"method\":\"eth_call\",");
    sb_appendf(sb, "\"params\":[{");
    sb_appendf(sb, "\"to\":");
    json_escape_append(sb, cfg->contract_address[0] ? cfg->contract_address : "0x0000000000000000000000000000000000000000");
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"data\":");
    json_escape_append(sb, "0x.MNTN_PLACEHOLDER_CALLDATA");
    sb_appendf(sb, "},\"latest\"],");
    sb_appendf(sb, "\"note\":");
    json_escape_append(
        sb,
        "Replace data with ABI-encoded calldata or use personal_sign/account_signData on the anchor payload."
    );
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"anchor_hash_fnv64\":\"0x%016llx\"", (unsigned long long)anchor_hash);
    sb_appendf(sb, "}");
}

typedef struct ProxyConfig {
    char listen_host[256];
    char listen_port[32];
    char upstream_host[256];
    char upstream_port[32];
    int max_connections;
    int idle_timeout_sec;
    int backlog;
    bool verbose;

    char spool_dir[PATH_MAX];
    bool delta_link_on_close;
} ProxyConfig;

typedef struct Buffer {
    uint8_t data[INF_BUFFER_CAP];
    size_t start;
    size_t end;
} Buffer;

typedef struct Peer {
    int fd;
    bool read_eof;
    bool write_shutdown;
    bool connecting;
} Peer;

struct Session;

typedef struct FdCtx {
    struct Session *session;
    int side; /* 0 = client, 1 = upstream, 2 = listener */
} FdCtx;

typedef struct Metrics {
    uint64_t accepted;
    uint64_t rejected;
    uint64_t active;
    uint64_t closed;
    uint64_t upstream_connect_fail;
    uint64_t bytes_c2u;
    uint64_t bytes_u2c;
    uint64_t read_errors;
    uint64_t write_errors;
    uint64_t idle_kills;
} Metrics;

typedef struct Config {
    char listen_host[256];
    char listen_port[32];
    char upstream_host[256];
    char upstream_port[32];
    int max_connections;
    int idle_timeout_sec;
    int backlog;
    bool verbose;
} Config;

typedef struct Session {
    uint64_t id;
    time_t created_at;
    time_t last_activity;

    char client_addr[128];

    Peer client;
    Peer upstream;

    Buffer to_upstream;
    Buffer to_client;

    struct Session *next;
} Session;

static volatile sig_atomic_t g_running = 1;
static Metrics g_metrics = {0};
static FdCtx *g_fd_map[INF_FD_MAP_CAP] = {0};
static Session *g_sessions = NULL;
static uint64_t g_next_session_id = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void logf_msg(const Config *cfg, const char *fmt, ...) {
    if (!cfg->verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static time_t now_sec(void) {
    return time(NULL);
}

static bool buffer_empty(const Buffer *b) {
    return b->start == b->end;
}

static size_t buffer_size(const Buffer *b) {
    return b->end - b->start;
}

static size_t buffer_space(const Buffer *b) {
    return INF_BUFFER_CAP - b->end;
}

static void buffer_compact(Buffer *b) {
    if (b->start > 0 && b->start != b->end) {
        memmove(b->data, b->data + b->start, b->end - b->start);
        b->end -= b->start;
        b->start = 0;
    } else if (b->start == b->end) {
        b->start = 0;
        b->end = 0;
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static int parse_host_port(const char *input, char *host, size_t host_sz, char *port, size_t port_sz) {
    if (!input || !host || !port) return -1;

    const char *colon = strrchr(input, ':');
    if (!colon || colon == input || *(colon + 1) == '\0') {
        return -1;
    }

    size_t host_len = (size_t)(colon - input);
    size_t port_len = strlen(colon + 1);

    if (host_len >= host_sz || port_len >= port_sz) {
        return -1;
    }

    memcpy(host, input, host_len);
    host[host_len] = '\0';
    memcpy(port, colon + 1, port_len + 1);
    return 0;
}

static void config_init(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->listen_host, sizeof(cfg->listen_host), "0.0.0.0");
    snprintf(cfg->listen_port, sizeof(cfg->listen_port), "9000");
    snprintf(cfg->upstream_host, sizeof(cfg->upstream_host), "127.0.0.1");
    snprintf(cfg->upstream_port, sizeof(cfg->upstream_port), "8080");
    cfg->max_connections = INF_DEFAULT_MAX_CONN;
    cfg->idle_timeout_sec = INF_DEFAULT_IDLE_SEC;
    cfg->backlog = INF_DEFAULT_BACKLOG;
    cfg->verbose = false;
}

static void parse_args(Config *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--listen")) {
            if (++i >= argc) die("missing value for --listen");
            if (parse_host_port(argv[i], cfg->listen_host, sizeof(cfg->listen_host),
                                cfg->listen_port, sizeof(cfg->listen_port)) != 0) {
                die("invalid --listen, expected host:port");
            }
        } else if (!strcmp(argv[i], "--upstream")) {
            if (++i >= argc) die("missing value for --upstream");
            if (parse_host_port(argv[i], cfg->upstream_host, sizeof(cfg->upstream_host),
                                cfg->upstream_port, sizeof(cfg->upstream_port)) != 0) {
                die("invalid --upstream, expected host:port");
            }
        } else if (!strcmp(argv[i], "--max-connections")) {
            if (++i >= argc) die("missing value for --max-connections");
            cfg->max_connections = atoi(argv[i]);
            if (cfg->max_connections <= 0) die("invalid max connections");
        } else if (!strcmp(argv[i], "--idle-timeout")) {
            if (++i >= argc) die("missing value for --idle-timeout");
            cfg->idle_timeout_sec = atoi(argv[i]);
            if (cfg->idle_timeout_sec <= 0) die("invalid idle timeout");
        } else if (!strcmp(argv[i], "--backlog")) {
            if (++i >= argc) die("missing value for --backlog");
            cfg->backlog = atoi(argv[i]);
            if (cfg->backlog <= 0) die("invalid backlog");
        } else if (!strcmp(argv[i], "--verbose")) {
            cfg->verbose = true;
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
}

static int epoll_add(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_mod(int epfd, int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static int epoll_del(int epfd, int fd) {
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

static void unregister_fd_ctx(int fd) {
    if (fd >= 0 && fd < INF_FD_MAP_CAP) {
        free(g_fd_map[fd]);
        g_fd_map[fd] = NULL;
    }
}

static int register_fd_ctx(int fd, Session *session, int side) {
    if (fd < 0 || fd >= INF_FD_MAP_CAP) return -1;
    FdCtx *ctx = (FdCtx *)malloc(sizeof(FdCtx));
    if (!ctx) return -1;
    ctx->session = session;
    ctx->side = side;
    g_fd_map[fd] = ctx;
    return 0;
}

static Session *session_create(void) {
    Session *s = (Session *)calloc(1, sizeof(Session));
    if (!s) return NULL;
    s->id = g_next_session_id++;
    s->created_at = now_sec();
    s->last_activity = s->created_at;
    s->client.fd = -1;
    s->upstream.fd = -1;
    s->next = g_sessions;
    g_sessions = s;
    g_metrics.active++;
    return s;
}

static void session_unlink(Session *s) {
    Session **pp = &g_sessions;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void session_destroy(Session *s, int epfd) {
    if (!s) return;

    if (s->client.fd >= 0) {
        epoll_del(epfd, s->client.fd);
        unregister_fd_ctx(s->client.fd);
        close(s->client.fd);
        s->client.fd = -1;
    }
    if (s->upstream.fd >= 0) {
        epoll_del(epfd, s->upstream.fd);
        unregister_fd_ctx(s->upstream.fd);
        close(s->upstream.fd);
        s->upstream.fd = -1;
    }

    session_unlink(s);
    free(s);
    g_metrics.closed++;
    if (g_metrics.active > 0) g_metrics.active--;
}

static int resolve_and_bind_listener(const Config *cfg) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    int listen_fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(cfg->listen_host, cfg->listen_port, &hints, &res);
    if (rc != 0) {
        die("getaddrinfo listen: %s", gai_strerror(rc));
    }

    for (rp = res; rp; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0) continue;

        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (set_nonblocking(listen_fd) != 0) {
                close(listen_fd);
                listen_fd = -1;
                continue;
            }
            if (listen(listen_fd, cfg->backlog) != 0) {
                close(listen_fd);
                listen_fd = -1;
                continue;
            }
            break;
        }

        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) {
        die("unable to bind listener on %s:%s", cfg->listen_host, cfg->listen_port);
    }

    return listen_fd;
}

static int connect_nonblocking_upstream(const Config *cfg, struct sockaddr_storage *addr_out, socklen_t *addrlen_out) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(cfg->upstream_host, cfg->upstream_port, &hints, &res);
    if (rc != 0) {
        return -1;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (set_nonblocking(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (addr_out && addrlen_out) {
            memset(addr_out, 0, sizeof(*addr_out));
            memcpy(addr_out, rp->ai_addr, rp->ai_addrlen);
            *addrlen_out = (socklen_t)rp->ai_addrlen;
        }

        int ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0 || errno == EINPROGRESS) {
            freeaddrinfo(res);
            return fd;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return -1;
}

static void peer_shutdown_write(Peer *p) {
    if (p->fd >= 0 && !p->write_shutdown) {
        shutdown(p->fd, SHUT_WR);
        p->write_shutdown = true;
    }
}

static uint32_t session_events_for_side(const Session *s, int side) {
    uint32_t ev = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (side == 0) {
        if (!s->client.read_eof && buffer_space((Buffer *)&s->to_upstream) > 0) {
            ev |= EPOLLIN;
        }
        if (!buffer_empty((Buffer *)&s->to_client)) {
            ev |= EPOLLOUT;
        }
    } else {
        if (s->upstream.connecting) {
            ev |= EPOLLOUT;
        } else {
            if (!s->upstream.read_eof && buffer_space((Buffer *)&s->to_client) > 0) {
                ev |= EPOLLIN;
            }
            if (!buffer_empty((Buffer *)&s->to_upstream)) {
                ev |= EPOLLOUT;
            }
        }
    }
    return ev;
}

static void session_refresh_interest(Session *s, int epfd) {
    if (s->client.fd >= 0) {
        epoll_mod(epfd, s->client.fd, session_events_for_side(s, 0));
    }
    if (s->upstream.fd >= 0) {
        epoll_mod(epfd, s->upstream.fd, session_events_for_side(s, 1));
    }
}

static bool session_fully_done(const Session *s) {
    return s->client.read_eof &&
           s->upstream.read_eof &&
           buffer_empty((Buffer *)&s->to_upstream) &&
           buffer_empty((Buffer *)&s->to_client);
}

static void session_consider_half_close(Session *s) {
    if (s->client.read_eof && buffer_empty(&s->to_upstream)) {
        peer_shutdown_write(&s->upstream);
    }
    if (s->upstream.read_eof && buffer_empty(&s->to_client)) {
        peer_shutdown_write(&s->client);
    }
}

static int handle_connect_complete(Session *s) {
    if (!s->upstream.connecting) return 0;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s->upstream.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return -1;
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    s->upstream.connecting = false;
    return 0;
}

static int relay_read_into_buffer(int from_fd, Buffer *dst, uint64_t *metric_bytes) {
    buffer_compact(dst);
    if (buffer_space(dst) == 0) return 0;

    ssize_t n = recv(from_fd, dst->data + dst->end, buffer_space(dst), 0);
    if (n > 0) {
        dst->end += (size_t)n;
        *metric_bytes += (uint64_t)n;
        return 1;
    }
    if (n == 0) return -2; /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

static int relay_write_from_buffer(int to_fd, Buffer *src) {
    if (buffer_empty(src)) return 0;

    ssize_t n = send(to_fd, src->data + src->start, buffer_size(src), MSG_NOSIGNAL);
    if (n > 0) {
        src->start += (size_t)n;
        if (src->start == src->end) {
            src->start = 0;
            src->end = 0;
        }
        return 1;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

static void addr_to_string(const struct sockaddr *sa, socklen_t slen, char *out, size_t out_sz) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc = getnameinfo(sa, slen, host, sizeof(host), serv, sizeof(serv),
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc == 0) {
        snprintf(out, out_sz, "%s:%s", host, serv);
    } else {
        snprintf(out, out_sz, "unknown");
    }
}

static void handle_listener_accept(const Config *cfg, int epfd, int listen_fd) {
    while (1) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int cfd = accept(listen_fd, (struct sockaddr *)&ss, &slen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }

        if ((int)g_metrics.active >= cfg->max_connections) {
            g_metrics.rejected++;
            close(cfd);
            continue;
        }

        if (set_nonblocking(cfd) != 0) {
            close(cfd);
            continue;
        }

        Session *s = session_create();
        if (!s) {
            close(cfd);
            continue;
        }

        s->client.fd = cfd;
        addr_to_string((struct sockaddr *)&ss, slen, s->client_addr, sizeof(s->client_addr));

        int ufd = connect_nonblocking_upstream(cfg, NULL, NULL);
        if (ufd < 0) {
            g_metrics.upstream_connect_fail++;
            session_destroy(s, epfd);
            continue;
        }
        s->upstream.fd = ufd;
        s->upstream.connecting = true;

        if (register_fd_ctx(cfd, s, 0) != 0 || register_fd_ctx(ufd, s, 1) != 0) {
            session_destroy(s, epfd);
            continue;
        }

        if (epoll_add(epfd, cfd, session_events_for_side(s, 0)) != 0 ||
            epoll_add(epfd, ufd, session_events_for_side(s, 1)) != 0) {
            session_destroy(s, epfd);
            continue;
        }

        g_metrics.accepted++;
        logf_msg(cfg, "[accept] session=%llu client=%s", (unsigned long long)s->id, s->client_addr);
    }
}

static bool handle_side_io(const Config *cfg, Session *s, int side, uint32_t ev, int epfd) {
    s->last_activity = now_sec();

    Peer *peer = (side == 0) ? &s->client : &s->upstream;
    Buffer *inbuf = (side == 0) ? &s->to_upstream : &s->to_client;
    Buffer *outbuf = (side == 0) ? &s->to_client : &s->to_upstream;
    uint64_t *metric = (side == 0) ? &g_metrics.bytes_c2u : &g_metrics.bytes_u2c;

    if (ev & (EPOLLERR | EPOLLHUP)) {
        return false;
    }

    if (side == 1 && peer->connecting && (ev & EPOLLOUT)) {
        if (handle_connect_complete(s) != 0) {
            g_metrics.upstream_connect_fail++;
            logf_msg(cfg, "[connect-fail] session=%llu errno=%d", (unsigned long long)s->id, errno);
            return false;
        }
    }

    if (ev & EPOLLIN) {
        int rr = relay_read_into_buffer(peer->fd, inbuf, metric);
        if (rr < 0) {
            if (rr == -2) {
                peer->read_eof = true;
            } else {
                g_metrics.read_errors++;
                return false;
            }
        }
    }

    if (ev & EPOLLRDHUP) {
        peer->read_eof = true;
    }

    if (ev & EPOLLOUT) {
        if (!(side == 1 && peer->connecting)) {
            int wr = relay_write_from_buffer(peer->fd, outbuf);
            if (wr < 0) {
                g_metrics.write_errors++;
                return false;
            }
        }
    }

    session_consider_half_close(s);

    if (session_fully_done(s)) {
        return false;
    }

    session_refresh_interest(s, epfd);
    return true;
}

static void close_all_sessions(int epfd) {
    Session *s = g_sessions;
    while (s) {
        Session *next = s->next;
        session_destroy(s, epfd);
        s = next;
    }
}

static void print_metrics(const Config *cfg) {
    (void)cfg;
    fprintf(stderr,
        "[metrics] accepted=%llu rejected=%llu active=%llu closed=%llu "
        "upstream_fail=%llu c2u=%llu u2c=%llu idle_kills=%llu\n",
        (unsigned long long)g_metrics.accepted,
        (unsigned long long)g_metrics.rejected,
        (unsigned long long)g_metrics.active,
        (unsigned long long)g_metrics.closed,
        (unsigned long long)g_metrics.upstream_connect_fail,
        (unsigned long long)g_metrics.bytes_c2u,
        (unsigned long long)g_metrics.bytes_u2c,
        (unsigned long long)g_metrics.idle_kills
    );
}

static void enforce_idle_timeouts(const Config *cfg, int epfd) {
    time_t now = now_sec();
    Session *s = g_sessions;

    while (s) {
        Session *next = s->next;
        if ((now - s->last_activity) > cfg->idle_timeout_sec) {
            g_metrics.idle_kills++;
            logf_msg(cfg, "[idle-timeout] session=%llu client=%s",
                     (unsigned long long)s->id, s->client_addr);
            session_destroy(s, epfd);
        }
        s = next;
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --path PATH --campaign ID --advertiser ID "
        "[--card card0] [--interval SEC] [--once] [--no-xattrs] [--no-stat] [--no-gpu] "
        "[--blockchain] [--emit-jsonrpc-stub] [--chain-namespace .MNTN] "
        "[--chain-id mntn-placeholder-1] [--geth-rpc-url URL] [--contract-address 0x...]\n",
        argv0
    );
}

static void config_init(ProxyConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->listen_host, sizeof(cfg->listen_host), "0.0.0.0");
    snprintf(cfg->listen_port, sizeof(cfg->listen_port), "9000");
    snprintf(cfg->upstream_host, sizeof(cfg->upstream_host), "127.0.0.1");
    snprintf(cfg->upstream_port, sizeof(cfg->upstream_port), "8080");
    snprintf(cfg->spool_dir, sizeof(cfg->spool_dir), "./spool");
    cfg->max_connections = INF_DEFAULT_MAX_CONN;
    cfg->idle_timeout_sec = INF_DEFAULT_IDLE_SEC;
    cfg->backlog = INF_DEFAULT_BACKLOG;
    cfg->verbose = false;
    cfg->delta_link_on_close = true;
}

static void parse_args(ProxyConfig *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--listen")) {
            if (++i >= argc) die("missing value for --listen");
            if (parse_host_port(argv[i], cfg->listen_host, sizeof(cfg->listen_host),
                                cfg->listen_port, sizeof(cfg->listen_port)) != 0) {
                die("invalid --listen, expected host:port");
            }
        } else if (!strcmp(argv[i], "--upstream")) {
            if (++i >= argc) die("missing value for --upstream");
            if (parse_host_port(argv[i], cfg->upstream_host, sizeof(cfg->upstream_host),
                                cfg->upstream_port, sizeof(cfg->upstream_port)) != 0) {
                die("invalid --upstream, expected host:port");
            }
        } else if (!strcmp(argv[i], "--spool-dir")) {
            if (++i >= argc) die("missing value for --spool-dir");
            snprintf(cfg->spool_dir, sizeof(cfg->spool_dir), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--no-delta-link")) {
            cfg->delta_link_on_close = false;
        } else if (!strcmp(argv[i], "--max-connections")) {
            if (++i >= argc) die("missing value for --max-connections");
            cfg->max_connections = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--idle-timeout")) {
            if (++i >= argc) die("missing value for --idle-timeout");
            cfg->idle_timeout_sec = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--backlog")) {
            if (++i >= argc) die("missing value for --backlog");
            cfg->backlog = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--verbose")) {
            cfg->verbose = true;
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }
}

int main(int argc, char **argv) {
    ProxyConfig cfg;
    config_init(&cfg);
    parse_args(&cfg, argc, argv);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = resolve_and_bind_listener(&cfg);
    int epfd = epoll_create1(0);
    if (epfd < 0) die("epoll_create1 failed: %s", strerror(errno));

    if (register_fd_ctx(listen_fd, NULL, 2) != 0) {
        die("listener ctx registration failed");
    }
    if (epoll_add(epfd, listen_fd, EPOLLIN | EPOLLERR | EPOLLHUP) != 0) {
        die("epoll add listener failed: %s", strerror(errno));
    }

    fprintf(stderr,
        "[SteelOx] listening=%s:%s upstream=%s:%s max_conn=%d idle=%ds spool=%s\n",
        cfg.listen_host, cfg.listen_port,
        cfg.upstream_host, cfg.upstream_port,
        cfg.max_connections, cfg.idle_timeout_sec,
        cfg.spool_dir
    );

    struct epoll_event events[INF_EPOLL_MAX_EVENTS];
    time_t last_maintenance = now_sec();

    while (g_running) {
        int n = epoll_wait(epfd, events, INF_EPOLL_MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait failed: %s", strerror(errno));
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd < 0 || fd >= INF_FD_MAP_CAP || !g_fd_map[fd]) continue;

            FdCtx *ctx = g_fd_map[fd];
            if (ctx->side == 2) {
                handle_listener_accept(&cfg, epfd, listen_fd);
                continue;
            }

            Session *s = ctx->session;
            if (!s) continue;

            bool keep = handle_side_io(&cfg, s, ctx->side, events[i].events, epfd);
            if (!keep) {
                logf_msg(&cfg, "[close] session=%llu client=%s",
                         (unsigned long long)s->id, s->client_addr);
                session_destroy(s, epfd, &cfg);
            }
        }

        time_t now = now_sec();
        if (now != last_maintenance) {
            enforce_idle_timeouts(&cfg, epfd);
            if (cfg.verbose) {
                print_metrics(&cfg);
            }
            last_maintenance = now;
        }
    }

    fprintf(stderr, "[SteelOx] shutting down\n");
    close_all_sessions(epfd); /* you can add cfg here too if you want spool on shutdown */

    epoll_del(epfd, listen_fd);
    unregister_fd_ctx(listen_fd);
    close(listen_fd);
    close(epfd);

    print_metrics(&cfg);
    return 0;
}
    Config cfg;
    config_init(&cfg);
    parse_args(&cfg, argc, argv);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = resolve_and_bind_listener(&cfg);
    int epfd = epoll_create1(0);
    if (epfd < 0) die("epoll_create1 failed: %s", strerror(errno));

    if (register_fd_ctx(listen_fd, NULL, 2) != 0) {
        die("listener ctx registration failed");
    }
    if (epoll_add(epfd, listen_fd, EPOLLIN | EPOLLERR | EPOLLHUP) != 0) {
        die("epoll add listener failed: %s", strerror(errno));
    }

    fprintf(stderr,
        "[SteelOx] listening=%s:%s upstream=%s:%s max_conn=%d idle=%ds\n",
        cfg.listen_host, cfg.listen_port,
        cfg.upstream_host, cfg.upstream_port,
        cfg.max_connections, cfg.idle_timeout_sec
    );

    struct epoll_event events[INF_EPOLL_MAX_EVENTS];
    time_t last_maintenance = now_sec();

    while (g_running) {
        int n = epoll_wait(epfd, events, INF_EPOLL_MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait failed: %s", strerror(errno));
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd < 0 || fd >= INF_FD_MAP_CAP || !g_fd_map[fd]) continue;

            FdCtx *ctx = g_fd_map[fd];
            if (ctx->side == 2) {
                handle_listener_accept(&cfg, epfd, listen_fd);
                continue;
            }

            Session *s = ctx->session;
            if (!s) continue;

            bool keep = handle_side_io(&cfg, s, ctx->side, events[i].events, epfd);
            if (!keep) {
                logf_msg(&cfg, "[close] session=%llu client=%s",
                         (unsigned long long)s->id, s->client_addr);
                session_destroy(s, epfd);
            }
        }

        time_t now = now_sec();
        if (now != last_maintenance) {
            enforce_idle_timeouts(&cfg, epfd);
            if (cfg.verbose) {
                print_metrics(&cfg);
            }
            last_maintenance = now;
        }
    }

    fprintf(stderr, "[SteelOx] shutting down\n");
    close_all_sessions(epfd);

    epoll_del(epfd, listen_fd);
    unregister_fd_ctx(listen_fd);
    close(listen_fd);
    close(epfd);

    print_metrics(&cfg);
    return 0;
}
