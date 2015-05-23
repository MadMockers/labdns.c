
#define _BSD_SOURCE

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>

#include <net/if.h>
#include <netinet/in.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <asm/types.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static int debug = 0;

static int is_private_addr(uint32_t addr)
{
    uint32_t private[4][2] = {
        { 0x7F000000, 0xFF000000 }, // 127.0.0.0   /8
        { 0x0A000000, 0xFF000000 }, // 10.0.0.0    /8
        { 0xAC100000, 0xFFF00000 }, // 172.16.0.0  /12
        { 0xC0A80000, 0xFFFF0000 }, // 192.168.0.0 /16
    };
    int i;
    for(i = 0;i < 4;i++)
    {
        if((htonl(addr) & private[i][1]) == private[i][0])
            return 1;
    }
    return 0;
}

static void on_ip_notification(char *interface)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(s == -1)
    {
        fprintf(stderr, "Failed to create DGRAM socket: %s (%d)\n", strerror(errno), errno);
        return;
    }

    struct ifreq ifr;
    strcpy(ifr.ifr_name, interface);

    if(ioctl(s, SIOCGIFADDR, &ifr) == -1)
    {
        fprintf(stderr, "ioctl on DGRAM socket failed: %s (%d)\n", strerror(errno), errno);
        close(s);
        return;
    }
    uint32_t ip =
        *(uint32_t*)&((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;

    if(ioctl(s, SIOCGIFHWADDR, &ifr) == -1)
    {
        fprintf(stderr, "ioctl on DGRAM socket failed: %s (%d)\n", strerror(errno), errno);
        close(s);
        return;
    }

    close(s);

    if(is_private_addr(ip))
    {
        fprintf(stderr, "Ignoring private address on interface %s\n", interface);
        return;
    }

    unsigned char mac_addr[6];
    memcpy(mac_addr, ifr.ifr_hwaddr.sa_data, sizeof(mac_addr));
    char s_mac_addr[128];
    snprintf(s_mac_addr, sizeof(s_mac_addr), "%02X:%02X:%02X:%02X:%02X:%02X",
        mac_addr[0],
        mac_addr[1],
        mac_addr[2],
        mac_addr[3],
        mac_addr[4],
        mac_addr[5]);

    fprintf(stderr, "%s; %s - %u.%u.%u.%u\n", interface, s_mac_addr,
        (ip >>  0)&0xFF, 
        (ip >>  8)&0xFF, 
        (ip >> 16)&0xFF, 
        (ip >> 24)&0xFF);
}

static void on_start(void)
{
    /* iterate interfaces now */
    struct ifconf ifc;
    char buf[1024];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int ret;
    if(s == -1)
    {
        fprintf(stderr, "Failed to create DGRAM socket: %s (%d)\n", strerror(errno), errno);
        return;
    }

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    ret = ioctl(s, SIOCGIFCONF, &ifc);
    close(s);

    if(ret == -1)
    {
        fprintf(stderr, "Failed to get list of interfaces\n");
        close(s);
        return;
    }

    struct ifreq *it = ifc.ifc_req;
    struct ifreq *end = it + (ifc.ifc_len / sizeof(struct ifreq));
    for(;it != end;it++)
    {
        on_ip_notification(it->ifr_name);
    }
}

static void monitor(void)
{
    int nl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if(nl == -1)
    {
        fprintf(stderr, "Failed to create NETLINK socket: %s (%d)\n", strerror(errno), errno);
        return;
    }

    struct sockaddr_nl addr;
    memset(&addr, '\0', sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_IFADDR; // TODO: ipv6 | RTMGRP_IPV6_IFADDR;
    if(bind(nl, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        fprintf(stderr, "Failed to bind NETLINK socket: %s (%d)\n", strerror(errno), errno);
        return;
    }

    int rtl;
    char buffer[4096];
    struct nlmsghdr *nlh = (void*)buffer;
    struct ifaddrmsg *ifa;
    struct rtattr *rth;
    size_t len;

    while((len = recv(nl, nlh, 4096, 0)) > 0)
    {
        for(;(NLMSG_OK(nlh, len)) && (nlh->nlmsg_type != NLMSG_DONE); nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type != RTM_NEWADDR)
                continue; /* some other kind of announcement */

            ifa = (struct ifaddrmsg *) NLMSG_DATA (nlh);

            rth = IFA_RTA(ifa);
            rtl = IFA_PAYLOAD(nlh);
            for(;rtl && RTA_OK(rth, rtl); rth = RTA_NEXT(rth,rtl))
            {
                char name[IFNAMSIZ];
                uint32_t ipaddr;

                if (rth->rta_type != IFA_LOCAL)
                    continue;

                ipaddr = *(uint32_t*)RTA_DATA(rth);
                ipaddr = htonl(ipaddr);

                on_ip_notification(
                    if_indextoname(ifa->ifa_index, name));
            }
        }
    }
    close(nl);
}

static void daemonize(void)
{
    if(fork())
        exit(0);
    setsid();
    
    int dev_null = open("/dev/null", O_WRONLY);
    if(dev_null != -1)
    {
        dup2(dev_null, 0);
        dup2(dev_null, 1);
        dup2(dev_null, 2);
        close(dev_null);
    }

    int tty_fd = open("/dev/tty", O_RDWR);
    if(tty_fd != -1)
    {
        ioctl(tty_fd, TIOCNOTTY);
        close(tty_fd);
    }
}

static void watchdog(void)
{
    int pid = -1;
    while(1)
    {
        if(pid == -1)
        {
            if(!(pid = fork()))
                return;
        }
        int result, status;
        result = waitpid(pid, &status, 0);
        if(result == pid)
        {
            /* if child wanted to exit, then we also exit */
            if(WIFEXITED(status))
                exit(0);
            /* otherwise, restart */
            pid = -1;
        }
        if(result == -1)
        {
            if(errno == ECHILD)
                pid = -1;
            else
                exit(1); // Unexpected error
        }
    }
    fprintf(stderr, "WatchDog: Spawning new process\n");
}

void check_stop_file(void)
{
    struct stat sb;
    if(stat(".stop", &sb) == 0)
    {
        /* stop file exists! */
        fprintf(stderr, "Stop file exists! Exitting\n");
        exit(0);
    }
}

void *stop_thread(void *ctxt)
{
    while(1)
    {
        check_stop_file();
        sleep(60);
    }
}

int main(int argc, char *argv[])
{
    if(argc == 2 && strcmp(argv[1], "debug") == 0)
        debug = 1;
    /* check we aren't already running */
    int fd = shm_open("labdns", O_RDWR | O_CREAT, 0600);
    if(fd == -1)
    {
        fprintf(stderr, "Failed to open 'labdns' shared memory\n");
        return 1;
    }

    ftruncate(fd, 1);
    if(lockf(fd, F_TLOCK, 1) == -1)
    {
        fprintf(stderr, "labdns is already running\n");
        return 1;
    }

    /* chdir to path of executable */
    chdir(dirname(argv[0]));
    check_stop_file();

    if(!debug)
    {
        daemonize();
        watchdog();
    }

    pthread_t thread;
    pthread_create(&thread, NULL, stop_thread, NULL);
    pthread_detach(thread);

    on_start();
    monitor();
}
