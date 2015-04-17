
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <net/if.h>
#include <netinet/in.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <asm/types.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static int debug = 0;

static void on_ip_notification(char *interface, uint32_t ip)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(s == -1)
    {
        fprintf(stderr, "Failed to create DGRAM socket: %s (%d)\n", strerror(errno), errno);
        return;
    }

    struct ifreq ifr;
    char buf[1024];

    strcpy(ifr.ifr_name, interface);
    if(ioctl(s, SIOCGIFHWADDR, &ifr) == -1)
    {
        fprintf(stderr, "ioctl on DGRAM socket failed: %s (%d)\n", strerror(errno), errno);
        return;
    }

    char mac_addr[6];
    memcpy(mac_addr, ifr.ifr_hwaddr.sa_data, sizeof(mac_addr));
    char *s_mac_addr;
    asprintf(&s_mac_addr, "%02X:%02X:%02X:%02X:%02X:%02X",
        (unsigned char)mac_addr[0],
        (unsigned char)mac_addr[1],
        (unsigned char)mac_addr[2],
        (unsigned char)mac_addr[3],
        (unsigned char)mac_addr[4],
        (unsigned char)mac_addr[5]);

    fprintf(stderr, "%s: %d.%d.%d.%d\n", s_mac_addr,
        (ip >> 24)&0xFF, 
        (ip >> 16)&0xFF, 
        (ip >>  8)&0xFF, 
        (ip >>  0)&0xFF);
    
    free(s_mac_addr);
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
                    if_indextoname(ifa->ifa_index, name), ipaddr);
            }
        }
    }
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

    if(!debug)
    {
        daemonize();
        watchdog();
    }

    monitor();
}
