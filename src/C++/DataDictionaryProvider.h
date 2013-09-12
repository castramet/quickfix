/* -*- C++ -*- */

/****************************************************************************
** Copyright (c) quickfixengine.org  All rights reserved.
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
**
****************************************************************************/

#ifndef FIX_DATADICTIONARYPROVIDER_H
#define FIX_DATADICTIONARYPROVIDER_H

#ifdef _MSC_VER
#pragma warning( disable : 4503 4355 4786 4290 )
#endif

#include "DataDictionary.h"
#include "Exceptions.h"

namespace FIX
{
class BeginString;
class ApplVerID;

/**
* Queries for DataDictionary based on appropriate version of %FIX.
*/

class DataDictionaryProvider
{
public:
  DataDictionaryProvider() {}
  DataDictionaryProvider( const DataDictionaryProvider& copy );

  const DataDictionary& getSessionDataDictionary(const BeginString& beginString)
  throw( DataDictionaryNotFound );

  const DataDictionary& getApplicationDataDictionary(const ApplVerID& applVerID)
  throw( DataDictionaryNotFound );

  void addTransportDataDictionary(const BeginString& beginString, const DataDictionary*);
  void addApplicationDataDictionary(const ApplVerID applVerID, const DataDictionary*);

private:
#ifdef HAVE_BOOST
  typedef boost::unordered_map<std::string, const DataDictionary*, ItemHash, Util::String::equal_to> dictionary_map_t;
#else
  typedef std::map<std::string, const DataDictionary*> dictionary_map_t;
#endif

  dictionary_map_t m_transportDictionaries;
  dictionary_map_t m_applicationDictionaries;
  DataDictionary emptyDataDictionary;
};
}

#endif //FIX_DATADICTIONARYPROVIDER_H

