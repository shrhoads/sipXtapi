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

#ifndef DISABLE_STREAM_PLAYER // [

#ifndef _StreamBufferDataSource_h_
#define _StreamBufferDataSource_h_

// SYSTEM INCLUDES
// APPLICATION INCLUDES
#include "mp/StreamBufferDataSource.h"
#include "mp/StreamDataSource.h"
#include "os/OsDefs.h"
#include "os/OsStatus.h"

// DEFINES
// MACROS
#ifndef __min
#define __min(a,b) (((a) < (b)) ? (a) : (b))
#endif

// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS
// STRUCTS
// TYPEDEFS
// FORWARD DECLARATIONS
class UtlString;

//:Defines a stream data source built ontop of a UtlString
class StreamBufferDataSource : public StreamDataSource
{
/* //////////////////////////// PUBLIC //////////////////////////////////// */
public:

/* ============================ CREATORS ================================== */
///@name Creators
//@{
   StreamBufferDataSource(UtlString *pBuffer, int iFlags);
     //:Default constructor

   virtual
   ~StreamBufferDataSource();
     //:Destructor

//@}

/* ============================ MANIPULATORS ============================== */
///@name Manipulators
//@{
   virtual OsStatus open() ;
     //:Opens the data source

   virtual OsStatus close() ;
     //:Closes the data source

   virtual OsStatus destroyAndDelete() ;
     //:Destroys and deletes the data source object

   virtual OsStatus read(char *szBuffer, int iLength, int& iLengthRead) ;
     //:Reads iLength bytes of data from the data source and places the
     //:data into the passed szBuffer buffer.
     //
     //!param szBuffer - Buffer to place data
     //!param iLength - Max length to read
     //!param iLengthRead - The actual amount of data read.

   virtual OsStatus peek(char* szBuffer, int iLength, int& iLengthRead) ;
     //:Identical to read, except the stream pointer is not advanced.
     //
     //!param szBuffer - Buffer to place data
     //!param iLength - Max length to read
     //!param iLengthRead - The actual amount of data read.
   
   virtual OsStatus seek(unsigned int iLocation) ;
     //:Moves the stream pointer to the an absolute location.
     //
     //!param iLocation - The desired seek location

//@}

/* ============================ ACCESSORS ================================= */
///@name Accessors
//@{

   virtual OsStatus getLength(int& iLength) ;
     //:Gets the length of the stream (if available)

   virtual OsStatus getPosition(int& iPosition) ;
     //:Gets the current position within the stream.


   virtual OsStatus toString(UtlString& string) ;
     //:Renders a string describing this data source.  
     // This is often used for debugging purposes.
   
//@}

/* ============================ INQUIRY =================================== */
///@name Inquiry
//@{

   

//@}

/* //////////////////////////// PROTECTED ///////////////////////////////// */
protected:

   StreamBufferDataSource& operator=(const StreamBufferDataSource& rhs);
     //:Assignment operator (not supported)

   StreamBufferDataSource(const StreamBufferDataSource& rStreamBufferDataSource);
     //:Copy constructor (not supported)


/* //////////////////////////// PRIVATE /////////////////////////////////// */
private:
   UtlString* mpBuffer ;      // buffer -- the data source
   int        miPosition ;    // the current position within the data source
};

/* ============================ INLINE METHODS ============================ */

#endif  // _StreamBufferDataSource_h_

#endif // DISABLE_STREAM_PLAYER ]
