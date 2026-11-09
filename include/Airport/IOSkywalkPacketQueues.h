//
//  IOSkywalkPacketQueues.h
//  AirportItlwm — Skywalk datapath queues
//
//  Minimal declarations of the four concrete Skywalk packet-queue classes and the
//  IOSkywalkInterface::registerInterfaceWithQueues entry point, in this ABI-faithful
//  include/Airport style (NOT the MacKernelSDK skywalk headers — those would ODR-conflict
//  with <Airport/IOSkywalkPacketBufferPool.h> / IOSkywalkNetworkPacket.h, AND their callback
//  typedefs use `IOReturn` (= int) returns which MISMATCH the real KC mangling).
//
//  Types here are pinned to the EXACT mangled KC symbols (verified via nm on
//  BootKernelExtensions.kc) so the linker binds Apple's real factories:
//    __ZN26IOSkywalkTxSubmissionQueue8withPoolE...   (non-const pkt variant)
//    __ZN26IOSkywalkTxCompletionQueue8withPoolE...
//    __ZN26IOSkywalkRxSubmissionQueue8withPoolE...   (non-const pkt variant)
//    __ZN26IOSkywalkRxCompletionQueue8withPoolE...
//  NOTE: every callback returns `UInt32` (mangled 'j'), NOT IOReturn ('i').
//

#ifndef IOSkywalkPacketQueues_h
#define IOSkywalkPacketQueues_h

#include <IOKit/IOService.h>
#include "IOSkywalkPacketBufferPool.h"

class IOSkywalkPacket;
class IOSkywalkTxSubmissionQueue;
class IOSkywalkTxCompletionQueue;
class IOSkywalkRxSubmissionQueue;
class IOSkywalkRxCompletionQueue;

typedef UInt32 (*IOSkywalkQueryFreeSpaceHandler)(OSObject *owner, IOSkywalkTxSubmissionQueue *queue, UInt32 *outSpace);
typedef UInt32 (*IOSkywalkTxSubmissionQueueAction)(OSObject *owner, IOSkywalkTxSubmissionQueue *queue, IOSkywalkPacket **, UInt32, void *);
// Array-mode action: the queue passes a contiguous array of packet pointers.
typedef UInt32 (*IOSkywalkTxSubmissionQueueArrayAction)(OSObject *owner, IOSkywalkTxSubmissionQueue *queue, IOSkywalkPacket *const *, UInt32, void *);
typedef UInt32 (*IOSkywalkTxCompletionQueueAction)(OSObject *owner, IOSkywalkTxCompletionQueue *queue, IOSkywalkPacket **, UInt32, void *);
typedef UInt32 (*IOSkywalkRxSubmissionQueueAction)(OSObject *owner, IOSkywalkRxSubmissionQueue *queue, IOSkywalkPacket **, UInt32, void *);
typedef UInt32 (*IOSkywalkRxCompletionQueueAction)(OSObject *owner, IOSkywalkRxCompletionQueue *queue, IOSkywalkPacket **, UInt32, void *);

class IOSkywalkPacketQueue : public IOService {
    OSDeclareAbstractStructors(IOSkywalkPacketQueue)
public:
    bool isQueueReady() const;
    uint8_t filter[0x80];
};

class IOSkywalkTxSubmissionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkTxSubmissionQueue)
public:
    // Direct imported call. options 0x1 schedules deferred work on its workloop.
    IOReturn requestDequeue(void *, UInt32);
    // The KC factory forwards the penultimate uint as options
    // (the mutable-packet wrapper adds 0x8), and the final uint as service class.
    // IOOptionBits aliases UInt32 in the pinned SDK; both arguments mangle as 'j'.
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool, UInt32 capacity, UInt32 queueId,
                                                OSObject *owner, IOSkywalkQueryFreeSpaceHandler handler,
                                                IOSkywalkTxSubmissionQueueAction action, void *refCon,
                                                IOOptionBits options, UInt32 serviceClass);
    // WARNING: the IOSkywalkPacket** overload above ORs 0x8 into options. That sets
    // list mode (getPacketListForNetworking + listDequeue): the action then receives
    // a pointer to a packet chain, not an array. Indexing it as an array reads the
    // queue's own fields as packets and panics.
    // The KC base factory has an IOSkywalkPacket* const* action;
    // without 0x8 it uses array mode (refillTxRingForNetworking + legacyDequeue), as
    // Apple's BCM driver does.
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool, UInt32 capacity, UInt32 queueId,
                                                OSObject *owner, IOSkywalkQueryFreeSpaceHandler handler,
                                                IOSkywalkTxSubmissionQueueArrayAction action, void *refCon,
                                                IOOptionBits options, UInt32 serviceClass);
};

class IOSkywalkTxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkTxCompletionQueue)
public:
    IOReturn requestEnqueue(void *, UInt32);
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool, UInt32 capacity, UInt32 queueId,
                                                OSObject *owner, IOSkywalkTxCompletionQueueAction action,
                                                void *refCon, IOOptionBits options);
};

class IOSkywalkRxSubmissionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkRxSubmissionQueue)
public:
    // The KC factory bounds buffersPerPacket by the pool maximum.
    // The following uint is the queue ID. Both arguments mangle as 'j'.
    static IOSkywalkRxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool, UInt32 capacity, UInt32 buffersPerPacket,
                                                UInt32 queueId, OSObject *owner,
                                                IOSkywalkRxSubmissionQueueAction action, void *refCon, IOOptionBits options);
};

class IOSkywalkRxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkRxCompletionQueue)
public:
    IOReturn requestEnqueue(void *, UInt32);
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool, UInt32 capacity, UInt32 queueId,
                                                OSObject *owner, IOSkywalkRxCompletionQueueAction action,
                                                void *refCon, IOOptionBits options);
};

#endif /* IOSkywalkPacketQueues_h */
