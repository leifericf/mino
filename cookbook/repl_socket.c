/*
 * repl_socket.c — a REPL served over a TCP socket.
 *
 * Demonstrates: the in-process REPL handle (mino_repl_*), I/O opt-in,
 * hosting a REPL from an event loop without threads.
 *
 * Build:  cc -std=c99 -I.. -o repl_socket repl_socket.c ../mino.c
 * Run:    ./repl_socket        (listens on port 7100)
 *         rlwrap nc 127.0.0.1 7100
 *
 * Single-client for simplicity; a real host would use poll/epoll/kqueue.
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 7100
#define BUFSZ 4096

static void send_str(int fd, const char *s)
{
    size_t len = strlen(s);
    (void)write(fd, s, len);
}

static void send_prompt(int fd, int continuation)
{
    send_str(fd, continuation ? "..> " : "=> ");
}

int main(void)
{
    int         srv, cli;
    struct sockaddr_in addr;
    mino_env_t *env;
    mino_repl_t *repl;
    char         buf[BUFSZ];

    /* Set up the listening socket. */
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    {
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(srv, 1);
    printf("listening on 127.0.0.1:%d\n", PORT);

    /* Accept one client. */
    cli = accept(srv, NULL, NULL);
    if (cli < 0) { perror("accept"); return 1; }
    printf("client connected\n");

    /* Create the mino runtime with full capabilities. */
    mino_state_t *S = mino_state_new();
    env = mino_new(S);
    mino_install_io(S, env);
    repl = mino_repl_new(S, env);

    send_str(cli, "mino REPL\n");
    send_prompt(cli, 0);

    /* Read lines and feed them to the REPL. */
    for (;;) {
        ssize_t n = read(cli, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';

        /* Feed line-by-line. */
        {
            char *line = buf;
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                mino_val_t *out = NULL;
                int         rc;
                *nl = '\0';
                rc = mino_repl_feed(repl, line, &out);
                switch (rc) {
                case MINO_REPL_OK:
                    if (out != NULL) {
                        /* Print the result back to the client. */
                        char repr[4096];
                        int  len;
                        /* Use a tmpfile to capture printed output. */
                        {
                            FILE *tmp = tmpfile();
                            if (tmp != NULL) {
                                mino_print_to(S, tmp, out);
                                len = (int)ftell(tmp);
                                if (len > 0 && len < (int)sizeof(repr)) {
                                    rewind(tmp);
                                    (void)fread(repr, 1, (size_t)len, tmp);
                                    repr[len] = '\0';
                                    send_str(cli, repr);
                                }
                                fclose(tmp);
                            }
                        }
                        send_str(cli, "\n");
                    }
                    send_prompt(cli, 0);
                    break;
                case MINO_REPL_MORE:
                    send_prompt(cli, 1);
                    break;
                case MINO_REPL_ERROR:
                    send_str(cli, "error: ");
                    send_str(cli, mino_last_error(S));
                    send_str(cli, "\n");
                    send_prompt(cli, 0);
                    break;
                }
                line = nl + 1;
            }
        }
    }

    printf("client disconnected\n");
    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    close(cli);
    close(srv);
    return 0;
}
