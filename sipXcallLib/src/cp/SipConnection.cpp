//
// Copyright (C) 2005-2019 SIPez LLC.  All rights reserved.
//
// Copyright (C) 2004-2008 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// $$
///////////////////////////////////////////////////////////////////////////////

// Author: Daniel Petrie dpetrie AT SIPez DOT com

// SYSTEM INCLUDES

#ifdef __pingtel_on_posix__
#include <stdlib.h>
#endif

// APPLICATION INCLUDES
#include <os/OsQueuedEvent.h>
#include <os/OsTimer.h>
#include <os/OsUtil.h>
#include <os/OsProcess.h>
#include <utl/UtlRandom.h>
#include <net/NetMd5Codec.h>
#include <net/SipMessageEvent.h>
#include <net/SipUserAgent.h>
#include <utl/UtlNameValueTokenizer.h>
#include <sdp/SdpCodecList.h>
#include <net/Url.h>
#include <net/SipSession.h>
#include <net/NetBase64Codec.h>
#include <cp/SipConnection.h>
#include <mi/CpMediaInterface.h>
#include <cp/CallManager.h>
#include <cp/CpCallManager.h>
#include <cp/CpPeerCall.h>
#include <cp/CpMultiStringMessage.h>
#include <cp/CpIntMessage.h>
#include <os/OsNatAgentTask.h>
#include "ptapi/PtCall.h"
#include <net/TapiMgr.h>
#ifdef HAVE_NSS
#include "net/pk12wrapper.h"
#endif

// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS
#define CALL_STATUS_FIELD "status"
#define MAX_ADDRESS_CANDIDATES      12

#ifdef _WIN32
#   define CALL_CONTROL_TONES
#endif

// STATIC VARIABLE INITIALIZATIONS
const UtlContainableType SipConnection::TYPE = "SipConnection";
UtlRandom* SipConnection::sRandom = NULL;

/* //////////////////////////// PUBLIC //////////////////////////////////// */

/* ============================ CREATORS ================================== */

// Constructor
SipConnection::SipConnection(const char* outboundLineAddress,
                             UtlBoolean isEarlyMediaFor180Enabled,
                             CpCallManager* callMgr,
                             CpCall* call,
                             CpMediaInterface* mediaInterface,
                             //UiContext* callUiContext,
                             SipUserAgent* sipUA,
                             int offeringDelayMilliSeconds,
                             int sessionReinviteTimer,
                             int availableBehavior,
                             const char* forwardUnconditionalUrl,
                             int busyBehavior,
                             const char* forwardOnBusyUrl)
                             : Connection(callMgr, call, mediaInterface, offeringDelayMilliSeconds,
                             availableBehavior, forwardUnconditionalUrl,
                             busyBehavior, forwardOnBusyUrl),
                             mpPassThroughData(NULL)
{
    if(sRandom == NULL)
    {
        sRandom = new UtlRandom();
    }

    inviteFromThisSide = 0 ;
    mIsEarlyMediaFor180 = true ;
    mContactId = 0 ;
    mpSecurity = 0 ;
    mbByeAttempted = false ;
    mRtpTransport = RTP_TRANSPORT_UDP ;
    mRtpTcpRole = RTP_TCP_ROLE_ACTPASS ;
    sipUserAgent = sipUA;
    inviteMsg = NULL;
    mReferMessage = NULL;
    lastLocalSequenceNumber = 0;
    lastRemoteSequenceNumber = -1;
    reinviteState = ACCEPT_INVITE;
    mIsEarlyMediaFor180 = isEarlyMediaFor180Enabled;
    mDropping = FALSE ;
    mLocationHeader = NULL;
    mBandwidthId = AUDIO_MICODEC_BW_DEFAULT;
    mbLocallyInitiatedRemoteHold = false ;
    mRemoteUserAgent = NULL;
    mContactType = CONTACT_AUTO ;

    // Build a from tag
    int fromTagInt = sRandom->rand();
    char fromTagBuffer[60];
    sprintf(fromTagBuffer, "%dc%d", call->getCallIndex(), fromTagInt);
    mFromTag = fromTagBuffer;

    if(outboundLineAddress)
    {
        mFromUrl = outboundLineAddress;

        // Before adding the from tag, construct the local contact with the
        // device's NAT friendly contact information (getContactUri).  The host
        // and port from that replaces the public address or record host and
        // port.  The UserId and URL parameters should be retained.
        UtlString contactHostPort;
        UtlString address;
        Url tempUrl(mFromUrl);
        sipUserAgent->getContactUri(&contactHostPort);
        Url hostPort(contactHostPort);
        hostPort.getHostAddress(address);
        tempUrl.setHostAddress(address);
        tempUrl.setHostPort(hostPort.getHostPort());
        tempUrl.toString(mLocalContact);

        // Set the from tag in case this is an outbound call
        // If this is an in bound call, the from URL will get
        // over written by the To field from the SIP request
        mFromUrl.setFieldParameter("tag", mFromTag);
    }

    mDefaultSessionReinviteTimer = sessionReinviteTimer;
    mSessionReinviteTimer = 0;

#ifdef TEST_PRINT
    osPrintf("SipConnection::mDefaultSessionReinviteTimer = %d\n",
        mDefaultSessionReinviteTimer);
#endif

    mIsReferSent = FALSE;
    mIsAcceptSent = FALSE;

    mbCancelling = FALSE;        // this is the flag that indicates CANCEL is sent
    // but no response has been received  if set to TRUE

    // State variable which indicates an action to
    // perform after hold has completed.
    mHoldCompleteAction = CpCallManager::CP_UNSPECIFIED;
#ifdef TEST_PRINT
    if (!callId.isNull())
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Leaving SipConnection constructor: %s\n", callId.data());
    else
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Leaving SipConnection constructor: call is Null\n");
#endif

}

// Destructor
SipConnection::~SipConnection()
{
    UtlString callId;
#ifdef TEST_PRINT
    if (mpCall) {
        mpCall->getCallId(callId);
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Entering SipConnection destructor: %s\n", callId.data());
    } else
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Entering SipConnection destructor: call is Null\n");
#endif

    if(inviteMsg)
    {
        delete inviteMsg;
        inviteMsg = NULL;
    }
    if(mReferMessage)
    {
        delete mReferMessage;
        mReferMessage = NULL;
    }

    if(mpPassThroughData)
    {
        delete mpPassThroughData;
        mpPassThroughData = NULL;
    }

#ifdef TEST_PRINT
    if (!callId.isNull())
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Leaving SipConnection destructor: %s\n", callId.data());
    else
        OsSysLog::add(FAC_CP, PRI_DEBUG, "Leaving SipConnection destructor: call is Null\n");
#endif
}

/* ============================ MANIPULATORS ============================== */

// Assignment operator
SipConnection&
SipConnection::operator=(const SipConnection& rhs)
{
    if (this == &rhs)            // handle the assignment to self case
        return *this;

    return *this;
}


UtlBoolean SipConnection::dequeue()
{
    UtlBoolean connectionDequeued = FALSE;
#ifdef TEST_PRINT
    osPrintf("Connection::dequeue this: %p inviteMsg: %p\n", this, inviteMsg);
#endif
    if(getState() == CONNECTION_QUEUED)
    {
        int tagNum = -1;
        proceedToRinging(inviteMsg, sipUserAgent, tagNum, mLineAvailableBehavior);

        setState(CONNECTION_ALERTING, CONNECTION_LOCAL);
        /** SIPXTAPI: TBD **/

        connectionDequeued = TRUE;
    }

    return(connectionDequeued);
}

UtlBoolean SipConnection::requestShouldCreateConnection(const SipMessage* sipMsg,
                                                        SipUserAgent& sipUa,
                                                        SdpCodecList* codecFactory)
{
    UtlBoolean createConnection = FALSE;
    UtlString method;
    sipMsg->getRequestMethod(&method);
    UtlString toField;
    UtlString address;
    UtlString protocol;
    int port;
    UtlString user;
    UtlString userLabel;
    UtlString tag;
    sipMsg->getToAddress(&address, &port, &protocol, &user, &userLabel, &tag);

    // Dangling or delated ACK
    if(method.compareTo(SIP_ACK_METHOD) == 0)
    {
        // Ignore it and do not create a connection
        createConnection = FALSE;
    }

    // INVITE to create a connection
    //if to tag is already set then return 481 error
    else if(method.compareTo(SIP_INVITE_METHOD) == 0 && tag.isNull())
    {
        // Assume the best case, as this will be checked
        // again before the call is answered
        UtlBoolean atLeastOneCodecSupported = TRUE;
        if(codecFactory == NULL ||
            codecFactory->getCodecCount() == 0)
            atLeastOneCodecSupported = TRUE;

        // Verify that we have some RTP codecs in common
        else
        {
            // Get the SDP and findout if there are any
            // codecs in common
            UtlString rtpAddress;
            int rtpPort;
            int rtcpPort;
            int videoRtpPort;
            int videoRtcpPort;
            int localBandwidth = 0;
            int matchingBandwidth = 0;
            int localVideoFramerate = 0;
            int matchingVideoFramerate = 0;
            const SdpBody* bodyPtr = sipMsg->getSdpBody(sipMsg->getSecurityAttributes());
            if(bodyPtr)
            {
                int numMatchingCodecs = 0;
                SdpCodec** encoderCodecs = NULL;
                SdpSrtpParameters srtpParamsPlaceholder;
                memset(&srtpParamsPlaceholder, 0, sizeof(srtpParamsPlaceholder));

                bodyPtr->getBestAudioCodecs(*codecFactory,
                                            numMatchingCodecs,
                                            encoderCodecs,
                                            rtpAddress,
                                            rtpPort,
                                            rtcpPort,
                                            videoRtpPort,
                                            videoRtcpPort,
                                            srtpParamsPlaceholder,
                                            srtpParamsPlaceholder,
                                            localBandwidth,
                                            matchingBandwidth,
                                            localVideoFramerate,
                                            matchingVideoFramerate);

                if(numMatchingCodecs > 0)
                {
                    atLeastOneCodecSupported = TRUE;
                }
                else
                {
                    atLeastOneCodecSupported = FALSE;

                    // Send back a bad media error
                    // There are no codecs in common
                    SipMessage badMediaResponse;
                    badMediaResponse.setInviteBadCodecs(sipMsg, &sipUa);
                    sipUa.send(badMediaResponse);
                }

                // Free up the codec copies and pointer array
                SdpCodecList::freeArray(numMatchingCodecs, encoderCodecs);
            }

            // Assume that SDP will be sent in ACK
            else
                atLeastOneCodecSupported = TRUE;
        }

        if(atLeastOneCodecSupported)
        {
            // Create a new connection
            createConnection = TRUE;
        }
        else
        {
            createConnection = FALSE;
#ifdef TEST_PRINT
            osPrintf("SipConnection::requestShouldCreateConnection FALSE INVITE with no supported RTP codecs\n");
#endif
        }
    }

    // NOTIFY for REFER
    // a non-existing transaction.
    else if(method.compareTo(SIP_NOTIFY_METHOD) == 0)
    {
        UtlString eventType;
        sipMsg->getEventField(eventType);
        eventType.toLower();
        int typeIndex = eventType.index(SIP_EVENT_REFER);
        if(typeIndex >=0)
        {
            // Send a bad callId/transaction message
            SipMessage badTransactionMessage;
            badTransactionMessage.setBadTransactionData(sipMsg);
            sipUa.send(badTransactionMessage);
        }
        // All other NOTIFY events are ignored
        createConnection = FALSE;
    }

    else if(method.compareTo(SIP_REFER_METHOD) == 0)
    {
        createConnection = TRUE;
    }

    // All other methods: this is part of
    // a non-existing transaction.
    else
    {
        // Send a bad callId/transaction message
        SipMessage badTransactionMessage;
        badTransactionMessage.setBadTransactionData(sipMsg);
        if(tag.isNull())
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "SipConnection::requestShouldCreateConnection invalid method(%s) for creating new call",
                    method.data());
        }
        else
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "SipConnection::requestShouldCreateConnection for method: %s has To tag: %s",
                    method.data(), tag.data());
        }

        sipUa.send(badTransactionMessage);
        createConnection = FALSE;
    }

    return createConnection;
}

UtlBoolean SipConnection::shouldCreateConnection(SipUserAgent& sipUa,
                                                 OsMsg& eventMessage,
                                                 SdpCodecList* codecFactory)
{
    UtlBoolean createConnection = FALSE;
    int msgType = eventMessage.getMsgType();
    int msgSubType = eventMessage.getMsgSubType();
    const SipMessage* sipMsg = NULL;
    int messageType;

    if(msgType == OsMsg::PHONE_APP &&
        msgSubType == CallManager::CP_SIP_MESSAGE)
    {
        sipMsg = ((SipMessageEvent&)eventMessage).getMessage();
        messageType = ((SipMessageEvent&)eventMessage).getMessageStatus();
#ifdef TEST_PRINT
        OsSysLog::add(FAC_CP, PRI_DEBUG,
                      "SipConnection::messageType: %d", messageType);
#endif

        switch(messageType)
        {
            // This is a request which failed to get sent
        case SipMessageEvent::TRANSPORT_ERROR:
        case SipMessageEvent::SESSION_REINVITE_TIMER:
        case SipMessageEvent::AUTHENTICATION_RETRY:
            // Ignore it and do not create a connection
            createConnection = FALSE;
            break;

        default:
            // Its a SIP Response
            if(sipMsg->isResponse())
            {
                // Ignore it and do not create a connection
                createConnection = FALSE;
            }
            // Its a SIP Request
            else
            {
                createConnection = SipConnection::requestShouldCreateConnection(sipMsg, sipUa, codecFactory);
            }
            break;
        }

        if(!createConnection)
        {
            UtlString msgBytes;
            int numBytes;
            sipMsg->getBytes(&msgBytes, &numBytes);
            msgBytes.insert(0, "SipConnection::shouldCreateConnection: FALSE\n");
#ifdef TEST_PRINT
            osPrintf("%s\n", msgBytes.data());
#endif
        }
#ifdef TEST_PRINT
        else
        {

            osPrintf("Create a SIP connection\n");
        }
#endif

    }

    return(createConnection);
}

// Select a compatible contact type given the request URL
SIPX_CONTACT_TYPE SipConnection::selectCompatibleContactType(const SipMessage& request)
{
    SIPX_CONTACT_TYPE contactType = mContactType ;
    SIPX_TRANSPORT_TYPE eTransportType = TRANSPORT_UDP ;

    if (mContactId > 0)
    {
        // Someone has given us a contact type (likely from sipXcallAccept/
        // acceptConnection).  Use it.
        SIPX_CONTACT_ADDRESS* pContact = sipUserAgent->getContactDb().find(mContactId) ;
        assert(pContact) ;
        if (pContact)
        {
            contactType = pContact->eContactType ;
        }
    }
    else
    {
        // Try to figure out the best-case contact type from the request URI.  If
        // nothing matches -- use auto.

        // Parse useful data from the request URI
        UtlString requestUriHost ;
        int requestUriPort ;
        UtlString strUri ;
        request.getRequestUri(&strUri) ;
        Url requestUri(strUri, true) ;
        requestUri.getHostAddress(requestUriHost) ;
        requestUriPort = requestUri.getHostPort() ;
        if (!portIsValid(requestUriPort))
        {
            requestUriPort = 5060 ;
        }

        // Figure out 'best' transport given request URI
        UtlString transportParam ;
        if (requestUri.getUrlParameter("transport", transportParam))
        {
            eTransportType = sipUserAgent->getContactDb().findTransportType(
                    transportParam) ;
        }
        else if (requestUri.getScheme() == Url::SipsUrlScheme)
        {
            eTransportType = TRANSPORT_TLS ;
        }

        SIPX_CONTACT_ADDRESS stun_contact;
        SIPX_CONTACT_ADDRESS local_contact;
        UtlString adapterName;
        UtlString localAddress;
        getLocalAddress(&localAddress);

        getContactAdapterName(adapterName, localAddress, false);

        // Look for matches against the request URI and our DB of IP addresses --
        // the user other side sent to our NAT address, then STICK to it.
        if (sipUserAgent->getContactDb().getRecordForAdapter(stun_contact,
                adapterName.data(), CONTACT_NAT_MAPPED, eTransportType) &&
                (strcmp(stun_contact.cIpAddress, requestUriHost) == 0) &&
                (requestUriPort == (!portIsValid(stun_contact.iPort) ?
                5060 : stun_contact.iPort)))
        {
            mContactId = stun_contact.id;
            contactType = CONTACT_NAT_MAPPED ;
        }
        else if (sipUserAgent->getContactDb().getRecordForAdapter(local_contact,
                adapterName.data(), CONTACT_LOCAL, eTransportType) &&
                (strcmp(local_contact.cIpAddress, requestUriHost) == 0) &&
                (requestUriPort == (!portIsValid(local_contact.iPort) ?
                5060 : local_contact.iPort)))
        {
            mContactId = local_contact.id;
            contactType = CONTACT_LOCAL ;
        }
        else if (eTransportType >= TRANSPORT_CUSTOM)
        {
            // If using an external transport, this is unlikely to match --
            // assume NAT the local
            if (sipUserAgent->getContactDb().getRecordForAdapter(stun_contact,
                adapterName.data(), CONTACT_NAT_MAPPED, eTransportType))
            {
                mContactId = stun_contact.id;
                contactType = CONTACT_NAT_MAPPED ;
            }
            else if (sipUserAgent->getContactDb().getRecordForAdapter(local_contact,
                     adapterName.data(), CONTACT_LOCAL, eTransportType))
            {
                mContactId = local_contact.id;
                contactType = CONTACT_LOCAL ;
            }
        }
    }

    return contactType ;
}


void SipConnection::updateContact(Url*               pContactUrl,
                                  SIPX_CONTACT_TYPE  eType,
                                  Url*               pToUrl,
                                  UtlString*         pRemoteHostOrIp,
                                  int*               pRemotePort)
{
    UtlString useIp ;

    // If we don't have a contact selected and we have an invite, try
    // to select a compatible contact.  This sets the mContactId as a side
    // effect.
    if ((mContactId == 0) && inviteMsg && !inviteFromThisSide)
    {
        mContactType = selectCompatibleContactType(*inviteMsg);
    }

    // Try looking up the contact record
    SIPX_CONTACT_ADDRESS* pContact = sipUserAgent->getContactDb().find(mContactId);
    if (pContact == NULL)
    {
        // If we didn't find one (or this is the initial request), select one based
        // on the requested

        // Try NAT first
        if ((eType == CONTACT_AUTO) || (eType == CONTACT_NAT_MAPPED) || (eType == CONTACT_RELAY))
        {
            pContact = sipUserAgent->getContactDb().findByType(CONTACT_NAT_MAPPED, TRANSPORT_UDP);
        }

        if (pContact == NULL)
        {
            UtlString to;
            if (inviteMsg)
            {
                inviteMsg->getToUri(&to);
                to.toLower();
                if (to.contains("sips:") || to.contains("transport=tls"))
                {
                    pContact = sipUserAgent->getContactDb().findByType(CONTACT_LOCAL, TRANSPORT_TLS);
                }
                else if (to.contains("transport=tcp"))
                {
                    pContact = sipUserAgent->getContactDb().findByType(CONTACT_LOCAL, TRANSPORT_TCP);
                }
                else if (to.contains("transport=") && !to.contains("transport=udp"))
                {
                    Url toUrl(to);
                    UtlString sTransport;
                    UtlString localIp = inviteMsg->getLocalIp();

                    toUrl.getUrlParameter("transport", sTransport);
                    pContact = sipUserAgent->getContactDb().findByType(CONTACT_LOCAL, TRANSPORT_CUSTOM, sTransport);
                }
                else
                {
                    pContact = sipUserAgent->getContactDb().findByType(CONTACT_LOCAL, TRANSPORT_UDP);
                }
            }
            else
            {
                pContact = sipUserAgent->getContactDb().find(mContactId);
            }
        }
    }

    // If we do have a valid contact record, tweak the URL
    if (pContact)
    {
        switch (pContact->eTransportType)
        {
            case TRANSPORT_UDP:
                {
                    UtlString contactIP = pContact->cIpAddress ;
                    int contactPort = pContact->iPort ;

                    if ( pToUrl &&
                            (pContact->eContactType == CONTACT_NAT_MAPPED ||
                            pContact->eContactType == CONTACT_RELAY)    )
                    {
                        UtlString checkHost ;
                        int checkPort ;

                        pToUrl->getHostAddress(checkHost) ;
                        checkPort = pToUrl->getHostPort() ;
                        OsNatAgentTask::getInstance()->findContactAddress(
                                checkHost, checkPort,
                                &contactIP, &contactPort) ;
                    }

                    pContactUrl->setScheme(Url::SipUrlScheme) ;
                    pContactUrl->setHostAddress(contactIP) ;
                    pContactUrl->setHostPort(contactPort) ;
                    pContactUrl->removeUrlParameter("transport") ;
                }
                break ;
            case TRANSPORT_TCP:
                pContactUrl->setScheme(Url::SipUrlScheme) ;
                pContactUrl->setHostAddress(pContact->cIpAddress) ;
                pContactUrl->setHostPort(pContact->iPort) ;
                pContactUrl->removeUrlParameter("transport") ;
                pContactUrl->setUrlParameter("transport", "tcp");
                break ;
            case TRANSPORT_TLS:
                pContactUrl->setScheme(Url::SipsUrlScheme) ;
                pContactUrl->setHostAddress(pContact->cIpAddress) ;
                pContactUrl->setHostPort(pContact->iPort) ;
                pContactUrl->removeUrlParameter("transport") ;
                break ;
            case TRANSPORT_CUSTOM:
            default:
                {
                    Url fromUrl ;
                    UtlString customRouteId = pContact->cCustomRouteID ;

                    if (customRouteId.isNull())
                    {
                        pContactUrl->setHostAddress(pContact->cIpAddress) ;
                        pContactUrl->setHostPort(pContact->iPort) ;
                    }
                    else
                    {
                        pContactUrl->setUserId(NULL) ;
                        pContactUrl->setHostAddress(customRouteId) ;
                        pContactUrl->setHostPort(0) ;
                    }

                    pContactUrl->setScheme(Url::SipUrlScheme) ;
                    pContactUrl->removeUrlParameter("transport") ;
                    pContactUrl->setUrlParameter("transport", pContact->cCustomTransportName) ;
                }
                break ;
        }
    }
}

void SipConnection::updateContactFromResponse(const SipMessage* pResponse)
{
    UtlString viaAddr ;
    int viaPort = -1 ;
    int receivedPort = -1 ;
    UtlString viaProtocol ;
    UtlBoolean receivedSet = false ;
    UtlBoolean maddrSet = false ;
    UtlBoolean receivedPortSet = false ;
    UtlBoolean shouldMapRportResponse = TRUE;
    if(sipUserAgent)
    {
        shouldMapRportResponse = sipUserAgent->getUseRportMapping();
    }

    pResponse->getLastVia(&viaAddr, &viaPort, &viaProtocol, &receivedPort, &receivedSet, &maddrSet, &receivedPortSet) ;
    if (shouldMapRportResponse && (receivedSet || receivedPortSet))
    {
        if (receivedPortSet && portIsValid(receivedPort))
        {
            viaPort = receivedPort ;
        }

        Url contactURL(mLocalContact) ;
        contactURL.setHostAddress(viaAddr) ;
        contactURL.setHostPort(viaPort) ;
        contactURL.toString(mLocalContact) ;
    }
}


void SipConnection::buildLocalContact(Url fromUrl,
                                      UtlString& localContact,
                                      Url* pToUrl)
{

    // Get host and port from local contact
    UtlString address;
    UtlString contactHostPort;
    sipUserAgent->getContactUri(&contactHostPort);
    Url hostPort(contactHostPort);
    hostPort.getHostAddress(address);
    int port = hostPort.getHostPort();

    // Get display name and user id from from Url
    UtlString displayName;
    UtlString userId;
    fromUrl.getDisplayName(displayName);
    fromUrl.getUserId(userId);

    // Construct a new contact URL with host/port from local contact
    // and display name/userid from From URL
    Url contactUrl(mLocalContact, FALSE);
    contactUrl.setUserId(userId.data());
    contactUrl.setDisplayName(displayName);
    contactUrl.setHostAddress(address);
    contactUrl.setHostPort(port);
    contactUrl.includeAngleBrackets();

    updateContact(&contactUrl, mContactType, pToUrl) ;
    contactUrl.toString(localContact);
}


UtlBoolean SipConnection::dial(const char* dialString,
                               const char* localLineAddress,
                               const char* callId,
                               const char* callController,
                               const char* originalCallConnection,
                               UtlBoolean  requestQueuedCall,
                               const void* pDisplay,
                               const void* pSecurity,
                               const char* locationHeader,
                               const int   bandWidth,
                               UtlBoolean  bOnHold,
                               const char* originalCallId,
                               const RTP_TRANSPORT rtpTransportOptions)
{
    UtlBoolean dialOk = FALSE;
    SipMessage sipInvite;
    const char* callerDisplayName = NULL;
    UtlString hostAddresses[MAX_ADDRESS_CANDIDATES];
    int receiveRtpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES];
    RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
    int nRtpContacts ;
    int totalBandwidth = 0;
    SdpSrtpParameters srtpParams;
    UtlString dummyFrom;
    UtlString fromAddress;
    UtlString goodToAddress;
    int cause = CONNECTION_CAUSE_NORMAL;
    mBandwidthId = bandWidth;
    mbLocallyInitiatedRemoteHold = bOnHold ;
    int iCSeq ;

    mRtpTransport = (RTP_TRANSPORT_UDP | RTP_TRANSPORT_TCP) & rtpTransportOptions;
    mRtpTcpRole = (RTP_TCP_ROLE_ACTIVE |  RTP_TCP_ROLE_PASSIVE |  RTP_TCP_ROLE_ACTPASS) & rtpTransportOptions;
    memset(&srtpParams, 0, sizeof(srtpParams));

    if(getState() == CONNECTION_IDLE && mpMediaInterface != NULL)
    {
        UtlString localAddress ;
        SIPX_CONTACT_ADDRESS* pAddress = sipUserAgent->getContactDb().getLocalContact(mContactId) ;
        if (pAddress != NULL)
        {
            localAddress = pAddress->cIpAddress ;
        }

        UtlBoolean bAudioAvailable = FALSE;
        if (mpMediaInterface)
        {
            bAudioAvailable = mpMediaInterface->isAudioAvailable();
        }

        mRemoteIsCallee = TRUE;
        setCallId(callId);

        buildFromToAddresses(dialString, "xxxx", callerDisplayName,
            dummyFrom, goodToAddress);
        mLastToAddress = goodToAddress;

        // The local address is always set
        mFromUrl.toString(fromAddress);

        // Create a new connection in the media flowgraph
        if (!bAudioAvailable ||
            mpMediaInterface->createConnection(mConnectionId,
                                               localAddress,
                                               0,
                                               (void*)pDisplay,
                                               (void*)pSecurity,
                                               rtpTransportOptions) != OS_SUCCESS)
        {
            setCallId(callId);
            if (!bAudioAvailable)
            {
                fireSipXMediaEvent(MEDIA_DEVICE_FAILURE, MEDIA_CAUSE_DEVICE_UNAVAILABLE, MEDIA_TYPE_AUDIO);
            }
            setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_RESOURCES_NOT_AVAILABLE);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_RESOURCE_LIMIT) ;
        }
        else
        {
            if(mpPassThroughData)
            {
                OsSysLog::add(FAC_CP, PRI_DEBUG, "SipConnection::dial mpPassThroughData cached, setting on mediaConnection: %d", mConnectionId);
                mpMediaInterface->setMediaPassThrough(mConnectionId,
                                                      mpPassThroughData->mMediaType,
                                                      mpPassThroughData->mMediaTypeStreamIndex,
                                                      mpPassThroughData->mMediaRecieveAddress,
                                                      mpPassThroughData->mRtpPort,
                                                      mpPassThroughData->mRtcpPort);
                delete mpPassThroughData;
                mpPassThroughData = NULL;
            }

            int numCodecs;
            SdpCodec** rtpCodecsArray = NULL;
            mpMediaInterface->setContactType(mConnectionId, mContactType, mContactId) ;
            SdpCodecList supportedCodecs;
            nRtpContacts = 0 ;
            int videoFramerate = 0;
            mpMediaInterface->getCapabilitiesEx(mConnectionId,
                MAX_ADDRESS_CANDIDATES,
                hostAddresses,
                receiveRtpPorts,
                receiveRtcpPorts,
                receiveVideoRtpPorts,
                receiveVideoRtcpPorts,
                transportTypes,
                nRtpContacts,
                supportedCodecs,
                srtpParams,
                bandWidth,
                totalBandwidth,
                videoFramerate);

            // Use the full set of codecs for offer
            supportedCodecs.getCodecs(numCodecs, rtpCodecsArray);

            // If we want the call initially on hold, clear our the rtp addresses
            if (mbLocallyInitiatedRemoteHold)
            {
                hostAddresses[0] = "0.0.0.0" ;
                nRtpContacts = 1 ;
            }


#ifdef TEST_PRINT
            osPrintf("Using To address: \"%s\"\n", goodToAddress.data());
#endif

            if (callController && callController[0] != '\0')
            {
                UtlString origCallId;

                if (originalCallId !=NULL)
                {
                    // if there was one passed-in, use it
                    origCallId = originalCallId;
                }
                else
                {
                    // get the original callid from the call object
                    mpCall->getOriginalCallId(origCallId);
                }
                fireSipXCallEvent(CALLSTATE_NEWCALL, CALLSTATE_CAUSE_TRANSFER, (void*) origCallId.data()) ;
            }

            // Create and send an INVITE
            sipInvite.setSecurityAttributes(mpSecurity);
            mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
            sipInvite.setInviteData(fromAddress.data(), goodToAddress.data(),
                  NULL, mLocalContact.data(), callId,
                  iCSeq, mDefaultSessionReinviteTimer);

            if (!mpCallManager->isIceEnabled())
            {
                nRtpContacts = 1 ;
            }
            sipInvite.addSdpBody(nRtpContacts, hostAddresses, receiveRtpPorts,
                  receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                  transportTypes,
                  numCodecs, rtpCodecsArray, &srtpParams, totalBandwidth,
                  videoFramerate, NULL, rtpTransportOptions) ;


            if (locationHeader)
            {
                mLocationHeader = locationHeader;
                sipInvite.setLocationField(locationHeader);
            }

            // Set caller preference if caller wants queueing or campon
            if(requestQueuedCall)
            {
                sipInvite.addRequestDisposition(SIP_DISPOSITION_QUEUE);
            }

            // Set the requested by field (BYE Also style transfer)
            if(callController && originalCallConnection == NULL)
            {
                UtlString requestedByField(callController);
                const char* alsoTags = strchr(dialString, '>');
                int uriIndex = requestedByField.index('<');
                if(uriIndex < 0)
                {
                    requestedByField.insert(0, '<');
                    requestedByField.append('>');
                }
                if(alsoTags)
                {
                    alsoTags++;
                    requestedByField.append(alsoTags);
                }
                sipInvite.setRequestedByField(requestedByField.data());
                cause = CONNECTION_CAUSE_TRANSFER;
            }

            // Set the header fields for REFER style transfer INVITE
            if(callController && originalCallConnection)
            {
                mOriginalCallConnectionAddress = originalCallConnection;
                sipInvite.setReferredByField(callController);
                cause = CONNECTION_CAUSE_TRANSFER;
            }

            if (numCodecs == 0)
            {
                setState(Connection::CONNECTION_FAILED, Connection::CONNECTION_LOCAL);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NO_CODECS);
            } else if (!prepareInviteSdpForSend(&sipInvite, mConnectionId, pSecurity))
            {
                setState(Connection::CONNECTION_FAILED, Connection::CONNECTION_LOCAL);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_SMIME_FAILURE) ;
            }
            else
            {
                // Save a copy of the invite
                inviteMsg = new SipMessage(sipInvite);
                inviteFromThisSide = TRUE;
                setCallerId();

                if (mbLocallyInitiatedRemoteHold)
                {
                    setState(Connection::CONNECTION_ESTABLISHED, Connection::CONNECTION_LOCAL, 0, PtTerminalConnection::HELD);
                }
                else
                {
                    setState(Connection::CONNECTION_ESTABLISHED, Connection::CONNECTION_LOCAL);
                }

                if(!goodToAddress.isNull() && send(sipInvite))
                {
                    setState(CONNECTION_INITIATED, CONNECTION_REMOTE, cause);
                    setState(CONNECTION_OFFERING, CONNECTION_REMOTE, cause);
                    dialOk = TRUE;
                    fireSipXCallEvent(CALLSTATE_REMOTE_OFFERING, CALLSTATE_CAUSE_NORMAL) ;

                    // Prepare to receive the codecs, without any srtp
                    // for early media
                    if (!mbLocallyInitiatedRemoteHold)
                    {
                        // Dial case, we send INVITe with SDP so we cannot later match payload IDs of remote side
                        mpMediaInterface->startRtpReceive(mConnectionId,
                                numCodecs,
                                rtpCodecsArray);
                        fireAudioStartEvents();
                    }
                }
                else
                {
    #ifdef TEST_PRINT
                    osPrintf("INVITE send failed\n");
    #endif
                    setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_DEST_NOT_OBTAINABLE);
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_ADDRESS) ;
                    // Failed to send a message for transfer
                    if(callController && !goodToAddress.isNull())
                    {
                        // ( back a status to the original call
                        UtlString originalCallId;
                        mpCall->getOriginalCallId(originalCallId);
                        CpMultiStringMessage transfereeStatus(CallManager::CP_TRANSFEREE_CONNECTION_STATUS,
                            originalCallId.data(),
                            mOriginalCallConnectionAddress.data(),
                            NULL, NULL, NULL,
                            CONNECTION_FAILED, SIP_REQUEST_TIMEOUT_CODE);
    #ifdef TEST_PRINT
                        osPrintf("SipConnection::dial posting CP_TRANSFEREE_CONNECTION_STATUS to call: %s\n",
                            originalCallId.data());
    #endif
                        mpCallManager->postMessage(transfereeStatus);
                    }
                }
            }

            // Free up the codec copies and pointer array
            SdpCodecList::freeArray(numCodecs, rtpCodecsArray);
        }
    }

    return(dialOk);
}


UtlBoolean SipConnection::canSendInfo()
{
    return !mCSeqMgr.isInTransaction(CSEQ_ID_INFO) ;
}


UtlBoolean SipConnection::sendInfo(UtlString contentType, UtlString sContent)
{
    bool bRet = false;

    SipMessage sipInfoMessage;
    UtlString fromAddress;
    UtlString toAddress;
    UtlString callId;
    int       iCSeq ;

    mCSeqMgr.startTransaction(CSEQ_ID_INFO, iCSeq) ;

    getFromField(&fromAddress);
    getRemoteAddress(&toAddress);
    getCallId(&callId);
    sipInfoMessage.setRequestData(SIP_INFO_METHOD, mRemoteContact, fromAddress, toAddress, callId);
    sipInfoMessage.setContactField(mLocalContact.data());
    sipInfoMessage.setContentType(contentType.data());
    sipInfoMessage.setContentLength(sContent.length());
    if(! mRouteField.isNull())
    {
        // set route
        sipInfoMessage.setRouteField(mRouteField);
    }
    HttpBody* pBody = new HttpBody(sContent.data(), sContent.length());
    sipInfoMessage.setBody(pBody);
    sipInfoMessage.setCSeqField(iCSeq, SIP_INFO_METHOD);

    if(send(sipInfoMessage, sipUserAgent->getMessageQueue()))
    {
        bRet = true;
    }
    else
    {
        // With sipX TAPI, send network error event.
        SIPX_INFOSTATUS_INFO info;

        memset((void*) &info, 0, sizeof(SIPX_INFOSTATUS_INFO));

        info.event = INFOSTATUS_NETWORK_ERROR;
        info.nSize = sizeof(SIPX_INFOSTATUS_INFO);
        info.hInfo = 0;
        info.szResponseText = (const char*)"INFO: network error";
        TapiMgr::getInstance().fireEvent(this->mpCallManager,
                                         EVENT_CATEGORY_INFO_STATUS, &info);
    }

    //delete pBody; // DONT delete here!  body is deleted by HttpMessage class
    return bRet;
}

void SipConnection::outOfFocus()
{
    //mTerminalConnState = PtTerminalConnection::HELD ;
}

UtlBoolean SipConnection::answer(const void* pDisplay)
{
#ifdef TEST_PRINT
    OsSysLog::add(FAC_SIP, PRI_WARNING,
        "Entering SipConnection::answer inviteMsg=0x%08x ", (int)inviteMsg);
#endif

    UtlBoolean answerOk = FALSE;
    const SdpBody* sdpBody = NULL;
    UtlString hostAddresses[MAX_ADDRESS_CANDIDATES] ;
    int receiveRtpPorts[MAX_ADDRESS_CANDIDATES] ;
    int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
    int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES] ;
    int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
    RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
    int numAddresses = 0;
    int totalBandwidth = 0;
    int videoFramerate = 0;
    int matchingBandwidth;
    int matchingVideoFramerate;
    SdpCodecList supportedCodecs;
    SdpSrtpParameters srtpParams;
    SdpSrtpParameters matchingSrtpParams;

    memset(&srtpParams, 0, sizeof(srtpParams));
    memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));

    int currentState = getState();
    if( mpMediaInterface != NULL &&
        inviteMsg && !inviteFromThisSide &&
        (currentState == CONNECTION_ALERTING ||
        currentState == CONNECTION_OFFERING ||
        currentState == CONNECTION_INITIATED ||
        currentState == CONNECTION_IDLE))
    {
        int numEncoderCodecs = 0;
        SdpCodec** encoderCodecs = NULL;
        int numDecoderCodecs = 0;
        SdpCodec** decoderCodecs = NULL;

        mpMediaInterface->setVideoWindowDisplay(pDisplay);
        // Get supported codecs

        mpMediaInterface->setSecurityAttributes(mpSecurity);
        mpMediaInterface->getCapabilitiesEx(mConnectionId,
            MAX_ADDRESS_CANDIDATES,
            hostAddresses,
            receiveRtpPorts,
            receiveRtcpPorts,
            receiveVideoRtpPorts,
            receiveVideoRtcpPorts,
            transportTypes,
            numAddresses,
            supportedCodecs,
            srtpParams,
            mBandwidthId,
            totalBandwidth,
            videoFramerate);

        inviteMsg->setSecurityAttributes(mpSecurity);
        sdpBody = inviteMsg->getSdpBody(mpSecurity, mpCallManager);

        if (mpSecurity && !sdpBody)
        {
            SipMessage notAcceptable;

            notAcceptable.setResponseData(inviteMsg,
                                          SIP_REQUEST_NOT_ACCEPTABLE_HERE_CODE,
                                          SIP_REQUEST_NOT_ACCEPTABLE_HERE_TEXT);
            send(notAcceptable);

            setState(CONNECTION_FAILED, CONNECTION_REMOTE);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REMOTE_SMIME_UNSUPPORTED) ;

        }
        else
        {
            getInitialSdpCodecs(inviteMsg,
                                supportedCodecs,
                                numEncoderCodecs,
                                encoderCodecs,
                                remoteRtpAddress,
                                remoteRtpPort,
                                remoteRtcpPort,
                                remoteVideoRtpPort,
                                remoteVideoRtcpPort,
                                srtpParams,
                                matchingSrtpParams,
                                totalBandwidth,
                                matchingBandwidth,
                                videoFramerate,
                                matchingVideoFramerate);

            fireIncompatibleCodecsEvent(&supportedCodecs, encoderCodecs, numEncoderCodecs) ;

            if (matchingBandwidth != 0)
            {
                mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
            }
            if (matchingVideoFramerate != 0)
            {
                mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
            }

            // Test for matching codecs and common encryption. If we insist on encryption
            // and remote side is not capable then fail.
            if (mpSecurity && srtpParams.securityLevel && matchingSrtpParams.securityLevel == 0)
            {
                // No common codecs send INVITE error response
                SipMessage sipResponse;
                sipResponse.setInviteBadCodecs(inviteMsg, sipUserAgent);
                send(sipResponse);

                setState(CONNECTION_FAILED, CONNECTION_LOCAL, CONNECTION_CAUSE_RESOURCES_NOT_AVAILABLE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REMOTE_SMIME_UNSUPPORTED) ;
            }
            // Compatable codecs send OK response
            else
            {
                // Respond with an OK
#ifdef TEST_PRINT
                osPrintf("Sending INVITE OK\n");
#endif

                // There was no SDP in the INVITE, so give them all of
                // the codecs we support.
                if(!sdpBody)
                {
#ifdef TEST_PRINT
                    osPrintf("Sending initial SDP in OK\n");
#endif
                }

                // Try to match the payload IDs from the remote side's SDP offer
                mpMediaInterface->copyPayloadIds(mConnectionId, numEncoderCodecs, encoderCodecs);

                // Tweak Contact given request URI / settings
                setContactType(selectCompatibleContactType(*inviteMsg)) ;

                // Get Media Capabilties (need to call again because rtp
                // addresses and ports may have changed)
                mpMediaInterface->getCapabilitiesEx(mConnectionId,
                        MAX_ADDRESS_CANDIDATES,
                        hostAddresses,
                        receiveRtpPorts,
                        receiveRtcpPorts,
                        receiveVideoRtpPorts,
                        receiveVideoRtcpPorts,
                        transportTypes,
                        numAddresses,
                        supportedCodecs,
                        srtpParams,
                        mBandwidthId,
                        totalBandwidth,
                        videoFramerate);

                // Make a copy of the codec that we offered as we must be prepared to receive all
                // of them
                supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

                // If there was SDP in the INVITE and it indicated hold:
                if( sdpBody &&
                    ((remoteRtpPort <= 0) || (remoteRtpAddress.compareTo("0.0.0.0") == 0)))
                {
                    hostAddresses[0] = "0.0.0.0";  // hold address
                }

                // If we were originally want a hold, then don't allow the far
                if (mbLocallyInitiatedRemoteHold)
                {
                    hostAddresses[0] = "0.0.0.0" ;
                    numAddresses = 1 ;
                }

                // Build response
                SipMessage sipResponse;

                sipResponse.setSecurityAttributes(mpSecurity);
                sipResponse.setInviteOkData(inviteMsg,
                        mDefaultSessionReinviteTimer, mLocalContact.data());

                if (mLocationHeader.length() != 0)
                {
                    sipResponse.setLocationField(mLocationHeader.data());
                }
                if (strcmp(hostAddresses[0], "0.0.0.0") == 0)
                {
                    mHoldState = TERMCONNECTION_HELD ;

                    sipResponse.addSdpBody(1, hostAddresses, receiveRtpPorts,
                            receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                            transportTypes,
                            numDecoderCodecs, decoderCodecs, &matchingSrtpParams,
                            totalBandwidth, matchingVideoFramerate, inviteMsg) ;
                }
                else
                {
                    mHoldState = TERMCONNECTION_TALKING ;

                    if (!mpCallManager->isIceEnabled())
                    {
                        numAddresses = 1 ;
                    }
                    sipResponse.addSdpBody(numAddresses, hostAddresses, receiveRtpPorts,
                            receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                            transportTypes,
                            numDecoderCodecs, decoderCodecs, &matchingSrtpParams,
                            totalBandwidth, matchingVideoFramerate, inviteMsg) ;
                }

                if (mRtpTransport != RTP_TRANSPORT_UDP)
                {
                    // the callee must determine its TCP role,
                    // which, in turn will determine the remote
                    // endpoint's TCP role.
                    // The tcp role will be communicated via the sdp

                    // determine TCP connectivity
                    // only if (not 0.0.0.0)
                    if (remoteRtpAddress.compareTo("0.0.0.0") != 0)
                    {
                        if (mRtpTcpRole != RTP_TCP_ROLE_ACTIVE)
                        {
                            mRtpTcpRole = RTP_TCP_ROLE_PASSIVE;
                        }
                        mpMediaInterface->setConnectionTcpRole(mConnectionId, mRtpTcpRole);
                    }
                }

                prepareInviteSdpForSend(&sipResponse, mConnectionId, mpSecurity) ;

                // Send a INVITE OK response
                if(!send(sipResponse))
                {
                    OsSysLog::add(FAC_CP, PRI_DEBUG,
                        "SipConnection::answer: INVITE OK failed: %s",
                        remoteRtpAddress.data());
                    OsSysLog::add(FAC_CP, PRI_DEBUG,
                        "SipConnection::answer: CONNECTION_FAILED, CONNECTION_LOCAL, CONNECTION_CAUSE_NORMAL");

                    setState(CONNECTION_FAILED, CONNECTION_LOCAL, CONNECTION_CAUSE_NORMAL);
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NETWORK) ;
                }
                else
                {
                    int termState = PtTerminalConnection::TALKING ;
                    if (!mpCall->isInFocus())
                    {
                        termState = PtTerminalConnection::HELD ;
                    }

                    setState(CONNECTION_ESTABLISHED, CONNECTION_LOCAL, CONNECTION_CAUSE_NORMAL, termState);

                    if (mpCall->isInFocus())
                    {
                        if (mHoldState == TERMCONNECTION_TALKING)
                        {
                            fireSipXCallEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_NORMAL);
                        }
                        else
                        {
                            fireSipXCallEvent(CALLSTATE_REMOTE_HELD, CALLSTATE_CAUSE_NORMAL);
                        }
                    }
                    else
                    {
                        if (mHoldState == TERMCONNECTION_TALKING)
                        {
                            fireSipXCallEvent(CALLSTATE_BRIDGED, CALLSTATE_CAUSE_NORMAL);
                        }
                        else
                        {
                            fireSipXCallEvent(CALLSTATE_HELD, CALLSTATE_CAUSE_NORMAL);
                        }
                    }

                    answerOk = TRUE;

                    // Setup media channel
                    if (!mpCall->isInFocus())
                    {
                        mpMediaInterface->defocus() ;
                    }

                    if (mHoldState == TERMCONNECTION_TALKING)
                    {
                        // Delayed SDP, none in INVITE
                        if(sdpBody == NULL && numDecoderCodecs > 0)
                        {
                            // Answer case, if the INVITE had SDP we can match the payload IDs of the remote side
                            // otherwise, we cannot later match the remote payload IDs when we recieve SDP in the ACK
                            // Need to be prepared to receive RTP once we have advertised codecs.
                            mpMediaInterface->startRtpReceive(mConnectionId,
                                    numDecoderCodecs, decoderCodecs);
                            fireAudioStartEvents();
                        }

                        // if we have a send codec chosen Start sending media
                        else if(numEncoderCodecs > 0)
                        {
                            // Answer case, if the INVITE had SDP we can match the payload IDs of the remote side
                            // otherwise, we cannot later match the remote payload IDs when we recieve SDP in the ACK
                            mpMediaInterface->startRtpReceive(mConnectionId,
                                    numDecoderCodecs, decoderCodecs);
                            fireAudioStartEvents();

                            setMediaDestination(remoteRtpAddress.data(),
                                remoteRtpPort,
                                remoteRtcpPort,
                                remoteVideoRtpPort,
                                remoteVideoRtcpPort,
                                inviteMsg->getSdpBody(mpSecurity));

                            if(remoteRtpPort > 0)
                            {
                                // TODO: Answering, should also check if remote audio and video is actively receiving

                                mpMediaInterface->startRtpSend(mConnectionId,
                                    numEncoderCodecs, encoderCodecs);
                            }
                            fireAudioStartEvents() ;
                        }
                        else
                        {
                            mpMediaInterface->stopRtpSend(mConnectionId);
                            mpMediaInterface->stopRtpReceive(mConnectionId);

                            fireAudioStopEvents() ;
                        }
                    }
                    inviteMsg->getAllowField(mAllowedRemote);
                }
            }
        }

        // Free up the codec copies and pointer array
        SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
        SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);
    }

    return(answerOk);
}

UtlBoolean SipConnection::accept(int ringingTimeOutSeconds, 
                                 const void* pSecurity,
                                 const char* locationHeader, 
                                 const int bandWidth,
                                 UtlBoolean sendEarlyMedia)
{
    UtlBoolean ringingSent = FALSE;
    int cause = 0;
    UtlString contentType("");
    mBandwidthId = bandWidth;

    if (inviteMsg)
    {
        inviteMsg->getContentType(&contentType);
    }

    if (locationHeader)
    {
        mLocationHeader = locationHeader;
    }

    if (pSecurity)
    {
        mpSecurity = (SIPXTACK_SECURITY_ATTRIBUTES*)pSecurity;
#ifdef HAVE_NSS
        if (mpSecurity->getCertDbLocation() != NULL)
        {
            P12Wrapper_Init((char*)mpSecurity->getCertDbLocation(), "");
        }
        else
        {
            P12Wrapper_Init(".", "");
        }
#endif
    }

    if(mpMediaInterface != NULL && inviteMsg &&
        !inviteFromThisSide && getState(true /*LOCAL_ONLY*/, cause) == CONNECTION_OFFERING)
    {
        UtlString hostAddresses[MAX_ADDRESS_CANDIDATES];
        int receiveRtpPorts[MAX_ADDRESS_CANDIDATES];
        int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES];
        int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES];
        int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES];
        RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
        int numAddresses;
        int numEncoderCodecs = 0;
        int numDecoderCodecs = 0;
        int matchingBandwidth = 0;
        int totalBandwidth = 0;
        int videoFramerate = 0;
        int matchingVideoFramerate;
        SdpCodec** encoderCodecs = NULL;
        SdpCodec** decoderCodecs = NULL;
        SdpCodecList supportedCodecs;
        UtlString replaceCallId;
        UtlString replaceToTag;
        UtlString replaceFromTag;
        SdpSrtpParameters srtpParams;
        SdpSrtpParameters matchingSrtpParams;

        memset(&srtpParams, 0, sizeof(srtpParams));
        memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));

        // Make sure that this isn't part of a transfer.  If we find a
        // REPLACES header, then we shouldn't accept the call, but rather
        // we should return a 481 response.
        if (inviteMsg->getReplacesData(replaceCallId, replaceToTag, replaceFromTag))
        {
            SipMessage badTransaction;
            badTransaction.setBadTransactionData(inviteMsg);
            send(badTransaction);
#ifdef TEST_PRINT
            osPrintf("SipConnection::accept - CONNECTION_FAILED, cause BUSY : 754\n");
#endif
            OsSysLog::add(FAC_CP, PRI_ERR, "SipConnection::accept - incoming call part of a transfer");
            setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_BUSY);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_TRANSACTION_DOES_NOT_EXIST) ;
        }
        else if (!pSecurity && contentType.compareTo(CONTENT_SMIME_PKCS7) == 0)
        {
            SipMessage undecipherable;
            undecipherable.setResponseData(inviteMsg, SIP_REQUEST_UNDECIPHERABLE_CODE,
                SIP_REQUEST_UNDECIPHERABLE_TEXT);
            setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_NOT_ALLOWED);
            setState(CONNECTION_FAILED, CONNECTION_LOCAL, CONNECTION_CAUSE_NOT_ALLOWED);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_SMIME_FAILURE) ;
            send(undecipherable);
        }
        else
        {
            mpMediaInterface->getCapabilitiesEx(mConnectionId,
                                                MAX_ADDRESS_CANDIDATES,
                                                hostAddresses,
                                                receiveRtpPorts,
                                                receiveRtcpPorts,
                                                receiveVideoRtpPorts,
                                                receiveVideoRtcpPorts,
                                                transportTypes,
                                                numAddresses,
                                                supportedCodecs,
                                                srtpParams,
                                                mBandwidthId, // ?
                                                totalBandwidth,
                                                videoFramerate);

            inviteMsg->setSecurityAttributes(mpSecurity);

            // Get the codecs to send/encode if SDP is provided
            getInitialSdpCodecs(inviteMsg,
                                supportedCodecs,
                                numEncoderCodecs,
                                encoderCodecs,
                                remoteRtpAddress,
                                remoteRtpPort,
                                remoteRtcpPort,
                                remoteVideoRtpPort,
                                remoteVideoRtcpPort,
                                srtpParams,
                                matchingSrtpParams,
                                totalBandwidth,
                                matchingBandwidth,
                                videoFramerate,
                                matchingVideoFramerate);

            // Try to match the payload IDs from the remote side's SDP offer
            mpMediaInterface->copyPayloadIds(mConnectionId, numEncoderCodecs, encoderCodecs);

            // Need to get the codecs again as the payload IDs may have changed
            mpMediaInterface->getCapabilitiesEx(mConnectionId,
                                                MAX_ADDRESS_CANDIDATES,
                                                hostAddresses,
                                                receiveRtpPorts,
                                                receiveRtcpPorts,
                                                receiveVideoRtpPorts,
                                                receiveVideoRtcpPorts,
                                                transportTypes,
                                                numAddresses,
                                                supportedCodecs,
                                                srtpParams,
                                                mBandwidthId, // ?
                                                totalBandwidth,
                                                videoFramerate);

            // Make a copy of the codecs that we offered as we must be prepared to receive all
            // of them
            supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

            if (matchingBandwidth != 0)
            {
                mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
            }
            if (matchingVideoFramerate != 0)
            {
                mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
            }

            ringingSent = TRUE;
            proceedToRinging(inviteMsg, 
                             sipUserAgent, 
                             -1, 
                             mLineAvailableBehavior,
                             sendEarlyMedia ? numAddresses : 0, 
                             hostAddresses, 
                             receiveRtpPorts,
                             receiveRtcpPorts, 
                             receiveVideoRtpPorts, 
                             receiveVideoRtcpPorts,
                             transportTypes,
                             numDecoderCodecs, 
                             decoderCodecs, 
                             &matchingSrtpParams,
                             totalBandwidth, 
                             matchingVideoFramerate);

            // Keep track of the fact that this is a transfer
            if(cause != CONNECTION_CAUSE_TRANSFER)
            {
                cause = CONNECTION_CAUSE_NORMAL;
            }
            setState(CONNECTION_ALERTING, CONNECTION_LOCAL, cause);
            fireSipXCallEvent(CALLSTATE_ALERTING, CALLSTATE_CAUSE_NORMAL) ;

            // Try to setup for early receipt of media.
            if(numEncoderCodecs > 0)
            {
                // Accept case, if the INVITE had SDP we can match the payload IDs of the remote side
                // otherwise, we cannot later match the remote payload IDs when we recieve SDP in the ACK
                mpMediaInterface->startRtpReceive(mConnectionId,
                        numDecoderCodecs, decoderCodecs);
                fireAudioStartEvents();

                // if early media is desired, start sending RTP
                if(sendEarlyMedia)
                {
                    setMediaDestination(remoteRtpAddress,
                                        remoteRtpPort,
                                        remoteRtcpPort,
                                        remoteVideoRtpPort,
                                        remoteVideoRtcpPort,
                                        inviteMsg->getSdpBody(mpSecurity));

                    if(remoteRtpPort > 0)
                    {
                        // TODO: Accepting, should also check to see if remote streams are actively receiving

                        mpMediaInterface->startRtpSend(mConnectionId,
                                                       numEncoderCodecs, 
                                                       encoderCodecs);
                    }
                    fireAudioStartEvents();
                }
            }

            // If forward on no answer is enabled set the timer
            if(ringingTimeOutSeconds > 0 )
            {
                // Set a timer to post a message to this call
                // to timeout the ringing and forward
                setRingingTimer(ringingTimeOutSeconds);
            }

            // Free up the codec copies and pointer array
            SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
            SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);
        }
    }
    return(ringingSent);
}


UtlBoolean SipConnection::reject(int errorCode, const char* errorText)
{
    UtlBoolean responseSent = FALSE;
    if(inviteMsg && !inviteFromThisSide)
    {
        int state = getState();
        if (state == CONNECTION_OFFERING)
        {
            UtlString replaceCallId;
            UtlString replaceToTag;
            UtlString replaceFromTag;

            // Make sure that this isn't part of a transfer.  If we find a
            // REPLACES header, then we shouldn't accept the call, but rather
            // we should return a 481 response.
            if (inviteMsg->getReplacesData(replaceCallId, replaceToTag, replaceFromTag))
            {
                SipMessage badTransaction;
                badTransaction.setBadTransactionData(inviteMsg);
                responseSent = send(badTransaction);
#ifdef TEST_PRINT
                osPrintf("SipConnection::reject - CONNECTION_FAILED, cause BUSY : 825\n");
#endif
                setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_BUSY);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_TRANSACTION_DOES_NOT_EXIST) ;
            }
            else
            {
                SipMessage rejectMessage;
                rejectMessage.setInviteErrorData(inviteMsg,
                                                 errorCode < 400 ? SIP_BUSY_CODE : errorCode,
                                                 (errorText == NULL || errorText[0] == '\0') ? SIP_BUSY_TEXT : errorText);
                responseSent = send(rejectMessage);
#ifdef TEST_PRINT
                osPrintf("SipConnection::reject - CONNECTION_FAILED, cause BUSY : 833\n");
#endif
                setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_BUSY);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;
            }
        }
        else if (state == CONNECTION_ALERTING)
        {
            SipMessage terminateMessage;
            terminateMessage.setRequestTerminatedResponseData(inviteMsg);
            responseSent = send(terminateMessage);
#ifdef TEST_PRINT
            osPrintf("SipConnection::reject - CONNECTION_DISCONNECTED, cause CONNECTION_CAUSE_CANCELLED : 845\n");
#endif
            setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_CANCELLED);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;
        }
    }
    return(responseSent);
}

UtlBoolean SipConnection::redirect(const char* forwardAddress)
{
    UtlBoolean redirectSent = FALSE;
    if(inviteMsg && !inviteFromThisSide &&
        (getState() == CONNECTION_OFFERING ||
        getState() == CONNECTION_ALERTING))
    {
        UtlString targetUrl;
        UtlString dummyFrom;
        const char* callerDisplayName = NULL;
        const char* targetCallId = NULL;
        buildFromToAddresses(forwardAddress, targetCallId, callerDisplayName,
            dummyFrom, targetUrl);
        // Send a redirect message
        SipMessage redirectResponse;
        redirectResponse.setForwardResponseData(inviteMsg,
            targetUrl.data());
        if (mLocationHeader.length() != 0)
        {
            redirectResponse.setLocationField(mLocationHeader.data());
        }
        redirectSent = send(redirectResponse);
        setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_REDIRECTED);
        setState(CONNECTION_DISCONNECTED, CONNECTION_LOCAL, CONNECTION_CAUSE_REDIRECTED);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REDIRECTED) ;

        targetUrl = OsUtil::NULL_OS_STRING;
        dummyFrom = OsUtil::NULL_OS_STRING;
    }
    return(redirectSent);
}

UtlBoolean SipConnection::hangUp()
{
    return(doHangUp());
}

UtlBoolean SipConnection::hold()
{
    UtlBoolean messageSent = FALSE;
    SdpSrtpParameters srtpParams;
    int iCSeq ;
    memset(&srtpParams, 0, sizeof(srtpParams));

    // If the call is connected and we are not in the middle of a SIP transaction
    // note:  we used to check for an existing hold state, but, we might
    //        be doing a re-hold if we are replacing a call leg that had sent
    //        a hold message to the remote side.
    if(mpMediaInterface != NULL &&
            inviteMsg && getState() == CONNECTION_ESTABLISHED &&
            reinviteState == ACCEPT_INVITE) /* &&
            mTerminalConnState!=PtTerminalConnection::HELD)*/
    {
        UtlString hostAddresses[MAX_ADDRESS_CANDIDATES] ;
        int receiveRtpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
        RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
        int numAddresses = 0;
        int totalBandwidth = 0;
        int videoFramerate = 0;
        SdpCodecList supportedCodecs;
        mpMediaInterface->getCapabilitiesEx(mConnectionId,
            MAX_ADDRESS_CANDIDATES,
            hostAddresses,
            receiveRtpPorts,
            receiveRtcpPorts,
            receiveVideoRtpPorts,
            receiveVideoRtcpPorts,
            transportTypes,
            numAddresses,
            supportedCodecs,
            srtpParams,
            mBandwidthId,
            totalBandwidth,
            videoFramerate);
        int numCodecs = 0;
        SdpCodec** codecsArray = NULL;
        supportedCodecs.getCodecs(numCodecs, codecsArray);

        mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;

        // Build an INVITE with the RTP address in the SDP of 0.0.0.0
        SipMessage holdMessage;
        holdMessage.setReinviteData(inviteMsg,
            mRemoteContact,
            mLocalContact.data(),
            inviteFromThisSide,
            mRouteField,
            iCSeq,
            mDefaultSessionReinviteTimer);

        if (mLocationHeader.length() != 0)
        {
           holdMessage.setLocationField(mLocationHeader.data());
        }
        hostAddresses[0] = "0.0.0.0" ;
        holdMessage.addSdpBody(1, hostAddresses, receiveRtpPorts,
                    receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                    transportTypes,
                    numCodecs, codecsArray, &srtpParams, totalBandwidth, videoFramerate) ;

        prepareInviteSdpForSend(&holdMessage, mConnectionId, mpSecurity) ;
        if(inviteMsg)
        {
            delete inviteMsg;
        }
        inviteMsg = new SipMessage(holdMessage);
        inviteFromThisSide = TRUE;

        if(send(holdMessage))
        {
            messageSent = TRUE;

            // Disallow INVITEs while this transaction is taking place
            reinviteState = REINVITING;
            mHoldState = TERMCONNECTION_HOLDING;
        }

        // Free up the codec copies and array
        SdpCodecList::freeArray(numCodecs, codecsArray);
    }

    mbLocallyInitiatedRemoteHold = true ;

    return(messageSent);
}

UtlBoolean SipConnection::offHold()
{
    return(doOffHold(FALSE));
}


UtlBoolean SipConnection::renegotiateCodecs()
{
    if (mCSeqMgr.isInTransaction(CSEQ_ID_INVITE))
    {
        UtlString remoteAddress ;
        UtlString callId ;

        getRemoteAddress(&remoteAddress) ;
        getCallId(&callId) ;

        CpMultiStringMessage* retryTimerMessage =
                new CpMultiStringMessage(CpCallManager::CP_RENEGOTIATE_CODECS_CONNECTION, callId, remoteAddress) ;
        OsTimer* timer = new OsTimer(mpCallManager->getMessageQueue(), (intptr_t)retryTimerMessage);

        OsTime timerTime(500) ;
        timer->oneshotAfter(timerTime);
    }
    else
    {
        silentRemoteHold() ;
        doOffHold(TRUE) ;
    }

    return true ;
}


UtlBoolean SipConnection::silentRemoteHold()
{
    if (mpMediaInterface)
    {
        mpMediaInterface->stopRtpSend(mConnectionId);
        mpMediaInterface->stopRtpReceive(mConnectionId);
        fireAudioStopEvents() ;
    }

    return true ;
}

UtlBoolean SipConnection::doOffHold(UtlBoolean forceReInvite)
{
    UtlBoolean messageSent = FALSE;
    SdpSrtpParameters srtpParams;
    int totalBandwidth = 0;
    int videoFramerate = 0;

    mRemoteRequestedHold = FALSE;

    memset(&srtpParams, 0, sizeof(srtpParams));

    // If the call is connected and
    // we are not in the middle of a SIP transaction
    if(mpMediaInterface != NULL &&
            inviteMsg && getState() == CONNECTION_ESTABLISHED &&
            reinviteState == ACCEPT_INVITE &&
            (mTerminalConnState == PtTerminalConnection::HELD ||
            (forceReInvite &&
            mTerminalConnState == PtTerminalConnection::TALKING)))

    {
        UtlString hostAddresses[MAX_ADDRESS_CANDIDATES] ;
        int receiveRtpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES] ;
        int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES] ;
        RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
        int numAddresses = 0;
        SdpCodecList supportedCodecs;
        mpMediaInterface->getCapabilitiesEx(mConnectionId,
            MAX_ADDRESS_CANDIDATES,
            hostAddresses,
            receiveRtpPorts,
            receiveRtcpPorts,
            receiveVideoRtpPorts,
            receiveVideoRtcpPorts,
            transportTypes,
            numAddresses,
            supportedCodecs,
            srtpParams,
            mBandwidthId,
            totalBandwidth,
            videoFramerate);

        int numCodecs = 0;
        SdpCodec** rtpCodecs = NULL;
        supportedCodecs.getCodecs(numCodecs, rtpCodecs);

        // Build an INVITE with the RTP address in the SDP
        // as the real address
#ifdef TEST_PRINT
        osPrintf("SipConnection::offHold rtpAddress: %s\n",
            rtpAddress.data());
#endif

        int iCSeq ;
        mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
        SipMessage offHoldMessage;
        offHoldMessage.setReinviteData(inviteMsg,
            mRemoteContact,
            mLocalContact.data(),
            inviteFromThisSide,
            mRouteField,
            iCSeq,
            mDefaultSessionReinviteTimer);

        if (mLocationHeader.length() != 0)
        {
           offHoldMessage.setLocationField(mLocationHeader.data());
        }

        if (!mpCallManager->isIceEnabled())
        {
            numAddresses = 1 ;
        }
        offHoldMessage.addSdpBody(numAddresses, hostAddresses, receiveRtpPorts,
                    receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                    transportTypes,
                    numCodecs, rtpCodecs, &srtpParams, totalBandwidth, videoFramerate) ;
        mHoldState = TERMCONNECTION_UNHOLDING  ;

        // Free up the codec copies and array
        SdpCodecList::freeArray(numCodecs, rtpCodecs);

        if(inviteMsg)
        {
            delete inviteMsg;
        }

        prepareInviteSdpForSend(&offHoldMessage, mConnectionId, mpSecurity) ;

        inviteMsg = new SipMessage(offHoldMessage);
        inviteFromThisSide = TRUE;

        if(send(offHoldMessage))
        {
            messageSent = TRUE;
            // Disallow INVITEs while this transaction is taking place
            reinviteState = REINVITING;

            // If we are doing a forced reINVITE
            // there are no state changes

            // Otherwise signal the offhold state changes
            if(!forceReInvite)
            {
                if (mpCall->getCallType() != CpCall::CP_NORMAL_CALL)
                {
                    mpCall->setCallType(CpCall::CP_NORMAL_CALL);
                }
                setState(CONNECTION_ESTABLISHED, CONNECTION_REMOTE, CONNECTION_CAUSE_UNHOLD);
                // fireSipXEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_NORMAL) ;
            }
        }
    }

    mbLocallyInitiatedRemoteHold = false ;

    return(messageSent);
}

UtlBoolean SipConnection::originalCallTransfer(UtlString&  dialString,
                                               const char* transferControllerAddress,
                                               const char* targetCallId)
{
    UtlBoolean ret = FALSE;

    mIsReferSent = FALSE;
#ifdef TEST_PRINT
    UtlString remoteAddr;
    getRemoteAddress(&remoteAddr);
    UtlString conState;
    getStateString(getState(), &conState);
    osPrintf("SipConnection::originalCallTransfer on %s %x %x:\"%s\" state: %d\n",
        remoteAddr.data(), inviteMsg, dialString.data(),
        dialString.length() ? dialString.data() : "", conState.data());
#endif
    if(inviteMsg && dialString && *dialString &&
        getState() == CONNECTION_ESTABLISHED)
    {
        // If the transferee (the party at the other end of this
        // connection) supports the REFER method
        const char* callerDisplayName = NULL;

        // If blind transfer
        {
            UtlString targetUrl;
            UtlString dummyFrom;
            buildFromToAddresses(dialString, targetCallId, callerDisplayName,
                dummyFrom, targetUrl);
            dialString = targetUrl;
            //alsoUri.append(";token1");
        }

        if(isMethodAllowed(SIP_REFER_METHOD))
        {
            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_INITIATED) ;

            mTargetCallConnectionAddress = dialString;
            mTargetCallId = targetCallId;

            // If the connection is not already on hold, do a hold
            // first and then do the REFER transfer
            if(mHoldState == TERMCONNECTION_TALKING || mHoldState == TERMCONNECTION_HOLDING)
            {
                mHoldCompleteAction = CpCallManager::CP_BLIND_TRANSFER;

                // need to do a remote hold first
                // Then after that is complete do the REFER
                if (mHoldState == TERMCONNECTION_TALKING)
                {
                    hold();
                }
                ret = TRUE;
            }
            else
            {
                // Send a REFER to tell the transferee to
                // complete a blind transfer
                doBlindRefer();

                ret = mIsReferSent;
            }
        }
        else
        {
            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
        }
    }
    return(ret);
}

void SipConnection::doBlindRefer()
{
    // Send a REFER message
    SipMessage referRequest;

    int iCSeq ;
    mCSeqMgr.startTransaction(CSEQ_ID_REFER, iCSeq) ;

    referRequest.setReferData(inviteMsg,
        inviteFromThisSide,
        iCSeq,
        mRouteField.data(),
        mLocalContact.data(),
        mRemoteContact.data(),
        mTargetCallConnectionAddress.data(),
        // The following keeps the target call id on the
        // transfer target the same as the consultative call
        //mTargetCallId);
        // The following does not set the call Id on the xfer target
        "");

    if (mLocationHeader.length() != 0)
    {
       referRequest.setLocationField(mLocationHeader.data());
    }

    mIsReferSent = send(referRequest);
}

UtlBoolean SipConnection::targetCallBlindTransfer(const char* dialString,
                                                  const char* transferControllerAddress)
{
    // This should never get here
    unimplemented("SipConnection::targetCallBlindTransfer");
    return(FALSE);
}

UtlBoolean SipConnection::transferControllerStatus(int connectionState, int response)
{
    // It should never get here
    unimplemented("SipConnection::transferControllerStatus");
    return(FALSE);
}

UtlBoolean SipConnection::transfereeStatus(int callState, int returnCode)
{
    UtlBoolean referResponseSent = FALSE;
    UtlString targetCallId ;
    UtlString callId ;
    UtlString remoteAddress ;

#ifdef TEST_PRINT
    osPrintf("SipConnection::transfereeStatus callType: %d referMessage: %x\n",
        mpCall->getCallType(), mReferMessage);
#endif


    // If this call and connection received a REFER request
    if(mpCall->getCallType() ==
        CpCall::CP_TRANSFEREE_ORIGINAL_CALL &&
        mReferMessage)
    {
        UtlString transferMethod;
        mReferMessage->getRequestMethod(&transferMethod);

        // REFER type transfer
        if(transferMethod.compareTo(SIP_REFER_METHOD) == 0)
        {
            int num;
            UtlString method;
            mReferMessage->getCSeqField(&num , &method);

            UtlString event;
            event.append(SIP_EVENT_REFER);
            event.append(";cseq=");
            char buff[50];
            sprintf(buff,"%d", num);
            event.append(buff);

            // Generate an appropriate NOTIFY message to indicate the
            // outcome
            SipMessage referNotify;

            HttpBody* body = NULL;
            int iCSeq ;
            mCSeqMgr.startTransaction(CSEQ_ID_NOTIFY, iCSeq) ;
            referNotify.setNotifyData(mReferMessage,
                iCSeq,
                mRouteField,
                NULL,
                event,
                "id");
            if(callState == CONNECTION_ESTABLISHED)
            {
                getCallId(&callId) ;
                mpCall->getTargetCallId(targetCallId) ;
                getRemoteAddress(&remoteAddress) ;

                CpMultiStringMessage joinMessage(CpCallManager::CP_TRANSFER_OTHER_PARTY_JOIN,
                        callId, remoteAddress, targetCallId) ;
                mpCallManager->postMessage(joinMessage);

                body = new HttpBody(SIP_REFER_SUCCESS_STATUS, -1, CONTENT_TYPE_MESSAGE_SIPFRAG);
            }
            else if(callState == CONNECTION_ALERTING)
            {
                SipMessage alertingMessage;
                switch(returnCode)
                {
                case SIP_EARLY_MEDIA_CODE:
                    alertingMessage.setResponseFirstHeaderLine(SIP_PROTOCOL_VERSION,
                        returnCode, SIP_RINGING_TEXT);
                    break;

                default:
                    alertingMessage.setResponseFirstHeaderLine(SIP_PROTOCOL_VERSION,
                        returnCode, SIP_RINGING_TEXT);
                    break;
                }
                UtlString messageBody;
                int len;
                alertingMessage.getBytes(&messageBody,&len);

                body = new HttpBody(messageBody.data(), -1, CONTENT_TYPE_MESSAGE_SIPFRAG);
            }
            else
            {
                getCallId(&callId) ;
                getRemoteAddress(&remoteAddress) ;

                CpMultiStringMessage unholdMessage(CpCallManager::CP_TRANSFER_OTHER_PARTY_UNHOLD, callId, remoteAddress) ;
                mpCallManager->postMessage(unholdMessage);

                body = new HttpBody(SIP_REFER_FAILURE_STATUS, -1, CONTENT_TYPE_MESSAGE_SIPFRAG);
            }
            referNotify.setBody(body);

            // Add the content type for the body
            referNotify.setContentType(CONTENT_TYPE_MESSAGE_SIPFRAG);

            // Add the content length
            int len;
            UtlString bodyString;
            body->getBytes(&bodyString, &len);
            referNotify.setContentLength(len);

            referResponseSent = send(referNotify);

            // Only delete if this is a final notify
            if(callState != CONNECTION_ALERTING && mReferMessage)
            {
                delete mReferMessage;
                mReferMessage = NULL;
            }
        }

        // Should be BYE Also type transfer
        else
        {
            SipMessage transferResponse;
            if(callState == CONNECTION_ESTABLISHED)
            {
                transferResponse.setOkResponseData(mReferMessage, mLocalContact);
            }
            else
            {
                transferResponse.setReferFailedData(mReferMessage);
            }
            referResponseSent = send(transferResponse);
            if(mReferMessage) delete mReferMessage;
            mReferMessage = NULL;
        }

    }

    else
    {
        osPrintf("SipConnection::transfereeStatus FAILED callType: %d mReferMessage: %p\n",
            mpCall->getCallType() , mReferMessage);
    }
    return(referResponseSent);
}

UtlBoolean SipConnection::doHangUp(const char* dialString,
                                   const char* callerId)
{
    int localState = getState(1);
    UtlBoolean hangUpOk = FALSE;
    UtlBoolean fireAudioStop = TRUE;

    if (!mDropping)
    {
        mDropping = true ;

        int cause;
        // always get remote connection state
        int currentState = getState(0, cause);
        const char* callerDisplayName = NULL;
        SipMessage sipRequest;
        UtlString alsoUri;

        // If blind transfer
        if(dialString && *dialString)
        {
            UtlString dummyFrom;
            buildFromToAddresses(dialString, callerId, callerDisplayName,
                dummyFrom, alsoUri);
        }

        // Tell the other end that we are hanging up
        // Need to send SIP CANCEL if we are the caller
        // and the callee connection state is not finalized
        if(mRemoteIsCallee &&
            currentState != CONNECTION_FAILED &&
            currentState != CONNECTION_ESTABLISHED &&
            currentState != CONNECTION_DISCONNECTED &&
            currentState != CONNECTION_UNKNOWN)
        {
            // We are the caller, cancel the incomplete call
            // Send a CANCEL
            //sipRequest = new SipMessage();

            // We are calling and the call is not setup yet so
            // cancel.  If we get a subsequent OK, we need to send
            // a BYE.
            if(inviteFromThisSide)
            {
                sipRequest.setCancelData(inviteMsg, mLocalContact.data());
                mLastRequestMethod = SIP_CANCEL_METHOD;

                // If this was a canceled transfer INVITE, send back a status
                if(!mOriginalCallConnectionAddress.isNull())
                {
                    UtlString originalCallId;
                    mpCall->getOriginalCallId(originalCallId);
                    CpMultiStringMessage transfereeStatus(CallManager::CP_TRANSFEREE_CONNECTION_STATUS,
                        originalCallId.data(),
                        mOriginalCallConnectionAddress.data(),
                        NULL, NULL, NULL,
                        CONNECTION_FAILED, SIP_REQUEST_TIMEOUT_CODE);
#ifdef TEST_PRINT
                    osPrintf("SipConnection::processResponse posting CP_TRANSFEREE_CONNECTION_STATUS to call: %s\n",
                        originalCallId.data());
#endif
                    mpCallManager->postMessage(transfereeStatus);

                }
            }
            else
            {
                // Someone is calling us and we are hanging up before the
                // call is setup.
                if (!mbByeAttempted)
                {
                    sipRequest.setInviteForbidden(inviteMsg);
                }
            }

            if(send(sipRequest))
            {
#ifdef TEST_PRINT

                if(inviteFromThisSide)
                    osPrintf("unsetup call CANCEL message sent\n");
                else
                    osPrintf("unsetup call BYE message sent\n");
#endif
                // Lets try not setting this to disconected until
                // we get the response or a timeout
                //setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_CANCELLED);

                // However we need something to indicate that the call
                // is being cancelled to handle a race condition when the callee responds with 200 OK
                // before it receives the Cancel.
                mbCancelling = TRUE;

                hangUpOk = TRUE;
            }
        }

        // We are the Caller or callee
        else if(currentState == CONNECTION_ESTABLISHED)
        {
            // the call is connected
            // Send a BYE
            //sipRequest = new SipMessage();
            //UtlString directoryServerUri;
            //if(!inviteFromThisSide)
            //{
            //UtlString dirAddress;
            //UtlString dirProtocol;
            //int dirPort;

            //sipUserAgent->getDirectoryServer(0, &dirAddress,
            //        &dirPort, &dirProtocol);
            //SipMessage::buildSipUrl(&directoryServerUri,
            //    dirAddress.data(), dirPort, dirProtocol.data());
            //}
#ifdef TEST_PRINT
            osPrintf("setup call BYE route: %s remote contact: %s\n",
                mRouteField.data(), mRemoteContact.data());
#endif
            if (localState == CONNECTION_ESTABLISHED)
            {
                int iCSeq ;
                mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
                sipRequest.setByeData(inviteMsg,
                    mRemoteContact,
                    inviteFromThisSide,
                    iCSeq,
                    mRouteField.data(),
                    alsoUri.data(),
                    mLocalContact.data());
            }
            else
            {
                sipRequest.setInviteForbidden(inviteMsg);

                // Set the state here because we are sending a response, not a BYE
                // and won't get back an OK
                setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE);
                fireAudioStop = FALSE;
            }

            mLastRequestMethod = SIP_BYE_METHOD;
            if (!mbByeAttempted)
            {
                if (!send(sipRequest))
                {
                    // If sending the BYE failed, we will receive no response to end
                    // the connection.  So we have to generate it here, as we cannot
                    // allow the inability to send a BYE to make it impossible to
                    // disconnect a call.
                    OsSysLog::add(FAC_CP, PRI_DEBUG,
                        "SipConnection::doHangUp: Sending BYE failed.  "
                        "Terminating connection.");
                    setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE);
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL);
                }
                else
                {
                    mbByeAttempted = true;
                }

                if (!mVoiceQualityReportTarget.isNull())
                    sendVoiceQualityReport(mVoiceQualityReportTarget) ;
            }
            hangUpOk = TRUE;
        }

        if (mpMediaInterface != NULL && mConnectionId >= 0)
        {
            mpMediaInterface->stopRtpSend(mConnectionId);
            mpMediaInterface->stopRtpReceive(mConnectionId);
            if (fireAudioStop)
            {
                fireAudioStopEvents() ;
            }
            mpMediaInterface->deleteConnection(mConnectionId);
            mpMediaInterface = NULL ;
            mConnectionId = CpMediaInterface::getInvalidConnectionId();
        }
    }

    return(hangUpOk);
}

void SipConnection::buildFromToAddresses(const char* dialString,
                                         const char* callerId,
                                         const char* callerDisplayName,
                                         UtlString& fromAddress,
                                         UtlString& goodToAddress)
{
    UtlString sipAddress;
    int sipPort;
    UtlString sipProtocol;

    fromAddress.remove(0);
    goodToAddress.remove(0);

    // Build a From address
    sipUserAgent->getFromAddress(&sipAddress, &sipPort, &sipProtocol);
    SipMessage::buildSipUrl(&fromAddress, sipAddress.data(),
        sipPort, sipProtocol.data(), callerId, callerDisplayName,
        mFromTag.data());

    // Check the to Address
    UtlString toAddress;
    UtlString toProtocol;
    UtlString toUser;
    UtlString toUserLabel;

    int toPort;

#ifdef TEST_PRINT
    osPrintf("SipConnection::dial got dial string: \"%s\"\n",
        dialString);
#endif

    // Use the Url object to perserve parameters and display name
    Url toUrl(dialString);
    toUrl.getHostAddress(toAddress);

    //SipMessage::parseAddressFromUri(dialString, &toAddress, &toPort,
    //      &toProtocol, &toUser, &toUserLabel);
    if(toAddress.isNull())
    {
        sipUserAgent->getDirectoryServer(0, &toAddress,
            &toPort, &toProtocol);
#ifdef TEST_PRINT
        osPrintf("Got directory server: \"%s\"\n",
            toAddress.data());
#endif
        toUrl.setHostAddress(toAddress.data());
        toUrl.setHostPort(toPort);
        if(!toProtocol.isNull())
        {
            toUrl.setUrlParameter("transport", toProtocol.data());
        }
    }
    //SipMessage::buildSipUrl(&goodToAddress, toAddress.data(),
    //              toPort, toProtocol.data(), toUser.data(),
    //              toUserLabel.data());
    toUrl.toString(goodToAddress);
//    mToUrl = toUrl;
}

UtlBoolean SipConnection::processMessage(OsMsg& eventMessage)
{
    int msgType = eventMessage.getMsgType();
    int msgSubType = eventMessage.getMsgSubType();
    UtlBoolean processedOk = TRUE;
    const SipMessage* sipMsg = NULL;
    int messageType;

    if(msgType == OsMsg::PHONE_APP &&
        msgSubType == CallManager::CP_SIP_MESSAGE)
    {
        sipMsg = ((SipMessageEvent&)eventMessage).getMessage();
        messageType = ((SipMessageEvent&)eventMessage).getMessageStatus();
#ifdef TEST_PRINT
        OsSysLog::add(FAC_CP, PRI_DEBUG,
            "SipConnection::messageType: %d", messageType);
#endif
        UtlBoolean messageIsResponse = sipMsg->isResponse();
        UtlString method;
        int sequenceNum;
        if(!messageIsResponse)
        {
            sipMsg->getRequestMethod(&method);
        }
        else
        {
            sipMsg->getCSeqField(&sequenceNum, &method);
        }

        // This is a request which failed to get sent
        if(messageType == SipMessageEvent::TRANSPORT_ERROR)
        {
#ifdef TEST_PRINT
            osPrintf("Processing message transport error method: %s\n",
                messageIsResponse ? method.data() : "response");
#endif
            if(!inviteMsg)
            {
#ifdef TEST_PRINT
                osPrintf("SipConnection::processMessage failed response\n");
#endif
                // THis call was not setup (i.e. did not try to sent an
                // invite and we did not receive one.  This is a bad call
                setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_DEST_NOT_OBTAINABLE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_ADDRESS) ;
            }
            // We only care about INVITE.
            // BYE, CANCLE and ACK are someone else's problem.
            // REGISTER and OPTIONS are handled else where
            else if(sipMsg->isSameMessage(inviteMsg) &&
                getState() == CONNECTION_OFFERING)
            {
#ifdef TEST_PRINT
                osPrintf("No response to INVITE\n");
#endif
                setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_DEST_NOT_OBTAINABLE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_ADDRESS) ;

#ifdef TEST_PRINT
                osPrintf("SipConnection::processMessage originalConnectionAddress: %s connection state: CONNECTION_FAILED transport failed\n",
                    mOriginalCallConnectionAddress.data());
#endif

                // If this was a failed transfer INVITE, send back a status
                if(!mOriginalCallConnectionAddress.isNull())
                {
                    UtlString originalCallId;
                    mpCall->getOriginalCallId(originalCallId);
                    CpMultiStringMessage transfereeStatus(CallManager::CP_TRANSFEREE_CONNECTION_STATUS,
                        originalCallId.data(),
                        mOriginalCallConnectionAddress.data(),
                        NULL, NULL, NULL,
                        CONNECTION_FAILED, SIP_REQUEST_TIMEOUT_CODE);
#ifdef TEST_PRINT
                    osPrintf("SipConnection::processResponse posting CP_TRANSFEREE_CONNECTION_STATUS to call: %s\n",
                        originalCallId.data());
#endif
                    mpCallManager->postMessage(transfereeStatus);

                }

            }

            // We did not get a response to the session timer
            // re-invite, so terminate the connection
            else if(sipMsg->isSameMessage(inviteMsg) &&
                getState() == CONNECTION_ESTABLISHED &&
                reinviteState == REINVITING &&
                mSessionReinviteTimer > 0)
            {
#ifdef TEST_PRINT
                osPrintf("SipConnection::processMessage failed session timer request\n");
#endif
                hangUp();
            }

            // A BYE or CANCEL failed to get sent
            else if(!messageIsResponse &&
                (method.compareTo(SIP_BYE_METHOD) == 0 ||
                method.compareTo(SIP_CANCEL_METHOD) == 0))
            {
#ifdef TEST_PRINT
                osPrintf("SipConnection::processMessage failed BYE\n");
#endif
                setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_DEST_NOT_OBTAINABLE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NETWORK) ;
            }

            // We we fail to send a response to the invite, (e.g. ACK not
            // received) then fail out the call
            else if (messageIsResponse && inviteMsg)
            {
                int responseCseq ;
                UtlString responseMethod ;
                int inviteCseq ;
                UtlString inviteMethod ;
                sipMsg->getCSeqField(&responseCseq, &responseMethod) ;
                inviteMsg->getCSeqField(&inviteCseq, &inviteMethod) ;

                if ( (responseMethod.compareTo(inviteMethod) == 0) &&
                      (responseCseq == inviteCseq))
                {
                    setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_DEST_NOT_OBTAINABLE);
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NETWORK) ;
                }
                else
                {
                    if (reinviteState == REINVITING)
                    {
                        reinviteState = ACCEPT_INVITE;
                    }
                }
            }
            else
            {
                if (reinviteState == REINVITING)
                    reinviteState = ACCEPT_INVITE;
#ifdef TEST_PRINT
                osPrintf("SipConnection::processMessage unhandled failed message\n");
#endif
            }

            processedOk = TRUE;
        }

        // Session timer is about to expire send a Re-INVITE to
        // keep the session going
        else if(messageType == SipMessageEvent::SESSION_REINVITE_TIMER)
        {
            extendSessionReinvite();
        }

        // The response was blocked by the user agent authentication
        // This message is only to keep in sync. with the sequence number
        else if(messageType == SipMessageEvent::AUTHENTICATION_RETRY)
        {
            if(sipMsg->isResponse())
            {
                UtlString cseqId(method);
                if(method.compareTo(SIP_BYE_METHOD) == 0 ||
                   method.compareTo(SIP_CANCEL_METHOD) == 0)
                {
                    cseqId = CSEQ_ID_INVITE;
                }

                // If this was the INVITE we need to update the
                // cached invite so that its cseq is up to date
                if(inviteMsg && sipMsg->isResponseTo(inviteMsg))
                {
                    int iCSeq ;
                    mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;

                    inviteMsg->setCSeqField(iCSeq,
                        SIP_INVITE_METHOD);
                }
                // BYE, INFO, NOTIFY, OPTIONS and REFER can get challenged for authentication also.
                // Need to update cseq for them as well to account for resend with incremented cseq.
                else if(method.compareTo(SIP_INVITE_METHOD) &&
                        sequenceNum == mCSeqMgr.getCSeqNumber(cseqId))
                {
                    mCSeqMgr.endTransaction(cseqId);
                    int newCseq;
                    mCSeqMgr.startTransaction(cseqId, newCseq);
                    if(sequenceNum + 1 != newCseq)
                    {
                        OsSysLog::add(FAC_CP, PRI_ERR,
                            "SipConnection conflicting CSeq for auth. retry %s request retry CSeq: %d CSeqMgr CSeq: %d",
                            method.data(), sequenceNum + 1, newCseq);
                    }
                    else
                    {
                        OsSysLog::add(FAC_CP, PRI_DEBUG,
                            "SipConnection updated CSeqMgr for auth retry %s new CSeq: %d",
                            method.data(), newCseq);
                    }
                }
                else
                {
                    OsSysLog::add(FAC_CP, PRI_ERR,
                        "SipConnection auth retry for %s CSeq: %d does not match outstanding transaction CSeq: %d",
                        method.data(), sequenceNum, mCSeqMgr.getCSeqNumber(cseqId));
                }

                // If this was the INVITE we need to update the
                // cached invite so that its cseq is up to date

            }
#ifdef TEST_PRINT
            else
            {
                osPrintf("SipConnection::processMessage request with AUTHENTICATION_RETRY\n");
            }
#endif
        }


        else if(sipMsg->isResponse())
        {
#ifdef TEST_PRINT
            ((SipMessage*)sipMsg)->logTimeEvent("PROCESSING");
#endif
            processedOk = processResponse(sipMsg);
            //numCodecs, rtpCodecs);
        }
        else
        {
#ifdef TEST_PRINT
            ((SipMessage*)sipMsg)->logTimeEvent("PROCESSING");
#endif
            processedOk = processRequest(sipMsg);
            //numCodecs, rtpCodecs);
        }

#ifdef TEST_PRINT
        sipMsg->dumpTimeLog();
#endif

    }
    else
    {
        processedOk = FALSE;
    }

    return(processedOk);
}

UtlBoolean SipConnection::extendSessionReinvite()
{
    UtlBoolean messageSent = FALSE;

    if(inviteMsg && getState() == CONNECTION_ESTABLISHED)
    {
        if (!mCSeqMgr.isInTransaction(CSEQ_ID_INVITE))
        {
            SipMessage reinvite(*inviteMsg);

            // Up the sequence number and resend
            int iCSeq ;
            mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
            reinvite.setCSeqField(iCSeq, SIP_INVITE_METHOD);

            // Reset the transport states
            reinvite.resetTransport();
            reinvite.removeLastVia();

            //remove all routes
            UtlString route;
            while ( reinvite.removeRouteUri(0 , &route)){}

            if ( !mRouteField.isNull())
            {
                //set correct route
                reinvite.setRouteField(mRouteField);
            }
            if (mLocationHeader.length() != 0)
            {
                reinvite.setLocationField(mLocationHeader.data());
            }

            messageSent = send(reinvite);
            delete inviteMsg;
            inviteMsg = new SipMessage(reinvite);

            // Disallow the other side from ReINVITing until this
            // transaction is complete.
            if(messageSent)
                reinviteState = REINVITING;
#ifdef TEST_PRINT
            osPrintf("Session Timer ReINVITE reinviteState: %d\n",
                reinviteState);
#endif
        }
        else
        {
            SipMessageEvent* sipMsgEvent =
                    new SipMessageEvent(new SipMessage(*inviteMsg),
                    SipMessageEvent::SESSION_REINVITE_TIMER);

            OsTimer* timer = new OsTimer((mpCallManager->getMessageQueue()),
                                         (intptr_t)sipMsgEvent);

            OsTime timerTime(3, 0);
            timer->oneshotAfter(timerTime);
        }
    }
    else if(inviteMsg == NULL &&
        getState() == CONNECTION_IDLE)
    {
        // A stray timer expired and the call does not exist.

        setState(CONNECTION_FAILED, CONNECTION_REMOTE);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_RESOURCE_LIMIT) ;
    }

    return(messageSent);
}

UtlBoolean SipConnection::processRequest(const SipMessage* request)
{
    UtlString sipMethod;
    UtlBoolean processedOk = TRUE;
    request->getRequestMethod(&sipMethod);

#ifdef TEST_PRINT
    int requestSequenceNum = 0;
    UtlString requestSeqMethod;
    request->getCSeqField(&requestSequenceNum, &requestSeqMethod);

    OsSysLog::add(FAC_CP, PRI_DEBUG, 
        "SipConnection::processRequest inviteMsg: %x requestSequenceNum: %d lastRemoteSequenceNumber: %d connectionState: %d reinviteState: %d",
        inviteMsg, requestSequenceNum, lastRemoteSequenceNumber,
        getState(), reinviteState);
#endif

    // INVITE
    // We are being hailed
    if(strcmp(sipMethod.data(),SIP_INVITE_METHOD) == 0)
    {
#ifdef TEST_PRINT
        osPrintf("%s in INVITE case\n", getName().data());
#endif
        processInviteRequest(request);
    }

    // SIP REFER received (transfer)
    else if(strcmp(sipMethod.data(),SIP_REFER_METHOD) == 0)
    {
#ifdef TEST_PRINT
        osPrintf("SIP REFER method received\n");
#endif
        processReferRequest(request);
    }

    // SIP ACK received
    else if(strcmp(sipMethod.data(),SIP_ACK_METHOD) == 0)
    {
#ifdef TEST_PRINT
        osPrintf("%s SIP ACK method received\n", getName().data());
#endif
        processAckRequest(request);
    }

    // BYE
    // The call is being shutdown
    else if(strcmp(sipMethod.data(), SIP_BYE_METHOD)  == 0)
    {
#ifdef TEST_PRINT
        OsSysLog::add(FAC_CP, PRI_DEBUG, "%s %s method received to close down call",
            getName().data(), sipMethod.data());
#endif
        processByeRequest(request);
    }

    // CANCEL
    // The call is being shutdown
    else if(strcmp(sipMethod.data(), SIP_CANCEL_METHOD) == 0)
    {
#ifdef TEST_PRINT
        osPrintf("%s %s method received to close down call\n",
            getName().data(), sipMethod.data());
#endif
        processCancelRequest(request);
    }

    // NOTIFY
    else if(strcmp(sipMethod.data(), SIP_NOTIFY_METHOD) == 0)
    {
#ifdef TEST_PRINT
        osPrintf("%s method received\n",
            sipMethod.data());
#endif
        processNotifyRequest(request);
    }

    else
    {
#ifdef TEST_PRINT
        osPrintf("SipConnection::processRequest %s method NOT HANDLED\n",
            sipMethod.data());
#endif
    }

    return(processedOk);
}


void SipConnection::processInviteRequestBadTransaction(const SipMessage* request, int tag)
{
    SipMessage sipResponse;
    sipResponse.setBadTransactionData(request);

    if(tag >= 0)
    {
        sipResponse.setToFieldTag(tag);
    }

    send(sipResponse);
}

void SipConnection::processInviteRequestLoop(const SipMessage* request, int tag)
{
    UtlString viaField;
    inviteMsg->getViaField(&viaField, 0);
    UtlString oldInviteBranchId;
    SipMessage::getViaTag(viaField.data(),
            "branch",
            oldInviteBranchId);

    request->getViaField(&viaField, 0);
    UtlString newInviteBranchId;
    SipMessage::getViaTag(viaField.data(),
            "branch",
            newInviteBranchId);

    // from a different branch
    if(!oldInviteBranchId.isNull() &&
            oldInviteBranchId.compareTo(newInviteBranchId) != 0)
    {
        SipMessage sipResponse;
        sipResponse.setLoopDetectedData(request);
        if(tag >= 0)
        {
            sipResponse.setToFieldTag(tag);
        }

        send(sipResponse);
    }
    else
    {
        // no-op, ignore duplicate INVITE
        OsSysLog::add(FAC_SIP, PRI_WARNING,
            "SipConnection::processInviteRequest received duplicate request");
    }
}

void SipConnection::processInviteRequestBadRefer(const SipMessage* request, int tag)
{
    SipMessage badTransaction;
    badTransaction.setBadTransactionData(request);
    if(tag >= 0)
    {
        badTransaction.setToFieldTag(tag);
    }

    send(badTransaction);

    // Bug 3658: as transfer target, call waiting disabled, transferee comes via proxy
    // manager, sequence of messages is 2 INVITEs from proxy, 486 then this 481.
    // When going from IDLE to DISCONNECTED, set the dropping flag so call will get
    // cleaned up
    mpCall->setDropState(TRUE);
    setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE);
    setState(CONNECTION_DISCONNECTED, CONNECTION_LOCAL);
    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_REFER) ;

}

void SipConnection::processInviteRequestOffering(const SipMessage* request,
                                                 int               tag,
                                                 UtlBoolean        doesReplaceCallLegExist,
                                                 int               replaceCallLegState,
                                                 UtlString&        replaceCallId,
                                                 UtlString&        replaceToTag,
                                                 UtlString&        replaceFromTag)
{
    UtlString callId ;

    getCallId(&callId) ;
    request->getCSeqField(&lastRemoteSequenceNumber, NULL) ;

    // Save a copy of the INVITE
    inviteMsg = new SipMessage(*request);
    inviteFromThisSide = FALSE;
    setCallerId();

    // Set the to tag if it is not set in the Invite
    if(tag >= 0)
    {
        inviteMsg->setToFieldTag(tag);

        // Update the cached from field after saving the tag
        inviteMsg->getToUrl(mFromUrl);
    }

    // Save line Id
    UtlString uri;
    request->getRequestUri(&uri);
    // Convert the URI to name-addr format.
    Url parsedUri(uri, TRUE);
    // Store into mLocalContact, which is in name-addr format.
    parsedUri.toString(mLocalContact);

    int cause = CONNECTION_CAUSE_NORMAL;

    // Replaces is independent of REFER so
    // do not assume there must be a Refer-To or Refer-By
    // Assume the replaces call leg does not exist if the call left
    // state state is not established.  The latest RFC states that
    // one cannot use replaces for an early dialog
    if (doesReplaceCallLegExist &&
        (replaceCallLegState != CONNECTION_ESTABLISHED))
    {
        doesReplaceCallLegExist = FALSE ;
    }


    // Allow transfer if the call leg exists and the call leg is
    // established.  Transferring a call while in early dialog is
    // illegal.
    if (doesReplaceCallLegExist)
    {
        cause = CONNECTION_CAUSE_TRANSFER;

        // Setup the meta event data
        int metaEventId = mpCallManager->getNewMetaEventId();
        const char* metaEventCallIds[2];
        metaEventCallIds[0] = callId.data();        // target call Id
        metaEventCallIds[1] = replaceCallId.data(); // original call Id
        mpCall->startMetaEvent(metaEventId,
            PtEvent::META_CALL_TRANSFERRING,
            2, metaEventCallIds);
        mpCall->setCallType(CpCall::CP_TRANSFER_TARGET_TARGET_CALL);

        fireSipXCallEvent(CALLSTATE_NEWCALL, CALLSTATE_CAUSE_TRANSFERRED, (void*) replaceCallId.data()) ;
    }
    else
    {
        // This call does not contain a REPLACES header, however, it
        // may still be part of a blind transfer, so look for the
        // referred-by or requested-by headers
        UtlString referredBy;
        UtlString requestedBy;
        request->getReferredByField(referredBy);
        request->getRequestedByField(requestedBy);
        if (!referredBy.isNull() || !requestedBy.isNull())
        {
            cause = CONNECTION_CAUSE_TRANSFER;
            mpCall->setCallType(CpCall::CP_TRANSFER_TARGET_TARGET_CALL);
        }
        mpCall->startMetaEvent( mpCallManager->getNewMetaEventId(),
            PtEvent::META_CALL_STARTING,
            0,
            0,
            mRemoteIsCallee);

        if (!mRemoteIsCallee)   // inbound call
        {
            mpCall->setCallState(mResponseCode, mResponseText, PtCall::ACTIVE);
            setState(CONNECTION_ESTABLISHED, CONNECTION_REMOTE, PtEvent::CAUSE_NEW_CALL);
            setState(CONNECTION_INITIATED, CONNECTION_LOCAL, PtEvent::CAUSE_NEW_CALL);
            fireSipXCallEvent(CALLSTATE_NEWCALL, CALLSTATE_CAUSE_NORMAL) ;
        }
    }


    // If this is not part of a call leg replaces operation
    // we normally go to offering so that the application
    // can decide to accept, reject or redirect
    if(!doesReplaceCallLegExist)
    {
        setState(CONNECTION_OFFERING, CONNECTION_LOCAL, cause);
        fireSipXCallEvent(CALLSTATE_OFFERING, CALLSTATE_CAUSE_NORMAL) ;

    }

    // If we are replacing a call let answer the call
    // immediately do not go to offering first.
    if(doesReplaceCallLegExist)
    {
        if (((CpPeerCall*)mpCall)->isConnectionLocallyInitiatedRemoteHold(replaceCallId.data(),
                replaceToTag.data(),
                replaceFromTag.data()))
        {
            mbLocallyInitiatedRemoteHold = true ;
        }

        // Go immediately to answer the call
        answer();

        // Bob 11/16/01: The following setState was added to close a race between
        // the answer (above) and hangup (below).  The application layer is notified
        // of state changed on on the replies to these messages.  These can lead to
        // dropped transfer if the BYE response is received before INVITE response.
        setState(CONNECTION_ESTABLISHED, CONNECTION_REMOTE, CONNECTION_CAUSE_TRANSFER);

        // Drop the leg to be replaced
        ((CpPeerCall*)mpCall)->hangUp(replaceCallId.data(),
                replaceToTag.data(),
                replaceFromTag.data());
    }
    else if(mOfferingDelay == IMMEDIATE)
    {
        accept(mForwardOnNoAnswerSeconds);
    }
    else
    {
        // If the delay is not forever, setup a timer to expire
        if(mOfferingDelay > IMMEDIATE)
        {
            setOfferingTimer(mOfferingDelay);
        }
    }
}

void SipConnection::processInviteRequestReinvite(const SipMessage* request, int tag)
{
    // Keep track of the last sequence number;
    request->getCSeqField(&lastRemoteSequenceNumber, NULL);

    // Do not allow other Requests until the ReINVITE is complete
    reinviteState = REINVITED;

    UtlString hostAddresses[MAX_ADDRESS_CANDIDATES];
    int receiveRtpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveRtcpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveVideoRtpPorts[MAX_ADDRESS_CANDIDATES];
    int receiveVideoRtcpPorts[MAX_ADDRESS_CANDIDATES];
    RTP_TRANSPORT transportTypes[MAX_ADDRESS_CANDIDATES];
    int numAddresses = 0;
    int totalBandwidth = 0;
    int matchingBandwidth = 0;
    int videoFramerate = 0;
    int matchingVideoFramerate = 0;
    SdpCodecList supportedCodecs;
    SdpSrtpParameters srtpParams;
    SdpSrtpParameters matchingSrtpParams;
    memset(&srtpParams, 0, sizeof(srtpParams));
    memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));


    if (mpMediaInterface)
    {
        mpMediaInterface->getCapabilitiesEx(mConnectionId,
            MAX_ADDRESS_CANDIDATES,
            hostAddresses,
            receiveRtpPorts,
            receiveRtcpPorts,
            receiveVideoRtpPorts,
            receiveVideoRtcpPorts,
            transportTypes,
            numAddresses,
            supportedCodecs,
            srtpParams,
            mBandwidthId,
            totalBandwidth,
            videoFramerate);
    }

    // If we were originally initiated the hold, then don't allow the far
    // end to take us off hold
    if (mbLocallyInitiatedRemoteHold)
    {
        hostAddresses[0] = "0.0.0.0" ;
        numAddresses = 1 ;
    }

    int numEncoderCodecs = 0;
    SdpCodec** encoderCodecs = NULL;
    int numDecoderCodecs = 0;
    SdpCodec** decoderCodecs = NULL;

    // Make a copy of the codec that we offered as we must be prepared to receive all
    // of them
    supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

    // Get the RTP info from the message if present
    // Should check the content type first
    if(mpMediaInterface && getInitialSdpCodecs(request,
                                               supportedCodecs,
                                               numEncoderCodecs,
                                               encoderCodecs,
                                               remoteRtpAddress,
                                               remoteRtpPort,
                                               remoteRtcpPort,
                                               remoteVideoRtpPort,
                                               remoteVideoRtcpPort,
                                               srtpParams,
                                               matchingSrtpParams,
                                               totalBandwidth,
                                               matchingBandwidth,
                                               videoFramerate,
                                               matchingVideoFramerate))
    {
        fireIncompatibleCodecsEvent(&supportedCodecs, encoderCodecs, numEncoderCodecs) ;

        if (matchingBandwidth != 0)
        {
            mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
        }
        if (matchingVideoFramerate != 0)
        {
            mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
        }
        // If the codecs match send an OK
        if(numEncoderCodecs > 0)
        {
            // Setup media channel
            setMediaDestination(remoteRtpAddress.data(),
                remoteRtpPort,
                remoteRtcpPort,
                remoteVideoRtpPort,
                remoteVideoRtcpPort,
                request->getSdpBody(mpSecurity));

            // Far side requested hold
            if(remoteRtpPort == 0 ||
                remoteRtpAddress.compareTo("0.0.0.0") == 0)
            {
                mpMediaInterface->stopRtpReceive(mConnectionId);
                mpMediaInterface->stopRtpSend(mConnectionId);
                fireAudioStopEvents(MEDIA_CAUSE_HOLD) ;

                if (mpCall->isInFocus())
                {
                    fireSipXCallEvent(CALLSTATE_REMOTE_HELD, CALLSTATE_CAUSE_NORMAL);
                }
                else
                {
                    fireSipXCallEvent(CALLSTATE_HELD, CALLSTATE_CAUSE_NORMAL);
                }
                mHoldState = TERMCONNECTION_HELD ;
                mRemoteRequestedHold = TRUE;
            }
            else if(remoteRtpPort > 0)
            {
                if(receiveRtpPorts[0] <= 0 || hostAddresses[0].compareTo("0.0.0.0") == 0)
                {
                    // This event should be a NOP -- we should have already
                    // sent an CALLSTATE_AUDIO_STOP when we placed the call
                    // on hold originally.
                    mpMediaInterface->stopRtpReceive(mConnectionId);
                    mpMediaInterface->stopRtpSend(mConnectionId);
                    fireAudioStopEvents(MEDIA_CAUSE_HOLD) ;

                    // We are refusing to come off hold
                    if (mpCall->isInFocus())
                    {
                        fireSipXCallEvent(CALLSTATE_REMOTE_HELD, CALLSTATE_CAUSE_NORMAL);
                    }
                    else
                    {
                        fireSipXCallEvent(CALLSTATE_HELD, CALLSTATE_CAUSE_NORMAL);
                    }
                    mHoldState = TERMCONNECTION_HELD ;
                }
                else
                {
                    // ReINVITE recieved, if it has SDP, we should copy and use its payload IDs
                    // Allow unhold
                    mpMediaInterface->startRtpReceive(mConnectionId,
                        numDecoderCodecs, decoderCodecs);
                    fireAudioStartEvents(MEDIA_CAUSE_UNHOLD) ;

                    // TODO: reINVITE, should also check to see if remote streams are actively receiving

                    mpMediaInterface->startRtpSend(mConnectionId,
                        numEncoderCodecs, encoderCodecs);
                    mRemoteRequestedHold = FALSE;


                    mHoldState = TERMCONNECTION_TALKING ;

                    // Fire a CALLSTATE_CAUSE_NORMAL event for remote side taking
                    // us off hold
                    if (mpCall->isInFocus())
                    {
                        fireSipXCallEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_NORMAL);
                    }
                    else
                    {
                        fireSipXCallEvent(CALLSTATE_BRIDGED, CALLSTATE_CAUSE_NORMAL);
                    }

                    fireAudioStartEvents(MEDIA_CAUSE_UNHOLD) ;
                }
            }

            // Send a INVITE OK response
            if(remoteRtpPort <= 0 ||
                remoteRtpAddress.compareTo("0.0.0.0") == 0)
            {
                hostAddresses[0] = "0.0.0.0" ;
            }

            SipMessage sipResponse;
            sipResponse.setSecurityAttributes(mpSecurity);
            sipResponse.setInviteOkData(request,
                    mDefaultSessionReinviteTimer, mLocalContact.data());
            if (mLocationHeader.length() !=0)
            {
                sipResponse.setLocationField(mLocationHeader.data());
            }
            if(tag >= 0)
            {
                sipResponse.setToFieldTag(tag);
            }

            // Save the invite for future reference
            if(inviteMsg)
            {
                delete inviteMsg;
            }
            inviteMsg = new SipMessage(*request);
            inviteFromThisSide = FALSE;
            setCallerId();

            if (strcmp(hostAddresses[0], "0.0.0.0") == 0)
            {
                sipResponse.addSdpBody(1, hostAddresses, receiveRtpPorts,
                        receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                        transportTypes,
                        numDecoderCodecs, decoderCodecs, &matchingSrtpParams,
                        totalBandwidth, matchingVideoFramerate, inviteMsg) ;
            }
            else
            {
                if (!mpCallManager->isIceEnabled())
                {
                    numAddresses = 1 ;
                }
                sipResponse.addSdpBody(numAddresses, hostAddresses, receiveRtpPorts,
                        receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                        transportTypes,
                        numDecoderCodecs, decoderCodecs, &matchingSrtpParams,
                        totalBandwidth, matchingVideoFramerate, inviteMsg) ;
            }

            prepareInviteSdpForSend(&sipResponse, mConnectionId, mpSecurity) ;
            send(sipResponse);

            if(tag >= 0)
            {
                inviteMsg->setToFieldTag(tag);

                // Update the cached from field after saving the tag
                inviteMsg->getToUrl(mFromUrl);
            }

        }
        // If the codecs do not match send an error
        else
        {
            // No common codecs send INVITE error response
            SipMessage sipResponse;
            sipResponse.setInviteBadCodecs(request, sipUserAgent);
            if(tag >= 0)
            {
                sipResponse.setToFieldTag(tag);
            }
            send(sipResponse);
        }
    }
    else
    {
        SipMessage sipResponse;
        sipResponse.setSecurityAttributes(mpSecurity);
        sipResponse.setInviteOkData(request,
                mDefaultSessionReinviteTimer, mLocalContact.data());
        if(tag >= 0)
        {
            sipResponse.setToFieldTag(tag);
        }
        if (mLocationHeader.length() !=0)
        {
            sipResponse.setLocationField(mLocationHeader.data());
        }

        if (strcmp(hostAddresses[0], "0.0.0.0") == 0)
        {
            sipResponse.addSdpBody(1, hostAddresses, receiveRtpPorts,
                    receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                    transportTypes,
                    numDecoderCodecs, decoderCodecs, &srtpParams,
                    totalBandwidth, matchingVideoFramerate, inviteMsg) ;
            mHoldState = TERMCONNECTION_HELD ;
        }
        else
        {
            if (!mpCallManager->isIceEnabled())
            {
                numAddresses = 1 ;
            }
            sipResponse.addSdpBody(numAddresses, hostAddresses, receiveRtpPorts,
                    receiveRtcpPorts, receiveVideoRtpPorts, receiveVideoRtcpPorts,
                    transportTypes,
                    numDecoderCodecs, decoderCodecs, &srtpParams,
                    totalBandwidth, matchingVideoFramerate, inviteMsg) ;
            mHoldState = TERMCONNECTION_HELD ;
        }

        prepareInviteSdpForSend(&sipResponse, mConnectionId, mpSecurity) ;
        send(sipResponse);

        // Save the invite for future reference
        if(inviteMsg)
        {
            delete inviteMsg;
        }
        inviteMsg = new SipMessage(*request);
        inviteFromThisSide = FALSE;
        setCallerId();

        if(tag >= 0)
        {
            inviteMsg->setToFieldTag(tag);

            // Update the cached from field after saving the tag
            inviteMsg->getToUrl(mFromUrl);
        }
    }

    // Free up the codec copies and arrray
    SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
    SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);
}


void SipConnection::processInviteRequest(const SipMessage* request)
{
    UtlString sipMethod;
    UtlString callId;
    int requestSequenceNum = 0;
    int tagNum = -1;
    OsStatus createdConnection = OS_SUCCESS;
    const SdpBody* pSdpBody = request->getSdpBody(this->mpSecurity);

    // Store the remote User agent
    request->getUserAgentField(&mRemoteUserAgent);

    // Store the local address (used for multiple interface support)
    setLocalAddress(request->getLocalIp().data());

    // Record Call-Id
    getCallId(&callId);
    if(callId.isNull())
    {
        request->getCallIdField(&callId);
        setCallId(callId.data());
    }

    // Record the To Field tag
    UtlString toAddr;
    UtlString toProto;
    int toPort;
    UtlString tag;
    request->getToAddress(&toAddr, &toPort, &toProto, NULL,
            NULL, &tag);
    // The tag is not set, add it
    if(tag.isNull())
    {
        tagNum = sRandom->rand();
    }

    // Record Allow Field
    if(mAllowedRemote.isNull())
    {
        // Get the methods the other side supports
        request->getAllowField(mAllowedRemote);
    }

    // Record remote contact (may can change over time)
    UtlString contactInResponse;
    if (request->getContactUri(0 , &contactInResponse))
    {
        mContactUriStr = contactInResponse.data();
        mRemoteContact = contactInResponse;
    }

    // Get the route for subsequent requests
    request->buildRouteField(&mRouteField);

    /*
     * Allocate media resources
     */
    if (mpMediaInterface != NULL &&
        !mpMediaInterface->isConnectionIdValid(mConnectionId))
    {
        // Create a new connection in the flow graph
        // look at pSdpBody and determine rtpTransportOptions
        int rtpTransportFlags = RTP_TRANSPORT_UDP;

        if (pSdpBody)
        {
            bool bTcpAvailable = pSdpBody->isTransportAvailable(OsSocket::TCP, MEDIA_TYPE_AUDIO);
            bool bUdpAvailable = pSdpBody->isTransportAvailable(OsSocket::UDP, MEDIA_TYPE_AUDIO);
            if (!bUdpAvailable && bTcpAvailable)
            {
                rtpTransportFlags = RTP_TRANSPORT_TCP | mRtpTcpRole;
                mRtpTransport = RTP_TRANSPORT_TCP;
            }
        }
        createdConnection = mpMediaInterface->createConnection(
                mConnectionId,
                request->getLocalIp().data(),
                0,
                NULL /* VIDEO: WINDOW HANDLE */,
                mpSecurity,
                rtpTransportFlags);
    }

    if (createdConnection != OS_SUCCESS)
    {
        // If we can't create a connection -- dump the call
        SipMessage busyMessage;
        busyMessage.setInviteBusyData(request);
        send(busyMessage);

        setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_RESOURCES_NOT_AVAILABLE);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_RESOURCE_LIMIT) ;
    }
    else
    {
        UtlBoolean hasReplaceHeader = FALSE;
        UtlBoolean doesReplaceCallLegExist = FALSE;
        int       replaceCallLegState = -1 ;
        UtlString replaceCallId;
        UtlString replaceToTag;
        UtlString replaceFromTag;

        /**
         * Determine if this INVITE is part of a REFER
         */
        hasReplaceHeader = request->getReplacesData(replaceCallId,
                replaceToTag,
                replaceFromTag);
        if (hasReplaceHeader)
        {
            // Ugly assumption that this is a CpPeerCall
            doesReplaceCallLegExist =
                ((CpPeerCall*)mpCall)->getConnectionState(
                replaceCallId.data(), replaceToTag.data(),
                replaceFromTag.data(),
                replaceCallLegState,
                TRUE);

            if (doesReplaceCallLegExist && (replaceCallLegState != CONNECTION_ESTABLISHED))
            {
                doesReplaceCallLegExist = FALSE ;
            }
        }

        /*
         * Figure out what to do with the INVITE request
         */
        request->getCSeqField(&requestSequenceNum, NULL);
        if(inviteMsg && requestSequenceNum < lastRemoteSequenceNumber)
        {
            // Old Invite (resent)
            processInviteRequestBadTransaction(request, tagNum) ;
        }
        else if (inviteMsg && !inviteFromThisSide &&
                requestSequenceNum == lastRemoteSequenceNumber)
        {
            // A loop (we sent the INVITE)
            processInviteRequestLoop(request, tagNum) ;
        }
        else if (hasReplaceHeader && !doesReplaceCallLegExist)
        {
            // Bogus Refer Data
            processInviteRequestBadRefer(request, tagNum) ;
        }
        else if ((getState() == CONNECTION_IDLE) ||
            (getState() == CONNECTION_OFFERING &&
            mRemoteIsCallee && !request->isSameMessage(inviteMsg)))
        {
            // Standard Offering
            processInviteRequestOffering(
                    request,
                    tagNum,
                    doesReplaceCallLegExist,
                    replaceCallLegState,
                    replaceCallId,
                    replaceToTag,
                    replaceFromTag) ;

        }
        else if (reinviteState != ACCEPT_INVITE)
        {
            // We are busy: send 491 Request Pending
            SipMessage sipResponse;
            sipResponse.setRequestPendingData(request) ;
            if(tagNum >= 0)
            {
                sipResponse.setToFieldTag(tagNum);
            }
            send(sipResponse);
        }
        else if(inviteMsg &&
                requestSequenceNum > lastRemoteSequenceNumber &&
                getState() == CONNECTION_ESTABLISHED)
        {
            // Standard Re-invite
            processInviteRequestReinvite(request, tagNum) ;
        }
        else
        {
            // Special Cases ...
            if(inviteMsg && request->isSameMessage(inviteMsg))
            {
                // Special case: We sent the invite to our self
                SipMessage sipResponse;
                sipResponse.setInviteBusyData(request);
                if(tagNum >= 0)
                {
                    sipResponse.setToFieldTag(tagNum);
                }
                send(sipResponse);
                // NOTE: Events fired on response processing.
            }
        }
    }
}


void SipConnection::processReferRequest(const SipMessage* request)
{
    mIsAcceptSent = FALSE;

    UtlString referTo;
    UtlString referredBy;
    request->getReferredByField(referredBy);
    request->getReferToField(referTo);
    bool bTakeFocus ;

    //reject Refers to non sip URLs
    Url referToUrl(referTo);
    UtlString protocol;
    referToUrl.getUrlType(protocol);

    int connectionState = getState();
    // Cannot transfer if there is not already a call setup
    if(connectionState != CONNECTION_ESTABLISHED &&
        connectionState != CONNECTION_IDLE)
    {
        SipMessage sipResponse;
        sipResponse.setReferDeclinedData(request);
        send(sipResponse);
    }

    // If there is not exactly one Refered-By
    // or not exactly one Refer-To header
    // or there is already a REFER in progress
    else if(request->getHeaderValue(1, SIP_REFERRED_BY_FIELD) != NULL||
        request->getHeaderValue(1, SIP_REFER_TO_FIELD) != NULL ||
        mReferMessage)
    {
        SipMessage sipResponse;
        sipResponse.setRequestBadRequest(request);
        send(sipResponse);
    }

    //if Url is not of type Sip
    else if (protocol.index("SIP" , 0, UtlString::ignoreCase) != 0)
    {
        SipMessage sipResponse;
        sipResponse.setRequestBadUrlType(request);
        send(sipResponse);
    }
    // Give the transfer a try.
    else if(connectionState == CONNECTION_ESTABLISHED)
    {
        // Create a second call if it does not exist already
        // Set the target call id in this call
        // Set this call's type to transferee original call
        UtlString targetCallId;
        Url targetUrl(referTo);
        targetUrl.getHeaderParameter(SIP_CALLID_FIELD, targetCallId);
        // targetUrl.removeHeaderParameters();
        targetUrl.toString(referTo);
        //SipMessage::parseParameterFromUri(referTo.data(), "Call-ID",
        //    &targetCallId);
#ifdef TEST_PRINT
        osPrintf("SipConnection::processRequest REFER refer-to: %s callid: %s\n",
            referTo.data(), targetCallId.data());
#endif

        // Setup the meta event data
        int metaEventId = mpCallManager->getNewMetaEventId();
        const char* metaEventCallIds[2];
        UtlString thisCallId;
        getCallId(&thisCallId);
        metaEventCallIds[0] = targetCallId.data();
        metaEventCallIds[1] = thisCallId.data();

        // Mark the begining of a transfer meta event in this call
        mpCall->startMetaEvent(metaEventId,
            PtEvent::META_CALL_TRANSFERRING,
            2, metaEventCallIds);

        // If in focus, take out of focus and mark the state for the creation of
        // the new call
        if (mpCall->isInFocus())
        {
            CpIntMessage yieldFocus(CallManager::CP_YIELD_FOCUS, (intptr_t)mpCall);
            mpCallManager->postMessage(yieldFocus);
            bTakeFocus = true ;
        }
        else
        {
            bTakeFocus = false ;
        }
        // If this is part of a conference, hold other parties
        UtlString remoteAddress ;
        getRemoteAddress(&remoteAddress) ;
        CpMultiStringMessage holdOtherPartiesRequest(CpCallManager::CP_TRANSFER_OTHER_PARTY_HOLD,
                thisCallId, remoteAddress) ;
        mpCallManager->postMessage(holdOtherPartiesRequest) ;

        // The new call by default assumes focus.
        // Mark the new call as part of this transfer meta event
        mpCallManager->createCall(&targetCallId, metaEventId,
            PtEvent::META_CALL_TRANSFERRING, 2, metaEventCallIds, bTakeFocus);
        mpCall->setTargetCallId(targetCallId.data());
        mpCall->setCallType(CpCall::CP_TRANSFEREE_ORIGINAL_CALL);
        mpCallManager->setOutboundLineForCall(targetCallId, mLocalContact, mContactType) ;

        // Send a message to the target call to create the
        // connection and send the INVITE
        CpMultiStringMessage transfereeConnect(CallManager::CP_TRANSFEREE_CONNECTION,
            targetCallId.data(), referTo.data(), referredBy.data(), thisCallId.data(),
            remoteAddress.data(), mbLocallyInitiatedRemoteHold, mRtpTransport);

#ifdef TEST_PRINT
        osPrintf("SipConnection::processRequest posting CP_TRANSFEREE_CONNECTION\n");
#endif
        mpCallManager->postMessage(transfereeConnect);

        // Send an accepted response, a NOTIFY is sent later to
        // provide the resulting outcome
        SipMessage sipResponse;
        sipResponse.setResponseData(request, SIP_ACCEPTED_CODE,
            SIP_ACCEPTED_TEXT, mLocalContact);
        if (mLocationHeader.length() != 0)
        {
            sipResponse.setLocationField(mLocationHeader.data());
        }
        mIsAcceptSent = send(sipResponse);

        // Save a copy for the NOTIFY
        mReferMessage = new SipMessage(*request);

    }

    else if(connectionState == CONNECTION_IDLE)
    {
        // Set the identity of this connection
        request->getFromUrl(mToUrl);
        request->getToUrl(mFromUrl);
        UtlString callId;
        request->getCallIdField(&callId);
        setCallId(callId);
        UtlString fromField;
        mToUrl.toString(fromField);

        // Post a message to add a connection to this call
        CpMultiStringMessage transfereeConnect(CallManager::CP_TRANSFEREE_CONNECTION,
            callId.data(), referTo.data(),
            referredBy.data(), callId.data(), fromField.data(), mbLocallyInitiatedRemoteHold, mRtpTransport);
        mpCallManager->postMessage(transfereeConnect);

        // Assume focus, probably not the right thing
        //mpCallManager->unhold(callId.data());

        // Send back a response
        SipMessage referResponse;
        referResponse.setResponseData(request, SIP_ACCEPTED_CODE,
            SIP_ACCEPTED_TEXT, mLocalContact);
        if (mLocationHeader.length() !=0)
        {
            referResponse.setLocationField(mLocationHeader.data());
        }
        mIsAcceptSent = send(referResponse);

        // Save a copy for the NOTIFY
        mReferMessage = new SipMessage(*request);

        setState(CONNECTION_UNKNOWN, CONNECTION_REMOTE);
        /** SIPXTAPI: TBD **/
    }
} // end of processReferRequest

void SipConnection::processNotifyRequest(const SipMessage* request)
{
    UtlString eventType;
    request->getEventField(eventType);

    // IF this is a REFER result notification
    int refIndex = eventType.index(SIP_EVENT_REFER);
    if(refIndex >= 0)
    {
        UtlString contentType;
        request->getContentType(&contentType);
        const HttpBody* body = request->getBody();

        // If we have a body that contains the REFER status/outcome
        if(     body &&
            ( contentType.index(CONTENT_TYPE_SIP_APPLICATION, 0, UtlString::ignoreCase) == 0 ||
            contentType.index(CONTENT_TYPE_MESSAGE_SIPFRAG, 0, UtlString::ignoreCase) == 0) )
        {
            // Send a NOTIFY response, we like the content
            // Need to send this ASAP and before the BYE
            SipMessage notifyResponse;
            notifyResponse.setOkResponseData(request, mLocalContact);
            send(notifyResponse);

            const char* bytes;
            int numBytes;
            body->getBytes(&bytes, &numBytes);

            SipMessage response(bytes, numBytes);

            int state = -1 ;
            int cause = -1 ;
            int responseCode = response.getResponseStatusCode();
            mResponseCode = responseCode;
            response.getResponseStatusText(&mResponseText);

            if(responseCode == SIP_OK_CODE)
            {
                state = CONNECTION_ESTABLISHED;
                cause = CONNECTION_CAUSE_NORMAL;

                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_SUCCESS) ;
            }
            else if(responseCode == SIP_DECLINE_CODE)
            {
                state = CONNECTION_FAILED;
                cause = CONNECTION_CAUSE_CANCELLED;

                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
            }
            else if(responseCode == SIP_BAD_METHOD_CODE ||
                responseCode == SIP_UNIMPLEMENTED_METHOD_CODE)
            {
                state = CONNECTION_FAILED;
                cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;
                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
            }
            else if(responseCode == SIP_RINGING_CODE)
            {
                // Note: this is the state for the transferee
                // side of the connection between the transferee
                // and the transfer target (i.e. not the target
                // which is actually ringing)
                state = CONNECTION_OFFERING;
                cause = CONNECTION_CAUSE_NORMAL;
                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_RINGING) ;
            }
            else if(responseCode == SIP_EARLY_MEDIA_CODE)
            {
                // Note: this is the state for the transferee
                // side of the connection between the transferee
                // and the transfer target (i.e. not the target
                // which is actually ringing)
                state = CONNECTION_ESTABLISHED;
                cause = CONNECTION_CAUSE_UNKNOWN;

                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_RINGING) ;
            }
            else if (responseCode == SIP_SERVICE_UNAVAILABLE_CODE)
            {
                state = CONNECTION_FAILED;
                cause = CONNECTION_CAUSE_SERVICE_UNAVAILABLE;
                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
            }
            else if (responseCode >= SIP_4XX_CLASS_CODE)
            {
                state = CONNECTION_FAILED;
                cause = CONNECTION_CAUSE_BUSY;
                fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
            }
            else
            {
                // Ignore other provision response -- 3XX class response are
                // handled in the stack automatically.
            }

            if(responseCode >= SIP_OK_CODE)
            {
                // Signal the connection in the target call with the final status
                UtlString targetCallId;
                UtlString toField;
                mToUrl.toString(toField);
                mpCall->getTargetCallId(targetCallId);
                CpMultiStringMessage transferControllerStatus(CallManager::CP_TRANSFER_CONNECTION_STATUS,
                    targetCallId.data(), toField.data(),
                    NULL, NULL, NULL,
                    state, cause);
#ifdef TEST_PRINT
                osPrintf("SipConnection::processNotifyRequest posting CP_TRANSFER_CONNECTION_STATUS to call: %s\n",
                    targetCallId.data());
#endif
                mpCallManager->postMessage(transferControllerStatus);

                // Drop this connection, the transfer succeeded
                // Do the drop at the last possible momment so that
                // both calls have some overlap.
                if(responseCode == SIP_OK_CODE) doHangUp();
            }

#ifdef TEST_PRINT
            else
            {
                osPrintf("SipConnection::processNotifyRequest ignoring REFER response %d\n",
                    responseCode);
            }
#endif
        } // End if body in the NOTIFY

        // Unknown NOTIFY content type for this event type REFER
        else
        {
            // THis probably should be some sort of error response
            // Send a NOTIFY response
            SipMessage notifyResponse;
            notifyResponse.setOkResponseData(request, mLocalContact);
            send(notifyResponse);
        }

    } // End REFER NOTIFY
} // End of processNotifyRequest

void SipConnection::processAckRequest(const SipMessage* request)
{
    int requestSequenceNum = 0;
    UtlString requestSeqMethod;

    request->getCSeqField(&requestSequenceNum, &requestSeqMethod);

    // If this ACK belongs to the last INVITE and
    // we are accepting the INVITE
    if(mpMediaInterface != NULL && getState() == CONNECTION_ESTABLISHED &&
        (lastRemoteSequenceNumber == requestSequenceNum || mIsAcceptSent))
    {
        // Record remote contact (may can change over time)
        UtlString contactInResponse;
        if (request->getContactUri(0 , &contactInResponse))
        {
            mContactUriStr = contactInResponse.data();
            mRemoteContact = contactInResponse;
        }

        UtlString rtpAddress;
        int receiveRtpPort;
        int receiveRtcpPort;
        int receiveVideoRtpPort;
        int receiveVideoRtcpPort;
        int totalBandwidth = 0;
        int matchingBandwidth = 0;
        int videoFramerate = 0;
        int matchingVideoFramerate = 0;
        SdpCodecList supportedCodecs;
        SdpSrtpParameters srtpParams;
        memset(&srtpParams, 0, sizeof(srtpParams));

        mpMediaInterface->getCapabilities(mConnectionId,
            rtpAddress,
            receiveRtpPort,
            receiveRtcpPort,
            receiveVideoRtpPort,
            receiveVideoRtcpPort,
            supportedCodecs,
            srtpParams,
            mBandwidthId,
            totalBandwidth,
            videoFramerate);

        // If codecs set ACK in SDP
        // If there is an SDP body find the best
        //codecs, address & port
        int numEncoderCodecs = 0;
        SdpCodec** encoderCodecs = NULL;
        int numDecoderCodecs = 0;
        SdpCodec** decoderCodecs = NULL;
        SdpSrtpParameters matchingSrtpParams;
        memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));

        // Make a copy of the codec that we offered as we must be prepared to receive all
        // of them
        supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

        if(getInitialSdpCodecs(request,
                               supportedCodecs,
                               numEncoderCodecs,
                               encoderCodecs,
                               remoteRtpAddress,
                               remoteRtpPort,
                               remoteRtcpPort,
                               remoteVideoRtpPort,
                               remoteVideoRtcpPort,
                               srtpParams,
                               matchingSrtpParams,
                               totalBandwidth,
                               matchingBandwidth,
                               videoFramerate,
                               matchingVideoFramerate) && numEncoderCodecs > 0)
        {
            fireIncompatibleCodecsEvent(&supportedCodecs, encoderCodecs, numEncoderCodecs) ;

            if (matchingBandwidth != 0)
            {
                mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
            }
            if (matchingVideoFramerate != 0)
            {
                mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
            }
            // Set up the remote RTP sockets
            setMediaDestination(remoteRtpAddress.data(),
                remoteRtpPort,
                remoteRtcpPort,
                remoteVideoRtpPort,
                remoteVideoRtcpPort,
                request->getSdpBody(mpSecurity));

#ifdef TEST_PRINT
            osPrintf("RTP SENDING address: %s port: %d\n", remoteRtpAddress.data(), remoteRtpPort);
#endif

            // TODO: received ACK with SDP, should also check if remote streams are actively receiving

            mpMediaInterface->startRtpSend(mConnectionId,
                numEncoderCodecs, encoderCodecs);
            fireAudioStartEvents() ;
        }
#ifdef TEST_PRINT
        osPrintf("ACK reinviteState: %d\n", reinviteState);
#endif
        // Free up the codec copies and pointer array
        SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
        SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);

        if(reinviteState == ACCEPT_INVITE)
        {
            inviteFromThisSide = FALSE;
            setCallerId();

            if (numEncoderCodecs > 0)
            {
                setState(CONNECTION_ESTABLISHED, CONNECTION_REMOTE);
                if (mpCall->isInFocus())
                {
                    fireSipXCallEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_NORMAL);
                }
                else
                {
                    fireSipXCallEvent(CALLSTATE_BRIDGED, CALLSTATE_CAUSE_NORMAL);
                }
            }


            // If the other side did not send an Allowed field in
            // the INVITE, send an OPTIONS method to see if the
            // otherside supports methods such as REFER
            if(mAllowedRemote.isNull())
            {
                int iCSeq ;
                mCSeqMgr.startTransaction(CSEQ_ID_OPTIONS, iCSeq) ;
                SipMessage optionsRequest;
                optionsRequest.setOptionsData(inviteMsg, mRemoteContact, inviteFromThisSide,
                    iCSeq, mRouteField.data(), mLocalContact);
                send(optionsRequest);
            }
        }
        // Reset to allow sub sequent re-INVITE
        else if(reinviteState == REINVITED)
        {
#ifdef TEST_PRINT
            osPrintf("ReINVITE ACK - ReINVITE allowed again\n");
#endif
            reinviteState = ACCEPT_INVITE;
        }

        // If we are in the middle of a transfer meta event
        // on the target phone and target call it ends here
        if(mpCall->getCallType() ==
            CpCall::CP_TRANSFER_TARGET_TARGET_CALL)
        {
            mpCall->setCallType(CpCall::CP_NORMAL_CALL);
        }
    }

    // Else error response to the ACK
    //getState() != CONNECTION_ESTABLISHED
    // requestSequenceNum != requestSequenceNum
    else
    {
#ifdef TEST_PRINT
        osPrintf("Ignoring ACK connectionState: %d request CSeq: %d invite CSeq: %d\n",
            getState(), requestSequenceNum, requestSequenceNum);
#endif

        // If there is no invite message then shut down this connection
        if(!inviteMsg)
        {
            setState(CONNECTION_FAILED, CONNECTION_LOCAL);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NO_KNOWN_INVITE) ;
        }

        // ACKs do not get a response
    }

} // End of processAckRequest

void SipConnection::processByeRequest(const SipMessage* request)
{
// DO NOT CHECK IN
//#ifdef TEST_PRINT
    OsSysLog::add(FAC_SIP, PRI_WARNING,
        "Entering SipConnection::processByeRequest inviteMsg=%p last CSeq: %d", 
        inviteMsg,
        lastRemoteSequenceNumber);
//#endif
    int requestSequenceNum = 0;
    UtlString requestSeqMethod;

    request->getCSeqField(&requestSequenceNum, &requestSeqMethod);

    if(inviteMsg && lastRemoteSequenceNumber < requestSequenceNum)
    {
        lastRemoteSequenceNumber = requestSequenceNum;

        // Stop the media usage ASAP
        if (mpMediaInterface != NULL)
        {
            mpMediaInterface->stopRtpSend(mConnectionId);
            mpMediaInterface->stopRtpReceive(mConnectionId);
            fireAudioStopEvents() ;
        }

        // Build an OK response
        SipMessage sipResponse;
        sipResponse.setOkResponseData(request, mLocalContact);
        send(sipResponse);

        setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;
    }

    // BYE is not legal in the current state
    else
    {
        // Build an error response
        SipMessage sipResponse;
        sipResponse.setByeErrorData(request);
        send(sipResponse);

        // Do not change the state

        // I do not recall the context of the above comment
        // May want to change to failed state in all cases
        if(getState() == CONNECTION_IDLE)
        {
            setState(CONNECTION_FAILED, CONNECTION_LOCAL);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BYE_DURING_IDLE) ;
        }
        else if(!inviteMsg)
        {
            // If an invite was not sent or received something
            // is wrong.  This bye is invalid.
            setState(CONNECTION_FAILED, CONNECTION_LOCAL);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NO_KNOWN_INVITE) ;
        }
    }
#ifdef TEST_PRINT
    OsSysLog::add(FAC_SIP, PRI_WARNING,
        "Leaving SipConnection::processByeRequest inviteMsg=%p", inviteMsg);
#endif

    if (!mVoiceQualityReportTarget.isNull())
        sendVoiceQualityReport(mVoiceQualityReportTarget) ;

} // End of processByeRequest

void SipConnection::processCancelRequest(const SipMessage* request)
{
#ifdef TEST_PRINT
    OsSysLog::add(FAC_SIP, PRI_WARNING,
        "Entering SipConnection::processCancelRequest inviteMsg=0x%08x ", (int)inviteMsg);
#endif
    int requestSequenceNum = 0;
    UtlString requestSeqMethod;

    request->getCSeqField(&requestSequenceNum, &requestSeqMethod);

    int calleeState = getState();

    // If it is acceptable to CANCLE the call
    if(lastRemoteSequenceNumber == requestSequenceNum &&
        calleeState != CONNECTION_IDLE &&
        calleeState != CONNECTION_DISCONNECTED &&
        calleeState !=  CONNECTION_FAILED &&
        calleeState != CONNECTION_ESTABLISHED)
    {
        // Build a 487 response
        if (!inviteFromThisSide)
        {
            SipMessage sipResponse;
            sipResponse.setRequestTerminatedResponseData(inviteMsg);
            send(sipResponse);
        }

        setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, CONNECTION_CAUSE_CANCELLED);
        setState(CONNECTION_DISCONNECTED, CONNECTION_LOCAL, CONNECTION_CAUSE_CANCELLED);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_CANCEL) ;

        // Build an OK response
        SipMessage cancelResponse;
        cancelResponse.setOkResponseData(request, mLocalContact);
        send(cancelResponse);
    }
    // CANCEL is not legal in the current state
    else
    {
        if(lastRemoteSequenceNumber == requestSequenceNum)
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "CANCEL not valid in current call state: %d",
                    calleeState);
        }
        else
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "CANCEL CSeq: %d does not match current call transaction CSeq: %d",
                    requestSequenceNum, lastRemoteSequenceNumber);
        }

        // Build an error response
        SipMessage sipResponse;
        sipResponse.setBadTransactionData(request);
        send(sipResponse);

        // Do not change the state

        // Do not know where the above comment came from
        // If there was no invite sent or received this is a bad call
        if(!inviteMsg)
        {
            setState(CONNECTION_FAILED, CONNECTION_LOCAL, CONNECTION_CAUSE_CANCELLED);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NO_KNOWN_INVITE) ;
        }
    }
} // End of processCancelRequest

UtlBoolean SipConnection::getInitialSdpCodecs(const SipMessage* sdpMessage,
                                              SdpCodecList& supportedCodecsArray,
                                              int& numCodecsInCommon,
                                              SdpCodec** &commonCodecsForEncoder,
                                              UtlString& remoteAddress,
                                              int& remotePort,
                                              int& remoteRtcpPort,
                                              int& remoteVideoRtpPort,
                                              int& remoteVideoRtcpPort,
                                              SdpSrtpParameters& localSrtpParams,
                                              SdpSrtpParameters& matchingSrtpParams,
                                              int localBandwidth,
                                              int& matchingBandwidth,
                                              int localVideoFramerate,
                                              int& matchingVideoFramerate)
{
   memset((void*)&matchingSrtpParams, 0, sizeof(SdpSrtpParameters));

   // Get the RTP info from the message if present
   // Should check the content type first
   SdpBody* sdpBody = (SdpBody*)sdpMessage->getSdpBody(mpSecurity, mpCallManager);
   if(sdpBody)
   {
#ifdef TEST_PRINT
      osPrintf("SDP body in INVITE, finding best codec\n");
#endif
      sdpBody->getBestAudioCodecs(supportedCodecsArray,
                                  numCodecsInCommon,
                                  commonCodecsForEncoder,
                                  //commonCodecsForDecoder,
                                  remoteAddress,
                                  remotePort,
                                  remoteRtcpPort,
                                  remoteVideoRtpPort,
                                  remoteVideoRtcpPort,
                                  localSrtpParams,
                                  matchingSrtpParams,
                                  localBandwidth,
                                  matchingBandwidth,
                                  localVideoFramerate,
                                  matchingVideoFramerate);

      // We do not care what the subset of codecs that are enabled locally
      // and supported remotely.  We must be prepared for all the codecs that
      // are enabled.
          
      mpMediaInterface->setSrtpParams(matchingSrtpParams);

      // To be complient with RFC 3264
      if(sdpBody->findValueInField("a", "sendonly"))
      {
         remoteAddress = "0.0.0.0";
      }
   }
   else if (!sdpBody && mpSecurity)
   {
      sdpBody = (SdpBody*)sdpMessage->getSdpBody(NULL, mpCallManager);
      if (sdpBody)
      {
         // if the message had an unencrypted SDP body,
         // but we are expecting encrypted, then fail
         setState(CONNECTION_FAILED, CONNECTION_REMOTE);
         fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;
         sdpBody = NULL;
      }
   }
#ifdef TEST_PRINT
   else
   {
      osPrintf("No SDP in message\n");
   }
#endif

   return(sdpBody != NULL);
}

UtlBoolean SipConnection::processResponse(const SipMessage* response)
{
    int sequenceNum;
    UtlString sequenceMethod;
    UtlBoolean processedOk = TRUE;
    UtlString responseText;
    UtlString contactUri;
    int previousState = getState();
    int responseCode = response->getResponseStatusCode();
    mResponseCode = responseCode;

    response->getResponseStatusText(&responseText);
    mResponseText = responseText;
    response->getCSeqField(&sequenceNum, &sequenceMethod);

    if(!inviteMsg)
    {
        // An invite was not sent or received.  This call is invalid.
        setState(CONNECTION_FAILED, CONNECTION_REMOTE);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;
    }
    else if(strcmp(sequenceMethod.data(), SIP_INVITE_METHOD) == 0)
    {
        processInviteResponse(response);
        if (mTerminalConnState == PtTerminalConnection::HELD)
        {
            UtlString remoteAddress;
            getRemoteAddress(&remoteAddress);
            postTaoListenerMessage(PtEvent::TERMINAL_CONNECTION_HELD, PtEvent::CAUSE_NEW_CALL);
        }

    } // End INVITE responses

    // REFER responses:
    else if(strcmp(sequenceMethod.data(), SIP_REFER_METHOD) == 0)
    {
        processReferResponse(response);
    }

    // Options response
    else if(strcmp(sequenceMethod.data(), SIP_OPTIONS_METHOD) == 0)
    {
        processOptionsResponse(response);
    }

    // NOTIFY response
    else if(strcmp(sequenceMethod.data(), SIP_NOTIFY_METHOD) == 0)
    {
        processNotifyResponse(response);
    }
    else if(strcmp(sequenceMethod.data(), SIP_INFO_METHOD) == 0)
    {
        if (sequenceNum == mCSeqMgr.getCSeqNumber(CSEQ_ID_INFO))
        {
            if (responseCode >= SIP_OK_CODE)
            {
                mCSeqMgr.endTransaction(CSEQ_ID_INFO) ;
            }
        }
        else
        {
            OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring INFO response: CSeq %d != %d",
                    sequenceNum,
                    mCSeqMgr.getCSeqNumber(CSEQ_ID_INFO)) ;
        }
    }
    // else
    // BYE, CANCEL responses
    else if(strcmp(sequenceMethod.data(), SIP_BYE_METHOD) == 0 ||
        strcmp(sequenceMethod.data(), SIP_CANCEL_METHOD) == 0)
    {
        // We check the sequence number and method name of the
        // last sent request to make sure this is a response to
        // something that we actually sent
        if(mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum &&
                sequenceMethod.compareTo(mLastRequestMethod) == 0)
        {
#ifdef TEST_PRINT
            osPrintf("%s response: %d %s\n", sequenceMethod.data(),
                responseCode, responseText.data());
#endif
            if(responseCode >= SIP_OK_CODE)
            {
                if (mpMediaInterface != NULL)
                {
                    mpMediaInterface->stopRtpSend(mConnectionId);
                    mpMediaInterface->stopRtpReceive(mConnectionId);
                    fireAudioStopEvents() ;
                }
            }

            // If this is the response to a BYE Also transfer
            if(getState() == CONNECTION_ESTABLISHED &&
                responseCode >= SIP_OK_CODE &&
                strcmp(sequenceMethod.data(), SIP_BYE_METHOD) == 0 &&
                !mTargetCallConnectionAddress.isNull() &&
                !isMethodAllowed(SIP_REFER_METHOD))
            {
                // We need to send notification to the target call
                // as to whether the transfer failed or succeeded
                int state;
                int cause;
                if(responseCode == SIP_OK_CODE)
                {
                    state = CONNECTION_ESTABLISHED;
                    //cause = CONNECTION_CAUSE_NORMAL;
                    //setState(CONNECTION_DISCONNECTED, CONNECTION_CAUSE_TRANSFER);
                    cause = CONNECTION_CAUSE_TRANSFER;
                }
                else if(responseCode == SIP_BAD_EXTENSION_CODE ||
                    responseCode == SIP_UNIMPLEMENTED_METHOD_CODE)
                {
                    state = CONNECTION_FAILED;
                    cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;
                }
                else if(responseCode == SIP_DECLINE_CODE)
                {
                    state = CONNECTION_FAILED;
                    cause = CONNECTION_CAUSE_CANCELLED;
                }
                else
                {
                    state = CONNECTION_FAILED;
                    cause = CONNECTION_CAUSE_BUSY;
                }

                setState(state, CONNECTION_REMOTE, cause);
                /** SIPXTAPI: TBD **/

                // Send the message to the target call
                UtlString targetCallId;
                UtlString toField;
                mToUrl.toString(toField);
                mpCall->getTargetCallId(targetCallId);
                CpMultiStringMessage transferControllerStatus(CallManager::CP_TRANSFER_CONNECTION_STATUS,
                    targetCallId.data(), toField.data(),
                    NULL, NULL, NULL,
                    state, cause);
#ifdef TEST_PRINT
                osPrintf("SipConnection::processResponse BYE posting CP_TRANSFER_CONNECTION_STATUS to call: %s\n",
                    targetCallId.data());
#endif
                mpCallManager->postMessage(transferControllerStatus);

                // Reset mTargetCallConnectionAddress so that if this connection
                // is not disconnected due to transfer failure, it can
                // try another transfer or just disconnect.
                mTargetCallConnectionAddress = "";

            }

            //for BYE
            else if(responseCode >= SIP_OK_CODE &&
                mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum &&
                (strcmp(sequenceMethod.data(), SIP_BYE_METHOD) == 0))
            {
                OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "SipConnection::processResponse: Response %d "
                    "received for BYE", responseCode);
                setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NORMAL) ;

                // If we are in the middle of a transfer meta event
                // on the target phone and target call it ends here
                int metaEventId = 0;
                int metaEventType = PtEvent::META_EVENT_NONE;
                int numCalls = 0;
                const UtlString* metaEventCallIds = NULL;
                if(mpCall)
                {
                    mpCall->getMetaEvent(metaEventId, metaEventType, numCalls,
                        &metaEventCallIds);
                    if(metaEventId > 0 && metaEventType == PtEvent::META_CALL_TRANSFERRING)
                        mpCall->stopMetaEvent();
                }
            }
            else if(responseCode >= SIP_OK_CODE &&
                mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum &&
                (strcmp(sequenceMethod.data(), SIP_CANCEL_METHOD) == 0) &&
                previousState != CONNECTION_ESTABLISHED &&
                previousState != CONNECTION_FAILED &&
                previousState != CONNECTION_DISCONNECTED &&
                previousState != CONNECTION_UNKNOWN &&
                previousState != CONNECTION_FAILED)
            {
                //start 32 second timer according to the bis03 draft
                UtlString callId;
                mpCall->getCallId(callId);
                UtlString remoteAddr;
                getRemoteAddress(&remoteAddr);
                CpMultiStringMessage* CancelTimerMessage =
                    new CpMultiStringMessage(CpCallManager::CP_CANCEL_TIMER, callId.data(), remoteAddr.data());
                OsTimer* timer = new OsTimer(mpCallManager->getMessageQueue(), (intptr_t)CancelTimerMessage);

                // HACK: Changing this value to 2 to fix a problem in tear down.  We need to do the following:
                //       1) Figure out why 487 responses are not dispatched to this listener (SipTransaction::whatRelationship)
                //       2) Note when a 487 response is received as part of cancel, note when the 200 OK was received for cancel
                //       3) Drop the call as early as possible

                OsTime timerTime(2, 0);
                timer->oneshotAfter(timerTime);
            }

            else if(sequenceMethod.compareTo(SIP_BYE_METHOD) == 0)
            {
                processByeResponse(response);
            }
            else if(sequenceMethod.compareTo(SIP_CANCEL_METHOD) == 0)
            {
                processCancelResponse(response);
            }
            // else Ignore provisional responses
            else
            {
#ifdef TEST_PRINT
                osPrintf("%s provisional response ignored: %d %s\n",
                    sequenceMethod.data(),
                    responseCode,
                    responseText.data());
#endif
            }
        }
        else
        {
#ifdef TEST_PRINT
            osPrintf("%s response ignored: %d %s invalid cseq last: %d last method: %s\n",
                sequenceMethod.data(),
                responseCode,
                responseText.data(),
                mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE),
                mLastRequestMethod.data());
#endif
        }
    }//END - else if(strcmp(sequenceMethod.data(), SIP_BYE_METHOD) == 0 || strcmp(sequenceMethod.data(), SIP_CANCEL_METHOD) == 0)

    // Unknown method response
    else
    {
#ifdef TEST_PRINT
        osPrintf("%s response ignored: %d %s\n",
            sequenceMethod.data(),
            responseCode,
            responseText.data());
#endif
    }

    return(processedOk);
}  // End of processResponse

void SipConnection::processInviteResponseRinging(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();
    const SdpBody* pBody = response->getSdpBody(mpSecurity) ;
    UtlBoolean isEarlyMedia = TRUE;

    if (responseCode == SIP_RINGING_CODE && !mIsEarlyMediaFor180)
    {
        isEarlyMedia = FALSE;
    }

    // Record remote contact (may can change over time)
    UtlString contactInResponse;
    if (response->getContactUri(0 , &contactInResponse))
    {
        mContactUriStr = contactInResponse.data();
    }
    // If there is SDP we have early media or remote ringback
    int cause = CONNECTION_CAUSE_NORMAL;
    if(pBody && isEarlyMedia && mpMediaInterface != NULL)
    {
        cause = CONNECTION_CAUSE_UNKNOWN;

        // If this is the initial INVITE
        if(reinviteState == ACCEPT_INVITE)
        {
            /*
             * Setup the sending of audio
             */
            UtlString rtpAddress;
            int receiveRtpPort;
            int receiveRtcpPort;
            int receiveVideoRtpPort;
            int receiveVideoRtcpPort;
            int totalBandwidth = 0;
            int matchingBandwidth = 0;
            int videoFramerate = 0;
            int matchingVideoFramerate = 0;
            SdpCodecList supportedCodecs;
            SdpSrtpParameters srtpParams;
            memset(&srtpParams, 0, sizeof(srtpParams));
            mpMediaInterface->getCapabilities(mConnectionId,
                rtpAddress,
                receiveRtpPort,
                receiveRtcpPort,
                receiveVideoRtpPort,
                receiveVideoRtcpPort,
                supportedCodecs,
                srtpParams,
                mBandwidthId,
                totalBandwidth,
                videoFramerate);
            // Setup the media channel
            // The address should be retrieved from the sdpBody
            int numEncoderCodecs = 0;
            SdpCodec** encoderCodecs = NULL;
            int numDecoderCodecs = 0;
            SdpCodec** decoderCodecs = NULL;
            SdpSrtpParameters matchingSrtpParams;
            memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));

            // Make a copy of the codec that we offered as we must be prepared to receive all
            // of them
            supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

            getInitialSdpCodecs(response,
                                supportedCodecs,
                                numEncoderCodecs,
                                encoderCodecs,
                                remoteRtpAddress,
                                remoteRtpPort,
                                remoteRtcpPort,
                                remoteVideoRtpPort,
                                remoteVideoRtcpPort,
                                srtpParams,
                                matchingSrtpParams,
                                totalBandwidth,
                                matchingBandwidth,
                                videoFramerate,
                                matchingVideoFramerate);

            fireIncompatibleCodecsEvent(&supportedCodecs, encoderCodecs, numEncoderCodecs) ;

            if(numEncoderCodecs > 0)
            {
                if (matchingBandwidth != 0)
                {
                    mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
                }
                if (matchingVideoFramerate != 0)
                {
                    mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
                }

                // Set up the remote RTP sockets if we have a legitimate
                // address to send RTP
                if( !remoteRtpAddress.isNull() &&
                        (remoteRtpAddress.compareTo("0.0.0.0") != 0) &&
                        (remoteRtpPort > 0) )
                {
                    UtlString role = ((SdpBody*)pBody)->getRtpTcpRole();
                    if (role.compareTo("active") == 0)
                    {
                        mpMediaInterface->setConnectionTcpRole(mConnectionId, RTP_TCP_ROLE_PASSIVE);
                    }
                    else if (role.compareTo("passive") == 0)
                    {
                        mpMediaInterface->setConnectionTcpRole(mConnectionId, RTP_TCP_ROLE_ACTIVE);
                    }

                    setMediaDestination(remoteRtpAddress.data(),
                        remoteRtpPort,
                        remoteRtcpPort,
                        remoteVideoRtpPort,
                        remoteVideoRtcpPort,
                        response->getSdpBody(mpSecurity));

                    // 18X Ringing recieved, if we sent SDP in the INVITE and this reponse contains SDP,
                    // we should NOT copy the payload IDs from the response.
                    mpMediaInterface->startRtpReceive(mConnectionId,
                        numDecoderCodecs,
                        decoderCodecs);
                    fireAudioStartEvents() ;

                    // TODO: 18X early media, should also check to see if remote is actively receiving

                    mpMediaInterface->startRtpSend(mConnectionId,
                        numEncoderCodecs,
                        encoderCodecs);

                    fireAudioStartEvents() ;
                }
            }

            // Free up the codec copies and pointer array
            SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
            SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);
        }
    }


    /*
     * Set state and Fire off events
     */
    setState(CONNECTION_ALERTING, CONNECTION_REMOTE, cause);
    if ((pBody != NULL) || (responseCode == SIP_EARLY_MEDIA_CODE))
    {
        fireSipXCallEvent(CALLSTATE_REMOTE_ALERTING, CALLSTATE_CAUSE_EARLY_MEDIA) ;
    }
    else
    {
        fireSipXCallEvent(CALLSTATE_REMOTE_ALERTING, CALLSTATE_CAUSE_NORMAL) ;
    }
}


void SipConnection::processInviteResponseBusy(const SipMessage* response)
{
    /*
     * Set States
     */
    setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_BUSY);
    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BUSY) ;

    reinviteState = ACCEPT_INVITE ;

    // Should we call stop audio???
}

void SipConnection::processInviteResponseQueued(const SipMessage* response)
{
    // This is not really supported -- hasn't even been tested (or atleast
    // since 1999)
    setState(CONNECTION_QUEUED, CONNECTION_REMOTE);
}

void SipConnection::processInviteResponseRequestPending(const SipMessage* response)
{
    /*
    * Temp Re-invite failure, queue and retry
    */
    reinviteState = ACCEPT_INVITE;

    SipMessageEvent* sipMsgEvent =
            new SipMessageEvent(new SipMessage(*response),
            SipMessageEvent::SESSION_REINVITE_TIMER);
    OsTimer* timer = new OsTimer((mpCallManager->getMessageQueue()),
            (intptr_t)sipMsgEvent);

    OsTime timerTime((sRandom->rand() % 3), (sRandom->rand() % 1000) * 1000);
    timer->oneshotAfter(timerTime);
}


void SipConnection::processInviteResponseFailed(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();

    if (reinviteState == ACCEPT_INVITE)
    {
        /*
         * Initial Invite
         */

        int cause = CONNECTION_CAUSE_UNKNOWN;
        int warningCode;

        /*
         * Figure out cause of failure
         */
        switch(responseCode)
        {
        case HTTP_UNAUTHORIZED_CODE:
            cause = CONNECTION_CAUSE_NOT_ALLOWED;
            break;

        case HTTP_PROXY_UNAUTHORIZED_CODE:
            cause = CONNECTION_CAUSE_NETWORK_NOT_ALLOWED;
            break;

        case SIP_REQUEST_TIMEOUT_CODE:
            cause = CONNECTION_CAUSE_CANCELLED;
            break;

        case SIP_REQUEST_NOT_ACCEPTABLE_HERE_CODE:
            cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;
            break;

        case SIP_BAD_REQUEST_CODE:
            response->getWarningCode(&warningCode);
            if(warningCode == SIP_WARN_MEDIA_NAVAIL_CODE)
            {
                // incompatible media
                cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;
            }
            break;
        default:

            // 400 class errors
            if(responseCode < SIP_SERVER_INTERNAL_ERROR_CODE)
            {
                cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;
            }
            // 500 class errors
            else if(// implicit: responseCode >= SIP_SERVER_INTERNAL_ERROR_CODE &&
                responseCode < SIP_GLOBAL_BUSY_CODE)
            {
                cause = CONNECTION_CAUSE_NETWORK_NOT_OBTAINABLE;
            }
            // 600 class errors
            else if(responseCode >= SIP_GLOBAL_BUSY_CODE)
            {
                cause = CONNECTION_CAUSE_NETWORK_CONGESTION;
            }
            // Who knows.
            else
            {
               cause = CONNECTION_CAUSE_UNKNOWN;
            }
            break;
        }

        /*
         * Set state based on failure code
         */
        if(responseCode == SIP_REQUEST_TERMINATED_CODE)
        {
            cause = CONNECTION_CAUSE_CANCELLED;
            setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, cause);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BUSY) ;
        }
        else if (responseCode == SIP_NOT_FOUND_CODE)
        {
            cause = CONNECTION_CAUSE_DEST_NOT_OBTAINABLE;
            setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, cause);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_ADDRESS) ;
        }
        else if (responseCode == SIP_REQUEST_UNDECIPHERABLE_CODE)
        {
            cause = CONNECTION_CAUSE_NOT_ALLOWED;
            setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, cause);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REMOTE_SMIME_UNSUPPORTED);
        }
        else if (responseCode == SIP_REQUEST_NOT_ACCEPTABLE_HERE_CODE)
        {
            setState(CONNECTION_DISCONNECTED, CONNECTION_REMOTE, cause);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REQUEST_NOT_ACCEPTED);
        }
        else if (responseCode == SIP_REQUEST_TIMEOUT_CODE)
        {
            setState(CONNECTION_FAILED, CONNECTION_REMOTE, cause);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NO_RESPONSE);
        }
        else if (responseCode == SIP_BAD_ADDRESS_CODE)
        {
           setState(CONNECTION_FAILED, CONNECTION_REMOTE, cause);
           fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_ADDRESS);
        }
        else if (responseCode == SIP_DECLINE_CODE || responseCode == SIP_FORBIDDEN_CODE)
        {
           setState(CONNECTION_FAILED, CONNECTION_REMOTE, cause);
           fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REQUEST_NOT_ACCEPTED);
        }
        else
        {
            int errorClass = responseCode / 100;
            setState(CONNECTION_FAILED, CONNECTION_REMOTE, cause);
            switch(errorClass)
            {
                case 5: // 500 class
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_SERVER_ERROR);
                break;

                default:
                    fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_UNKNOWN);
                break;
            }
        }

        mbCancelling = FALSE;   // if was cancelling, now cancelled.
    }
    else if (reinviteState == REINVITING &&
            responseCode != SIP_REQUEST_NOT_ACCEPTABLE_HERE_CODE)
    {
        /*
         * Reinvite Failed -- fire off failure event
         */

        reinviteState = ACCEPT_INVITE;
        //processedOk = false;

        // Temp Fix: If we failed to renegotiate a invite, failed the
        // connection so that the upper layers can react.  We *SHOULD*
        // fire off a new event to application layer indicating that the
        // reinvite failed -- or make hold/unhold blocking. (Bob 8/14/02)

        postTaoListenerMessage(CONNECTION_FAILED, CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION, false);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_RESOURCE_LIMIT) ;
    }
    else if (reinviteState == REINVITING &&
            responseCode == SIP_REQUEST_NOT_ACCEPTABLE_HERE_CODE)
    {
        /*
         * Reinvite Failed -- ignore response??  More work may be required
         * here.
         */
        reinviteState = ACCEPT_INVITE;

        // RFC 3261 states:
        /*
            During the session, either Alice or Bob may decide to change the
            characteristics of the media session.  This is accomplished by
            sending a re-INVITE containing a new media description.  This re-
            INVITE references the existing dialog so that the other party knows
            that it is to modify an existing session instead of establishing a
            new session.  The other party sends a 200 (OK) to accept the change.
            The requestor responds to the 200 (OK) with an ACK.  If the other
            party does not accept the change, he sends an error response such as
            488 (Not Acceptable Here), which also receives an ACK.  However, the
            failure of the re-INVITE does not cause the existing call to fail -
            the session continues using the previously negotiated
            characteristics.  Full details on session modification are in Section
            14.
        */
        // my interpretation of this is that we need to continue the session
        // and not cause a disconnect - MDC 6/23/2005

        fireSipXCallEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_REQUEST_NOT_ACCEPTED) ;
    }
}


void SipConnection::processInviteResponseHangingUp(const SipMessage* response)
{
    mbCancelling = FALSE;   // if was cancelling, now cancelled.

    // Send an ACK
    SipMessage sipAckRequest;
    sipAckRequest.setAckData(response,inviteMsg, mLocalContact);
    send(sipAckRequest);

    // Always get the remote contact as it may can change over time
    UtlString contactInResponse;
    if (response->getContactUri(0 , &contactInResponse))
    {
        mContactUriStr = contactInResponse.data();
        mRemoteContact.remove(0);
        mRemoteContact.append(contactInResponse);
    }

    // Get the route for subsequent requests
    response->buildRouteField(&mRouteField);

    // Send a BYE
    SipMessage sipByeRequest;
    int iCSeq ;
    mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
    sipByeRequest.setByeData(inviteMsg, mRemoteContact, TRUE,
        iCSeq, mRouteField.data(), NULL,
        mLocalContact.data());

    mLastRequestMethod = SIP_BYE_METHOD;
    send(sipByeRequest);
}


void SipConnection::processInviteResponseNormal(const SipMessage* response)
{
    int previousState = getState();
    SipMessage* pOriginalInvite = inviteMsg;

    /*
     * Update routing and send ack
     */

    // Save the contact field in the response to send further reinvites
    UtlString contactInResponse;
    if (response->getContactUri(0 , &contactInResponse))
    {
        mContactUriStr = contactInResponse.data();
        mRemoteContact = contactInResponse ;
    }

    // Get the route for subsequent requests only if this is
    // the initial transaction
    if(previousState != CONNECTION_ESTABLISHED)
    {
        response->buildRouteField(&mRouteField);
    }

    // Construct an ACK
    SipMessage sipRequest;
    sipRequest.setAckData(response, inviteMsg, mLocalContact,
        mSessionReinviteTimer);

    // Set the route field
    if(!mRouteField.isNull())
    {
        sipRequest.setRouteField(mRouteField.data());
    }
    // Send the ACK message
    send(sipRequest);


    // Check to see if the original invite had an S/MIME type.
    // If it did, and the response type is not S/MIME, fail.
    UtlString originalContentType;
    UtlString contentType;
    pOriginalInvite->getContentType(&originalContentType);
    response->getContentType(&contentType);
    if (originalContentType.compareTo(CONTENT_SMIME_PKCS7) == 0 &&
            contentType.compareTo(CONTENT_SMIME_PKCS7) != 0)
    {
        /*
         * SMIME failed
         */

        setState(CONNECTION_DISCONNECTED, CONNECTION_LOCAL);
        fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_REMOTE_SMIME_UNSUPPORTED) ;

        // Send a BYE
        SipMessage sipByeRequest;
        int iCSeq ;
        mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
        sipByeRequest.setByeData(inviteMsg, mRemoteContact, TRUE,
                iCSeq, mRouteField.data(), NULL,
                mLocalContact.data());

        mLastRequestMethod = SIP_BYE_METHOD;
        send(sipByeRequest);
    }
    else
    {

        // Determine if session timer if requested/set
        mSessionReinviteTimer = 0;
        UtlString refresher ;
        int timerSeconds  = 0 ;

        /*
        // Session timers are somewhat broken -- they have not been
        // updated since the old SIP RFC -- work is needed here.
        //
        response->getSessionExpires(&mSessionReinviteTimer, &refresher);
        if (refresher.isNull() || refresher.compareTo("uac", UtlString::ignoreCase))
        {
            // Set a timer to reINVITE to keep the session up
            if(mSessionReinviteTimer > mDefaultSessionReinviteTimer &&
                mDefaultSessionReinviteTimer != 0) // if default < 0 disallow
            {
                mSessionReinviteTimer = mDefaultSessionReinviteTimer;
            }

            timerSeconds = mSessionReinviteTimer;
            if(mSessionReinviteTimer > 2)
            {
                timerSeconds = mSessionReinviteTimer / 2; //safety factor
            }
        }
        else
        {
            mSessionReinviteTimer = 0 ;
        }*/

        // Start the session reINVITE timer
        if(mSessionReinviteTimer > 0)
        {
            SipMessageEvent* sipMsgEvent =
                new SipMessageEvent(new SipMessage(sipRequest),
                SipMessageEvent::SESSION_REINVITE_TIMER);
            OsTimer* timer = new OsTimer((mpCallManager->getMessageQueue()),
                (intptr_t)sipMsgEvent);
            // Convert from mSeconds to uSeconds
            OsTime timerTime(timerSeconds, 0);
            timer->oneshotAfter(timerTime);
        }

        if (mHoldState == TERMCONNECTION_HOLDING)
        {
            /*
             * We requested a hold operation -- assume that worked.
             */

            setTerminalConnectionState(PtTerminalConnection::HELD, 1);
            mHoldState = TERMCONNECTION_HELD;

            // The prerequisit hold was completed we
            // can now do the next action/transaction
            switch(mHoldCompleteAction)
            {
            case CpCallManager::CP_BLIND_TRANSFER:
                // This hold was performed as a precurser to
                // A blind transfer.
                mHoldCompleteAction = CpCallManager::CP_UNSPECIFIED;
                doBlindRefer();
                break;

            default:
                // Bogus action, reset it
                mHoldCompleteAction = CpCallManager::CP_UNSPECIFIED;
                break;
            }
        }
        else if (mHoldState == TERMCONNECTION_UNHOLDING)
        {
            mHoldState = TERMCONNECTION_TALKING ;
            setTerminalConnectionState(PtTerminalConnection::TALKING, 1);
        }
        else if (mHoldState == TERMCONNECTION_TALKING)
        {
            setTerminalConnectionState(PtTerminalConnection::TALKING, 1);
        }

        /*
        * Deal with SDP
        */

        UtlString rtpAddress;
        int receiveRtpPort;
        int receiveRtcpPort;
        int receiveVideoRtpPort;
        int receiveVideoRtcpPort;
        int totalBandwidth = 0;
        int matchingBandwidth = 0;
        int videoFramerate = 0;
        int matchingVideoFramerate = 0;
        SdpCodecList supportedCodecs;
        SdpSrtpParameters srtpParams;
        memset(&srtpParams, 0, sizeof(srtpParams));

        if (mpMediaInterface != NULL)
        {
            mpMediaInterface->getCapabilities(mConnectionId,
                    rtpAddress,
                    receiveRtpPort,
                    receiveRtcpPort,
                    receiveVideoRtpPort,
                    receiveVideoRtcpPort,
                    supportedCodecs,
                    srtpParams,
                    mBandwidthId,
                    totalBandwidth,
                    videoFramerate);
        }

        // Setup the media channel
        // The address should be retrieved from the sdpBody
        int numEncoderCodecs = 0;
        SdpCodec** encoderCodecs = NULL;
        int numDecoderCodecs = 0;
        SdpCodec** decoderCodecs = NULL;
        // Make a copy of the codec that we offered as we must be prepared to receive all
        // of them
        supportedCodecs.getCodecs(numDecoderCodecs, decoderCodecs);

        SdpSrtpParameters matchingSrtpParams;
        memset(&matchingSrtpParams, 0, sizeof(matchingSrtpParams));
        getInitialSdpCodecs(response,
                            supportedCodecs,
                            numEncoderCodecs,
                            encoderCodecs,
                            remoteRtpAddress,
                            remoteRtpPort,
                            remoteRtcpPort,
                            remoteVideoRtpPort,
                            remoteVideoRtcpPort,
                            srtpParams,
                            matchingSrtpParams,
                            totalBandwidth,
                            matchingBandwidth,
                            videoFramerate,
                            matchingVideoFramerate);

        fireIncompatibleCodecsEvent(&supportedCodecs, encoderCodecs, numEncoderCodecs) ;

        if (numEncoderCodecs > 0 && mpMediaInterface != NULL)
        {
            if (matchingBandwidth != 0)
            {
                mpMediaInterface->setConnectionBitrate(mConnectionId, matchingBandwidth);
            }
            if (matchingVideoFramerate != 0)
            {
                mpMediaInterface->setConnectionFramerate(mConnectionId, matchingVideoFramerate);
            }
            // Set up the remote RTP sockets if we have a legitimate
            // address to send RTP
            if(!remoteRtpAddress.isNull() &&
                remoteRtpAddress.compareTo("0.0.0.0") != 0)
            {
                setMediaDestination(remoteRtpAddress.data(),
                    remoteRtpPort,
                    remoteRtcpPort,
                    remoteVideoRtpPort,
                    remoteVideoRtcpPort,
                    response->getSdpBody(mpSecurity));

                    const SdpBody* pBody = response->getSdpBody(mpSecurity);
                    UtlString role = ((SdpBody*)pBody)->getRtpTcpRole();
                    if (role.compareTo("active") == 0)
                    {
                        mpMediaInterface->setConnectionTcpRole(mConnectionId, RTP_TCP_ROLE_PASSIVE);
                    }
                    else if (role.compareTo("passive") == 0)
                    {
                        mpMediaInterface->setConnectionTcpRole(mConnectionId, RTP_TCP_ROLE_ACTIVE);
                    }
            }

            if(reinviteState == ACCEPT_INVITE)
            {
                setState(CONNECTION_ESTABLISHED, CONNECTION_REMOTE);
            }

            // No RTP address, stop media
            if (remoteRtpAddress.isNull() ||
                remoteRtpPort <= 0 ||
                remoteRtpAddress.compareTo("0.0.0.0") == 0 ||
                mHoldState == TERMCONNECTION_HELD ||
                mHoldState == TERMCONNECTION_HOLDING)
            {
                mHoldState = TERMCONNECTION_HELD ;
                if (mpCall->isInFocus())
                {
                    fireSipXCallEvent(CALLSTATE_REMOTE_HELD, CALLSTATE_CAUSE_NORMAL);
                }
                else
                {
                    fireSipXCallEvent(CALLSTATE_HELD, CALLSTATE_CAUSE_NORMAL);
                }

                mpMediaInterface->stopRtpSend(mConnectionId);
                mpMediaInterface->stopRtpReceive(mConnectionId);
                fireAudioStopEvents(MEDIA_CAUSE_HOLD) ;
            }
            else
            {
                mHoldState = TERMCONNECTION_TALKING ;

                // mpMediaInterface->stopRtpReceive(mConnectionId);
                // 200 OK reponse received, if we sent SDP in the INVITE and this reponse contains SDP,
                // we should NOT copy the payload IDs from the response.
                mpMediaInterface->startRtpReceive(mConnectionId,
                        numDecoderCodecs,
                        decoderCodecs);
                fireAudioStartEvents() ;

                // TODO: Initial INVITE, should also check to see that remote side is receiving

                mpMediaInterface->startRtpSend(mConnectionId,
                        numEncoderCodecs,
                        encoderCodecs);


                if (mpCall->isInFocus())
                {
                    fireSipXCallEvent(CALLSTATE_CONNECTED, CALLSTATE_CAUSE_NORMAL) ;
                }
                else
                {
                    fireSipXCallEvent(CALLSTATE_BRIDGED, CALLSTATE_CAUSE_NORMAL) ;
                }

                fireAudioStartEvents() ;
            }
        }
        else
        {
            // Initial INVITE -- and incompatible codecs or no codecs

            setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_RESOURCE_LIMIT) ;

            // Send a BYE
            SipMessage sipByeRequest;
            int iCSeq ;
            mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
            UtlString localContact ;
            sipByeRequest.setByeData(inviteMsg,
                mRemoteContact,
                inviteFromThisSide,
                iCSeq,
                mRouteField.data(),
                NULL, // no alsoUri
                mLocalContact.data());

            mLastRequestMethod = SIP_BYE_METHOD;
            send(sipByeRequest);
        }

        // Allow ReINVITEs from the other side again
        if(reinviteState == REINVITING)
        {
            reinviteState = ACCEPT_INVITE;
        }

        /*
        * If not allow header -- send options to determine remote capabilties
        */
        if(mAllowedRemote.isNull())
        {
            // Get the methods the other side supports
            response->getAllowField(mAllowedRemote);

            // If the other side did not set the allowed field
            // send an OPTIONS request to see what it supports
            if(mAllowedRemote.isNull())
            {
                int iCSeq ;
                mCSeqMgr.startTransaction(CSEQ_ID_OPTIONS, iCSeq) ;
                SipMessage optionsRequest;
                optionsRequest.setOptionsData(inviteMsg,
                    mRemoteContact,
                    inviteFromThisSide,
                    iCSeq,
                    mRouteField.data(), mLocalContact);

                send(optionsRequest);
            }
        }
        // Free up the codec copies and pointer array
        SdpCodecList::freeArray(numDecoderCodecs, decoderCodecs);
        SdpCodecList::freeArray(numEncoderCodecs, encoderCodecs);
    }
}

void SipConnection::processInviteResponseRedirect(const SipMessage* response)
{
    // NOTE: ACK gets sent by the SipUserAgent for error responses

    // If the call has not already failed
    if (getState() != CONNECTION_FAILED)
    {
        // Get the first contact uri
        UtlString contactUri;
        response->getContactUri(0, &contactUri);

        if(!contactUri.isNull() && inviteMsg)
        {
                mContactUriStr = contactUri.data();
            // Create a new INVITE
            SipMessage sipRequest(*inviteMsg);
            sipRequest.changeUri(contactUri.data());

            // Don't use the contact in the to field for redirect
            // Use the same To field, but clear the tag
            mToUrl.removeFieldParameter("tag");
            UtlString toField;
            mToUrl.toString(toField);
            sipRequest.setRawToField(toField);

            // Set incremented Cseq
            int iCSeq ;
            mCSeqMgr.startTransaction(CSEQ_ID_INVITE, iCSeq) ;
            sipRequest.setCSeqField(iCSeq,
                SIP_INVITE_METHOD);
            if (mLocationHeader.length() != 0)
            {
                sipRequest.setLocationField(mLocationHeader.data());
            }

            // Decrement the max-forwards header
            int maxForwards;
            if(!sipRequest.getMaxForwards(maxForwards))
            {
                maxForwards = SIP_DEFAULT_MAX_FORWARDS;
            }
            maxForwards--;
            sipRequest.setMaxForwards(maxForwards);

            // Remove all routes
            UtlString route;
            while ( sipRequest.removeRouteUri(0,&route)){}

            // Make sure we reset DNS SRV as this is a new request
            sipRequest.clearDNSField();
            sipRequest.resetTransport();

            // Get rid of the original invite and save a copy of
            // the new one
            if(inviteMsg)
            {
                delete inviteMsg;
            }
            inviteMsg = new SipMessage(sipRequest);
            inviteFromThisSide = TRUE;

            // Send the invite
            if(send(sipRequest))
            {
                // Change the state back to Offering
                setState(CONNECTION_OFFERING, CONNECTION_REMOTE, CONNECTION_CAUSE_REDIRECTED);
                fireSipXCallEvent(CALLSTATE_REMOTE_OFFERING, CALLSTATE_CAUSE_NORMAL) ;
            }
            else
            {
                UtlString redirected;
                int len;
                sipRequest.getBytes(&redirected, &len);

                // The send failed
                setState(CONNECTION_FAILED, CONNECTION_REMOTE, CONNECTION_CAUSE_NETWORK_NOT_OBTAINABLE);
                fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_NETWORK) ;
            }
        }
        else
        {
            // Receive a redirect with NO contact or a RANDOM redirect
            setState(CONNECTION_FAILED, CONNECTION_REMOTE);
            fireSipXCallEvent(CALLSTATE_DISCONNECTED, CALLSTATE_CAUSE_BAD_REDIRECT) ;
        }
    }
}

void SipConnection::processInviteResponseUnknown(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();

    if (responseCode >= SIP_OK_CODE || mIsReferSent)
    {
        // TODO:: Isn't the ACK automatically sent for error response by the
        //        user agent???  I believe the ACK code can be deleted, but
        //        we don't have any test infrastructure to validate this.
        //        (Bob/2006-02-28)

        // Send an ACK
        SipMessage sipRequest;
        sipRequest.setAckData(response,inviteMsg);
        send(sipRequest);

        if(reinviteState == REINVITED)
        {
            reinviteState = ACCEPT_INVITE;
        }
    }
}

void SipConnection::processInviteResponse(const SipMessage* response)
{
    int previousState = getState();
    int responseCode = response->getResponseStatusCode();
    SdpBody* pBody = (SdpBody*)response->getSdpBody();

    if (pBody)
    {
        // take care of sdp options
        bool bTcpAvailable = pBody->isTransportAvailable(OsSocket::TCP, MEDIA_TYPE_AUDIO);
        UtlString tcpRole = pBody->getRtpTcpRole();
        RtpTcpRoles role = RTP_TCP_ROLE_ACTPASS;
        if (bTcpAvailable)
        {
            if (tcpRole.compareTo("active") == 0)
            {
                // if response was RTP_TCP_ROLE_ACTIVE, we are passive
                role = RTP_TCP_ROLE_PASSIVE;
            }
            else if (tcpRole.compareTo("passive") == 0)
            {
                // if response was passive, we are RTP_TCP_ROLE_ACTIVE
                role = RTP_TCP_ROLE_ACTIVE;
            }
            mpMediaInterface->setConnectionTcpRole(mConnectionId, role);
        }
    }


    // Store the remote User agent
    response->getUserAgentField(&mRemoteUserAgent);

    /*
     * Validate Transaction (make sure it is what we expect)
     */
    int sequenceNum ;
    UtlString sequenceMethod;
    response->getCSeqField(&sequenceNum, &sequenceMethod);
    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_INVITE) ;
        }

        /*
         * Deal with tags
         */
        if (inviteMsg && (responseCode >= SIP_OK_CODE))
        {
            UtlString toAddr;
            UtlString toProto;
            int toPort;
            UtlString inviteTag;

            // Check to see if there is a tag set in the To field that
            // should be remembered for future messages.
            inviteMsg->getToAddress(&toAddr, &toPort, &toProto,
                NULL, NULL, &inviteTag);

            // Do not save the tag unless it is a final response
            // This is the stupid/simple thing to do until we need
            // more elaborate tracking of forked/serial branches
            if(inviteTag.isNull())
            {
                response->getToAddress(&toAddr, &toPort, &toProto,
                    NULL, NULL, &inviteTag);

                if(!inviteTag.isNull())
                {
                    inviteMsg->setToFieldTag(inviteTag.data());

                    // Update the cased to field after saving the tag
                    inviteMsg->getToUrl(mToUrl);
                }
            }
        }

        // Update the local contact w/ rport results
        updateContactFromResponse(response) ;

        /*
         * Handle various response cases
         */
        if ((responseCode == SIP_RINGING_CODE ||
                responseCode == SIP_EARLY_MEDIA_CODE) &&
                reinviteState == ACCEPT_INVITE)
        {
            // New Call
            processInviteResponseRinging(response) ;
        }
        else if (responseCode == SIP_BUSY_CODE &&
                reinviteState == ACCEPT_INVITE)
        {
            // Other end is busy
            processInviteResponseBusy(response) ;
        }
        else if (responseCode == SIP_QUEUED_CODE &&
                reinviteState == ACCEPT_INVITE)
        {
            // Call is queued
            processInviteResponseQueued(response) ;
        }
        else if (responseCode == SIP_REQUEST_PENDING_CODE)
        {
            // Overlapping Transaction
            processInviteResponseRequestPending(response) ;
        }
        else if (responseCode >= SIP_BAD_REQUEST_CODE)
        {
            // Invite Failed
            processInviteResponseFailed(response) ;
        }
        else if (responseCode == SIP_OK_CODE &&
                (getState() == CONNECTION_DISCONNECTED || mbCancelling))
        {
            // Otherside picked up after we decided to drop the call
            processInviteResponseHangingUp(response) ;
        }
        else if (responseCode == SIP_OK_CODE)
        {
            // Normal case -- other side picked up
            processInviteResponseNormal(response) ;
        }
        else if (responseCode >= SIP_MULTI_CHOICE_CODE &&
                responseCode < SIP_BAD_REQUEST_CODE)
        {
            // Redirect response
            processInviteResponseRedirect(response) ;
        }
        else
        {
            // Unknown/Unexpected response
            processInviteResponseUnknown(response) ;
        }


        /*
         * If this was part of a transfer, fire off transfer status events
         */
        int currentState = getState();
        if(previousState != currentState &&
            !mOriginalCallConnectionAddress.isNull())
        {
            if(currentState == CONNECTION_ESTABLISHED ||
                currentState == CONNECTION_FAILED ||
                currentState == CONNECTION_ALERTING)
            {
                UtlString originalCallId;
                mpCall->getOriginalCallId(originalCallId);
                CpMultiStringMessage transfereeStatus(CallManager::CP_TRANSFEREE_CONNECTION_STATUS,
                        originalCallId.data(),
                        mOriginalCallConnectionAddress.data(),
                        NULL, NULL, NULL,
                        currentState, responseCode);
                mpCallManager->postMessage(transfereeStatus);
            }
        }
    }
    else
    {
        if ((responseCode >= SIP_OK_CODE) &&
                (mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) > sequenceNum))    // TODO: Deal with wrap
        {
            // This is an old message / resend

            // Construct an ACK
            SipMessage sipRequest;
            sipRequest.setAckData(response, inviteMsg, mLocalContact,
                mSessionReinviteTimer);

            // Set the route field
            if(!mRouteField.isNull())
            {
                sipRequest.setRouteField(mRouteField.data());
            }
            // Send the ACK message
            send(sipRequest);

        }
        else
        {
            OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring INVITE response: CSeq %d != %d",
                    sequenceNum,
                    mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE)) ;
        }
    }
} // End of processInviteResponse


void SipConnection::processReferResponse(const SipMessage* response)
{
    int sequenceNum;
    UtlString sequenceMethod;

    int state = CONNECTION_UNKNOWN;
    int cause = CONNECTION_CAUSE_UNKNOWN;
    int responseCode = response->getResponseStatusCode();
    response->getCSeqField(&sequenceNum, &sequenceMethod);

    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_REFER) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_REFER) ;
        }

        // 2xx class responses are no-ops as it only indicates
        // the transferee is attempting to INVITE the transfer target
        if(responseCode == SIP_OK_CODE)
        {
            state = CONNECTION_DIALING;
            cause = CONNECTION_CAUSE_NORMAL;

            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_ACCEPTED) ;
        }
        else if(responseCode == SIP_ACCEPTED_CODE)
        {
            state = CONNECTION_OFFERING;
            cause = CONNECTION_CAUSE_NORMAL;

            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_ACCEPTED) ;
        }
        else if(responseCode == SIP_DECLINE_CODE)
        {
            state = CONNECTION_FAILED;
            cause = CONNECTION_CAUSE_CANCELLED;

            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
        }
        else if(responseCode == SIP_BAD_METHOD_CODE ||
            responseCode == SIP_UNIMPLEMENTED_METHOD_CODE)
        {
            state = CONNECTION_FAILED;
            cause = CONNECTION_CAUSE_INCOMPATIBLE_DESTINATION;

            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
        }
        else if(responseCode >= SIP_MULTI_CHOICE_CODE)
        {
            state = CONNECTION_FAILED;
            cause = CONNECTION_CAUSE_BUSY;

            fireSipXCallEvent(CALLSTATE_TRANSFER_EVENT, CALLSTATE_CAUSE_TRANSFER_FAILURE) ;
        }

        // We change state on the target/consultative call
        if(responseCode >= SIP_OK_CODE)
        {
            // Signal the connection in the target call with the final status
            UtlString targetCallId;
            UtlString toField;
            mToUrl.toString(toField);
            mpCall->getTargetCallId(targetCallId);
            CpMultiStringMessage transferControllerStatus(CallManager::CP_TRANSFER_CONNECTION_STATUS,
                targetCallId.data(), toField.data(),
                NULL, NULL, NULL,
                state, cause);
#ifdef TEST_PRINT
            osPrintf("SipConnection::processResponse REFER posting CP_TRANSFER_CONNECTION_STATUS to call: %s\n",
                targetCallId.data());
#endif
            mpCallManager->postMessage(transferControllerStatus);

            // Drop this connection, the transfer succeeded
            // Do the drop at the last possible momment so that
            // both calls have some overlap.
            if(responseCode == SIP_OK_CODE) doHangUp();
        }

#ifdef TEST_PRINT
        // We ignore provisional response
        // as they do not indicate any state change
        else
        {
            osPrintf("SipConnection::processResponse ignoring REFER response %d\n",
                responseCode);
        }
#endif
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring REFER response: CSeq %d != %d",
                sequenceNum,
                mCSeqMgr.getCSeqNumber(CSEQ_ID_REFER)) ;
    }
} // End of processReferResponse

void SipConnection::processNotifyResponse(const SipMessage* response)
{
    int sequenceNum;
    UtlString sequenceMethod;
    int responseCode = response->getResponseStatusCode();
    response->getCSeqField(&sequenceNum, &sequenceMethod);

    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_NOTIFY) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_NOTIFY) ;
        }
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring NOTIFY response: CSeq %d != %d",
                sequenceNum,
                mCSeqMgr.getCSeqNumber(CSEQ_ID_NOTIFY)) ;
    }
}

void SipConnection::processOptionsResponse(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();
    UtlString responseText;
    int sequenceNum;
    UtlString sequenceMethod;

    response->getResponseStatusText(&responseText);
    response->getCSeqField(&sequenceNum, &sequenceMethod);

    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_OPTIONS) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_OPTIONS) ;
        }

        if (responseCode == SIP_OK_CODE)
        {
            response->getAllowField(mAllowedRemote);
        }
        else if (responseCode > SIP_OK_CODE)
        {
            // It seems the other side does not support OPTIONS
            response->getAllowField(mAllowedRemote);

            // Assume default minimum
            if(mAllowedRemote.isNull())
            {
                mAllowedRemote = "INVITE, BYE, ACK, CANCEL, REFER";
            }
        }
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring OPTIONS response: CSeq %d != %d",
                sequenceNum,
                mCSeqMgr.getCSeqNumber(CSEQ_ID_OPTIONS)) ;
    }
} // End of processOptionsResponse


void SipConnection::processByeResponse(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();
    int sequenceNum;
    UtlString sequenceMethod;
    response->getCSeqField(&sequenceNum, &sequenceMethod);

    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_INVITE) ;
        }

        if(responseCode == SIP_TRYING_CODE)
        {
            // Set a timer so that if we get a 100 and never get a
            // final response, we still tear down the connection

            UtlString localAddress;
            UtlString remoteAddress;
            UtlString callId;
            getFromField(&localAddress);
            getToField(&remoteAddress);
            getCallId(&callId);

            CpMultiStringMessage* expiredBye =
                new CpMultiStringMessage(CallManager::CP_FORCE_DROP_CONNECTION,
                callId.data(), remoteAddress.data(), localAddress.data());
            OsTimer* timer = new OsTimer((mpCallManager->getMessageQueue()),
                (intptr_t)expiredBye);

            // Convert from mSeconds to uSeconds
            OsTime timerTime(sipUserAgent->getSipStateTransactionTimeout() / 1000, 0);
            timer->oneshotAfter(timerTime);
        }
        else if(responseCode >= SIP_2XX_CLASS_CODE)
        {
            // All final codes are treated the same, since if attempting the
            // BYE fails, for safety, we need to terminate the call anyway.
            // Stop sending & receiving RTP
            if (mpMediaInterface != NULL)
            {
                mpMediaInterface->stopRtpSend(mConnectionId);
                mpMediaInterface->stopRtpReceive(mConnectionId);
                fireAudioStopEvents() ;
            }
        }
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring BYE response: CSeq %d != %d",
                sequenceNum,
                mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE)) ;
    }

} // End of processByeResponse

void SipConnection::processCancelResponse(const SipMessage* response)
{
    int responseCode = response->getResponseStatusCode();
    int sequenceNum;
    UtlString sequenceMethod;
    response->getCSeqField(&sequenceNum, &sequenceMethod) ;

    if (mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE) == sequenceNum)
    {
        if (responseCode >= SIP_OK_CODE)
        {
            mCSeqMgr.endTransaction(CSEQ_ID_INVITE) ;
        }

        if(responseCode == SIP_TRYING_CODE)
        {
            // Set a timer so that if we get a 100 and never get a
            // final response, we still tear down the connection

            UtlString localAddress;
            UtlString remoteAddress;
            UtlString callId;
            getFromField(&localAddress);
            getToField(&remoteAddress);
            getCallId(&callId);

            CpMultiStringMessage* expiredBye =
                new CpMultiStringMessage(CallManager::CP_FORCE_DROP_CONNECTION,
                callId.data(), remoteAddress.data(), localAddress.data());

            OsTimer* timer = new OsTimer((mpCallManager->getMessageQueue()),
                (intptr_t)expiredBye);
            // Convert from mSeconds to uSeconds
            OsTime timerTime(sipUserAgent->getSipStateTransactionTimeout() / 1000, 0);
            timer->oneshotAfter(timerTime);
        }
        else if(responseCode >= SIP_2XX_CLASS_CODE)
        {
            // Stop sending & receiving RTP
            if (mpMediaInterface != NULL)
            {
                mpMediaInterface->stopRtpSend(mConnectionId);
                mpMediaInterface->stopRtpReceive(mConnectionId);
                fireAudioStopEvents() ;
            }
        }
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG, "Ignoring CANCEL response: CSeq %d != %d",
                sequenceNum,
                mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE)) ;
    }
} // End of processCancelResponse



void SipConnection::setCallerId()
{
    UtlString newCallerId;

    if(inviteMsg)
    {
        UtlString user;
        UtlString addr;
        //UtlString fromProtocol;
        Url uri;
        UtlString userLabel;
        //int port;
        if(!inviteFromThisSide)
        {
            inviteMsg->getFromUrl(mToUrl);
            uri = mToUrl;
            inviteMsg->getToUrl(mFromUrl);
            inviteMsg->getRequestUri(&mRemoteUriStr);

#ifdef TEST_PRINT
            UtlString fromString;
            UtlString toString;
            mToUrl.toString(toString);
            mFromUrl.toString(fromString);
            osPrintf("SipConnection::setCallerId INBOUND to: %s from: %s\n",
                toString.data(), fromString.data());
#endif
        }
        else
        {
            inviteMsg->getToUrl(mToUrl);
            uri = mToUrl;
            inviteMsg->getFromUrl(mFromUrl);
            inviteMsg->getRequestUri(&mLocalUriStr);

#ifdef TEST_PRINT
            UtlString fromString;
            UtlString toString;
            mToUrl.toString(toString);
            mFromUrl.toString(fromString);
            osPrintf("SipConnection::setCallerId INBOUND to: %s from: %s\n",
                toString.data(), fromString.data());
#endif
        }

        uri.getHostAddress(addr);
        //port = uri.getHostPort();
        //uri.getUrlParameter("transport", fromProtocol);
        uri.getUserId(user);
        uri.getDisplayName(userLabel);
        // Set the caller ID
        // Use the first that is not empty string of:
        // user label
        // user id
        // host address
        userLabel.strip(UtlString::both);
#ifdef TEST_PRINT
        osPrintf("SipConnection::setCallerid label: %s user %s address: %s\n",
            userLabel.data(), user.data(), addr.data());
#endif

        if(!userLabel.isNull())
        {
            newCallerId.append(userLabel.data());
        }
        else
        {
            user.strip(UtlString::both);
            if(!user.isNull())
            {
                newCallerId.append(user.data());
            }
            else
            {
                addr.strip(UtlString::both);
                newCallerId.append(addr.data());
            }
        }
    }
    Connection::setCallerId(newCallerId.data());
}

UtlBoolean SipConnection::processNewFinalMessage(SipUserAgent* sipUa,
                                                 OsMsg* eventMessage)
{
    // This static method is called if no connection was found.

    UtlBoolean sendSucceeded = FALSE;

    int msgType = eventMessage->getMsgType();
    int msgSubType = eventMessage->getMsgSubType();
    const SipMessage* sipMsg = NULL;

    if(msgType == OsMsg::PHONE_APP &&
        msgSubType == CallManager::CP_SIP_MESSAGE)
    {
        sipMsg = ((SipMessageEvent*)eventMessage)->getMessage();
        int port;
        int sequenceNum;
        UtlString method;
        UtlString address;
        UtlString protocol;
        UtlString user;
        UtlString userLabel;
        UtlString tag;
        UtlString sequenceMethod;
        sipMsg->getToAddress(&address, &port, &protocol, &user, &userLabel, &tag);
        sipMsg->getCSeqField(&sequenceNum, &method);

        int responseCode = sipMsg->getResponseStatusCode();

        // INVITE to create a connection
        //if to tag is already set then return 481 error
        if(method.compareTo(SIP_INVITE_METHOD) == 0 &&
            !tag.isNull() &&
            responseCode == SIP_OK_CODE)
        {
            UtlString fromField;
            UtlString toField;
            UtlString uri;
            UtlString callId;

            sipMsg->getFromField(&fromField);
            sipMsg->getToField(&toField);
            sipMsg->getContactUri( 0 , &uri);
            if(uri.isNull())
                uri.append(toField.data());

            sipMsg->getCallIdField(&callId);
            SipMessage* ackMessage = new SipMessage();
            ackMessage->setAckData(uri,
                fromField,
                toField,
                callId,
                sequenceNum);
            sendSucceeded = sipUa->send(*ackMessage, 0, 0);
            delete ackMessage;

            if (sendSucceeded)
            {
                SipMessage* byeMessage = new SipMessage();
                byeMessage->setByeData(uri,
                    fromField,
                    toField,
                    callId,
                    NULL,
                    sequenceNum + 1);

                sendSucceeded = sipUa->send(*byeMessage, 0, 0);
                delete byeMessage;
            }
        }

    }
    return sendSucceeded;
}


void SipConnection::setContactType(SIPX_CONTACT_TYPE eType, Url* pToUrl)
{
    mContactType = eType ;
    if (mpMediaInterface != NULL)
    {
        mpMediaInterface->setContactType(mConnectionId, eType, mContactId) ;
    }

    UtlString localContact ;
    buildLocalContact(mFromUrl, localContact, pToUrl);
    mLocalContact = localContact ;
}


void SipConnection::setVoiceQualityReportTarget(const char* szTargetSipUrl)
{
    assert(szTargetSipUrl) ;
    if (szTargetSipUrl)
    {
        mVoiceQualityReportTarget = szTargetSipUrl ;
    }
}


void SipConnection::sendVoiceQualityReport(const char* szTargetSipUrl)
{
    assert(szTargetSipUrl) ;

    if (mpMediaInterface && mpCall && szTargetSipUrl)
    {
        UtlString report ;
        UtlString callId ;
        char buffer[256] ;
        UtlString notifyCallId;

        mpCall->getCallId(callId);
        if ((mpMediaInterface->generateVoiceQualityReport(mConnectionId,
                callId, report) == OS_SUCCESS) && report.length() > 0)
        {
            Url uriTargetURL(szTargetSipUrl) ;
            UtlString uri ;
            uriTargetURL.getUri(uri) ;

            SipMessage message ;

            // Try to cram this message into a UDP packet
            // message.setUseShortFieldNames(true) ;
            message.setMaxForwards(9) ;

            // Generate a shorter call id
            OsTime current_time;
            OsDateTime::getCurTime(current_time);
            int64_t start_time =
                    ((int64_t) current_time.seconds()) * 1000000 + current_time.usecs();

            // Get the process ID.
            int process_id;
            process_id = OsProcess::getCurrentPID();

            // Get the host identity.
            UtlString thisHost;
            OsSocket::getHostIp(&thisHost);
            // Ensure it does not contain @.
            thisHost.replace('@','*');

            // Compose the static fields.
            sprintf(buffer, "%d_%" PRId64 "d_%s",
                    process_id, start_time, thisHost.data());
            // Hash them.
            NetMd5Codec encoder;
            encoder.encode(buffer, notifyCallId);
            // Truncate the hash to 16 characters.
            notifyCallId.remove(16);

            UtlString from ;
            mpCallManager->getLocalAddress(from) ;

            message.setNotifyData(uri,
                    from,
                    szTargetSipUrl,
                    notifyCallId,
                    sRandom->rand() % 32768,
                    "vq-rtcpxr",
                    NULL,
                    NULL,
                    mLocalContact,
                    NULL) ;

             HttpBody* pBody = new HttpBody(report, report.length(),
                    CONTENT_TYPE_VQ_RTCP_XR);

            message.setBody(pBody) ;
            message.setContentLength(pBody->length()) ;
            message.setContentType(CONTENT_TYPE_VQ_RTCP_XR) ;

            /*
             * Figure out transport type
             */
            SIPX_TRANSPORT_TYPE protocol = TRANSPORT_UDP;
            UtlString localIp ;
            int port ;
            sipUserAgent->getLocalAddress(&localIp, &port, protocol);
            UtlString toField;
            message.getToField(&toField);
            if (toField.contains("sips:") || toField.contains("transport=tls"))
            {
                protocol = TRANSPORT_TLS;
            }
            else if (toField.contains("transport=tcp"))
            {
                protocol = TRANSPORT_TCP;
            }
            else if (toField.contains("transport="))
            {
                protocol = TRANSPORT_CUSTOM;
            }

            /*
             * If sending via UDP, make sure that we aren't going fragment,
             * if so, switch over to TCP.
             */
            if (protocol == TRANSPORT_UDP)
            {
                UtlString test ;
                int length ;
                message.getBytes(&test, &length) ;

                if (length > 1200)
                {
                    uriTargetURL.setUrlParameter("transport", "tcp") ;
                    uriTargetURL.getUri(uri) ;

                    message.setNotifyData(uri,
                            from,
                            uriTargetURL.toString(),
                            notifyCallId,
                            sRandom->rand() % 32768,
                            "vq-rtcpxr",
                            NULL,
                            NULL,
                            mLocalContact,
                            NULL) ;
                }
            }

            send(message, NULL, NULL, true) ;
        }
        else
        {
            OsSysLog::add(FAC_CP, PRI_ERR, "Unable to generate voice quality report for %s/%d",
                    callId.data(), mConnectionId) ;
        }
    }
}

void SipConnection::cacheMediaPassThroughData(CpMediaInterface::MEDIA_STREAM_TYPE mediaType,
                                              int mediaTypeStreamIndex,
                                              UtlString& receiveAddress,
                                              int rtpPort,
                                              int rtcpPort)
{
    // For now only support a single stream
    OsSysLog::add(FAC_CP, PRI_DEBUG, "SipConnection::cacheMediaPassThroughData mpPassThroughData cached");
    assert(mpPassThroughData == NULL);
    mpPassThroughData = new MediaStreamPassThroughData(mediaType, mediaTypeStreamIndex, receiveAddress, rtpPort, rtcpPort);
}

/* ============================ ACCESSORS ================================= */

UtlContainableType SipConnection::getContainableType() const
{
    return SipConnection::TYPE;
}

UtlBoolean SipConnection::getRemoteAddress(UtlString* remoteAddress) const
{
    return(getRemoteAddress(remoteAddress, FALSE));
}


UtlBoolean SipConnection::getRemoteAddress(UtlString* remoteAddress,
                                           UtlBoolean leaveFieldParmetersIn) const
{
    // leaveFieldParmetersIn gives the flexability of getting the
    // tag when the connection is still an early dialog

    int remoteState = getState();
    // If this is an early dialog or we explicily want the
    // field parameters
    if(leaveFieldParmetersIn ||
        remoteState == CONNECTION_ESTABLISHED ||
        remoteState == CONNECTION_DISCONNECTED ||
        remoteState == CONNECTION_FAILED ||
        remoteState == CONNECTION_UNKNOWN)
    {
        // Cast as the toString method is not const
        ((Url)mToUrl).toString(*remoteAddress);
    }

    else
    {
        Url toNoFieldParameters(mToUrl);
        toNoFieldParameters.removeFieldParameters();
        toNoFieldParameters.toString(*remoteAddress);
    }

    if (*remoteAddress == "" || *remoteAddress == "sip:" || *remoteAddress == "sips:")
    {
        *remoteAddress = mLastToAddress;
    }

#ifdef TEST_PRINT
    osPrintf("SipConnection::getRemoteAddress address: %s\n",
        remoteAddress->data());
#endif

    return(inviteMsg != NULL);
}

UtlBoolean SipConnection::isSameRemoteAddress(Url& remoteAddress) const
{
    return(isSameRemoteAddress(remoteAddress, TRUE));
}

UtlBoolean SipConnection::isSameRemoteAddress(Url& remoteAddress,
                                              UtlBoolean tagsMustMatch) const
{
    UtlBoolean isSame = FALSE;

    int remoteState = getState();
    // If this is an early dialog or we explicily want the
    // field parameters
    Url mToUrlTmp(mToUrl);

    if(tagsMustMatch ||
        remoteState == CONNECTION_ESTABLISHED ||
        remoteState == CONNECTION_DISCONNECTED ||
        remoteState == CONNECTION_FAILED ||
        remoteState == CONNECTION_UNKNOWN)
    {
        isSame = SipMessage::isSameSession(mToUrlTmp, remoteAddress);
    }
    else
    {
        // The do not requrie a tag in the remote address
        isSame = SipMessage::isSameSession(remoteAddress, mToUrlTmp);
    }

    return(isSame);
}

UtlBoolean SipConnection::getSession(SipSession& session)
{
    UtlString callId;
    getCallId(&callId);
    SipSession ssn;
    ssn.setCallId(callId.data());
    ssn.setLastFromCseq(mCSeqMgr.getCSeqNumber(CSEQ_ID_INVITE));
    ssn.setLastToCseq(lastRemoteSequenceNumber);
    ssn.setFromUrl(mFromUrl);
    ssn.setToUrl(mToUrl);
    UtlString localContact;
    ssn.setLocalContact(Url(mLocalContact.data(), FALSE));

    if (!mRemoteUriStr.isNull())
        ssn.setRemoteRequestUri(mRemoteUriStr);
    if (!mLocalUriStr.isNull())
        ssn.setLocalRequestUri(mLocalUriStr);
    if (!mContactUriStr.isNull())
        ssn.setContactRequestUri(mContactUriStr);

    session = ssn;
    return(TRUE);
}

int SipConnection::getNextCseq()
{
    int iCSeq ;
    mCSeqMgr.startTransaction("UNKNOWN", iCSeq) ;
    return(iCSeq);
}


OsStatus SipConnection::getInviteHeaderValue(const char* headerName, int headerIndex, UtlString& headerValue) const
{
    OsStatus status = OS_INVALID_STATE;
    if(inviteMsg)
    {
        const char* valueCharPtr = inviteMsg->getHeaderValue(headerIndex, headerName);
        OsSysLog::add(FAC_CP, PRI_DEBUG,
            "SipConnection::getInviteHeaderValue(name=\"%s\", index=%d value=\"%s\")",
            headerName, headerIndex, 
            valueCharPtr ? valueCharPtr : "NULL");
        if(valueCharPtr)
        {
            headerValue = valueCharPtr;
            status = OS_SUCCESS;
        }
        else
        {
            headerValue = "";
            status = OS_NOT_FOUND;

#ifdef TEST_PRINT
            UtlString msgString;
            int msgLen;
            inviteMsg->getBytes(&msgString, &msgLen);
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                "SipConnection::getInviteHeaderValue INVITE does not contain header=\"%s\"\n%s",
                headerName, msgString.data());
#endif
        }
    }
    else
    {
        OsSysLog::add(FAC_CP, PRI_DEBUG,
            "SipConnection::getInviteHeaderValue NULL INVITE");
    }

    return(status);
}

OsStatus SipConnection::getFromField(UtlString* fromField)
{
    OsStatus ret = OS_SUCCESS;

    UtlString host;
    mFromUrl.getHostAddress(host);
    if(host.isNull())
        ret = OS_NOT_FOUND;

    mFromUrl.toString(*fromField);

#ifdef TEST_PRINT
    osPrintf("SipConnection::getFromAddress address: %s\n",
        fromField->data());
#endif

    return ret;
}

OsStatus SipConnection::getToField(UtlString* toField)
{
    OsStatus ret = OS_SUCCESS;
    UtlString host;
    mToUrl.getHostAddress(host);
    if (host.isNull())
        ret = OS_NOT_FOUND;

    mToUrl.toString(*toField);

#ifdef TEST_PRINT
    osPrintf("SipConnection::getToAddress address: %s\n",
        toField->data());
#endif

    return ret;
}


/* ============================ INQUIRY =================================== */

UtlBoolean SipConnection::isInstanceOf(const UtlContainableType type) const
{
    return ((type != NULL)
            && (getContainableType() != NULL)
            && ((type == getContainableType()) ||
                Connection::isInstanceOf(type)));
}

UtlBoolean SipConnection::willHandleMessage(OsMsg& eventMessage) const
{
    int msgType = eventMessage.getMsgType();
    int msgSubType = eventMessage.getMsgSubType();
    UtlBoolean handleMessage = FALSE;
    const SipMessage* sipMsg = NULL;
    int messageType;

    // Do not handle message if marked for deletion
    if (isMarkedForDeletion())
    {
        OsSysLog::add(FAC_CP, PRI_DEBUG,
                "SipConnection::willHandleMessage connection marked for deletion, not handling SIP message");
        // already set handleMessage = false;
    }
    else if(inviteMsg &&
            msgType == OsMsg::PHONE_APP &&
            msgSubType == CallManager::CP_SIP_MESSAGE)
    {
        sipMsg = ((SipMessageEvent&)eventMessage).getMessage();
        messageType = ((SipMessageEvent&)eventMessage).getMessageStatus();

#ifdef TEST_PRINT
        UtlString dumpMessage;
        int msgLen;
        inviteMsg->getBytes(&dumpMessage, &msgLen);
        OsSysLog::add(FAC_CP, PRI_DEBUG,
            "Invite from this side: %s message:\n%s", 
            inviteFromThisSide ? "T" : "F",
            dumpMessage.data());
#endif

        SipDialog thisDialog(inviteMsg, inviteFromThisSide);

        if(sipMsg == NULL)
        {
            OsSysLog::add(FAC_CP, PRI_ERR,
                "SipConnection::willHandleMessage PHONE_APP CP_SIP_MESSAGE with NULL message");
        }
        // If the callId, To and From tags match it belongs to this message
        else if(thisDialog.isSameDialog(*sipMsg))
        {
           handleMessage = TRUE;
#ifdef TEST_PRINT
           OsSysLog::add(FAC_CP, PRI_DEBUG,
               "SipConnection::willHandleMessage same dialog");
#endif
        }
        else if(thisDialog.isEarlyDialogFor(*sipMsg))
        {
           handleMessage = TRUE;
#ifdef TEST_PRINT
           OsSysLog::add(FAC_CP, PRI_DEBUG,
               "SipConnection::willHandleMessage same is early dialog");
#endif
        }
        else if(thisDialog.wasEarlyDialogFor(*sipMsg))
        {
           handleMessage = TRUE;
#ifdef TEST_PRINT
           OsSysLog::add(FAC_CP, PRI_DEBUG,
               "SipConnection::willHandleMessage same was early dialog");
#endif
        }

#ifdef TEST_PRINT
        else
        {
           UtlString dialogString;
           thisDialog.toString(dialogString);
           OsSysLog::add(FAC_CP, PRI_DEBUG,
               "SipConnection::willHandleMessage NOT same dialog:\%s",
               dialogString.data());
        }
#endif
    }
    else if(inviteMsg == NULL)
    {
        OsSysLog::add(FAC_CP, PRI_DEBUG,
                "SipConnection::willHandleMessage NULL invite message.");
    }

    return(handleMessage);
}

UtlBoolean SipConnection::isConnection(const char* callId,
                                       const char* toTag,
                                       const char* fromTag,
                                       UtlBoolean  strictCompare) const
{
    UtlBoolean matches = FALSE;

    // Do not handle message if marked for deletion
    if (isMarkedForDeletion())
        return false ;

    if(inviteMsg)
    {
        UtlString thisCallId;
        inviteMsg->getCallIdField(&thisCallId);

        if(thisCallId.compareTo(callId) == 0)
        {
            UtlString thisFromTag;
            UtlString thisToTag;
            mFromUrl.getFieldParameter("tag", thisFromTag);
            mToUrl.getFieldParameter("tag", thisToTag);

            if (strictCompare)
            {
                // for transfer target in a consultative call,
                // thisFromTag is remote, thisToTag is local
                if((thisFromTag.compareTo(toTag) == 0 &&
                    thisToTag.compareTo(fromTag) == 0 ))
                {
                    matches = TRUE;
                }
            }

            // Do a sloppy comparison
            //  Allow a match either way
            else
            {
                if((thisFromTag.compareTo(fromTag) == 0 &&
                    thisToTag.compareTo(toTag) == 0 ) ||
                    (thisFromTag.compareTo(toTag) == 0 &&
                    thisToTag.compareTo(fromTag) == 0 ))
                {
                    matches = TRUE;
                }
            }
#ifdef TEST_PRINT
            osPrintf("SipConnection::isConnection toTag=%s\n\t fromTag=%s\n\t thisToTag=%s\n\t thisFromTag=%s\n\t matches=%d\n",
                toTag, fromTag, thisToTag.data(), thisFromTag.data(), (int)matches) ;
#endif
        }
    }

    return(matches);
}

// Determine if the other side of this connection (remote side)
// supports the given method
UtlBoolean SipConnection::isMethodAllowed(const char* method)
{
    // Eventually we may want to send an OPTIONS request if
    // we do not know.  For now assume that the other side
    // sent an Allowed header field in the final response.
    // If we do not know (mAllowedRemote is NULL) assume
    // it is supported.
    UtlBoolean methodSupported = TRUE;
    int methodIndex = mAllowedRemote.index(method);
    if(methodIndex >=0)
    {
        methodSupported = TRUE;
    }

    // We explicitly know that it does not support this method
    else if(!mAllowedRemote.isNull())
    {
        methodSupported = FALSE;
    }
#ifdef TEST_PRINT
    osPrintf("SipConnection::isMethodAllowed method: %s allowed: %s return: %d index: %d null?: %d\n",
        method, mAllowedRemote.data(), methodSupported, methodIndex,
        mAllowedRemote.isNull());
#endif

    return(methodSupported);
}

void SipConnection::setSecurity(const SIPXTACK_SECURITY_ATTRIBUTES* const pSecurity)
{
    mpSecurity = (SIPXTACK_SECURITY_ATTRIBUTES*)pSecurity;
}

UtlBoolean SipConnection::isLocallyInitiatedRemoteHold() const
{
    return mbLocallyInitiatedRemoteHold ;
}

UtlBoolean SipConnection::isInviteFromThisSide() const
{
    return(inviteFromThisSide);
}

/* //////////////////////////// PROTECTED ///////////////////////////////// */

UtlBoolean SipConnection::prepareInviteSdpForSend(SipMessage* pMsg,
                                            int         connectionId,
                                            const void* pSecurityAttributes)
{
    UtlBoolean bRet = TRUE;

    SdpBody* pBody = (SdpBody*)pMsg->getSdpBody();
    if (pBody == NULL)
    {
       return FALSE;
    }

    if (RTP_TCP_ROLE_ACTPASS != mRtpTcpRole)
    {
        pBody->setRtpTcpRole(mRtpTcpRole);
    }

    // Now, if needed, encrypt the SdpBody, replace it with an S/MIME body
    if (pSecurityAttributes)
    {
        pMsg->setEventData(mpCallManager);
        mpSecurity = (SIPXTACK_SECURITY_ATTRIBUTES*)pSecurityAttributes;
#ifdef HAVE_NSS
        int rc = -1;
        rc = P12Wrapper_Init((char*)mpSecurity->getCertDbLocation(), "");
#endif
        pMsg->setSecurityAttributes((SIPXTACK_SECURITY_ATTRIBUTES*)pSecurityAttributes);
        if (pMsg->smimeEncryptSdp(mpCallManager))
        {
            SIPX_SECURITY_INFO info;
            memset(&info, 0, sizeof(SIPX_SECURITY_INFO));
            info.nSize = sizeof(SIPX_SECURITY_INFO);
            info.event = SECURITY_ENCRYPT;
            info.cause = SECURITY_CAUSE_ENCRYPT_SUCCESS;
            info.nCertificateSize = mpSecurity->getSmimeKeyLength();
            info.pCertificate = (void*)mpSecurity->getSmimeKey();
            info.szSRTPkey = (char*)mpSecurity->getSrtpKey();
            UtlString callId;
            pMsg->getCallIdField(&callId);
            info.callId = (char*)callId.data();

            fireSipXSecurityEvent(&info);
            bRet = TRUE;
        }
        else
        {
            UtlString bodyBytes;
            int bodyLength;

            pMsg->getBody()->getBytes(&bodyBytes, &bodyLength);
            pMsg->setContentLength(bodyLength);
            bRet = FALSE;
        }
    }
    return bRet;
}

void SipConnection::setMediaDestination(const char*    hostAddress,
                                        int            audioRtpPort,
                                        int            audioRtcpPort,
                                        int            videoRtpPort,
                                        int            videoRtcpPort,
                                        const SdpBody* pRemoteBody)
{
    UtlBoolean bSetDestination = FALSE ;

    /*
     * Assumption: that ICE is either enabled for both audio and video or not
     * at all.  If you attempt to mix, this won't work correctly.  To fix
     * this, we need to break setConnectionDestination(...) into two methods
     * -- one for audio and one for video.
     */

    if (mpMediaInterface && hostAddress && (strcasecmp(hostAddress, "0.0.0.0") != 0))
    {
        if (pRemoteBody && mpCallManager->isIceEnabled())
        {
            int         candidateIds[MAX_ADDRESS_CANDIDATES] ;
            UtlString   transportIds[MAX_ADDRESS_CANDIDATES] ;
            UtlString   transportTypes[MAX_ADDRESS_CANDIDATES] ;
            uint64_t    qValues[MAX_ADDRESS_CANDIDATES];
            UtlString   candidateIps[MAX_ADDRESS_CANDIDATES] ;
            int         candidatePorts[MAX_ADDRESS_CANDIDATES] ;
            int         nCandidates = 0 ;

            // Check for / add audio candidate addresses
            if (pRemoteBody->getCandidateAttributes(SDP_AUDIO_MEDIA_TYPE,
                    MAX_ADDRESS_CANDIDATES,
                    candidateIds,
                    transportIds,
                    transportTypes,
                    qValues,
                    candidateIps,
                    candidatePorts,
                    nCandidates))
            {
                bSetDestination = TRUE ;

                int lastId = -1 ;
                for (int i=0; i<nCandidates; i++)
                {
                    if (transportTypes[i].compareTo("UDP") == 0)
                    {
                        if (candidateIds[i] != lastId)
                        {
                            if (mpMediaInterface->addAudioRtpConnectionDestination(
                                    mConnectionId,
                                    qValues[i],
                                    candidateIps[i],
                                    candidatePorts[i]) != OS_SUCCESS)
                            {
                                OsSysLog::add(FAC_NET, PRI_ERR,
                                        "Failed to set audio rtp media destination (%d %s %s:%d)",
                                        candidateIds[i], transportIds[i].data(),
                                        candidateIps[i].data(), candidatePorts[i]) ;

                            }
                        }
                        else
                        {
                            if (mpMediaInterface->addAudioRtcpConnectionDestination(
                                    mConnectionId,
                                    qValues[i],
                                    candidateIps[i],
                                    candidatePorts[i]) != OS_SUCCESS)
                            {
                                OsSysLog::add(FAC_NET, PRI_ERR,
                                        "Failed to set audio rtcp media destination (%d %s %s:%d)",
                                        candidateIds[i], transportIds[i].data(),
                                        candidateIps[i].data(), candidatePorts[i]) ;
                            }
                        }
                        lastId = candidateIds[i] ;
                    }
                }
            }

            // Check for / add video candidate addresses
            if (pRemoteBody->getCandidateAttributes(SDP_VIDEO_MEDIA_TYPE,
                    MAX_ADDRESS_CANDIDATES,
                    candidateIds,
                    transportIds,
                    transportTypes,
                    qValues,
                    candidateIps,
                    candidatePorts,
                    nCandidates))
            {
                bSetDestination = TRUE ;

                int lastId = -1 ;
                for (int i=0; i<nCandidates; i++)
                {
                    if (transportTypes[i].compareTo("UDP") == 0)
                    {
                        if (candidateIds[i] != lastId)
                        {
                            if (mpMediaInterface->addVideoRtpConnectionDestination(
                                    mConnectionId,
                                    qValues[i],
                                    candidateIps[i],
                                    candidatePorts[i]) != OS_SUCCESS)
                            {
                                OsSysLog::add(FAC_NET, PRI_ERR,
                                        "Failed to set video rtp media destination (%d %s %s:%d)",
                                        candidateIds[i], transportIds[i].data(),
                                        candidateIps[i].data(), candidatePorts[i]) ;

                            }
                        }
                        else
                        {
                            if (mpMediaInterface->addVideoRtcpConnectionDestination(
                                    mConnectionId,
                                    qValues[i],
                                    candidateIps[i],
                                    candidatePorts[i]) != OS_SUCCESS)
                            {
                                OsSysLog::add(FAC_NET, PRI_ERR,
                                        "Failed to set video rtcp media destination (%d %s %s:%d)",
                                        candidateIds[i], transportIds[i].data(),
                                        candidateIps[i].data(), candidatePorts[i]) ;
                            }
                        }
                        lastId = candidateIds[i] ;
                    }
                }
            }
        }

        if (!bSetDestination)
        {
            mpMediaInterface->setConnectionDestination(mConnectionId,
                    hostAddress,
                    audioRtpPort,
                    audioRtcpPort,
                    videoRtpPort,
                    videoRtcpPort);
        }
    }
}


void SipConnection::getRemoteUserAgent(UtlString* userAgent)
{
    *userAgent = mRemoteUserAgent;
}


void SipConnection::fireIncompatibleCodecsEvent(SdpCodecList*     pSupportedCodecs,
                                                SdpCodec**       ppMatchedCodecs,
                                                int              nMatchedCodces)
{
    bool bIncludedAudioCodecs = false ;
    bool bIncludedVideoCodec = false ;
    bool bFoundAudioCodecs = false ;
    bool bFoundVideoCodecs = false ;

    // Figure out what was advertised/we supported
    if (pSupportedCodecs->getCodecCount(MIME_TYPE_AUDIO) > 0)
    {
        bIncludedAudioCodecs = true ;
    }
    if (pSupportedCodecs->getCodecCount(MIME_TYPE_VIDEO) > 0)
    {
        bIncludedVideoCodec = true ;
    }

    // Figure out what we matched
    for (int i=0; i<nMatchedCodces; i++)
    {
        UtlString type ;
        ppMatchedCodecs[i]->getMediaType(type) ;
        if (type.compareTo(MIME_TYPE_AUDIO, UtlString::ignoreCase) == 0)
        {
            bFoundAudioCodecs = true ;
        }
        else if (type.compareTo(MIME_TYPE_VIDEO, UtlString::ignoreCase) == 0)
        {
            bFoundVideoCodecs = true ;
        }
    }

    if (bIncludedAudioCodecs && !bFoundAudioCodecs)
    {
        fireSipXMediaEvent(MEDIA_REMOTE_STOP, MEDIA_CAUSE_INCOMPATIBLE, MEDIA_TYPE_AUDIO) ;
    }

    if (bIncludedVideoCodec && !bFoundVideoCodecs)
    {
        if (mpMediaInterface->getVideoWindowDisplay() != NULL)
        {
            fireSipXMediaEvent(MEDIA_REMOTE_STOP, MEDIA_CAUSE_INCOMPATIBLE, MEDIA_TYPE_VIDEO) ;
        }
    }
}


void SipConnection::fireAudioStartEvents(SIPX_MEDIA_CAUSE cause)
{
    UtlString audioCodecName;
    UtlString videoCodecName;
    SIPX_CODEC_INFO tapiCodec;
    memset(&tapiCodec, 0, sizeof(SIPX_CODEC_INFO)) ;

    if (mpMediaInterface)
    {
        OsStatus status = mpMediaInterface->getPrimaryCodec(mConnectionId,
            audioCodecName,
            videoCodecName,
            &tapiCodec.audioCodec.iPayloadType,
            &tapiCodec.videoCodec.iPayloadType,
            tapiCodec.bIsEncrypted);

        if (status == OS_SUCCESS)
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "SipConnection::fireAudioStartEvents audio: %s payload: %d video: %s payload: %d",
                     audioCodecName.data(), tapiCodec.audioCodec.iPayloadType,
                     videoCodecName.data(), tapiCodec.videoCodec.iPayloadType);
            strncpy(tapiCodec.audioCodec.cName, audioCodecName.data(), SIPXTAPI_CODEC_NAMELEN-1);
            strncpy(tapiCodec.videoCodec.cName, videoCodecName.data(), SIPXTAPI_CODEC_NAMELEN-1);

            if (mpMediaInterface->isSendingRtpAudio(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_LOCAL_START, cause, MEDIA_TYPE_AUDIO, &tapiCodec) ;
            }
            if (mpMediaInterface->isSendingRtpVideo(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_LOCAL_START, cause, MEDIA_TYPE_VIDEO, &tapiCodec) ;
            }
            if (mpMediaInterface->isReceivingRtpAudio(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_REMOTE_START, cause, MEDIA_TYPE_AUDIO, &tapiCodec) ;
            }
            if (mpMediaInterface->isReceivingRtpVideo(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_REMOTE_START, cause, MEDIA_TYPE_VIDEO, &tapiCodec) ;
            }
        }
        else
        {
            OsSysLog::add(FAC_CP, PRI_DEBUG,
                    "SipConnection::fireAudioStartEvents getPrimaryCodec returned: %d",
                    status);
        }
    }
    else
    {
        OsSysLog::add(FAC_CP, PRI_DEBUG,
                "SipConnection::fireAudioStartEvents no mediaInterface");
    }
}


void SipConnection::fireAudioStopEvents(SIPX_MEDIA_CAUSE cause)
{
    if (mpMediaInterface)
    {
        if (mpMediaInterface->isAudioInitialized(mConnectionId))
        {
/*
            if (!mpMediaInterface->isSendingRtpAudio(mConnectionId) &&
                !mpMediaInterface->isSendingRtpAudio(mConnectionId))
            {
                mpMediaInterface->enableAudioTransport(mConnectionId, false);
            }
*/
            if (!mpMediaInterface->isSendingRtpAudio(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_LOCAL_STOP, cause, MEDIA_TYPE_AUDIO) ;
            }
            if (!mpMediaInterface->isReceivingRtpAudio(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_REMOTE_STOP, cause, MEDIA_TYPE_AUDIO) ;
            }
        }

        if (mpMediaInterface->isVideoInitialized(mConnectionId))
        {
/*
            if (!mpMediaInterface->isSendingRtpVideo(mConnectionId) &&
                !mpMediaInterface->isSendingRtpVideo(mConnectionId))
            {
                mpMediaInterface->enableVideoTransport(mConnectionId, false);
            }
*/
            if (!mpMediaInterface->isSendingRtpVideo(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_LOCAL_STOP, cause, MEDIA_TYPE_VIDEO) ;
            }
            if (!mpMediaInterface->isReceivingRtpVideo(mConnectionId))
            {
                fireSipXMediaEvent(MEDIA_REMOTE_STOP, cause, MEDIA_TYPE_VIDEO) ;
            }
        }
    }
}



/* //////////////////////////// PRIVATE /////////////////////////////////// */

void SipConnection::proceedToRinging(const SipMessage* inviteMessage,
                                     SipUserAgent* sipUserAgent, 
                                     int tagNum,
                                     int availableBehavior,
                                     int numAddresses, 
                                     UtlString hostAddresses[], 
                                     int receiveRtpPorts[],
                                     int receiveRtcpPorts[], 
                                     int receiveVideoRtpPorts[], 
                                     int receiveVideoRtcpPorts[],
                                     RTP_TRANSPORT transportTypes[],
                                     int numMatchingCodecs, 
                                     SdpCodec* matchingCodecs[], 
                                     SdpSrtpParameters* matchingSrtpParams,
                                     int totalBandwidth, 
                                     int matchingVideoFramerate)
{
    UtlString name = mpCall->getName();
#ifdef TEST_PRINT
    osPrintf("%s SipConnection::proceedToRinging\n", name.data());
#endif

    // Send back a ringing INVITE response
    SipMessage sipResponse;
    sipResponse.setInviteRingingData(inviteMessage);
    if(tagNum >= 0)
    {
        sipResponse.setToFieldTag(tagNum);
    }

    // If we have addresses to receive RTP on, assume early media
    if(numAddresses > 0)
    {
        // set early media response code
        sipResponse.setResponseFirstHeaderLine(SIP_PROTOCOL_VERSION,
                                               SIP_EARLY_MEDIA_CODE,
                                               SIP_EARLY_MEDIA_TEXT);

        // Add SDP to indicate early media
        sipResponse.addSdpBody(numAddresses, 
                               hostAddresses, 
                               receiveRtpPorts,
                               receiveRtcpPorts, 
                               receiveVideoRtpPorts, 
                               receiveVideoRtcpPorts,
                               transportTypes,
                               numMatchingCodecs, 
                               matchingCodecs, 
                               matchingSrtpParams,
                               totalBandwidth, 
                               matchingVideoFramerate,
                               inviteMessage);

        if (mRtpTransport != RTP_TRANSPORT_UDP)
        {
            // the callee must determime its TCP role,
            // which, in turn will determine the remote
            // endpoint's TCP role.
            // The tcp role will be communicated via the sdp

            // determine TCP connectivity
            // only if (not 0.0.0.0)
            if (remoteRtpAddress.compareTo("0.0.0.0") != 0)
            {
                if (mRtpTcpRole != RTP_TCP_ROLE_ACTIVE)
                {
                    mRtpTcpRole = RTP_TCP_ROLE_PASSIVE;
                }
                mpMediaInterface->setConnectionTcpRole(mConnectionId, mRtpTcpRole);
            }
        }

        prepareInviteSdpForSend(&sipResponse, mConnectionId, mpSecurity) ;
    }

    if(send(sipResponse))
    {
#ifdef TEST_PRINT
        osPrintf("INVITE Ringing sent successfully\n");
#endif
    }
    else
    {
#ifdef TEST_PRINT
        osPrintf("INVITE Ringing send failed\n");
#endif
    }
}

UtlBoolean SipConnection::send(SipMessage& message,
                               OsMsgQ* responseListener,
                               void* responseListenerData,
                               UtlBoolean bUseSendToTransportType)
{
    SIPX_TRANSPORT_DATA* pTransport = NULL;

    if (!bUseSendToTransportType)
    {
        UtlString localIp = message.getLocalIp();
        if (message.getLocalIp().length() < 1)
        {
            int port = -1;
            SIPX_TRANSPORT_TYPE protocol = TRANSPORT_UDP;

            sipUserAgent->getLocalAddress(&localIp, &port, protocol);
            UtlString toField;
            message.getToField(&toField);
            if (toField.contains("sips:") || toField.contains("transport=tls"))
            {
                protocol = TRANSPORT_TLS;
            }
            else if (toField.contains("transport=tcp"))
            {
                protocol = TRANSPORT_TCP;
            }
            else if (toField.contains("transport="))
            {
                protocol = TRANSPORT_CUSTOM;
            }

            sipUserAgent->getLocalAddress(&localIp, &port, protocol);
            message.setLocalIp(localIp);
        }
        if (false == SIPX_TRANSPORT_DATA::isCustomTransport(&mTransport))
        {
            bool bDummy;
            UtlString transport = message.getTransportName(bDummy);

            // get the transport string
            // our mTransport object has not been set yet.
            pTransport = (SIPX_TRANSPORT_DATA*)sipUserAgent->lookupExternalTransport(transport, localIp);
            if (pTransport)
            {
                mTransport = *pTransport;
            }
        }
        else
        {
            pTransport = &mTransport;
        }
    }

    return sipUserAgent->send(message, responseListener, responseListenerData, pTransport);
}

/**
* Registers a listener of this observable.
*/
void SipConnection::registerObserver(UtlObserver* observer)
{
    mObservers.insert(new UtlVoidPtr((void*) observer));
}

/**
* Removes a listener of this observable.
*/
void SipConnection::removeObserver(UtlObserver* observer)
{
    UtlVoidPtr value((void*) observer) ;
    mObservers.destroy(&value);
}

/**
* The observable calls this to notify its
* observers of a change.
*/
void SipConnection::notify(int code, void *pUserData)
{
    UtlSListIterator iterator(mObservers);
    UtlObserver* pObserver = NULL;
    UtlVoidPtr* pContainer = NULL;
    while ((pContainer = (UtlVoidPtr*)iterator()))
    {
        pObserver = (UtlObserver*)pContainer->getValue();
        pObserver->onNotify(this, code, pUserData);
    }
}




/* ============================ FUNCTIONS ================================= */

