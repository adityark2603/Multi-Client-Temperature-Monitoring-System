// client.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <sys/dispatch.h>

#define SERVER_NAME "TempServer"

typedef struct {
    int type;
    pid_t pid;
    double temp;
} temp_msg_t;

int main(int argc, char *argv[]) {
    double interval = 1.0; // send interval in seconds
    if (argc >= 2) interval = atof(argv[1]);

    /* open the server name */
    int coid = name_open(SERVER_NAME, 0);
    if (coid == -1) {
        perror("name_open");
        fprintf(stderr, "Make sure server is running and name '%s' exists\n", SERVER_NAME);
        return 1;
    }

    printf("Client started (pid=%d). Sending every %.2f seconds. Ctrl-C to stop.\n", getpid(), interval);

    srand((unsigned)time(NULL) ^ getpid());

    while (1) {
        temp_msg_t msg;
        msg.type = 1;
        msg.pid = getpid();

        /* produce a random temperature reading (for testing)
           let's simulate between 15.0 and 40.0 degC */
        double r = ((double)rand() / RAND_MAX);
        msg.temp = 15.0 + r * (40.0 - 15.0);

        char rbuf[128];
        int status = MsgSend(coid, &msg, sizeof(msg), rbuf, sizeof(rbuf));
        if (status == -1) {
            perror("MsgSend");
            break;
        }
        printf("[client %d] Sent %.3f -> reply: %s\n", msg.pid, msg.temp, rbuf);
        fflush(stdout);

        /* sleep for 'interval' seconds */
        struct timespec ts;
        ts.tv_sec = (time_t)interval;
        ts.tv_nsec = (long)((interval - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }

    name_close(coid);
    return 0;
}
