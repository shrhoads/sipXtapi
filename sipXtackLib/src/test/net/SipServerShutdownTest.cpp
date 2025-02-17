//
// Copyright (C) 2006-2010 SIPez LLC. 
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

#include <sipxunittests.h>

#include <os/OsDefs.h>
#include <os/OsTimerTask.h>
#include <os/OsProcess.h>
#include <net/SipMessage.h>
#include <net/SipUserAgent.h>
#include <net/SipLineMgr.h>
#include <net/SipRefreshMgr.h>
#include <net/SipTcpServer.h>

#define SIP_SHUTDOWN_ITERATIONS 3

/**
 * Unittest for server shutdown testing
 */
class SipServerShutdownTest : public SIPX_UNIT_BASE_CLASS
{
      CPPUNIT_TEST_SUITE(SipServerShutdownTest);
      CPPUNIT_TEST(testTcpShutdown);
      CPPUNIT_TEST_SUITE_END();

public:

   void testTcpShutdown()
   {
      SipUserAgent sipUA( PORT_NONE
                         ,PORT_NONE
                         ,PORT_NONE
                         ,NULL     // default publicAddress
                         ,NULL     // default defaultUser
                         ,"127.0.0.1"     // default defaultSipAddress
                         ,NULL     // default sipProxyServers
                         ,NULL     // default sipDirectoryServers
                         ,NULL     // default sipRegistryServers
                         ,NULL     // default authenticationScheme
                         ,NULL     // default authenicateRealm
                         ,NULL     // default authenticateDb
                         ,NULL     // default authorizeUserIds
                         ,NULL     // default authorizePasswords
         );

      for (int i=0; i<SIP_SHUTDOWN_ITERATIONS; ++i)
      {
         SipTcpServer pSipTcpServer(5090, &sipUA, SIP_TRANSPORT_TCP, 
                                    "SipTcpServer-%d", false);
         pSipTcpServer.startListener();

         OsTask::delay(1000);

         pSipTcpServer.shutdownListener();
      }

   };

};

CPPUNIT_TEST_SUITE_REGISTRATION(SipServerShutdownTest);
