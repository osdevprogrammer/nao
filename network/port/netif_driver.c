#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/etharp.h"

extern void nic_transmit_packet(uint8_t* buffer, uint32_t length);
extern uint32_t nic_poll_received_packet(uint8_t* buffer);

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    uint8_t tx_buffer[1518];
    uint32_t total_len = 0;

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        for (uint32_t i = 0; i < q->len; i++) {
            if (total_len < sizeof(tx_buffer)) {
                tx_buffer[total_len++] = ((uint8_t*)q->payload)[i];
            }
        }
    }

    nic_transmit_packet(tx_buffer, total_len);
    return ERR_OK;
}

extern uint32_t nic_poll_received_packet(uint8_t* buffer);

void netif_driver_poll(struct netif *netif) {
    // Temporary stack buffer to hold an incoming raw ethernet frame
    static uint8_t rx_temp_buffer[2048];
    
    // Poll the hardware to see if a packet is waiting in the ring buffer
    uint32_t packet_len = nic_poll_received_packet(rx_temp_buffer);
    
    // Keep processing as long as packets are waiting in the hardware ring buffer
    while (packet_len > 0) {
        // Allocate an lwIP packet buffer structure to hold this frame
        struct pbuf* p = pbuf_alloc(PBUF_RAW, packet_len, PBUF_RAM);
        if (p != NULL) {
            // Copy the raw bytes from your driver array into the lwIP managed memory chain
            pbuf_take(p, rx_temp_buffer, packet_len);
            
            // Pass the completed frame directly into lwIP's entry function (netif->input)
            // For ethernet interfaces, netif->input points to eth_input or netif_input
            if (netif->input(p, netif) != ERR_OK) {
                // If the stack rejects it or fails to parse it, free the memory immediately
                pbuf_free(p);
            }
        }
        
        // Check if there is another back-to-back packet waiting in the hardware
        packet_len = nic_poll_received_packet(rx_temp_buffer);
    }
}
extern uint8_t mac_address[6];
err_t netif_driver_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output; 
    netif->linkoutput = low_level_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->hwaddr_len = 6;  // Standard Ethernet MAC length
    
    // Copy the physical card's MAC address into lwIP's interface structure
    for (int i = 0; i < 6; i++) {
        netif->hwaddr[i] = mac_address[i];
    }

    // Ensure the interface flags declare it supports Ethernet, Broadcast, and ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}