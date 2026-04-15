/*
 * config.c — load structured configuration from a .mino file.
 *
 * Demonstrates: sandboxed eval (no I/O), extracting C values from a map,
 * using keywords as config keys.
 *
 * Build:  cc -std=c99 -I.. -o config config.c ../mino.c
 * Run:    ./config
 */

#include "mino.h"
#include <stdio.h>

/*
 * A .mino config file is just a map literal:
 *
 *   {:port 8080
 *    :host "0.0.0.0"
 *    :debug true
 *    :workers 4
 *    :routes [{:path "/api" :handler "api_handler"}
 *             {:path "/"    :handler "static_handler"}]}
 */
static const char *config_src =
    "{:port 8080\n"
    " :host \"0.0.0.0\"\n"
    " :debug true\n"
    " :workers 4\n"
    " :routes [{:path \"/api\" :handler \"api_handler\"}\n"
    "          {:path \"/\"    :handler \"static_handler\"}]}";

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_env_new(S);
    mino_val_t *cfg;
    mino_val_t *val;

    /* Core bindings only — no I/O. The config file cannot read files,
     * print, or access the network. */
    mino_install_core(S, env);

    /* Evaluate the config source. */
    cfg = mino_eval_string(S, config_src, env);
    if (cfg == NULL) {
        fprintf(stderr, "config error: %s\n", mino_last_error(S));
        return 1;
    }

    /* Bind the config map so we can query it from mino expressions. */
    mino_env_set(S, env, "cfg", cfg);
    val = mino_eval_string(S, "(get cfg :port)", env);
    if (val != NULL) {
        long long port;
        if (mino_to_int(val, &port)) {
            printf("port: %lld\n", port);
        }
    }

    val = mino_eval_string(S, "(get cfg :host)", env);
    if (val != NULL) {
        const char *host;
        size_t      len;
        if (mino_to_string(val, &host, &len)) {
            printf("host: %.*s\n", (int)len, host);
        }
    }

    val = mino_eval_string(S, "(get cfg :debug)", env);
    if (val != NULL) {
        printf("debug: %s\n", mino_to_bool(val) ? "on" : "off");
    }

    /* Iterate over routes. */
    val = mino_eval_string(S, "(get cfg :routes)", env);
    if (val != NULL) {
        long long count;
        mino_val_t *c = mino_eval_string(S, "(count (get cfg :routes))", env);
        if (c != NULL && mino_to_int(c, &count)) {
            long long i;
            printf("routes (%lld):\n", count);
            for (i = 0; i < count; i++) {
                char expr[128];
                mino_val_t *route;
                mino_val_t *path;
                snprintf(expr, sizeof(expr),
                         "(nth (get cfg :routes) %lld)", i);
                route = mino_eval_string(S, expr, env);
                if (route == NULL) continue;
                mino_env_set(S, env, "__r", route);
                path = mino_eval_string(S, "(get __r :path)", env);
                if (path != NULL) {
                    const char *s;
                    size_t      n;
                    if (mino_to_string(path, &s, &n)) {
                        printf("  %.*s\n", (int)n, s);
                    }
                }
            }
        }
    }

    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
