#include "ip.h"
#include "tcp.h"

IPPacketReceiveCallback IPCallback;

void IPpacket::ToNetOrder() {
    header.ip_len = htons(header.ip_len);
    header.ip_id = htons(header.ip_id);
    header.ip_off = htons(header.ip_off);
    header.ip_sum = htons(header.ip_sum);
}

void IPpacket::RecoverOrder() {
    header.ip_len = ntohs(header.ip_len);
    header.ip_id = ntohs(header.ip_id);
    header.ip_off = ntohs(header.ip_off);
    header.ip_sum = ntohs(header.ip_sum);
}

/*
 * return ip_sum when initial ip_sum = 0
 * return 0 if checksum sucessfully
 */
uint16_t Checksum(const void* vdata, size_t length) {
    char* data = (char*)vdata;
    uint64_t acc = 0xffff;
    uint32_t offset = ((uintptr_t)data) & 3;
    if (offset) {
        size_t count = 4 - offset;
        count = std::min(count, length);
        uint32_t word = 0;
        memcpy(offset + (char*)&word, data, count);
        acc += ntohl(word);
        data += count;
        length -= count;
    }
    char* end = data + (length & ~3);
    for (;data != end; data += 4) {
        uint32_t word;
        memcpy(&word, data, 4);
        acc += ntohl(word);
    }
    length &= 3;
    if (length) {
        uint32_t word = 0;
        memcpy(&word, data, length);
        acc += ntohl(word);
    }
    acc = (acc & 0xffffffff) + (acc >> 32);
    while (acc >> 16)
        acc = (acc & 0xffff) + (acc >> 16);
    if (offset & 1)
        acc = ((acc & 0xff00) >> 8) | ((acc & 0x00ff) << 8);

    return htons(~acc);
}

int myIPCallback(const void* buf, const int len) {
    try {
        IPpacket pkt;
        pkt.header = *(ip*)buf;
        int header_len = sizeof(ip);
        if (Checksum(&pkt.header, header_len) != 0) {
            my_printf("[IPCallback] [Checksum error]\n");
            return -1;
        }
        my_printf("[IPCallback] [Checksum success]\n");
        pkt.RecoverOrder();
        pkt.payload = (u_char*)buf + header_len;

        my_printf("[IPCallback] [srcIP: %s] [dstIP: %s] [len: %d]\n",
            IPtoStr(pkt.header.ip_src), IPtoStr(pkt.header.ip_dst), len);
        my_printf("[IPCallback] [received an IP packet]:\n");
        for (int i = 0; i < len - header_len; ++i) {
            my_printf("%02x ", pkt.payload[i]);
        }
        my_printf("\n");
        if (IsArrive(pkt.header.ip_dst)) {
            my_printf("[IPCallback] Successfully receive IPpacket to me!\n");
            TCP_handler(pkt, len - header_len);
            
        } else {
            my_printf("[IPCallback] [Forwarding]\n");
            sendIPPacket(pool, pkt.header.ip_src, pkt.header.ip_dst, IPPROTO_UDP, pkt.payload, len - header_len);
        }
    } catch (const char* err) {
        my_printf("[IPCallback] error: %s", err);
        return -1;
    }
    return 0;
}

bool IsArrive(in_addr addr) {
    int len = pool.device_list.size();
    for (int i = 0 ; i < len; ++i) {
        Device* dev = pool.device_list[i];
        if (addr.s_addr == dev->ip.s_addr)
            return true;
    }
    return false;
}

int sendIPPacket(DevicePool& pool, const struct in_addr src, const struct in_addr dest,
    int proto, const void* buf, int len) {
    my_printf("[sendIPPacket] [srcIP: %s] [dstIP: %s]\n", IPtoStr(src), IPtoStr(dest));
    Device* dev = pool.findDevice(src, dest);
    if (!dev) {
        my_printf("[sendIPPacket] No suitable device to send this IP packet\n");
        return -1;
    }
    char* dstMAC;
    if (IsSameSubnet(dev->ip, dest, dev->subnetMask)) {
        my_printf("[sendIPPacket] [srcIP: %s] [dstIP: %s] in same subnet\n",
            IPtoStr(dev->ip), IPtoStr(dest));
        try {
            dstMAC = ARPFindMAC(dev, dest);
        } catch (const char* err) {
            my_printf("[sendIPPacket] to [IP=%s] err: %s\n", inet_ntoa(dest), err);
            return -1;
        }
    } else {
        try {
            dstMAC = (char*)router.GetNexthop(dest);
        } catch (const char* e) {
            my_printf("[sendIPPacket] to [IP=%s] err: %s\n", inet_ntoa(dest), e);
            return -1;
        }
    }
    IPpacket pkt;
    pkt.header.ip_src = src;
    pkt.header.ip_dst = dest;
    pkt.header.ip_p = proto;
    pkt.payload = (u_char*)buf;
    size_t header_len = sizeof(ip);
    int total_len = header_len + len;
    pkt.header.ip_len = total_len;
    pkt.ToNetOrder();
    pkt.header.ip_sum = 0;
    pkt.header.ip_sum = Checksum(&pkt.header, header_len);
    u_char* pkt_send = new u_char[total_len];
    memcpy(pkt_send, &pkt.header, header_len);
    memcpy(pkt_send + header_len, pkt.payload, len);
    dev->sendFrame(pkt_send, total_len, ETHERTYPE_IP, dstMAC);
    return 0;
}

int setIPPacketReceiveCallback(IPPacketReceiveCallback callback) {
    my_printf("[setIPPacketReceiveCallback]......\n");
    try {
        IPCallback = callback;
    } catch (const char* err) {
        my_printf("[setIPPacketReceiveCallback] error: %s\n", err);
        return -1;
    }
    return 0;
}

void startBroadcast() {
    int len = pool.device_list.size();
    for (int i = 0 ; i < len; ++i) {
        Device* dev = pool.device_list[i];
        dev->broadcastRouteTable = std::thread(broadcastRouteTable, dev);
    }
}