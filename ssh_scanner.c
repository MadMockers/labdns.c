
#define _BSD_SOURCE

#include <netinet/in.h>

#include <arpa/inet.h>

#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

static uint32_t network, mask, cur_ip;
static pthread_mutex_t cur_ip_mtx = PTHREAD_MUTEX_INITIALIZER;
static long ms_timeout;

char **exec = NULL;

static inline uint32_t get_next_ip_locked(void)
{
    /* check if we are at broadcast addr */
    if(((cur_ip+1)&~mask) == 0)
    {
        return 0;
    }
    return htonl(cur_ip++);
}

static uint32_t get_next_ip(void)
{

    uint32_t result;
    pthread_mutex_lock(&cur_ip_mtx);
    result = get_next_ip_locked();
    pthread_mutex_unlock(&cur_ip_mtx);
    return result;
}

static void on_found(char *ip)
{
    if(!exec)
        return;

    int argc = 0;
    char **p, **argv = exec;
    while(*(argv++)) argc++;
    argc++;

    if(!argc)
        return;

    p = argv = malloc(sizeof(char**)*argc);
    memcpy(argv, exec, sizeof(char**)*argc);

    int free_count = 0;
    char *free_array[32];

    for(;*p && free_count < 32;p++)
    {
        if(strcmp(*p, "{}") == 0)
        {
            *p = ip;
        }
        else
        {
            char *pp = strdup(*p);
            char *freeme = pp;
            char *strs[64];
            int len = 0;
            int count = 0;
            int i;
            strs[count++] = pp;
            for(;*pp && count < 64;pp++)
            {
                if(*pp == '\\')
                    continue;
                if( *pp == '{' &&
                    *(pp+1) == '}')
                {
                    *pp = '\0';
                    strs[count++] = ip;
                    strs[count++] = pp+2;
                    pp++;
                }
            }
            for(i = 0;i < count;i++)
            {
                len += strlen(strs[i]);
            }
            char *arg = malloc(len+1);
            arg[len] = '\0';
            pp = arg;
            free_array[free_count++] = arg;
            for(i = 0;i < count;i++)
            {
                len = strlen(strs[i]);
                memcpy(pp, strs[i], len);
                pp += len;
            }
            free(freeme);
            *p = arg;
        }
    }

    if(!fork())
    {
        execvp(argv[0], argv);
        fprintf(stderr, "exec '%s' failed: %s (%d)\n", argv[0], strerror(errno), errno);
        exit(0);
    }

    int i;
    for(i = 0;i < free_count;i++)
        free(free_array[i]);
    free(argv);
}

static void do_test(int s, struct sockaddr_in *addr)
{
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); // set non-blocking

    while(1)
    {
        if(connect(s, (const struct sockaddr*)addr, sizeof(*addr)) == -1)
        {
            if(errno == EISCONN)
                break;
            if(errno != EINPROGRESS && errno != EALREADY)
            {
                /* filter out errors we don't care about */
                if(errno != ECONNREFUSED)
                    fprintf(stderr, "connect() failed: %s (%d)\n", strerror(errno), errno);
                return;
            }
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval timeout = { 0, ms_timeout * 1000 };

        int result = select(s+1, NULL, &wfds, NULL, &timeout);
        if(result == -1)
        {
            fprintf(stderr, "Error selecting on connect(): %s (%d)\n", strerror(errno), errno);
            return;
        }
        if(result == 0)
        {
            /* connect timed out */
            return;
        }
    }

    /* wait for data on read */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timeval timeout = { 0, ms_timeout * 1000 };
    int result = select(s+1, &rfds, NULL, NULL, &timeout);
    if(result == -1)
    {
        fprintf(stderr, "Error selecting on read(): %s (%d)\n", strerror(errno), errno);
        return;
    }
    if(result == 0)
    {
        /* read timed out */
        return;
    }

    char buf[1024];
    int len = read(s, buf, sizeof(buf));
    if(len == 0)
    {
        /* connection returned nothing */
        return;
    }
    if(len == -1)
    {
        fprintf(stderr, "Error on read(): %s (%d)\n", strerror(errno), errno);
        return;
    }
    buf[len] = '\0';
    
    if(strstr(buf, "SSH-") == buf)
    {
        /* SSH found... */
        uint32_t ip = (uint32_t)addr->sin_addr.s_addr;
        int len = snprintf(buf, sizeof(buf), "%d.%d.%d.%d\n",
            (ip>> 0)&0xFF,
            (ip>> 8)&0xFF,
            (ip>>16)&0xFF,
            (ip>>24)&0xFF);
        /* do this as syscall, to be atomic */
        write(1, buf, len);
        buf[len-1] = '\0';
        on_found(buf);
    }
}

void *worker(void *ctxt)
{
    struct sockaddr_in addr;
    uint32_t ip;

    while( (ip = get_next_ip()) )
    {
        memset(&addr, '\0', sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(22);
        addr.sin_addr.s_addr = ip;

        int s = socket(AF_INET, SOCK_STREAM, 0);
        if(s == -1)
        {
            fprintf(stderr, "socket() failed: %s (%d)\n", strerror(errno), errno);
            return NULL;
        }
        do_test(s, &addr);
        close(s);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if(argc < 5)
    {
        fprintf(stderr, "Usage: %s <network> <netmask> <thread count> <timeout (ms)> [execvp...]\n", argv[0]);
        return 1;
    }

    signal(SIGCHLD, SIG_IGN); // Children automatically get reaped

    exec = (argv+5);
    network = htonl(inet_addr(argv[1]));
    mask    = htonl(inet_addr(argv[2]));
    network &= mask;
    cur_ip = network+1;
    ms_timeout = strtol(argv[4], NULL, 0);

    int i, thread_count = strtol(argv[3], NULL, 0);
    pthread_t threads[thread_count];

    for(i = 0;i < thread_count;i++)
    {
        fprintf(stderr, "Starting thread %d\n", i);
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for(i = 0;i < thread_count;i++)
    {
        pthread_join(threads[i], NULL);
        fprintf(stderr, "Joined thread %d\n", i);
    }
    return 0;
}
