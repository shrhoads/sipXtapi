//  
// Copyright (C) 2007-2010 SIPez LLC. 
// Licensed to SIPfoundry under a Contributor Agreement. 
//
// Copyright (C) 2004-2008 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// $$
///////////////////////////////////////////////////////////////////////////////

// Author: Daniel Petrie <dpetrie AT SIPez DOT com>

// SYSTEM INCLUDES

//#define TEST_PRINT

#include <assert.h>

// APPLICATION INCLUDES
#if defined(_WIN32)
#       include "resparse/wnt/nterrno.h"
#elif defined(__pingtel_on_posix__)
#	include <sys/types.h>
#       include <sys/socket.h>
#       include <stdlib.h>
#endif

#include <utl/UtlHashBagIterator.h>
#include <net/SipSrvLookup.h>
#include <net/SipUserAgent.h>
#include <net/SipSession.h>
#include <net/SipMessageEvent.h>
#include <utl/UtlNameValueTokenizer.h>
#include <net/SipObserverCriteria.h>
#include <os/HostAdapterAddress.h>
#include <net/Url.h>
#ifdef SIP_TLS
#include <net/SipTlsServer.h>
#endif
#include <net/SipTcpServer.h>
#include <net/SipUdpServer.h>
#include <net/SipLineMgr.h>
#include <tapi/sipXtapiEvents.h>
#include <os/OsDateTime.h>
#include <os/OsEvent.h>
#include <os/OsQueuedEvent.h>
#include <os/OsTimer.h>
#include <os/OsTimerTask.h>
#include <os/OsEventMsg.h>
#include <os/OsPtrMsg.h>
#include <os/OsRpcMsg.h>
#include <os/OsConfigDb.h>
#include <os/OsRWMutex.h>
#include <os/OsReadLock.h>
#include <os/OsWriteLock.h>
#ifndef _WIN32
// version.h is generated as part of the build by other platforms.  For
// windows, the sip stack version is defined under the project settings.
#include <net/version.h>
#endif
#include <os/OsSysLog.h>
#include <os/OsFS.h>
#include <utl/UtlTokenizer.h>
#include <tapi/sipXtapiInternal.h>
#include <os/OsNatAgentTask.h>

// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS

#define MAXIMUM_SIP_LOG_SIZE 100000
#define SIP_UA_LOG "sipuseragent.log"
#define CONFIG_LOG_DIR SIPX_LOGDIR

#ifndef  VENDOR
# define VENDOR "sipX"
#endif

#ifndef PLATFORM_UA_PARAM
#if defined(_WIN32)
#  define PLATFORM_UA_PARAM " (WinNT)"
#elif defined(_VXWORKS)
#  define PLATFORM_UA_PARAM " (VxWorks)"
#elif defined(__APPLE__)
#  define PLATFORM_UA_PARAM " (Darwin)"
#elif defined(ANDROID)
#  define PLATFORM_UA_PARAM " (Android)"
#elif defined(__linux__)
#  define PLATFORM_UA_PARAM " (Linux)"
#elif defined(sun)
#  define PLATFORM_UA_PARAM " (Solaris)"
#endif
#endif /* PLATFORM_UA_PARAM */

//#define TEST_PRINT 1
//#define LOG_TIME

// STATIC VARIABLE INITIALIZATIONS

/* //////////////////////////// PUBLIC //////////////////////////////////// */

/* ============================ CREATORS ================================== */

// Constructor
SipUserAgent::SipUserAgent(int sipTcpPort,
                           int sipUdpPort,
                           int sipTlsPort,
                           const char* publicAddress,
                           const char* defaultUser,
                           const char* defaultAddress,
                           const char* sipProxyServers,
                           const char* sipDirectoryServers,
                           const char* sipRegistryServers,
                           const char* authenticationScheme,
                           const char* authenticateRealm,
                           OsConfigDb* authenticateDb,
                           OsConfigDb* authorizeUserIds,
                           OsConfigDb* authorizePasswords,
                           SipLineMgr* lineMgr,
                           int sipFirstResendTimeout,
                           UtlBoolean defaultToUaTransactions,
                           int readBufferSize,
                           int queueSize,
                           UtlBoolean bUseNextAvailablePort,
                           UtlString certNickname,
                           UtlString certPassword,
                           UtlString dbLocation,
                           UtlBoolean doUaMessageChecks
                           ) 
        : SipUserAgentBase(sipTcpPort, sipUdpPort, sipTlsPort, queueSize)
        , mSipTcpServer(NULL)
        , mSipUdpServer(NULL)
#ifdef SIP_TLS
        , mSipTlsServer(NULL)
#endif
        , mMessageLogRMutex(OsRWMutex::Q_FIFO)
        , mMessageLogWMutex(OsRWMutex::Q_FIFO)
        , mpLineMgr(NULL)
        , mIsUaTransactionByDefault(defaultToUaTransactions)
        , mbUseRport(FALSE)
        , mUseRportMapping(TRUE)
        , mbUseLocationHeader(FALSE)
        , mbIncludePlatformInUserAgentName(TRUE)
        , mDoUaMessageChecks(doUaMessageChecks)
        , mbShuttingDown(FALSE)
        , mbShutdownDone(FALSE)
        , mRegisterTimeoutSeconds(4)        
        , mbAllowHeader(true)
        , mbDateHeader(true)
        , mbShortNames(false)
        , mAcceptLanguage()
        , mpLastSipMessage(NULL)
{    
   OsSysLog::add(FAC_SIP, PRI_DEBUG,
                 "SipUserAgent::_ sipTcpPort = %d, sipUdpPort = %d, sipTlsPort = %d",
                 sipTcpPort, sipUdpPort, sipTlsPort);
                 
    // Make SIP logging off by default
    mMessageLogEnabled = FALSE;

    // Get pointer to line manager
    mpLineMgr = lineMgr;

    // Create and start the SIP TLS, TCP and UDP Servers
    if (mUdpPort != PORT_NONE)
    {
        mSipUdpServer = new SipUdpServer(mUdpPort, this,
                readBufferSize, bUseNextAvailablePort, defaultAddress );
        mSipUdpServer->startListener();
        mUdpPort = mSipUdpServer->getServerPort() ;
    }

    if (mTcpPort != PORT_NONE)
    {
        mSipTcpServer = new SipTcpServer(mTcpPort, this, SIP_TRANSPORT_TCP, 
                "SipTcpServer-%d", bUseNextAvailablePort, defaultAddress);
        mSipTcpServer->startListener();
        mTcpPort = mSipTcpServer->getServerPort() ;
    }

#ifdef SIP_TLS
    if (mTlsPort != PORT_NONE)
    {
        mSipTlsServer = new SipTlsServer(mTlsPort,
                                         this,
                                         bUseNextAvailablePort,
                                         certNickname,
                                         certPassword,
                                         dbLocation,
                                         defaultAddress);
        mSipTlsServer->startListener();
        mTlsPort = mSipTlsServer->getServerPort() ;
    }
#endif
 
    mMaxMessageLogSize = MAXIMUM_SIP_LOG_SIZE;
    mMaxForwards = SIP_DEFAULT_MAX_FORWARDS;

    // TCP sockets not used for an hour are garbage collected
    mMaxTcpSocketIdleTime = 3600;

    // INVITE transactions need to stick around for a minimum of
    // 3 minutes
    mMinInviteTransactionTimeout = 180;

    mForkingEnabled = TRUE;
    mRecurseOnlyOne300Contact = FALSE;

    // By default copy all of the Vias from incoming requests that have
    // a max-forwards == 0
    mReturnViasForMaxForwards = TRUE;

    mMaxSrvRecords = 4;
    mDnsSrvTimeout = 4; // seconds

#ifdef TEST_PRINT
    // Default the log on
    startMessageLog();
#else
    // Disable the message log
    stopMessageLog();
#endif

    // Authentication
    if(authenticationScheme)
    {
        mAuthenticationScheme.append(authenticationScheme);
        HttpMessage::cannonizeToken(mAuthenticationScheme);
        // Do not require authentication if not BASIC or DIGEST
        if(   0 != mAuthenticationScheme.compareTo(HTTP_BASIC_AUTHENTICATION,
                                                   UtlString::ignoreCase
                                                   )
           && 0 !=mAuthenticationScheme.compareTo(HTTP_DIGEST_AUTHENTICATION,
                                                  UtlString::ignoreCase
                                                  )
           )
        {
            mAuthenticationScheme.remove(0);
        }

    }
    if(authenticateRealm)
    {
        mAuthenticationRealm.append(authenticateRealm);
    }

    if(authenticateDb)
    {
        mpAuthenticationDb = authenticateDb;
    }
    else
    {
        mpAuthenticationDb = new OsConfigDb();
    }

    if(authorizeUserIds)
    {
        mpAuthorizationUserIds = authorizeUserIds;
    }
    else
    {
        mpAuthorizationUserIds = new OsConfigDb();
    }

    if(authorizePasswords)
    {
        mpAuthorizationPasswords = authorizePasswords;
    }
    else
    {
        mpAuthorizationPasswords = new OsConfigDb();
    }

    // SIP Server info
    if(sipProxyServers)
    {
        proxyServers.append(sipProxyServers);
    }
    if(sipDirectoryServers)
    {
        directoryServers.append(sipDirectoryServers);
    }
    if(defaultUser)
    {
        defaultSipUser.append(defaultUser);
        defaultSipUser.strip(UtlString::both);
    }

    if (!defaultAddress || strcmp(defaultAddress, "0.0.0.0") == 0)
    {
        // get the first CONTACT entry in the Db
        SIPX_CONTACT_ADDRESS* pContact = mContactDb.find(1); 
        assert(pContact) ;
        // Bind to the contact's Ip
        if (pContact)
        {
            defaultSipAddress = pContact->cIpAddress;
        }
    }
    else
    {
        defaultSipAddress.append(defaultAddress);
        sipIpAddress.append(defaultAddress);
    }

    if(sipRegistryServers)
    {
        registryServers.append(sipRegistryServers);
    }

    if(publicAddress && *publicAddress)
    {
        mConfigPublicAddress = publicAddress ;
        
        // make a config CONTACT entry
        UtlString adapterName;
        SIPX_CONTACT_ADDRESS contact;
        contact.eContactType = CONTACT_CONFIG;
        strcpy(contact.cIpAddress, publicAddress);

        if (getContactAdapterName(adapterName, defaultSipAddress, false))
        {
            strcpy(contact.cInterface, adapterName.data());
        }
        else
        {
           // If getContactAdapterName can't find an adapter.
           OsSysLog::add(FAC_SIP, PRI_WARNING,
                         "SipUserAgent::_ no adapter found for address '%s'",
                         defaultSipAddress.data());
           strcpy(contact.cInterface, "(unknown)");
        }
        contact.iPort = mUdpPort; // what about the TCP port?
        contact.eTransportType = TRANSPORT_UDP;  // what about TCP transport?        
        addContactAddress(contact);
    }
    else
    {
        sipIpAddress = defaultSipAddress;
    }

    mSipPort = PORT_NONE;

    UtlString hostIpAddress(sipIpAddress.data());

    //Timers
    if ( sipFirstResendTimeout <= 0)
    {
        mFirstResendTimeoutMs = SIP_DEFAULT_RTT;
    }
    else if ( sipFirstResendTimeout > 0  && sipFirstResendTimeout < 100)
    {
        mFirstResendTimeoutMs = SIP_MINIMUM_RTT;
    }
    else
    {
        mFirstResendTimeoutMs = sipFirstResendTimeout;
    }
    mLastResendTimeoutMs = 8 * mFirstResendTimeoutMs;
    mReliableTransportTimeoutMs = 2 * mLastResendTimeoutMs;
    mTransactionStateTimeoutMs = 10 * mLastResendTimeoutMs;

    // How long before we expire transactions by default
    mDefaultExpiresSeconds = 180; // mTransactionStateTimeoutMs / 1000;
    mDefaultSerialExpiresSeconds = 20;

    if(portIsValid(mUdpPort))
    {
        SipMessage::buildSipUrl(&mContactAddress,
                hostIpAddress.data(),
                (mUdpPort == SIP_PORT) ? PORT_NONE : mUdpPort,
                (mUdpPort == mTcpPort) ? "" : SIP_TRANSPORT_UDP,
                defaultSipUser.data());

#ifdef TEST_PRINT
        osPrintf("UDP default contact: \"%s\"\n", mContactAddress.data());
#endif

    }

    if(portIsValid(mTcpPort) && mTcpPort != mUdpPort)
    {
        SipMessage::buildSipUrl(&mContactAddress, hostIpAddress.data(),
                (mTcpPort == SIP_PORT) ? PORT_NONE : mUdpPort,
                SIP_TRANSPORT_TCP, defaultSipUser.data());
#ifdef TEST_PRINT
        osPrintf("TCP default contact: \"%s\"\n", mContactAddress.data());
#endif
    }

    // Initialize the transaction id seed
    SipTransaction::smBranchIdBase = mContactAddress;

    // Allow the default SIP methods
    allowMethod(SIP_INVITE_METHOD);
    allowMethod(SIP_ACK_METHOD);
    allowMethod(SIP_CANCEL_METHOD);
    allowMethod(SIP_BYE_METHOD);
    allowMethod(SIP_REFER_METHOD);
    allowMethod(SIP_OPTIONS_METHOD);
    allowMethod(SIP_PING_METHOD);

    defaultUserAgentName.append( VENDOR );
    defaultUserAgentName.append( "/" );
    defaultUserAgentName.append( SIP_STACK_VERSION );

    OsMsgQ* incomingQ = getMessageQueue();
    mpTimer = new OsTimer(incomingQ, 0);
    // Convert from mSeconds to uSeconds
    OsTime lapseTime(0, mTransactionStateTimeoutMs * 1000);
    mpTimer->periodicEvery(lapseTime, lapseTime);

    OsTime time;
    OsDateTime::getCurTimeSinceBoot(time);
    mLastCleanUpTime = time.seconds();

    // bandreasen: This was removed on main -- not sure why
    //     given that this boolean is passed in
    mIsUaTransactionByDefault = defaultToUaTransactions;
}

// Copy constructor
SipUserAgent::SipUserAgent(const SipUserAgent& rSipUserAgent) :
        mMessageLogRMutex(OsRWMutex::Q_FIFO),
        mMessageLogWMutex(OsRWMutex::Q_FIFO)
        , mbAllowHeader(false)
        , mbDateHeader(false)
        , mbShortNames(false)
        , mAcceptLanguage()
        , mRegisterTimeoutSeconds(4)
{
}

// Destructor
SipUserAgent::~SipUserAgent()
{
    mpTimer->stop();
    delete mpTimer;
    mpTimer = NULL;

    OsSysLog::add(FAC_SIP, PRI_INFO,
                  "SipUserAgent::~SipUserAgent waitUntilShutDown");
    OsSysLog::flush();

    // Wait until this OsServerTask has stopped or handleMethod
    // might access something we are about to delete here.
    waitUntilShutDown();

    if(mSipTcpServer)
    {
       OsSysLog::add(FAC_SIP, PRI_INFO,
                     "SipUserAgent::~SipUserAgent shutting down mSipTcpServer");
       OsSysLog::flush();

       mSipTcpServer->shutdownListener();
       mSipTcpServer->requestShutdown();
       delete mSipTcpServer;
       mSipTcpServer = NULL;
    }

    if(mSipUdpServer)
    {
       OsSysLog::add(FAC_SIP, PRI_INFO,
                     "SipUserAgent::~SipUserAgent shutting down mSipUdpServer");
       OsSysLog::flush();

       mSipUdpServer->shutdownListener();
       mSipUdpServer->requestShutdown();
       delete mSipUdpServer;
       mSipUdpServer = NULL;
    }

#ifdef SIP_TLS
    if(mSipTlsServer)
    {
       OsSysLog::add(FAC_SIP, PRI_INFO,
                     "SipUserAgent::~SipUserAgent shutting down mSipTlsServer");
       OsSysLog::flush();

       mSipTlsServer->shutdownListener();
       mSipTlsServer->requestShutdown();
       delete mSipTlsServer;
       mSipTlsServer = NULL;
    }
#endif

    OsSysLog::add(FAC_SIP, PRI_INFO,
                  "SipUserAgent::~SipUserAgent deleting databases");
    OsSysLog::flush();

    if(mpAuthenticationDb)
    {
        delete mpAuthenticationDb;
        mpAuthenticationDb = NULL;
    }

    if(mpAuthorizationUserIds)
    {
        delete mpAuthorizationUserIds;
        mpAuthorizationUserIds = NULL;
    }

    if(mpAuthorizationPasswords)
    {
        delete mpAuthorizationPasswords;
        mpAuthorizationPasswords = NULL;
    }

    allowedSipMethods.destroyAll();
    mMessageObservers.destroyAll();
    allowedSipExtensions.destroyAll();

    OsSysLog::add(FAC_SIP, PRI_INFO,
                  "SipUserAgent::~SipUserAgent exiting");
    OsSysLog::flush();
}

/* ============================ MANIPULATORS ============================== */


// Assignment operator
SipUserAgent&
SipUserAgent::operator=(const SipUserAgent& rhs)
{
   if (this == &rhs)            // handle the assignment to self case
      return *this;

   return *this;
}

void SipUserAgent::shutdown(UtlBoolean blockingShutdown)
{
    OsSysLog::add(FAC_SIP, PRI_INFO,
                  "SipUserAgent::shutdown(blocking=%s) starting shutdown", blockingShutdown ? "TRUE" : "FALSE");
    OsSysLog::flush();

    mbShuttingDown = TRUE;
    mSipTransactions.stopTransactionTimers();

    if(blockingShutdown == TRUE)
    {
        OsEvent shutdownEvent;
        OsStatus res;
        intptr_t rpcRetVal;

        mbBlockingShutdown = TRUE;

        OsRpcMsg shutdownMsg(OsMsg::PHONE_APP, SipUserAgent::SHUTDOWN_MESSAGE, shutdownEvent);
        postMessage(shutdownMsg);
        OsSysLog::add(FAC_SIP, PRI_INFO,
                      "SipUserAgent::shutdown waiting for shutdown to complete");
        OsSysLog::flush();
        res = shutdownEvent.wait();
        OsSysLog::add(FAC_SIP, PRI_INFO,
                      "SipUserAgent::shutdown shutdown complete wait status=%d", res);
        OsSysLog::flush();
        assert(res == OS_SUCCESS);

        res = shutdownEvent.getEventData(rpcRetVal);
        OsSysLog::add(FAC_SIP, PRI_INFO,
                      "SipUserAgent::shutdown shutdown event status=%d", rpcRetVal);
        OsSysLog::flush();
        assert(res == OS_SUCCESS && rpcRetVal == OS_SUCCESS);

        mbShutdownDone = TRUE;
    }
    else
    {
        mbBlockingShutdown = FALSE;
        OsMsg shutdownMsg(OsMsg::PHONE_APP, SipUserAgent::SHUTDOWN_MESSAGE);
        postMessage(shutdownMsg);
    }

    OsSysLog::add(FAC_SIP, PRI_INFO,
                  "SipUserAgent::shutdown exiting shutdown");
    OsSysLog::flush();
}

void SipUserAgent::enableStun(const char* szStunServer, 
                              int iStunPort,
                              int refreshPeriodInSecs,                               
                              OsNotification* pNotification,
                              const char* szIp) 
{
    if (mSipUdpServer)
    {
        mSipUdpServer->enableStun(szStunServer, 
                iStunPort,
                szIp, 
                refreshPeriodInSecs, 
                pNotification) ;
    }
}

void SipUserAgent::addMessageConsumer(OsServerTask* messageEventListener)
{
        // Need to do the real thing by keeping a list of consumers
        // and putting a mutex around the add to list
        //if(messageListener)
        //{
        //      osPrintf("WARNING: message consumer is NOT a LIST\n");
        //}
        //messageListener = messageEventListener;
    if(messageEventListener)
    {
        addMessageObserver(*(messageEventListener->getMessageQueue()));
    }
}

void SipUserAgent::addMessageObserver(OsMsgQ& messageQueue,
                                      const char* sipMethod,
                                      UtlBoolean wantRequests,
                                      UtlBoolean wantResponses,
                                      UtlBoolean wantIncoming,
                                      UtlBoolean wantOutGoing,
                                      const char* eventName,
                                      SipSession* pSession,
                                      void* observerData)
{
    SipObserverCriteria* observer = new SipObserverCriteria(observerData,
        &messageQueue,
        sipMethod, wantRequests, wantResponses, wantIncoming,
        wantOutGoing, eventName, pSession);

        {
            // Add the observer and its filter criteria to the list lock scope
        OsWriteLock lock(mObserverMutex);
        mMessageObservers.insert(observer);

        // Allow the specified method
        if(sipMethod && *sipMethod && wantRequests)
            allowMethod(sipMethod);
    }
}


UtlBoolean SipUserAgent::removeMessageObserver(OsMsgQ& messageQueue, void* pObserverData /*=NULL*/)
{
    OsWriteLock lock(mObserverMutex);
    SipObserverCriteria* pObserver = NULL ;
    UtlBoolean bRemovedObservers = FALSE ;

    // Traverse all of the observers and remove any that match the
    // message queue/observer data.  If the pObserverData is null, all
    // matching message queue/observers will be removed.  Otherwise, only
    // those observers that match both the message queue and observer data
    // are removed.
    UtlHashBagIterator iterator(mMessageObservers);
    while ((pObserver = (SipObserverCriteria*) iterator()))
    {
        if (pObserver->getObserverQueue() == &messageQueue)
        {
            if ((pObserverData == NULL) ||
                    (pObserverData == pObserver->getObserverData()))
            {
                bRemovedObservers = true ;
                UtlContainable* wasRemoved = mMessageObservers.removeReference(pObserver);

                if(wasRemoved)
                {
                   delete wasRemoved;
                }

            }
        }
    }

    return bRemovedObservers ;
}

void SipUserAgent::allowMethod(const char* methodName, const bool bAllow)
{
    if(methodName)
    {
        UtlString matchName(methodName);
        // Do not add the name if it is already in there
        if(NULL == allowedSipMethods.find(&matchName))
        {
            if (bAllow)
            {
                allowedSipMethods.append(new UtlString(methodName));
            }
        }
        else
        {
            if (!bAllow)
            {
                allowedSipMethods.destroy(allowedSipMethods.find(&matchName));
            }
        }
    }
}


UtlBoolean SipUserAgent::send(SipMessage& message,
                            OsMsgQ* responseListener,
                            void* responseListenerData,
                            SIPX_TRANSPORT_DATA* pTransport)
{
   if(mbShuttingDown)
   {
      return FALSE;
   }

   UtlBoolean sendSucceeded = FALSE;
   UtlBoolean isResponse = message.isResponse();

   mpLastSipMessage = &message;

   // ===========================================

   // Do all the stuff that does not require transaction locking first

   // Make sure the date field is set
   long epochDate;
   if(!message.getDateField(&epochDate) && mbDateHeader)
   {
      message.setDateField();
   }

   if (mbUseLocationHeader)
   {
      message.setLocationField(mLocationHeader.data());
   }

   // Make sure the message includes a contact if required and
   // update it to the best possible known contact.
   prepareContact(message, NULL, NULL) ;

   // Get Method
   UtlString method;
   if(isResponse)
   {
      int num = 0;
      message.getCSeqField(&num , &method);
   }
   else
   {
      message.getRequestMethod(&method);

      // Make sure that max-forwards is set
      int maxForwards;
      if(!message.getMaxForwards(maxForwards))
      {
         message.setMaxForwards(mMaxForwards);
      }
   }

   // ===========================================

   // Do the stuff that requires the transaction type knowledge
   // i.e. UA verse proxy transaction

   if(!isResponse)
   {
      // This should always be true now:
      if(message.isFirstSend())
      {
         // Save the transaction listener info
         if (responseListener)
         {
            message.setResponseListenerQueue(responseListener);
         }
         if (responseListenerData)
         {
            message.setResponseListenerData(responseListenerData);
         }
      }

      // This is not the first time this message has been sent
      else
      {
         // Should not be getting here.
         OsSysLog::add(FAC_SIP, PRI_WARNING, "SipUserAgent::send message being resent");
      }
   }

   // ===========================================

   // Find or create a transaction:
   UtlBoolean isUaTransaction = TRUE;
   enum SipTransaction::messageRelationship relationship;

   //mSipTransactions.lock();

#if 0 // TODO enable only for transaction match debugging - log is confusing otherwise
   OsSysLog::add(FAC_SIP, PRI_DEBUG
                 ,"SipUserAgent::send searching for existing transaction"
                 );
#endif
   // verify that the transaction does not already exist
   SipTransaction* transaction = mSipTransactions.findTransactionFor(
      message,
      TRUE, // outgoing
      relationship);

   // Found a transaction for this message
   if(transaction)
   {
      isUaTransaction = transaction->isUaTransaction();

      // Response for which a transaction already exists
      if(isResponse)
      {
         if(isUaTransaction)
         {
            // It seems that the polite thing to do is to add the
            // allowed methods to all final responses
            UtlString allowedMethodsSet;
            if(message.getResponseStatusCode() >= SIP_OK_CODE &&
               !message.getAllowField(allowedMethodsSet) && mbAllowHeader && mbAllowHeader)
            {
               UtlString allowedMethods;
               getAllowedMethods(&allowedMethods);
               message.setAllowField(allowedMethods);
            }
         }
      }

      // Request for which a transaction already exists
      else
      {
         // should not get here unless this is a CANCEL or ACK
         // request
         if((method.compareTo(SIP_CANCEL_METHOD) == 0) ||
            (method.compareTo(SIP_ACK_METHOD) == 0))
         {
         }

         // A request for which a transaction already exists
         // other than ACK and CANCEL
         else
         {
            // Should not be getting here
            OsSysLog::add(FAC_SIP, PRI_WARNING,
                          "SipUserAgent::send %s request matches existing transaction",
                          method.data());

            // We pretend there is no match so this becomes a
            // new transaction branch.  Make sure we unlock the
            // transaction before we reset to NULL.
            mSipTransactions.markAvailable(*transaction);
            transaction = NULL;
         }
      }
   }

   // No existing transaction for this message
   if(transaction == NULL)
   {
      if(isResponse)
      {
         // Should not get here except possibly on a server
         OsSysLog::add(FAC_SIP, PRI_WARNING,
                       "SipUserAgent::send response without an existing transaction"
                       );
      }
      else
      {
         // If there is already a via in the request this must
         // be a proxy transaction
         UtlString viaField;
         SipTransaction* parentTransaction = NULL;
         enum SipTransaction::messageRelationship parentRelationship;
         if(message.getViaField(&viaField, 0))
         {
            isUaTransaction = FALSE;

            // See if there is a parent server proxy transaction
#if 0 // TODO enable only for transaction match debugging - log is confusing otherwise
            OsSysLog::add(FAC_SIP, PRI_DEBUG
                          ,"SipUserAgent::send searching for parent transaction"
                          );
#endif
            parentTransaction =
               mSipTransactions.findTransactionFor(message,
                                                   FALSE, // incoming
                                                   parentRelationship);
         }

         // Create a new transactions
         // This should only be for requests
         transaction = new SipTransaction(&message, TRUE,
                                          isUaTransaction);
         transaction->markBusy();
         mSipTransactions.addTransaction(transaction);

         if(!isUaTransaction &&
            parentTransaction)
         {
            if(parentRelationship ==
               SipTransaction::MESSAGE_DUPLICATE)
            {
               // Link the parent server transaction to the
               // child client transaction
               parentTransaction->linkChild(*transaction);
               // The parent will be unlocked with the transaction
            }
            else
            {
               UtlString parentRelationshipString;
               SipTransaction::getRelationshipString(parentRelationship, parentRelationshipString);
               OsSysLog::add(FAC_SIP, PRI_WARNING,
                             "SipUserAgent::send proxied client transaction not "
                             "part of server transaction, parent relationship: %s",
                             parentRelationshipString.data());

               if(parentTransaction)
               {
                  mSipTransactions.markAvailable(*parentTransaction);
               }
            }
         }
         else if(!isUaTransaction)
         {
            // this happens all the time in the authproxy, so log only at debug
            OsSysLog::add(FAC_SIP, PRI_DEBUG,
                          "SipUserAgent::send proxied client transaction does not have parent");
         }
         else if(parentTransaction)
         {
            mSipTransactions.markAvailable(*parentTransaction);
         }

         relationship = SipTransaction::MESSAGE_UNKNOWN;
      }
   }

   if(transaction)
   {
      // Make sure the User Agent field is set
      if(isUaTransaction)
      {
         setSelfHeader(message);

         // Make sure the accept language is set
         UtlString language;
         message.getAcceptLanguageField(&language);
         if(language.isNull() && !mAcceptLanguage.isNull())
         {
            // Beware that this value does not describe the desired media
            // sessions, but rather the preferred languages for reason
            // phrases, etc. (RFC 3261 sec. 20.3)  Thus, it is useful to
            // have a value for this header even in requests like
            // SUBSCRIBE/NOTIFY which are expected to not be seen by a human.
            // This value should be configurable, though.
            message.setAcceptLanguageField(mAcceptLanguage);
         }

         // add allow field to Refer and Invite request . It is
         // mandatory for refer method
         UtlString allowedMethodsSet;
         if (   ! message.getAllowField(allowedMethodsSet)
             && (   method.compareTo(SIP_REFER_METHOD) == 0
                 || method.compareTo(SIP_INVITE_METHOD) == 0
                 )
             )
         {
             if (mbAllowHeader)
             {
                UtlString allowedMethods;
                getAllowedMethods(&allowedMethods);
                message.setAllowField(allowedMethods);
             }
         }

         // Set the supported extensions if this is not
         // an ACK or NOTIFY request and the Supported field 
         // is not already set.
         if(   method.compareTo(SIP_ACK_METHOD) != 0 &&
               method.compareTo(SIP_NOTIFY_METHOD) != 0
            && !message.getHeaderValue(0, SIP_SUPPORTED_FIELD)
            )
         {
            UtlString supportedExtensions;
            getSupportedExtensions(supportedExtensions);
            if (supportedExtensions.length() > 0)
            {
               message.setSupportedField(supportedExtensions.data());
            }
         }
      }

      // If this is the top most parent and it is a client transaction
      //  There is no server transaction, so cancel all of the children
      if(   !isResponse         && (method.compareTo(SIP_CANCEL_METHOD) == 0)
         && transaction->getTopMostParent() == NULL
         && !transaction->isServerTransaction()
         )
      {
         transaction->cancel(*this, mSipTransactions);
      }
      else
      {
         //  All other messages just get sent.
         // check for external transport
         bool bDummy;
         UtlString transport = message.getTransportName(bDummy);
         
         if (!pTransport)
         {
             UtlString localIp = message.getLocalIp();
             int dummy;
             
             if (localIp.length() < 1)
             {
                getLocalAddress(&localIp, &dummy, TRANSPORT_UDP);
             }
             pTransport = (SIPX_TRANSPORT_DATA*)lookupExternalTransport(transport, localIp);
         }
         sendSucceeded = transaction->handleOutgoing(message,
                                                     *this,
                                                     mSipTransactions,
                                                     relationship,
                                                     pTransport);
      }

      mSipTransactions.markAvailable(*transaction);
   }
   else
   {
      OsSysLog::add(FAC_SIP, PRI_ERR,"SipUserAgent::send failed to construct new transaction");
   }

   return(sendSucceeded);
}

UtlBoolean SipUserAgent::sendUdp(SipMessage* message,
                                 const char* serverAddress,
                                 int port)
{
  UtlBoolean isResponse = message->isResponse();
  UtlString method;
  int seqNum;
  UtlString seqMethod;
  int responseCode = 0;
  UtlBoolean sentOk = FALSE;
  UtlString msgBytes;
  UtlString messageStatusString = "SipUserAgent::sendUdp ";
  int timesSent = message->getTimesSent();

  prepareContact(*message, serverAddress, &port) ;

  if(!isResponse)
    {
      message->getRequestMethod(&method);
    }
  else
    {
      message->getCSeqField(&seqNum, &seqMethod);
      responseCode = message->getResponseStatusCode();
    }

  if(timesSent == 0)
    {
#ifdef TEST_PRINT
      osPrintf("First UDP send of message\n");
#endif

      message->touchTransportTime();

#ifdef TEST_PRINT
      osPrintf("SipUserAgent::sendUdp Sending UDP message\n");
#endif
    }
  // get the message if it was previously sent.
  else
    {
      char buffer[20];
      sprintf(buffer, "%d", timesSent);
      messageStatusString.append("resend ");
      messageStatusString.append(buffer);
      messageStatusString.append(" of UDP message\n");
    }

  // Send the message
    if (mbShortNames || message->getUseShortFieldNames())
    {
        message->replaceLongFieldNames();
    }

  // Disallow an address begining with * as it gets broadcasted on NT
  if(! strchr(serverAddress, '*') && *serverAddress)
    {
      sentOk = mSipUdpServer->send(message, serverAddress, port);
    }
  else if(*serverAddress == '\0')
    {
      // Only bother processing if the logs are enabled
      if (    isMessageLoggingEnabled() ||
              OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
        {
          UtlString msgBytes;
          int msgLen;
          message->getBytes(&msgBytes, &msgLen);
          msgBytes.insert(0, "No send address\n");
          msgBytes.append("--------------------END--------------------\n");
          logMessage(msgBytes.data(), msgBytes.length());
          OsSysLog::add(FAC_SIP_OUTGOING, PRI_INFO, "%s", msgBytes.data());
        }
      sentOk = FALSE;
    }
  else
    {
      sentOk = FALSE;
    }

#ifdef TEST_PRINT
  osPrintf("SipUserAgent::sendUdp sipUdpServer send returned: %d\n",
           sentOk);
  osPrintf("SipUserAgent::sendUdp isResponse: %d method: %s seqmethod: %s responseCode: %d\n",
           isResponse, method.data(), seqMethod.data(), responseCode);
#endif
  // If we have not failed schedule a resend
  if(sentOk)
    {
      messageStatusString.append("UDP SIP User Agent sent message:\n");
      messageStatusString.append("----Remote Host:");
      messageStatusString.append(serverAddress);
      messageStatusString.append("---- Port: ");
      char buff[10];
      sprintf(buff, "%d", !portIsValid(port) ? 5060 : port);
      messageStatusString.append(buff);
      messageStatusString.append("----\n");

#ifdef TEST_PRINT
      osPrintf("%s", messageStatusString.data());
#endif
    }
  else
    {
      messageStatusString.append("UDP SIP User Agent failed to send message:\n");
      messageStatusString.append("----Remote Host:");
      messageStatusString.append(serverAddress);
      messageStatusString.append("---- Port: ");
      char buff[10];
      sprintf(buff, "%d", !portIsValid(port) ? 5060 : port);
      messageStatusString.append(buff);
      messageStatusString.append("----\n");
      message->logTimeEvent("FAILED");
    }

  // Only bother processing if the logs are enabled
  if (    isMessageLoggingEnabled() ||
          OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
    {
      int len;
      message->getBytes(&msgBytes, &len);
      msgBytes.insert(0, messageStatusString.data());
      msgBytes.append("--------------------END--------------------\n");
#ifdef TEST_PRINT
      osPrintf("%s", msgBytes.data());
#endif
      logMessage(msgBytes.data(), msgBytes.length());
      if (msgBytes.length())
      {
        OsSysLog::add(FAC_SIP_OUTGOING, PRI_INFO, "%s", msgBytes.data());
      }
    }

  // if we failed to send it is the calling functions problem to deal with the error

  return(sentOk);
}

UtlBoolean SipUserAgent::sendSymmetricUdp(SipMessage& message,
                                        const char* serverAddress,
                                        int port)
{
    prepareContact(message, serverAddress, &port) ;

    // Update Via
    int bestKnownPort;
    UtlString bestKnownAddress;
    getViaInfo(OsSocket::UDP, 
            bestKnownAddress, bestKnownPort, 
            serverAddress, &port) ;
    message.removeLastVia() ;
    message.addVia(bestKnownAddress, bestKnownPort, SIP_TRANSPORT_UDP);
    message.setLastViaTag("", "rport");

    // Send away
    UtlBoolean sentOk = mSipUdpServer->sendTo(message,
                                             serverAddress,
                                             port);

    // Don't bother processing unless the logs are enabled
    if (    isMessageLoggingEnabled() ||
            OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
    {
        UtlString msgBytes;
        int msgLen;
        message.getBytes(&msgBytes, &msgLen);
        UtlString outcomeMsg;
        char portString[20];
        sprintf(portString, "%d", !portIsValid(port) ? 5060 : port);

        if(sentOk)
        {
            outcomeMsg.append("UDP SIP User Agent sentTo message:\n----Remote Host:");
            outcomeMsg.append(serverAddress);
            outcomeMsg.append("---- Port: ");
            outcomeMsg.append(portString);
            outcomeMsg.append("----\n");
            msgBytes.insert(0, outcomeMsg);
            msgBytes.append("--------------------END--------------------\n");
        }
        else
        {
            outcomeMsg.append("SIP User agent FAILED sendTo message:\n----Remote Host:");
            outcomeMsg.append(serverAddress);
            outcomeMsg.append("---- Port: ");
            outcomeMsg.append(portString);
            outcomeMsg.append("----\n");
            msgBytes.insert(0, outcomeMsg);
            msgBytes.append("--------------------END--------------------\n");
        }

        logMessage(msgBytes.data(), msgBytes.length());
        OsSysLog::add(FAC_SIP_OUTGOING, PRI_INFO, "%s", msgBytes.data());
    }

    return(sentOk);
}

UtlBoolean SipUserAgent::sendCustom(SIPX_TRANSPORT_DATA* pTransport,
                                    SipMessage* message,
                                    const char* sendAddress,
                                    const int sendPort)
{
    UtlBoolean bSent(false);
    UtlString bytes;
    int length;
    message->getBytes(&bytes, &length);

    if (!pTransport)
    {
        bool bCustom = false;
        const UtlString transportName = message->getTransportName(bCustom);
        if (bCustom)
        {
            pTransport = (SIPX_TRANSPORT_DATA*)lookupExternalTransport(transportName, message->getLocalIp());
        }
    }    
    if (pTransport)
    {
        if (pTransport->bRouteByUser)
        {
            UtlString from, to ;
            Url fromUrl, toUrl ;
            UtlString temp ;

            /*
             * Routing by "user" mode is a bit odd.  Ideally, we return the
             * identity of the user (user@domain) without a port value.  
             * However, if we only have hostname, we will return just that 
             * (as opposed to "@hostname" which getIndentity returns).
             */           
            message->getFromUrl(message->isResponse() ? toUrl : fromUrl) ;
            message->getToUrl(message->isResponse() ? fromUrl : toUrl) ;

            // Parse From routing id
            fromUrl.setHostPort(-1) ;
            fromUrl.getUserId(temp) ;
            if (temp.isNull())
                fromUrl.getHostAddress(from) ;
            else
            {
                fromUrl.getUserId(from) ;
            }

            // Parse To routing it
            toUrl.setHostPort(-1) ;
            toUrl.getUserId(temp) ;
            if (temp.isNull())
                toUrl.getHostAddress(to) ;
            else
            {
                toUrl.getHostAddress(temp) ;
                if (temp.compareTo("aol.com", UtlString::ignoreCase) == 0)
                    toUrl.getUserId(to) ;
                else
                    toUrl.getIdentity(to) ;
            }

            if (OsSysLog::willLog(FAC_SIP_CUSTOM, PRI_DEBUG))
            {
                UtlString data((const char*) bytes.data(), length) ;
                OsSysLog::add(FAC_SIP_CUSTOM, PRI_DEBUG, "[Sent] From: %s To: %s\r\n%s\r\n", 
                        from.data(),
                        to.data(),
                        data.data()) ;
            }

            bSent = pTransport->pFnWriteProc(pTransport->hTransport,
                                             to.data(),
                                             -1,
                                             from.data(),
                                             -1,
                                             (void*)bytes.data(),
                                             length,
                                             pTransport->pUserData);
           
        }
        else
        {

            if (OsSysLog::willLog(FAC_SIP_CUSTOM, PRI_DEBUG))
            {
                UtlString data((const char*) bytes.data(), length) ;
                OsSysLog::add(FAC_SIP_CUSTOM, PRI_DEBUG, "[Sent] From: %s To: %s\r\n%s\r\n", 
                        pTransport->szLocalIp,
                        sendAddress,
                        data.data()) ;
            }


            bSent = pTransport->pFnWriteProc(pTransport->hTransport,
                                             sendAddress,
                                             sendPort,
                                             pTransport->szLocalIp,
                                             pTransport->iLocalPort,
                                             (void*)bytes.data(),
                                             length,
                                             pTransport->pUserData);
        }
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::sendCustom - no external transport record found");
    }                                        
                                     
    return bSent;
}

UtlBoolean SipUserAgent::sendStatelessResponse(SipMessage& rresponse)
{
    UtlBoolean sendSucceeded = FALSE;

    // Forward via the server transaction
    SipMessage responseCopy(rresponse);
    responseCopy.removeLastVia();
    responseCopy.resetTransport();
    responseCopy.clearDNSField();

    UtlString sendProtocol;
    UtlString sendAddress;
    int sendPort;
    int receivedPort;
    UtlBoolean receivedSet;
    UtlBoolean maddrSet;
    UtlBoolean receivedPortSet;

    // use the via as the place to send the response
    responseCopy.getLastVia(&sendAddress, &sendPort, &sendProtocol,
        &receivedPort, &receivedSet, &maddrSet,
        &receivedPortSet);

    // If the sender of the request indicated support of
    // rport (i.e. received port) send this response back to
    // the same port it came from
    if(portIsValid(receivedPort) && receivedPortSet)
    {
        sendPort = receivedPort;
    }

    if(sendProtocol.compareTo(SIP_TRANSPORT_UDP, UtlString::ignoreCase) == 0)
    {
        sendSucceeded = sendUdp(&responseCopy, sendAddress.data(), sendPort);
    }
    else if(sendProtocol.compareTo(SIP_TRANSPORT_TCP, UtlString::ignoreCase) == 0)
    {
        sendSucceeded = sendTcp(&responseCopy, sendAddress.data(), sendPort);
    }
#ifdef SIP_TLS
    else if(sendProtocol.compareTo(SIP_TRANSPORT_TLS, UtlString::ignoreCase) == 0)
    {
        sendSucceeded = sendTls(&responseCopy, sendAddress.data(), sendPort);
    }
#endif
    else // must be custom
    {
        sendSucceeded = sendCustom(NULL, &responseCopy, sendAddress.data(), sendPort);
    }

    return(sendSucceeded);
}

UtlBoolean SipUserAgent::sendStatelessRequest(SipMessage& request,
                           UtlString& address,
                           int port,
                           OsSocket::IpProtocolSocketType protocol,
                           UtlString& branchId)
{
    // Convert the enum to a protocol string
    UtlString viaProtocolString;
    SipMessage::convertProtocolEnumToString(protocol,
                                            viaProtocolString);

    // Get via info
    UtlString viaAddress;
    int viaPort;
    getViaInfo(protocol,
               viaAddress,
               viaPort,
               address.data(),
               &port);

    // Add the via field data
    request.addVia(viaAddress.data(),
                   viaPort,
                   viaProtocolString,
                   branchId.data());

    // Send using the correct protocol
    UtlBoolean sendSucceeded = FALSE;
    if(protocol == OsSocket::UDP)
    {
        sendSucceeded = sendUdp(&request, address.data(), port);
    }
    else if(protocol == OsSocket::TCP)
    {
        sendSucceeded = sendTcp(&request, address.data(), port);
    }
#ifdef SIP_TLS
    else if(protocol == OsSocket::SSL_SOCKET)
    {
        sendSucceeded = sendTls(&request, address.data(), port);
    }
#endif
    else if (protocol >= OsSocket::CUSTOM)
    {
        sendSucceeded = sendCustom(NULL, &request, address.data(), port);
    }
    return(sendSucceeded);
}

UtlBoolean SipUserAgent::sendTcp(SipMessage* message,
                                 const char* serverAddress,
                                 int port)
{
    UtlBoolean sendSucceeded = FALSE;
    int len;
    UtlString msgBytes;
    UtlString messageStatusString = "SipUserAgent::sendTcp ";

    if (mbShortNames || message->getUseShortFieldNames())
    {
        message->replaceLongFieldNames();
    }

    // Disallow an address begining with * as it gets broadcasted on NT
    if(!strchr(serverAddress,'*') && *serverAddress)
    {
        if (mSipTcpServer)
        {
            sendSucceeded = mSipTcpServer->send(message, serverAddress, port);
        }
    }
    else if(*serverAddress == '\0')
    {
        if (    isMessageLoggingEnabled() ||
                OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
        {
            message->getBytes(&msgBytes, &len);
            msgBytes.insert(0, "No send address\n");
            msgBytes.append("--------------------END--------------------\n");
            logMessage(msgBytes.data(), msgBytes.length());
            OsSysLog::add(FAC_SIP_OUTGOING, PRI_INFO, "%s", msgBytes.data());
        }
        sendSucceeded = FALSE;
    }
    else
    {
        sendSucceeded = FALSE;
    }

    if(sendSucceeded)
    {
        messageStatusString.append("TCP SIP User Agent sent message:\n");
        //osPrintf("%s", messageStatusString.data());
    }
    else
    {
        messageStatusString.append("TCP SIP User Agent failed to send message:\n");
        //osPrintf("%s", messageStatusString.data());
        message->logTimeEvent("FAILED");
    }

    if (   isMessageLoggingEnabled()
        || OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO)
        )
    {
        message->getBytes(&msgBytes, &len);
        messageStatusString.append("----Remote Host:");
            messageStatusString.append(serverAddress);
            messageStatusString.append("---- Port: ");
            char buff[10];
            sprintf(buff, "%d", !portIsValid(port) ? 5060 : port);
            messageStatusString.append(buff);
            messageStatusString.append("----\n");

        msgBytes.insert(0, messageStatusString.data());
        msgBytes.append("--------------------END--------------------\n");
#ifdef TEST_PRINT
        osPrintf("%s", msgBytes.data());
#endif
        logMessage(msgBytes.data(), msgBytes.length());
        OsSysLog::add(FAC_SIP_OUTGOING , PRI_INFO, "%s", msgBytes.data());
    }

    return(sendSucceeded);
}


UtlBoolean SipUserAgent::sendTls(SipMessage* message,
                                                                const char* serverAddress,
                                                                int port)
{
#ifdef SIP_TLS
   int sendSucceeded = FALSE;
   int len;
   UtlString msgBytes;
   UtlString messageStatusString;

    if (mbShortNames || message->getUseShortFieldNames())
    {
        message->replaceLongFieldNames();
    }

   // Disallow an address begining with * as it gets broadcasted on NT
   if(!strchr(serverAddress,'*') && *serverAddress)
   {
        sendSucceeded = mSipTlsServer->send(message, serverAddress, port);
   }
   else if(*serverAddress == '\0')
   {
      if (    isMessageLoggingEnabled() ||
          OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
      {
         message->getBytes(&msgBytes, &len);
         msgBytes.insert(0, "No send address\n");
         msgBytes.append("--------------------END--------------------\n");
         logMessage(msgBytes.data(), msgBytes.length());
         OsSysLog::add(FAC_SIP_OUTGOING, PRI_INFO, "%s", msgBytes.data());
      }
      sendSucceeded = FALSE;
   }
   else
   {
      sendSucceeded = FALSE;
   }

   if(sendSucceeded)
   {
      messageStatusString.append("TLS SIP User Agent sent message:\n");
      //osPrintf("%s", messageStatusString.data());

   }
   else
   {
      messageStatusString.append("TLS SIP User Agent failed to send message:\n");
      //osPrintf("%s", messageStatusString.data());
      message->logTimeEvent("FAILED");
   }

   if (    isMessageLoggingEnabled() ||
       OsSysLog::willLog(FAC_SIP_OUTGOING, PRI_INFO))
   {
      message->getBytes(&msgBytes, &len);
      messageStatusString.append("----Remote Host:");
      messageStatusString.append(serverAddress);
      messageStatusString.append("---- Port: ");
      char buff[10];
      sprintf(buff, "%d", !portIsValid(port) ? 5060 : port);
      messageStatusString.append(buff);
      messageStatusString.append("----\n");

      msgBytes.insert(0, messageStatusString.data());
      msgBytes.append("--------------------END--------------------\n");
#ifdef TEST_PRINT
      osPrintf("%s", msgBytes.data());
#endif
      logMessage(msgBytes.data(), msgBytes.length());
      OsSysLog::add(FAC_SIP_OUTGOING , PRI_INFO, "%s", msgBytes.data());
   }

   return(sendSucceeded);
#else
   return FALSE ;
#endif
}

void SipUserAgent::dispatch(SipMessage* message, int messageType, SIPX_TRANSPORT_DATA* pTransport)
{
    if(mbShuttingDown)
    {
        delete message;
        return;
    }
 
    int len;
    UtlString msgBytes;
    UtlString messageStatusString;
    UtlBoolean resentWithAuth = FALSE;
    UtlBoolean isResponse = message->isResponse();
    UtlBoolean shouldDispatch = FALSE;
    SipMessage* delayedDispatchMessage = NULL;

#ifdef LOG_TIME
   OsTimeLog eventTimes;
   eventTimes.addEvent("start");
#endif

   // Get the message bytes for logging before the message is
   // potentially deleted or nulled out.
   if (   isMessageLoggingEnabled()
       || OsSysLog::willLog(FAC_SIP_INCOMING_PARSED, PRI_DEBUG)
       || OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
   {
      message->getBytes(&msgBytes, &len);
   }

   if (isResponse)
   {
        UtlString viaAddr ;
        int viaPort = -1 ;
        int receivedPort = -1 ;
        UtlString viaProtocol ;
        UtlBoolean receivedSet = false ;
        UtlBoolean maddrSet = false ;
        UtlBoolean receivedPortSet = false ;

        message->getLastVia(&viaAddr, &viaPort, &viaProtocol, &receivedPort,
                            &receivedSet, &maddrSet, &receivedPortSet) ;
        if (mUseRportMapping && (receivedSet || receivedPortSet))
        {
            UtlString sendAddress ;
            int sendPort ;

            if (receivedPortSet && portIsValid(receivedPort))
            {
                viaPort = receivedPort ;
            }

            // Inform NAT agent (used for lookups)
            message->getSendAddress(&sendAddress, &sendPort) ;
            OsNatAgentTask::getInstance()->addExternalBinding(NULL, 
                    sendAddress, sendPort, viaAddr, viaPort) ;            

            // Inform UDP server (used for events)
            if (mSipUdpServer)
            {
                UtlString method ;
                int cseq;           
                message->getCSeqField(&cseq, &method);

                mSipUdpServer->updateSipKeepAlive(message->getLocalIp(),
                        method, sendAddress, sendPort, viaAddr, viaPort) ;
            }
        }
        else
        {
            UtlString sendAddress ;
            int sendPort ;
            message->getSendAddress(&sendAddress, &sendPort) ;
            OsNatAgentTask::getInstance()->clearExternalBinding(NULL, 
                    sendAddress, sendPort, true) ;
        }
   }

   if(messageType == SipMessageEvent::APPLICATION)
   {
#ifdef TEST_PRINT
      osPrintf("SIP User Agent received message via protocol: %d\n",
               message->getSendProtocol());
      message->logTimeEvent("DISPATCHING");
#endif

      UtlBoolean isUaTransaction = mIsUaTransactionByDefault;
      enum SipTransaction::messageRelationship relationship;
      SipTransaction* transaction =
         mSipTransactions.findTransactionFor(*message,
                                             FALSE, // incoming
                                             relationship);

#ifdef LOG_TIME
      eventTimes.addEvent("found TX");
#endif
      if(transaction == NULL)
      {
         if(isResponse)
         {
            OsSysLog::add(FAC_SIP, PRI_WARNING,"SipUserAgent::dispatch "
                          "received response without transaction");

#ifdef TEST_PRINT
            if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
            {
               OsSysLog::add(FAC_SIP, PRI_DEBUG,
                             "=Response w/o request=>\n%s\n======================>\n",
                             msgBytes.data());

               UtlString transString;
               mSipTransactions.toStringWithRelations(transString, *message, FALSE);
               OsSysLog::add(FAC_SIP, PRI_DEBUG,
                             "Transaction list:\n%s\n===End transaction list===",
                             transString.data());
            }
#endif
            }
         // New transaction for incoming request
         else
         {
            transaction = new SipTransaction(message, FALSE /* incoming */,
                                             isUaTransaction);

            // Add the new transaction to the list
            transaction->markBusy();
            mSipTransactions.addTransaction(transaction);

            UtlString method;
            message->getRequestMethod(&method);

            if(method.compareTo(SIP_ACK_METHOD) == 0)
            {
               // This may be normal - it will occur whenever the ACK is not traversing
               // the same proxy where the transaction is completing was origniated.
               // This happens on each call setup in the authproxy, for example, because
               // the original transaction was in the forking proxy.
               relationship = SipTransaction::MESSAGE_ACK;
               OsSysLog::add(FAC_SIP, PRI_DEBUG,
                             "SipUserAgent::dispatch received ACK without transaction");
            }
            else if(method.compareTo(SIP_CANCEL_METHOD) == 0)
            {
               relationship = SipTransaction::MESSAGE_CANCEL;
               OsSysLog::add(FAC_SIP, PRI_WARNING,
                             "SipUserAgent::dispatch received CANCEL without transaction");
            }
            else
            {
               relationship = SipTransaction::MESSAGE_REQUEST;
            }
         }
      }

#ifdef LOG_TIME
      eventTimes.addEvent("handling TX");
#endif
      // This is a message that was already recieved once
      if (   transaction
          && relationship == SipTransaction::MESSAGE_DUPLICATE
          )
      {
         // Resends of final INVITE responses need to be
         // passed through if they are 2xx class or the ACk
         // needs to be resent if it was a failure (i.e. 3xx,4xx,5xx,6xx)
         if(message->isResponse())
         {
            int responseCode = message->getResponseStatusCode();
            UtlString transactionMethod;
            int respCseq;
            message->getCSeqField(&respCseq, &transactionMethod);

            if (   responseCode >= SIP_2XX_CLASS_CODE 
                && transactionMethod.compareTo(SIP_INVITE_METHOD) == 0
                )
            {
               transaction->handleIncoming(*message,
                                           *this,
                                           relationship,
                                           mSipTransactions,
                                           delayedDispatchMessage,
                                           pTransport);

               // Should never dispatch a resendof a 2xx
               if(delayedDispatchMessage)
               {
                  delete delayedDispatchMessage;
                  delayedDispatchMessage = NULL;
               }
            }
         }

         messageStatusString.append("Received duplicate message\n");
#ifdef TEST_PRINT
         osPrintf("%s", messageStatusString.data());
#endif
      }

      // The first time we received this message
      else if(transaction)
      {
         switch (relationship)
         {
         case SipTransaction::MESSAGE_FINAL:
         case SipTransaction::MESSAGE_PROVISIONAL:
         case SipTransaction::MESSAGE_CANCEL_RESPONSE:
         {
            int delayedResponseCode = -1;
            SipMessage* request = transaction->getRequest();
            isUaTransaction = transaction->isUaTransaction();

            shouldDispatch =
               transaction->handleIncoming(*message,
                                           *this,
                                           relationship,
                                           mSipTransactions,
                                           delayedDispatchMessage,
                                           pTransport);

            if(delayedDispatchMessage)
            {
               delayedResponseCode =
                  delayedDispatchMessage->getResponseStatusCode();
            }

            // Check for Authentication Error
            if(   request
               && delayedDispatchMessage
               && delayedResponseCode == HTTP_UNAUTHORIZED_CODE
               && isUaTransaction
               )
            {
               resentWithAuth =
                  resendWithAuthorization(delayedDispatchMessage,
                                          request,
                                          &messageType,
                                          HttpMessage::SERVER);
            }

            // Check for Proxy Authentication Error
            if(   request
               && delayedDispatchMessage
               && delayedResponseCode == HTTP_PROXY_UNAUTHORIZED_CODE
               && isUaTransaction
               )
            {
               resentWithAuth =
                  resendWithAuthorization(delayedDispatchMessage,
                                          request,
                                          &messageType,
                                          HttpMessage::PROXY);
            }

            // If we requested authentication for this response,
            // validate the authorization
            UtlString requestAuthScheme;
            if(   request
               && request->getAuthenticationScheme(&requestAuthScheme,
                                                HttpMessage::SERVER))
            {
               UtlString reqUri;
               request->getRequestUri(&reqUri);

               if(authorized(message, reqUri.data()))
               {
#ifdef TEST_PRINT
                  osPrintf("response is authorized\n");
#endif
               }

               // What do we do with an unauthorized response?
               // For now we just let it through.
               else
               {
                  OsSysLog::add(FAC_SIP, PRI_WARNING, "UNAUTHORIZED RESPONSE");
#                 ifdef TEST_PRINT
                  osPrintf("WARNING: UNAUTHORIZED RESPONSE\n");
#                 endif
               }
            }

            // If we have a request for this incoming response
            // Forward it on to interested applications
            if (   request
                && (shouldDispatch || delayedDispatchMessage)
                )
            {
               UtlString method;
               request->getRequestMethod(&method);
               OsMsgQ* responseQ = NULL;
               responseQ =  request->getResponseListenerQueue();
               if (responseQ  && shouldDispatch)
               {
                  SipMessage * msg = new SipMessage(*message);
                  msg->setResponseListenerData(request->getResponseListenerData() );
                  SipMessageEvent eventMsg(msg);
                  eventMsg.setMessageStatus(messageType);
                  responseQ->send(eventMsg);
                  // The SipMessage gets freed with the SipMessageEvent
                  msg = NULL;
               }

               if(responseQ  && delayedDispatchMessage)
               {
                  SipMessage* tempDelayedDispatchMessage =
                     new SipMessage(*delayedDispatchMessage);

                  tempDelayedDispatchMessage->setResponseListenerData(
                     request->getResponseListenerData());

                  SipMessageEvent eventMsg(tempDelayedDispatchMessage);
                  eventMsg.setMessageStatus(messageType);
                  if (!mbShuttingDown)
                  {
                     responseQ->send(eventMsg);
                  }
                  // The SipMessage gets freed with the SipMessageEvent
                  tempDelayedDispatchMessage = NULL;
               }
            }
         }
         break;

         case SipTransaction::MESSAGE_REQUEST:
         {
         // if this is a request check if it is supported
            SipMessage* response = NULL;
            UtlString disallowedExtensions;
            UtlString method;
            UtlString allowedMethods;
            UtlString contentEncoding;
            UtlString toAddress;
            UtlString fromAddress;
            UtlString uriAddress;
            UtlString protocol;
            UtlString sipVersion;
            int port;
            int seqNumber;
            UtlString seqMethod;
            UtlString callIdField;
            int maxForwards;

            message->getRequestMethod(&method);
            if(isUaTransaction)
            {
               getAllowedMethods(&allowedMethods);
               whichExtensionsNotAllowed(message, &disallowedExtensions);
               message->getContentEncodingField(&contentEncoding);

               //delete leading and trailing white spaces
               disallowedExtensions = disallowedExtensions.strip(UtlString::both);
               allowedMethods = allowedMethods.strip(UtlString::both);
               contentEncoding = contentEncoding.strip(UtlString::both);
            }

            message->getToAddress(&toAddress, &port, &protocol);
            message->getFromAddress(&fromAddress, &port, &protocol);
            message->getUri(&uriAddress, &port, &protocol);
            message->getRequestProtocol(&sipVersion);
            sipVersion.toUpper();
            message->getCSeqField(&seqNumber, &seqMethod);
            seqMethod.toUpper();
            message->getCallIdField(&callIdField);

            // Check if the method is supported
            if(   isUaTransaction
               && !isMethodAllowed(method.data())
               )
            {
               response = new SipMessage();

               response->setRequestUnimplemented(message);
            }

            // Check if the extensions are supported
            else if(   mDoUaMessageChecks
                    && isUaTransaction
                    && !disallowedExtensions.isNull()
                    )
            {
               response = new SipMessage();
               response->setRequestBadExtension(message,
                                                disallowedExtensions);
            }

            // Check if the encoding is supported
            // i.e. no encoding
            else if(   mDoUaMessageChecks
                    && isUaTransaction
                    && !contentEncoding.isNull()
                    )
            {
               response = new SipMessage();
               response->setRequestBadContentEncoding(message,"");
            }

            // Check the addresses are present
            else if(toAddress.isNull() || fromAddress.isNull() ||
                    uriAddress.isNull())
            {
               response = new SipMessage();
               response->setRequestBadAddress(message);
            }

            // Check SIP version
            else if(strcmp(sipVersion.data(), SIP_PROTOCOL_VERSION) != 0)
            {
               response = new SipMessage();
               response->setRequestBadVersion(message);
            }

            // Check for missing CSeq or Call-Id
            else if(callIdField.isNull() || seqNumber < 0 ||
                    strcmp(seqMethod.data(), method.data()) != 0)
            {
               response = new SipMessage();
               response->setRequestBadRequest(message);
            }

            // Authentication Required
            else if(isUaTransaction &&
                    shouldAuthenticate(message))
            {
               if(!authorized(message))
               {
#ifdef TEST_PRINT
                  osPrintf("SipUserAgent::dispatch message Unauthorized\n");
#endif
                  response = new SipMessage();
                  response->setRequestUnauthorized(message,
                                                   mAuthenticationScheme.data(),
                                                   mAuthenticationRealm.data(),
                                                   "1234567890", // :TODO: nonce should be generated by SipNonceDb
                                                   "abcdefghij"  // opaque
                                                   );
               }
#ifdef TEST_PRINT
               else
               {
                  osPrintf("SipUserAgent::dispatch message Authorized\n");
               }
#endif //TEST_PRINT
            }

            // Process Options requests :TODO: - this does not route in the redirect server
            else if(isUaTransaction &&
                    !message->isResponse() &&
                    method.compareTo(SIP_OPTIONS_METHOD) == 0)
            {
               // Send an OK, the allowed field will get added to all final responces.
               response = new SipMessage();
               response->setResponseData(message,
                                         SIP_OK_CODE,
                                         SIP_OK_TEXT);

               delete(message);
               message = NULL;
            }

            else if(message->getMaxForwards(maxForwards))
            {
               if(maxForwards <= 0)
               {
                  response = new SipMessage();
                  response->setResponseData(message,
                                            SIP_TOO_MANY_HOPS_CODE,
                                            SIP_TOO_MANY_HOPS_TEXT);

                  response->setWarningField(SIP_WARN_MISC_CODE, sipIpAddress.data(),
                                            SIP_TOO_MANY_HOPS_TEXT
                                            );

                  setSelfHeader(*response);
                  
                  // If we are suppose to return the vias in the
                  // error response for Max-Forwards exeeded
                  if(mReturnViasForMaxForwards)
                  {

                     // The setBody method frees up the body before
                     // setting the new one, if there is a body
                     // We remove the body so that we can serialize
                     // the message without getting the body
                     message->setBody(NULL);

                     UtlString sipFragString;
                     int sipFragLen;
                     message->getBytes(&sipFragString, &sipFragLen);

                     // Create a body to contain the Vias from the request
                     HttpBody* sipFragBody =
                        new HttpBody(sipFragString.data(),
                                     sipFragLen,
                                     CONTENT_TYPE_MESSAGE_SIPFRAG);

                     // Attach the body to the response
                     response->setBody(sipFragBody);

                     // Set the content type of the body to be sipfrag
                     response->setContentType(CONTENT_TYPE_MESSAGE_SIPFRAG);
                  }

                  delete(message);
                  message = NULL;
               }
            }
            else
            {
               message->setMaxForwards(mMaxForwards);
            }

            // If the request is invalid
            if(response)
            {
               // Send the error response
               transaction->handleOutgoing(*response,
                                           *this,
                                           mSipTransactions,
                                           SipTransaction::MESSAGE_FINAL,
                                           pTransport);
               delete response;
               response = NULL;
               if(message) delete message;
               message = NULL;
            }
            else if(message)
            {
               mpLastSipMessage = message;
               shouldDispatch =
                  transaction->handleIncoming(*message,
                                              *this,
                                              relationship,
                                              mSipTransactions,
                                              delayedDispatchMessage,
                                              pTransport);
            }
            else
            {
               OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::dispatch NULL message to handle");
               //osPrintf("ERROR: SipUserAgent::dispatch NULL message to handle\n");
            }
         }
         break;

         case SipTransaction::MESSAGE_ACK:
         case SipTransaction::MESSAGE_2XX_ACK:
         case SipTransaction::MESSAGE_CANCEL:
         {
            int maxForwards;

            // Check the ACK max-forwards has not gone too many hopes
            if(!isResponse &&
               (relationship == SipTransaction::MESSAGE_ACK ||
                relationship == SipTransaction::MESSAGE_2XX_ACK) &&
               message->getMaxForwards(maxForwards) &&
               maxForwards <= 0 )
            {

               // Drop ACK on the floor.
               if(message) delete(message);
               message = NULL;
            }

            else if(message)
            {
               shouldDispatch =
                  transaction->handleIncoming(*message,
                                              *this,
                                              relationship,
                                              mSipTransactions,
                                              delayedDispatchMessage,
                                              pTransport);
            }
         }
         break;

         case SipTransaction::MESSAGE_NEW_FINAL:
         {
            // Forward it on to interested applications
            SipMessage* request = transaction->getRequest();
            shouldDispatch = TRUE;
            if( request)
            {
               UtlString method;
               request->getRequestMethod(&method);
               OsMsgQ* responseQ = NULL;
               responseQ =  request->getResponseListenerQueue();
               if (responseQ)
               {
                  SipMessage * msg = new SipMessage(*message);
                  msg->setResponseListenerData(request->getResponseListenerData() );
                  SipMessageEvent eventMsg(msg);
                  eventMsg.setMessageStatus(messageType);
                  responseQ->send(eventMsg);
                  // The SipMessage gets freed with the SipMessageEvent
                  msg = NULL;
               }
            }
         }
         break;

         default:
         {
            if (OsSysLog::willLog(FAC_SIP, PRI_WARNING))
            {
               UtlString relationString;
               SipTransaction::getRelationshipString(relationship, relationString);
               OsSysLog::add(FAC_SIP, PRI_WARNING, 
                             "SipUserAgent::dispatch unhandled incoming message: %s",
                             relationString.data());
            }
         }
         break;
      }
      }

      if(transaction)
      {
         mSipTransactions.markAvailable(*transaction);
      }
   }
   else
   {
      shouldDispatch = TRUE;
      messageStatusString.append("SIP User agent FAILED to send message:\n");
   }

#ifdef LOG_TIME
   eventTimes.addEvent("queuing");
#endif

   if (    isMessageLoggingEnabled()
       || OsSysLog::willLog(FAC_SIP_INCOMING_PARSED, PRI_DEBUG)
       )
   {
      msgBytes.insert(0, messageStatusString.data());
      msgBytes.append("++++++++++++++++++++END++++++++++++++++++++\n");
#ifdef TEST_PRINT
      osPrintf("%s", msgBytes.data());
#endif
      logMessage(msgBytes.data(), msgBytes.length());
      OsSysLog::add(FAC_SIP_INCOMING_PARSED, PRI_DEBUG, "%s", msgBytes.data());
   }

   if(message && shouldDispatch)
   {
#ifdef TEST_PRINT
      osPrintf("DISPATCHING message\n");
#endif

      queueMessageToObservers(message, messageType);
   }
   else
   {
      delete message;
      message = NULL;
   }

   if(delayedDispatchMessage)
   {
      if (   isMessageLoggingEnabled()
          || OsSysLog::willLog(FAC_SIP_INCOMING_PARSED, PRI_DEBUG)
          )
      {
         UtlString delayMsgString;
         int delayMsgLen;
         delayedDispatchMessage->getBytes(&delayMsgString,
                                          &delayMsgLen);
         delayMsgString.insert(0, "SIP User agent delayed dispatch message:\n");
         delayMsgString.append("++++++++++++++++++++END++++++++++++++++++++\n");
#ifdef TEST_PRINT
         osPrintf("%s", delayMsgString.data());
#endif
         logMessage(delayMsgString.data(), delayMsgString.length());
         OsSysLog::add(FAC_SIP_INCOMING_PARSED, PRI_DEBUG, "%s",
                       delayMsgString.data());
      }

      queueMessageToObservers(delayedDispatchMessage, messageType);
   }

#ifdef LOG_TIME
   eventTimes.addEvent("GC");
#endif

   // All garbage collection should now be done in the
   // context of the SipUserAgent to prevent hickups in
   // the reading of SipMessages off the sockets.
   //garbageCollection();

#ifdef LOG_TIME
   eventTimes.addEvent("finish");
   UtlString timeString;
   eventTimes.getLogString(timeString);
   OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipUserAgent::dispatch time log: %s",
                 timeString.data());
#endif
}

#undef LOG_TIME

void SipUserAgent::queueMessageToObservers(SipMessage* message,
                                           int messageType)
{
   UtlString callId;
   message->getCallIdField(&callId);
   UtlString method;
   message->getRequestMethod(&method);

   // Create a new message event
   SipMessageEvent event(message);
   event.setMessageStatus(messageType);

   // Find all of the observers which are interested in
   // this method and post the message
   UtlBoolean isRsp = message->isResponse();
   if(isRsp)
   {
      int cseq;
      message->getCSeqField(&cseq, &method);
   }

   queueMessageToInterestedObservers(event, method);
   // send it to those with no method descrimination as well
   queueMessageToInterestedObservers(event, "");

   // Do not delete the message it gets deleted with the event
   message = NULL;
}

void SipUserAgent::queueMessageToInterestedObservers(SipMessageEvent& event,
                                                     const UtlString& method)
{
   const SipMessage* message;
   if((message = event.getMessage()))
   {
      // Find all of the observers which are interested in
      // this method and post the message
      UtlString messageEventName;
      message->getEventField(&messageEventName, NULL, NULL);

      // do these constructors before taking the lock
      UtlString observerMatchingMethod(method);

      // lock the message observer list
      OsReadLock lock(mObserverMutex);

      UtlHashBagIterator observerIterator(mMessageObservers, &observerMatchingMethod);
      SipObserverCriteria* observerCriteria;
      while ((observerCriteria = (SipObserverCriteria*) observerIterator()))
      {
         // Check message direction and type
         if (   (  message->isResponse() && observerCriteria->wantsResponses())
             || (! message->isResponse() && observerCriteria->wantsRequests())
             )
         {
            // Decide if the event filter applies
            bool useEventFilter = false;
            bool matchedEvent = false;
            if (! message->isResponse()) // events apply only to requests
            {
               UtlString criteriaEventName;
               observerCriteria->getEventName(criteriaEventName);

               useEventFilter = ! criteriaEventName.isNull();
               if (useEventFilter)
               {
                  // see if the event type matches
                  matchedEvent = (   (   method.compareTo(SIP_SUBSCRIBE_METHOD,
                                                          UtlString::ignoreCase)
                                      == 0
                                      || method.compareTo(SIP_NOTIFY_METHOD,
                                                          UtlString::ignoreCase)
                                      == 0
                                      )
                                  && 0==messageEventName.compareTo(criteriaEventName,
                                                                   UtlString::ignoreCase
                                                                   )
                                  );
               }
            } // else - this is a response - event filter is not applicable

            // Check to see if the session criteria matters
            SipSession* pCriteriaSession = observerCriteria->getSession() ;
            bool useSessionFilter = (NULL != pCriteriaSession);
            UtlBoolean matchedSession = FALSE;
            if (useSessionFilter)
            {
               // it matters; see if it matches
               matchedSession = pCriteriaSession->isSameSession((SipMessage&) *message);
            }

            // We have a message type (req|rsp) the observer wants - apply filters
            if (   (! useSessionFilter || matchedSession)
                && (! useEventFilter   || matchedEvent)
                )
            {
               // This event is interesting, so send it up...
               OsMsgQ* observerQueue = observerCriteria->getObserverQueue();
               void* observerData = observerCriteria->getObserverData();

               // Cheat a little and set the observer data to be passed back
               ((SipMessage*) message)->setResponseListenerData(observerData);

               // Put the message in the observers queue
               if (!mbShuttingDown)
               {
                  int numMsgs = observerQueue->numMsgs();
                  int maxMsgs = observerQueue->maxMsgs();
                  if (numMsgs < maxMsgs)
                  {
                     observerQueue->send(event);
                  }
                  else
                  {
                     OsSysLog::add(FAC_SIP, PRI_ERR,
                           "queueMessageToInterestedObservers - queue full (name=%s, numMsgs=%d)",
                           observerQueue->getName().data(), numMsgs);
                  }
               }
            }
         }
         else
         {
            // either direction or req/rsp not a match
         }
      } // while observers
   }
   else
   {
      OsSysLog::add(FAC_SIP, PRI_CRIT, "queueMessageToInterestedObservers - no message");
   }
}


UtlBoolean checkMethods(SipMessage* message)
{
        return(TRUE);
}

UtlBoolean checkExtensions(SipMessage* message)
{
        return(TRUE);
}


UtlBoolean SipUserAgent::handleMessage(OsMsg& eventMessage)
{
   UtlBoolean messageProcessed = FALSE;
   //osPrintf("SipUserAgent: handling message\n");
   int msgType = eventMessage.getMsgType();
   int msgSubType = eventMessage.getMsgSubType();
   // Print message if input queue to SipUserAgent exceeds 100.
   if (getMessageQueue()->numMsgs() > 100)
   {
      SipMessageEvent* sipEvent;

      OsSysLog::add(FAC_SIP, PRI_DEBUG,
                    "SipUserAgent::handleMessage msgType = %d, msgSubType = %d, msgEventType = %d, "
                    "queue length = %d",
                    msgType, msgSubType, 
                    // Only extract msgEventType if msgType and msgSubType are right.
                    msgType == OsMsg::OS_EVENT && msgSubType == OsEventMsg::NOTIFY ?
                    (((OsEventMsg&)eventMessage).getUserData((intptr_t&)sipEvent),
                     sipEvent ? sipEvent->getMessageStatus() : -98) :
                    -99 /* dummy value */,
                    getMessageQueue()->numMsgs());
   }

   if(msgType == OsMsg::PHONE_APP)
   {
      // Final message from SipUserAgent::shutdown - all timers are stopped and are safe to delete
      if(msgSubType == SipUserAgent::SHUTDOWN_MESSAGE)
      {
#ifdef TEST_PRINT
         osPrintf("SipUserAgent::handleMessage shutdown complete message.\n");
#endif
         mSipTransactions.deleteTransactionTimers();

         if(mbBlockingShutdown == TRUE)
         {
            OsEvent* pEvent = ((OsRpcMsg&)eventMessage).getEvent();

            OsStatus res = pEvent->signal(OS_SUCCESS);
            assert(res == OS_SUCCESS);
         }
         else
         {
            mbShutdownDone = TRUE;
         }
      }
      else if (msgSubType == SipUserAgent::KEEPALIVE_MESSAGE)
      {
          OsPtrMsg& msg = (OsPtrMsg&) eventMessage ;

          SipUdpServer* pUdpServer = (SipUdpServer*) msg.getPtr() ;
          OsTimer* pTimer = (OsTimer*) msg.getPtr2() ;

          if (pUdpServer && pTimer)
          {
             pUdpServer->sendSipKeepAlive(pTimer) ;
          }
      } 
      else
      {
         SipMessage* sipMsg = (SipMessage*)((SipMessageEvent&)eventMessage).getMessage();
         if(sipMsg)
         {
            //messages for which the UA is consumer will end up here.
            OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipUserAgent::handleMessage posting message");

            // I cannot remember what kind of message ends up here???
            if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
            {
               int len;
               UtlString msgBytes;
               sipMsg->getBytes(&msgBytes, &len);
               OsSysLog::add(FAC_SIP, PRI_DEBUG,
                             "??????????????????????????????????????\n"
                             "%s???????????????????????????????????\n",
                             msgBytes.data());
            }
         }
      }
      messageProcessed = TRUE;
   }

   // A timer expired
   else if(msgType == OsMsg::OS_EVENT &&
           msgSubType == OsEventMsg::NOTIFY)
   {
      OsTimer* timer;
      SipMessageEvent* sipEvent = NULL;

      ((OsEventMsg&)eventMessage).getUserData((intptr_t&)sipEvent);
      ((OsEventMsg&)eventMessage).getEventData((intptr_t&)timer);

      if(sipEvent)
      {
         const SipMessage* sipMessage = sipEvent->getMessage();
         int msgEventType = sipEvent->getMessageStatus();

         // Resend timeout
         if(msgEventType == SipMessageEvent::TRANSACTION_RESEND)
         {
            if(sipMessage)
            {
               // Note: only delete the timer and notifier if there
               // is a message AND we can get a lock on the transaction.  
               //  WARNING: you cannot touch the contents of the transaction
               // attached to the message until the transaction has been
               // locked (via findTransactionFor, if no transaction is 
               // returned, it either no longer exists or we could not get
               // a lock for it.

#              ifdef TEST_PRINT
               {
                  UtlString callId;
                  int protocolType = sipMessage->getSendProtocol();
                  sipMessage->getCallIdField(&callId);

                  if(sipMessage->getSipTransaction() == NULL)
                  {
                     osPrintf("SipUserAgent::handleMessage "
                              "resend Timeout message with NULL transaction\n");
                  }
                  osPrintf("SipUserAgent::handleMessage "
                           "resend Timeout of message for %d protocol, callId: \"%s\" \n",
                           protocolType, callId.data());
               }
#              endif


               int nextTimeout = -1;
               enum SipTransaction::messageRelationship relationship;
               //mSipTransactions.lock();
               SipTransaction* transaction =
                  mSipTransactions.findTransactionFor(*sipMessage,
                                                      TRUE, // timers are only set for outgoing messages I think
                                                      relationship);
               if(transaction)
               {
                   if(timer)
                   {
                      transaction->removeTimer(timer);

                      delete timer;
                      timer = NULL;
                   }

                   // If we are in shutdown mode, unlock the transaction
                   // and set it to null.  We pretend that the transaction
                   // does not exist (i.e. noop).
                   if(mbShuttingDown)
                   {
                       mSipTransactions.markAvailable(*transaction);
                       transaction = NULL;
                   }
               }


               // If we cannot lock it, it does not exist (or atleast
               // pretend it does not exist.  The transaction will be
               // null if it has been deleted or we cannot get a lock
               // on the transaction.  
               if(transaction)
               {
                  SipMessage* delayedDispatchMessage = NULL;
                  bool bCustom = false;
                  const UtlString transportName = sipMessage->getTransportName(bCustom);
                  SIPX_TRANSPORT_DATA* pTransport = NULL;
                  if (bCustom)
                  {
                      pTransport = (SIPX_TRANSPORT_DATA*)lookupExternalTransport(transportName, sipMessage->getLocalIp());
                  }
                  transaction->handleResendEvent(*sipMessage,
                                                 *this,
                                                 relationship,
                                                 mSipTransactions,
                                                 nextTimeout,
                                                 delayedDispatchMessage,
                                                 pTransport);

                  if(nextTimeout == 0)
                  {
                     if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
                     {
                        UtlString transactionString;
                        transaction->toString(transactionString, TRUE);
                        transactionString.insert(0,
                                                 "SipUserAgent::handleMessage "
                                                 "timeout send failed\n"
                                                 );
                        OsSysLog::add(FAC_SIP, PRI_DEBUG, "%s\n", transactionString.data());
                        //osPrintf("%s\n", transactionString.data());
                     }
                  }

                  if(delayedDispatchMessage)
                  {
                     // Only bother processing if the logs are enabled
                     if (    isMessageLoggingEnabled() ||
                         OsSysLog::willLog(FAC_SIP_INCOMING, PRI_DEBUG))
                     {
                        UtlString delayMsgString;
                        int delayMsgLen;
                        delayedDispatchMessage->getBytes(&delayMsgString,
                                                         &delayMsgLen);
                        delayMsgString.insert(0, "SIP User agent delayed dispatch message:\n");
                        delayMsgString.append("++++++++++++++++++++END++++++++++++++++++++\n");
#ifdef TEST_PRINT
                        osPrintf("%s", delayMsgString.data());
#endif
                        logMessage(delayMsgString.data(), delayMsgString.length());
                        OsSysLog::add(FAC_SIP_INCOMING_PARSED, PRI_DEBUG,"%s",
                                      delayMsgString.data());
                     }

                     // if the request has a responseQueue, post the response.
                     OsMsgQ* responseQ = NULL;
                     responseQ =  sipMessage->getResponseListenerQueue();
                     if ( responseQ &&
                          !sipMessage->isResponse() &&
                          delayedDispatchMessage->isResponse())
                     {
                        SipMessage *messageToQ = new SipMessage(*delayedDispatchMessage);

                        messageToQ->setResponseListenerData(sipMessage->getResponseListenerData());
                        SipMessageEvent eventMsg(messageToQ);
                        eventMsg.setMessageStatus(SipMessageEvent::APPLICATION);
                        responseQ->send(eventMsg);
                        // The SipMessage gets freed with the SipMessageEvent
                        messageToQ = NULL;
                     }

                     queueMessageToObservers(delayedDispatchMessage,
                                             SipMessageEvent::APPLICATION
                                             );

                     // delayedDispatchMessage gets freed in queueMessageToObservers
                     delayedDispatchMessage = NULL;
                  }
               }

               // No transaction for this timeout
               else
               {
                  OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::handleMessage "
                                "SIP message timeout expired with no matching transaction");

                  // Somehow the transaction got deleted perhaps it timed
                  // out and there was a log jam that prevented the handling
                  // of the timeout ????? This should not happen.
               }

               if(transaction)
               {
                  mSipTransactions.markAvailable(*transaction);
               }

               // Do this outside so that we do not get blocked
               // on locking or delete the transaction out
               // from under ouselves
               if(nextTimeout == 0)
               {
                  // Make a copy and dispatch it
                  dispatch(new SipMessage(*sipMessage),
                           SipMessageEvent::TRANSPORT_ERROR);
               }

               // The timer made its own copy of this message.
               // It is deleted by dispatch ?? if it is not
               // rescheduled.
            } // End if sipMessage
         } // End SipMessageEvent::TRANSACTION_RESEND

         // Timeout for garbage collection
         else if(msgEventType == SipMessageEvent::TRANSACTION_GARBAGE_COLLECTION)
         {
#ifdef TEST_PRINT
            OsSysLog::add(FAC_SIP, PRI_DEBUG,
                          "SipUserAgent::handleMessage garbage collecting");
            osPrintf("SipUserAgent::handleMessage garbage collecting\n");
#endif
         }

         // Timeout for an transaction to expire
         else if(msgEventType == SipMessageEvent::TRANSACTION_EXPIRATION)
         {
#ifdef TEST_PRINT
            OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipUserAgent::handleMessage transaction expired");
#endif
            if(sipMessage)
            {
               // Note: only delete the timer and notifier if there
               // is a message AND we can get a lock on the transaction.  
               //  WARNING: you cannot touch the contents of the transaction
               // attached to the message until the transaction has been
               // locked (via findTransactionFor, if no transaction is 
               // returned, it either no longer exists or we could not get
               // a lock for it.

#ifdef TEST_PRINT
               if(sipMessage->getSipTransaction() == NULL)
               {
                  osPrintf("SipUserAgent::handleMessage expires Timeout message with NULL transaction\n");
               }
#endif
               int nextTimeout = -1;
               enum SipTransaction::messageRelationship relationship;
               //mSipTransactions.lock();
               SipTransaction* transaction =
                  mSipTransactions.findTransactionFor(*sipMessage,
                                                      TRUE, // timers are only set for outgoing?
                                                      relationship);
               if(transaction)
               {
                   if(timer)
                   {
                      transaction->removeTimer(timer);

                      delete timer;
                      timer = NULL;
                   }

                   // If we are in shutdown mode, unlock the transaction
                   // and set it to null.  We pretend that the transaction
                   // does not exist (i.e. noop).
                   if(mbShuttingDown)
                   {
                       mSipTransactions.markAvailable(*transaction);
                       transaction = NULL;
                   }
               }

               if(transaction)
               {
                  SipMessage* delayedDispatchMessage = NULL;
                  transaction->handleExpiresEvent(*sipMessage,
                                                  *this,
                                                  relationship,
                                                  mSipTransactions,
                                                  nextTimeout,
                                                  delayedDispatchMessage,
                                                  NULL);

                  mSipTransactions.markAvailable(*transaction);

                  if(delayedDispatchMessage)
                  {
                     // Only bother processing if the logs are enabled
                     if (    isMessageLoggingEnabled() ||
                         OsSysLog::willLog(FAC_SIP_INCOMING_PARSED, PRI_DEBUG))
                     {
                        UtlString delayMsgString;
                        int delayMsgLen;
                        delayedDispatchMessage->getBytes(&delayMsgString,
                                                         &delayMsgLen);
                        delayMsgString.insert(0, "SIP User agent delayed dispatch message:\n");
                        delayMsgString.append("++++++++++++++++++++END++++++++++++++++++++\n");
#ifdef TEST_PRINT
                        osPrintf("%s", delayMsgString.data());
#endif
                        logMessage(delayMsgString.data(), delayMsgString.length());
                        OsSysLog::add(FAC_SIP_INCOMING_PARSED, PRI_DEBUG,"%s",
                                      delayMsgString.data());
                     }

                     // wdn - if the request has a responseQueue, post the response.
                     OsMsgQ* responseQ = NULL;
                     responseQ =  sipMessage->getResponseListenerQueue();
                     if ( responseQ &&
                          !sipMessage->isResponse() &&
                          delayedDispatchMessage->isResponse())
                     {
                         SipMessage *messageToQ = new SipMessage(*delayedDispatchMessage);

                         messageToQ->setResponseListenerData(sipMessage->getResponseListenerData());
                         SipMessageEvent eventMsg(messageToQ);
                         eventMsg.setMessageStatus(SipMessageEvent::APPLICATION);
                         responseQ->send(eventMsg);
                         // The SipMessage gets freed with the SipMessageEvent
                         messageToQ = NULL;
                     }

                     // delayedDispatchMessage gets freed in queueMessageToObservers
                     queueMessageToObservers(delayedDispatchMessage,
                                             SipMessageEvent::APPLICATION
                                             );

                     //delayedDispatchMessage gets freed in queueMessageToObservers
                     delayedDispatchMessage = NULL;
                  }
               }
               else // Could not find a transaction for this exired message
               {
                  if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
                  {
                     UtlString noTxMsgString;
                     int noTxMsgLen;
                     sipMessage->getBytes(&noTxMsgString, &noTxMsgLen);

                     OsSysLog::add(FAC_SIP, PRI_DEBUG,
                                   "SipUserAgent::handleMessage "
                                   "event timeout with no matching transaction: %s",
                                   noTxMsgString.data());
                  }
               }
            }
         }

         // Unknown timeout
         else
         {
            OsSysLog::add(FAC_SIP, PRI_WARNING,
                          "SipUserAgent::handleMessage unknown timeout event: %d.", msgEventType);
#           ifdef TEST_PRINT
            osPrintf("ERROR: SipUserAgent::handleMessage unknown timeout event: %d.\n",
                     msgEventType);
#           endif
         }

         // As this is OsMsg is attached as a void* to the timeout event
         // it must be explicitly deleted.  The attached SipMessage
         // will get freed with it.
         delete sipEvent;
         sipEvent = NULL;
      } // end if sipEvent
      messageProcessed = TRUE;
   }

   else
   {
#ifdef TEST_PRINT
      osPrintf("SipUserAgent: Unknown message type: %d\n", msgType);
#endif
      messageProcessed = TRUE;
   }

   // Only GC if no messages are waiting -- othewise we may delete a timer 
   // that is queued up for us.
   if (getMessageQueue()->isEmpty())
   {
      garbageCollection();
      OsSysLog::add(FAC_SIP, PRI_DEBUG,
                    "SipUserAgent::handleMessage after GC, queue size = %d handled message type: %d subtype: %d",
                    getMessageQueue()->numMsgs(), msgType, msgSubType);
   }
   return(messageProcessed);
}

void SipUserAgent::garbageCollection()
{
    OsTime time;
    OsDateTime::getCurTimeSinceBoot(time);
    long bootime = time.seconds();

    long then = bootime - (mTransactionStateTimeoutMs / 1000);
    long tcpThen = bootime - mMaxTcpSocketIdleTime;
    long oldTransaction = then - (mTransactionStateTimeoutMs / 1000);
    long oldInviteTransaction = then - mMinInviteTransactionTimeout;

    // If the timeout is negative we never timeout or garbage collect
    // tcp connections
    if(mMaxTcpSocketIdleTime < 0)
    {
        tcpThen = -1;
    }

    if(mLastCleanUpTime < then)
    {
#      ifdef LOG_TIME
        OsSysLog::add(FAC_SIP, PRI_DEBUG,
            "SipUserAgent::garbageCollection"
                     " bootime: %ld then: %ld tcpThen: %ld"
                     " oldTransaction: %ld oldInviteTransaction: %ld",
                     bootime, then, tcpThen, oldTransaction,
                     oldInviteTransaction);
#endif
        mSipTransactions.removeOldTransactions(oldTransaction,
                                              oldInviteTransaction);
#      ifdef LOG_TIME
       OsSysLog::add(FAC_SIP, PRI_DEBUG,
                     "SipUserAgent::garbageCollection starting removeOldClients(udp)");
#      endif

       // Changed by Udit for null pointer check
       if (mSipUdpServer)
       {
        mSipUdpServer->removeOldClients(then);
       }

        if (mSipTcpServer)
        {
#         ifdef LOG_TIME
          OsSysLog::add(FAC_SIP, PRI_DEBUG,
                        "SipUserAgent::garbageCollection starting removeOldClients(tcp)");
#         endif
            mSipTcpServer->removeOldClients(tcpThen);
        }
#ifdef SIP_TLS
        if (mSipTlsServer)
        {
          OsSysLog::add(FAC_SIP, PRI_DEBUG,
                        "SipUserAgent::garbageCollection starting removeOldClients(tls)");
            mSipTlsServer->removeOldClients(tcpThen);
        }
#endif
#      ifdef LOG_TIME
       OsSysLog::add(FAC_SIP, PRI_DEBUG,
                     "SipUserAgent::garbageCollection done");
#      endif
        mLastCleanUpTime = bootime;
    }
}

UtlBoolean SipUserAgent::addCrLfKeepAlive(const char* szLocalIp,
                                    const char* szRemoteIp,
                                    const int   remotePort,
                                          const int   keepAliveSecs,
                                          OsNatKeepaliveListener* pListener)
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->addCrLfKeepAlive(szLocalIp, szRemoteIp, 
                remotePort, keepAliveSecs, pListener) ;
    }

    return bSuccess ;
}


UtlBoolean SipUserAgent::removeCrLfKeepAlive(const char* szLocalIp,
                                             const char* szRemoteIp,
                                             const int   remotePort) 
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->removeCrLfKeepAlive(szLocalIp, szRemoteIp, 
                remotePort) ;
    }

    return bSuccess ;
}

UtlBoolean SipUserAgent::addStunKeepAlive(const char* szLocalIp,
                                          const char* szRemoteIp,
                                          const int   remotePort,
                                          const int   keepAliveSecs,
                                          OsNatKeepaliveListener* pListener)
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->addStunKeepAlive(szLocalIp, szRemoteIp, 
                remotePort, keepAliveSecs, pListener) ;
    }

    return bSuccess ;
}

UtlBoolean SipUserAgent::removeStunKeepAlive(const char* szLocalIp,
                                             const char* szRemoteIp,
                                             const int   remotePort) 
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->removeStunKeepAlive(szLocalIp, szRemoteIp, 
                remotePort) ;
    }

    return bSuccess ;
}


UtlBoolean SipUserAgent::addSipKeepAlive(const char* szLocalIp,
                                         const char* szRemoteIp,
                                         const int   remotePort,
                                         const char* szMethod,
                                         const int   keepAliveSecs,
                                         OsNatKeepaliveListener* pListener)
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->addSipKeepAlive(szLocalIp, szRemoteIp, 
                remotePort, szMethod, keepAliveSecs, pListener) ;
    }

    return bSuccess ;
}

UtlBoolean SipUserAgent::removeSipKeepAlive(const char* szLocalIp,
                                            const char* szRemoteIp,
                                            const int   remotePort,
                                            const char* szMethod) 
{
    UtlBoolean bSuccess = false ;

    if (mSipUdpServer)
    {
        bSuccess = mSipUdpServer->removeSipKeepAlive(szLocalIp, szRemoteIp, 
                remotePort, szMethod) ;
    }

    return bSuccess ;
}

/* ============================ ACCESSORS ================================= */

// Enable or disable the outbound use of rport (send packet to actual
// port -- not advertised port).
UtlBoolean SipUserAgent::setUseRport(UtlBoolean bEnable)
{
    UtlBoolean bOld = mbUseRport ;

    mbUseRport = bEnable ;

    return bOld ;
}

void SipUserAgent::setUseRportMapping(UtlBoolean bEnable)
{
    mUseRportMapping = bEnable;
}

UtlBoolean SipUserAgent::getUseRportMapping() const
{
    return(mUseRportMapping);
}

// Is use report set?
UtlBoolean SipUserAgent::getUseRport() const
{
    return mbUseRport ;
}

void SipUserAgent::setUserAgentName(const UtlString& name)
{
    defaultUserAgentName = name;
    return;
}

const UtlString& SipUserAgent::getUserAgentName() const
{
    return defaultUserAgentName;
}


// Get the manually configured public address
UtlBoolean SipUserAgent::getConfiguredPublicAddress(UtlString* pIpAddress, int* pPort)
{
    UtlBoolean bSuccess = FALSE ;

    if (mConfigPublicAddress.length())
    {
        if (pIpAddress)
        {
            *pIpAddress = mConfigPublicAddress ;
        }

        if (pPort)
        {
            *pPort = mSipUdpServer->getServerPort() ;
        }

        bSuccess = TRUE ;
    }

    return bSuccess ;
}

// Get the local address and port
UtlBoolean SipUserAgent::getLocalAddress(UtlString* pIpAddress, int* pPort, SIPX_TRANSPORT_TYPE protocol)
{
    if (pIpAddress)
    {
        if (defaultSipAddress.length() > 0)
        {
            *pIpAddress = defaultSipAddress;
        }
        else
        {
            OsSocket::getHostIp(pIpAddress) ;
        }   
    }

    if (pPort)
    {
        switch (protocol)
        {
            case TRANSPORT_UDP:
                if (mSipUdpServer)
                    *pPort = mSipUdpServer->getServerPort() ;
                break;
            case TRANSPORT_TCP:
                if (mSipTcpServer)
                    *pPort = mSipTcpServer->getServerPort() ;
                break;
#ifdef SIP_TLS
            case TRANSPORT_TLS:
                if (mSipTlsServer)
                    *pPort = mSipTlsServer->getServerPort();
                break;
#endif
            default:
                if (mSipUdpServer)
                    *pPort = mSipUdpServer->getServerPort() ;
                break;
        }
    }

    return TRUE ;
}


// Get the NAT mapped address and port
UtlBoolean SipUserAgent::getNatMappedAddress(UtlString* pIpAddress, int* pPort)
{
    UtlBoolean bRet(FALSE);
    
    if (mSipUdpServer)
    {
        bRet = mSipUdpServer->getStunAddress(pIpAddress, pPort);
    }
    else if (mSipTcpServer)
    {
        // TODO - a TCP server should also be able to return a stun address
        //bRet = mSipTcpServer->getStunAddress(pIpAddress, pPort);
    }
    return bRet;
}


void SipUserAgent::setIsUserAgent(UtlBoolean isUserAgent)
{
    mIsUaTransactionByDefault = isUserAgent;
}

/// Add either Server or User-Agent header, as appropriate
void SipUserAgent::setSelfHeader(SipMessage& message)
{
   if (mIsUaTransactionByDefault)
   {
      setUserAgentHeader(message);
   }
   else
   {
      setServerHeader(message);
   }
}
    

// setUserAgentHeaderProperty
//      provides a string to be appended to the standard User-Agent
//      header value between "<vendor>/<version>" and the platform (eg "(VxWorks)")
//      Value should be formated either as "token/token" or "(string)"
//      with no leading or trailing space.
void SipUserAgent::setUserAgentHeaderProperty( const char* property )
{
    if ( property )
    {
       mUserAgentHeaderProperties.append(" ");
       mUserAgentHeaderProperties.append( property );
    }
}


void SipUserAgent::setMaxForwards(int maxForwards)
{
    if(maxForwards > 0)
    {
        mMaxForwards = maxForwards;
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG,"SipUserAgent::setMaxForwards maxForwards <= 0: %d",
            maxForwards);
    }
}

int SipUserAgent::getMaxForwards()
{
    int maxForwards;
    if(mMaxForwards <= 0)
    {
        OsSysLog::add(FAC_SIP, PRI_DEBUG,"SipUserAgent::getMaxForwards maxForwards <= 0: %d",
            mMaxForwards);

        maxForwards = SIP_DEFAULT_MAX_FORWARDS;
    }
    else
    {
        maxForwards = mMaxForwards;
    }

    return(maxForwards);
}

int SipUserAgent::getMaxSrvRecords() const
{
    return(mMaxSrvRecords);
}

void SipUserAgent::setMaxSrvRecords(int maxSrvRecords)
{
    mMaxSrvRecords = maxSrvRecords;
}

int SipUserAgent::getDnsSrvTimeout()
{
    return(mDnsSrvTimeout);
}

void SipUserAgent::setDnsSrvTimeout(int timeout)
{
    mDnsSrvTimeout = timeout;
}

void SipUserAgent::setForking(UtlBoolean enabled)
{
    mForkingEnabled = enabled;
}

void SipUserAgent::prepareContact(SipMessage& message,
                                  const char* szTargetUdpAddress, 
                                  const int*  piTargetUdpPort)
{
    // Add a default contact if none is present
    //   AND To all requests -- except REGISTERs (server mode?)
    //   OR all non-failure responses 
    int num = 0;
    UtlString method ;
    message.getCSeqField(&num , &method);
    UtlString contact ;
    if (    !message.getContactUri(0, &contact) &&
            ((!message.isResponse() && (method.compareTo(SIP_REGISTER_METHOD, UtlString::ignoreCase) != 0))
            || (message.getResponseStatusCode() < SIP_MULTI_CHOICE_CODE)))
    {
        UtlString contactIp ;
        if (message.isResponse())
            contactIp = message.getLocalIp() ;            
        if (contactIp.isNull())
            contactIp = sipIpAddress ;

        // TODO:: We need to figure out what the protocol SHOULD be if not 
        // already specified.  For example, if we are sending via TCP 
        // we should use a TCP contact -- the application layer and override
        // if desired by passing a contact.

        SipMessage::buildSipUrl(&contact,
                contactIp,
                mUdpPort == SIP_PORT ? PORT_NONE : mUdpPort,
                NULL,
                defaultSipUser.data());
        message.setContactField(contact) ;
    }

    // Update contact if we know anything about the target and our NAT binding
    if (message.getContactField(0, contact))
    {
        Url       urlContact(contact) ;        
        UtlString contactIp ;
        int       contactPort ;

        // Init Contact Info
        urlContact.getHostAddress(contactIp) ;
        contactPort = urlContact.getHostPort() ;

        // Try specified send to host:port
        if (szTargetUdpAddress && piTargetUdpPort &&
                (OsNatAgentTask::getInstance()->findContactAddress(
                szTargetUdpAddress, *piTargetUdpPort, 
                &contactIp, &contactPort)))
        {
            urlContact.setHostAddress(contactIp) ;
            urlContact.setHostPort(contactPort) ;
            message.removeHeader(SIP_CONTACT_FIELD, 0) ;
            message.setContactField(urlContact.toString()) ;
        }
        else
        {
            // Otherwise dig out info from message -- also make sure this is 
            // contact we should adjust 
            UtlString sendProtocol ;        
            if ((urlContact.getScheme() == Url::SipUrlScheme) && 
                    ((urlContact.getUrlParameter("transport", sendProtocol) == false)
                    || (sendProtocol.compareTo("udp", UtlString::ignoreCase) == 0)))
            {               
                if (message.isResponse())
                {
                    // Response: See if we have a better contact address to the 
                    // remote party

                    UtlString receivedFromAddress ;
                    int       receivedFromPort ;

                    message.getSendAddress(&receivedFromAddress, &receivedFromPort) ;
                    if (OsNatAgentTask::getInstance()->findContactAddress(
                            receivedFromAddress, receivedFromPort, 
                            &contactIp, &contactPort))
                    {
                        urlContact.setHostAddress(contactIp) ;
                        urlContact.setHostPort(contactPort) ;
                        message.removeHeader(SIP_CONTACT_FIELD, 0) ;
                        message.setContactField(urlContact.toString()) ;
                    }
                }
                else
                {               
                    // Request: See if we have a better contact address to the 
                    // remote party

                    UtlString requestUriAddress ;
                    int       requestUriPort ;
                    UtlString requestUriProtocol ;  // ignored

                    message.getUri(&requestUriAddress, &requestUriPort, &requestUriProtocol) ;

                    if (OsNatAgentTask::getInstance()->findContactAddress(
                            requestUriAddress, requestUriPort, 
                            &contactIp, &contactPort))
                    {
                        urlContact.setHostAddress(contactIp) ;
                        urlContact.setHostPort(contactPort) ;
                        message.removeHeader(SIP_CONTACT_FIELD, 0) ;
                        message.setContactField(urlContact.toString()) ;
                    }
                }
            }
        }
    }    
}

void SipUserAgent::getAllowedMethods(UtlString* allowedMethods)
{
        UtlDListIterator iterator(allowedSipMethods);
        allowedMethods->remove(0);
        UtlString* method;

        while ((method = (UtlString*) iterator()))
        {
                if(!method->isNull())
                {
                        if(!allowedMethods->isNull())
                        {
                                allowedMethods->append(", ");
                        }
                        allowedMethods->append(method->data());
                }
        }
}

void SipUserAgent::getViaInfo(int protocol,
                              UtlString& address,
                              int&        port,
                              const char* pszTargetAddress,
                              const int*  piTargetPort)
{
    address = sipIpAddress;

    if(protocol == OsSocket::TCP)
    {
        port = mTcpPort == SIP_PORT ? PORT_NONE : mTcpPort;
    }
#ifdef SIP_TLS
    else if(protocol == OsSocket::SSL_SOCKET)
    {
        port = mTlsPort == SIP_TLS_PORT ? PORT_NONE : mTlsPort;
    }
#endif
    else
    {
        if(portIsValid(mSipPort))
        {
            port = mSipPort;
        }
        else if(mUdpPort == SIP_PORT)
        {
            port = PORT_NONE;
        }
        else
        {
            port = mUdpPort;
        }

        if (pszTargetAddress && piTargetPort)
        {
            OsNatAgentTask::getInstance()->findContactAddress(pszTargetAddress, *piTargetPort, 
                    &address, &port) ;
        }
    }
}

void SipUserAgent::getFromAddress(UtlString* address, int* port, UtlString* protocol)
{
    UtlTokenizer tokenizer(registryServers);
        UtlString regServer;

        tokenizer.next(regServer, ",");
        SipMessage::parseAddressFromUri(regServer.data(), address,
                port, protocol);

    if(address->isNull())
    {
            protocol->remove(0);
            // TCP only
            if(portIsValid(mTcpPort) && !portIsValid(mUdpPort))
            {
                    protocol->append(SIP_TRANSPORT_TCP);
                    *port = mTcpPort;
            }
            // UDP only
            else if(portIsValid(mUdpPort) && !portIsValid(mTcpPort))
            {
                    protocol->append(SIP_TRANSPORT_UDP);
                    *port = mUdpPort;
            }
            // TCP & UDP on non-standard port
            else if(mTcpPort != SIP_PORT)
            {
                    *port = mTcpPort;
            }
            // TCP & UDP on standard port
            else
            {
                    *port = PORT_NONE;
            }

            // If there is an address configured use it
            UtlNameValueTokenizer::getSubField(defaultSipAddress.data(), 0,
                    ", \t", address);

            // else use the local host ip address
            if(address->isNull())
            {
            address->append(sipIpAddress);
                    //OsSocket::getHostIp(address);
            }
    }
}

void SipUserAgent::getDirectoryServer(int index, UtlString* address,
                                                                          int* port, UtlString* protocol)
{
        UtlString serverAddress;
        UtlNameValueTokenizer::getSubField(directoryServers.data(), 0,
                SIP_MULTIFIELD_SEPARATOR, &serverAddress);

        address->remove(0);
        *port = PORT_NONE;
        protocol->remove(0);
        SipMessage::parseAddressFromUri(serverAddress.data(),
                address, port, protocol);
        serverAddress.remove(0);
}

void SipUserAgent::getProxyServer(int index, UtlString* address,
                                                                          int* port, UtlString* protocol)
{
        UtlString serverAddress;
        UtlNameValueTokenizer::getSubField(proxyServers.data(), 0,
                SIP_MULTIFIELD_SEPARATOR, &serverAddress);

        address->remove(0);
        *port = PORT_NONE;
        protocol->remove(0);
        SipMessage::parseAddressFromUri(serverAddress.data(), address, port, protocol);
        serverAddress.remove(0);
}

void SipUserAgent::setProxyServers(const char* sipProxyServers)
{
    if (sipProxyServers)
    {
        proxyServers = sipProxyServers ;
    }
    else
    {
        proxyServers.remove(0) ;
    }
}

int SipUserAgent::getSipStateTransactionTimeout()
{
    return mTransactionStateTimeoutMs;
}

int SipUserAgent::getReliableTransportTimeout()
{
    return(mReliableTransportTimeoutMs);
}

int SipUserAgent::getFirstResendTimeout()
{
    return(mFirstResendTimeoutMs);
}

int SipUserAgent::getLastResendTimeout()
{
    return(mLastResendTimeoutMs);
}

int SipUserAgent::getDefaultExpiresSeconds() const
{
    return(mDefaultExpiresSeconds);
}

void SipUserAgent::setDefaultExpiresSeconds(int expiresSeconds)
{
    if(expiresSeconds > 0 &&
       expiresSeconds <= mMinInviteTransactionTimeout)
    {
        mDefaultExpiresSeconds = expiresSeconds;
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR,
                      "SipUserAgent::setDefaultExpiresSeconds "
                      "illegal expiresSeconds value: %d IGNORED",
            expiresSeconds);
    }
}

int SipUserAgent::getDefaultSerialExpiresSeconds() const
{
    return(mDefaultSerialExpiresSeconds);
}

void SipUserAgent::setDefaultSerialExpiresSeconds(int expiresSeconds)
{
    if(expiresSeconds > 0 &&
       expiresSeconds <= mMinInviteTransactionTimeout)
    {
        mDefaultSerialExpiresSeconds = expiresSeconds;
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::setDefaultSerialExpiresSeconds "
                      "illegal expiresSeconds value: %d IGNORED",
            expiresSeconds);
    }
}

void SipUserAgent::setMaxTcpSocketIdleTime(int idleTimeSeconds)
{
    if(mMinInviteTransactionTimeout < idleTimeSeconds)
    {
        mMaxTcpSocketIdleTime = idleTimeSeconds;
    }
    else
    {
        OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::setMaxTcpSocketIdleTime "
                      "idleTimeSeconds: %d less than mMinInviteTransactionTimeout: %d IGNORED",
            idleTimeSeconds, mMinInviteTransactionTimeout);
    }
}

void SipUserAgent::setHostAliases(UtlString& aliases)
{
    UtlString aliasString;
    int aliasIndex = 0;
    while(UtlNameValueTokenizer::getSubField(aliases.data(), aliasIndex,
                    ", \t", &aliasString))
    {
        Url aliasUrl(aliasString);
        UtlString hostAlias;
        aliasUrl.getHostAddress(hostAlias);
        int port = aliasUrl.getHostPort();

        if(!portIsValid(port))
        {
            hostAlias.append(":5060");
        }
        else
        {
            char portString[20];
            sprintf(portString, ":%d", port);
            hostAlias.append(portString);
        }

        UtlString* newAlias = new UtlString(hostAlias);
        mMyHostAliases.insert(newAlias);
        aliasIndex++;
    }
}

void SipUserAgent::printStatus()

{
    if(mSipUdpServer)
    {
        mSipUdpServer->printStatus();
    }
    if(mSipTcpServer)
    {
        mSipTcpServer->printStatus();
    }
#ifdef SIP_TLS
    if(mSipTlsServer)
    {
        mSipTlsServer->printStatus();
    }
#endif

    UtlString txString;
    mSipTransactions.toString(txString);

    osPrintf("Transactions:\n%s\n", txString.data());
}

void SipUserAgent::startMessageLog(int newMaximumLogSize)
{
    if(newMaximumLogSize > 0) mMaxMessageLogSize = newMaximumLogSize;
    if(newMaximumLogSize == -1) mMaxMessageLogSize = -1;
    mMessageLogEnabled = TRUE;

    {
                OsWriteLock Writelock(mMessageLogWMutex);
                OsReadLock ReadLock(mMessageLogRMutex);
                if(mMaxMessageLogSize > 0)
                        mMessageLog.capacity(mMaxMessageLogSize);
        }
}

void SipUserAgent::stopMessageLog()
{
    mMessageLogEnabled = FALSE;
}

void SipUserAgent::clearMessageLog()
{
    OsWriteLock Writelock(mMessageLogWMutex);
    OsReadLock Readlock(mMessageLogRMutex);
    mMessageLog.remove(0);
}

void SipUserAgent::logMessage(const char* message, int messageLength)
{
    if(mMessageLogEnabled)
    {
#ifdef TEST_PRINT
        osPrintf("SIP LOGGING ENABLED\n");
#endif
       {// lock scope
           OsWriteLock Writelock(mMessageLogWMutex);
           // Do not allow the log go grow beyond the maximum
           if(mMaxMessageLogSize > 0 &&
              ((((int)mMessageLog.length()) + messageLength) > mMaxMessageLogSize))
           {
               mMessageLog.remove(0,
                     mMessageLog.length() + messageLength - mMaxMessageLogSize);
           }

           mMessageLog.append(message, messageLength);
       }//lock scope
    }
#ifdef TEST_PRINT
    else osPrintf("SIP LOGGING DISABLED\n");
#endif
}

void SipUserAgent::getMessageLog(UtlString& logData)
{
        OsReadLock Readlock(mMessageLogRMutex);
        logData = mMessageLog;
}

void SipUserAgent::allowExtension(const char* extension)
{
#ifdef TEST_PRINT
    osPrintf("Allowing extension: \"%s\"\n", extension);
#endif
    UtlString* extensionName = new UtlString(extension);
    allowedSipExtensions.append(extensionName);
}

void SipUserAgent::getSupportedExtensions(UtlString& extensionsString)
{
    extensionsString.remove(0);
    UtlString* extensionName = NULL;
    UtlDListIterator iterator(allowedSipExtensions);
    while ((extensionName = (UtlString*) iterator()))
    {
        if(!extensionsString.isNull()) extensionsString.append(", ");
        extensionsString.append(extensionName->data());
    }
}

void SipUserAgent::setLocationHeader(const char* szHeader)
{
    mLocationHeader = szHeader;
}

void SipUserAgent::setRecurseOnlyOne300Contact(UtlBoolean recurseOnlyOne)
{
    mRecurseOnlyOne300Contact = recurseOnlyOne;
}

SipMessage* SipUserAgent::getRequest(const SipMessage& response)
{
    // If the transaction exists and can be locked it
    // is returned.
    enum SipTransaction::messageRelationship relationship;
    SipTransaction* transaction =
        mSipTransactions.findTransactionFor(response,
                                             FALSE, // incoming
                                             relationship);
    SipMessage* request = NULL;

    if(transaction && transaction->getRequest())
    {
        // Make a copy to return
        request = new SipMessage(*(transaction->getRequest()));
    }

    // Need to unlock the transaction
    if(transaction)
        mSipTransactions.markAvailable(*transaction);

    return(request);
}

int SipUserAgent::getTcpPort() const
{
    int iPort = PORT_NONE ;

    if (mSipTcpServer)
    {
        iPort = mSipTcpServer->getServerPort() ;
    }

    return iPort ;
}

int SipUserAgent::getUdpPort() const
{
    int iPort = PORT_NONE ;

    if (mSipUdpServer)
    {
        iPort = mSipUdpServer->getServerPort() ;
    }

    return iPort ;
}

int SipUserAgent::getTlsPort() const
{
    int iPort = PORT_NONE ;

#ifdef SIP_TLS
    if (mSipTlsServer)
    {
        iPort = mSipTlsServer->getServerPort() ;
    }
#endif

    return iPort ;
}


/* ============================ INQUIRY =================================== */

UtlBoolean SipUserAgent::isMethodAllowed(const char* method)
{
        UtlString methodName(method);
        UtlBoolean isAllowed = (allowedSipMethods.occurrencesOf(&methodName) > 0);

        if (!isAllowed)
        {
           /* The method was not explicitly requested, but check for whether the 
            * application has registered for the wildcard.  If so, the method is 
            * allowed, but we do not advertise that fact in the Allow header.*/
           UtlString wildcardMethod;
           
           OsReadLock lock(mObserverMutex);
           isAllowed = mMessageObservers.contains(&wildcardMethod);
        }
        
        return(isAllowed);
}

UtlBoolean SipUserAgent::isExtensionAllowed(const char* extension) const
{
#ifdef TEST_PRINT
    osPrintf("isExtensionAllowed extension: \"%s\"\n", extension);
#endif
    UtlString extensionString;
    if(extension) extensionString.append(extension);
    extensionString.toLower();
        UtlString extensionName(extensionString);
        extensionString.remove(0);
        return(allowedSipExtensions.occurrencesOf(&extensionName) > 0);
}

void SipUserAgent::whichExtensionsNotAllowed(const SipMessage* message,
                                                           UtlString* disallowedExtensions) const
{
        int extensionIndex = 0;
        UtlString extension;

        disallowedExtensions->remove(0);
        while(message->getRequireExtension(extensionIndex, &extension))
        {
                if(!isExtensionAllowed(extension.data()))
                {
                        if(!disallowedExtensions->isNull())
                        {
                                disallowedExtensions->append(SIP_MULTIFIELD_SEPARATOR);
                                disallowedExtensions->append(SIP_SINGLE_SPACE);
                        }
                        disallowedExtensions->append(extension.data());
                }
                extensionIndex++;
        }
        extension.remove(0);
}

UtlBoolean SipUserAgent::isMessageLoggingEnabled()
{
    return(mMessageLogEnabled);
}

UtlBoolean SipUserAgent::isReady()
{
    return isStarted();
}

UtlBoolean SipUserAgent::waitUntilReady()
{
    // Lazy hack, should be a semaphore or event
    int count = 0;
    while(!isReady() && count < 5)
    {
        delay(500);
        count++;
    }

    return isReady() ;
}

UtlBoolean SipUserAgent::isForkingEnabled()
{
    return(mForkingEnabled);
}

UtlBoolean SipUserAgent::isMyHostAlias(Url& route) const
{
    UtlString hostAlias;
    route.getHostAddress(hostAlias);
    int port = route.getHostPort();

    if(port == PORT_NONE)
    {
        hostAlias.append(":5060");
    }
    else
    {
        char portString[20];
        sprintf(portString, ":%d", port);
        hostAlias.append(portString);
    }

    UtlString aliasMatch(hostAlias);
    UtlContainable* found = mMyHostAliases.find(&aliasMatch);

    return(found != NULL);
}

UtlBoolean SipUserAgent::recurseOnlyOne300Contact()
{
    return(mRecurseOnlyOne300Contact);
}


UtlBoolean SipUserAgent::isOk(OsSocket::IpProtocolSocketType socketType)
{
    UtlBoolean retval = FALSE;
    switch(socketType)
    {
        case OsSocket::TCP :
            if (mSipTcpServer)
            {
                retval = mSipTcpServer->isOk();
            }
            break;
        case OsSocket::UDP :
            if (mSipUdpServer)
            {
                retval = mSipUdpServer->isOk();
            }
            break;
#ifdef SIP_TLS
        case OsSocket::SSL_SOCKET :
            if (mSipTlsServer)
            {
                retval = mSipTlsServer->isOk();
            }
            break;
#endif
        default :
           OsSysLog::add(FAC_SIP, PRI_ERR, "SipUserAgent::isOK - invalid socket type %d",
                         socketType);
            break;
    }

    return retval;
}

UtlBoolean SipUserAgent::isShutdownDone()
{
    return mbShutdownDone;
}

/* //////////////////////////// PROTECTED ///////////////////////////////// */
UtlBoolean SipUserAgent::shouldAuthenticate(SipMessage* message) const
{
    UtlString method;
    message->getRequestMethod(&method);

        //SDUA - Do not authenticate if a CANCEL or an ACK req/res from other side
        UtlBoolean methodCompare = TRUE ;
    if (   strcmp(method.data(), SIP_ACK_METHOD) == 0
        || strcmp(method.data(), SIP_CANCEL_METHOD) == 0
        )
        {
                methodCompare = FALSE;
        }

        method.remove(0);
    return(   methodCompare
           && (   0 == mAuthenticationScheme.compareTo(HTTP_BASIC_AUTHENTICATION,
                                                       UtlString::ignoreCase
                                                       )
               || 0 == mAuthenticationScheme.compareTo(HTTP_DIGEST_AUTHENTICATION,
                                                       UtlString::ignoreCase
                                                       )
               )
           );
}

UtlBoolean SipUserAgent::authorized(SipMessage* request, const char* uri) const
{
    UtlBoolean allowed = FALSE;
    // Need to create a nonce database for nonce's created
    // for each message (or find the message for the previous
    // sequence number containing the authentication response
    // and nonce for this request)
    const char* nonce = "1234567890"; // :TBD: should be using nonce from the message

    if(mAuthenticationScheme.compareTo("") == 0)
    {
        allowed = TRUE;
    }

    else
    {
        UtlString user;
        UtlString password;

        // Get the user id
        request->getAuthorizationUser(&user);
        // Look up the password
        mpAuthenticationDb->get(user.data(), password);

#ifdef TEST_PRINT
        osPrintf("SipUserAgent::authorized user:%s password found:\"%s\"\n",
            user.data(), password.data());

#endif
        // If basic is set allow basic or digest
        if(mAuthenticationScheme.compareTo(HTTP_BASIC_AUTHENTICATION,
                                           UtlString::ignoreCase
                                           ) == 0
           )
        {
            allowed = request->verifyBasicAuthorization(user.data(),
                password.data());


            // Try Digest if basic failed
            if(! allowed)
            {
#ifdef TEST_PRINT
                osPrintf("SipUserAgent::authorized basic auth. failed\n");
#endif
                allowed = request->verifyMd5Authorization(user.data(),
                                                password.data(),
                                                nonce,
                                                mAuthenticationRealm.data(),
                                                uri);
            }
#ifdef TEST_PRINT
            else
            {
                osPrintf("SipUserAgent::authorized basic auth. passed\n");
            }
#endif
        }

        // If digest is set allow only digest
        else if(mAuthenticationScheme.compareTo(HTTP_DIGEST_AUTHENTICATION,
                                                UtlString::ignoreCase
                                                ) == 0
                )
        {
            allowed = request->verifyMd5Authorization(user.data(),
                                                password.data(),
                                                nonce,
                                                mAuthenticationRealm.data(),
                                                uri);
        }
        user.remove(0);
        password.remove(0);
    }

    return(allowed);
}

void SipUserAgent::addAuthentication(SipMessage* message) const
{
    message->setAuthenticationData(mAuthenticationScheme.data(),
                        mAuthenticationRealm.data(),
                        "1234567890",  // nonce
                        "abcdefghij"); // opaque
}

UtlBoolean SipUserAgent::resendWithAuthorization(SipMessage* response,
                                                                                                SipMessage* request,
                                                                                                int* messageType,
                                                                                                int authorizationEntity)
{
        UtlBoolean requestResent =FALSE;
        int sequenceNum;
        UtlString method;
        response->getCSeqField(&sequenceNum, &method);

    // The transaction sends the ACK for error cases now
        //if(method.compareTo(SIP_INVITE_METHOD , UtlString::ignoreCase) == 0)
        //{
                // Need to send an ACK to finish transaction
        //      SipMessage ackMessage;
        //      ackMessage.setAckData(response, request);
        //      send(ackMessage);
        //}

        SipMessage* authorizedRequest = new SipMessage();

#ifdef TEST_PRINT
    osPrintf("**************************************\n");
    osPrintf("CREATING message in resendWithAuthorization @ address: %X\n",authorizedRequest);
    osPrintf("**************************************\n");
#endif

        if ( mpLineMgr && mpLineMgr->buildAuthenticatedRequest(response, request,authorizedRequest))
        {
#ifdef TEST_PRINT
        osPrintf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
        UtlString authBytes;
        int authBytesLen;
        authorizedRequest->getBytes(&authBytes, &authBytesLen);
        osPrintf("Auth. message:\n%s", authBytes.data());
        osPrintf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
#endif

        requestResent = send(*authorizedRequest);
        // Send the response back to the application
        // to notify it of the CSeq change for the response
        *messageType = SipMessageEvent::AUTHENTICATION_RETRY;
        }
#ifdef TEST
    else
    {
        osPrintf("Giving up on entity %d authorization, userId: \"%s\"\n",
            authorizationEntity, dbUserId.data());
        osPrintf("authorization failed previously sent: %d\n",
            request->getAuthorizationField(&authField, authorizationEntity));
    }
#endif

    delete authorizedRequest;

    return(requestResent);
}

void SipUserAgent::lookupSRVSipAddress(UtlString protocol, UtlString& sipAddress, int& port, UtlString& srcIp)
{
    OsSocket::IpProtocolSocketType transport = OsSocket::UNKNOWN;

    if (sipIpAddress != "127.0.0.1")
    {
        server_t *server_list;
        server_list = SipSrvLookup::servers(sipAddress.data(),
                                            "sip",
                                            transport,
                                            port,
                                            srcIp.data());

        // The returned value is a sorted array of server_t with last element having host=NULL.
        // The servers are arranged in order of decreasing preference.
        if ( !server_list )
        {
#ifdef TEST_PRINT
            osPrintf("The DNS server is not SRV capable; \nbind servers v8.0 and above are SRV capable\n");
#endif
        }
        else
        {
            // The result array contains the hostname,
            //   socket type, IP address and port (in network byte order)
            //   DNS preference and weight
            server_t toServerUdp;
            server_t toServerTcp;
            int i;

#ifdef TEST_PRINT
            osPrintf("\n   Pref   Wt   Type    Name(IP):Port\n");
            for (i=0; SipSrvLookup::isValidServerT(server_list[i]); i++)
            {
                UtlString name;
                UtlString ip;
                    SipSrvLookup::getHostNameFromServerT(server_list[i],
                                                        name);
                    SipSrvLookup::getIpAddressFromServerT(server_list[i],
                                                        ip);
                osPrintf( "%6d %5d %5d   %s(%s):%d\n",
                    SipSrvLookup::getPreferenceFromServerT(server_list[i]),
                    SipSrvLookup::getWeightFromServerT(server_list[i]),
                    SipSrvLookup::getProtocolFromServerT(server_list[i]),
                    name.data(),
                    ip.data(),
                    SipSrvLookup::getPortFromServerT(server_list[i]) );
            }
#endif

            for (i=0; server_list[i].isValidServerT(); i++)
            {
                if (server_list[i].getProtocolFromServerT() ==
                    OsSocket::UDP)
                {
                    if (! toServerUdp.isValidServerT())
                    {
                        toServerUdp = server_list[i];
#ifdef TEST_PRINT
                        UtlString name;
                        SipSrvLookup::getHostNameFromServerT(toServerUdp,
                                                            name);
                        osPrintf("UDP server %s\n", name.data());
#endif
                    }
                }
                else if (server_list[i].getProtocolFromServerT() ==
                         OsSocket::TCP)
                {
                    if (toServerTcp.isValidServerT())
                    {
                        toServerTcp = server_list[i];
#ifdef TEST_PRINT
                        UtlString name;
                        SipSrvLookup::getHostNameFromServerT(toServerTcp,
                                                            name);
                        osPrintf("TCP server %s\n", name.data());
#endif
                    }
                }
            }

            if (!protocol.compareTo("TCP") &&
                toServerTcp.isValidServerT())
            {
                int newPort = toServerTcp.getPortFromServerT();
                if (portIsValid(newPort))
                {
                    toServerTcp.getIpAddressFromServerT(sipAddress);
                    port = newPort;
                }
                OsSysLog::add(FAC_SIP, PRI_DEBUG,"SipUserAgent:: found TCP server %s port %d",
                              sipAddress.data(), newPort
                              );
            }
            else if (toServerUdp.isValidServerT())
            {
                int newPort = toServerUdp.getPortFromServerT();
                if (portIsValid(newPort))
                {
                    toServerUdp.getIpAddressFromServerT(sipAddress);
                    port = newPort;
                }
#ifdef TEST_PRINT
                osPrintf("found UDP server %s port %d/%d\n",
                   sipAddress.data(), newPort,
                   SipSrvLookup::getPortFromServerT(toServerUdp));
#endif
            }

            delete[] server_list;
        }
    }
}

void SipUserAgent::setServerHeader(SipMessage& message)
{
   UtlString existing;
   message.getServerField(&existing);

   if(existing.isNull())
   {
      UtlString headerValue;
      selfHeaderValue(headerValue);

      message.setServerField(headerValue.data());
   }
}

void SipUserAgent::setUserAgentHeader(SipMessage& message)
{
   UtlString uaName;
   message.getUserAgentField(&uaName);

   if(uaName.isNull())
   {
      selfHeaderValue(uaName);
      message.setUserAgentField(uaName.data());
   }
}

void SipUserAgent::selfHeaderValue(UtlString& self)
{
    self = defaultUserAgentName;

     if ( !mUserAgentHeaderProperties.isNull() )
     {
         self.append(mUserAgentHeaderProperties);
     }

     if (mbIncludePlatformInUserAgentName)
     {
         self.append(PLATFORM_UA_PARAM);
    }
}

void SipUserAgent::setIncludePlatformInUserAgentName(const bool bInclude)
{
    mbIncludePlatformInUserAgentName = bInclude;
}

const bool SipUserAgent::addContactAddress(SIPX_CONTACT_ADDRESS& contactAddress)
{
    bool bRC = mContactDb.updateContact(contactAddress) ;
    if (!bRC)
        bRC = mContactDb.addContact(contactAddress);

    return bRC ;
}

void SipUserAgent::getContactAddresses(SIPX_CONTACT_ADDRESS* pContacts[], int &numContacts)
{
    mContactDb.getAll(pContacts, numContacts);
}

void SipUserAgent::setHeaderOptions(const bool bAllowHeader,
                          const bool bDateHeader,
                          const bool bShortNames,
                          const UtlString& acceptLanguage)
{
    mbAllowHeader = bAllowHeader;
    mbDateHeader = bDateHeader;
    mbShortNames = bShortNames;
    mAcceptLanguage = acceptLanguage;
}                          

void SipUserAgent::prepareVia(SipMessage& message,
                              UtlString&  branchId, 
                              OsSocket::IpProtocolSocketType& toProtocol,
                              const char* szTargetAddress, 
                              const int*  piTargetPort,
                              SIPX_TRANSPORT_DATA* pTransport)
{
    UtlString viaAddress;
    UtlString viaProtocolString;
    SipMessage::convertProtocolEnumToString(toProtocol, viaProtocolString);
    if ((pTransport) && toProtocol == OsSocket::CUSTOM)
    {
        viaProtocolString = pTransport->szTransport ;
    }

    int viaPort;
    getViaInfo(toProtocol, viaAddress, viaPort, szTargetAddress, piTargetPort);

    // if the viaAddress is a local address that
    // has a STUN or RELAY address associated with it,
    // and the CONTACT is not a local address,
    // then change the viaAddress and port to match
    // the address and port from the Contact
    UtlString stunnedAddress;
    UtlString relayAddress;
    UtlString contactAddress;
    int contactPort;
    UtlString contactProtocol;
    SIPX_CONTACT_ADDRESS* pLocalContact = getContactDb().find(viaAddress, viaPort, CONTACT_LOCAL);
    SIPX_CONTACT_ADDRESS* pStunnedAddress = NULL;
    SIPX_CONTACT_ADDRESS* pRelayAddress = NULL;
    int numStunnedContacts = 0;
    const SIPX_CONTACT_ADDRESS* stunnedContacts[MAX_IP_ADDRESSES];
    int numRelayContacts = 0;
    const SIPX_CONTACT_ADDRESS* relayContacts[MAX_IP_ADDRESSES];

    if (pLocalContact)
    {
        getContactDb().getAllForAdapter(stunnedContacts,
                                                    pLocalContact->cInterface,
                                                    numStunnedContacts, 
                                                    CONTACT_NAT_MAPPED);

        getContactDb().getAllForAdapter(relayContacts,
                                                    pLocalContact->cInterface,
                                                    numRelayContacts, 
                                                    CONTACT_RELAY);

        int i = 0;
        bool bFound = false;
        while (!bFound && 
                TRUE == 
                message.getContactAddress(i++, &contactAddress, &contactPort, &contactProtocol))
        {
            int j = 0;
            for (j = 0; j < numStunnedContacts; j++)
            {
                if (strcmp(contactAddress, stunnedContacts[j]->cIpAddress) == 0)
                {
                    // contact is a stunned address, corresponding to 
                    // the local address in the via, so, use it
                    viaAddress = stunnedContacts[j]->cIpAddress;
                    viaPort = stunnedContacts[j]->iPort;
                    bFound = true;
                    break;
                }
            }
            for (j = 0; j < numRelayContacts; j++)
            {
                if (strcmp(contactAddress, relayContacts[j]->cIpAddress) == 0)
                {
                    // contact is a stunned address, corresponding to 
                    // the local address in the via, so, use it
                    viaAddress = relayContacts[j]->cIpAddress;
                    viaPort = relayContacts[j]->iPort;
                    bFound = true;
                    break;
                }
            }

        }
    }

    UtlString routeId ;
    if ((pTransport) && toProtocol == OsSocket::CUSTOM)
    {
        routeId = pTransport->cRoutingId ;
    }

    // Add the via field data
    message.addVia(viaAddress.data(),
                   viaPort,
                   viaProtocolString,
                   branchId.data(),
                   (toProtocol == OsSocket::UDP) && getUseRport(),
                   routeId.data());
    return;
}
void SipUserAgent::addExternalTransport(const UtlString transportName, const SIPX_TRANSPORT_DATA* const pTransport)
{
    const UtlString key = transportName + "|" + UtlString(pTransport->szLocalIp);
    mExternalTransports.insertKeyAndValue((UtlContainable*)new UtlString(key), new UtlVoidPtr((void*)pTransport));
    return;
}

void SipUserAgent::removeExternalTransport(const UtlString transportName, const SIPX_TRANSPORT_DATA* const pTransport)
{
    const UtlString key = transportName + "|" + UtlString(pTransport->szLocalIp);
    
    mExternalTransports.destroy((UtlContainable*)&key);
    return;
}

const SIPX_TRANSPORT_DATA* const SipUserAgent::lookupExternalTransport(const UtlString transportName, const UtlString ipAddress) const
{
    const UtlString key = transportName + "|" + ipAddress;
    UtlVoidPtr* pTransportContainer;
    
    pTransportContainer = (UtlVoidPtr*) mExternalTransports.findValue(&key);
    
    if (pTransportContainer)
    {
        const SIPX_TRANSPORT_DATA* const pTransport = (const SIPX_TRANSPORT_DATA* const) pTransportContainer->getValue();
        return pTransport;
    }
    return NULL;
}
#ifdef SIP_TLS
// ITlsSink implementations
bool SipUserAgent::onServerCertificate(void* pCert,
                                       char* serverHostName)
{
    bool bRet = true;

    if (!mpLastSipMessage)
    {
        bRet = false;
    }
    else
    {
        char szSubjAltName[256];

        memset(szSubjAltName, 0, sizeof(szSubjAltName));
        SmimeBody::getSubjAltName(szSubjAltName, (CERTCertificate*)pCert, sizeof(szSubjAltName));
        bRet = mpLastSipMessage->fireSecurityEvent(this,
                                    SECURITY_TLS,
                                    SECURITY_CAUSE_TLS_SERVER_CERTIFICATE,
                                    mpLastSipMessage->getSecurityAttributes(),
                                    pCert,
                                    szSubjAltName);
        if (!bRet)
        {
            mpLastSipMessage->fireSecurityEvent(this,
                                                SECURITY_TLS,
                                                SECURITY_CAUSE_TLS_CERTIFICATE_REJECTED,
                                                mpLastSipMessage->getSecurityAttributes(),
                                                pCert,
                                                szSubjAltName);
        }

    }
    return bRet;
}

bool SipUserAgent::onTlsEvent(int cause)
{
    bool bRet = true;

    return bRet;
}


#endif

/* //////////////////////////// PRIVATE /////////////////////////////////// */

/* ============================ FUNCTIONS ================================= */
