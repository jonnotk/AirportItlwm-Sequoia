#ifndef AIRITL_OPAQUE_SKYWALK_PACKET_H
#define AIRITL_OPAQUE_SKYWALK_PACKET_H
#include <IOKit/IOTypes.h>
class IOMemoryDescriptor;
class IOSkywalkPacketQueue;
// Imported calls only. Native objects derive from IOCommand. This opaque
// declaration supplies no layout, allocation or vtable. Qualify every call.
class IOSkywalkPacket {
public:
    UInt32 getPacketBufferCount() const;
    UInt32 getBufferSize() const;
    void *getDataVirtualAddress() const;
    UInt32 getDataLength() const;
    UInt16 getDataOffset() const;
    IOMemoryDescriptor *getMemoryDescriptor() const;
    IOReturn setDataOffsetAndLength(UInt16, UInt32);
    IOReturn prepareWithQueue(IOSkywalkPacketQueue *, UInt32, UInt32);
    void setTransferDirection(UInt32);
private:
    IOSkywalkPacket() = delete;
};
#endif
