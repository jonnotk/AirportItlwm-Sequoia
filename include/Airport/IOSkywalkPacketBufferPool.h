//
//  IOSkywalkPacketBufferPool.h
//  itlwm
//
//  Created by qcwap on 2023/6/15.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOSkywalkPacketBufferPool_h
#define IOSkywalkPacketBufferPool_h

#include <IOKit/IOService.h>

class IOSkywalkMemorySegment;
class IOSkywalkMemorySegmentDescriptor;
class IOSkywalkPacket;
class IOSkywalkPacketBuffer;
class IOSkywalkPacketDescriptor;
class IOSkywalkPacketBufferDescriptor;

class IOSkywalkPacketBufferPool : public OSObject {
    OSDeclareDefaultStructors(IOSkywalkPacketBufferPool)
    
public:
    struct PoolOptions {
        uint32_t packetCount;
        uint32_t bufferCount;
        uint32_t bufferSize;
        uint32_t maxBuffersPerPacket;
        uint32_t memorySegmentSize;
        uint32_t poolFlags;
        uint64_t pad;
    };
    
public:
#if __IO80211_TARGET >= __MAC_15_0
    // Pinned 24G720 allocation methods return zero-success 32-bit status.
    using AllocationResult = IOReturn;
#else
    using AllocationResult = bool;
#endif
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithName(char const*,void *,uint,IOSkywalkPacketBufferPool::PoolOptions const*);
    virtual bool initWithName(char const*,OSObject *,uint,IOSkywalkPacketBufferPool::PoolOptions const*);
    virtual AllocationResult allocatePacket(IOSkywalkPacket **,uint);
    virtual AllocationResult allocatePacket(uint,IOSkywalkPacket **,uint);
    virtual AllocationResult allocatePackets(uint,uint *,IOSkywalkPacket **,uint);
    virtual void deallocatePacket(IOSkywalkPacket *);
    virtual void deallocatePackets(IOSkywalkPacket **,uint);
    virtual void deallocatePacketList(IOSkywalkPacket *);
    virtual void deallocatePacketChain(unsigned long long);
    virtual AllocationResult allocatePacketBuffer(IOSkywalkPacketBuffer **,uint);
    virtual AllocationResult allocatePacketBuffers(uint *,IOSkywalkPacketBuffer **,uint);
    virtual void deallocatePacketBuffer(IOSkywalkPacketBuffer *);
    virtual void deallocatePacketBuffers(IOSkywalkPacketBuffer **,uint);
    virtual AllocationResult newPacket(IOSkywalkPacketDescriptor *,IOSkywalkPacket **);
    virtual AllocationResult newPacketBuffer(IOSkywalkPacketBufferDescriptor *,IOSkywalkPacketBuffer **);
    // NOTE: Apple's real ABI returns IOReturn (0 = success, e.g. 0xe00002bd on failure), NOT bool.
    // This was reconstructed wrong as `bool` originally; it only mattered once we OVERRIDE this slot
    // (vtable slot 52 / runtime 0x190) in AirportItlwmSkywalkPacketPool — a bool return leaves the upper
    // eax bits undefined and the framework's `test eax,eax` would misread success as failure.
    virtual IOReturn newMemorySegment(IOSkywalkMemorySegmentDescriptor *,IOSkywalkMemorySegment **);
    
public:
    static IOSkywalkPacketBufferPool *withName(char const*,OSObject *,uint,IOSkywalkPacketBufferPool::PoolOptions const*);
    
public:
    uint8_t filter[0xB8];
};

#endif /* IOSkywalkPacketBufferPool_h */

