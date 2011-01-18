/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <maidsafe/transport/tcptransport.h>
#include <maidsafe/base/log.h>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <google/protobuf/descriptor.h>

namespace asio = boost::asio;
namespace bs = boost::system;
namespace ip = asio::ip;
namespace pt = boost::posix_time;

namespace transport {

TcpTransport::TcpTransport(
    boost::shared_ptr<boost::asio::io_service> asio_service)
        : Transport(asio_service),
          acceptor_(),
          current_connection_id_(1),
          connections_(),
          mutex_() {}

TcpTransport::~TcpTransport() {
  BOOST_FOREACH(ConnectionMap::value_type const& connection, connections_) {
    asio_service_->post(boost::bind(&TcpConnection::Close, connection.second));
  }
  StopListening();
}

TransportCondition TcpTransport::StartListening(const Endpoint &endpoint) {
  if (listening_port_ != 0)
    return kAlreadyStarted;

  if (endpoint.port == 0)
    return kInvalidAddress;

  ip::tcp::endpoint ep(endpoint.ip, endpoint.port);
  acceptor_.reset(new ip::tcp::acceptor(*asio_service_));

  bs::error_code ec;
  acceptor_->open(ep.protocol(), ec);

  if (ec)
    return kInvalidAddress;

  acceptor_->bind(ep, ec);

  if (ec)
    return kBindError;

  acceptor_->listen(asio::socket_base::max_connections, ec);

  if (ec)
    return kListenError;

  ConnectionPtr new_connection(new TcpConnection(this,
                               boost::asio::ip::tcp::endpoint()));
  listening_port_ = acceptor_->local_endpoint().port();

  // The connection object is kept alive in the acceptor handler until
  // HandleAccept() is called.
  acceptor_->async_accept(new_connection->Socket(),
                          boost::bind(&TcpTransport::HandleAccept, this,
                                      new_connection, _1));
  return kSuccess;
}

void TcpTransport::StopListening() {
  boost::system::error_code ec;
  acceptor_->close(ec);
  listening_port_ = 0;
}

void TcpTransport::HandleAccept(const ConnectionPtr &connection,
                                const bs::error_code &ec) {
  if (listening_port_ == 0)
    return;

  if (!ec) {
    boost::mutex::scoped_lock lock(mutex_);
    ConnectionId connection_id = NextConnectionId();
    connection->SetConnectionId(connection_id);
    connections_.insert(std::make_pair(connection_id, connection));
    connection->StartReceiving();
  }

  ConnectionPtr new_connection(new TcpConnection(this,
                               boost::asio::ip::tcp::endpoint()));

  // The connection object is kept alive in the acceptor handler until
  // HandleAccept() is called.
  acceptor_->async_accept(new_connection->Socket(),
                          boost::bind(&TcpTransport::HandleAccept, this,
                                      new_connection, _1));
}

ConnectionId TcpTransport::NextConnectionId() {
  ConnectionId id = current_connection_id_++;
  if (id == 0) ++id;
  return id;
}

void TcpTransport::Send(const std::string &data,
                        const Endpoint &endpoint,
                        const Timeout &timeout) {
  ip::tcp::endpoint tcp_endpoint(endpoint.ip, endpoint.port);
  ConnectionPtr connection(new TcpConnection(this, tcp_endpoint));

  {
    boost::mutex::scoped_lock lock(mutex_);
    ConnectionId connection_id = NextConnectionId();
    connection->SetConnectionId(connection_id);
    connections_.insert(std::make_pair(connection_id, connection));
  }

  connection->Send(data, timeout, false);
}

void TcpTransport::RemoveConnection(ConnectionId id) {
  boost::mutex::scoped_lock lock(mutex_);
  connections_.erase(id);
}

}  // namespace transport
