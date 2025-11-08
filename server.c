// server.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/dispatch.h>

#define SERVER_NAME "TempServer"
#define SHM_NAME "/temp_stats_shm"
#define MAX_SAMPLES 1024
#define PERIOD_SECONDS 5   // periodic stats every 5 seconds

typedef struct {
    int type; // not used, placeholder
    pid_t pid;
    double temp;
} temp_msg_t;

typedef struct {
    pthread_mutex_t mutex; // to protect stats and clients reading
    double avg;
    double minimum;
    double maximum;
    int count;
    time_t last_updated;
} shared_stats_t;

/* in-server circular buffer of last samples */
typedef struct {
    double samples[MAX_SAMPLES];
    int head;
    int count;
    pthread_mutex_t buf_mutex;
} circbuf_t;

static circbuf_t buffer;
static shared_stats_t *shm_stats = NULL;

/* helper to initialize circular buffer */
static void buffer_init(circbuf_t *b) {
    b->head = 0;
    b->count = 0;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_PRIVATE);
    pthread_mutex_init(&b->buf_mutex, &a);
    pthread_mutexattr_destroy(&a);
}

/* push sample */
static void buffer_push(circbuf_t *b, double val) {
    pthread_mutex_lock(&b->buf_mutex);
    b->samples[b->head] = val;
    b->head = (b->head + 1) % MAX_SAMPLES;
    if (b->count < MAX_SAMPLES) b->count++;
    pthread_mutex_unlock(&b->buf_mutex);
}

/* compute stats over current buffer content */
static void buffer_compute_stats(circbuf_t *b, double *avg, double *min, double *max, int *count) {
    pthread_mutex_lock(&b->buf_mutex);
    if (b->count == 0) {
        *avg = 0.0;
        *min = 0.0;
        *max = 0.0;
        *count = 0;
        pthread_mutex_unlock(&b->buf_mutex);
        return;
    }
    double sum = 0.0;
    double mm = b->samples[0];
    double Mx = b->samples[0];
    for (int i = 0; i < b->count; ++i) {
        double v = b->samples[(b->head + MAX_SAMPLES - b->count + i) % MAX_SAMPLES];
        sum += v;
        if (v < mm) mm = v;
        if (v > Mx) Mx = v;
    }
    *avg = sum / b->count;
    *min = mm;
    *max = Mx;
    *count = b->count;
    pthread_mutex_unlock(&b->buf_mutex);
}

/* Timer thread - wakes up every PERIOD_SECONDS and writes computed stats to shared memory */
static void *timer_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(PERIOD_SECONDS);

        double avg, min, max;
        int count;
        buffer_compute_stats(&buffer, &avg, &min, &max, &count);

        /* write to shared memory protected by its mutex */
        pthread_mutex_lock(&shm_stats->mutex);
        shm_stats->avg = avg;
        shm_stats->minimum = min;
        shm_stats->maximum = max;
        shm_stats->count = count;
        shm_stats->last_updated = time(NULL);
        pthread_mutex_unlock(&shm_stats->mutex);

        printf("[server] Periodic stats: count=%d avg=%.3f min=%.3f max=%.3f\n",
               count, avg, min, max);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    printf("Starting temp_server...\n");

    /* 1) Initialize buffer */
    buffer_init(&buffer);

    /* 2) Create/open POSIX shared memory and map it */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }
    size_t shm_size = sizeof(shared_stats_t);
    if (ftruncate(shm_fd, shm_size) == -1) {
        perror("ftruncate");
        return 1;
    }
    void *addr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shm_stats = (shared_stats_t *)addr;

    /* Initialize shared_stats: mutex must be process-shared */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm_stats->mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);

    shm_stats->avg = 0;
    shm_stats->minimum = 0;
    shm_stats->maximum = 0;
    shm_stats->count = 0;
    shm_stats->last_updated = time(NULL);

    /* 3) Create named channel so clients can connect using name_open() */
    name_attach_t *attach = name_attach(NULL, SERVER_NAME, 0);
    if (!attach) {
        perror("name_attach");
        return 1;
    }
    printf("[server] Name attached as '%s'\n", SERVER_NAME);

    /* 4) Launch timer thread to periodically compute stats and update shared memory */
    pthread_t tid;
    if (pthread_create(&tid, NULL, timer_thread, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }
    pthread_detach(tid);

    /* 5) Main message loop: receive temp_msg_t from clients */
    while (1) {
        temp_msg_t rmsg;
        memset(&rmsg, 0, sizeof(rmsg));
        int rcvid = MsgReceive(attach->chid, &rmsg, sizeof(rmsg), NULL);
        if (rcvid == -1) {
            perror("MsgReceive");
            continue;
        }
        /* Basic validation */
        printf("[server] Received from pid=%d temp=%.3f\n", rmsg.pid, rmsg.temp);
        fflush(stdout);

        /* Store sample in server buffer */
        buffer_push(&buffer, rmsg.temp);

        /* Optionally update shared memory immediately (not necessary if timer updates) */
        pthread_mutex_lock(&shm_stats->mutex);
        /* Keep last_updated quickly accessible */
        shm_stats->last_updated = time(NULL);
        pthread_mutex_unlock(&shm_stats->mutex);

        /* Prepare reply */
        char reply_msg[128];
        int written = snprintf(reply_msg, sizeof(reply_msg), "ACK: received %.3f from pid %d", rmsg.temp, rmsg.pid);
        MsgReply(rcvid, EOK, reply_msg, written + 1);
    }

    /* Cleanup (never reached in this sample) */
    name_detach(attach, 0);
    munmap(addr, shm_size);
    close(shm_fd);
    shm_unlink(SHM_NAME);
    return 0;
}
