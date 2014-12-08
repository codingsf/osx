/*
 * Copyright (c) 1998-2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.  
 * Please see the License for the specific language governing rights and 
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <libkern/OSByteOrder.h>

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include <IOKit/usb/IOUSBDevice.h>
#include <IOKit/usb/IOUSBInterface.h>
#include <IOKit/usb/IOUSBPipe.h>
#include <IOKit/usb/IOUSBLog.h>

#include "IOUSBInterfaceUserClient.h"

struct AsyncPB {
    OSAsyncReference 		fAsyncRef;
    UInt32 			fMax;
    IOMemoryDescriptor 		*fMem;
    IOUSBDevRequestDesc		req;
};

struct IsoAsyncPB {
    OSAsyncReference 	fAsyncRef;
    int			frameLen;	// In bytes
    void *		frameBase;	// In user task
    IOMemoryDescriptor *dataMem;
    IOMemoryDescriptor *countMem;
    IOUSBIsocFrame	frames[0];
};

struct LowLatencyIsoAsyncPB {
    OSAsyncReference 		fAsyncRef;
    int				frameLen;	// In bytes
    void *			frameBase;	// In user task
    IOMemoryDescriptor *	dataMem;
    IOMemoryDescriptor *	countMem;
    IOUSBLowLatencyIsocFrame	frames[0];
};


//=============================================================================================
//
//	Note on the use of IncrementOutstandingIO(), DecrementOutstandingIO() and doing extra
//	retain()/release() on async calls:
//
//	The UserClient is a complex driver.  It is attached to its provider when it is instantiated
// 	due to a device/interface being created but it is ALSO used by the user land IOUSBLib.  
//	Thus, it effectively has 2 clients, one on the kernel side and one on the user side.  When
//	the user side closes the connection to this user client, we terminate ourselves.  When the
//	device is unplugged, we get terminated by our provider.  The Increment/DecrementOutstandingIO()
//	calls are used to keep track of any IO that has not finished so that when OUR provider attempts
//	to terminate us, we won't get released (because we have our provider open) until the IO completes.
//	However, when our user land client closes us, we terminate ourselves.  In this case the 
//	OutstandingIO() calls do not help us in preventing our object from going away.  That is why we
//	also use a retain()/release() pair on async calls;  if we are closed before the async request is
//	complete, we will not go away and hence won't panic when we try to execute the completion routine.
//
//	Yes, we could have use only retain()/release(), but all our other kernel drivers use the 
//	outstandingIO metaphor so I thought it would be complete to use it here as well.
//
//	Another interesting piece is that we wait 1 ms in our clientClose method.  This will allow other 
//	pending threads to run and possible enter our driver (See bug #2862199).
//
//=============================================================================================
//

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#define super IOUserClient
OSDefineMetaClassAndStructors(IOUSBInterfaceUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
enum {
        kMethodObjectThis = 0,
        kMethodObjectOwner
    };
    
const IOExternalMethod 
IOUSBInterfaceUserClient::sMethods[kNumUSBInterfaceMethods] = {
    { //    kUSBInterfaceUserClientOpen
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::open,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientClose
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::close,
	kIOUCScalarIScalarO,
	0,
	0
    },
    { //    kUSBInterfaceUserClientGetDevice
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetDevice,
	kIOUCScalarIScalarO,
	0,
	1
    },
    { //    kUSBInterfaceUserClientSetAlternateInterface
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::SetAlternateInterface,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientGetFrameNumber
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetFrameNumber,
	kIOUCScalarIStructO,
	0,
	0xffffffff
    },
    { //    kUSBInterfaceUserClientGetPipeProperties
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetPipeProperties,
	kIOUCScalarIScalarO,
	1,
	5
    },
    { //    kUSBInterfaceUserClientReadPipe
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ReadPipe,
	kIOUCScalarIStructO,
	3,
	0xffffffff
    },
    { //    kUSBInterfaceUserClientReadPipeOOL
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ReadPipeOOL,
	kIOUCStructIStructO,
	sizeof(IOUSBBulkPipeReq),
	sizeof(UInt32)
    },
    { //    kUSBInterfaceUserClientWritePipe
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::WritePipe,
	kIOUCScalarIStructI,
	3,
	0xffffffff
    },
    { //    kUSBInterfaceUserClientWritePipeOOL
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::WritePipeOOL,
	kIOUCStructIStructO,
	sizeof(IOUSBBulkPipeReq),
	0
    },
    { //    kUSBInterfaceUserClientGetPipeStatus
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetPipeStatus,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientAbortPipe
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::AbortPipe,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientResetPipe
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ResetPipe,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientSetPipeIdle
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::SetPipeIdle,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientSetPipeActive
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::SetPipeActive,
	kIOUCScalarIScalarO,
	1,
	0
    },
    { //    kUSBInterfaceUserClientClearPipeStall
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ClearPipeStall,
	kIOUCScalarIScalarO,
	2,
	0
    },
    { //    kUSBInterfaceUserClientControlRequestOut
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ControlRequestOut,
	kIOUCScalarIStructI,
	4,
	0xffffffff
    },
    { //    kUSBInterfaceUserClientControlRequestIn
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ControlRequestIn,
	kIOUCScalarIStructO,
	4,
	0xffffffff
    },
    { //    kUSBInterfaceUserClientControlRequestOutOOL
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ControlRequestOutOOL,
	kIOUCStructIStructO,
	sizeof(IOUSBDevReqOOLTO),
	0
    },
    { //    kUSBInterfaceUserClientControlRequestInOOL
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::ControlRequestInOOL,
	kIOUCStructIStructO,
	sizeof(IOUSBDevReqOOLTO),
	sizeof(UInt32)
    },
    { //    kUSBInterfaceuserClientSetPipePolicy
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::SetPipePolicy,
	kIOUCScalarIScalarO,
	3,
	0
    },
    { //    kUSBInterfaceuserClientGetBandwidthAvailable
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetBandwidthAvailable,
	kIOUCScalarIScalarO,
	0,
	1
    },
    { //    kUSBInterfaceuserClientGetEndpointProperties
	(IOService*)kMethodObjectThis,
	(IOMethod) &IOUSBInterfaceUserClient::GetEndpointProperties,
	kIOUCScalarIScalarO,
	3,
	3
    },
};



const IOExternalAsyncMethod 
IOUSBInterfaceUserClient::sAsyncMethods[kNumUSBInterfaceAsyncMethods] = {
    { //    kUSBDeviceUserClientSetAsyncPort
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::SetAsyncPort,
	kIOUCScalarIScalarO,
	0,
	0
    },
    { //    kUSBInterfaceUserClientControlAsyncRequestOut
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::ControlAsyncRequestOut,
	kIOUCStructIStructO,
	sizeof(IOUSBDevReqOOLTO),
	0
    },
    { //    kUSBInterfaceUserClientControlAsyncRequestIn
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::ControlAsyncRequestIn,
	kIOUCStructIStructO,
	sizeof(IOUSBDevReqOOLTO),
	0
    },
    { //    kUSBInterfaceUserClientAsyncReadPipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::AsyncReadPipe,
	kIOUCScalarIScalarO,
	5,
	0
    },
    { //    kUSBInterfaceUserClientAsyncWritePipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::AsyncWritePipe,
	kIOUCScalarIScalarO,
	5,
	0
    },
    { //    kUSBInterfaceUserClientReadIsochPipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::ReadIsochPipe,
	kIOUCStructIStructO,
	sizeof(IOUSBIsocStruct),
	0
    },
    { //    kUSBInterfaceUserClientWriteIsochPipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::WriteIsochPipe,
	kIOUCStructIStructO,
	sizeof(IOUSBIsocStruct),
	0
    },
    { //    kUSBInterfaceUserClientLowLatencyReadIsochPipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::LowLatencyReadIsochPipe,
	kIOUCStructIStructO,
	sizeof(IOUSBLowLatencyIsocStruct),
	0
    },
    { //    kUSBInterfaceUserClientLowLatencyWriteIsochPipe
	(IOService*)kMethodObjectThis,
	(IOAsyncMethod) &IOUSBInterfaceUserClient::LowLatencyWriteIsochPipe,
	kIOUCStructIStructO,
	sizeof(IOUSBLowLatencyIsocStruct),
	0
    }
};



void 
IOUSBInterfaceUserClient::SetExternalMethodVectors()
{
    fMethods = sMethods;
    fNumMethods = kNumUSBInterfaceMethods;
    fAsyncMethods = sAsyncMethods;
    fNumAsyncMethods = kNumUSBInterfaceAsyncMethods;
}



IOExternalMethod *
IOUSBInterfaceUserClient::getTargetAndMethodForIndex(IOService **target, UInt32 index)
{
    if (index < (UInt32)fNumMethods) 
    {
        if ((IOService*)kMethodObjectThis == fMethods[index].object)
            *target = this;
        else if ((IOService*)kMethodObjectOwner == fMethods[index].object)
            *target = fOwner;
        else
            return NULL;
	return (IOExternalMethod *) &fMethods[index];
    }
    else
	return NULL;
}



IOExternalAsyncMethod * 
IOUSBInterfaceUserClient::getAsyncTargetAndMethodForIndex(IOService **target, UInt32 index)
{
    if (index < (UInt32)fNumAsyncMethods) 
    {
        if ((IOService*)kMethodObjectThis == fAsyncMethods[index].object)
            *target = this;
        else if ((IOService*)kMethodObjectOwner == fAsyncMethods[index].object)
            *target = fOwner;
        else
            return NULL;
	return (IOExternalAsyncMethod *) &fAsyncMethods[index];
    }
    else
	return 0;
}



bool
IOUSBInterfaceUserClient::initWithTask(task_t owningTask, void *security_id , UInt32 type )
{
    if (!super::initWithTask(owningTask, security_id , type))
        return false;

    if (!owningTask)
	return false;

    fTask = owningTask;
    fDead = false;
    SetExternalMethodVectors();

    return true;
}



bool 
IOUSBInterfaceUserClient::start( IOService * provider )
{
    IOWorkLoop		*wl;
    
    USBLog(7, "+%s[%p]::start(%p)", getName(), this, provider);
    
    IncrementOutstandingIO();		// make sure we don't close until start is done

    fOwner = OSDynamicCast(IOUSBInterface, provider);

    if (!fOwner)
    {
 	USBError(1, "%s[%p]::start - provider is NULL!", getName(), this);
	goto ErrorExit;
    }
    
    if(!super::start(provider))
    {
 	USBError(1, "%s[%p]::start - super::start returned false!", getName(), this);
        goto ErrorExit;
    }

    fGate = IOCommandGate::commandGate(this);

    if(!fGate)
    {
	USBError(1, "%s[%p]::start - unable to create command gate", getName(), this);
	goto ErrorExit;
    }

    wl = getWorkLoop();
    if (!wl)
    {
	USBError(1, "%s[%p]::start - unable to find my workloop", getName(), this);
	fGate->release();
	goto ErrorExit;
    }
    
    if (wl->addEventSource(fGate) != kIOReturnSuccess)
    {
	USBError(1, "%s[%p]::start - unable to add gate to work loop", getName(), this);
	fGate->release();
	goto ErrorExit;
    }

    DecrementOutstandingIO();
    return true;
    
ErrorExit:
    
    if ( fGate != NULL )
    {
        fGate->release();
        fGate = NULL;
    }
        
    DecrementOutstandingIO();
    return false;
}


// This method is NOT the IOService open(IOService*) method.
IOReturn 
IOUSBInterfaceUserClient::open(bool seize)
{
    IOOptionBits	options = seize ? kIOServiceSeize : 0;

    USBLog(7, "+%s[%p]::open(%p)", getName(), this);
    
    if (!fOwner)
        return kIOReturnNotAttached;
        
    if (!fOwner->open(this, options))
	return kIOReturnExclusiveAccess;

    fNeedToClose = false;
    
    return kIOReturnSuccess;
}



// This is NOT the normal IOService::close(IOService*) method.
// We are treating this is a proxy that we should close our parent, but
// maintain the connection with the task
IOReturn
IOUSBInterfaceUserClient::close()
{
    IOReturn 	ret = kIOReturnSuccess;
    
    USBLog(7, "+%s[%p]::close", getName(), this);
    IncrementOutstandingIO();
    
    if (fOwner && !isInactive())
    {
	if (fOwner->isOpen(this))
	{
	    fNeedToClose = true;				// the last outstanding IO will close this
	    if (GetOutstandingIO() > 1)				// 1 for the one at the top of this routine
	    {
		int		i;
    
		USBLog(5, "%s[%p]::close - outstanding IO, aborting pipes", getName(), this);
		for (i=1; i <= kUSBMaxPipes; i++)
		    AbortPipe(i);
	    }
	}
	else
	    ret = kIOReturnNotOpen;
    }
    else
	ret = kIOReturnNotAttached;
 
    DecrementOutstandingIO();
    USBLog(7, "-%s[%p]::close - returning %x", getName(), this, ret);
    return ret;
}



//
// clientClose - my client on the user side has released the mach port, so I will no longer
// be talking to him
//
IOReturn 
IOUSBInterfaceUserClient::clientClose( void )
{
    USBLog(7, "+%s[%p]::clientClose()", getName(), this);

    // Sleep for 1 ms to allow other threads that are pending to run
    //
    IOSleep(1);
    
    fTask = NULL;
    
    terminate();

    USBLog(7, "-%s[%p]::clientClose()", getName(), this);

    return kIOReturnSuccess;			// DONT call super::clientClose, which just returns notSupported
}


IOReturn 
IOUSBInterfaceUserClient::clientDied( void )
{
    IOReturn ret;

    USBLog(3, "+%s[%p]::clientDied()", getName(), this);

    fDead = true;				// don't send any mach messages in this case
    ret = super::clientDied();

    USBLog(3, "-%s[%p]::clientDied()", getName(), this);

    return ret;
}



void
IOUSBInterfaceUserClient::ReqComplete(void *obj, void *param, IOReturn res, UInt32 remaining)
{
    void *	args[1];
    AsyncPB * pb = (AsyncPB *)param;
    IOUSBInterfaceUserClient *me = OSDynamicCast(IOUSBInterfaceUserClient, (OSObject*)obj);

    if (!me)
	return;
	
    USBLog(7, "%s[%p]::ReqComplete, req = %08x, remaining = %08x", me->getName(), me, (int)pb->fMax, (int)remaining);

    if(res == kIOReturnSuccess) 
    {
        args[0] = (void *)(pb->fMax - remaining);
    }
    else 
    {
        args[0] = 0;
    }
    if (pb->fMem)
    {
	pb->fMem->complete();
	pb->fMem->release();
    }
    if (!me->fDead)
	sendAsyncResult(pb->fAsyncRef, res, args, 1);

    IOFree(pb, sizeof(*pb));
    me->DecrementOutstandingIO();
    me->release();
}


void
IOUSBInterfaceUserClient::IsoReqComplete(void *obj, void *param, IOReturn res, IOUSBIsocFrame *pFrames)
{
    void *	args[1];
    IsoAsyncPB * pb = (IsoAsyncPB *)param;
    IOUSBInterfaceUserClient *me = OSDynamicCast(IOUSBInterfaceUserClient, (OSObject*)obj);

    if (!me)
	return;
	
    args[0] = pb->frameBase;
    pb->countMem->writeBytes(0, pb->frames, pb->frameLen);
    pb->dataMem->complete();
    pb->dataMem->release();
    pb->countMem->release();
    if (!me->fDead)
	sendAsyncResult(pb->fAsyncRef, res, args, 1);

    IOFree(pb, sizeof(*pb)+pb->frameLen);
    me->DecrementOutstandingIO();
    me->release(); 
}




void
IOUSBInterfaceUserClient::LowLatencyIsoReqComplete(void *obj, void *param, IOReturn res, IOUSBLowLatencyIsocFrame *pFrames)
{
    void *			args[1];
    LowLatencyIsoAsyncPB * 	pb = (LowLatencyIsoAsyncPB *) param;
    IOUSBInterfaceUserClient *	me = OSDynamicCast(IOUSBInterfaceUserClient, (OSObject*)obj);

    if (!me)
	return;
	
    args[0] = pb->frameBase;
    pb->countMem->writeBytes(0, pb->frames, pb->frameLen);
    pb->dataMem->complete();
    pb->dataMem->release();
    pb->countMem->release();
    if (!me->fDead)
	sendAsyncResult(pb->fAsyncRef, res, args, 1);

    IOFree(pb, sizeof(*pb)+pb->frameLen);
    me->DecrementOutstandingIO();
    me->release(); 
}




IOReturn 
IOUSBInterfaceUserClient::SetAlternateInterface(UInt8 altSetting)
{
    IOReturn	ret;
    
    USBLog(7, "+%s[%p]::SetAlternateInterface(%d)", getName(), this, altSetting);
    IncrementOutstandingIO();
    
    if (fOwner && !isInactive())
	ret = fOwner->SetAlternateInterface(this, altSetting);
    else
	ret = kIOReturnNotAttached;
	
    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::SetAlternateInterface - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::GetDevice(io_service_t *device)
{
    IOReturn		ret;
    
    IncrementOutstandingIO();

    if (fOwner && !isInactive())
    {
	// Although not documented, Simon says that exportObjectToClient consumes a reference,
	// so we have to put an extra retain on the device.  The user client side of the USB stack (USBLib)
	// always calls this routine in order to cache the USB device.  Radar #2586534
	fOwner->GetDevice()->retain();
	ret = exportObjectToClient(fTask, fOwner->GetDevice(), device);
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::GetDevice - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::GetFrameNumber(IOUSBGetFrameStruct *data, UInt32 *size)
{
    IOReturn		ret = kIOReturnSuccess;

    USBLog(7, "+%s[%p]::GetFrameNumber", getName(), this);

    if(*size != sizeof(IOUSBGetFrameStruct))
	return kIOReturnBadArgument;
    
    if (fOwner && !isInactive())
    {
	clock_get_uptime(&data->timeStamp);
	data->frame = fOwner->GetDevice()->GetBus()->GetFrameNumber();
    }
    else
        ret = kIOReturnNotAttached;

    if (ret)
	USBLog(3, "%s[%p]::GetFrameNumber - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::GetBandwidthAvailable(UInt32 *bandwidth)
{
    IOReturn		ret = kIOReturnSuccess;

    USBLog(7, "+%s[%p]::GetBandwidthAvailable", getName(), this);

    if (fOwner && !isInactive())
    {
	*bandwidth = fOwner->GetDevice()->GetBus()->GetBandwidthAvailable();
    }
    else
        ret = kIOReturnNotAttached;

    if (ret)
	USBLog(3, "%s[%p]::GetBandwidthAvailable - returning err %x", getName(), this, ret);
	
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::GetEndpointProperties(UInt8 alternateSetting, UInt8 endpointNumber, UInt8 direction, UInt32 *transferType, UInt32 *maxPacketSize, UInt32 *interval)
{
    IOReturn		ret = kIOReturnSuccess;
    UInt8		myTT;
    UInt16		myMPS;
    UInt8		myIV;

    USBLog(7, "+%s[%p]::GetEndpointProperties", getName(), this);

    if (fOwner && !isInactive())
	ret = fOwner->GetEndpointProperties(alternateSetting, endpointNumber, direction, &myTT, &myMPS, &myIV);
    else
        ret = kIOReturnNotAttached;

    if (ret)
	USBLog(3, "%s[%p]::GetEndpointProperties - returning err %x", getName(), this, ret);
    else
    {
        *transferType = myTT;
        *maxPacketSize = myMPS;
        *interval = myIV;
	USBLog(3, "%s[%p]::GetEndpointProperties - tt=%d, mps=%d, int=%d", getName(), this, *transferType, *maxPacketSize, *interval);
    }
    return ret;
}



IOUSBPipe*
IOUSBInterfaceUserClient::GetPipeObj(UInt8 pipeNo)
{
    IOUSBPipe *pipe = NULL;
    
    if (fOwner && !isInactive())
    {
        if (pipeNo == 0)
            pipe = fOwner->GetDevice()->GetPipeZero();
            
        if ((pipeNo > 0) && (pipeNo <= kUSBMaxPipes))
            pipe = fOwner->GetPipeObj(pipeNo-1);
    }
    // we need to retain the pipe object because it could actually get released by the device/interface
    // and we don't want it to go away. This retain should probably be done in IOUSBDevice or IOUSBInterface
    // but we would affect too many people if we did that right now.
    if (pipe)
        pipe->retain();
	
    return pipe;
}



// because of the way User Client works, these params need to be treated as pointers to 32 bit ints instead
// of pointers to smaller values, because on the other side they are automatically dereferenced as such
IOReturn
IOUSBInterfaceUserClient::GetPipeProperties(UInt8 pipeRef, UInt32 *direction, UInt32 *number, UInt32 *transferType, UInt32 *maxPacketSize, UInt32 *interval)
{
    IOUSBPipe 			*pipeObj;
    IOReturn			ret = kIOReturnSuccess;

    USBLog(7, "+%s[%p]::GetPipeProperties", getName(), this);

    IncrementOutstandingIO();
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);    
	if(pipeObj)
	{
	    if (direction)
		*direction = pipeObj->GetDirection();
	    if (number)
		*number = pipeObj->GetEndpointNumber();
	    if (transferType)
		*transferType = pipeObj->GetType();
	    if (maxPacketSize)
		*maxPacketSize = pipeObj->GetMaxPacketSize();
	    if (interval)
		*interval = pipeObj->GetInterval();
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::GetPipeProperties - returning err %x", getName(), this, ret);
    return ret;
}


IOReturn
IOUSBInterfaceUserClient::ReadPipe(UInt8 pipeRef, UInt32 noDataTimeout, UInt32 completionTimeout, void *buf, UInt32 *size)
{
    IOReturn 			ret;
    IOMemoryDescriptor *	mem;
    IOUSBPipe 			*pipeObj;

    USBLog(7, "+%s(%p)::ReadPipe(%d, %d, %d, %p, %p)", getName(), this, pipeRef, noDataTimeout, completionTimeout, buf, size);
    
    IncrementOutstandingIO();				// do this to "hold" ourselves until we complete
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    mem = IOMemoryDescriptor::withAddress(buf, *size, kIODirectionIn);
	    if(mem)
	    { 
                *size = 0;
		ret = pipeObj->Read(mem, noDataTimeout, completionTimeout, 0, size);
		mem->release();
	    }
	    else
		ret =  kIOReturnNoMemory;
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ReadPipe - returning err %x", getName(), this, ret);

    return ret;
}



/*
 * Out of line version of read pipe - the buffer isn't mapped (yet) into the kernel task
 */
IOReturn
IOUSBInterfaceUserClient::ReadPipeOOL(IOUSBBulkPipeReq *reqIn, UInt32 *sizeOut, IOByteCount inCount, IOByteCount *outCount)
{
    IOReturn 			ret;
    IOMemoryDescriptor *	mem;
    UInt8			pipeRef = reqIn->pipeRef;
    IOUSBPipe 			*pipeObj;

    USBLog(7, "+%s(%p)::ReadPipeOOL(%p, %p, %d, %d)", getName(), this, reqIn, sizeOut, inCount, *outCount);
    IncrementOutstandingIO();				// do this to "hold" ourselves until we complete
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
            *sizeOut = 0;
	    mem = IOMemoryDescriptor::withAddress((vm_address_t)reqIn->buf, reqIn->size, kIODirectionIn, fTask);
	    if(mem)
	    {
		ret = mem->prepare();
		if(ret == kIOReturnSuccess)
		    ret = pipeObj->Read(mem, reqIn->noDataTimeout, reqIn->completionTimeout, 0, sizeOut);
		mem->complete();
		mem->release();
	    }
	    else
	    {
		ret = kIOReturnNoMemory;
	    }
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ReadPipeOOL - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn
IOUSBInterfaceUserClient::WritePipe(UInt8 pipeRef, UInt32 noDataTimeout, UInt32 completionTimeout, void *buf, UInt32 size)
{
    IOReturn 			ret;
    IOMemoryDescriptor *	mem;
    IOUSBPipe 			*pipeObj;

    USBLog(7, "+%s(%p)::WritePipe(%d, %d, %d, %p, %p)", getName(), this, pipeRef, noDataTimeout, completionTimeout, buf, size);
    IncrementOutstandingIO();				// do this to "hold" ourselves until we complete

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    mem = IOMemoryDescriptor::withAddress(buf, size, kIODirectionOut);
	    if(mem) 
	    {
		ret = pipeObj->Write(mem, noDataTimeout, completionTimeout);
		mem->release();
	    }
	    else
		ret = kIOReturnNoMemory;
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::WritePipe - returning err %x", getName(), this, ret);
    return ret;
}



/*
 * Out of line version of write pipe - the buffer isn't mapped (yet) into the kernel task
 */
IOReturn
IOUSBInterfaceUserClient::WritePipeOOL(IOUSBBulkPipeReq *req, IOByteCount inCount)
{
    IOReturn 			ret;
    IOMemoryDescriptor *	mem;
    IOUSBPipe 			*pipeObj;

    USBLog(7, "+%s(%p)::WritePipeOOL(%d)", getName(), this, inCount);
    
    IncrementOutstandingIO();				// do this to "hold" ourselves until we complete

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(req->pipeRef);
	if(pipeObj)
	{
	    mem = IOMemoryDescriptor::withAddress((vm_address_t)req->buf, req->size, kIODirectionOut, fTask);
	    if(mem) 
	    {
		ret = mem->prepare();
		if(ret == kIOReturnSuccess)
		    ret = pipeObj->Write(mem, req->noDataTimeout, req->completionTimeout);
		mem->complete();
		mem->release();
	    }
	    else
		ret = kIOReturnNoMemory;
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::WritePipeOOL - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::GetPipeStatus(UInt8 pipeRef)
{
    IOUSBPipe 		*pipeObj;
    IOReturn		ret;
    
    USBLog(7, "+%s[%p]::GetPipeStatus", getName(), this);
    
    IncrementOutstandingIO();

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    ret = pipeObj->GetPipeStatus();
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
    
    DecrementOutstandingIO();
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::AbortPipe(UInt8 pipeRef)
{
    IOUSBPipe 		*pipeObj;
    IOReturn		ret;
    
    USBLog(7, "+%s[%p]::AbortPipe", getName(), this);
    
    IncrementOutstandingIO();
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    ret =  pipeObj->Abort();
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
    {
        if ( ret == kIOUSBUnknownPipeErr )
            USBLog(5, "%s[%p]::AbortPipe - returning err %x", getName(), this, ret);
        else
            USBLog(3, "%s[%p]::AbortPipe - returning err %x", getName(), this, ret);
    }
    
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::ResetPipe(UInt8 pipeRef)
{
    IOUSBPipe 			*pipeObj;
    IOReturn			ret;

    USBLog(7, "+%s[%p]::ResetPipe", getName(), this);

    IncrementOutstandingIO();

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    ret = pipeObj->Reset();
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ResetPipe - returning err %x", getName(), this, ret);
    
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::SetPipeIdle(UInt8 pipeRef)
{
    if (!fOwner || isInactive())
        return kIOReturnNotAttached;
        
    return(0);
}



IOReturn 
IOUSBInterfaceUserClient::SetPipeActive(UInt8 pipeRef)
{
    if (!fOwner || isInactive())
        return kIOReturnNotAttached;
        
    return(0);
}



IOReturn 
IOUSBInterfaceUserClient::ClearPipeStall(UInt8 pipeRef, bool bothEnds)
{
    IOUSBPipe 		*pipeObj;
    IOReturn		ret;

    USBLog(7, "+%s[%p]::ClearPipeStall", getName(), this);

    IncrementOutstandingIO();
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    USBLog(2, "%s[%p]::ClearPipeStall = bothEnds = %d", getName(), this, bothEnds);
	    ret = pipeObj->ClearPipeStall(bothEnds);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ClearPipeStall - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::SetPipePolicy(UInt8 pipeRef, UInt16 maxPacketSize, UInt8 maxInterval)
{
    IOUSBPipe 		*pipeObj;
    IOReturn		ret;

    USBLog(7, "+%s[%p]::SetPipePolicy", getName(), this);

    IncrementOutstandingIO();
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if(pipeObj)
	{
	    USBLog(2, "%s[%p]::SetPipePolicy(%d, %d)", getName(), this, maxPacketSize, maxInterval);
	    ret = pipeObj->SetPipePolicy(maxPacketSize, maxInterval);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::SetPipePolicy - returning err %x", getName(), this, ret);
    return ret;
}



/*
 * There's a limit of max 6 arguments to user client methods, so the type, recipient and request
 * are packed into one 16 bit integer.
 */
IOReturn
IOUSBInterfaceUserClient::ControlRequestOut(UInt32 param1, UInt32 param2, UInt32 noDataTimeout, UInt32 completionTimeout, void *buf, UInt32 size)
{
    IOReturn 		ret;
    IOUSBDevRequest	req;
    UInt8		pipeRef = (param1 >> 16) & 0xFF;
    UInt8		bmRequestType = (param1 >> 8) & 0xFF;
    UInt8		bRequest = param1 & 0xFF;
    UInt16		wValue = (param2 >> 16) & 0xFFFF;
    UInt16		wIndex = param2 & 0xFFFF;
    IOUSBPipe		*pipeObj;
    
    USBLog(7, "+%s[%p]::ControlRequestOut", getName(), this);

    IncrementOutstandingIO();
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if (pipeObj)
	{
	    req.bmRequestType = bmRequestType;
	    req.bRequest = bRequest;
	    req.wValue = wValue;
	    req.wIndex = wIndex;
	    req.wLength = size;
	    req.pData = buf;
	    ret = pipeObj->ControlRequest(&req, noDataTimeout, completionTimeout);
	
	    if(kIOReturnSuccess != ret) 
		USBLog(3, "%s[%p]::ControlRequestOut err:0x%x", getName(), this, ret);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
	
    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ControlRequestOut - returning err %x", getName(), this, ret);
    return ret;
}



IOReturn
IOUSBInterfaceUserClient::ControlRequestIn(UInt32 param1, UInt32 param2, UInt32 noDataTimeout, UInt32 completionTimeout, void *buf, UInt32 *size)
{
    IOReturn 		ret;
    IOUSBDevRequest	req;
    UInt8		pipeRef = (param1 >> 16) & 0xFF;
    UInt8		bmRequestType = (param1 >> 8) & 0xFF;
    UInt8		bRequest = param1 & 0xFF;
    UInt16		wValue = (param2 >> 16) & 0xFFFF;
    UInt16		wIndex = param2 & 0xFFFF;
    IOUSBPipe		*pipeObj;
    
    USBLog(7, "+%s[%p]::ControlRequestIn", getName(), this);

    IncrementOutstandingIO();

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipeRef);
	if (pipeObj)
	{
	    req.bmRequestType = bmRequestType;
	    req.bRequest = bRequest;
	    req.wValue = wValue;
	    req.wIndex = wIndex;
	    req.wLength = *size;
	    req.pData = buf;
	    ret = pipeObj->ControlRequest(&req, noDataTimeout, completionTimeout);
	
	    if(ret == kIOReturnSuccess) 
		*size = req.wLenDone;
	    else 
	    {
		USBLog(3, "%s[%p]::ControlRequestIn err:0x%x", getName(), this, ret);
		*size = 0;
	    }
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ControlRequestIn - returning err %x", getName(), this, ret);
    return ret;
}



//
// ControlRequestOutOOL: reqIn->wLength > 4K
//
IOReturn
IOUSBInterfaceUserClient::ControlRequestOutOOL(IOUSBDevReqOOLTO *reqIn, IOByteCount inCount)
{
    IOReturn 			ret;
    IOUSBDevRequestDesc		req;
    IOMemoryDescriptor *	mem;
    IOUSBPipe *			pipeObj;

    USBLog(7, "+%s[%p]::ControlRequestOutOOL", getName(), this);

    if(inCount != sizeof(IOUSBDevReqOOLTO))
        return kIOReturnBadArgument;

    IncrementOutstandingIO();
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(reqIn->pipeRef);
	if(pipeObj)
	{
	    mem = IOMemoryDescriptor::withAddress((vm_address_t)reqIn->pData, reqIn->wLength, kIODirectionOut, fTask);
	    if(mem) 
	    {
		req.bmRequestType = reqIn->bmRequestType;
		req.bRequest = reqIn->bRequest;
		req.wValue = reqIn->wValue;
		req.wIndex = reqIn->wIndex;
		req.wLength = reqIn->wLength;
		req.pData = mem;
	    
		ret = mem->prepare();
		if(ret == kIOReturnSuccess)
		    ret = pipeObj->ControlRequest(&req, reqIn->noDataTimeout, reqIn->completionTimeout);
		mem->complete();
		mem->release();
	    }
	    else
		ret = kIOReturnNoMemory;
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ControlRequestOutOOL - returning err %x", getName(), this, ret);
    return ret;
}



//
// ControlRequestInOOL: reqIn->wLength > 4K
//
IOReturn
IOUSBInterfaceUserClient::ControlRequestInOOL(IOUSBDevReqOOLTO *reqIn, UInt32 *sizeOut, IOByteCount inCount, IOByteCount *outCount)
{
    IOReturn 			ret;
    IOUSBDevRequestDesc		req;
    IOMemoryDescriptor *	mem;
    IOUSBPipe *			pipeObj;

    USBLog(7, "+%s[%p]::ControlRequestInOOL", getName(), this);

    if((inCount != sizeof(IOUSBDevReqOOLTO)) || (*outCount != sizeof(UInt32)))
        return kIOReturnBadArgument;

    IncrementOutstandingIO();

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(reqIn->pipeRef);
	if(pipeObj)
	{
	    mem = IOMemoryDescriptor::withAddress((vm_address_t)reqIn->pData, reqIn->wLength, kIODirectionIn, fTask);
	    if(mem) 
	    {
		req.bmRequestType = reqIn->bmRequestType;
		req.bRequest = reqIn->bRequest;
		req.wValue = reqIn->wValue;
		req.wIndex = reqIn->wIndex;
		req.wLength = reqIn->wLength;
		req.pData = mem;
	    
		ret = mem->prepare();
		if(ret == kIOReturnSuccess)
		    ret = pipeObj->ControlRequest(&req, reqIn->noDataTimeout, reqIn->completionTimeout);
	
		mem->complete();
		mem->release();
		if(ret == kIOReturnSuccess) 
		{
		    *sizeOut = req.wLenDone;
		}
		else 
		{
		    USBLog(3, "%s[%p]::ControlRequestInOOL err:0x%x", getName(), this, ret);
		    *sizeOut = 0;
		}
		*outCount = sizeof(UInt32);
	    }
	    else
		ret = kIOReturnNoMemory;
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    DecrementOutstandingIO();
    if (ret)
	USBLog(3, "%s[%p]::ControlRequestInOOL - returning err %x", getName(), this, ret);
    return ret;
}


// ASYNC METHODS

IOReturn 
IOUSBInterfaceUserClient::SetAsyncPort(OSAsyncReference asyncRef)
{
    if (!fOwner || isInactive())
        return kIOReturnNotAttached;
        
    fWakePort = (mach_port_t) asyncRef[0];
    return kIOReturnSuccess;
}


IOReturn
IOUSBInterfaceUserClient::ControlAsyncRequestOut(OSAsyncReference asyncRef, IOUSBDevReqOOLTO *reqIn, IOByteCount inCount)
{
    IOReturn 			ret = kIOReturnSuccess;
    IOUSBPipe *			pipeObj;

    IOUSBCompletion		tap;
    AsyncPB * 			pb = NULL;
    IOMemoryDescriptor *	mem = NULL;

    USBLog(7, "+%s[%p]::ControlAsyncRequestOut", getName(), this);

    if(inCount != sizeof(IOUSBDevReqOOLTO))
        return kIOReturnBadArgument;

    retain();					// to make sure ReqComplete is still around
    IncrementOutstandingIO();			// to make sure ReqComplete is still around

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(reqIn->pipeRef);
	if(pipeObj)
	{
	    if (reqIn->wLength)
	    {
		if (reqIn->pData)
		{
		    mem = IOMemoryDescriptor::withAddress((vm_address_t)reqIn->pData, reqIn->wLength, kIODirectionOut, fTask);
		    if (!mem)
			ret = kIOReturnNoMemory;
		}
		else
		    ret = kIOReturnBadArgument;
	    }
	    if (ret == kIOReturnSuccess)
	    {
		pb = (AsyncPB *)IOMalloc(sizeof(AsyncPB));
		if(!pb) 
		    ret = kIOReturnNoMemory;
	    }
	    
	    if (mem && (ret == kIOReturnSuccess))
		ret = mem->prepare();
	
	    if (ret == kIOReturnSuccess)
	    {
		pb->req.bmRequestType = reqIn->bmRequestType;
		pb->req.bRequest = reqIn->bRequest;
		pb->req.wValue = reqIn->wValue;
		pb->req.wIndex = reqIn->wIndex;
		pb->req.wLength = reqIn->wLength;
		pb->req.pData = mem;
	    
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->fMax = reqIn->wLength;
		pb->fMem = mem;
		tap.target = this;
		tap.action = &ReqComplete;	// Want same number of reply args as for ControlReqIn
		tap.parameter = pb;
		ret = pipeObj->ControlRequest(&pb->req, reqIn->noDataTimeout, reqIn->completionTimeout, &tap);
	    }
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
    
    if(ret != kIOReturnSuccess) 
    {
	USBLog(3, "%s[%p]::ControlAsyncRequestOut err 0x%x", getName(), this, ret);
	if(mem) 
	{
	    mem->complete();
	    mem->release();
	}
	if(pb)
	    IOFree(pb, sizeof(*pb));
	
        // only decrement if we are not going to be successful. otherwise, this will be done in the completion
	DecrementOutstandingIO();
        release();
    }
    return ret;
}



IOReturn
IOUSBInterfaceUserClient::ControlAsyncRequestIn(OSAsyncReference asyncRef, IOUSBDevReqOOLTO *reqIn, IOByteCount inCount)
{
    IOReturn 			ret = kIOReturnSuccess;
    IOUSBPipe *			pipeObj;

    IOUSBCompletion		tap;
    AsyncPB * 			pb = NULL;
    IOMemoryDescriptor *	mem = NULL;

    USBLog(7, "+%s[%p]::ControlAsyncRequestIn", getName(), this);

    if(inCount != sizeof(IOUSBDevReqOOLTO))
        return kIOReturnBadArgument;

    retain();
    IncrementOutstandingIO();		// to make sure ReqComplete is still around

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(reqIn->pipeRef);
	if(pipeObj)
	{
	    if (reqIn->wLength)
	    {
		if (reqIn->pData)
		{
		    mem = IOMemoryDescriptor::withAddress((vm_address_t)reqIn->pData, reqIn->wLength, kIODirectionIn, fTask);
		    if (!mem)
			ret = kIOReturnNoMemory;
		}
		else
		    ret = kIOReturnBadArgument;
	    }
	    if (ret == kIOReturnSuccess)
	    {
		pb = (AsyncPB *)IOMalloc(sizeof(AsyncPB));
		if(!pb) 
		    ret = kIOReturnNoMemory;
	    }
	    if (mem && (ret == kIOReturnSuccess))
		ret = mem->prepare();
	
	    if (ret == kIOReturnSuccess)
	    {
		pb->req.bmRequestType = reqIn->bmRequestType;
		pb->req.bRequest = reqIn->bRequest;
		pb->req.wValue = reqIn->wValue;
		pb->req.wIndex = reqIn->wIndex;
		pb->req.wLength = reqIn->wLength;
		pb->req.pData = mem;
	    
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->fMax = reqIn->wLength;
		pb->fMem = mem;
		tap.target = this;
		tap.action = &ReqComplete;
		tap.parameter = pb;
		ret = pipeObj->ControlRequest(&pb->req, reqIn->noDataTimeout, reqIn->completionTimeout, &tap);
	    }
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
        
    if(ret != kIOReturnSuccess) 
    {
	USBLog(3, "%s[%p]::ControlAsyncRequestIn err 0x%x", getName(), this, ret);
	if(mem) 
	{
	    mem->complete();
	    mem->release();
	}
	if(pb)
	    IOFree(pb, sizeof(*pb));
	
        // only decrement if we are not going to be successful. otherwise, this will be done in the completion
	DecrementOutstandingIO();
        release();
    }

    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::DoIsochPipeAsync(OSAsyncReference asyncRef, IOUSBIsocStruct *stuff, IODirection direction)
{
    IOReturn 			ret;
    IOUSBPipe *			pipeObj;
    IOUSBIsocCompletion		tap;
    IOMemoryDescriptor *	dataMem = NULL;
    IOMemoryDescriptor *	countMem = NULL;
    int				frameLen = 0;	// In bytes
    IsoAsyncPB * 		pb = NULL;

    USBLog(7, "+%s[%p]::DoIsochPipeAsync", getName(), this);
    retain();
    IncrementOutstandingIO();		// to make sure IsoReqComplete is still around
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(stuff->fPipe);
	if(pipeObj)
	{
	    frameLen = stuff->fNumFrames * sizeof(IOUSBIsocFrame);
	    do {
		dataMem = IOMemoryDescriptor::withAddress((vm_address_t)stuff->fBuffer, stuff->fBufSize, direction, fTask);
		if(!dataMem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		ret = dataMem->prepare();
		if(ret != kIOReturnSuccess)
		    break;
	
		countMem = IOMemoryDescriptor::withAddress((vm_address_t)stuff->fFrameCounts, frameLen, kIODirectionOutIn, fTask);
		if(!countMem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		// Copy in requested transfers, we'll copy out result in completion routine
		pb = (IsoAsyncPB *)IOMalloc(sizeof(IsoAsyncPB) + frameLen);
		if(!pb) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
	
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->frameLen = frameLen;
		pb->frameBase = stuff->fFrameCounts;
		pb->dataMem = dataMem;
		pb->countMem = countMem;
		countMem->readBytes(0, pb->frames, frameLen);
		tap.target = this;
		tap.action = &IsoReqComplete;
		tap.parameter = pb;
		if(direction == kIODirectionOut)
		    ret = pipeObj->Write(dataMem, stuff->fStartFrame, stuff->fNumFrames, pb->frames, &tap);
		else
		    ret = pipeObj->Read(dataMem, stuff->fStartFrame, stuff->fNumFrames, pb->frames, &tap);
	    } while (false);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    if(kIOReturnSuccess != ret) 
    {
	USBLog(3, "%s[%p]::DoIsochPipeAsync err 0x%x", getName(), this, ret);
	if(dataMem)
	    dataMem->release();
	if(countMem)
	    countMem->release();
	if(pb)
	    IOFree(pb, sizeof(*pb) + frameLen);
	DecrementOutstandingIO();
        release();
    }

    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::DoLowLatencyIsochPipeAsync(OSAsyncReference asyncRef, IOUSBLowLatencyIsocStruct *stuff, IODirection direction)
{
    IOReturn 			ret;
    IOUSBPipe *			pipeObj;
    IOUSBLowLatencyIsocCompletion		tap;
    IOMemoryDescriptor *	dataMem = NULL;
    IOMemoryDescriptor *	countMem = NULL;
    int				frameLen = 0;	// In bytes
    LowLatencyIsoAsyncPB * 	pb = NULL;

    USBLog(7, "+%s[%p]::DoLowLatencyIsochPipeAsync", getName(), this);
    retain();
    IncrementOutstandingIO();		// to make sure LowLatencyIsoReqComplete is still around
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(stuff->fPipe);
	if(pipeObj)
	{
	    frameLen = stuff->fNumFrames * sizeof(IOUSBLowLatencyIsocFrame);
	    do {
		dataMem = IOMemoryDescriptor::withAddress((vm_address_t)stuff->fBuffer, stuff->fBufSize, direction, fTask);
		if(!dataMem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		ret = dataMem->prepare();
		if(ret != kIOReturnSuccess)
		    break;
	
		countMem = IOMemoryDescriptor::withAddress((vm_address_t)stuff->fFrameCounts, frameLen, kIODirectionOutIn, fTask);
		if(!countMem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		// Copy in requested transfers, we'll copy out result in completion routine
		pb = (LowLatencyIsoAsyncPB *)IOMalloc(sizeof(LowLatencyIsoAsyncPB) + frameLen);
		if(!pb) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
	
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->frameLen = frameLen;
		pb->frameBase = stuff->fFrameCounts;
		pb->dataMem = dataMem;
		pb->countMem = countMem;
		countMem->readBytes(0, pb->frames, frameLen);
		tap.target = this;
		tap.action = &LowLatencyIsoReqComplete;
		tap.parameter = pb;
		if(direction == kIODirectionOut)
		    ret = pipeObj->Write(dataMem, stuff->fStartFrame, stuff->fNumFrames, pb->frames, &tap, stuff->fUpdateFrequency);
		else
		    ret = pipeObj->Read(dataMem, stuff->fStartFrame, stuff->fNumFrames, pb->frames, &tap, stuff->fUpdateFrequency);
	    } while (false);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;

    if(kIOReturnSuccess != ret) 
    {
	USBLog(3, "%s[%p]::DoLowLatencyIsochPipeAsync err 0x%x", getName(), this, ret);
	if(dataMem)
	    dataMem->release();
	if(countMem)
	    countMem->release();
	if(pb)
	    IOFree(pb, sizeof(*pb) + frameLen);
	DecrementOutstandingIO();
        release();
    }

    USBLog(7, "-%s[%p]::DoLowLatencyIsochPipeAsync", getName(), this);
    return ret;
}



IOReturn 
IOUSBInterfaceUserClient::ReadIsochPipe(OSAsyncReference asyncRef, IOUSBIsocStruct *stuff, UInt32 sizeIn)
{
    return DoIsochPipeAsync(asyncRef, stuff, kIODirectionIn);
}



IOReturn 
IOUSBInterfaceUserClient::WriteIsochPipe(OSAsyncReference asyncRef, IOUSBIsocStruct *stuff, UInt32 sizeIn)
{
    return DoIsochPipeAsync(asyncRef, stuff, kIODirectionOut);
}

IOReturn 
IOUSBInterfaceUserClient::LowLatencyReadIsochPipe(OSAsyncReference asyncRef, IOUSBLowLatencyIsocStruct *stuff, UInt32 sizeIn)
{
    return DoLowLatencyIsochPipeAsync(asyncRef, stuff, kIODirectionIn);
}



IOReturn 
IOUSBInterfaceUserClient::LowLatencyWriteIsochPipe(OSAsyncReference asyncRef, IOUSBLowLatencyIsocStruct *stuff, UInt32 sizeIn)
{
    return DoLowLatencyIsochPipeAsync(asyncRef, stuff, kIODirectionOut);
}


/*
 * Async version of read pipe - the buffer isn't mapped (yet) into the kernel task
 */
IOReturn
IOUSBInterfaceUserClient::AsyncReadPipe(OSAsyncReference asyncRef, UInt32 pipe, void *buf, UInt32 size, UInt32 noDataTimeout, UInt32 completionTimeout)
{
    IOReturn 			ret;
    IOUSBPipe 			*pipeObj;
    IOUSBCompletion		tap;
    IOMemoryDescriptor *	mem = NULL;
    AsyncPB * 			pb = NULL;

    USBLog(7, "+%s[%p]::AsyncReadPipe", getName(), this);
    retain();
    IncrementOutstandingIO();		// to make sure ReqComplete is still around
    
    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipe);
	if(pipeObj)
	{
	    do {
		mem = IOMemoryDescriptor::withAddress((vm_address_t)buf, size, kIODirectionIn, fTask);
		if(!mem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		ret = mem->prepare();
		if(ret != kIOReturnSuccess)
		    break;
	
		pb = (AsyncPB *)IOMalloc(sizeof(AsyncPB));
		if(!pb) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
	
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->fMax = size;
		pb->fMem = mem;
		tap.target = this;
		tap.action = &ReqComplete;
		tap.parameter = pb;
		ret = pipeObj->Read(mem, noDataTimeout, completionTimeout, &tap, NULL);
	    } while (false);
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
    
    if(ret != kIOReturnSuccess) 
    {
	USBLog(3, "%s[%p]::AsyncReadPipe err 0x%x", getName(), this, ret);
	if(mem) 
	{
	    mem->complete();
	    mem->release();
	}
	if(pb)
	    IOFree(pb, sizeof(*pb));
	
        DecrementOutstandingIO();
        release();
    }
    return ret;
}



/*
 * Async version of write pipe - the buffer isn't mapped (yet) into the kernel task
 */
IOReturn
IOUSBInterfaceUserClient::AsyncWritePipe(OSAsyncReference asyncRef, UInt32 pipe, void *buf, UInt32 size, UInt32 noDataTimeout, UInt32 completionTimeout)
{
    IOReturn 			ret;
    IOUSBPipe 			*pipeObj;
    IOUSBCompletion		tap;
    IOMemoryDescriptor *	mem = NULL;
    AsyncPB * 			pb = NULL;

    USBLog(7, "+%s[%p]::AsyncWritePipe", getName(), this);
    retain();
    IncrementOutstandingIO();		// to make sure ReqComplete is still around

    if (fOwner && !isInactive())
    {
	pipeObj = GetPipeObj(pipe);
	if(pipeObj)
	{
	    do {
		mem = IOMemoryDescriptor::withAddress((vm_address_t)buf, size, kIODirectionOut, fTask);
		if(!mem) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
		ret = mem->prepare();
		if(ret != kIOReturnSuccess)
		    break;
	
		pb = (AsyncPB *)IOMalloc(sizeof(AsyncPB));
		if(!pb) 
		{
		    ret = kIOReturnNoMemory;
		    break;
		}
	
		bcopy(asyncRef, pb->fAsyncRef, sizeof(OSAsyncReference));
		pb->fMax = size;
		pb->fMem = mem;
		tap.target = this;
		tap.action = &ReqComplete;
		tap.parameter = pb;
		ret = pipeObj->Write(mem, noDataTimeout, completionTimeout, &tap);
	    } while (false);
	
	    pipeObj->release();
	}
	else
	    ret = kIOUSBUnknownPipeErr;
    }
    else
        ret = kIOReturnNotAttached;
    
    if(ret != kIOReturnSuccess) 
    {
	USBLog(3, "%s[%p]::AsyncWritePipe err 0x%x", getName(), this, ret);
	if(mem) 
	{
	    mem->complete();
	    mem->release();
	}
	if(pb)
	    IOFree(pb, sizeof(*pb));
            
	DecrementOutstandingIO();
        release();
    }
    return ret;
}


//
// stop
// 
// This IOService method is called AFTER we have closed our provider, assuming that the provider was 
// ever opened. If we issue I/O to the provider, then we must have it open, and we will not close
// our provider until all of that I/O is completed.
void 
IOUSBInterfaceUserClient::stop(IOService * provider)
{
    
    USBLog(7, "+%s[%p]::stop(%p)", getName(), this, provider);

    if (GetOutstandingIO() > 0)
	USBError(1, "%s[%p]::stop called with outstanding IO!!", getName(), this);
	
    if (fGate)
    {
	IOWorkLoop		*wl = getWorkLoop();
	if (wl)
	{
	    wl->removeEventSource(fGate);
	}
	else
	{
	    USBError(1, "%s[%p]::stop - have gate, but no valid workloop!", getName(), this);
	}
	fGate->release();
	fGate = NULL;
    }
    super::stop(provider);

    USBLog(7, "-%s[%p]::stop(%p)", getName(), this, provider);

}

void 
IOUSBInterfaceUserClient::free()
{
    
    if (fGate)
    {
	IOWorkLoop		*wl = getWorkLoop();
	if (wl)
	{
	    wl->removeEventSource(fGate);
	}
	else
	{
	    USBError(1, "%s[%p]::free - have gate, but no valid workloop!", getName(), this);
	}
	fGate->release();
	fGate = NULL;
    }
    super::free();
}


bool 
IOUSBInterfaceUserClient::finalize( IOOptionBits options )
{
    bool ret;

    USBLog(7, "+%s[%p]::finalize(%08x)", getName(), this, (int)options);
    
    ret = super::finalize(options);
    
    USBLog(7, "-%s[%p]::finalize(%08x) - returning %s", getName(), this, (int)options, ret ? "true" : "false");
    return ret;
}


bool
IOUSBInterfaceUserClient::willTerminate( IOService * provider, IOOptionBits options )
{
    // this method is intended to be used to stop any pending I/O and to make sure that 
    // we have begun getting our callbacks in order. by the time we get here, the 
    // isInactive flag is set, so we really are marked as being done. we will do in here
    // what we used to do in the message method (this happens first)
    USBLog(3, "%s[%p]::willTerminate isInactive = %d", getName(), this, isInactive());
    return super::willTerminate(provider, options);
}


bool
IOUSBInterfaceUserClient::didTerminate( IOService * provider, IOOptionBits options, bool * defer )
{
    // this method comes at the end of the termination sequence. Hopefully, all of our outstanding IO is complete
    // in which case we can just close our provider and IOKit will take care of the rest. Otherwise, we need to 
    // hold on to the device and IOKit will terminate us when we close it later
    USBLog(3, "%s[%p]::didTerminate isInactive = %d, outstandingIO = %d", getName(), this, isInactive(), fOutstandingIO);
    if ( GetOutstandingIO() == 0 )
	fOwner->close(this);
    else
	fNeedToClose = true;
    return super::didTerminate(provider, options, defer);
}


void
IOUSBInterfaceUserClient::DecrementOutstandingIO(void)
{
    if (!fGate)
    {
	if (!--fOutstandingIO && fNeedToClose)
	{
	    USBLog(3, "%s[%p]::DecrementOutstandingIO isInactive = %d, outstandingIO = %d - closing device", getName(), this, isInactive(), fOutstandingIO);
	    fOwner->close(this);
	}
	return;
    }
    fGate->runAction(ChangeOutstandingIO, (void*)-1);
}


void
IOUSBInterfaceUserClient::IncrementOutstandingIO(void)
{
    if (!fGate)
    {
	fOutstandingIO++;
	return;
    }
    fGate->runAction(ChangeOutstandingIO, (void*)1);
}


IOReturn
IOUSBInterfaceUserClient::ChangeOutstandingIO(OSObject *target, void *param1, void *param2, void *param3, void *param4)
{
    IOUSBInterfaceUserClient *me = OSDynamicCast(IOUSBInterfaceUserClient, target);
    UInt32	direction = (UInt32)param1;
    
    if (!me)
    {
	USBLog(1, "IOUSBInterfaceUserClient::ChangeOutstandingIO - invalid target");
	return kIOReturnSuccess;
    }
    switch (direction)
    {
	case 1:
	    me->fOutstandingIO++;
	    break;
	    
	case -1:
	    if (!--me->fOutstandingIO && me->fNeedToClose)
	    {
		USBLog(3, "%s[%p]::ChangeOutstandingIO isInactive = %d, outstandingIO = %d - closing device", me->getName(), me, me->isInactive(), me->fOutstandingIO);
		me->fOwner->close(me);
	    }
	    break;
	    
	default:
	    USBLog(1, "%s[%p]::ChangeOutstandingIO - invalid direction", me->getName(), me);
    }
    return kIOReturnSuccess;
}


UInt32
IOUSBInterfaceUserClient::GetOutstandingIO()
{
    UInt32	count = 0;
    
    if (!fGate)
    {
	return fOutstandingIO;
    }
    
    fGate->runAction(GetGatedOutstandingIO, (void*)&count);
    
    return count;
}

IOReturn
IOUSBInterfaceUserClient::GetGatedOutstandingIO(OSObject *target, void *param1, void *param2, void *param3, void *param4)
{
    IOUSBInterfaceUserClient *me = OSDynamicCast(IOUSBInterfaceUserClient, target);

    if (!me)
    {
	USBLog(1, "IOUSBInterfaceUserClient::GetGatedOutstandingIO - invalid target");
	return kIOReturnSuccess;
    }

    *(UInt32 *) param1 = me->fOutstandingIO;

    return kIOReturnSuccess;
}


