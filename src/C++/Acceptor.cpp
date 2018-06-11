/****************************************************************************
** Copyright (c) 2001-2014
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

#ifdef _MSC_VER
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "Acceptor.h"
#include "Parser.h"
#include "Session.h"
#include "SessionFactory.h"
#include "HttpServer.h"
#include <algorithm>
#include <fstream>

namespace FIX
{
COLDSECTION Acceptor::Acceptor( Application& application,
                    MessageStoreFactory& messageStoreFactory,
                    const SessionSettings& settings )
THROW_DECL( ConfigError )
: m_threadid( 0 ),
  m_application( application ),
  m_messageStoreFactory( messageStoreFactory ),
  m_settings( settings ),
  m_pLogFactory( 0 ),
  m_pLog( 0 ),
  m_firstPoll( true ),
  m_stop( true )
{
  initialize();
}

COLDSECTION Acceptor::Acceptor( Application& application,
                    MessageStoreFactory& messageStoreFactory,
                    const SessionSettings& settings,
                    LogFactory& logFactory )
THROW_DECL( ConfigError )
: m_threadid( 0 ),
  m_application( application ),
  m_messageStoreFactory( messageStoreFactory ),
  m_settings( settings ),
  m_pLogFactory( &logFactory ),
  m_pLog( logFactory.create() ),
  m_firstPoll( true ),
  m_stop( true )
{
  initialize();
}

COLDSECTION void Acceptor::initialize() THROW_DECL( ConfigError )
{
  std::set < SessionID > sessions = m_settings.getSessions();
  std::set < SessionID > ::iterator i;

  if ( !sessions.size() )
    throw ConfigError( "No sessions defined" );

  SessionFactory factory( m_application, m_messageStoreFactory,
                          m_pLogFactory );

  for ( i = sessions.begin(); i != sessions.end(); ++i )
  {
    if ( m_settings.get( *i ).getString( CONNECTION_TYPE ) == "acceptor" )
    {
      m_sessionIDs.insert( *i );
      m_sessions[ *i ] = factory.create( *i, m_settings.get( *i ) );
    }
  }

  if ( !m_sessions.size() )
    throw ConfigError( "No sessions defined for acceptor" );
}

COLDSECTION Acceptor::~Acceptor()
{
  Sessions::iterator i;
  for ( i = m_sessions.begin(); i != m_sessions.end(); ++i )
    delete i->second;

  if( m_pLogFactory && m_pLog )
    m_pLogFactory->destroy( m_pLog );
}

Session* Acceptor::getSession
( const std::string& msg, Responder& responder )
{
  Message message;
  if ( !message.setStringHeader( msg ) )
    return 0;

  BeginString beginString;
  SenderCompID clSenderCompID;
  TargetCompID clTargetCompID;
  MsgType msgType;
  try
  {
    message.getHeader().getField( beginString );
    message.getHeader().getField( clSenderCompID );
    message.getHeader().getField( clTargetCompID );
    message.getHeader().getField( msgType );
    if ( msgType != "A" ) return 0;

    SenderCompID senderCompID( clTargetCompID );
    TargetCompID targetCompID( clSenderCompID );
    SessionID sessionID( beginString, senderCompID, targetCompID );

    Sessions::iterator i = m_sessions.find( sessionID );
    if ( i != m_sessions.end() )
    {
      i->second->setResponder( &responder );
      return i->second;
    }
  }
  catch ( FieldNotFound& ) {}
  return 0;
}

Session* Acceptor::getSession
( Sg::sg_buf_t msg, Responder& responder )
{
  Sg::sg_buf_t begin = Parser::findBeginString( msg );
  if( LIKELY(NULL != Sg::data<const char*>(begin)) )
  {
    Sg::split( msg, begin );
    Sg::sg_buf_t type = Parser::findMsgType( msg );
    const char* p = Sg::data<const char*>(type);
    if ( LIKELY(NULL != p) &&
         LIKELY(*p == 'A') && LIKELY(Sg::size(type) == 1) )
    {
      Sg::split( msg, type );
      Sg::sg_buf_t sender = Parser::findSenderCompID( msg );
      if( LIKELY(NULL != Sg::data<const char*>(sender)) )
      {
        Sg::sg_buf_t target = Parser::findTargetCompID( msg );
        if( LIKELY(NULL != Sg::data<const char*>(target)) )
        {
          Sessions::iterator i = m_sessions.find( SessionID( begin,
                                                     target, sender ) );
          if ( i != m_sessions.end() )
          {
            i->second->setResponder( &responder );
            return i->second;
          }
        }
      }
    }
  }
  return NULL;
}

Session* Acceptor::getSession( const SessionID& sessionID ) const
{
  Sessions::const_iterator i = m_sessions.find( sessionID );
  if( i != m_sessions.end() )
    return i->second;
  else
    return 0;
}

const Dictionary* Acceptor::getSessionSettings( const SessionID& sessionID ) const
{
  try
  {
    return &m_settings.get( sessionID );
  }
  catch( ConfigError& )
  {
    return 0;
  }
}

void Acceptor::start() THROW_DECL( ConfigError, RuntimeError )
{
  m_stop = false;
  onConfigure( m_settings );
  onInitialize( m_settings );

  HttpServer::startGlobal( m_settings );

  if( !thread_spawn( &startThread, this, m_threadid ) )
    throw RuntimeError("Unable to spawn thread");
}

void Acceptor::block() THROW_DECL( ConfigError, RuntimeError )
{
  m_stop = false;
  onConfigure( m_settings );
  onInitialize( m_settings );

  startThread(this);
}

bool Acceptor::poll( double timeout ) THROW_DECL( ConfigError, RuntimeError )
{
  if( m_firstPoll )
  {
    m_stop = false;
    onConfigure( m_settings );
    onInitialize( m_settings );
    m_firstPoll = false;
  }

  return onPoll( timeout );
}

void Acceptor::stop( bool force )
{
  if( isStopped() ) return;

  HttpServer::stopGlobal();

  std::vector<Session*> enabledSessions;

  Sessions sessions = m_sessions;
  Sessions::iterator i = sessions.begin();
  for ( ; i != sessions.end(); ++i )
  {
    Session* pSession = Session::lookupSession(i->first);
    if( pSession && pSession->isEnabled() )
    {
      enabledSessions.push_back( pSession );
      pSession->logout();
      Session::unregisterSession( pSession->getSessionID() );
    }
  }

  if( !force )
  {
    for ( int second = 1; second <= 10 && isLoggedOn(); ++second )
      process_sleep( 1 );
  }

  m_stop = true;
  onStop();
  if( m_threadid )
    thread_join( m_threadid );
  m_threadid = 0;

  std::vector<Session*>::iterator session = enabledSessions.begin();
  for( ; session != enabledSessions.end(); ++session )
    (*session)->logon();
}

bool Acceptor::isLoggedOn()
{
  Sessions sessions = m_sessions;
  Sessions::iterator i = sessions.begin();
  for ( ; i != sessions.end(); ++i )
  {
    if( i->second->isLoggedOn() )
      return true;
  }
  return false;
}

THREAD_PROC Acceptor::startThread( void* p )
{
  Acceptor * pAcceptor = static_cast < Acceptor* > ( p );
  pAcceptor->onStart();
  return 0;
}
}
