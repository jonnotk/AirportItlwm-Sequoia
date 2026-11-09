//
//  AirportItlwmSkywalkPacketPool.h
//  itlwm — DMA-capable Skywalk packet pool
//
//  A concrete IOSkywalkPacketBufferPool subclass that makes the pool's memory segments
//  DMA-capable for the iwx PCIe NIC. The BASE pool cannot DMA: base segments are created with
//  NO IODMACommand (seg+0x38 == NULL), so IOSkywalkMemorySegment::prepare() SKIPS the device DMA
//  mapping and the framework's nexus-connect leaves the buffers unmapped in the NIC's IOVM address
//  space -> fault ~4s after airportd enables the datapath.
//
//  Apple's reference (AppleBCMWLANPCIeSkywalkPacketPool) attaches DMA by overriding newMemorySegment
//  (vtable slot 52 / runtime 0x190) so every segment it creates carries an IODMACommand built from the
//  device's IOMapper (IOMapper::copyMapperForDevice(pciDevice)). We mirror that minimally: override
//  newMemorySegment to call the base, then attach an IODMACommand built from our captured mapper.
//
//  DMA-build template pinned from AppleBCMWLANPCIeSkywalkMemorySegment::setMapper:
//    IODMACommand::withSpecification(OutputLittle64, 64, 0, kMapped(0), 0, 0x1000, mapper, NULL)
//    -> segment->setDMACommand(dma, 0)   (consumed by IOSkywalkMemorySegment::prepare @+0x38)
//

#ifndef AirportItlwmSkywalkPacketPool_h
#define AirportItlwmSkywalkPacketPool_h

#include "IOSkywalkPacketBufferPool.h"

class IODMACommand;
class IOMapper;

// Minimal declaration so we can attach the DMACommand. setDMACommand is a NON-virtual KC method,
// called directly (symbol _ZN21IOSkywalkMemorySegment13setDMACommandEP12IODMACommandj, returns
// IOReturn: 0 on store, 0xe00002d5 if a command is already attached).
class IOSkywalkMemorySegment {
public:
    IOReturn setDMACommand(IODMACommand *command, unsigned int options);
};

class AirportItlwmSkywalkPacketPool : public IOSkywalkPacketBufferPool {
    OSDeclareDefaultStructors(AirportItlwmSkywalkPacketPool)

public:
    // Captured device DMA mapper (IOMapper::copyMapperForDevice(iwx IOPCIDevice)). Set BEFORE
    // initWithName so it is present when the framework lazily creates segments at connect.
    // Lands past the base instance size (0xB8), so Apple's internal writes never touch it.
    IOMapper *fMapper;

    // Allocate + set the mapper + init in one shot (the base static withName() would allocate a
    // BASE pool, not this subclass). Returns a +1-retained pool, or NULL on failure.
    static AirportItlwmSkywalkPacketPool *withMapperName(char const *name, OSObject *owner, uint n,
                                                         IOSkywalkPacketBufferPool::PoolOptions const *opts,
                                                         IOMapper *mapper);

    virtual void free() APPLE_KEXT_OVERRIDE;

    // THE injection point (slot 52 / runtime 0x190): make every segment DMA-capable.
    virtual IOReturn newMemorySegment(IOSkywalkMemorySegmentDescriptor *desc,
                                      IOSkywalkMemorySegment **out) APPLE_KEXT_OVERRIDE;
};

#endif /* AirportItlwmSkywalkPacketPool_h */
