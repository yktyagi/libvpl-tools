/*############################################################################
  # Copyright (C) 2005 Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include "base_allocator.h"
#include <assert.h>
#include <algorithm>
#include "vm/thread_defs.h"

MFXFrameAllocator::MFXFrameAllocator() {
    pthis  = this;
    Alloc  = Alloc_;
    Lock   = Lock_;
    Free   = Free_;
    Unlock = Unlock_;
    GetHDL = GetHDL_;
}

MFXFrameAllocator::~MFXFrameAllocator() {}

mfxStatus MFXFrameAllocator::Alloc_(mfxHDL pthis,
                                    mfxFrameAllocRequest* request,
                                    mfxFrameAllocResponse* response) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXFrameAllocator& self = *(MFXFrameAllocator*)pthis;

    return self.AllocFrames(request, response);
}

mfxStatus MFXFrameAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXFrameAllocator& self = *(MFXFrameAllocator*)pthis;

    return self.LockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXFrameAllocator& self = *(MFXFrameAllocator*)pthis;

    return self.UnlockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Free_(mfxHDL pthis, mfxFrameAllocResponse* response) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXFrameAllocator& self = *(MFXFrameAllocator*)pthis;

    return self.FreeFrames(response);
}

mfxStatus MFXFrameAllocator::GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL* handle) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXFrameAllocator& self = *(MFXFrameAllocator*)pthis;

    return self.GetFrameHDL(mid, handle);
}

BaseFrameAllocator::BaseFrameAllocator() : mtx(), m_responses(), m_ExtResponses() {}

BaseFrameAllocator::~BaseFrameAllocator() {}

mfxStatus BaseFrameAllocator::CheckRequestType(mfxFrameAllocRequest* request) {
    if (0 == request)
        return MFX_ERR_NULL_PTR;

    // check that Media SDK component is specified in request
    if ((request->Type & MEMTYPE_FROM_MASK) != 0)
        return MFX_ERR_NONE;
    else
        return MFX_ERR_UNSUPPORTED;
}

mfxStatus BaseFrameAllocator::ReallocFrame(mfxMemId midIn,
                                           const mfxFrameInfo* info,
                                           mfxU16 memType,
                                           mfxMemId* midOut) {
    return ReallocImpl(midIn, info, memType, midOut);
}

mfxStatus BaseFrameAllocator::AllocFrames(mfxFrameAllocRequest* request,
                                          mfxFrameAllocResponse* response) {
    if (0 == request || 0 == response || 0 == request->NumFrameSuggested)
        return MFX_ERR_MEMORY_ALLOC;

    if (MFX_ERR_NONE != CheckRequestType(request))
        return MFX_ERR_UNSUPPORTED;

    mfxStatus sts = MFX_ERR_NONE;

    if ( // External Frames
        ((request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) &&
         (request->Type & (MFX_MEMTYPE_FROM_DECODE | MFX_MEMTYPE_FROM_ENC | MFX_MEMTYPE_FROM_PAK)))
        // Exception: Internal Frames for FEI ENC / PAK reconstructs
        || ((request->Type & MFX_MEMTYPE_INTERNAL_FRAME) &&
            (request->Type & (MFX_MEMTYPE_FROM_ENC | MFX_MEMTYPE_FROM_PAK)))) {
        bool foundInCache = false;
        // external decoder allocations
        std::list<UniqueResponse>::iterator it = m_ExtResponses.begin(), et = m_ExtResponses.end();
        UniqueResponse checker(*response, request->Info.Width, request->Info.Height, request->Type);
        for (; it != et; ++it) {
            // same decoder and same size
            if (request->AllocId == it->AllocId && checker(*it)) {
                // check if enough frames were allocated
                if (request->NumFrameSuggested > it->NumFrameActual)
                    return MFX_ERR_MEMORY_ALLOC;

                it->m_refCount++;
                // return existing response
                *response    = (mfxFrameAllocResponse&)*it;
                foundInCache = true;
            }
        }

        if (!foundInCache) {
            sts = AllocImpl(request, response);
            if (sts == MFX_ERR_NONE) {
                response->AllocId = request->AllocId;
                m_ExtResponses.push_back(
                    UniqueResponse(*response,
                                   request->Info.Width,
                                   request->Info.Height,
                                   UniqueResponse::CropMemoryTypeToStore(request->Type)));
            }
        }
    }
    else {
        // internal allocations

        // reserve space before allocation to avoid memory leak
        m_responses.push_back(mfxFrameAllocResponse());

        sts = AllocImpl(request, response);
        if (sts == MFX_ERR_NONE) {
            m_responses.back() = *response;
        }
        else {
            m_responses.pop_back();
        }
    }

    return sts;
}

mfxStatus BaseFrameAllocator::FreeFrames(mfxFrameAllocResponse* response) {
    std::lock_guard<std::mutex> lock(mtx);

    if (response == 0)
        return MFX_ERR_INVALID_HANDLE;

    mfxStatus sts = MFX_ERR_NONE;

    // check whether response is an external decoder response
    std::list<UniqueResponse>::iterator i =
        std::find_if(m_ExtResponses.begin(),
                     m_ExtResponses.end(),
                     std::bind(IsSame(), *response, std::placeholders::_1));

    if (i != m_ExtResponses.end()) {
        if ((--i->m_refCount) == 0) {
            sts = ReleaseResponse(response);
            m_ExtResponses.erase(i);
        }
        return sts;
    }

    // if not found so far, then search in internal responses
    std::list<mfxFrameAllocResponse>::iterator i2 =
        std::find_if(m_responses.begin(),
                     m_responses.end(),
                     std::bind(IsSame(), *response, std::placeholders::_1));

    if (i2 != m_responses.end()) {
        sts = ReleaseResponse(response);
        m_responses.erase(i2);
        return sts;
    }

    // not found anywhere, report an error
    return MFX_ERR_INVALID_HANDLE;
}

mfxStatus BaseFrameAllocator::Close() {
    std::lock_guard<std::mutex> lock(mtx);

    std::list<UniqueResponse>::iterator i;
    for (i = m_ExtResponses.begin(); i != m_ExtResponses.end(); i++) {
        ReleaseResponse(&*i);
    }
    m_ExtResponses.clear();

    std::list<mfxFrameAllocResponse>::iterator i2;
    for (i2 = m_responses.begin(); i2 != m_responses.end(); i2++) {
        ReleaseResponse(&*i2);
    }

    return MFX_ERR_NONE;
}

MFXBufferAllocator::MFXBufferAllocator() {
    pthis  = this;
    Alloc  = Alloc_;
    Lock   = Lock_;
    Free   = Free_;
    Unlock = Unlock_;
}

MFXBufferAllocator::~MFXBufferAllocator() {}

mfxStatus MFXBufferAllocator::Alloc_(mfxHDL pthis, mfxU32 nbytes, mfxU16 type, mfxMemId* mid) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXBufferAllocator& self = *(MFXBufferAllocator*)pthis;

    return self.AllocBuffer(nbytes, type, mid);
}

mfxStatus MFXBufferAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxU8** ptr) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXBufferAllocator& self = *(MFXBufferAllocator*)pthis;

    return self.LockBuffer(mid, ptr);
}

mfxStatus MFXBufferAllocator::Unlock_(mfxHDL pthis, mfxMemId mid) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXBufferAllocator& self = *(MFXBufferAllocator*)pthis;

    return self.UnlockBuffer(mid);
}

mfxStatus MFXBufferAllocator::Free_(mfxHDL pthis, mfxMemId mid) {
    if (0 == pthis)
        return MFX_ERR_MEMORY_ALLOC;

    MFXBufferAllocator& self = *(MFXBufferAllocator*)pthis;

    return self.FreeBuffer(mid);
}
