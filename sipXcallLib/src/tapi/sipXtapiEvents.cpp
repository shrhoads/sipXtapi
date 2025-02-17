//
// Copyright (C) 2005-2021 SIPez LLC.  All rights reserved.
//
// Copyright (C) 2004-2006 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// $$
///////////////////////////////////////////////////////////////////////////////


// SYSTEM INCLUDES
#if !defined(_WIN32)
#  include <stddef.h>
#endif

#include <assert.h>
// APPLICATION INCLUDES
#include "tapi/sipXtapiEvents.h"
#include "tapi/sipXtapiInternal.h"
#include "tapi/SipXHandleMap.h"
#include "tapi/SipXEventDispatcher.h"
#include "utl/UtlSList.h"
#include "utl/UtlSListIterator.h"
#include "utl/UtlVoidPtr.h"
#include "os/OsMutex.h"
#include "utl/UtlString.h"
#include "os/OsLock.h"
#include "net/Url.h"
#include "utl/UtlHashMap.h"
#include "utl/UtlString.h"
#include "net/SipSession.h"
#include "cp/CallManager.h"

// DEFINES
#ifdef WIN32
#define SNPRINTF _snprintf
#else
#define SNPRINTF snprintf
#endif

// #define DEBUG_SIPXTAPI_EVENTS

// GLOBAL VARIABLES
UtlBoolean  g_bListenersEnabled = true ;
UtlSList*	g_pEventListeners = new UtlSList();
OsMutex*    g_pEventListenerLock = new OsMutex(OsMutex::Q_FIFO) ;

// EXTERNAL VARIABLES
extern SipXHandleMap* gpCallHandleMap ;   // sipXtapiInternal.cpp

// EXTERNAL FUNCTIONS
// STRUCTURES

// FUNCTION DECLARATIONS
static const char* convertLinestateEventToString(SIPX_LINESTATE_EVENT event);
static const char* convertLinestateCauseToString(SIPX_LINESTATE_CAUSE cause);

/* ============================ FUNCTIONS ================================= */

static const char* convertEventCategoryToString(SIPX_EVENT_CATEGORY category)
{
    const char* str = "Unknown" ;

    switch (category)
    {
        case EVENT_CATEGORY_CALLSTATE:
            str = "EVENT_CATEGORY_CALLSTATE" ;
            break ;
        case EVENT_CATEGORY_LINESTATE:
            str = "EVENT_CATEGORY_LINESTATE" ;
            break ;
        case EVENT_CATEGORY_INFO_STATUS:
            str = "EVENT_CATEGORY_INFO_STATUS" ;
            break ;
        case EVENT_CATEGORY_INFO:
            str = "EVENT_CATEGORY_INFO" ;
            break ;
        case EVENT_CATEGORY_SUB_STATUS:
            str = "EVENT_CATEGORY_SUB_STATUS" ;
            break ;
        case EVENT_CATEGORY_NOTIFY:
            str = "EVENT_CATEGORY_NOTIFY" ;
            break ;
        case EVENT_CATEGORY_CONFIG:
            str = "EVENT_CATEGORY_CONFIG" ;
            break ;
        case EVENT_CATEGORY_SECURITY:
            str = "EVENT_CATEGORY_SECURITY";
            break;
        case EVENT_CATEGORY_MEDIA:
            str = "EVENT_CATEGORY_MEDIA" ;
            break ;
        case EVENT_CATEGORY_KEEPALIVE:
            str = "EVENT_CATEGORY_KEEPALIVE" ;
            break ;
    }

    return str ;
}

static const char* convertCallstateEventToString(SIPX_CALLSTATE_EVENT eMajor)
{
    const char* str = "Unknown" ;
    switch (eMajor)
    {
        case CALLSTATE_UNKNOWN:
            str = "UNKNOWN" ;
            break ;
        case CALLSTATE_NEWCALL:
            str = "NEWCALL" ;
            break ;
        case CALLSTATE_DIALTONE:
            str = "DIALTONE" ;
            break ;
        case CALLSTATE_REMOTE_OFFERING:
            str = "REMOTE_OFFERING" ;
            break ;
        case CALLSTATE_REMOTE_ALERTING:
            str = "REMOTE_ALERTING" ;
            break ;
        case CALLSTATE_CONNECTED:
            str = "CONNECTED" ;
            break ;
        case CALLSTATE_BRIDGED:
            str = "BRIDGED" ;
            break ;
        case CALLSTATE_HELD:
            str = "HELD" ;
            break ;
        case CALLSTATE_REMOTE_HELD:
            str = "REMOTE_HELD" ;
            break ;
        case CALLSTATE_DISCONNECTED:
            str = "DISCONNECTED" ;
            break ;
        case CALLSTATE_OFFERING:
            str = "OFFERING" ;
            break ;
        case CALLSTATE_ALERTING:
            str = "ALERTING" ;
            break ;
        case CALLSTATE_DESTROYED:
            str = "DESTROYED" ;
            break;
        case CALLSTATE_TRANSFER_EVENT:
            str = "TRANSFER_EVENT" ;
            break ;
    }
    return str;
}

static const char* convertCallstateCauseToString(SIPX_CALLSTATE_CAUSE eMinor)
{
    const char* str = "Unknown" ;
    switch (eMinor)
    {
        case CALLSTATE_CAUSE_UNKNOWN:
            str = "CAUSE_UNKNOWN" ;
            break ;
        case CALLSTATE_CAUSE_NORMAL:
            str = "CAUSE_NORMAL" ;
            break ;
        case CALLSTATE_CAUSE_TRANSFERRED:
            str = "CAUSE_TRANSFERRED" ;
            break ;
        case CALLSTATE_CAUSE_TRANSFER:
            str = "CAUSE_TRANSFER" ;
            break ;
        case CALLSTATE_CAUSE_CONFERENCE:
            str = "CAUSE_CONFERENCE" ;
            break ;
        case CALLSTATE_CAUSE_EARLY_MEDIA:
            str = "CAUSE_EARLY_MEDIA" ;
            break ;
        case CALLSTATE_CAUSE_REQUEST_NOT_ACCEPTED:
            str = "REQUEST_NOT_ACCEPTED" ;
            break ;
        case CALLSTATE_CAUSE_BAD_ADDRESS:
            str = "CAUSE_BADADDRESS" ;
            break ;
        case CALLSTATE_CAUSE_BUSY:
            str = "CAUSE_BUSY" ;
            break ;
        case CALLSTATE_CAUSE_RESOURCE_LIMIT:
            str = "CAUSE_RESOURCE_LIMIT" ;
            break ;
        case CALLSTATE_CAUSE_NETWORK:
            str = "CAUSE_NETWORK" ;
            break ;
        case CALLSTATE_CAUSE_REDIRECTED:
            str = "CAUSE_REDIRECTED" ;
            break ;
        case CALLSTATE_CAUSE_NO_RESPONSE:
            str = "CAUSE_NO_RESPONSE" ;
            break ;
        case CALLSTATE_CAUSE_AUTH:
            str = "CAUSE_AUTH" ;
            break ;
        case CALLSTATE_CAUSE_TRANSFER_INITIATED:
            str = "CAUSE_TRANSFER_INITIATED";
            break;
        case CALLSTATE_CAUSE_TRANSFER_ACCEPTED:
            str = "CAUSE_TRANSFER_ACCEPTED";
            break;
        case CALLSTATE_CAUSE_TRANSFER_TRYING:
            str = "CAUSE_TRANSFER_TRYING";
            break;
        case CALLSTATE_CAUSE_TRANSFER_RINGING:
            str = "CAUSE_TRANSFER_RINGING";
            break;
        case CALLSTATE_CAUSE_TRANSFER_SUCCESS:
            str = "CAUSE_TRANSFER_SUCCESS";
            break;
        case CALLSTATE_CAUSE_TRANSFER_FAILURE:
            str = "CAUSE_TRANSFER_FAILURE";
            break;
        case CALLSTATE_CAUSE_REMOTE_SMIME_UNSUPPORTED:
            str = "CAUSE_REMOTE_SMIME_UNSUPPORTED";
            break ;
        case CALLSTATE_CAUSE_SMIME_FAILURE:
            str = "CAUSE_SMIME_FAILURE";
            break ;
        case CALLSTATE_CAUSE_SHUTDOWN:
            str = "CAUSE_SHUTDOWN";
            break;
        case CALLSTATE_CAUSE_BAD_REFER:
            str = "CALLSTATE_CAUSE_BAD_REFER";
            break;
        case CALLSTATE_CAUSE_NO_KNOWN_INVITE:
            str = "CALLSTATE_CAUSE_NO_KNOWN_INVITE";
            break;
        case CALLSTATE_CAUSE_BYE_DURING_IDLE:
            str = "CALLSTATE_CAUSE_BYE_DURING_IDLE";
            break;
        case CALLSTATE_CAUSE_UNKNOWN_STATUS_CODE:
            str = "CALLSTATE_CAUSE_UNKNOWN_STATUS_CODE";
            break;
        case CALLSTATE_CAUSE_BAD_REDIRECT:
            str = "CALLSTATE_CAUSE_BAD_REDIRECT";
            break;
        case CALLSTATE_CAUSE_TRANSACTION_DOES_NOT_EXIST:
            str = "CALLSTATE_CAUSE_TRANSACTION_DOES_NOT_EXIST";
            break;
        case CALLSTATE_CAUSE_CANCEL:
            str = "CAUSE_CANCEL";
            break;
        case CALLSTATE_CAUSE_NO_CODECS:
            str = "CAUSE_NO_CODECS";
            break;
        case CALLSTATE_CAUSE_SERVER_ERROR:
            str = "CALLSTATE_CAUSE_SERVER_ERROR";
            break;
        default:
            assert(FALSE);
            break ;
    }
    return str;
}

static const char* convertMediaEventToString(SIPX_MEDIA_EVENT event)
{
    const char* str = "Unknown" ;
    switch (event)
    {
        case MEDIA_LOCAL_START:
            str = "LOCAL_START" ;
            break ;
        case MEDIA_LOCAL_STOP:
            str = "LOCAL_STOP" ;
            break ;
        case MEDIA_REMOTE_START:
            str = "REMOTE_START" ;
            break ;
        case MEDIA_REMOTE_STOP:
            str = "REMOTE_STOP" ;
            break ;
        case MEDIA_REMOTE_SILENT:
            str = "REMOTE_SILENT" ;
            break ;
        case MEDIA_PLAYFILE_START:
            str = "PLAYFILE_START" ;
            break ;
        case MEDIA_PLAYFILE_FINISH:
            str = "PLAYFILE_FINISH" ;
            break ;
        case MEDIA_PLAYFILE_STOP:
           str = "PLAYFILE_STOP" ;
           break ;
        case MEDIA_PLAYBUFFER_START:
            str = "PLAYBUFFER_START" ;
            break ;
        case MEDIA_PLAYBUFFER_FINISH:
           str = "PLAYBUFFER_FINISH" ;
           break ;
        case MEDIA_PLAYBUFFER_STOP:
           str = "PLAYBUFFER_STOP" ;
           break ;
        case MEDIA_RECORDFILE_START:
            str = "RECORDFILE_START" ;
            break ;
        case MEDIA_RECORD_PAUSE:
            str = "RECORD_PAUSE" ;
            break ;
        case MEDIA_RECORD_RESUME:
            str = "RECORD_RESUME" ;
            break ;
        case MEDIA_RECORDFILE_STOP:
            str = "RECORDFILE_STOP" ;
            break ;
        case MEDIA_RECORDBUFFER_START:
            str = "RECORDBUFFER_START" ;
            break ;
        case MEDIA_RECORDBUFFER_STOP:
            str = "RECORDBUFFER_STOP" ;
            break ;
        case MEDIA_REMOTE_DTMF:
            str = "REMOTE_DTMF" ;
            break ;
        case MEDIA_DEVICE_FAILURE:
            str = "MEDIA_DEVICE_FAILURE";
            break ;
        case MEDIA_REMOTE_ACTIVE:
            str = "MEDIA_REMOTE_ACTIVE" ;
            break ;
        case MEDIA_MIC_ENERGY_LEVEL:
            str = "MEDIA_MIC_ENERGY_LEVEL";
            break;
        case MEDIA_SPEAKER_ENERGY_LEVEL:
            str = "MEDIA_SPEAKER_ENERGY_LEVEL";
            break;
        case MEDIA_RTP_ENERGY_LEVEL:
            str = "MEDIA_RTP_ENERGY_LEVEL";
            break;
        case MEDIA_H264_SPS:
            str = "MEDIA_H264_SPS";
            break;
        case MEDIA_H264_PPS:
            str = "MEDIA_H264_PPS";
            break;
        case MEDIA_INPUT_DEVICE_NOT_PRESENT:
            str = "MEDIA_INPUT_DEVICE_NOT_PRESENT";
            break;
        case MEDIA_OUTPUT_DEVICE_NOT_PRESENT:
            str = "MEDIA_OUTPUT_DEVICE_NOT_PRESENT";
            break;
        case MEDIA_INPUT_DEVICE_NOW_PRESENT:
            str = "MEDIA_INPUT_DEVICE_NOW_PRESENT";
            break;
        case MEDIA_OUTPUT_DEVICE_NOW_PRESENT:
            str = "MEDIA_OUTPUT_DEVICE_NOW_PRESENT";
            break;
        default:
            break ;
    }
    return str;
}


static const char* convertMediaTypeToString(SIPX_MEDIA_TYPE type)
{
    const char* str = "Unknown" ;
    switch (type)
    {
        case MEDIA_TYPE_AUDIO:
            str = "AUDIO" ;
            break ;
        case MEDIA_TYPE_VIDEO:
            str = "VIDEO" ;
            break ;
        default:
            break ;
    }
    return str;
}

static const char* convertMediaCauseToString(SIPX_MEDIA_CAUSE cause)
{
    const char* str = "Unknown" ;
    switch (cause)
    {
        case MEDIA_CAUSE_NORMAL:
            str = "CAUSE_NORMAL" ;
            break ;
        case MEDIA_CAUSE_HOLD:
            str = "CAUSE_HOLD" ;
            break ;
        case MEDIA_CAUSE_UNHOLD:
            str = "CAUSE_UNHOLD" ;
            break ;
        case MEDIA_CAUSE_FAILED:
            str = "CAUSE_FAILED" ;
            break ;
        case MEDIA_CAUSE_DEVICE_UNAVAILABLE:
            str = "CAUSE_DEVICE_UNAVAILABLE";
            break;
        case MEDIA_CAUSE_INCOMPATIBLE:
            str = "MEDIA_CAUSE_INCOMPATIBLE";
            break;
        case MEDIA_CAUSE_DTMF_START:
            str = "CAUSE_DTMF_START";
            break;
	    case MEDIA_CAUSE_DTMF_STOP:
            str = "CAUSE_DTMF_STOP";
            break;
        case MEDIA_CAUSE_DEVICE_NOW_AVAILABLE:
            str = "CAUSE_DEVICE_NOW_AVAILABLE";
            break;
        default:
            break ;
    }
    return str;
}

static const char* convertKeepaliveEventToString(SIPX_KEEPALIVE_EVENT event)
{
    const char* str = "Unknown" ;
    switch (event)
    {
        case KEEPALIVE_START:
            str = "KEEPALIVE_START" ;
            break ;
        case KEEPALIVE_FEEDBACK:
            str = "KEEPALIVE_FEEDBACK" ;
            break ;
        case KEEPALIVE_FAILURE:
            str = "KEEPALIVE_FAILURE" ;
            break ;
        case KEEPALIVE_STOP:
            str = "KEEPALIVE_STOP" ;
            break ;
        default:
            break ;
    }
    return str;
}

static const char* convertKeepaliveCauseToString(SIPX_KEEPALIVE_CAUSE event)
{
    const char* str = "Unknown" ;
    switch (event)
    {
        case KEEPALIVE_CAUSE_NORMAL:
            str = "KEEPALIVE_CAUSE_NORMAL" ;
            break ;
    }
    return str;
}

static const char* convertKeepaliveTypeToString(SIPX_KEEPALIVE_TYPE type)
{
    const char* str = "Unknown" ;
    switch (type)
    {
        case SIPX_KEEPALIVE_CRLF:
            str = "SIPX_KEEPALIVE_CRLF" ;
            break ;
        case SIPX_KEEPALIVE_STUN:
            str = "SIPX_KEEPALIVE_STUN" ;
            break ;
        case SIPX_KEEPALIVE_SIP_PING:
            str = "SIPX_KEEPALIVE_SIP_PING" ;
            break ;
        case SIPX_KEEPALIVE_SIP_OPTIONS:
            str = "SIPX_KEEPALIVE_SIP_OPTIONS" ;
            break ;
        default:
            break ;
    }
    return str ;
}


static const char* convertInfoStatusEventToString(SIPX_INFOSTATUS_EVENT event)
{
    const char* str = "Unknown" ;

    switch (event)
    {
        case INFOSTATUS_UNKNOWN:
            str = "INFOSTATUS_UNKNOWN" ;
            break ;
        case INFOSTATUS_RESPONSE:
            str = "INFOSTATUS_RESPONSE" ;
            break ;
        case INFOSTATUS_NETWORK_ERROR:
            str = "INFOSTATUS_NETWORK_ERROR" ;
            break;
        default:
            break ;
    }

    return str ;
}

static const char* convertMessageStatusToString(SIPX_MESSAGE_STATUS status)
{
    const char* str = "Unknown" ;

    switch (status)
    {
        case SIPX_MESSAGE_OK:
            str = "SIPX_MESSAGE_OK" ;
            break ;
        case SIPX_MESSAGE_FAILURE:
            str = "SIPX_MESSAGE_FAILURE" ;
            break ;
        case SIPX_MESSAGE_SERVER_FAILURE:
            str = "SIPX_MESSAGE_SERVER_FAILURE" ;
            break ;
        case SIPX_MESSAGE_GLOBAL_FAILURE:
            str = "SIPX_MESSAGE_GLOBAL_FAILURE" ;
            break ;
        default:
            break ;
    }

    return str ;
}


static const char* convertConfigEventToString(SIPX_CONFIG_EVENT event)
{
    const char* str = "Unknown" ;

    switch (event)
    {
        case CONFIG_UNKNOWN:
            str = "CONFIG_UNKNOWN" ;
            break ;
        case CONFIG_STUN_SUCCESS:
            str = "CONFIG_STUN_SUCCESS" ;
            break ;
        case CONFIG_STUN_FAILURE:
            str = "CONFIG_STUN_FAILURE" ;
            break ;
        default:
            break ;
    }
 
    return str ;
}

const char* convertSubscriptionStateToString(SIPX_SUBSCRIPTION_STATE state)
{
    const char* str = "Unknown" ;

    switch (state)
    {
        case SIPX_SUBSCRIPTION_PENDING:
            str = "SIPX_SUBSCRIPTION_PENDING" ;
            break ;
        case SIPX_SUBSCRIPTION_ACTIVE:
            str = "SIPX_SUBSCRIPTION_ACTIVE" ;
            break ;
        case SIPX_SUBSCRIPTION_FAILED:
            str = "SIPX_SUBSCRIPTION_FAILED" ;
            break ;
        case SIPX_SUBSCRIPTION_EXPIRED:
            str = "SIPX_SUBSCRIPTION_EXPIRED" ;
            break ;
        default:
            break ;
    }

    return str ;
}

const char* convertSubscriptionCauseToString(SIPX_SUBSCRIPTION_CAUSE cause)
{
    const char* str = "Unknown" ;

    switch (cause)
    {
        case SUBSCRIPTION_CAUSE_UNKNOWN:
            str = "SUBSCRIPTION_CAUSE_UNKNOWN" ;
            break ;
        case SUBSCRIPTION_CAUSE_NORMAL:
            str = "SUBSCRIPTION_CAUSE_NORMAL" ;
            break ;
        default:
            break ;
    }

    return str ;
}

#define SAFE_STRDUP(X) (((X) == NULL) ? NULL : strdup((X)))

SIPXTAPI_API SIPX_RESULT sipxDuplicateEvent(SIPX_EVENT_CATEGORY category, 
                                            const void*         pEventSource, 
                                            void**              pEventCopy) 
{
    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS ;

    assert(VALID_SIPX_EVENT_CATEGORY(category)) ;
    assert(pEventSource != NULL) ;
    assert(pEventCopy != NULL) ;

    if (VALID_SIPX_EVENT_CATEGORY(category) && pEventSource && pEventCopy)
    {
        switch (category)
        {
            case EVENT_CATEGORY_CALLSTATE:
                {
                    SIPX_CALLSTATE_INFO* pSourceInfo = (SIPX_CALLSTATE_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_CALLSTATE_INFO)) ;

                    SIPX_CALLSTATE_INFO* pInfo = new SIPX_CALLSTATE_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_CALLSTATE_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hCall = pSourceInfo->hCall ;
                    pInfo->hLine = pSourceInfo->hLine ;
                    pInfo->event = pSourceInfo->event ;
                    pInfo->cause = pSourceInfo->cause ;
                    pInfo->hAssociatedCall = pSourceInfo->hAssociatedCall ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_LINESTATE:
                {
                    SIPX_LINESTATE_INFO* pSourceInfo = (SIPX_LINESTATE_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_LINESTATE_INFO)) ;

                    SIPX_LINESTATE_INFO* pInfo = new SIPX_LINESTATE_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_LINESTATE_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hLine = pSourceInfo->hLine ;
                    pInfo->event = pSourceInfo->event ;
                    pInfo->cause = pSourceInfo->cause ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_INFO_STATUS:
                {
                    SIPX_INFOSTATUS_INFO* pSourceInfo = (SIPX_INFOSTATUS_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_INFOSTATUS_INFO)) ;

                    SIPX_INFOSTATUS_INFO* pInfo = new SIPX_INFOSTATUS_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_INFOSTATUS_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hInfo = pSourceInfo->hInfo ;
                    pInfo->status = pSourceInfo->status ;
                    pInfo->responseCode = pSourceInfo->responseCode ;
                    pInfo->szResponseText = SAFE_STRDUP(pSourceInfo->szResponseText) ;
                    pInfo->event = pSourceInfo->event ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_INFO:
                {
                    SIPX_INFO_INFO* pSourceInfo = (SIPX_INFO_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_INFO_INFO)) ;

                    SIPX_INFO_INFO* pInfo = new SIPX_INFO_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_INFO_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hCall = pSourceInfo->hCall ;
                    pInfo->hLine = pSourceInfo->hLine ;
                    pInfo->szFromURL = SAFE_STRDUP(pSourceInfo->szFromURL) ;
                    pInfo->szUserAgent = SAFE_STRDUP(pSourceInfo->szUserAgent) ;
                    pInfo->szContentType = SAFE_STRDUP(pSourceInfo->szContentType) ;
                    if (pSourceInfo->nContentLength && pSourceInfo->pContent)
                    {
                        pInfo->pContent = (char*) malloc(pSourceInfo->nContentLength) ;
                        memcpy((void*) pInfo->pContent, pSourceInfo->pContent, pSourceInfo->nContentLength) ;
                        pInfo->nContentLength = pSourceInfo->nContentLength ;
                    }
                    else
                    {
                        pInfo->pContent = NULL ;
                        pInfo->nContentLength = 0 ;
                    }                    

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_SUB_STATUS:
                {
                    SIPX_SUBSTATUS_INFO* pSourceInfo = (SIPX_SUBSTATUS_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_SUBSTATUS_INFO)) ;

                    SIPX_SUBSTATUS_INFO* pInfo = new SIPX_SUBSTATUS_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_SUBSTATUS_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hSub = pSourceInfo->hSub ;
                    pInfo->state = pSourceInfo->state ;
                    pInfo->cause = pSourceInfo->cause ;
                    pInfo->szSubServerUserAgent = SAFE_STRDUP(pSourceInfo->szSubServerUserAgent) ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_NOTIFY:
                {
                    SIPX_NOTIFY_INFO* pSourceInfo = (SIPX_NOTIFY_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_NOTIFY_INFO)) ;

                    SIPX_NOTIFY_INFO* pInfo = new SIPX_NOTIFY_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_NOTIFY_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->hSub = pSourceInfo->hSub ;
                    pInfo->szNotiferUserAgent = SAFE_STRDUP(pSourceInfo->szNotiferUserAgent) ;
                    pInfo->szContentType = SAFE_STRDUP(pSourceInfo->szContentType) ;
                    if (pSourceInfo->nContentLength && pSourceInfo->pContent)
                    {
                        pInfo->pContent = malloc(pSourceInfo->nContentLength) ;
                        memcpy((void*) pInfo->pContent, pSourceInfo->pContent, pSourceInfo->nContentLength) ;
                        pInfo->nContentLength = pSourceInfo->nContentLength ;
                    }
                    else
                    {
                        pInfo->pContent = NULL ;
                        pInfo->nContentLength = 0 ;
                    }

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_CONFIG:
                {
                    SIPX_CONFIG_INFO* pSourceInfo = (SIPX_CONFIG_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_CONFIG_INFO)) ;

                    SIPX_CONFIG_INFO* pInfo = new SIPX_CONFIG_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_CONFIG_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->event = pSourceInfo->event ;
                    
                    if (pSourceInfo->pData)
                    {
                        pInfo->pData = new SIPX_CONTACT_ADDRESS(*((SIPX_CONTACT_ADDRESS*) pSourceInfo->pData)) ;
                    }
                    else
                    {
                        pInfo->pData = NULL ;
                    }

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                rc = SIPX_RESULT_SUCCESS ;
                break ;
            case EVENT_CATEGORY_SECURITY:
                {
                    SIPX_SECURITY_INFO* pSourceInfo = (SIPX_SECURITY_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_SECURITY_INFO)) ;

                    SIPX_SECURITY_INFO* pInfo = new SIPX_SECURITY_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_SECURITY_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->szSRTPkey = SAFE_STRDUP(pSourceInfo->szSRTPkey) ;
                    if (pSourceInfo->nCertificateSize && pSourceInfo->pCertificate)
                    {
                        pInfo->pCertificate = malloc(pSourceInfo->nCertificateSize) ;
                        memcpy(pInfo->pCertificate, pSourceInfo->pCertificate, pSourceInfo->nCertificateSize) ;
                        pInfo->nCertificateSize = pSourceInfo->nCertificateSize ;
                    }
                    else
                    {
                        pInfo->pCertificate = NULL ;
                        pInfo->nCertificateSize = 0 ;
                    }
                    pInfo->event = pSourceInfo->event ;
                    pInfo->cause = pSourceInfo->cause ;
                    pInfo->szSubjAltName = SAFE_STRDUP(pSourceInfo->szSubjAltName) ;
                    pInfo->callId = SAFE_STRDUP(pSourceInfo->callId) ;
                    pInfo->hCall = pSourceInfo->hCall ;
                    pInfo->remoteAddress = SAFE_STRDUP(pSourceInfo->remoteAddress) ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_MEDIA:
                {
                    SIPX_MEDIA_INFO* pSourceInfo = (SIPX_MEDIA_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_MEDIA_INFO)) ;

                    SIPX_MEDIA_INFO* pInfo = new SIPX_MEDIA_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_MEDIA_INFO)) ;                    
                    pInfo->nSize = pSourceInfo->nSize ;
                    pInfo->event = pSourceInfo->event ;
                    pInfo->cause = pSourceInfo->cause ;
                    pInfo->mediaType = pSourceInfo->mediaType ;
                    pInfo->hCall = pSourceInfo->hCall ;
                    pInfo->nSize = pSourceInfo->nSize ;                    
                    memcpy(&pInfo->codec, &pSourceInfo->codec, sizeof(SIPX_CODEC_INFO)) ;
                    pInfo->idleTime = pSourceInfo->idleTime ;
                    pInfo->toneId = pSourceInfo->toneId;
                    pInfo->deviceName = SAFE_STRDUP(pSourceInfo->deviceName);
                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_KEEPALIVE:
                {
                    SIPX_KEEPALIVE_INFO* pSourceInfo = (SIPX_KEEPALIVE_INFO*) pEventSource ;
                    assert(pSourceInfo->nSize == sizeof(SIPX_KEEPALIVE_INFO)) ;

                    SIPX_KEEPALIVE_INFO* pInfo = new SIPX_KEEPALIVE_INFO ;
                    memset(pInfo, 0, sizeof(SIPX_KEEPALIVE_INFO)) ;                                                           
                    pInfo->nSize = pSourceInfo->nSize ;                    
                    pInfo->event = pSourceInfo->event ;
                    pInfo->cause = pSourceInfo->cause ;
                    pInfo->type = pSourceInfo->type ;
                    pInfo->szRemoteAddress = SAFE_STRDUP(pSourceInfo->szRemoteAddress) ;
                    pInfo->remotePort = pSourceInfo->remotePort ;
                    pInfo->keepAliveSecs = pSourceInfo->keepAliveSecs ;
                    pInfo->szFeedbackAddress = SAFE_STRDUP(pSourceInfo->szFeedbackAddress) ;
                    pInfo->feedbackPort = pSourceInfo->feedbackPort ;

                    *pEventCopy = pInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            default:
                *pEventCopy = NULL ;
                break;
        }
    }

    return rc ;
}


SIPXTAPI_API SIPX_RESULT sipxFreeDuplicatedEvent(SIPX_EVENT_CATEGORY category, 
                                                 void*               pEventCopy) 
{
    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS ;

    assert(VALID_SIPX_EVENT_CATEGORY(category)) ;
    assert(pEventCopy != NULL) ;

    if (VALID_SIPX_EVENT_CATEGORY(category) && pEventCopy)
    {
        switch (category)
        {
            case EVENT_CATEGORY_CALLSTATE:
                {
                    SIPX_CALLSTATE_INFO* pSourceInfo = (SIPX_CALLSTATE_INFO*) pEventCopy ;
                    delete pSourceInfo ;
                    
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_LINESTATE:
                {
                    SIPX_LINESTATE_INFO* pSourceInfo = (SIPX_LINESTATE_INFO*) pEventCopy ;
                    delete pSourceInfo ;
                    
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_INFO_STATUS:
                {
                    SIPX_INFOSTATUS_INFO* pSourceInfo = (SIPX_INFOSTATUS_INFO*) pEventCopy ;
                    if (pSourceInfo->szResponseText)
                    {
                        free((void*) pSourceInfo->szResponseText) ;
                    }                    
                    delete pSourceInfo ;


                    rc = SIPX_RESULT_SUCCESS ;
                }
                break;
            case EVENT_CATEGORY_INFO:
                {
                    SIPX_INFO_INFO* pSourceInfo = (SIPX_INFO_INFO*) pEventCopy ;
                    if (pSourceInfo->szFromURL)
                    {
                        free((void*) pSourceInfo->szFromURL) ;
                    }
                    if (pSourceInfo->szUserAgent)
                    {
                        free((void*) pSourceInfo->szUserAgent) ;
                    }
                    if (pSourceInfo->szContentType)
                    {
                        free((void*) pSourceInfo->szContentType) ;
                    }
                    if (pSourceInfo->pContent)
                    {
                        free((void*) pSourceInfo->pContent) ;
                    }                    
                    delete pSourceInfo ;
                   
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_SUB_STATUS:
                {
                    SIPX_SUBSTATUS_INFO* pSourceInfo = (SIPX_SUBSTATUS_INFO*) pEventCopy ;
                    if (pSourceInfo->szSubServerUserAgent)
                    {
                        free((void*) pSourceInfo->szSubServerUserAgent) ;
                    }
                    delete pSourceInfo ;
                    
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_NOTIFY:
                {
                    SIPX_NOTIFY_INFO* pSourceInfo = (SIPX_NOTIFY_INFO*) pEventCopy ;
                    if (pSourceInfo->szNotiferUserAgent)
                    {
                        free((void*) pSourceInfo->szNotiferUserAgent) ;
                    }                    
                    if (pSourceInfo->szContentType)
                    {
                        free((void*) pSourceInfo->szContentType) ;
                    }
                    if (pSourceInfo->pContent)
                    {
                        free((void*) pSourceInfo->pContent) ;
                    }                    
                    delete pSourceInfo ;

                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_CONFIG:
                {
                    SIPX_CONFIG_INFO* pSourceInfo = (SIPX_CONFIG_INFO*) pEventCopy ;                    
                    if (pSourceInfo->pData)
                    {
                        delete ((SIPX_CONTACT_ADDRESS*) pSourceInfo->pData) ;
                    }                    
                    delete pSourceInfo ;
                    rc = SIPX_RESULT_SUCCESS ;
                }
                rc = SIPX_RESULT_SUCCESS ;
                break ;
            case EVENT_CATEGORY_SECURITY:
                {
                    SIPX_SECURITY_INFO* pSourceInfo = (SIPX_SECURITY_INFO*) pEventCopy ;
                    if (pSourceInfo->szSRTPkey)
                    {
                        free((void*) pSourceInfo->szSRTPkey) ;
                    }
                    if (pSourceInfo->pCertificate)
                    {
                        free((void*) pSourceInfo->pCertificate) ;
                    }
                    if (pSourceInfo->szSubjAltName)
                    {
                        free((void*) pSourceInfo->szSubjAltName) ;
                    }
                    if (pSourceInfo->callId)
                    {
                        free((void*) pSourceInfo->callId) ;
                    }
                    if (pSourceInfo->remoteAddress)
                    {
                        free((void*) pSourceInfo->remoteAddress) ;
                    }                   
                    delete pSourceInfo ;
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            case EVENT_CATEGORY_MEDIA:
                {
                    SIPX_MEDIA_INFO* pSourceInfo = (SIPX_MEDIA_INFO*)pEventCopy;
                    if (pSourceInfo->deviceName)
                    {
                        free((void*)pSourceInfo->deviceName);
                    }
                    delete pSourceInfo;
                    rc = SIPX_RESULT_SUCCESS;
                }
                break ;
            case EVENT_CATEGORY_KEEPALIVE:
                {
                    SIPX_KEEPALIVE_INFO* pSourceInfo = (SIPX_KEEPALIVE_INFO*) pEventCopy ;
                    if (pSourceInfo->szRemoteAddress)
                    {
                        free((void*) pSourceInfo->szRemoteAddress) ;
                    }
                    if (pSourceInfo->szFeedbackAddress)
                    {
                        free((void*) pSourceInfo->szFeedbackAddress) ;                    
                    }
                    delete pSourceInfo ;
                    rc = SIPX_RESULT_SUCCESS ;
                }
                break ;
            default:
                break;
        }
    }

    return rc ;
}


SIPXTAPI_API char* sipxCallEventToString(SIPX_CALLSTATE_EVENT event,
                                         SIPX_CALLSTATE_CAUSE cause,
                                         char*  szBuffer,
                                         size_t nBuffer)
{
    assert(szBuffer != NULL) ;

    if (szBuffer)
    {
        SNPRINTF(szBuffer, nBuffer, "%s::%s",  convertCallstateEventToString(event), convertCallstateCauseToString(cause)) ;
    }

    return szBuffer ;
}

SIPXTAPI_API char* sipxLineEventToString(SIPX_LINESTATE_EVENT event,
                                         SIPX_LINESTATE_CAUSE cause,
                                         char*  szBuffer,
                                         size_t nBuffer)
{
#ifdef WIN32
   _snprintf(szBuffer, nBuffer, "%s::%s", convertLinestateEventToString(event), convertLinestateCauseToString(cause));
#else
   snprintf(szBuffer, nBuffer, "%s::%s", convertLinestateEventToString(event), convertLinestateCauseToString(cause));
#endif

    return szBuffer;
}



SIPXTAPI_API char* sipxEventToString(const SIPX_EVENT_CATEGORY category,
                                     const void* pEvent,
                                     char*  szBuffer,
                                     size_t nBuffer)
{
    switch (category)
    {
        case EVENT_CATEGORY_CALLSTATE:
            {
                SIPX_CALLSTATE_INFO* pCallEvent = (SIPX_CALLSTATE_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertCallstateEventToString(pCallEvent->event), 
                        convertCallstateCauseToString(pCallEvent->cause)) ;
            }
            break;
        case EVENT_CATEGORY_LINESTATE:
            {
                SIPX_LINESTATE_INFO* pLineEvent = (SIPX_LINESTATE_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertLinestateEventToString(pLineEvent->event), 
                        convertLinestateCauseToString(pLineEvent->cause)) ;
            }
            break;
        case EVENT_CATEGORY_INFO_STATUS:
            {
                SIPX_INFOSTATUS_INFO* pInfoEvent = (SIPX_INFOSTATUS_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertInfoStatusEventToString(pInfoEvent->event),
                        convertMessageStatusToString(pInfoEvent->status)) ;

            }
            break;
        case EVENT_CATEGORY_INFO:
            {
                SNPRINTF(szBuffer, nBuffer, "%s", 
                        convertEventCategoryToString(category)) ;
            }
            break ;
        case EVENT_CATEGORY_SUB_STATUS:
            {
                SIPX_SUBSTATUS_INFO* pStatusInfo = (SIPX_SUBSTATUS_INFO*) pEvent ;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertSubscriptionStateToString(pStatusInfo->state),
                        convertSubscriptionCauseToString(pStatusInfo->cause)) ;
            }
            break ;
        case EVENT_CATEGORY_NOTIFY:
            {
                SNPRINTF(szBuffer, nBuffer, "%s", 
                        convertEventCategoryToString(category)) ;
            }
            break ;
        case EVENT_CATEGORY_CONFIG:
            {
                SIPX_CONFIG_INFO* pConfigEvent = (SIPX_CONFIG_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s", 
                        convertEventCategoryToString(category),
                        convertConfigEventToString(pConfigEvent->event)) ;
            }
            break ;
        case EVENT_CATEGORY_SECURITY:
            {
                char cTemp[128] ;
                SIPX_SECURITY_INFO* pEventData = (SIPX_SECURITY_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s", 
                        convertEventCategoryToString(category),
                        sipxSecurityEventToString(pEventData->event, cTemp, sizeof(cTemp)),
                        sipxSecurityCauseToString(pEventData->cause, cTemp, sizeof(cTemp))) ;
            }
            break ;
        case EVENT_CATEGORY_MEDIA:
            {
                SIPX_MEDIA_INFO* pEventData = (SIPX_MEDIA_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertMediaEventToString(pEventData->event),
                        convertMediaTypeToString(pEventData->mediaType),
                        convertMediaCauseToString(pEventData->cause)) ;
            }
            break ;
        case EVENT_CATEGORY_KEEPALIVE:
            {
                SIPX_KEEPALIVE_INFO* pEventData = (SIPX_KEEPALIVE_INFO*)pEvent;
                SNPRINTF(szBuffer, nBuffer, "%s::%s::%s::%s", 
                        convertEventCategoryToString(category),
                        convertKeepaliveEventToString(pEventData->event),
                        convertKeepaliveTypeToString(pEventData->type),
                        convertKeepaliveCauseToString(pEventData->cause)) ;
            }
            break ;

        default:
            break;
    }
    return szBuffer ;
}


void ReportCallback(SIPX_CALL hCall,
                    SIPX_LINE hLine,
                    SIPX_CALLSTATE_EVENT event,
                    SIPX_CALLSTATE_CAUSE cause,
                    void* pUserData)
{
    SIPX_INSTANCE_DATA* pInst ;
    UtlString callId ;
    UtlString remoteAddress ;
    UtlString lineId ;
    static size_t nCnt = 0 ;

    if (sipxCallGetCommonData(hCall, &pInst, &callId, &remoteAddress, &lineId))
    {
        printf("<event i=%p, h=%04X, c=%4d, M=%25s, m=%25s, a=%s, c=%s l=%s/>\n",
                pInst,
                hCall,
                (int) ++nCnt,
                convertCallstateEventToString(event),
                convertCallstateCauseToString(cause),
                remoteAddress.data(),
                callId.data(),
                lineId.data()) ;
    }
}


void sipxFireCallEvent(const void* pSrc,
                       const char* szCallId,
                       SipSession* pSession,
                       const char* szRemoteAddress,
                       SIPX_CALLSTATE_EVENT event,
                       SIPX_CALLSTATE_CAUSE cause,
                       void* pEventData,
                       const char* szRemoteAssertedIdentity)
{
    OsStackTraceLogger stackLogger(FAC_SIPXTAPI, PRI_DEBUG, "sipxFireCallEvent");
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
            "sipxFireCallEvent Src=%p CallId=%s RemoteAddress=%s Event=%s:%s",
            pSrc, szCallId, szRemoteAddress, convertCallstateEventToString(event), convertCallstateCauseToString(cause)) ;
     
    SIPX_CALL hCall = SIPX_CALL_NULL;

    UtlString sessionCallId;

    SIPX_CALL_DATA* pCallData = NULL;
    SIPX_LINE hLine = SIPX_LINE_NULL ;
    UtlVoidPtr* ptr = NULL;

    SIPX_INSTANCE_DATA* pInst ;
    UtlString remoteAddress ;
    UtlString lineId ;
    UtlString contactAddress ;
    SIPX_CALL hAssociatedCall = SIPX_CALL_NULL ;

    SIPX_CONF _hConf = 0;

    assert(szCallId[0] == 'c');
    if (pSession)
    {
       pSession->getCallId(sessionCallId);
       if (event == CALLSTATE_DIALTONE && cause != CALLSTATE_CAUSE_CONFERENCE)
       {
          assert(sessionCallId.isNull());
       }
       else
       {
          assert(!sessionCallId.isNull());
          // Can only assume 's' prefix for calls initiated from this side.
          // Can potentially get this from the Connetion and conditionally
          // check, but for now don't check
          //assert(sessionCallId[0] == 's');
       }
    }

    // If this is an NEW inbound call (first we are hearing of it), then create
    // a call handle/data structure for it.
    if (event == CALLSTATE_NEWCALL)
    {
        pCallData = new SIPX_CALL_DATA;
        memset((void*) pCallData, 0, sizeof(SIPX_CALL_DATA));
        pCallData->state = SIPX_INTERNAL_CALLSTATE_UNKNOWN;

        pCallData->pInst = findSessionByCallManager(pSrc) ;                    
        pInst = pCallData->pInst ;

        pCallData->callId = new UtlString(szCallId) ;
        pCallData->sessionCallId = new UtlString(sessionCallId) ;
        pCallData->remoteAddress = new UtlString(szRemoteAddress) ;
        pCallData->pMutex = new OsRWMutex(OsRWMutex::Q_FIFO) ;

        Url urlFrom;
        if (pSession)
        {
           pSession->getFromUrl(urlFrom) ;
        }

        pCallData->lineURI = new UtlString(urlFrom.toString()) ;

        hCall = gpCallHandleMap->allocHandle(pCallData) ;
        OsSysLog::add(FAC_SIPXTAPI, PRI_DEBUG,
                      "sipxFireCallEvent new hCall: %d Call-Id: %s",
                      hCall,
                      pCallData->callId->data());

        if (pEventData)
        {
            const char* szOriginalCallId = (const char*) pEventData ;                
            hAssociatedCall = sipxCallLookupHandle(szOriginalCallId, pSrc) ;

            // Make sure we remove the call instead of allowing a drop.  When acting
            // as a transfer target, we are performing surgery on a CpPeerCall.  We
            // want to remove the call leg -- not drop the entire call.
            if ((hAssociatedCall) && (cause == CALLSTATE_CAUSE_TRANSFERRED))
            {
                // get the callstate of the replaced leg
                SIPX_CALL_DATA* pOldCallData = sipxCallLookup(hAssociatedCall, SIPX_LOCK_READ, stackLogger);
                bool bCallHoldInvoked = false;
                if (pOldCallData)
                {
                    bCallHoldInvoked = pOldCallData->bCallHoldInvoked;
                    sipxCallReleaseLock(pOldCallData, SIPX_LOCK_READ, stackLogger);
                }
                
                if (bCallHoldInvoked)
                {
                    SIPX_CALL_DATA* pData = sipxCallLookup(hCall, SIPX_LOCK_WRITE, stackLogger);
                    if (pData)
                    {
                        pData->bHoldAfterConnect = true;
                        sipxCallReleaseLock(pData, SIPX_LOCK_WRITE, stackLogger);
                    }
                }
                sipxCallSetRemoveInsteadofDrop(hAssociatedCall) ;

                SIPX_CONF hConf = sipxCallGetConf(hAssociatedCall) ;
                if (hConf)
                {
                    sipxAddCallHandleToConf(hCall, hConf) ;
                }
            }
            else if ((hAssociatedCall) && (cause == CALLSTATE_CAUSE_TRANSFER))
            {
                // This is the case where we are the transferee -- we want to
                // make sure that the new call is part of the conference
                SIPX_CONF hConf = sipxCallGetConf(hAssociatedCall) ;
                if (hConf)
                {
                    // The original call was part of a transfer -- make sure the
                    // replacement leg is also part of the conference.
                    sipxAddCallHandleToConf(hCall, hConf) ;
                }
            }
        }

        // Increment call count
        pInst->pLock->acquire() ;
        pInst->nCalls++ ;
        pInst->pLock->release() ;

        remoteAddress = szRemoteAddress ;
        lineId = urlFrom.toString() ;
    }
    else
    {
        if (sessionCallId.isNull())
        {
           hCall = sipxCallLookupHandle(szCallId, pSrc);
        }
        else
        {
           hCall = sipxCallLookupHandle(sessionCallId, pSrc);
        }
        if (!sipxCallGetCommonData(hCall, &pInst, NULL, &remoteAddress, &lineId)
           && CALLSTATE_DESTROYED != event)
        {
           osPrintf("event sipXtapiEvents: Unable to find call data for handle: %d\n", hCall) ;
           osPrintf("event %s:%s callId=%s address=%s\n",
                    convertCallstateEventToString(event),
                    convertCallstateCauseToString(cause),
                    szCallId, szRemoteAddress) ;
        }
        pCallData = sipxCallLookup(hCall, SIPX_LOCK_WRITE, stackLogger) ;
        if (pCallData && pSession && !pCallData->contactAddress)
        {
            pSession->getContactRequestUri(contactAddress);
            pCallData->contactAddress = new UtlString(contactAddress);
        }
        if (pCallData)
        {
            _hConf = pCallData->hConf;
            sipxCallReleaseLock(pCallData, SIPX_LOCK_WRITE, stackLogger) ;
        }
    }

//    printf("sipxFireCallEvent(hCall=%d, hConf=%d) event %s::%s\n",
//           hCall, _hConf,
//           convertCallstateEventToString(event), convertCallstateCauseToString(cause));;

//     if (event == CALLSTATE_NEWCALL)
//     {
//        gpCallHandleMap->dumpCalls();
//     }

    // Filter duplicate events
    UtlBoolean bDuplicateEvent = FALSE ;
    SIPX_CALLSTATE_EVENT lastEvent ;
    SIPX_CALLSTATE_CAUSE lastCause ;
    SIPX_INTERNAL_CALLSTATE state = SIPX_INTERNAL_CALLSTATE_UNKNOWN ;
    if (sipxCallGetState(hCall, lastEvent, lastCause, state))
    {           
        // Filter our duplicate events
        if ((lastEvent == event) && (lastCause == cause))
        {
            bDuplicateEvent = TRUE ;
        }          
    }

    // Only proceed if this isn't a duplicate event and we have a valid 
    // call handle.
    if (!bDuplicateEvent && hCall != 0)
    {
        sipxCallSetState(hCall, event, cause) ;

        // Find Line
        UtlString requestUri; 
        if (pSession)
        {
           pSession->getRemoteRequestUri(requestUri); 
        }
        hLine = sipxLineLookupHandle(lineId.data(), requestUri.data()) ; 
        if (0 == hLine) 
        {
            // no line exists for the lineId
            // log it
            OsSysLog::add(FAC_SIPXTAPI, PRI_NOTICE, "unknown line id = %s\n", lineId.data());
        }

        // Fill in remote address
        if (szRemoteAddress)
        {
            pCallData = sipxCallLookup(hCall, SIPX_LOCK_WRITE, stackLogger) ;
            if (pCallData)
            {
                if (pCallData->remoteAddress)
                {
                    delete pCallData->remoteAddress;
                }
                pCallData->remoteAddress = new UtlString(szRemoteAddress) ;
                sipxCallReleaseLock(pCallData, SIPX_LOCK_WRITE, stackLogger) ;
            }
        }


        if (g_bListenersEnabled)
        {
            OsLock eventLock(*g_pEventListenerLock) ;
            UtlSListIterator eventListenerItor(*g_pEventListeners);
            while ((ptr = (UtlVoidPtr*) eventListenerItor()) != NULL)
            {
                EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue();
                if (pData->pInst->pCallManager == pSrc)
                {
                    SIPX_CALLSTATE_INFO callInfo;
            
                    memset((void*) &callInfo, 0, sizeof(SIPX_CALLSTATE_INFO));
                    callInfo.event = event;
                    callInfo.cause = cause;
                    callInfo.hCall = hCall;
                    callInfo.hLine = hLine;
                    callInfo.hAssociatedCall = hAssociatedCall ;
                    callInfo.nSize = sizeof(SIPX_CALLSTATE_INFO);
                    
                    pData->pCallbackProc(EVENT_CATEGORY_CALLSTATE, &callInfo, pData->pUserData);
                }
            }
        }
#ifdef DEBUG_SIPXTAPI_EVENTS
        ReportCallback(hCall, hLine, event, cause, NULL) ;
#endif
    }

    // If this is a DESTROY message, free up resources after all listeners
    // have been notified.
    if ((hCall != 0) && (CALLSTATE_DESTROYED == event) && 
            (cause != CALLSTATE_CAUSE_SHUTDOWN))
    {
        SIPX_CONF hConf = sipxCallGetConf(hCall) ;
        if (hConf != 0)
        {
            SIPX_CONF_DATA* pConfData = sipxConfLookup(hConf, SIPX_LOCK_WRITE, stackLogger) ;
            if (pConfData)
            {                        
                sipxRemoveCallHandleFromConf(hConf, hCall) ;
                sipxConfReleaseLock(pConfData, SIPX_LOCK_WRITE, stackLogger) ;
            }
        }
        sipxCallObjectFree(hCall, stackLogger);
    }
    
    if ((CALLSTATE_DISCONNECTED == event) && ((sipxCallGetConf(hCall) != 0) || sipxCallIsRemoveInsteadOfDropSet(hCall)))
    {
        // If a leg of a conference is destroyed, simulate the audio stop and 
        // call destroyed events.

        SIPX_CALL_DATA* pCallData = sipxCallLookup(hCall, SIPX_LOCK_READ, stackLogger) ;
        if (pCallData)
        {
            pCallData->pInst->pCallManager->dropConnection(sessionCallId.data(),
                                                           szRemoteAddress) ;
            sipxCallReleaseLock(pCallData, SIPX_LOCK_READ, stackLogger) ;
        }

        if (pCallData->lastLocalMediaAudioEvent == MEDIA_LOCAL_START)
        {
            sipxFireMediaEvent(pSrc, sessionCallId.data(), szRemoteAddress, 
                    MEDIA_LOCAL_STOP, MEDIA_CAUSE_NORMAL, MEDIA_TYPE_AUDIO,
                    NULL) ;
        }

        if (pCallData->lastLocalMediaVideoEvent == MEDIA_LOCAL_START)
        {
            sipxFireMediaEvent(pSrc, sessionCallId.data(), szRemoteAddress, 
                    MEDIA_LOCAL_STOP, MEDIA_CAUSE_NORMAL, MEDIA_TYPE_VIDEO,
                    NULL) ;
        }

        if ((pCallData->lastRemoteMediaAudioEvent == MEDIA_REMOTE_START) || 
            (pCallData->lastRemoteMediaAudioEvent == MEDIA_REMOTE_SILENT) ||
            (pCallData->lastRemoteMediaAudioEvent == MEDIA_REMOTE_ACTIVE))
        {
            sipxFireMediaEvent(pSrc, sessionCallId.data(), szRemoteAddress, 
                    MEDIA_REMOTE_STOP, MEDIA_CAUSE_NORMAL, MEDIA_TYPE_AUDIO,
                    NULL) ;
        }

        if ((pCallData->lastRemoteMediaVideoEvent == MEDIA_REMOTE_START) || 
            (pCallData->lastRemoteMediaVideoEvent == MEDIA_REMOTE_SILENT) ||
            (pCallData->lastRemoteMediaVideoEvent == MEDIA_REMOTE_ACTIVE))
        {
            sipxFireMediaEvent(pSrc, sessionCallId.data(), szRemoteAddress, 
                    MEDIA_REMOTE_STOP, MEDIA_CAUSE_NORMAL, MEDIA_TYPE_VIDEO,
                    NULL) ;
        }

        sipxFireCallEvent(pSrc, szCallId, pSession, szRemoteAddress,
                       CALLSTATE_DESTROYED,
                       CALLSTATE_CAUSE_NORMAL,
                       pEventData) ;
    }  
    // check for the bHoldAfterConnect flag.  If it is true, start a hold
    if (CALLSTATE_CONNECTED == event || CALLSTATE_HELD == event)
    {
        SIPX_CALL_DATA* pCallData = sipxCallLookup(hCall, SIPX_LOCK_READ, stackLogger);
        if (pCallData)
        {
            bool bHoldAfterConnect = pCallData->bHoldAfterConnect;
            sipxCallReleaseLock(pCallData, SIPX_LOCK_READ, stackLogger);
            
            if (bHoldAfterConnect)
            {
                sipxCallHold(hCall);        
                pCallData = sipxCallLookup(hCall, SIPX_LOCK_WRITE, stackLogger);
                if (pCallData)
                {
                    pCallData->bHoldAfterConnect = false;
                    sipxCallReleaseLock(pCallData, SIPX_LOCK_WRITE, stackLogger);
                }
            }
        }
    }
}


void sipxFireMediaEvent(const void* pSrc,
                        const char* szCallId,
                        const char* szRemoteAddress,
                        SIPX_MEDIA_EVENT event,
                        SIPX_MEDIA_CAUSE cause,
                        SIPX_MEDIA_TYPE type,
                        void* pEventData) 
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
            "sipxFireMediaEvent Src=%p CallId=%s RemoteAddress=%s Event=%s:%s type=%d pEventData:%p",
            pSrc, szCallId, szRemoteAddress, convertMediaEventToString(event), 
            convertMediaCauseToString(cause), type, pEventData);

    SIPX_CALL hCall = sipxCallLookupHandle(szCallId, pSrc) ;
    bool bIgnored = false;

    /*
     * Check/Filter duplicate events
     */
    UtlBoolean bDuplicateEvent = FALSE ;
    if (hCall != SIPX_CALL_NULL ||
        // The following may be call independant events (so hCall can be NULL)
        event == MEDIA_INPUT_DEVICE_NOT_PRESENT ||
        event == MEDIA_OUTPUT_DEVICE_NOT_PRESENT ||
        event == MEDIA_INPUT_DEVICE_NOW_PRESENT ||
        event == MEDIA_OUTPUT_DEVICE_NOW_PRESENT)
    {
        SIPX_MEDIA_EVENT lastLocalMediaAudioEvent ;
        SIPX_MEDIA_EVENT lastLocalMediaVideoEvent ;
        SIPX_MEDIA_EVENT lastRemoteMediaAudioEvent ;
        SIPX_MEDIA_EVENT lastRemoteMediaVideoEvent ;

        if (sipxCallGetMediaState(hCall,
                lastLocalMediaAudioEvent,
                lastLocalMediaVideoEvent,
                lastRemoteMediaAudioEvent,
                lastRemoteMediaVideoEvent))
        {
            switch (type)
            {
                case MEDIA_TYPE_AUDIO:
                    if ((event == MEDIA_LOCAL_START) || (event == MEDIA_LOCAL_STOP))
                    {
                        if (event == lastLocalMediaAudioEvent)
                        {
                            bDuplicateEvent = true ;
                        }
                        else if (event == MEDIA_LOCAL_STOP && lastLocalMediaAudioEvent == MEDIA_UNKNOWN)
                        {
                            // Invalid state change
                            bDuplicateEvent = true ;
                        }
                    } 
                    else if ((event == MEDIA_REMOTE_START) || 
                            (event == MEDIA_REMOTE_STOP) || 
                            (event == MEDIA_REMOTE_SILENT))
                    {
                        if (event == lastRemoteMediaAudioEvent)
                        {
                            bDuplicateEvent = true ;
                        }
                        else if (event == MEDIA_REMOTE_STOP && lastRemoteMediaAudioEvent == MEDIA_UNKNOWN)
                        {
                            // Invalid state change
                            bDuplicateEvent = true ;
                        }
                    }
                    break ;
                case MEDIA_TYPE_VIDEO:
                    if ((event == MEDIA_LOCAL_START) || (event == MEDIA_LOCAL_STOP))
                    {
                        if (event == lastLocalMediaVideoEvent)
                        {
                            bDuplicateEvent = true ;
                        }
                        else if (event == MEDIA_LOCAL_STOP && lastLocalMediaVideoEvent == MEDIA_UNKNOWN)
                        {
                            // Invalid state change
                            bDuplicateEvent = true ;
                        }
                    } 
                    else if ((event == MEDIA_REMOTE_START) || 
                            (event == MEDIA_REMOTE_STOP) || 
                            (event == MEDIA_REMOTE_SILENT))
                    {
                        if (event == lastRemoteMediaVideoEvent)
                        {
                            bDuplicateEvent = true ;
                        }
                       else if (event == MEDIA_REMOTE_STOP && lastRemoteMediaVideoEvent == MEDIA_UNKNOWN)
                        {
                            // Invalid state change
                            bDuplicateEvent = true ;
                        }
                     }
                    break ;
            }
        }
        if (event == MEDIA_REMOTE_SILENT)
        {
            if (type == MEDIA_TYPE_AUDIO)
            {
                if (lastRemoteMediaAudioEvent == MEDIA_REMOTE_STOP)
                {
                    bIgnored = true;
                }
            }
            else if (type == MEDIA_TYPE_VIDEO)
            {
                if (lastRemoteMediaVideoEvent == MEDIA_REMOTE_STOP)
                {
                    bIgnored = true;
                }
            }
        }

        // Only proceed if this isn't a duplicate event 
        if (!bIgnored && !bDuplicateEvent)
        {    
            if (g_bListenersEnabled)
            {
            	OsLock eventLock(*g_pEventListenerLock) ;
                UtlSListIterator eventListenerItor(*g_pEventListeners) ;
                UtlVoidPtr* ptr = NULL;
                while ((ptr = (UtlVoidPtr*) eventListenerItor()) != NULL)
                {
                    EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue();
                    if (pData->pInst->pCallManager == pSrc)
                    {
                        SIPX_MEDIA_INFO mediaInfo ;

                        memset(&mediaInfo, 0, sizeof(mediaInfo)) ;
                        mediaInfo.nSize = sizeof(SIPX_MEDIA_INFO) ;
                        mediaInfo.event = event ;
                        mediaInfo.cause = cause ;
                        mediaInfo.mediaType = MEDIA_TYPE_AUDIO ;
                        mediaInfo.mediaType = type ;
                        mediaInfo.hCall = hCall ;

                        switch (event)
                        {
                            case MEDIA_LOCAL_START:
                            case MEDIA_REMOTE_START:
                                if (pEventData)
                                {
                                    memcpy(&mediaInfo.codec, pEventData, sizeof(SIPX_CODEC_INFO)) ;
                                }
                                break ;

                            case MEDIA_MIC_ENERGY_LEVEL:
                            case MEDIA_SPEAKER_ENERGY_LEVEL:
                            case MEDIA_RTP_ENERGY_LEVEL:
                                mediaInfo.idleTime = (intptr_t) pEventData;
                                break;

                            case MEDIA_REMOTE_SILENT:
                            case MEDIA_RECORDFILE_STOP:
                            case MEDIA_RECORD_PAUSE:
                            case MEDIA_RECORDBUFFER_STOP:
                                mediaInfo.idleTime = (intptr_t) pEventData ;
                                break ;
                            case MEDIA_REMOTE_DTMF:
                                mediaInfo.toneId = (SIPX_TONE_ID) (intptr_t) pEventData ;
                                break;

                            case MEDIA_H264_SPS:
                            case MEDIA_H264_PPS:
                            {
                                printf("sipxFireMediaEvent got %s\n",
                                    event == MEDIA_H264_SPS ? "MEDIA_H264_SPS" : "MEDIA_H264_PPS");

                                SIPX_VIDEO_CODEC* videoCodec = (SIPX_VIDEO_CODEC*) (intptr_t) pEventData;
                                mediaInfo.mediaType = MEDIA_TYPE_VIDEO;
                                memset(&(mediaInfo.codec), 0, sizeof(SIPX_CODEC_INFO));
                                if(videoCodec)
                                {
                                    memcpy(&(mediaInfo.codec.videoCodec), videoCodec, sizeof(SIPX_VIDEO_CODEC));
                                }
                            }
                            break;

                            case MEDIA_INPUT_DEVICE_NOT_PRESENT:
                            case MEDIA_OUTPUT_DEVICE_NOT_PRESENT:
                            case MEDIA_INPUT_DEVICE_NOW_PRESENT:
                            case MEDIA_OUTPUT_DEVICE_NOW_PRESENT:
                                mediaInfo.deviceName = (char*)pEventData;
                                break;

                            default:
                                break ;
                        }
                        pData->pCallbackProc(EVENT_CATEGORY_MEDIA, &mediaInfo, pData->pUserData);
                    }
                }
            }

            if (hCall != SIPX_CALL_NULL)
            {
                sipxCallSetMediaState(hCall, event, type) ;
            }                                 
        }
    }
}

void sipxFireKeepaliveEvent(const void*          pSrc,                                                        
                            SIPX_KEEPALIVE_EVENT event,
                            SIPX_KEEPALIVE_CAUSE cause,
                            SIPX_KEEPALIVE_TYPE  type,
                            const char*          szRemoteAddress,
                            int                  remotePort,
                            int                  keepAliveSecs,
                            const char*          szFeedbackAddress,
                            int                  feedbackPort)
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
            "sipxFireKeepaliveEvent src=%p event=%s:%s type=%s remote=%s:%d keepalive=%ds mapped=%s:%d\n",
            pSrc, 
            convertKeepaliveEventToString(event), convertKeepaliveCauseToString(cause), 
            convertKeepaliveTypeToString(type),
            szRemoteAddress ? szRemoteAddress : "",
            remotePort, 
            keepAliveSecs,
            szFeedbackAddress ? szFeedbackAddress : "",
            feedbackPort) ;
            
           
	OsLock eventLock(*g_pEventListenerLock) ;

    if (g_bListenersEnabled)
    {
        UtlSListIterator eventListenerItor(*g_pEventListeners) ;
        UtlVoidPtr* ptr = NULL;
        while ((ptr = (UtlVoidPtr*) eventListenerItor()) != NULL)
        {
            EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue();
            if (pData->pInst->pCallManager == pSrc)
            {
                SIPX_KEEPALIVE_INFO keepaliveInfo ;

                memset(&keepaliveInfo, 0, sizeof(SIPX_KEEPALIVE_INFO)) ;
                
                keepaliveInfo.nSize = sizeof(SIPX_KEEPALIVE_INFO) ;
                keepaliveInfo.event = event ;
                keepaliveInfo.cause = cause ;
                keepaliveInfo.type = type ;
                keepaliveInfo.szRemoteAddress = SAFE_STRDUP(szRemoteAddress) ;
                keepaliveInfo.remotePort = remotePort ;
                keepaliveInfo.keepAliveSecs = keepAliveSecs ;
                keepaliveInfo.szFeedbackAddress = SAFE_STRDUP(szFeedbackAddress) ;
                keepaliveInfo.feedbackPort = feedbackPort ;
                                                
                pData->pCallbackProc(EVENT_CATEGORY_KEEPALIVE, &keepaliveInfo, pData->pUserData);
            }
        }
    }                           
}


SIPXTAPI_API SIPX_RESULT sipxEventListenerAdd(const SIPX_INST hInst,
                                              SIPX_EVENT_CALLBACK_PROC pCallbackProc,
                                              void* pUserData)
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
            "sipxEventListenerAdd hInst=%p pCallbackProc=%p pUserData=%p",
            hInst, pCallbackProc, pUserData);

    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS;
    if (hInst && pCallbackProc)
    {
        rc = SIPX_RESULT_FAILURE ;
        SIPX_INSTANCE_DATA* pInst = (SIPX_INSTANCE_DATA*) hInst;
        if (pInst && pInst->pEventDispatcher)
        {
            if (pInst->pEventDispatcher->addListener(pCallbackProc, pUserData))
            {
                rc = SIPX_RESULT_SUCCESS ;
            }           
        }
    }

    return rc ;
}

SIPXTAPI_API SIPX_RESULT sipxEventListenerRemove(const SIPX_INST hInst, 
                                                 SIPX_EVENT_CALLBACK_PROC pCallbackProc, 
                                                 void* pUserData) 
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
            "sipxEventListenerRemove hInst=%p pCallbackProc=%p pUserData=%p",
            hInst, pCallbackProc, pUserData);

    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS;
    if (hInst && pCallbackProc)
    {
        rc = SIPX_RESULT_FAILURE ;
        SIPX_INSTANCE_DATA* pInst = (SIPX_INSTANCE_DATA*) hInst;
        if (pInst && pInst->pEventDispatcher)
        {
            if (pInst->pEventDispatcher->removeListener(pCallbackProc, pUserData))
            {
                rc = SIPX_RESULT_SUCCESS ;
            }           
        }
    }

    return rc ;
}


SIPX_RESULT __sipxEventListenerAdd(const SIPX_INST hInst,
                                   SIPX_EVENT_CALLBACK_PROC pCallbackProc,
                                   void *pUserData)
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
        "__sipxEventListenerAdd hInst=%p pCallbackProc=%p pUserData=%p",
        hInst, pCallbackProc, pUserData);
        
    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS;
	OsLock eventLock(*g_pEventListenerLock) ;
    
    if (hInst && pCallbackProc)
    {
	    EVENT_LISTENER_DATA *pData = new EVENT_LISTENER_DATA;
	    pData->pCallbackProc = pCallbackProc;
	    pData->pUserData = pUserData;
        pData->pInst = (SIPX_INSTANCE_DATA*) hInst;
    
	    g_pEventListeners->append(new UtlVoidPtr(pData));
        
        rc = SIPX_RESULT_SUCCESS;
    }

	return rc;
}

SIPX_RESULT __sipxEventListenerRemove(const SIPX_INST hInst, 
                                      SIPX_EVENT_CALLBACK_PROC pCallbackProc, 
                                      void* pUserData) 
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
        "__sipxEventListenerRemove hInst=%p pCallbackProc=%p pUserData=%p",
        hInst, pCallbackProc, pUserData);
        
    SIPX_RESULT rc = SIPX_RESULT_INVALID_ARGS ;

	OsLock eventLock(*g_pEventListenerLock) ;

	UtlVoidPtr* ptr ;

    if (hInst && pCallbackProc)
    {
	    UtlSListIterator itor(*g_pEventListeners) ;
	    while ((ptr = (UtlVoidPtr*) itor()) != NULL)
	    {
		    EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue() ;
		    if ((pData->pCallbackProc == pCallbackProc) &&
			    (pData->pUserData == pUserData) && 
                (pData->pInst->pCallManager == ((SIPX_INSTANCE_DATA*) hInst)->pCallManager))
		    {
			    g_pEventListeners->removeReference(ptr) ;
			    delete pData ;
			    delete ptr ;

                rc = SIPX_RESULT_SUCCESS ;
                break ;
		    }
        }
	}

	return rc ;
}

void sipxUpdateListeners(SIPX_INST hOldInst, SIPX_INST hNewInst) 
{
	OsLock eventLock(*g_pEventListenerLock) ;

	UtlVoidPtr* ptr ;

    if (hOldInst && hNewInst)
    {
	    UtlSListIterator itor(*g_pEventListeners) ;
	    while ((ptr = (UtlVoidPtr*) itor()) != NULL)
	    {
		    EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue() ;
            if (pData->pInst == hOldInst)
            {
                pData->pInst = (SIPX_INSTANCE_DATA*) hNewInst ;
                break ;
		    }
        }
	}
}

static const char* convertLinestateEventToString(SIPX_LINESTATE_EVENT event)
{
    const char* str = "Unknown" ;

    switch (event)
    {
        case LINESTATE_UNKNOWN:
            str = "UNKNOWN";
            break;
        case LINESTATE_REGISTERING:
            str = "REGISTERING";
            break;
        case LINESTATE_REGISTERED:
            str = "REGISTERED";
            break;
        case LINESTATE_UNREGISTERING:
            str = "UNREGISTERING";
            break;
        case LINESTATE_UNREGISTERED:
            str = "UNREGISTERED";
            break;
        case LINESTATE_REGISTER_FAILED:
            str = "REGISTER_FAILED";
            break;
        case LINESTATE_UNREGISTER_FAILED:
            str = "UNREGISTER_FAILED";
            break;
        case LINESTATE_PROVISIONED:
            str = "PROVISIONED";
            break;
        default:
            break ;
    }

    return str;
}


static const char* convertLinestateCauseToString(SIPX_LINESTATE_CAUSE cause)
{
    const char* str = "Unknown" ;

    switch (cause)
    {        
        case LINESTATE_CAUSE_UNKNOWN:
            str = "CAUSE_UNKNOWN";
            break;
        case LINESTATE_REGISTERING_NORMAL:
            str = "REGISTERING_NORMAL";
            break;
        case LINESTATE_REGISTERED_NORMAL:
            str = "REGISTERED_NORMAL";
            break;
        case LINESTATE_UNREGISTERING_NORMAL:
            str = "UNREGISTERING_NORMAL";
            break;
        case LINESTATE_UNREGISTERED_NORMAL:
            str = "UNREGISTERED_NORMAL";
            break;
        case LINESTATE_REGISTER_FAILED_COULD_NOT_CONNECT:
            str = "REGISTER_FAILED_COULD_NOT_CONNECT";
            break;
        case LINESTATE_REGISTER_FAILED_NOT_AUTHORIZED:
            str = "REGISTER_FAILED_NOT_AUTHORIZED";
            break;
        case LINESTATE_REGISTER_FAILED_TIMEOUT:
            str = "REGISTER_FAILED_TIMEOUT";
            break;
        case LINESTATE_UNREGISTER_FAILED_COULD_NOT_CONNECT:
            str = "UNREGISTER_FAILED_COULD_NOT_CONNECT";
            break;
        case LINESTATE_UNREGISTER_FAILED_NOT_AUTHORIZED:
            str = "UNREGISTER_FAILED_NOT_AUTHORIZED";
            break;
        case LINESTATE_UNREGISTER_FAILED_TIMEOUT:
            str = "UNREGISTER_FAILED_TIMEOUT";
            break;
        case LINESTATE_PROVISIONED_NORMAL:
            str = "PROVISIONED_NORMAL";
            break;
        default:
            break;
    }
    return str;
}


SIPXTAPI_API char* sipxConfigEventToString(SIPX_CONFIG_EVENT event, 
                                           char* szBuffer, 
                                           size_t nBuffer) 
{
    switch (event)
    {
        case CONFIG_UNKNOWN:
            SNPRINTF(szBuffer, nBuffer, "CONFIG_UNKNOWN") ;
            break ;
        case CONFIG_STUN_SUCCESS:
            SNPRINTF(szBuffer, nBuffer, "CONFIG_STUN_SUCCESS") ;
            break ;
        case CONFIG_STUN_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "CONFIG_STUN_FAILURE") ;
            break ;
        default:
            SNPRINTF(szBuffer, nBuffer, "ERROR -- UNKNOWN EVENT") ;
            assert(FALSE) ;
            break ;
    }

    return szBuffer;
}


SIPXTAPI_API char* sipxMediaEventToString(SIPX_MEDIA_EVENT event, 
                                           char* szBuffer, 
                                           size_t nBuffer) 
{
    switch (event)
    {
        case MEDIA_LOCAL_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_LOCAL_START") ;
            break ;
            
        case MEDIA_LOCAL_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_LOCAL_STOP") ;
            break ;
        
        case MEDIA_REMOTE_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_REMOTE_START") ;
            break ;
            
        case MEDIA_REMOTE_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_REMOTE_STOP") ;
            break ;
            
        case MEDIA_REMOTE_SILENT:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_REMOTE_SILENT") ;
            break ;
            
        case MEDIA_PLAYFILE_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYFILE_START") ;
            break ;

        case MEDIA_PLAYFILE_FINISH:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYFILE_FINISH") ;
            break ;

        case MEDIA_PLAYFILE_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYFILE_STOP") ;
            break ;

        case MEDIA_PLAYBUFFER_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYBUFFER_START") ;
            break ;

        case MEDIA_PLAYBUFFER_FINISH:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYBUFFER_FINISH") ;
            break ;

        case MEDIA_PLAYBUFFER_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_PLAYBUFFER_STOP") ;
            break ;
            
        case MEDIA_RECORDFILE_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORDFILE_START") ;
            break ;

        case MEDIA_RECORD_PAUSE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORD_PAUSE") ;
            break ;

        case MEDIA_RECORD_RESUME:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORD_RESUME") ;
            break ;

        case MEDIA_RECORDFILE_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORDFILE_STOP") ;
            break ;

        case MEDIA_RECORDBUFFER_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORDBUFFER_START") ;
            break ;

        case MEDIA_RECORDBUFFER_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RECORDBUFFER_STOP") ;
            break ;

        case MEDIA_REMOTE_DTMF:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_REMOTE_DTMF") ;
            break ;

        case MEDIA_DEVICE_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_DEVICE_FAILURE");
            break;

        case MEDIA_UNKNOWN:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_UNKNOWN");
            break ;

        case MEDIA_REMOTE_ACTIVE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_REMOTE_ACTIVE");
            break ;

        case MEDIA_MIC_ENERGY_LEVEL:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_MIC_ENERGY_LEVEL");
            break;

        case MEDIA_SPEAKER_ENERGY_LEVEL:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_SPEAKER_ENERGY_LEVEL");
            break;

        case MEDIA_RTP_ENERGY_LEVEL:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_RTP_ENERGY_LEVEL");
            break;

        case MEDIA_H264_SPS:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_H264_SPS");
            break;

        case MEDIA_H264_PPS:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_H264_PPS");
            break;

        case MEDIA_INPUT_DEVICE_NOT_PRESENT:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_INPUT_DEVICE_NOT_PRESENT");
            break;

        case MEDIA_OUTPUT_DEVICE_NOT_PRESENT:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_OUTPUT_DEVICE_NOT_PRESENT");
            break;

        case MEDIA_INPUT_DEVICE_NOW_PRESENT:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_INPUT_DEVICE_NOW_PRESENT");
            break;

        case MEDIA_OUTPUT_DEVICE_NOW_PRESENT:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_OUTPUT_DEVICE_NOW_PRESENT");
            break;
    }
    return szBuffer;
}

SIPXTAPI_API char* sipxMediaCauseToString(SIPX_MEDIA_CAUSE cause, 
                                           char* szBuffer, 
                                           size_t nBuffer) 
{
    switch (cause)
    {
        case MEDIA_CAUSE_NORMAL:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_NORMAL") ;
            break ;
        case MEDIA_CAUSE_HOLD:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_HOLD") ;
            break ;
        case MEDIA_CAUSE_UNHOLD:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_UNHOLD") ;
            break ;
        case MEDIA_CAUSE_FAILED:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_FAILED") ;
            break ;
        case MEDIA_CAUSE_DEVICE_UNAVAILABLE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_DEVICE_UNAVAILABLE");
            break;
        case MEDIA_CAUSE_INCOMPATIBLE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_INCOMPATIBLE");
            break;
	    case MEDIA_CAUSE_DTMF_START:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_DTMF_START");
	        break;
	    case MEDIA_CAUSE_DTMF_STOP:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_DTMF_STOP");
	        break;
        case MEDIA_CAUSE_DEVICE_NOW_AVAILABLE:
            SNPRINTF(szBuffer, nBuffer, "MEDIA_CAUSE_DEVICE_NOW_AVAILABLE");
            break;
    }
    return szBuffer;
}


SIPXTAPI_API char* sipxKeepaliveEventToString(SIPX_KEEPALIVE_EVENT event, 
                                              char* szBuffer, 
                                              size_t nBuffer) 
{
    switch (event)
    {
        case KEEPALIVE_START:
            SNPRINTF(szBuffer, nBuffer, "KEEPALIVE_START") ;
            break ;
        case KEEPALIVE_FEEDBACK:
            SNPRINTF(szBuffer, nBuffer, "KEEPALIVE_FEEDBACK") ;
            break ;
        case KEEPALIVE_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "KEEPALIVE_FAILURE") ;
            break ;
        case KEEPALIVE_STOP:
            SNPRINTF(szBuffer, nBuffer, "KEEPALIVE_STOP") ;
            break ;
    }
    return szBuffer;
}

SIPXTAPI_API char* sipxKeepaliveCauseToString(SIPX_KEEPALIVE_CAUSE cause, 
                                              char* szBuffer, 
                                              size_t nBuffer) 
{
    switch (cause)
    {
        case KEEPALIVE_CAUSE_NORMAL:
            SNPRINTF(szBuffer, nBuffer, "KEEPALIVE_CAUSE_NORMAL") ;
            break ;
    }
    return szBuffer;
}

SIPXTAPI_API char* sipxSecurityEventToString(SIPX_SECURITY_EVENT event, 
                                           char* szBuffer, 
                                           size_t nBuffer) 
{
    switch (event)
    {
        case SECURITY_ENCRYPT:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_ENCRYPT") ;
            break ;
        case SECURITY_DECRYPT:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_DECRYPT") ;
            break ;
        case SECURITY_TLS:
            SNPRINTF(szBuffer, nBuffer, "SEUCURITY_TLS") ;
            break ;
        default:
            SNPRINTF(szBuffer, nBuffer, "ERROR -- UNKNOWN SECURITY EVENT") ;
            assert(FALSE) ;
            break ;
    }

    return szBuffer;
}

SIPXTAPI_API char* sipxSecurityCauseToString(SIPX_SECURITY_CAUSE cause, 
                                           char* szBuffer, 
                                           size_t nBuffer) 
{
    switch (cause)
    {
        case SECURITY_CAUSE_NORMAL:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_NORMAL") ;
            break ;
        case SECURITY_CAUSE_ENCRYPT_SUCCESS:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_ENCRYPT_SUCCESS") ;
            break ;
        case SECURITY_CAUSE_ENCRYPT_FAILURE_LIB_INIT:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_ENCRYPT_FAILURE_LIB_INIT") ;
            break ;
        case SECURITY_CAUSE_ENCRYPT_FAILURE_BAD_PUBLIC_KEY:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_ENCRYPT_FAILURE_BAD_PUBLIC_KEY") ;
            break ;
        case SECURITY_CAUSE_ENCRYPT_FAILURE_INVALID_PARAMETER:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_ENCRYPT_FAILURE_INVALID_PARAMETER") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_SUCCESS:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_SUCCESS") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_FAILURE_DB_INIT:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_FAILURE_DB_INIT") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_FAILURE_BAD_DB_PASSWORD:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_FAILURE_BAD_DB_PASSWORD") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_FAILURE_INVALID_PARAMETER:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_FAILURE_INVALID_PARAMETER") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_BAD_SIGNATURE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_BAD_SIGNATURE") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_MISSING_SIGNATURE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_MISSING_SIGNATURE") ;
            break ;
        case SECURITY_CAUSE_DECRYPT_SIGNATURE_REJECTED:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_DECRYPT_SIGNATURE_REJECTED") ;
            break ;
        case SECURITY_CAUSE_TLS_SERVER_CERTIFICATE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_SERVER_CERTIFICATE") ;
            break ;
        case SECURITY_CAUSE_TLS_BAD_PASSWORD:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_BAD_PASSWORD");
            break;
        case SECURITY_CAUSE_TLS_LIBRARY_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_LIBRARY_FAILURE");
            break;
        case SECURITY_CAUSE_REMOTE_HOST_UNREACHABLE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_REMOTE_HOST_UNREACHABLE");
            break;
        case SECURITY_CAUSE_TLS_CONNECTION_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_CONNECTION_FAILURE");
            break;
        case SECURITY_CAUSE_TLS_HANDSHAKE_FAILURE:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_HANDSHAKE_FAILURE");
            break;
        case SECURITY_CAUSE_SIGNATURE_NOTIFY:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_SIGNATURE_NOTIFY");
            break;
        case SECURITY_CAUSE_TLS_CERTIFICATE_REJECTED:
            SNPRINTF(szBuffer, nBuffer, "SECURITY_CAUSE_TLS_CERTIFICATE_REJECTED");
            break;
        default:
            SNPRINTF(szBuffer, nBuffer, "ERROR -- UNKNOWN SECURITY CAUSE") ;
            assert(FALSE) ;
            break ;
    }

    return szBuffer;
}

SIPXTAPI_API char* sipxSubStatusStateToString(const SIPX_SUBSCRIPTION_STATE state, 
                                                 char* szBuffer, 
                                                 size_t nBuffer) 
{ 
    switch (state) 
    { 
        case SIPX_SUBSCRIPTION_PENDING: 
            SNPRINTF(szBuffer, nBuffer, "SIPX_SUBSCRIPTION_PENDING") ; 
            break ; 
        case SIPX_SUBSCRIPTION_ACTIVE: 
            SNPRINTF(szBuffer, nBuffer, "SIPX_SUBSCRIPTION_ACTIVE") ; 
            break ; 
        case SIPX_SUBSCRIPTION_FAILED: 
            SNPRINTF(szBuffer, nBuffer, "SIPX_SUBSCRIPTION_FAILED") ; 
            break ; 
        case SIPX_SUBSCRIPTION_EXPIRED: 
            SNPRINTF(szBuffer, nBuffer, "SIPX_SUBSCRIPTION_EXPIRED") ; 
            break ; 
        default: 
            SNPRINTF(szBuffer, nBuffer, "ERROR -- UNKNOWN EVENT") ; 
            assert(FALSE) ; 
            break ; 
    } 

    return szBuffer; 
} 
    
    
SIPXTAPI_API char* sipxSubStatusCauseToString(const SIPX_SUBSCRIPTION_CAUSE cause, 
                                                char* szBuffer, 
                                                size_t nBuffer) 
{ 
    switch (cause) 
    { 
        case SUBSCRIPTION_CAUSE_UNKNOWN: 
            SNPRINTF(szBuffer, nBuffer, "SUBSCRIPTION_CAUSE_UNKNOWN") ; 
            break ; 
        case SUBSCRIPTION_CAUSE_NORMAL: 
            SNPRINTF(szBuffer, nBuffer, "SUBSCRIPTION_CAUSE_NORMAL") ; 
            break ; 
        default: 
            SNPRINTF(szBuffer, nBuffer, "ERROR -- UNKNOWN EVENT") ; 
            assert(FALSE) ; 
            break ; 
    } 

    return szBuffer; 
} 


void sipxFireLineEvent(const void* pSrc,
                       const char* szLineIdentifier,
                       SIPX_LINESTATE_EVENT event,
                       SIPX_LINESTATE_CAUSE cause,
                       const char *bodyBytes)
{
    OsStackTraceLogger stackLogger(FAC_SIPXTAPI, PRI_DEBUG, "sipxFireLineEvent");
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
        "sipxFireLineEvent pSrc=%p szLineIdentifier=%s event=%d cause=%d",
        pSrc, szLineIdentifier, event, cause);

	OsLock eventLock(*g_pEventListenerLock) ;
    SIPX_LINE_DATA* pLineData = NULL;
    SIPX_LINE hLine = SIPX_LINE_NULL ;

    hLine = sipxLineLookupHandleByURI(szLineIdentifier);
    pLineData = sipxLineLookup(hLine, SIPX_LOCK_READ, stackLogger) ;
    if (pLineData)
    {
        if (g_bListenersEnabled)
        {
            UtlVoidPtr* ptr;
            UtlSListIterator eventListenerItor(*g_pEventListeners);
            while ((ptr = (UtlVoidPtr*) eventListenerItor()) != NULL)
            {
                EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue();
                if (pData->pInst->pRefreshManager == pSrc)
                {
                    SIPX_LINESTATE_INFO lineInfo;
                    
                    memset((void*) &lineInfo, 0, sizeof(SIPX_LINESTATE_INFO));
                    lineInfo.event = event;
                    lineInfo.cause = cause;
                    lineInfo.hLine = hLine;
                    lineInfo.nSize = sizeof(SIPX_LINESTATE_INFO);
                    
                    pData->pCallbackProc(EVENT_CATEGORY_LINESTATE, &lineInfo, pData->pUserData);
                }
            }      
        }
        sipxLineReleaseLock(pLineData, SIPX_LOCK_READ, stackLogger) ;  
    }
}

bool sipxFireEvent(const void* pSrc, 
                   SIPX_EVENT_CATEGORY category, 
                   void* pInfo)
{
    OsSysLog::add(FAC_SIPXTAPI, PRI_INFO,
        "sipxFireEvent pSrc=%p category=%d pInfo=%p",
        pSrc, category, pInfo);
	OsLock eventLock(*g_pEventListenerLock) ;

    bool bRet = true;
        
    if (g_bListenersEnabled)
    {
        UtlSListIterator eventListenerItor(*g_pEventListeners);
        UtlVoidPtr* ptr;
        while ((ptr = (UtlVoidPtr*) eventListenerItor()) != NULL)
        {
            EVENT_LISTENER_DATA *pData = (EVENT_LISTENER_DATA*) ptr->getValue();
            if (pData->pInst->pCallManager == pSrc || 
                pData->pInst->pRefreshManager == pSrc ||  
                pData->pInst->pLineManager == pSrc ||
                pData->pInst->pMessageObserver == pSrc ||
                pData->pInst->pSipUserAgent == pSrc)
            {

                // for security events, fill in the hCall
                if (category == EVENT_CATEGORY_SECURITY)
                {
                    SIPX_SECURITY_INFO* pSecInfo = (SIPX_SECURITY_INFO*) pInfo;
                    pSecInfo->hCall = sipxCallLookupHandle(pSecInfo->callId, pSrc);
                }

                if (!pData->pCallbackProc(category, pInfo, pData->pUserData))
                {
                    bRet = false;
                }
            }
        }
    }
    return bRet;
}
