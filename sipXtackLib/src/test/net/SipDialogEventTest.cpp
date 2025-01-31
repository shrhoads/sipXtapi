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
#include <net/SipDialogEvent.h>


/**
 * Unit test for SipDialogEvent
 */
class SipDialogEventTest : public SIPX_UNIT_BASE_CLASS
{
   CPPUNIT_TEST_SUITE(SipDialogEventTest);
   CPPUNIT_TEST(testDialogPackageParser);
   CPPUNIT_TEST(testDialogPackageFromPolycom);
   CPPUNIT_TEST_SUITE_END();

public:

   void testDialogPackageParser()
      {
         const char *package = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"0\" state=\"full\" entity=\"sip:moh@panther.pingtel.com:5120\">\n"
            "<dialog id=\"2\" call-id=\"call-1116603513-890@10.1.1.153\" local-tag=\"264460498\" remote-tag=\"1c10982\" direction=\"recipient\">\n"
            "<state>confirmed</state>\n"
            "<local>\n"
            "<identity>moh@panther.pingtel.com:5120</identity>\n"
            "<target uri=\"sip:moh@10.1.1.26:5120\"/>\n"
            "</local>\n"
            "<remote>\n"
            "<identity>4444@10.1.1.153</identity>\n"
            "</remote>\n"
            "</dialog>\n"
            "</dialog-info>\n"
            ;

         // Construct a SipDialogEvent from the XML.
         SipDialogEvent body(package);

         // Convert it back to XML, and see if it is the same.
         UtlString bodyString;
         int bodyLength;
       
         body.getBytes(&bodyString, &bodyLength);
       
         CPPUNIT_ASSERT(strcmp(bodyString.data(), package) == 0);

         // Extract the Dialog, and see if its call-Id matches.
         UtlSListIterator* itor = body.getDialogIterator();
         Dialog* pDialog = static_cast <Dialog*> ((*itor)());
         delete itor;

         UtlString callId;
         pDialog->getCallId(callId);

         UtlString refCallId = "call-1116603513-890@10.1.1.153";
         CPPUNIT_ASSERT(strcmp(callId.data(), refCallId.data()) == 0);

         // See that the reported event body length is right.
         int otherLength = body.getLength();
         CPPUNIT_ASSERT_EQUAL_MESSAGE("content length is not equal",
                                      bodyLength, otherLength);
      }

   void testDialogPackageFromPolycom()
      {
         const char *package = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"4\" state=\"partial\" entity=\"sip:222@panther.pingtel.com\">\n"
            "<dialog id=\"ida648720b\" call-id=\"62c3a00e-2f2662f8-53cfd2f7@10.1.20.231\" local-tag=\"B7691142-47C44851\" remote-tag=\"1900354342\" direction=\"initiator\">\n"
            "<state>terminated</state>\n"
            "<local>\n"
            "<target uri=\"sip:222@panther.pingtel.com\"/>\n"
            "</local>\n"
            "<remote>\n"
            "<identity>sip:100@panther.pingtel.com</identity>\n"
            "<target uri=\"sip:10.1.1.26:5100\"/>\n"
            "</remote>\n"
            "</dialog>\n"
            "</dialog-info>\n"
            ;
       
         // Construct a SipDialogEvent from the XML.
         SipDialogEvent body(package);

         // Convert it back to XML, and see if it is the same.
         UtlString bodyString;
         int bodyLength;
       
         body.getBytes(&bodyString, &bodyLength);
       
         CPPUNIT_ASSERT(strcmp(bodyString.data(), package) == 0);

         // Extract the Dialog, and see if its call-Id matches.
         UtlSListIterator* itor = body.getDialogIterator();
         Dialog* pDialog = static_cast <Dialog*> ((*itor)());
         delete itor;

         UtlString callId;
         pDialog->getCallId(callId);

         UtlString refCallId = "62c3a00e-2f2662f8-53cfd2f7@10.1.20.231";
         CPPUNIT_ASSERT(strcmp(callId.data(), refCallId.data()) == 0);

         // See that the reported event body length is right.
         int otherLength = body.getLength();
         CPPUNIT_ASSERT_EQUAL_MESSAGE("content length is not equal",
                                      bodyLength, otherLength);
      }
};

CPPUNIT_TEST_SUITE_REGISTRATION(SipDialogEventTest);
