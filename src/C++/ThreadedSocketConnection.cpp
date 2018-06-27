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
#include <poll.h>
#endif

#include "ThreadedSocketConnection.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "Session.h"
#include "Utility.h"

namespace FIX
{
ThreadedSocketConnection::ThreadedSocketConnection
(sys_socket_t s, Sessions sessions, Log* pLog )
: m_socket( s ), m_pLog( pLog ),
  m_sessions( sessions ), m_pSession( 0 ),
  m_disconnect( false ), m_pollspin( 0 )
{}

ThreadedSocketConnection::ThreadedSocketConnection
( const SessionID& sessionID, sys_socket_t s,
  const std::string& address, short port, 
  Log* pLog,
  const std::string& sourceAddress, short sourcePort )
  : m_socket( s ), m_address( address ), m_port( port ),
    m_sourceAddress( sourceAddress ), m_sourcePort( sourcePort ),
    m_pLog( pLog ),
    m_pSession( Session::lookupSession( sessionID ) ),
    m_disconnect( false ), m_pollspin( 0 )
{
  if ( m_pSession ) m_pSession->setResponder( this );
}

ThreadedSocketConnection::~ThreadedSocketConnection()
{
  if ( m_pSession )
  {
    m_pSession->setResponder( 0 );
    Session::unregisterSession( m_pSession->getSessionID() );
  }
}

bool ThreadedSocketConnection::send( const std::string& msg )
{
  return socket_send( m_socket, String::data(msg), String::size(msg) ) > 0;
}

bool ThreadedSocketConnection::send( Sg::sg_buf_ptr bufs, int n )
{
  return Sg::send(m_socket, bufs, n) > 0;
}

bool ThreadedSocketConnection::connect()
{
  // do the bind in the thread as name resolution may block
  if ( !m_sourceAddress.empty() || m_sourcePort )
    socket_bind( m_socket, String::c_str(m_sourceAddress), m_sourcePort );

  return socket_connect(getSocket(), String::c_str(m_address), m_port) >= 0;
}

void ThreadedSocketConnection::disconnect()
{
  m_disconnect = true;
  socket_close( m_socket );
  m_parser.reset();
}

inline bool HEAVYUSE ThreadedSocketConnection::readMessage( Sg::sg_buf_t& msg )
{
  while( true )
  {
    try
    {
      if( m_parser.parse() )
      {
        m_parser.retrieve( msg );
        return true;
      }
      break;
    }
    catch ( MessageParseError& e )
    {
      if( m_pLog )
      {
        m_pLog->onEvent( e.what() );
      }
    }
  }
  return false;
}

void HEAVYUSE HOTSECTION  ThreadedSocketConnection::processStream()
{
  Sg::sg_buf_t buf;
  while( readMessage( buf ) )
  {
    if ( !m_pSession )
    {
      if ( !setSession( buf ) )
      { disconnect(); break; }
    }
    try
    {
      m_ts.setCurrent();
      m_pSession->next( buf, m_ts );
    }
    catch( InvalidMessage& )
    {
      if( !m_pSession->isLoggedOn() )
      {
        disconnect();
        return;
      }
    }
  }
}

bool HEAVYUSE HOTSECTION ThreadedSocketConnection::read()
{
  try
  {
    int result, timeout, busy = m_pollspin;
    // Wait for input (1 second timeout)
    do {
      timeout = (busy-- > 0) ? 0 : 1000;
#ifndef _MSC_VER
      struct pollfd pfd = { m_socket, POLLIN | POLLPRI, 0 };
      result = poll(&pfd, 1, timeout);
#else
      struct pollfd pfd = { (SOCKET)m_socket, POLLIN, 0 };
      result = WSAPoll(&pfd, 1, timeout);
#endif
    } while( result == 0 && timeout == 0 );

    if( result > 0 ) // Something to read
    {
      // We can read without blocking
      Sg::sg_buf_t buf = m_parser.buffer();
      ssize_t size = recv( m_socket, IOV_BUF(buf), IOV_LEN(buf), 0 );
      if ( LIKELY(size > 0) )
      {
        m_parser.advance( size );
        processStream();
      }
      else
        throw SocketRecvFailed( size );
    }
    else if( result == 0 && m_pSession ) // Timeout
    {
      m_pSession->next();
    }
    else if( result < 0 ) // Error
    {
      throw SocketRecvFailed( result );
    }
    return true;
  }
  catch ( SocketRecvFailed& e )
  {
    if( m_disconnect )
      return false;

    if( m_pSession )
    {
      m_pSession->getLog()->onEvent( e.what() );
      m_pSession->disconnect();
    }
    else
    {
      disconnect();
    }

    return false;
  }
}

bool ThreadedSocketConnection::setSession( Sg::sg_buf_t& msg )
{
  m_pSession = Session::lookupSession( msg, true );
  if ( !m_pSession ) 
  {
    if( m_pLog )
    {
      m_pLog->onEvent( "Session not found for incoming message: " + Sg::toString(msg) );
      m_pLog->onIncoming( &msg, 1 );
    }
    return false;
  }

  SessionID sessionID = m_pSession->getSessionID();
  m_pSession = 0;

  // see if the session frees up within 5 seconds
  for( int i = 1; i <= 5; i++ )
  {
    if( !Session::isSessionRegistered( sessionID ) )
      m_pSession = Session::registerSession( sessionID );
    if( m_pSession ) break;
    process_sleep( 1 );
  }

  if ( !m_pSession ) 
    return false;
  if ( m_sessions.find(m_pSession->getSessionID()) == m_sessions.end() )
    return false;

  m_pSession->setResponder( this );
  m_pollspin = m_pSession->getPollSpin();
  return true;
}

} // namespace FIX
