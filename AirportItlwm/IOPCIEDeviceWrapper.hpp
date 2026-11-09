//
//  IOPCIEDeviceWrapper.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOPCIEDeviceWrapper_hpp
#define IOPCIEDeviceWrapper_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

#include <HAL/ItlHalService.hpp>

class IO80211WorkQueue;

class IOPCIEDeviceWrapper : public IOService {
    OSDeclareDefaultStructors(IOPCIEDeviceWrapper)
    
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;
    // 1-arg matchPropertyTable intentionally NOT overridden — overriding it caused infinite
    // recursion (super 2-arg -> virtual 1-arg -> our 1-arg -> our 2-arg) -> double fault. See .cpp.
    virtual bool matchPropertyTable(OSDictionary *table, SInt32 *score) override;
    virtual IOWorkLoop* getWorkLoop() const override;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) override;
    
public:
    // Each caller owns a fresh, unbound HAL. The wrapper never owns a HAL.
    ItlHalService *createHalService() const;
    IOPCIDevice *pciNub;

private:
    IO80211WorkQueue *fOwnedWorkQueue = NULL;
};

#endif /* IOPCIEDeviceWrapper_hpp */
