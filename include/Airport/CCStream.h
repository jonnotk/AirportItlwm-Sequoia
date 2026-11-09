//
//  CCStream.h
//  itlwm
//
//  Created by qcwap on 2023/6/14.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef CCStream_h
#define CCStream_h

#include <IOKit/IOService.h>
#include "CCPipe.h"

struct CCStreamOptions {
    uint64_t stream_type;
    uint64_t console_level;
    char pad[0x348];
};

class CCStream : public IOService {
    OSDeclareAbstractStructors(CCStream)
    
public:
    virtual void free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn configureReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn updateReport(IOReportChannelList *,UInt,void *,void *) APPLE_KEXT_OVERRIDE;
    virtual bool attach( IOService * provider ) APPLE_KEXT_OVERRIDE;
    virtual void detach( IOService * provider ) APPLE_KEXT_OVERRIDE;
    virtual bool profileLoaded(void);
    virtual bool profileRemoved(void);
#if __IO80211_TARGET >= __MAC_15_0
    virtual bool initWithPipeAndName(CCPipe *,char const*,CCStreamOptions const*);
#else
    virtual CCStream const *initWithPipeAndName(CCPipe *,char const*,CCStreamOptions const*);
#endif
    
public:
    static CCStream *withPipeAndName(CCPipe *,char const*,CCStreamOptions const*);
    CCPipe *getPipe() const;
    
public:
    uint8_t filter[0x90];
};

#endif /* CCStream_h */

