//
// Copyright (C) 2004-2006 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// $$
///////////////////////////////////////////////////////////////////////////////
// Author: Dan Petrie (dpetrie AT SIPez DOT com)

// SYSTEM INCLUDES

// APPLICATION INCLUDES
#include <os/OsMsg.h>
#include <os/OsEventMsg.h>
#include <utl/UtlHashMapIterator.h>
#include <net/SipSubscribeServer.h>
#include <net/SipUserAgent.h>
#include <net/SipPublishContentMgr.h>
#include <net/SipSubscriptionMgr.h>
#include <net/SipSubscribeServerEventHandler.h>
#include <net/HttpBody.h>
#include <net/SipMessage.h>
#include <net/SipDialogMgr.h>


// Private class to contain event type and event specific utilities
class SubscribeServerEventData : public UtlString
{
public:
    SubscribeServerEventData();

    virtual ~SubscribeServerEventData();

    // Parent UtlString contains the eventType
    SipSubscribeServerEventHandler* mpEventSpecificHandler;
    SipUserAgent* mpEventSpecificUserAgent;
    SipPublishContentMgr* mpEventSpecificContentMgr;
    SipSubscriptionMgr* mpEventSpecificSubscriptionMgr;

private:
    //! DISALLOWED accidental copying
    SubscribeServerEventData(const SubscribeServerEventData& rSubscribeServerEventData);
    SubscribeServerEventData& operator=(const SubscribeServerEventData& rhs);
};
SubscribeServerEventData::SubscribeServerEventData()
{
    mpEventSpecificHandler = NULL;
    mpEventSpecificUserAgent = NULL;
    mpEventSpecificContentMgr = NULL;
    mpEventSpecificSubscriptionMgr = NULL;
}

SubscribeServerEventData::~SubscribeServerEventData()
{
}


// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS
// STATIC VARIABLE INITIALIZATIONS

/* //////////////////////////// PUBLIC //////////////////////////////////// */

/* ============================ CREATORS ================================== */

SipSubscribeServer* SipSubscribeServer::buildBasicServer(SipUserAgent& userAgent,
                                                         const char* eventType)
{
    SipSubscribeServer* newServer = NULL;

    // Create a default publisher container
    SipPublishContentMgr* publishContent = new SipPublishContentMgr();

    // Create a default event handler
    SipSubscribeServerEventHandler* eventHandler = new SipSubscribeServerEventHandler();

    // Create a default subscription mgr
    SipSubscriptionMgr* subscriptionMgr = new SipSubscriptionMgr();

    newServer = new SipSubscribeServer(userAgent, 
                                      *publishContent,
                                      *subscriptionMgr,
                                      *eventHandler);

    if(eventType && *eventType)
    {
        // Enable the server to accept the given SIP event package
        newServer->enableEventType(eventType, 
                                   &userAgent, 
                                   publishContent,
                                   eventHandler,
                                   subscriptionMgr);
    }

    return(newServer);
}

// Constructor
SipSubscribeServer::SipSubscribeServer(SipUserAgent& defaultUserAgent,
                                       SipPublishContentMgr& defaultContentMgr,
                                       SipSubscriptionMgr& defaultSubscriptionMgr,
                                       SipSubscribeServerEventHandler& defaultEventHandler)
    : OsServerTask("SipSubscribeServer-%d")
    , mSubscribeServerMutex(OsMutex::Q_FIFO)
{
    mpDefaultUserAgent = &defaultUserAgent;
    mpDefaultContentMgr = &defaultContentMgr;
    mpDefaultSubscriptionMgr = &defaultSubscriptionMgr;
    mpDefaultEventHandler = &defaultEventHandler;
}


// Copy constructor NOT IMPLEMENTED
SipSubscribeServer::SipSubscribeServer(const SipSubscribeServer& rSipSubscribeServer)
: mSubscribeServerMutex(OsMutex::Q_FIFO)
{
}


// Destructor
SipSubscribeServer::~SipSubscribeServer()
{
   /*
    * Don't delete  mpDefaultContentMgr, mpDefaultSubscriptionMgr, or mpDefaultEventHandler
    *   they are owned by whoever constructed this server.
    */

   /*
   * jaro: actually these are never deleted, and good habit is to delete
   * objects in the same class where they are created if we keep pointer
   * to them in member variables. This doesn't cause any problems in Windows.
   * If it causes problems for someone, investigate it please, and don't solve
   * it by not deleting something.
   */
   delete mpDefaultEventHandler;
   delete mpDefaultSubscriptionMgr;
   delete mpDefaultContentMgr;
    // Iterate through and delete all the event data
    // TODO:
}

/* ============================ MANIPULATORS ============================== */

// Assignment operator
SipSubscribeServer& 
SipSubscribeServer::operator=(const SipSubscribeServer& rhs)
{
   if (this == &rhs)            // handle the assignment to self case
      return *this;

   return *this;
}

void SipSubscribeServer::contentChangeCallback(void* applicationData,
                                               const char* resourceId,
                                               const char* eventTypeKey,
                                               const char* eventType,
                                               UtlBoolean isDefaultContent)
{
    SipSubscribeServer* subServer = (SipSubscribeServer*)applicationData;
    subServer->notifySubscribers(resourceId, 
                                 eventTypeKey,
                                 eventType,
                                 isDefaultContent);
}

UtlBoolean SipSubscribeServer::notifySubscribers(const char* resourceId, 
                                                 const char* eventTypeKey,
                                                 const char* eventType,
                                                 UtlBoolean isDefaultContent)
{
   OsSysLog::add(FAC_SIP, PRI_DEBUG,
                 "SipSubscribeServer::notifySubscribers resourceId '%s', eventTypeKey '%s', eventType '%s', isDefaultContent %d",
                 resourceId, eventTypeKey, eventType, isDefaultContent);
    UtlBoolean notifiedSubscribers = FALSE;
    UtlString eventName(eventType ? eventType : "");

    lockForRead();
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventName);

    // Get the event specific info to find subscriptions interested in
    // this content
    if(eventData)
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG,
             "SipSubscribeServer::notifySubscribers received the request for sending out the notification for resourceId '%s', event type '%s'",
              resourceId, eventType);
              
        int numSubscriptions = 0;
        SipMessage** notifyArray = NULL;
        UtlString** acceptHeaderValuesArray = NULL;

        eventData->mpEventSpecificSubscriptionMgr->
           createNotifiesDialogInfo(resourceId,
                                                                            eventTypeKey,
                                                                            numSubscriptions,
                                                                            acceptHeaderValuesArray,
                                                                            notifyArray);

        OsSysLog::add(FAC_SIP, PRI_DEBUG,
             "SipSubscribeServer::notifySubscribers numSubscriptions for %s = %d",
              resourceId, numSubscriptions);

        // Setup and send a NOTIFY for each subscription interested in
        // this resourcesId and eventTypeKey
        SipMessage* notify = NULL;
        for(int notifyIndex = 0;
            notifyArray != NULL && 
              notifyIndex < numSubscriptions && 
              notifyArray[notifyIndex] != NULL;
            notifyIndex++)
        {
            notify = notifyArray[notifyIndex];

            // Fill in the NOTIFY request body/content
            eventData->mpEventSpecificHandler->
                    getNotifyContent(resourceId,
                    eventTypeKey,
                    eventType,
                    *(eventData->mpEventSpecificContentMgr),
                    *(acceptHeaderValuesArray[notifyIndex]),
                    *notify);

            // Send the NOTIFY request
            eventData->mpEventSpecificUserAgent->send(*notify);
        }

        // Free up the NOTIFY requests and accept header field values
        eventData->mpEventSpecificSubscriptionMgr->
                freeNotifies(numSubscriptions, 
                acceptHeaderValuesArray,
                notifyArray);
    }

    // event type not enabled
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR,
            "SipSubscribeServer::notifySubscribers event type: %s not enabled",
            eventName.data());
    }

    unlockForRead();

    return(notifiedSubscribers);
}

UtlBoolean SipSubscribeServer::enableEventType(const char* eventTypeToken,
                                             SipUserAgent* userAgent,
                                             SipPublishContentMgr* contentMgr,
                                             SipSubscribeServerEventHandler* eventHandler,
                                             SipSubscriptionMgr* subscriptionMgr)
{
    UtlBoolean addedEvent = FALSE;
    UtlString eventName(eventTypeToken ? eventTypeToken : "");
    lockForWrite();
    // Only add the event support if it does not already exist;
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventName);
    if(!eventData)
    {
        addedEvent = TRUE;
        eventData = new SubscribeServerEventData();
        *((UtlString*)eventData) = eventName;
        eventData->mpEventSpecificUserAgent = userAgent ? userAgent : mpDefaultUserAgent;
        eventData->mpEventSpecificContentMgr = contentMgr ? contentMgr : mpDefaultContentMgr;
        eventData->mpEventSpecificHandler = eventHandler ? eventHandler : mpDefaultEventHandler;
        eventData->mpEventSpecificSubscriptionMgr = subscriptionMgr ? subscriptionMgr : mpDefaultSubscriptionMgr;
        mEventDefinitions.insert(eventData);

        // Register an interest in SUBSCRIBE requests and NOTIFY responses
        // for this event type
        eventData->mpEventSpecificUserAgent->addMessageObserver(*(getMessageQueue()),
                                                                SIP_SUBSCRIBE_METHOD,
                                                                TRUE, // requests
                                                                FALSE, // not reponses
                                                                TRUE, // incoming
                                                                FALSE, // no outgoing
                                                                eventName,
                                                                NULL,
                                                                NULL);
        eventData->mpEventSpecificUserAgent->addMessageObserver(*(getMessageQueue()),
                                                                SIP_NOTIFY_METHOD,
                                                                FALSE, // no requests
                                                                TRUE, // reponses
                                                                TRUE, // incoming
                                                                FALSE, // no outgoing
                                                                eventName,
                                                                NULL,
                                                                NULL);

        // Register the callback for changes that occur in the
        // publish content manager
        eventData->mpEventSpecificContentMgr->setContentChangeObserver(eventName,
            this, contentChangeCallback);
    }

    unlockForWrite();

    return(addedEvent);
}

UtlBoolean SipSubscribeServer::disableEventType(const char* eventTypeToken,
                                             SipUserAgent*& userAgent,
                                             SipPublishContentMgr*& contentMgr,
                                             SipSubscribeServerEventHandler*& eventHandler,
                                             SipSubscriptionMgr*& subscriptionMgr)
{
    UtlBoolean removedEvent = FALSE;
    UtlString eventName(eventTypeToken ? eventTypeToken : "");
    lockForWrite();
    // Only add the event support if it does not already exist;
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.remove(&eventName);
    if(eventData)
    {
        removedEvent = TRUE;
        userAgent = eventData->mpEventSpecificUserAgent == mpDefaultUserAgent ? 
                        NULL : eventData->mpEventSpecificUserAgent;
        contentMgr = eventData->mpEventSpecificContentMgr == mpDefaultContentMgr ? 
                        NULL : eventData->mpEventSpecificContentMgr;
        eventHandler = eventData->mpEventSpecificHandler == mpDefaultEventHandler ?
                        NULL : eventData->mpEventSpecificHandler;
        subscriptionMgr = eventData->mpEventSpecificSubscriptionMgr == mpDefaultSubscriptionMgr ?
                        NULL : eventData->mpEventSpecificSubscriptionMgr;

        // Unregister interest in SUBSCRIBE requests and NOTIFY
        // responses for this event type
        eventData->mpEventSpecificUserAgent->removeMessageObserver(*(getMessageQueue()));


        delete eventData;
        eventData = NULL;
    }
    else
    {
        userAgent = NULL;
        contentMgr = NULL;
        eventHandler = NULL;
        subscriptionMgr = NULL;
    }

    unlockForWrite();

    return(removedEvent);
}

UtlBoolean SipSubscribeServer::handleMessage(OsMsg &eventMessage)
{
    int msgType = eventMessage.getMsgType();
    int msgSubType = eventMessage.getMsgSubType();

    // Timer fired
    if(msgType == OsMsg::OS_EVENT &&
       msgSubType == OsEventMsg::NOTIFY)
    {
        OsTimer* timer = 0;
        UtlString* subscribeDialogHandle = NULL;

        ((OsEventMsg&)eventMessage).getUserData((intptr_t&)subscribeDialogHandle);
        ((OsEventMsg&)eventMessage).getEventData((intptr_t&)timer);

        if(subscribeDialogHandle)
        {
            // Check if the subscription really expired and send 
            // the final NOTIFY if it did.
            handleExpiration(subscribeDialogHandle, timer);

            // Delete the handle;
            delete subscribeDialogHandle;

            // do not delete the timer.
            // handlExpiration deals with that and may reuse the timer
        }
    }

    // SIP message
    else if(msgType == OsMsg::PHONE_APP &&
       msgSubType == SipMessage::NET_SIP_MESSAGE)
    {
        const SipMessage* sipMessage = ((SipMessageEvent&)eventMessage).getMessage();

        UtlString method;
        if(sipMessage)
        {
            sipMessage->getCSeqField(NULL, &method);
        }

        // SUBSCRIBE requests
        if(sipMessage &&
           !sipMessage->isResponse() && 
           method.compareTo(SIP_SUBSCRIBE_METHOD) == 0)
        {
            handleSubscribe(*sipMessage);
        }

        // NOTIFY responses
        else if(sipMessage &&
                sipMessage->isResponse() && 
                method.compareTo(SIP_NOTIFY_METHOD) == 0)
        {
            handleNotifyResponse(*sipMessage);
        }  
    }

    return(TRUE);
}



/* ============================ ACCESSORS ================================= */

SipSubscribeServerEventHandler* 
SipSubscribeServer::getEventHandler(const UtlString& eventType)
{
    SipSubscribeServerEventHandler* eventHandler = NULL;
    lockForRead();
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventType);
    if(eventData)
    {
        eventHandler = eventData->mpEventSpecificHandler;
    }

    else 
    {
        eventHandler = mpDefaultEventHandler;
    }
    unlockForRead(); 

    return(eventHandler);
}

SipPublishContentMgr* 
SipSubscribeServer::getPublishMgr(const UtlString& eventType)
{
    SipPublishContentMgr* contentMgr = NULL;
    lockForRead();
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventType);
    if(eventData)
    {
        contentMgr = eventData->mpEventSpecificContentMgr;
    }

    else 
    {
        contentMgr = mpDefaultContentMgr;
    }
    unlockForRead(); 

    return(contentMgr);
}

SipSubscriptionMgr* SipSubscribeServer::getSubscriptionMgr(const UtlString& eventType)
{
    SipSubscriptionMgr* subscribeMgr = NULL;
    lockForRead();
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventType);
    if(eventData)
    {
        subscribeMgr = eventData->mpEventSpecificSubscriptionMgr;
    }

    else 
    {
        subscribeMgr = mpDefaultSubscriptionMgr;
    }
    unlockForRead(); 

    return(subscribeMgr);
}

/* ============================ INQUIRY =================================== */

UtlBoolean SipSubscribeServer::isEventTypeEnabled(const UtlString& eventType)
{
    lockForRead();
    // Only add the event support if it does not already exist;
    SubscribeServerEventData* eventData = 
        (SubscribeServerEventData*) mEventDefinitions.find(&eventType);
    unlockForRead();

    return(eventData != NULL);
}

/* //////////////////////////// PROTECTED ///////////////////////////////// */

/* //////////////////////////// PRIVATE /////////////////////////////////// */

UtlBoolean SipSubscribeServer::handleSubscribe(const SipMessage& subscribeRequest)
{
    UtlBoolean handledSubscribe = FALSE;
    UtlString eventName;
    subscribeRequest.getEventField(&eventName, NULL);

    // Not modifying the SubscribeServerEventData, just reading it
    lockForRead();

    // Get the event specific handler and information
    SubscribeServerEventData* eventPackageInfo = (SubscribeServerEventData*)
        mEventDefinitions.find(&eventName);

    // We handle this event type
    if(eventPackageInfo)
    {
        handledSubscribe = TRUE;
        UtlString resourceId;
        UtlString eventTypeKey, eventType;
        SipSubscribeServerEventHandler* handler =
            eventPackageInfo->mpEventSpecificHandler;

        // Get the keys used to identify the event state content
        handler->getKeys(subscribeRequest,
                         resourceId,
                         eventTypeKey,
                         eventType);

        SipMessage subscribeResponse;

        // Check if authenticated (or if it needs to be authenticated)
        if(handler->isAuthenticated(subscribeRequest,
                                     resourceId,
                                     eventTypeKey,
                                     subscribeResponse))
        {
            // Check if authorized (or if authorization is required)
            if(handler->isAuthorized(subscribeRequest,
                                     resourceId,
                                     eventTypeKey,
                                     subscribeResponse))
            {
                // The subscription is allowed, so update the
                // subscription state.  Set the To field tag if
                // this request initiated the dialog
                UtlString subscribeDialogHandle;
                UtlBoolean isNewDialog;
                UtlBoolean isExpiredSubscription;
                eventPackageInfo->mpEventSpecificSubscriptionMgr->updateDialogInfo(
                                                            subscribeRequest,
                                                            resourceId, 
                                                            eventTypeKey, 
                                                            getMessageQueue(),
                                                            subscribeDialogHandle, 
                                                            isNewDialog, 
                                                            isExpiredSubscription,
                                                            subscribeResponse);

                // Send the response ASAP to minimize resend handling of request
                 eventPackageInfo->mpEventSpecificUserAgent->send(subscribeResponse);

                 // Build a NOTIFY
                 SipMessage notifyRequest;

                 // Set the dialog information
                 eventPackageInfo->mpEventSpecificSubscriptionMgr->getNotifyDialogInfo(subscribeDialogHandle,
                                                                    notifyRequest);

                 // Set the NOTIFY content
                 UtlString acceptHeaderValue;
                 subscribeRequest.getAcceptField(acceptHeaderValue);
                 handler->getNotifyContent(resourceId, 
                                           eventTypeKey, 
                                           eventType, 
                                           *(eventPackageInfo->mpEventSpecificContentMgr),
                                           acceptHeaderValue,
                                           notifyRequest);

                 // Send the notify request
                 eventPackageInfo->mpEventSpecificUserAgent->send(notifyRequest);
            }
            // Not authorized
            else
            {
                // Send the response
                eventPackageInfo->mpEventSpecificUserAgent->send(subscribeResponse);
            }
        }

        // Not authenticated
        else
        {
            // Send the response
            eventPackageInfo->mpEventSpecificUserAgent->send(subscribeResponse);
        }
    }


    // We should not have received SUBSCRIBE requests for this event type
    // This event type has not been enabled in this SubscribeServer
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR, 
            "SipSubscribeServer::handleSubscribe event type: %s not enabled",
            eventName.data());

        SipMessage eventTypeNotHandled;
        eventTypeNotHandled.setResponseData(&subscribeRequest,
            SIP_BAD_EVENT_CODE, SIP_BAD_EVENT_TEXT);

        mpDefaultUserAgent->send(eventTypeNotHandled);
    }
    unlockForRead();

    return(handledSubscribe);
}

UtlBoolean SipSubscribeServer::handleNotifyResponse(const SipMessage& notifyResponse)
{
    UtlBoolean handledNotifyResponse = FALSE;
    int responseCode = notifyResponse.getResponseStatusCode();

    // Ignore provisional responses or success cases
    if(responseCode >= SIP_3XX_CLASS_CODE)
    {
        UtlString dialogHandle;
        notifyResponse.getDialogHandle(dialogHandle);

        // Not modifying the SubscribeServerEventData, just reading it
        lockForRead();

        // Get the event specific handler and information
        SubscribeServerEventData* eventPackageInfo = NULL;
        UtlHashMapIterator iterator(mEventDefinitions);
        
        while((eventPackageInfo = (SubscribeServerEventData*) iterator()))
        {
            // End this subscription as we got an error response from
            // the NOTIFY request.
            // Returns TRUE if the SipSubscriptionMgr has this dialog
            handledNotifyResponse = 
                eventPackageInfo->mpEventSpecificSubscriptionMgr->endSubscription(
                                                                    dialogHandle);
            if(handledNotifyResponse)
            {
                break;
            }
        }

        unlockForRead();

        // Should not happen, first of all we should never get a
        // response which does not correspond to a request sent from 
        // the SipUserAgent.  Secondly, we should not get a response to
        // and event type that we do not support
        if(!handledNotifyResponse)
        {
            OsSysLog::add(FAC_SIP, PRI_ERR,
                "SipSubscribeServer::handleNotifyResponse NOTIFY response with no dialog. Handle: %s",
                 dialogHandle.data());
        }
    }

    // Provisional or 2XX class responses
    else
    {
        handledNotifyResponse = TRUE;
    }

    return(handledNotifyResponse);
}

UtlBoolean SipSubscribeServer::handleExpiration(UtlString* subscribeDialogHandle,
                                                OsTimer* timer)
{
    // TODO: Currently timers are not set for the subscription
    // expiration time.  It is not clear this is really a useful
    // thing to do other than the fact that RFC 3265 says you
    // should send a final NOTIFY indicating that the subscription
    // expired.  I cannot come up with a use case where it is
    // needed that the subscribe client gets a final NOTIFY.
    // The client should already know when the expiration is
    // going to occur and that it has not reSUBSCRIBEd.
    OsSysLog::add(FAC_SIP, PRI_ERR,
                  "SipSubscribeServer::handleExpiration not implemented");
    return(FALSE);
}


void SipSubscribeServer::lockForRead()
{
    mSubscribeServerMutex.acquireRead();
}

void SipSubscribeServer::unlockForRead()
{
    mSubscribeServerMutex.releaseRead();
}

void SipSubscribeServer::lockForWrite()
{
    mSubscribeServerMutex.acquireWrite();
}

void SipSubscribeServer::unlockForWrite()
{
    mSubscribeServerMutex.releaseWrite();
}

/* ============================ FUNCTIONS ================================= */

