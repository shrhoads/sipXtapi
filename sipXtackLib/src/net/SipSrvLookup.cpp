//  
// Copyright (C) 2006-2013 SIPez LLC.  All rights reserved.
//
// Copyright (C) 2004-2007 SIPfoundry Inc.
// Licensed by SIPfoundry under the LGPL license.
//
// Copyright (C) 2004-2006 Pingtel Corp.  All rights reserved.
// Licensed to SIPfoundry under a Contributor Agreement.
//
// Rewritten based on DomainSearch by Christian Zahl, and SipSrvLookup
// by Henning Schulzrinne.
//
// $$
///////////////////////////////////////////////////////////////////////////////

#include <os/OsIntTypes.h>

#if defined(_WIN32)
#       include "resparse/wnt/sysdep.h"
#       include <resparse/wnt/netinet/in.h>
#       include <resparse/wnt/arpa/nameser.h>
#       include <resparse/wnt/resolv/resolv.h>
#       include <winsock2.h>
extern "C" {
#       include "resparse/wnt/inet_aton.h"       
}
#elif defined(_VXWORKS)
#       include <stdio.h>
#       include <netdb.h>
#       include <netinet/in.h>
#       include <vxWorks.h>
/* Use local lnameser.h for info missing from VxWorks version --GAT */
/* lnameser.h is a subset of resparse/wnt/arpa/nameser.h                */
#       include <resolv/nameser.h>
#       include <resparse/vxw/arpa/lnameser.h>
/* Use local lresolv.h for info missing from VxWorks version --GAT */
/* lresolv.h is a subset of resparse/wnt/resolv/resolv.h               */
#       include <resolv/resolv.h>
#       include <resparse/vxw/resolv/lresolv.h>
/* #include <sys/socket.h> used sockLib.h instead --GAT */
#       include <sockLib.h>
#       include <resolvLib.h>
#elif defined(__pingtel_on_posix__)
#       include <arpa/inet.h>
#       include <netinet/in.h>
#       include <sys/socket.h>
#       include <resolv.h>
#       include <netdb.h>
#else
#       error Unsupported target platform.
#endif

#ifndef __pingtel_on_posix__
extern struct __res_state _sip_res;
#endif
#ifdef WINCE
#   include <types.h>
#else
#   include <sys/types.h>
#endif
// Standard C includes.
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

// Application includes.
#include <os/OsDefs.h>
#include <os/OsSocket.h>
#include <os/OsLock.h>
#include <net/SipSrvLookup.h>

#include <os/OsSysLog.h>
#include "resparse/rr.h"

// The space allocated for returns from res_query.
#define DNS_RESPONSE_SIZE 4096

// The initial value of OptionCodeCNAMELImit.
#define DEFAULT_CNAME_LIMIT 5

// Forward references

// All of these functions are made forward references here rather than
// being protected methods in SipSrvLookup.h because some of them
// require #include "resparse/rr.h", which ultimately includes
// /usr/include/arpa/nameser_compat.h, which #defines STATUS, which is
// used in other places in our code for other purposes.

/**
 * @name Server List
 *
 * These methods maintain a list of servers and their properties that have
 * been found so far during the current search.
 * The list is a malloc'ed array of pointers to server_t's, and is
 * represented by a pointer to the array, a count of the allocated length
 * of the array, and a count of the number of entries in the array that
 * are used.
 */
///@{

/// Initialize the variables pointing to the list of servers found thus far.
static void server_list_initialize(server_t*& list,
                                   int& list_length_allocated,
                                   int& list_length_used);

///@}

/// Insert records into the list for a server address.
static void server_insert_addr(
   /// List control variables.
   server_t*& list,
   int& list_length_allocated,
   int& list_length_used,
   /// Components of the server_t.
   const char *host,
   ///< (copied)
   OsSocket::IpProtocolSocketType type,
   struct sockaddr_in sin,
   unsigned int priority,
   unsigned int weight);
/**<
 * If type is UNKNOWN (meaning no higher-level process has specified
 * the transport to this server, server_insert_addr may insert two
 * records, one for UDP and one for TCP.
 */

/**
 * Add server_t to the end of a list of server addresses.
 * Calculates sorting score.
 */
static void server_insert(
   /// List control variables.
   server_t*& list,
   int& list_length_allocated,
   int& list_length_used,
   /// Components of the server_t.
   const char *host,
   ///< (copied)
   OsSocket::IpProtocolSocketType type,
   struct sockaddr_in sin,
   unsigned int priority,
   unsigned int weight);

/**
 * Look up SRV records for a domain name, and from them find server
 * addresses to insert into the list of servers.
 */
static void lookup_SRV(server_t*& list,
                       int& list_length_allocated,
                       int& list_length_used,
                       const char *domain,
                       ///< domain name
                       const char *service,
                       ///< "sip" or "sips"
                       const char *proto_string,
                       ///< protocol string for DNS lookup
                       OsSocket::IpProtocolSocketType proto_code,
                       ///< protocol code for result list
                       const char* srcIp
   );

/**
 * Look up A records for a domain name, and insert them into the list
 * of servers.
 */
static void lookup_A(server_t*& list,
                     int& list_length_allocated,
                     int& list_length_used,
                     const char *domain,
                     ///< domain name
                     OsSocket::IpProtocolSocketType proto_code,
                     /**< protocol code for result list
                      *   UNKNOWN means both UDP and TCP are acceptable
                      *   SSL must be set explicitly. */
                     res_response* in_response,
                     ///< current DNS response, or NULL
                     int port,
                     ///< port
                     unsigned int priority,
                     ///< priority
                     unsigned int weight
                     ///< weight
   );
/**<
 * If in_response is non-NULL, use it as an initial source of A records.
 *
 * @returns TRUE if one or more addresses were added to the list.
 */

/**
 * Search for an RR with 'name' and 'type' in the answer and additional
 * sections of a DNS response.
 *
 * @return pointer to rdata structure for the first RR founr, or NULL.
 */
static union u_rdata* look_for(res_response* response,
                               ///< response to look in
                               const char* name,
                               ///< domain name
                               int type
                               ///< RR type
   );

/// Function to compare two server entries.
static int server_compare(const void* a, const void* b);
/**<
 * Compares two server_t's which represent two servers.
 * Used by qsort to sort the list of server entries into preference
 * order.  The sort rules are that the first (smallest) element is:
 * # Lowest priority
 * # Highest weighting score
 * Transport type (UDP, TCP, etc.) is ignored.
 *
 * @returns Integer comparison result as needed by qsort.
 */

static void sort_answers(res_response* response);

static int rr_compare(const void* a, const void* b);

/**
 * The array of option values.
 *
 * Set the initial values.
 */
int SipSrvLookup::options[OptionCodeLast+1] = {
   0,                           // OptionCodeNone
   0,                           // OptionCodeFirst
   0,                           // OptionCodeIgnoreSRV
   0,                           // OptionCodeIgnoreNAPTR
   0,                           // OptionCodeSortAnswers
   0,                           // OptionCodePrintAnswers
   DEFAULT_CNAME_LIMIT,         // OptionCodeCNAMELimit
   0,                           // OptionCodeNoDefaultTCP
   0                            // OptionCodeLast
};

/* //////////////////////////// PUBLIC //////////////////////////////////// */

/// Get the list of server entries for SIP domain name 'domain'.
server_t* SipSrvLookup::servers(const char* domain,
                                ///< SIP domain name or host name
                                const char* service,
                                ///< "sip" or "sips"
                                OsSocket::IpProtocolSocketType socketType,
                                ///< types of transport
                                int port,
                                ///< port number from URI, or PORT_NONE
                                const char* srcIp
                                ///< the outgoing interface ip to send the request on
   )
{
   server_t* list;
   int list_length_allocated;
   int list_length_used = 0;
   struct sockaddr_in in;

   OsSysLog::add(FAC_SIP, PRI_DEBUG,
                 "SipSrvLookup::servers domain = '%s', service = '%s', "
                 "socketType = %s, port = %d",
                 domain, service, OsSocket::ipProtocolString(socketType), port);

   // Initialize the list of servers.
   server_list_initialize(list, list_length_allocated, list_length_used);

   // Seize the lock.
   OsLock lock(sMutex);

   // Case 0: Eliminate contradictory combinations of service and type.
   
   // While a sip: URI can be used with a socketType of SSL_SOCKET
   // (e.g., <sip:foo@example.com;transport=tls>), a sips: URI must
   // be used with TLS.
   if ((strcmp(service, "sips") == 0 &&
        (socketType == OsSocket::TCP || socketType == OsSocket::UDP)))
   {
      OsSysLog::add(FAC_SIP, PRI_INFO,
                    "SipSrvLookup::servers Incompatible service '%s' and "
                    "socketType %d",
                    service, socketType);
      /* Add no elements to the list. */
   }
   else
   // Case 1: Domain name is a numeric IP address.
   if ( IS_INET_RETURN_OK( inet_aton((char *)domain, &in.sin_addr)) )
   {
      OsSysLog::add(FAC_SIP, PRI_DEBUG,
                    "SipSrvLookup::servers IP address ('%s') no DNS lookup", domain);

      in.sin_family = AF_INET;
      // Set up the port number.
      // If port was specified in the URI, that is the port to use.
      // Otherwise, if the service is sips, use 5061.  Otherwise use 5060.
      in.sin_port = htons(portIsValid(port) ? port :
                          ((strcmp(service, "sips") == 0) || (socketType == OsSocket::SSL_SOCKET)) ? 
                          5061 : 5060);
      // Set the transport if it is not already set for SIPS.
      if (socketType == OsSocket::UNKNOWN &&
          strcmp(service, "sips") == 0)
      {
         socketType = OsSocket::SSL_SOCKET;
      }
      server_insert_addr(list, list_length_allocated, list_length_used,
                         domain, socketType, in, 0, 0);
   }
   else
   {
      OsSysLog::add(FAC_SIP, PRI_DEBUG,
                    "SipSrvLookup::servers DNS SRV lookup for address: '%s'", domain);

      // Case 2: SRV records exist for this domain.
      // (Only used if no port is specified in the URI.)
      if (port <= 0 && !options[OptionCodeIgnoreSRV])
      {
         // If UDP transport is acceptable.
         if ((socketType == OsSocket::UNKNOWN ||
              socketType == OsSocket::UDP) &&
             strcmp(service, "sips") != 0)
         {
            lookup_SRV(list, list_length_allocated, list_length_used,
                       domain, service, "udp", OsSocket::UDP, srcIp);
         }
         // If TCP transport is acceptable.
         if ((socketType == OsSocket::UNKNOWN ||
              socketType == OsSocket::TCP) &&
             strcmp(service, "sips") != 0)
         {
            lookup_SRV(list, list_length_allocated, list_length_used,
                       domain, service, "tcp", OsSocket::TCP, srcIp);
         }

         // If TLS transport is acceptable.
         if (socketType == OsSocket::UNKNOWN ||
              socketType == OsSocket::SSL_SOCKET)
         {
            lookup_SRV(list, list_length_allocated, list_length_used,
                       domain, service, "tls", OsSocket::SSL_SOCKET, srcIp);
         }
      }
      // Case 3: Look for A records.
      // (Only used for non-numeric addresses for which SRV lookup did not
      // produce any addresses.  This includes if an explicit port was given.)
      if (list_length_used == 0)
      {
         OsSysLog::add(FAC_SIP, PRI_DEBUG,
                       "SipSrvLookup::servers DNS lookup A record for address: '%s'", domain);

         lookup_A(list, list_length_allocated, list_length_used,
                  domain,
                  // Default the transport for "sips".
                  (socketType == OsSocket::UNKNOWN &&
                   strcmp(service, "sips") == 0) ?
                  OsSocket::SSL_SOCKET : socketType,
                  // must do a query.
                  NULL,
                  // Default the port if it is not already set.
                  (portIsValid(port) ? port :
                  ((strcmp(service, "sips") == 0) || (socketType == OsSocket::SSL_SOCKET)) ? 
                  5061 : 5060),
                  // Set the priority and weight to 0.
                  0, 0);
      }
   }

   // Sort the list of servers found by priority and score.
   qsort(list, list_length_used, sizeof (server_t), server_compare);

   // Add ending empty element to list (after sorting the real entries).
   memset(&in, 0, sizeof(in)) ;
   server_insert(list, list_length_allocated, list_length_used,
                 NULL, OsSocket::UNKNOWN, in, 0, 0);

   // Return the list of servers.
   if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
   {
      // Debugging print of list of servers.
      for (int j = 0; j < list_length_used; j++)
      {
         if (list[j].isValidServerT())
         {
            UtlString host;
            list[j].getHostNameFromServerT(host);
            UtlString ip_addr;
            list[j].getIpAddressFromServerT(ip_addr);
            OsSysLog::add(FAC_SIP, PRI_DEBUG,
                          "SipSrvLookup::servers host = '%s', IP addr = '%s', "
                          "port = %d, weight = %u, score = %f, "
                          "priority = %u, proto = %s",
                          host.data(), ip_addr.data(),
                          list[j].getPortFromServerT(),
                          list[j].getWeightFromServerT(),
                          list[j].getScoreFromServerT(),
                          list[j].getPriorityFromServerT(),
                          OsSocket::ipProtocolString(list[j].getProtocolFromServerT())
                          );
         }
      }
   }
   return list;
}

/// Set an option value.
void SipSrvLookup::setOption(OptionCode option, int value)
{
   // Seize the lock, to ensure atomic effect.
   OsLock lock(sMutex);

   options[option] = value;
}

//! Sets the DNS SRV times.  Defaults: timeout=5, retries=4
void SipSrvLookup::setDnsSrvTimeouts(int initialTimeoutInSecs, int retries)
{
   OsSysLog::add(FAC_NET, PRI_DEBUG, "SipSrvLookup::setDnsSrvTimeouts(initialTimeoutInSecs=%d, retries=%d)", 
                 initialTimeoutInSecs, retries);
#if defined(ANDROID)
   // Could not put code in here for Android due to header file errors/conflicts in C++ code
   android_res_setDnsSrvTimeouts(initialTimeoutInSecs, retries);

#else
   if (initialTimeoutInSecs > 0)
   {
#  if defined(__pingtel_on_posix__)
      _res.retrans = initialTimeoutInSecs;
#  else
      _sip_res.retrans = initialTimeoutInSecs;
#  endif
   }

   if (retries > 0)
   {
#  if defined(__pingtel_on_posix__)
      _res.retry = retries;
#  else
      _sip_res.retry = retries;
#  endif
   }
#endif /* non ANDROID */
}

void  SipSrvLookup::getDnsSrvTimeouts(int& initialTimeoutInSecs, int& retries)
{
#if defined(ANDROID)
   // Could not put code in here for Android due to header file errors/conflicts in C++ code
   android_res_getDnsSrvTimeouts(&initialTimeoutInSecs, &retries);

#else

#  if defined(__pingtel_on_posix__)
   initialTimeoutInSecs = _res.retrans;
#  else
   initialTimeoutInSecs = _sip_res.retrans;
#  endif

#endif
}

/* //////////////////////////// PROTECTED ///////////////////////////////// */

/*
 * Lock to protect the resolver routines, which cannot tolerate multithreaded
 * use.
 */
OsMutex SipSrvLookup::sMutex(OsMutex::Q_PRIORITY |
                             OsMutex::DELETE_SAFE |
                             OsMutex::INVERSION_SAFE);

// Initialize the variables pointing to the list of servers found thus far.
void server_list_initialize(server_t*& list,
                            int& list_length_allocated,
                            int& list_length_used)
{
   list_length_allocated = 2;
   list = new server_t[list_length_allocated];
   list_length_used = 0;
}

// Add server_t to the end of a list of server addresses.
void server_insert_addr(server_t*& list,
                        int& list_length_allocated,
                        int& list_length_used,
                        const char* host,
                        OsSocket::IpProtocolSocketType type,
                        struct sockaddr_in sin,
                        unsigned int priority,
                        unsigned int weight)
{
   if (type != OsSocket::UNKNOWN)
   {
      // If the transport is specified, just insert the record.
      server_insert(list, list_length_allocated, list_length_used,
                    host, type, sin, priority, weight);
   }
   else
   {
      // If the transport is not specified, insert a UDP record.
      server_insert(list, list_length_allocated, list_length_used,
                    host, OsSocket::UDP, sin, priority, weight);
      // If specified, insert a TCP record.
      if (!SipSrvLookup::getOption(SipSrvLookup::OptionCodeNoDefaultTCP))
      {
         server_insert(list, list_length_allocated, list_length_used,
                       host, OsSocket::TCP, sin, priority, weight);
      }
   }
}

// Add server_t to the end of a list of server addresses.
void server_insert(server_t*& list,
                   int& list_length_allocated,
                   int& list_length_used,
                   const char* host,
                   OsSocket::IpProtocolSocketType type,
                   struct sockaddr_in sin,
                   unsigned int priority,
                   unsigned int weight)
{
   // Make sure there is room in the list.
   if (list_length_used == list_length_allocated)
   {
      // Allocate the new list.
      int new_length = 2 * list_length_allocated;
      server_t* new_list = new server_t[new_length];
      // Copy all the elements binarily, to avoid the overhead of
      // duplicating all the host strings.
      bcopy((char*) list, (char*) new_list,
            list_length_used * sizeof (server_t));
      // Erase the host pointers in the old list.
      for (int i = 0; i < list_length_used; i++)
      {
         list[i].host = NULL;
      }
      // Free the old list.
      delete[] list;
      // Replace the old list with the new one.
      list = new_list;
      list_length_allocated = new_length;
   }

   // Copy the element into the list.
   list[list_length_used].host =
      host != NULL ? strdup(host) : NULL;
   list[list_length_used].type = type;
   list[list_length_used].sin = sin;
   list[list_length_used].priority = priority;
   list[list_length_used].weight = weight;
   // Construct the score.
   // Why we construct it this way is described in
   // sipXtackLib/doc/developer/scores/README.
   if (weight == 0)
   {
      // If weight is 0, set score to infinity.
      list[list_length_used].score = 1000;
   }
   else
   {
      int i = rand();
      // If random number is 0, change it to 1, so log() doesn't have a problem.
      if (i == 0)
      {
         i = 1;
      }
      list[list_length_used].score = - log(((float) i) / RAND_MAX) / weight;
   }

   // Increment the count of elements in the list.
   list_length_used++;
}

/*
 * Look up SRV records for a domain name, and from them find server
 * addresses to insert into the list of servers.
 */
void lookup_SRV(server_t*& list,
                int& list_length_allocated,
                int& list_length_used,
                const char* domain,
                ///< domain name
                const char* service,
                ///< "sip" or "sips"
                const char* proto_string,
                ///< protocol string for DNS lookup
                OsSocket::IpProtocolSocketType proto_code,
                ///< protocol code for result list
                const char* srcIp
   )
{
   // To hold the return of res_query_and_parse.
   res_response* response;
   const char* canonical_name;

   // Construct buffer to hold the key string for the lookup:
   //    _service._protocol.domain
   // 5 bytes suffices for the added components and the ending NUL.
   char* lookup_name = (char*) malloc(strlen(service) + strlen(proto_string) +
                                      strlen(domain) + 5);

   // Construct the domain name to search on.
   sprintf(lookup_name, "_%s._%s.%s", service, proto_string, domain);

#if defined(_WIN32)
   // set the srcIp, and populate the DNS server list
   res_init_ip(srcIp);
#else
   res_init();
#endif

   // Make the query and parse the response.
   SipSrvLookup::res_query_and_parse(lookup_name, T_SRV, NULL, canonical_name,
                                     response);
   if (response != NULL)
   {
       unsigned int i;
      // For each answer that is an SRV record for this domain name.

      // Search the answer list of RRs.
      for (i = 0; i < response->header.ancount; i++)
      {
         if (response->answer[i]->rclass == C_IN &&
             response->answer[i]->type == T_SRV &&
             // Note we look for the canonical name now.
             strcasecmp(canonical_name, response->answer[i]->name) == 0)
         {
            // Call lookup_A to get the A records for the target host
            // name.  Give it the pointer to our current response,
            // because it might have the A records.  If not, lookup_A
            // will do a DNS lookup to get them.
            lookup_A(list, list_length_allocated, list_length_used,
                     response->answer[i]->rdata.srv.target, proto_code,
                     response,
                     response->answer[i]->rdata.srv.port,
                     response->answer[i]->rdata.srv.priority,
                     response->answer[i]->rdata.srv.weight);
         }
      }
      // Search the additional list of RRs.
      for (i = 0; i < response->header.arcount; i++)
      {
         if (response->additional[i]->rclass == C_IN &&
             response->additional[i]->type == T_SRV &&
             // Note we look for the canonical name now.
             strcasecmp(canonical_name, response->additional[i]->name) == 0)
         {
            // Call lookup_A to get the A records for the target host
            // name.  Give it the pointer to our current response,
            // because it might have the A records.  If not, lookup_A
            // will do a DNS lookup to get them.
            lookup_A(list, list_length_allocated, list_length_used,
                     response->additional[i]->rdata.srv.target, proto_code,
                     response,
                     response->additional[i]->rdata.srv.port,
                     response->additional[i]->rdata.srv.priority,
                     response->additional[i]->rdata.srv.weight);
         }
      }
   }

   // Free the result of res_parse.
   if (response != NULL)
   {
      res_free(response);
   }
   if (canonical_name != NULL && canonical_name != lookup_name)
   {
      free((void*) canonical_name);
   }
   free((void*) lookup_name);
}

/*
 * Look up A records for a domain name, and insert them into the list
 * of servers.
 */
void lookup_A(server_t*& list,
              int& list_length_allocated,
              int& list_length_used,
              const char* domain,
              ///< domain name
              OsSocket::IpProtocolSocketType proto_code,
              ///< protocol code for result list
              res_response* in_response,
              ///< current DNS response, or NULL
              int port,
              ///< port
              unsigned int priority,
              ///< priority
              unsigned int weight
              ///< weight
   )
{
   // To hold the return of res_query_and_parse.
   res_response* response;
   const char* canonical_name;

   // Make the query and parse the response.
   SipSrvLookup::res_query_and_parse(domain, T_A, in_response, canonical_name,
                                     response);

   // Search the list of RRs.
   // For each answer that is an SRV record for this domain name.
   if (response != NULL)
   {
       unsigned int i;
      // Search the answer list.
      for (i = 0; i < response->header.ancount; i++)
      {
         if (response->answer[i]->rclass == C_IN &&
             response->answer[i]->type == T_A &&
             // Note we look for the canonical name now.
             strcasecmp(canonical_name, response->answer[i]->name) == 0)
         {
            // An A record has been found.
            // Assemble the needed information and add it to the server list.
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_addr = response->answer[i]->rdata.address;
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);
            server_insert_addr(list, list_length_allocated,
                               list_length_used,
                               (const char*) domain,
                               proto_code, sin, priority, weight);
         }
      }
      // Search the additional list.
      for (i = 0; i < response->header.arcount; i++)
      {
         if (response->additional[i]->rclass == C_IN &&
             response->additional[i]->type == T_A &&
             // Note we look for the canonical name now.
             strcasecmp(canonical_name, response->additional[i]->name) == 0)
         { 
            // An A record has been found.
            // Assemble the needed information and add it to the server list.
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_addr = response->additional[i]->rdata.address;
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);
            server_insert_addr(list, list_length_allocated,
                               list_length_used,
                               (const char*) domain,
                               proto_code, sin, priority, weight);
         }
      }
   }
   
   // Free the result of res_parse if necessary.
   if (response != NULL && response != in_response)
   {
      res_free(response);
   }
   if (canonical_name != NULL && canonical_name != domain)
   {
      free((void*) canonical_name);
   }
}

// Perform a DNS query and parse the results.  Follows CNAME records.
void SipSrvLookup::res_query_and_parse(const char* in_name,
                         int type,
                         res_response* in_response,
                         const char*& out_name,
                         res_response*& out_response
   )
{
   // The number of CNAMEs we have followed.
   int cname_count = 0;
   // The response currently being examined.
   res_response* response = in_response;
   // The name currently being examined.
   const char* name = in_name;
   // TRUE if 'response' was a lookup for 'name' and 'type'.
   UtlBoolean response_for_this_name = FALSE;
   // Buffer into which to read DNS replies.
   char answer[DNS_RESPONSE_SIZE];
   union u_rdata* p;
   
   // Loop until we find a reason to exit.  Each turn around the loop does
   // another DNS lookup.
   while (1)
   {
      // While response != NULL and there is a CNAME record for name
      // in response.
      while (response != NULL &&
             (p = look_for(response, name, T_CNAME)) != NULL)
      {
         cname_count++;
         if (cname_count > SipSrvLookup::getOption(SipSrvLookup::OptionCodeCNAMELimit))
         {
            break;
         }
         // If necessary, free the current 'name'.
         if (name != in_name)
         {
            free((void*) name);
         }
         // Copy the canonical name from the CNAME record into 'name', so
         // we can still use it after freeing 'response'.
         name = strdup(p->string);
         // Remember that we are now looking for a name that was not the one
         // that we searched for to produce this response.  Hence, if we don't
         // find any RRs for it, that is not authoritative and we have to do
         // another DNS query.
         response_for_this_name = FALSE;
         // Go back and check whether the result name of the CNAME is listed
         // in this response.
      }
      // This response does not contain a CNAME for 'name'.  So it is either
      // a final response that gives us the RRs we are looking for, or
      // we need to do a lookup on 'name'.

      // Check whether the response was for this name, or contains records
      // of the type we are looking for.  If either, then any records we
      // are looking for are in this response, so we can return.
      if (response_for_this_name ||
          (response != NULL && look_for(response, name, type) != NULL))
      {
         break;
      }

      // We must do another lookup.
      // Start by freeing 'response' if we need to.
      if (response != in_response)
      {
         res_free(response);
      }
      response = NULL;
      // Now, 'response' will be from a query for 'name'.
      response_for_this_name = TRUE;
      // Debugging print.
      if (SipSrvLookup::getOption(SipSrvLookup::OptionCodePrintAnswers))
      {
         printf("res_query(\"%s\", class = %d, type = %d)\n",
                name, C_IN, type);
      }
      // Use res_query, not res_search, so defaulting rules are not
      // applied to the domain.
      if (res_query(name, C_IN, type,
                    (unsigned char*) answer, sizeof (answer)) == -1)
      {
         // res_query failed, return.
         break;
      }
      response = res_parse((char*) &answer);
      if (response == NULL)
      {
         // res_parse failed, return.
         break;
      }
      // If requested for testing purposes, sort the query and print it.
      // Sort first, so we see how sorting came out.
      if (SipSrvLookup::getOption(SipSrvLookup::OptionCodeSortAnswers))
      {
         sort_answers(response);
      }
      if (SipSrvLookup::getOption(SipSrvLookup::OptionCodePrintAnswers))
      {
         res_print(response);
      }
      // Now that we have a fresh DNS query to analyze, go back and check it
      // for a CNAME for 'name' and then for records of the requested type.
   }   

   // Final processing:  Copy the working name and response to the output
   // variables.
   out_name = name;
   out_response = response;
}

union u_rdata* look_for(res_response* response, const char* name,
                        int type)
{
    unsigned i;

   for (i = 0; i < response->header.ancount; i++)
   {
      if (response->answer[i]->rclass == C_IN &&
          response->answer[i]->type == type &&
          strcasecmp(name, response->answer[i]->name) == 0)
      {
         return &response->answer[i]->rdata;
      }
   }
   for (i = 0; i < response->header.arcount; i++)
   {
      if (response->additional[i]->rclass == C_IN &&
          response->additional[i]->type == type &&
          strcasecmp(name, response->additional[i]->name) == 0)
      {
         return &response->additional[i]->rdata;
      }
   }
   return NULL;
}

// Function to compare two server entries.
int server_compare(const void* a, const void* b)
{
    int result = 0;
    const server_t* s1 = (const server_t*) a;
    const server_t* s2 = (const server_t*) b;

    /* First compare priorities.  Lower priority values are preferred, and
     * should go at the beginning of the list, and so should be returned
     * as less-than.
     */
    if (s1->priority > s2->priority)
    {
        result = 1;
    }
    else if (s1->priority < s2->priority)
    {
        result = -1;
    }
    // Next compare the scores derived from the weights.
    // With the new scheme for computing scores, lower score values should
    // sort to the beginning of the list, that is, should compare less thn
    // higher scores.
    // See sipXtackLib/doc/developer/scores/README for details.
    else if (s1->score < s2->score)
    {
        result = -1;
    }
    else if (s1->score > s2->score)
    {
        result = 1;
    }
    // Compare the transport type, so UDP is favored over TCP.
    // That means that TCP must be larger than UDP.
    else if (s1->type == OsSocket::TCP && s2->type != OsSocket::TCP)
    {
        result = 1;
    }
    else if (s1->type != OsSocket::TCP && s2->type == OsSocket::TCP)
    {
        result = -1;
    }

    return result;
}

/* //////////////////////////// server_t ///////////////////////////////// */

/// Initializer for server_t
server_t::server_t() :
   host(NULL)
{
}

// Copy constructor for server_t
server_t::server_t(const server_t& rserver_t) :
   host(rserver_t.host != NULL ? strdup(rserver_t.host) : NULL),
   type(rserver_t.type),
   sin(rserver_t.sin),
   priority(rserver_t.priority),
   weight(rserver_t.weight),
   score(rserver_t.score)
{
}

// Assignment operator for server_t
server_t& server_t::operator=(const server_t& rhs)
{
   // Handle the assignment-to-self case.
   if (this == &rhs)
   {
      return *this;
   }

   // Copy the host strign, if present.
   host = rhs.host != NULL ? strdup(rhs.host) : NULL;
   // Copy the other fields.
   type = rhs.type;
   sin = rhs.sin;
   priority = rhs.priority;
   weight = rhs.weight;
   score = rhs.score;

   return *this;
}

/// Destructor for server_t
server_t::~server_t()
{
   // All that needs to be done is free the host string, if any.
   if (host != NULL)
   {
      free(host);
   }
}

/// Inquire if this is a valid SRV record
UtlBoolean server_t::isValidServerT()
{
   // Entry is valid if host is not NULL.
   return host != NULL;
}

/// Accessor for host name
void server_t::getHostNameFromServerT(UtlString& hostName)
{
   hostName = (host != NULL) ? host : "";
}

/// Accessor for host IP address
void server_t::getIpAddressFromServerT(UtlString& hostName)
{
   OsSocket::inet_ntoa_pt(sin.sin_addr, hostName);
}

/// Accessor for port
int server_t::getPortFromServerT()
{
   return ntohs(sin.sin_port);
}

/// Accessor for weight
unsigned int server_t::getWeightFromServerT()
{
   return weight;
}

/// Accessor for score
float server_t::getScoreFromServerT()
{
   return score;
}

/// Accessor for priority
unsigned int server_t::getPriorityFromServerT()
{
   return priority;
}

/// Accessor for protocol
OsSocket::IpProtocolSocketType server_t::getProtocolFromServerT()
{
   return type;
}

/**
 * Post-process the results of res_parse by sorting the lists of "answer" and
 * "additional" RRs, so that responses are reproducible.  (named tends to
 * rotate multiple answer RRs to the same query.)
 */
static void sort_answers(res_response* response)
{
   qsort((void*) response->answer, response->header.ancount,
         sizeof (s_rr*), rr_compare);
   qsort((void*) response->additional, response->header.arcount,
         sizeof (s_rr*), rr_compare);
}

/**
 * Function to compare two RRs for qsort.
 *
 * I was hoping to sort records by TTL values, but Bind cleverly gives all
 * answers the same TTL (the minimum of the lot).  So we have to sort by
 * address (for A records) or port/target (for SRV records).
 */
static int rr_compare(const void* a, const void* b)
{
   int t;

   // a and b are pointers to entries in the array of s_rr*'s.
   // Get the pointers to the s_rr's:
   s_rr* a_rr = *(s_rr**) a;
   s_rr* b_rr = *(s_rr**) b;

   // Compare on type.
   t = a_rr->type - b_rr->type;
   if (t != 0)
   {
      return t;
   }

   // Case on type.
   switch (a_rr->type)
   {
   case T_SRV:
      // Compare on target.
      t = strcmp(a_rr->rdata.srv.target, b_rr->rdata.srv.target);
      if (t != 0)
      {
         return t;
      }
      // Compare on port.
      if (a_rr->rdata.srv.port < b_rr->rdata.srv.port)
      {
         return -1;
      }
      else if (a_rr->rdata.srv.port > b_rr->rdata.srv.port)
      {
         return 1;
      }
      // Give up.
      return 0;

   case T_A:
      // Compare on address.
      return memcmp((const void*) &a_rr->rdata.address,
                    (const void*) &b_rr->rdata.address,
                    sizeof (struct sockaddr));

   default:
      return 0;
   }
}
