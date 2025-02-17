//  
// Copyright (C) 2006 SIPez LLC. 
// Licensed to SIPfoundry under a Contributor Agreement. 
//
// Copyright (C) 2004-2006 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// $$
///////////////////////////////////////////////////////////////////////////////

#ifndef _OsNatAgentTask_h_	/* [ */
#define _OsNatAgentTask_h_

// SYSTEM INCLUDES

// APPLICATION INCLUDES
#include "os/IStunSocket.h"
#include "os/OsNatKeepaliveListener.h"
#include "os/OsServerTask.h"
#include "os/OsRpcMsg.h"
#include "os/OsEventMsg.h"
#include "utl/UtlHashMap.h"
#include "os/TurnMessage.h"
#include "os/StunMessage.h"
#include "os/NatMsg.h"

// DEFINES
#define SYNC_MSG_TYPE    (OsMsg::USER_START + 2)      /**< Synchronized Msg type/id */

#define NAT_INITIAL_ABORT_COUNT                 4       /** Abort after N times (first attempt) */
#define NAT_PROBE_ABORT_COUNT                   3       /** Abort STUN probes after N attempts */
#define NAT_RESEND_ABORT_COUNT                  75      /** Fail after N times (refreshes) */
#define NAT_RESPONSE_TIMEOUT_MS                 300     /** How long to wait for each attempt */

#define NAT_FIND_BINDING_POOL_MS                50      /** poll delay for contact searchs */
#define NAT_BINDING_EXPIRATION_SECS             60      /** expiration for bindings if new renewed */


// MACROS
// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS
// STRUCTS
// TYPEDEFS
typedef enum
{
    STUN_DISCOVERY,
    STUN_PROBE,
    TURN_ALLOCATION,
    CRLF_KEEPALIVE,
    STUN_KEEPALIVE
} NAT_AGENT_BINDING_TYPE ;

typedef enum
{
    SUCCESS,
    SENDING,
    SENDING_ERROR,
    RESENDING,
    RESENDING_ERROR,
    FAILED
} NAT_AGENT_STATUS ;


#define MAX_OLD_TRANSACTIONS    3
typedef struct
{
    NAT_AGENT_BINDING_TYPE  type ;
    NAT_AGENT_STATUS        status ;
    UtlString               serverAddress ;
    int                     serverPort ;
    int                     options ;
    STUN_TRANSACTION_ID     transactionId ;
    int                     nOldTransactions ;
    STUN_TRANSACTION_ID     oldTransactionsIds[MAX_OLD_TRANSACTIONS] ;
    IStunSocket*            pSocket ;
    OsTimer*                pTimer ;
    int                     keepAliveSecs ;
    int                     abortCount ;
    int                     refreshErrors ;
    UtlString               address ;
    int                     port ;
    UtlString               username ;  // TURN_ALLOCATION only
    UtlString               password ;  // TURN_ALLOCATION only
    int                     priority ;  // STUN_PROBE only
    OsNatKeepaliveListener* pKeepaliveListener ;
} NAT_AGENT_CONTEXT ;


typedef struct 
{
    OsSocket*    pSocket ;
    UtlString    remoteAddress ;
    int          remotePort ;
    UtlString    contactAddress ;
    int          contactPort ;
    OsTime       expiration ;
} NAT_AGENT_EXTERNAL_CONTEXT ;


// FORWARD DECLARATIONS

/**
 * The OsNatAgentTask is responsible for servicing all stun requests and
 * and responses on behalf of the IStunSocket.  This handles the 
 * stun requests/responses however relies on someone else to pump sockets.
 *
 * Use cases:
 *
 *   1) Send a STUN request via a supplied IStunSocket
 *   2) Process responses from a IStunSocket
 *   3) Process server requests from a IStunSocket
 */
class OsNatAgentTask : public OsServerTask
{
/* //////////////////////////// PUBLIC //////////////////////////////////// */

/* ============================ CREATORS ================================== */

private:

    /**
     * Private constructor, use getInstance() 
     */
    OsNatAgentTask();

    /**
     * Private destuctor, use freeInstance() ;
     */
    virtual ~OsNatAgentTask();

public:
    /**
     * Obtain a singleton instance
     */
    static OsNatAgentTask* getInstance() ;

    /**
     * Release/Free the singleton instance obtained by calling getInstance.
     * This method is included for clean shutdown of the system.
     */
    static void releaseInstance() ;

/* ============================ MANIPULATORS ============================== */

    /**
     * Standard OsServerTask message handler -- used to process timer 
     * messages for stun refreshes, reads, etc.
     */
    virtual UtlBoolean handleMessage(OsMsg& rMsg) ;

    UtlBoolean sendStunProbe(IStunSocket* pSocket,
                             const UtlString&     remoteAddress,
                             int                  remotePort,
                             int                  priority) ;

    UtlBoolean enableStun(IStunSocket* pSocket,
                          const UtlString&     stunServer,
                          int                  stunPort,                                      
                          const int            stunOptions,
                          int                  keepAlive) ;

    UtlBoolean disableStun(IStunSocket* pSocket) ;

    UtlBoolean enableTurn(IStunSocket* pSocket,
                          const UtlString& turnServer,
                          int iTurnPort,
                          int keepAliveSecs,
                          const UtlString& username,
                          const UtlString& password) ;

    UtlBoolean primeTurnReception(IStunSocket* pSocket,
                                  const char* szAddress,
                                  int iPort ) ;

    UtlBoolean setTurnDestination(IStunSocket* pSocket,
                                  const char* szAddress,
                                  int iPort ) ;

    void disableTurn(IStunSocket* pSocket) ;

    UtlBoolean addCrLfKeepAlive(IStunSocket*    pSocket, 
                                const UtlString&        remoteIp,
                                int                     remotePort,
                                int                     keepAliveSecs,
                                OsNatKeepaliveListener* pListener) ;

    UtlBoolean removeCrLfKeepAlive(IStunSocket* pSocket,
                                   const UtlString&     serverIp,
                                   int                  serverPort) ;

    UtlBoolean addStunKeepAlive(IStunSocket*    pSocket, 
                                const UtlString&        remoteIp,
                                int                     remotePort,
                                int                     keepAliveSecs,
                                OsNatKeepaliveListener* pListener) ;

    UtlBoolean removeStunKeepAlive(IStunSocket* pSocket,
                                   const UtlString&     serverIp,
                                   int                  serverPort) ;

    UtlBoolean removeKeepAlives(IStunSocket* pSocket) ;

    UtlBoolean removeStunProbes(IStunSocket* pSocket) ;

    /**
     * Synchronize with the OsNatAgentTask by posting a message to this event
     * queue and waiting for that message to be processed.  Do not call this
     * method from the OsNatAgentTask's thread context (will block forever).
     */
    void synchronize() ;

    /**
     * Determines if probes of a higher priority are still outstanding
     */
    UtlBoolean areProbesOutstanding(IStunSocket* pSocket, int priority) ;

    /**
     * Does a binding of the designated type/server exist 
     */
    UtlBoolean doesBindingExist(IStunSocket*   pSocket,
                                NAT_AGENT_BINDING_TYPE type, 
                                const UtlString&       serverIp,
                                int                    serverPort) ;

    /**
     * Accessor for the timer object. 
     */
    OsTimer* getTimer() ;

    /* ============================ ACCESSORS ================================= */

    /**
     * Look at all of the stun data structures and see if you can find a 
     * known back-route to the specified destination.
     */
    UtlBoolean findContactAddress(  const UtlString& destHost, 
                                    int              destPort, 
                                    UtlString*       pContactHost, 
                                    int*             pContactPort,
                                    int              iTimeoutMs = 0) ;

    /**
     * Add an external binding (used for findContactAddress)
     */
    void addExternalBinding(OsSocket*  pSocket,
                            UtlString  remoteAddress,
                            int        remotePort,
                            UtlString  contactAddress,
                            int        contactPort) ;

    void clearExternalBinding(OsSocket*  pSocket,
                              UtlString  remoteAddress,
                              int        remotePort,
                              bool       bOnlyIfEmpty = false) ;


    /**
     * Locate an external binding for the specified destination host/port.  
     * This API while block while wait for a result.
     */
    UtlBoolean findExternalBinding(const UtlString& destHost, 
                                   int              destPort, 
                                   UtlString*       pContactHost, 
                                   int*             pContactPort,
                                   int              iTimeoutMs = 0,
                                   UtlBoolean*      pTimedOut = NULL) ;

/* ============================ INQUIRY =================================== */

/* //////////////////////////// PROTECTED ///////////////////////////////// */
protected:

    virtual UtlBoolean handleTimerEvent(NAT_AGENT_CONTEXT* pContext) ;

    virtual void handleStunTimeout(NAT_AGENT_CONTEXT* pContext) ;

    virtual void handleTurnTimeout(NAT_AGENT_CONTEXT* pContext) ;

    virtual UtlBoolean handleCrLfKeepAlive(NAT_AGENT_CONTEXT* pContext) ;

    virtual UtlBoolean handleStunKeepAlive(NAT_AGENT_CONTEXT* pContext) ;

    /**
     * Handle an inbound Stun message.  The messages are handled to this 
     * thread by the IStunSocket whenever someone calls one of the 
     * read methods.
     */
    virtual UtlBoolean handleStunMessage(NatMsg& rMsg) ;


    /**
     * Handle an inbound Turn message.  The messages are handled to this 
     * thread by the IStunSocket whenever someone calls one of the 
     * read methods.
     */
    virtual UtlBoolean handleTurnMessage(NatMsg& rMsg) ;


    /**
     * Handle a synchronization request.  Synchronization consists of sending
     * a message and waiting for that messsage to be processed.
     */
    virtual UtlBoolean handleSynchronize(OsRpcMsg& rMsg) ;


    virtual UtlBoolean sendMessage(StunMessage* pMsg, 
                                   IStunSocket* pSocket, 
                                   const UtlString& toAddress, 
                                   unsigned short toPort,
                                   PacketType packetType = UNKNOWN_PACKET) ;

    NAT_AGENT_CONTEXT* getBinding(IStunSocket* pSocket, NAT_AGENT_BINDING_TYPE type) ;

    NAT_AGENT_CONTEXT* getBinding(NAT_AGENT_CONTEXT* pContext) ;

    NAT_AGENT_CONTEXT* getBinding(STUN_TRANSACTION_ID* pId) ;

    void destroyBinding(NAT_AGENT_CONTEXT* pBinding) ;

    void releaseTimer(OsTimer* pTimer) ;

    UtlBoolean sendStunRequest(NAT_AGENT_CONTEXT* pBinding) ;
    
    UtlBoolean sendTurnRequest(NAT_AGENT_CONTEXT* pBinding) ;

    void markStunFailure(NAT_AGENT_CONTEXT* pBinding) ;

    void markStunSuccess(NAT_AGENT_CONTEXT* pBinding, const UtlString& mappedAddress, int mappedPort) ;

    void markTurnFailure(NAT_AGENT_CONTEXT* pBinding) ;

    void markTurnSuccess(NAT_AGENT_CONTEXT* pBinding, const UtlString& relayAddress, int relayPort) ;

    OsNatKeepaliveEvent populateKeepaliveEvent(NAT_AGENT_CONTEXT* pContext) ;

    void dumpContext(UtlString* pResults, NAT_AGENT_CONTEXT* pBinding) ;    


/* //////////////////////////// PRIVATE /////////////////////////////////// */
private:
    static OsNatAgentTask* spInstance ;    /**< Singleton instance */
    static OsMutex sLock ;                  /**< Lock for singleton accessors */    
    UtlSList mTimerPool;                    /**< List of free timers available for use */
    UtlHashMap mContextMap ;
    OsMutex mMapsLock ;                     /**< Lock for Notify and Connectiviy maps */

    UtlSList  mExternalBindingsList ;
    OsRWMutex mExternalBindingMutex ;
    
    
    /** Disabled copy constructor (not supported) */
    OsNatAgentTask(const OsNatAgentTask& rOsNatAgentTask);     

    /** Disabled equal operators (not supported) */
    OsNatAgentTask& operator=(const OsNatAgentTask& rhs);  
   
};

/* ============================ INLINE METHODS ============================ */

#endif  /* _OsNatAgentTask_h_ ] */
