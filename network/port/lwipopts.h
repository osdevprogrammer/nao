#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1  
#define MEM_ALIGNMENT               4  
#define LWIP_RAW                    1
#define LWIP_UDP                    1  
#define LWIP_TCP                    1  

#define LWIP_CALLBACK_API           1
// EXPLICITLY DISABLE THREADED INTERFACES FOR NO_SYS=1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_STATS                  0
#define MEM_SIZE                    16384
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_SEG            16
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define LWIP_DHCP                   1  // Enable DHCP support
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_ACD                    0
#define NETIF_LOOPBACK              0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#endif