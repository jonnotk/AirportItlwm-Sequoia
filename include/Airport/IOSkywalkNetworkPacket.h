#ifndef AIRITL_OPAQUE_SKYWALK_NETWORK_PACKET_H
#define AIRITL_OPAQUE_SKYWALK_NETWORK_PACKET_H
#include <IOKit/IOTypes.h>
// Opaque receivers for exported metadata setters on native pool type 1 packets.
// No object is allocated, laid out or subclassed from this declaration.
class IOSkywalkNetworkPacket {
public:
    IOReturn setHeadroom(UInt8);
    IOReturn setLinkHeaderLength(UInt8);
    IOReturn setIsLinkBroadcast(bool);
    IOReturn setIsLinkMulticast(bool);
    IOReturn setCompletionStatus(int);
private:
    IOSkywalkNetworkPacket() = delete;
};
#endif
