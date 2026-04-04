/*
 * runway_hook.c
 *
 * Minimal Runway API hook for C / libcurl.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 runway_hook.c -lcurl -o runway_hook
 *
 * Usage:
 *   export RUNWAYML_API_SECRET="key_..."
 *   ./runway_hook \
 *      --prompt "A cinematic slow dolly shot through fog" \
 *      --prompt-image "https://example.com/image.jpg"
 *
 *   # text-to-video mode
 *   ./runway_hook \
 *      --prompt "A serene mountain sunrise with mist in the valley"
 */

#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char prompt[2048];
    char prompt_image[2048];
    char model[64];
    char ratio[64];
    int duration;
    char task_id[256];
    bool poll_only;
} RunwayConfig;

typedef struct {
    char *ptr;
    size_t len;
} Memory;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void mem_init(Memory *m) {
    m->ptr = malloc(1);
    if (!m->ptr) die("malloc failed");
    m->ptr[0] = '\0';
    m->len = 0;
}

static void mem_free(Memory *m) {
    free(m->ptr);
    m->ptr = NULL;
    m->len = 0;
}

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    Memory *mem = (Memory *)userp;

    char *p = realloc(mem->ptr, mem->len + realsize + 1);
    if (!p) return 0;

    mem->ptr = p;
    memcpy(&(mem->ptr[mem->len]), contents, realsize);
    mem->len += realsize;
    mem->ptr[mem->len] = '\0';
    return realsize;
}

static char *json_escape(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n * 2 + 3);
    if (!out) die("malloc failed");
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        switch (c) {
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:   out[j++] = c; break;
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static int extract_json_string_field(const char *json, const char *field, char *out, size_t out_sz) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);

    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);

    size_t j = 0;
    while (*p && *p != '"' && j + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;
        out[j++] = *p++;
    }
    out[j] = '\0';
    return (j > 0) ? 0 : -1;
}

static int extract_json_status(const char *json, char *out, size_t out_sz) {
    return extract_json_string_field(json, "status", out, out_sz);
}

static int extract_json_id(const char *json, char *out, size_t out_sz) {
    return extract_json_string_field(json, "id", out, out_sz);
}

static int extract_first_output_url(const char *json, char *out, size_t out_sz) {
    const char *p = strstr(json, "\"output\":[");
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;

    size_t j = 0;
    while (*p && *p != '"' && j + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;
        out[j++] = *p++;
    }
    out[j] = '\0';
    return (j > 0) ? 0 : -1;
}

static long http_request(
    const char *method,
    const char *url,
    const char *api_key,
    const char *body,
    Memory *resp
) {
    CURL *curl = curl_easy_init();
    if (!curl) die("curl_easy_init failed");

    struct curl_slist *headers = NULL;
    char auth[4096];

    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-Runway-Version: 2024-11-06");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        die("curl error: %s", curl_easy_strerror(rc));
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return code;
}

static void cfg_init(RunwayConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->model, sizeof(cfg->model), "gen4.5");
    snprintf(cfg->ratio, sizeof(cfg->ratio), "1280:720");
    cfg->duration = 5;
}

static void parse_args(RunwayConfig *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prompt")) {
            if (++i >= argc) die("missing value for --prompt");
            snprintf(cfg->prompt, sizeof(cfg->prompt), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--prompt-image")) {
            if (++i >= argc) die("missing value for --prompt-image");
            snprintf(cfg->prompt_image, sizeof(cfg->prompt_image), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--model")) {
            if (++i >= argc) die("missing value for --model");
            snprintf(cfg->model, sizeof(cfg->model), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--ratio")) {
            if (++i >= argc) die("missing value for --ratio");
            snprintf(cfg->ratio, sizeof(cfg->ratio), "%s", argv[i]);
        } else if (!strcmp(argv[i], "--duration")) {
            if (++i >= argc) die("missing value for --duration");
            cfg->duration = atoi(argv[i]);
        } else if (!strcmp(argv[i], "--task-id")) {
            if (++i >= argc) die("missing value for --task-id");
            snprintf(cfg->task_id, sizeof(cfg->task_id), "%s", argv[i]);
            cfg->poll_only = true;
        } else {
            die("unknown arg: %s", argv[i]);
        }
    }

    if (!cfg->poll_only && cfg->prompt[0] == '\0') {
        die("missing --prompt");
    }
}

static void submit_task(const RunwayConfig *cfg, const char *api_key, char *task_id, size_t task_id_sz) {
    Memory resp;
    mem_init(&resp);

    char *esc_prompt = json_escape(cfg->prompt);
    char *esc_model = json_escape(cfg->model);
    char *esc_ratio = json_escape(cfg->ratio);
    char *esc_img = cfg->prompt_image[0] ? json_escape(cfg->prompt_image) : NULL;

    char body[8192];
    if (esc_img) {
        snprintf(body, sizeof(body),
            "{"
              "\"model\":%s,"
              "\"promptText\":%s,"
              "\"promptImage\":%s,"
              "\"ratio\":%s,"
              "\"duration\":%d"
            "}",
            esc_model, esc_prompt, esc_img, esc_ratio, cfg->duration
        );
    } else {
        snprintf(body, sizeof(body),
            "{"
              "\"model\":%s,"
              "\"promptText\":%s,"
              "\"ratio\":%s,"
              "\"duration\":%d"
            "}",
            esc_model, esc_prompt, esc_ratio, cfg->duration
        );
    }

    long code = http_request(
        "POST",
        "https://api.dev.runwayml.com/v1/image_to_video",
        api_key,
        body,
        &resp
    );

    if (code < 200 || code >= 300) {
        fprintf(stderr, "submit failed: HTTP %ld\n%s\n", code, resp.ptr);
        exit(1);
    }

    if (extract_json_id(resp.ptr, task_id, task_id_sz) != 0) {
        fprintf(stderr, "could not parse task id:\n%s\n", resp.ptr);
        exit(1);
    }

    printf("task_id=%s\n", task_id);

    free(esc_prompt);
    free(esc_model);
    free(esc_ratio);
    free(esc_img);
    mem_free(&resp);
}

static void poll_task(const char *api_key, const char *task_id) {
    char url[1024];
    snprintf(url, sizeof(url), "https://api.dev.runwayml.com/v1/tasks/%s", task_id);

    while (1) {
        Memory resp;
        mem_init(&resp);

        long code = http_request("GET", url, api_key, NULL, &resp);
        if (code < 200 || code >= 300) {
            fprintf(stderr, "poll failed: HTTP %ld\n%s\n", code, resp.ptr);
            mem_free(&resp);
            sleep(5);
            continue;
        }

        char status[128] = {0};
        extract_json_status(resp.ptr, status, sizeof(status));

        printf("status=%s\n", status[0] ? status : "UNKNOWN");

        if (!strcmp(status, "SUCCEEDED")) {
            char out[4096] = {0};
            if (extract_first_output_url(resp.ptr, out, sizeof(out)) == 0) {
                printf("output=%s\n", out);
            } else {
                printf("%s\n", resp.ptr);
            }
            mem_free(&resp);
            return;
        }

        if (!strcmp(status, "FAILED") || !strcmp(status, "CANCELED")) {
            printf("%s\n", resp.ptr);
            mem_free(&resp);
            exit(1);
        }

        mem_free(&resp);
        sleep(5);
    }
}

int main(int argc, char **argv) {
    const char *api_key = getenv("RUNWAYML_API_SECRET");
    if (!api_key || !*api_key) {
        die("RUNWAYML_API_SECRET is not set");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    RunwayConfig cfg;
    cfg_init(&cfg);
    parse_args(&cfg, argc, argv);

    char task_id[256] = {0};

    if (cfg.poll_only) {
        snprintf(task_id, sizeof(task_id), "%s", cfg.task_id);
    } else {
        submit_task(&cfg, api_key, task_id, sizeof(task_id));
    }

    poll_task(api_key, task_id);

    curl_global_cleanup();
    return 0;
}
