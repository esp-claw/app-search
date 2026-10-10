/*
 * SPDX-FileCopyrightText: 2026 esp-claw contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * search: web search through the device's "tavily" or "brave" credential profile.
 *   search_web   query, max_results (1-10, default 5), topic (general | news | finance)
 *   search_news  query, max_results: same as search_web with topic "news"
 * Tavily is tried first, Brave Search is the fallback. Results are compact JSON:
 * {"provider":"tavily","results":[{"title","url","snippet"}]}, snippets cut to 300 bytes.
 * The device injects the API keys; the app never sees them.
 */
#define CLAW_ARENA_SIZE 2048 /* largest accepted argument JSON */
#include "claw.h"
#include "claw_util.h"

CLAW_DEFINE_ARENA()

#define QUERY_MAX     400  /* raw (JSON-escaped) query bytes */
#define SNIPPET_MAX   300
#define RESULTS_MAX   10
#define RESULTS_DEF   5
#define CLOSE_RESERVE 64

static int32_t s_web = -1;
static int32_t s_news = -1;
static char s_resp[32 * 1024 + 1];
static uint32_t s_resp_len;
static char s_out[16 * 1024];
static char s_req[3 * QUERY_MAX + 256];

#define SCHEMA_QUERY                                                                           \
    "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"                        \
    "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10,\"description\":\"Default 5\"}"

CLAW_EXPORT("app_init") void app_init(void)
{
    s_web = CLAW_REGISTER("web", "Search the web. Returns title, url and a short snippet per result.",
                          "{\"type\":\"object\",\"properties\":{" SCHEMA_QUERY ","
                          "\"topic\":{\"type\":\"string\",\"enum\":[\"general\",\"news\",\"finance\"],"
                          "\"description\":\"Default general\"}},\"required\":[\"query\"]}");
    s_news = CLAW_REGISTER("news", "Search recent news articles. Returns title, url and a short snippet per result.",
                           "{\"type\":\"object\",\"properties\":{" SCHEMA_QUERY "},\"required\":[\"query\"]}");
}

static int32_t request(const char *profile, const char *method, const char *url, uint32_t url_len,
                       const char *headers, const char *body, uint32_t body_len)
{
    int32_t rc = claw_http_profile_request(CLAW_S(profile), CLAW_S(method), url, url_len, CLAW_S(headers),
                                           body, body_len);
    int32_t n;

    s_resp_len = 0;
    while ((n = claw_http_response_read(s_resp + s_resp_len, sizeof(s_resp) - 1 - s_resp_len)) > 0) {
        s_resp_len += (uint32_t)n;
    }
    s_resp[s_resp_len] = '\0';
    return rc;
}

/* Raw contents of the string value at v (escapes kept), or n = 0. */
static const char *raw_string(const char *v, const char *end, uint32_t *n)
{
    const char *close = v && v < end && *v == '"' ? cj_skip(v, end) : 0;

    *n = close ? (uint32_t)(close - v - 2) : 0;
    return close ? v + 1 : "";
}

static uint32_t hex4(const char *p)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        char c = p[i];

        v = v * 16 + (uint32_t)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* Appends the JSON string contents `s` (n bytes) URL-encoded: escapes decoded, UTF-8 percent-encoded. */
static void put_url_encoded(claw_buf_t *b, const char *s, uint32_t n)
{
    static const char hex[] = "0123456789ABCDEF";

    for (uint32_t i = 0; i < n; i++) {
        unsigned char utf8[4];
        uint32_t len = 1;
        uint32_t cp;

        utf8[0] = (unsigned char)s[i];
        if (s[i] == '\\' && i + 1 < n) {
            char e = s[++i];

            if (e == 'u' && i + 4 < n) {
                cp = hex4(s + i + 1);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < n && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4(s + i + 3) - 0xDC00);
                    i += 6;
                }
                if (cp < 0x80) {
                    utf8[0] = (unsigned char)cp;
                } else if (cp < 0x800) {
                    utf8[0] = (unsigned char)(0xC0 | cp >> 6);
                    utf8[1] = (unsigned char)(0x80 | (cp & 0x3F));
                    len = 2;
                } else if (cp < 0x10000) {
                    utf8[0] = (unsigned char)(0xE0 | cp >> 12);
                    utf8[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
                    utf8[2] = (unsigned char)(0x80 | (cp & 0x3F));
                    len = 3;
                } else {
                    utf8[0] = (unsigned char)(0xF0 | cp >> 18);
                    utf8[1] = (unsigned char)(0x80 | (cp >> 12 & 0x3F));
                    utf8[2] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
                    utf8[3] = (unsigned char)(0x80 | (cp & 0x3F));
                    len = 4;
                }
            } else {
                utf8[0] = (unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' :
                                          e == 'f' ? '\f' : e);
            }
        }
        for (uint32_t k = 0; k < len; k++) {
            unsigned char c = utf8[k];

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~') {
                cb_putn(b, (const char *)&c, 1);
            } else {
                char pct[3] = { '%', hex[c >> 4], hex[c & 0xF] };

                cb_putn(b, pct, 3);
            }
        }
    }
}

/* Appends at most `max` bytes of the raw JSON string contents `s`, never splitting an escape
 * sequence or a UTF-8 character; "…" marks a cut. */
static void put_cut(claw_buf_t *b, const char *s, uint32_t n, uint32_t max)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t len = 1;

        if (s[i] == '\\') {
            len = i + 1 < n && s[i + 1] == 'u' ? 6 : 2;
        } else {
            while (i + len < n && ((unsigned char)s[i + len] & 0xC0) == 0x80) {
                len++;
            }
        }
        if (i + len > max) {
            cb_putn(b, s, i);
            cb_put(b, "\xE2\x80\xA6");
            return;
        }
        i += len;
    }
    cb_putn(b, s, n < i ? n : i);
}

/* Appends {"title","url","snippet"} entries from the array `list` that fit completely. */
static uint32_t put_results(claw_buf_t *b, const char *list, const char *snippet_key, int64_t max)
{
    const char *end = s_resp + s_resp_len;
    uint32_t cap = b->cap;
    uint32_t count = 0;

    b->cap = cap - CLOSE_RESERVE;
    for (const char *item = cj_first(list, end); item && count < max && !b->full; item = cj_next(item, end)) {
        uint32_t mark = b->len;
        uint32_t n;
        const char *s;

        cb_put(b, count ? ",{\"title\":\"" : "{\"title\":\"");
        s = raw_string(cj_member(item, end, "title"), end, &n);
        put_cut(b, s, n, SNIPPET_MAX);
        cb_put(b, "\",\"url\":\"");
        s = raw_string(cj_member(item, end, "url"), end, &n);
        cb_putn(b, s, n);
        cb_put(b, "\",\"snippet\":\"");
        s = raw_string(cj_member(item, end, snippet_key), end, &n);
        put_cut(b, s, n, SNIPPET_MAX);
        cb_put(b, "\"}");
        if (b->full) {
            b->len = mark;
            b->buf[mark] = '\0';
            break;
        }
        count++;
    }
    b->cap = cap;
    return count;
}

static int32_t search_tavily(const char *query, uint32_t query_len, int64_t max, const char *topic)
{
    claw_buf_t body = { s_req, sizeof(s_req), 0, 0 };

    cb_put(&body, "{\"query\":\"");
    cb_putn(&body, query, query_len);
    cb_put(&body, "\",\"max_results\":");
    cb_put_u32(&body, (uint32_t)max);
    cb_put(&body, ",\"topic\":\"");
    cb_put(&body, topic);
    cb_put(&body, "\",\"search_depth\":\"basic\",\"include_answer\":false}");
    /* A POST: the device lets it through without confirmation because the tavily profile lists
     * "/search" in read_only_posts. */
    return request("tavily", "POST", CLAW_S("/search"), "{\"Content-Type\":\"application/json\"}", body.buf,
                   body.len);
}

static int32_t search_brave(const char *query, uint32_t query_len, int64_t max, int news)
{
    claw_buf_t url = { s_req, sizeof(s_req), 0, 0 };

    cb_put(&url, news ? "/res/v1/news/search?q=" : "/res/v1/web/search?result_filter=web&q=");
    put_url_encoded(&url, query, query_len);
    cb_put(&url, "&count=");
    cb_put_u32(&url, (uint32_t)max);
    cb_put(&url, "&text_decorations=false");
    return request("brave", "GET", url.buf, url.len, "{\"Accept\":\"application/json\"}", "", 0);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void put_int(claw_buf_t *b, int32_t v)
{
    if (v < 0) {
        cb_put(b, "-");
        v = -v;
    }
    cb_put_u32(b, (uint32_t)v);
}

static void tool_search(const char *args, const char *end, int news)
{
    claw_buf_t b = { s_out, sizeof(s_out), 0, 0 };
    uint32_t query_len;
    const char *query = raw_string(cj_member(args, end, "query"), end, &query_len);
    int64_t max = cj_uint(cj_member(args, end, "max_results"), end);
    char topic_buf[16];
    const char *topic = news ? "news" : "general";
    const char *resp_end;
    int32_t tavily;
    int32_t brave;

    if (query_len == 0 || query_len > QUERY_MAX) {
        CLAW_RESULT("{\"error\":\"query is required (at most 400 bytes)\"}");
        return;
    }
    if (max < 1 || max > RESULTS_MAX) {
        max = RESULTS_DEF;
    }
    if (!news && cj_member(args, end, "topic")) {
        topic = topic_buf;
        if (cj_string(cj_member(args, end, "topic"), end, topic_buf, sizeof(topic_buf)) < 0 ||
                (!streq(topic, "general") && !streq(topic, "news") && !streq(topic, "finance"))) {
            CLAW_RESULT("{\"error\":\"topic must be general, news or finance\"}");
            return;
        }
    }

    tavily = search_tavily(query, query_len, max, topic);
    resp_end = s_resp + s_resp_len;
    if (tavily == 200) {
        cb_put(&b, "{\"provider\":\"tavily\",\"results\":[");
        put_results(&b, cj_member(s_resp, resp_end, "results"), "content", max);
        cb_put(&b, "]}");
        claw_tool_result(b.buf, b.len);
        return;
    }

    brave = search_brave(query, query_len, max, streq(topic, "news"));
    resp_end = s_resp + s_resp_len;
    if (brave == 200) {
        const char *list = streq(topic, "news") ? cj_member(s_resp, resp_end, "results") :
                           cj_member(cj_member(s_resp, resp_end, "web"), resp_end, "results");

        cb_put(&b, "{\"provider\":\"brave\",\"results\":[");
        put_results(&b, list, "description", max);
        cb_put(&b, "]}");
        claw_tool_result(b.buf, b.len);
        return;
    }

    /* Statuses are HTTP codes, or -2 when the profile is not configured. */
    cb_put(&b, "{\"error\":\"search failed\",\"tavily_status\":");
    put_int(&b, tavily);
    cb_put(&b, ",\"brave_status\":");
    put_int(&b, brave);
    cb_put(&b, ",\"detail\":\"");
    cb_put_escaped(&b, s_resp, s_resp_len < 300 ? s_resp_len : 300);
    cb_put(&b, "\"}");
    claw_tool_result(b.buf, b.len);
}

CLAW_EXPORT("tool_call") int32_t tool_call(int32_t tool, const char *args, uint32_t len)
{
    if (tool == s_web) {
        tool_search(args, args + len, 0);
    } else if (tool == s_news) {
        tool_search(args, args + len, 1);
    } else {
        return -1;
    }
    return 0;
}
