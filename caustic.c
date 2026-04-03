/* caustic.c
 *
 * Magenta Framework Causal Ratio
 * Organizer for ad-interest / metadata enrichment with optional MNTN-side delivery.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 caustic.c -o caustic
 *
 * Build with HTTP:
 *   gcc -O2 -Wall -Wextra -std=c11 -DUSE_CURL caustic.c -lcurl -o caustic
 *
 * Usage:
 *   ./caustic --path ./artifact.bin --campaign CAMP-01 --advertiser ADV-01 --once
 *   ./caustic --path ./artifact.bin --campaign CAMP-01 --advertiser ADV-01 --interval 30
 *
 * Env:
 *   MNTN_ENDPOINT=https://your-configured-endpoint.example
 *   MNTN_API_KEY=your_api_key
 *
 * Notes:
 * - Designed as a sidecar / normalizer, not a native MNTN campaign object editor.
 * - Uses POSIX xattrs as metadata input.
 * - Uses AMDGPU sysfs as optional machine context input.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_CURL
#include <curl/curl.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define JSON_BUF_CAP (256 * 1024)

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
} Config;

typedef struct {
    unsigned long long vram_total;
    unsigned long long vram_used;
    unsigned long long gtt_total;
    unsigned long long gtt_used;
    bool ok;
} GpuMetrics;

typedef struct {
    unsigned long long size_bytes;
    bool ok;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    time_t mtime;
} FileMeta;

typedef struct {
    uint32_t metadata_density_ppm;
    uint32_t freshness_ppm;
    uint32_t gpu_pressure_ppm;
    uint32_t path_weight_ppm;
    uint32_t xattr_weight_ppm;
    uint32_t campaign_weight_ppm;
    uint32_t advertiser_weight_ppm;

    uint32_t magenta_interest_ppm;
    uint32_t magenta_causal_ratio_ppm;
    uint32_t magenta_quality_ppm;
} MagentaScores;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t ppm_mul(uint32_t a, uint32_t b) {
    uint64_t v = (uint64_t)a * (uint64_t)b;
    v += 500000ULL;
    v /= 1000000ULL;
    return (uint32_t)v;
}

static void sb_init(StrBuf *sb, size_t cap) {
    sb->data = (char *)malloc(cap);
    if (!sb->data) die("malloc failed");
    sb->len = 0;
    sb->cap = cap;
    sb->data[0] = '\0';
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    while (1) {
        va_start(ap, fmt);
        int n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
        va_end(ap);

        if (n < 0) die("vsnprintf failed");

        if ((size_t)n < (sb->cap - sb->len)) {
            sb->len += (size_t)n;
            return;
        }

        size_t new_cap = sb->cap * 2 + (size_t)n + 64;
        char *new_data = (char *)realloc(sb->data, new_cap);
        if (!new_data) die("realloc failed");
        sb->data = new_data;
        sb->cap = new_cap;
    }
}

static void json_escape_append(StrBuf *sb, const char *s) {
    sb_appendf(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
            case '\"': sb_appendf(sb, "\\\""); break;
            case '\\': sb_appendf(sb, "\\\\"); break;
            case '\b': sb_appendf(sb, "\\b"); break;
            case '\f': sb_appendf(sb, "\\f"); break;
            case '\n': sb_appendf(sb, "\\n"); break;
            case '\r': sb_appendf(sb, "\\r"); break;
            case '\t': sb_appendf(sb, "\\t"); break;
            default:
                if (*p < 0x20) sb_appendf(sb, "\\u%04x", *p);
                else sb_appendf(sb, "%c", *p);
        }
    }
    sb_appendf(sb, "\"");
}

static char *read_text_file_trim(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        --n;
    }

    return buf;
}

static bool parse_ull_file(const char *path, unsigned long long *out) {
    char *s = read_text_file_trim(path);
    if (!s) return false;

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    bool ok = (errno == 0 && end && *end == '\0');
    free(s);

    if (!ok) return false;
    *out = v;
    return true;
}

static GpuMetrics read_amdgpu_metrics(const char *card) {
    GpuMetrics m = {0};
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_vram_total", card);
    bool a = parse_ull_file(path, &m.vram_total);

    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_vram_used", card);
    bool b = parse_ull_file(path, &m.vram_used);

    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_gtt_total", card);
    bool c = parse_ull_file(path, &m.gtt_total);

    snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_gtt_used", card);
    bool d = parse_ull_file(path, &m.gtt_used);

    m.ok = a && b && c && d;
    return m;
}

static FileMeta read_file_meta(const char *path) {
    FileMeta fm = {0};
    struct stat st;
    if (stat(path, &st) != 0) return fm;

    fm.ok = true;
    fm.size_bytes = (unsigned long long)st.st_size;
    fm.mode = st.st_mode;
    fm.uid = st.st_uid;
    fm.gid = st.st_gid;
    fm.mtime = st.st_mtime;
    return fm;
}

static int count_xattrs(const char *path, size_t *out_count, size_t *out_name_bytes) {
    ssize_t namebuf_sz = listxattr(path, NULL, 0);
    if (namebuf_sz < 0) return -1;

    char *names = (char *)malloc((size_t)namebuf_sz + 1);
    if (!names) return -1;

    namebuf_sz = listxattr(path, names, (size_t)namebuf_sz);
    if (namebuf_sz < 0) {
        free(names);
        return -1;
    }

    size_t count = 0;
    size_t name_bytes = 0;
    for (ssize_t i = 0; i < namebuf_sz; ) {
        const char *name = &names[i];
        size_t nlen = strlen(name);
        count++;
        name_bytes += nlen;
        i += (ssize_t)nlen + 1;
    }

    free(names);
    *out_count = count;
    *out_name_bytes = name_bytes;
    return 0;
}

static void append_iso8601_utc(StrBuf *sb) {
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    json_escape_append(sb, buf);
}

static void append_xattrs_json(StrBuf *sb, const char *path) {
    ssize_t namebuf_sz = listxattr(path, NULL, 0);
    if (namebuf_sz < 0) {
        sb_appendf(sb, "{\"ok\":false,\"errno\":%d}", errno);
        return;
    }

    char *names = (char *)malloc((size_t)namebuf_sz + 1);
    if (!names) die("malloc xattrs failed");

    namebuf_sz = listxattr(path, names, (size_t)namebuf_sz);
    if (namebuf_sz < 0) {
        int e = errno;
        free(names);
        sb_appendf(sb, "{\"ok\":false,\"errno\":%d}", e);
        return;
    }

    sb_appendf(sb, "{");
    bool first = true;

    for (ssize_t i = 0; i < namebuf_sz; ) {
        const char *name = &names[i];
        size_t name_len = strlen(name);
        ssize_t val_sz = getxattr(path, name, NULL, 0);

        if (val_sz >= 0) {
            char *val = (char *)malloc((size_t)val_sz + 1);
            if (!val) die("malloc xattr value failed");

            val_sz = getxattr(path, name, val, (size_t)val_sz);
            if (val_sz >= 0) {
                val[val_sz] = '\0';
                if (!first) sb_appendf(sb, ",");
                first = false;
                json_escape_append(sb, name);
                sb_appendf(sb, ":");
                json_escape_append(sb, val);
            }
            free(val);
        }

        i += (ssize_t)name_len + 1;
    }

    sb_appendf(sb, "}");
    free(names);
}

static uint32_t file_freshness_ppm(time_t mtime) {
    time_t now = time(NULL);
    if (mtime <= 0 || now < mtime) return 500000U;

    time_t age = now - mtime;
    if (age <= 3600) return 1000000U;
    if (age <= 86400) return 850000U;
    if (age <= 7 * 86400) return 650000U;
    if (age <= 30 * 86400) return 450000U;
    return 250000U;
}

static MagentaScores compute_magenta_scores(
    const Config *cfg,
    const FileMeta *fm,
    const GpuMetrics *gm,
    size_t xattr_count,
    size_t xattr_name_bytes
) {
    MagentaScores s;
    memset(&s, 0, sizeof(s));

    uint32_t gpu_pressure = 0;
    if (cfg->include_gpu && gm->ok) {
        uint32_t vram_ppm = gm->vram_total ? (uint32_t)((gm->vram_used * 1000000ULL) / gm->vram_total) : 0;
        uint32_t gtt_ppm  = gm->gtt_total ? (uint32_t)((gm->gtt_used * 1000000ULL) / gm->gtt_total) : 0;
        gpu_pressure = (vram_ppm + gtt_ppm) / 2U;
    }

    uint32_t metadata_density = 0;
    if (fm->ok) {
        unsigned long long denom = fm->size_bytes ? fm->size_bytes : 1ULL;
        unsigned long long raw = ((unsigned long long)(xattr_count * 4096ULL + xattr_name_bytes) * 1000000ULL) / denom;
        if (raw > 1000000ULL) raw = 1000000ULL;
        metadata_density = (uint32_t)raw;
    }

    uint32_t freshness = fm->ok ? file_freshness_ppm(fm->mtime) : 400000U;

    uint32_t path_weight = 550000U;
    if (strstr(cfg->path, "campaign")) path_weight = 800000U;
    else if (strstr(cfg->path, "creative")) path_weight = 760000U;
    else if (strstr(cfg->path, "audience")) path_weight = 820000U;

    uint32_t xattr_weight = clamp_u32((uint32_t)(xattr_count * 90000U), 120000U, 1000000U);
    uint32_t campaign_weight = (cfg->campaign_id[0] ? 900000U : 300000U);
    uint32_t advertiser_weight = (cfg->advertiser_id[0] ? 900000U : 300000U);

    /* interest: bounded enrichment score */
    uint64_t interest =
        (uint64_t)metadata_density * 26ULL +
        (uint64_t)freshness * 18ULL +
        (uint64_t)xattr_weight * 20ULL +
        (uint64_t)campaign_weight * 18ULL +
        (uint64_t)advertiser_weight * 18ULL;

    interest /= 100ULL;
    if (interest > 1000000ULL) interest = 1000000ULL;

    /* causal ratio: balance metadata quality against machine pressure */
    uint64_t quality =
        (uint64_t)metadata_density * 32ULL +
        (uint64_t)freshness * 24ULL +
        (uint64_t)path_weight * 18ULL +
        (uint64_t)xattr_weight * 26ULL;

    quality /= 100ULL;
    if (quality > 1000000ULL) quality = 1000000ULL;

    uint64_t causal =
        ((uint64_t)interest * 65ULL + (uint64_t)quality * 35ULL) / 100ULL;

    if (gpu_pressure > 0) {
        uint32_t pressure_penalty = gpu_pressure / 5U;
        causal = (causal > pressure_penalty) ? (causal - pressure_penalty) : 0;
    }
    if (causal > 1000000ULL) causal = 1000000ULL;

    s.metadata_density_ppm = metadata_density;
    s.freshness_ppm = freshness;
    s.gpu_pressure_ppm = gpu_pressure;
    s.path_weight_ppm = path_weight;
    s.xattr_weight_ppm = xattr_weight;
    s.campaign_weight_ppm = campaign_weight;
    s.advertiser_weight_ppm = advertiser_weight;
    s.magenta_interest_ppm = (uint32_t)interest;
    s.magenta_causal_ratio_ppm = (uint32_t)causal;
    s.magenta_quality_ppm = (uint32_t)quality;

    return s;
}

static void build_payload_json(StrBuf *sb, const Config *cfg) {
    FileMeta fm = read_file_meta(cfg->path);
    GpuMetrics gm = cfg->include_gpu ? read_amdgpu_metrics(cfg->card) : (GpuMetrics){0};

    size_t xattr_count = 0;
    size_t xattr_name_bytes = 0;
    if (cfg->include_xattrs) {
        (void)count_xattrs(cfg->path, &xattr_count, &xattr_name_bytes);
    }

    MagentaScores sc = compute_magenta_scores(cfg, &fm, &gm, xattr_count, xattr_name_bytes);

    sb_appendf(sb, "{");
    sb_appendf(sb, "\"schema\":\"magenta-framework-causal-ratio/v1\",");
    sb_appendf(sb, "\"timestamp\":");
    append_iso8601_utc(sb);
    sb_appendf(sb, ",");

    sb_appendf(sb, "\"platform\":\"posix\",");
    sb_appendf(sb, "\"target\":\"mntn-reporting-bridge\",");
    sb_appendf(sb, "\"campaign_id\":");
    json_escape_append(sb, cfg->campaign_id);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"advertiser_id\":");
    json_escape_append(sb, cfg->advertiser_id);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"path\":");
    json_escape_append(sb, cfg->path);
    sb_appendf(sb, ",");

    sb_appendf(sb, "\"gpu\":{");
    sb_appendf(sb, "\"enabled\":%s,", cfg->include_gpu ? "true" : "false");
    sb_appendf(sb, "\"ok\":%s,", gm.ok ? "true" : "false");
    sb_appendf(sb, "\"card\":");
    json_escape_append(sb, cfg->card);
    sb_appendf(sb, ",");
    sb_appendf(sb, "\"vram_total_bytes\":%llu,", gm.vram_total);
    sb_appendf(sb, "\"vram_used_bytes\":%llu,", gm.vram_used);
    sb_appendf(sb, "\"gtt_total_bytes\":%llu,", gm.gtt_total);
    sb_appendf(sb, "\"gtt_used_bytes\":%llu", gm.gtt_used);
    sb_appendf(sb, "},");

    sb_appendf(sb, "\"file\":{");
    sb_appendf(sb, "\"ok\":%s,", fm.ok ? "true" : "false");
    sb_appendf(sb, "\"size_bytes\":%llu,", fm.size_bytes);
    sb_appendf(sb, "\"mtime\":%lld,", (long long)fm.mtime);
    sb_appendf(sb, "\"xattr_count\":%zu,", xattr_count);
    sb_appendf(sb, "\"xattr_name_bytes\":%zu", xattr_name_bytes);
    sb_appendf(sb, "},");

    if (cfg->include_xattrs) {
        sb_appendf(sb, "\"xattrs\":");
        append_xattrs_json(sb, cfg->path);
        sb_appendf(sb, ",");
    }

    sb_appendf(sb, "\"magenta\":{");
    sb_appendf(sb, "\"metadata_density_ppm\":%u,", sc.metadata_density_ppm);
    sb_appendf(sb, "\"freshness_ppm\":%u,", sc.freshness_ppm);
    sb_appendf(sb, "\"gpu_pressure_ppm\":%u,", sc.gpu_pressure_ppm);
    sb_appendf(sb, "\"path_weight_ppm\":%u,", sc.path_weight_ppm);
    sb_appendf(sb, "\"xattr_weight_ppm\":%u,", sc.xattr_weight_ppm);
    sb_appendf(sb, "\"campaign_weight_ppm\":%u,", sc.campaign_weight_ppm);
    sb_appendf(sb, "\"advertiser_weight_ppm\":%u,", sc.advertiser_weight_ppm);
    sb_appendf(sb, "\"magenta_interest_ppm\":%u,", sc.magenta_interest_ppm);
    sb_appendf(sb, "\"magenta_causal_ratio_ppm\":%u,", sc.magenta_causal_ratio_ppm);
    sb_appendf(sb, "\"magenta_quality_ppm\":%u", sc.magenta_quality_ppm);
    sb_appendf(sb, "}");

    sb_appendf(sb, "}");
}

#ifdef USE_CURL
static int post_payload(const char *endpoint, const char *api_key, const char *json) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char auth[1024];
    if (api_key && *api_key) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(rc));
        return -2;
    }

    if (code < 200 || code >= 300) {
        fprintf(stderr, "http error: %ld\n", code);
        return -3;
    }

    return 0;
}
#endif

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --path PATH --campaign ID --advertiser ID [--card card0] [--interval SEC] [--once] [--no-xattrs] [--no-stat] [--no-gpu]\n",
        argv0
    );
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
        } else {
            usage(argv[0]);
            exit(1);
        }
    }

    if (!cfg->campaign_id[0]) die("missing --campaign");
    if (!cfg->advertiser_id[0]) die("missing --advertiser");
}

int main(int argc, char **argv) {
    Config cfg;
    config_default(&cfg);
    parse_args(&cfg, argc, argv);

#ifdef USE_CURL
    const char *endpoint = getenv("MNTN_ENDPOINT");
    const char *api_key = getenv("MNTN_API_KEY");
#else
    const char *endpoint = NULL;
    const char *api_key = NULL;
#endif

    do {
        StrBuf sb;
        sb_init(&sb, JSON_BUF_CAP);
        build_payload_json(&sb, &cfg);

        puts(sb.data);

#ifdef USE_CURL
        if (endpoint && *endpoint) {
            int rc = post_payload(endpoint, api_key, sb.data);
            if (rc != 0) {
                fprintf(stderr, "post failed: %d\n", rc);
            }
        }
#else
        (void)endpoint;
        (void)api_key;
#endif

        sb_free(&sb);

        if (cfg.once) break;
        sleep((unsigned)cfg.interval_sec);
    } while (1);

    return 0;
}
