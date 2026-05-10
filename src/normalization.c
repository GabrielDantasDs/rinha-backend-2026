/*
 Simple, dependency-free helpers to build the 14-position vector described
 in the repo instructions.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include "normalization.h"

/* Normalization constants */
static float MAX_AMOUNT               = 10000.0f;
static float MAX_INSTALLMENTS         = 12.0f;
static float AMOUNT_VS_AVG_RATIO      = 10.0f;
static float MAX_MINUTES              = 1440.0f;
static float MAX_KM                   = 1000.0f;
static float MAX_TX_COUNT_24H         = 20.0f;
static float MAX_MERCHANT_AVG_AMOUNT  = 10000.0f;

/* In-memory MCC risk table — populated once at startup by normalization_init(). */
typedef struct {
    char  mcc[8];     /* MCC codes are up to 4 digits */
    float risk;
} mcc_entry_t;

#define MCC_TABLE_CAPACITY 1024
static mcc_entry_t g_mcc_table[MCC_TABLE_CAPACITY];
static int         g_mcc_count   = 0;
static float       g_mcc_default = 0.5f;

static int load_constants(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    char *p; double v;
#define LOAD(field, var) \
    p = strstr(buf, #field); \
    if (p && (p = strchr(p, ':')) && sscanf(p+1, " %lf", &v) == 1) var = (float)v;
    LOAD(max_amount,              MAX_AMOUNT)
    LOAD(max_installments,        MAX_INSTALLMENTS)
    LOAD(amount_vs_avg_ratio,     AMOUNT_VS_AVG_RATIO)
    LOAD(max_minutes,             MAX_MINUTES)
    LOAD(max_km,                  MAX_KM)
    LOAD(max_tx_count_24h,        MAX_TX_COUNT_24H)
    LOAD(max_merchant_avg_amount, MAX_MERCHANT_AVG_AMOUNT)
#undef LOAD
    return 0;
}

static int load_mcc_risk(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    g_mcc_count = 0;
    char *p = buf;
    while (g_mcc_count < MCC_TABLE_CAPACITY) {
        char *key_start = strchr(p, '"');
        if (!key_start) break;
        key_start++;
        char *key_end = strchr(key_start, '"');
        if (!key_end) break;

        size_t key_len = (size_t)(key_end - key_start);
        if (key_len == 0 || key_len >= sizeof(g_mcc_table[0].mcc)) {
            p = key_end + 1;
            continue;
        }

        char *colon = strchr(key_end, ':');
        if (!colon) break;
        double v = 0.5;
        if (sscanf(colon + 1, " %lf", &v) != 1) break;

        memcpy(g_mcc_table[g_mcc_count].mcc, key_start, key_len);
        g_mcc_table[g_mcc_count].mcc[key_len] = '\0';
        g_mcc_table[g_mcc_count].risk = (float)v;
        g_mcc_count++;

        /* advance past the value to the next key */
        p = colon + 1;
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == '\0' || *p == '}') break;
        p++;
    }
    return 0;
}

int normalization_init(const char *constants_path, const char *mcc_risk_path)
{
    int rc = 0;
    if (load_constants(constants_path) != 0) rc = -1;
    if (load_mcc_risk(mcc_risk_path)   != 0) rc = -1;
    return rc;
}

static float clampf(float x)
{
    if (x != x)  return 0.0f;   /* NaN */
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

/* ------------------------------------------------------------------ *
 * find_in_scope                                                         *
 *   Helper: find a key within a specific object scope.                 *
 *   For "merchant.avg_amount", finds the "avg_amount" key within       *
 *   the scope that comes after "merchant": { ... }                     *
 * ------------------------------------------------------------------ */
static const char *find_in_scope(const char *s, const char *scope, const char *key)
{
    /* Find the scope object first, e.g., "merchant": { */
    char scope_pattern[128];
    snprintf(scope_pattern, sizeof(scope_pattern), "\"%s\"", scope);
    const char *scope_pos = strstr(s, scope_pattern);
    if (!scope_pos) return NULL;

    /* Find the opening brace after the scope */
    scope_pos = strchr(scope_pos, '{');
    if (!scope_pos) return NULL;
    
    /* Now search for the key within this scope, until we hit the closing brace */
    const char *scope_start = scope_pos;
    int brace_depth = 1;
    const char *p = scope_pos + 1;
    
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
    
    while (*p && brace_depth > 0) {
        if (*p == '{') brace_depth++;
        else if (*p == '}') brace_depth--;
        
        if (brace_depth > 0 && (p = strstr(p, key_pattern)) != NULL) {
            /* Check if followed by colon */
            const char *after = p + strlen(key_pattern);
            while (*after && isspace((unsigned char)*after)) after++;
            if (*after == ':') {
                return p;
            }
            p++;
        } else {
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * extract_double                                                       *
 *   Finds a key anywhere in the JSON and parses the numeric value.    *
 *   Works with nested keys like "merchant.avg_amount" by searching    *
 *   within the correct object scope.                                  *
 * ------------------------------------------------------------------ */
static int extract_double(const char *s, const char *key, double *out)
{
    const char *dot = strchr(key, '.');
    const char *p = NULL;
    
    if (dot != NULL) {
        /* Dotted key: find within scope */
        char scope[128];
        size_t scope_len = dot - key;
        if (scope_len >= sizeof(scope)) return -1;
        memcpy(scope, key, scope_len);
        scope[scope_len] = '\0';
        
        const char *subkey = dot + 1;
        p = find_in_scope(s, scope, subkey);
    } else {
        /* Non-dotted key: search normally with quotes */
        char key_pattern[64];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
        p = strstr(s, key_pattern);
    }
    
    if (!p) return -1;
    
    /* Skip the key and find the colon */
    const char *colon = strchr(p, ':');
    if (!colon) return -1;
    
    colon++;
    while (*colon && isspace((unsigned char)*colon)) colon++;
    
    if (sscanf(colon, "%lf", out) == 1)
        return 0;
    
    return -1;
}

static int extract_bool(const char *s, const char *key, int *out)
{
    const char *dot = strchr(key, '.');
    const char *p = NULL;
    
    if (dot != NULL) {
        /* Dotted key: find within scope */
        char scope[128];
        size_t scope_len = dot - key;
        if (scope_len >= sizeof(scope)) return -1;
        memcpy(scope, key, scope_len);
        scope[scope_len] = '\0';
        
        const char *subkey = dot + 1;
        p = find_in_scope(s, scope, subkey);
    } else {
        /* Non-dotted key: search normally with quotes */
        char key_pattern[64];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
        p = strstr(s, key_pattern);
    }
    
    if (!p) return -1;
    
    /* Skip the key and find the colon */
    const char *colon = strchr(p, ':');
    if (!colon) return -1;
    
    colon++;
    while (*colon && isspace((unsigned char)*colon)) colon++;
    
    if (strncmp(colon, "true",  4) == 0) { *out = 1; return 0; }
    if (strncmp(colon, "false", 5) == 0) { *out = 0; return 0; }
    
    return -1;
}

static int extract_string(const char *s, const char *key, char *out, size_t out_sz)
{
    const char *dot = strchr(key, '.');
    const char *p = NULL;
    
    if (dot != NULL) {
        /* Dotted key: find within scope */
        char scope[128];
        size_t scope_len = dot - key;
        if (scope_len >= sizeof(scope)) return -1;
        memcpy(scope, key, scope_len);
        scope[scope_len] = '\0';
        
        const char *subkey = dot + 1;
        p = find_in_scope(s, scope, subkey);
    } else {
        /* Non-dotted key: search normally with quotes */
        char key_pattern[64];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
        p = strstr(s, key_pattern);
    }
    
    if (!p) return -1;
    
    /* Skip the key and find the colon */
    const char *colon = strchr(p, ':');
    if (!colon) return -1;
    
    colon++;
    while (*colon && isspace((unsigned char)*colon)) colon++;
    
    /* Expected to find a quoted string value */
    if (*colon != '"') return -1;
    
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return -1;
    
    size_t len = (size_t)(end - colon);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, colon, len);
    out[len] = '\0';
    
    return 0;
}

/* ------------------------------------------------------------------ *
 * is_in_array                                                           *
 *   Check if a string value appears in a JSON array within a scope.    *
 *   E.g., checks if "MERC-016" is in customer.known_merchants array   *
 * ------------------------------------------------------------------ */
static int is_in_array(const char *s, const char *scope, const char *array_key, const char *value)
{
    if (!s || !scope || !array_key || !value) return 0;
    
    /* Find the scope object first, e.g., "customer": { */
    char scope_pattern[128];
    snprintf(scope_pattern, sizeof(scope_pattern), "\"%s\"", scope);
    const char *scope_pos = strstr(s, scope_pattern);
    if (!scope_pos) return 0;

    /* Find the opening brace after the scope */
    scope_pos = strchr(scope_pos, '{');
    if (!scope_pos) return 0;
    
    /* Search for the array key within this scope */
    int brace_depth = 1;
    const char *p = scope_pos + 1;
    
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", array_key);
    
    const char *array_start = NULL;
    while (*p && brace_depth > 0) {
        if (*p == '{') brace_depth++;
        else if (*p == '}') brace_depth--;
        
        if (brace_depth > 0 && (p = strstr(p, key_pattern)) != NULL) {
            /* Check if followed by colon and opening bracket */
            const char *after = p + strlen(key_pattern);
            while (*after && isspace((unsigned char)*after)) after++;
            if (*after == ':') {
                after++;
                while (*after && isspace((unsigned char)*after)) after++;
                if (*after == '[') {
                    array_start = after;
                    break;
                }
            }
            p++;
        } else {
            break;
        }
    }
    
    if (!array_start) return 0;
    
    /* Now search for the value within the array, until closing bracket */
    char value_pattern[256];
    snprintf(value_pattern, sizeof(value_pattern), "\"%s\"", value);
    
    p = array_start + 1;
    int bracket_depth = 1;
    
    while (*p && bracket_depth > 0) {
        if (*p == '[') bracket_depth++;
        else if (*p == ']') bracket_depth--;
        
        if (bracket_depth > 0 && strstr(p, value_pattern) == p) {
            /* Found the value in the array */
            return 1;
        }
        p++;
    }
    
    return 0;
}

static float lookup_mcc_risk(const char *mcc)
{
    /* Linear scan over the in-memory table loaded at startup.
     * Tiny (~10 entries today, capped at 1024), so this beats hashing on
     * cache locality and code size. */
    for (int i = 0; i < g_mcc_count; i++) {
        if (strcmp(g_mcc_table[i].mcc, mcc) == 0) {
            return g_mcc_table[i].risk;
        }
    }
    return g_mcc_default;
}

static int parse_time_fields(const char *s, int *out_hour, int *out_wday)
{
    char ts[64];
    if (extract_string(s, "requested_at", ts, sizeof(ts)) != 0)
        return -1;
    struct tm tm = {0};
    char *res = strptime(ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (!res) res = strptime(ts, "%Y-%m-%dT%H:%M:%S", &tm);
    if (res) {
        time_t t = timegm(&tm);
        struct tm *g = gmtime(&t);
        *out_hour = g->tm_hour;
        int w = g->tm_wday;
        *out_wday = (w == 0) ? 6 : (w - 1);
        return 0;
    }
    const char *tpos = strchr(ts, 'T');
    if (!tpos) return -1;
    int hour = -1;
    if (sscanf(tpos, "T%2d:", &hour) == 1) {
        *out_hour = hour % 24;
        *out_wday = 0;
        return 0;
    }
    return -1;
}

int create_vector_from_request(char *request, float *vector)
{
    if (!request || !vector) return -1;
    for (int i = 0; i < 14; ++i) vector[i] = 0.0f;

    double amount = 0, installments = 0, customer_avg = 0;
    double terminal_km_from_home = 0, tx_count_24h = 0;
    double merchant_avg_amount = 0;
    int terminal_is_online = 0, terminal_card_present = 0;
    char merchant_id[128] = {0};
    char merchant_mcc[64]  = {0};

    if (extract_double(request, "amount", &amount) != 0)
        return -1;
    extract_double(request, "installments",         &installments);
    extract_double(request, "customer.avg_amount",  &customer_avg);
    extract_double(request, "terminal.km_from_home",&terminal_km_from_home);
    extract_double(request, "customer.tx_count_24h",&tx_count_24h);
    extract_double(request, "merchant.avg_amount",  &merchant_avg_amount);
    extract_bool  (request, "terminal.is_online",   &terminal_is_online);
    extract_bool  (request, "terminal.card_present",&terminal_card_present);
    extract_string(request, "merchant.id",   merchant_id, sizeof(merchant_id));
    extract_string(request, "merchant.mcc",  merchant_mcc, sizeof(merchant_mcc));

    /* 0: amount */
    vector[0] = (float)(amount / MAX_AMOUNT);

    /* 1: installments */
    vector[1] = (float)(installments / MAX_INSTALLMENTS);

    /* 2: amount_vs_avg */
    vector[2] = (customer_avg > 0.0)
        ? (float)((amount / customer_avg) / AMOUNT_VS_AVG_RATIO)
        : 0.0f;

    /* 3,4: hour_of_day, day_of_week */
    int hour = 0, wday = 0;
    if (parse_time_fields(request, &hour, &wday) == 0) {
        vector[3] = (float)hour / 23.0f;
        vector[4] = (float)wday / 6.0f;
    }

    /* 5,6: minutes_since_last_tx, km_from_last_tx
     * Use -1 sentinel when last_transaction is null (per DETECTION_RULES.md) */
    double minutes_since = 0, km_from_last = 0;
    int has_minutes = (extract_double(request, "last_transaction.minutes_since", &minutes_since) == 0);
    int has_km      = (extract_double(request, "last_transaction.km_from_current", &km_from_last) == 0);

    vector[5] = has_minutes ? (float)(minutes_since / MAX_MINUTES) : -1.0f;
    vector[6] = has_km      ? (float)(km_from_last  / MAX_KM)      : -1.0f;

    /* 7: km_from_home */
    vector[7] = (float)(terminal_km_from_home / MAX_KM);

    /* 8: tx_count_24h */
    vector[8] = (float)(tx_count_24h / MAX_TX_COUNT_24H);

    /* 9: is_online */
    vector[9]  = terminal_is_online   ? 1.0f : 0.0f;

    /* 10: card_present */
    vector[10] = terminal_card_present ? 1.0f : 0.0f;

    /* 11: unknown_merchant */
    /* Check if merchant_id is in customer.known_merchants array */
    vector[11] = 1.0f;  /* assume unknown by default */
    if (merchant_id[0] != '\0') {
        if (is_in_array(request, "customer", "known_merchants", merchant_id)) {
            vector[11] = 0.0f;  /* merchant is known */
        }
    }

    /* 12: mcc_risk */
    vector[12] = (merchant_mcc[0] != '\0') ? lookup_mcc_risk(merchant_mcc) : 0.5f;

    /* 13: merchant_avg_amount */
    vector[13] = (float)(merchant_avg_amount / MAX_MERCHANT_AVG_AMOUNT);

    return normalize_vector(vector, 14);
}

int normalize_vector(float *vector, int size)
{
    if (!vector || size != 14) return -1;

    /* clamp all to [0, 1] EXCEPT indices 5,6 which may be -1 */
    for (int i = 0; i < size; i++) {
        if (i != 5 && i != 6) {
            vector[i] = clampf(vector[i]);
        }
    }

    return 0;
}