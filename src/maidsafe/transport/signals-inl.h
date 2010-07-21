/* Copyright (c) 2009 maidsafe.net limited
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

/*******************************************************************************
 * NOTE: This API is unlikely to have any breaking changes applied.  However,  *
 *       it should not be regarded as a final API until this notice is removed.*
 ******************************************************************************/

#ifndef MAIDSAFE_TRANSPORT_SIGNALS_INL_H_
#define MAIDSAFE_TRANSPORT_SIGNALS_INL_H_

#include <boost/signals2/signal.hpp>
#include <maidsafe/maidsafe-dht_config.h>
#include <string>


namespace transport {

enum TransportCondition {
  kSuccess = 0,
  kError = 1,
  kRemoteUnreachable = 2,
  kNoConnection = 3,
  kNoNetwork = 4,
  kInvalidIP = 5,
  kInvalidPort = 6,
  kInvalidData = 7,
  kNoSocket = 8,
  kInvalidAddress = 9,
  kNoRendezvous = 10,
  kBehindFirewall = 11,
  kBindError = 12,
  kConnectError = 13,
  kAlreadyStarted = 14,
  kListenError = 15,
  kThreadResourceError = 16,
  kCloseSocketError = 17,
  kSendUdtFailure = 18,
  kSendTimeout = 19,
  kSendParseFailure = 20,
  kSendSizeFailure = 21,
  kReceiveUdtFailure = 22,
  kReceiveTimeout = 23,
  kReceiveParseFailure = 24,
  kReceiveSizeFailure = 25
};

struct SocketPerformanceStats {
 public:
  virtual ~SocketPerformanceStats() {}
};

typedef bs2::signal<void(const std::string&,
                         const SocketId&,
                         const float&)> SignalMessageReceived;
typedef bs2::signal<void(const transport::RpcMessage&,
                         const SocketId&,
                         const float&)> SignalRpcRequestReceived,
                                        SignalRpcResponseReceived;
typedef bs2::signal<void(const bool&,
                         const IP&,
                         const Port&)> SignalConnectionDown;
typedef bs2::signal<void(const SocketId&,
                         const TransportCondition&)> SignalSend, SignalReceive;
//typedef bs2::signal<void(const TransportMessage&,
//                         const TransportCondition&)> SignalSend;
typedef bs2::signal<void(boost::shared_ptr<SocketPerformanceStats>)>
                            SignalStats;


class Signals {
 public:
  virtual ~Signals() {}
   // CONNECTIONS (method is basically the same as sig.connect().)
  bs2::connection ConnectMessageReceived(
      const SignalMessageReceived::slot_type &message_received_slot) {
    return signal_message_received_.connect(message_received_slot);
  }
  bs2::connection ConnectRpcRequestReceived(
      const SignalRpcRequestReceived::slot_type &rpc_request_received_slot) {
    return signal_rpc_request_received_.connect(rpc_request_received_slot);
  }
  bs2::connection ConnectRpcResponseReceived(
      const SignalRpcResponseReceived::slot_type &rpc_response_received_slot) {
    return signal_rpc_response_received_.connect(rpc_response_received_slot);
  }
  bs2::connection ConnectConnectionDown(
      const SignalConnectionDown::slot_type &connection_down_slot) {
    return signal_connection_down_.connect(connection_down_slot);
  }
  bs2::connection ConnectSend(const SignalSend::slot_type &send_slot) {
    return signal_send_.connect(send_slot);
  }
  bs2::connection ConnectReceive(const SignalReceive::slot_type &receive_slot) {
    return signal_receive_.connect(receive_slot);
  }
  bs2::connection ConnectStats(const SignalStats::slot_type &stats_slot) {
    return signal_stats_.connect(stats_slot);
  }
 protected:
  Signals() : signal_message_received_(),
              signal_rpc_request_received_(),
              signal_rpc_response_received_(),
              signal_connection_down_(),
              signal_send_(),
              signal_receive_(),
              signal_stats_() {}
  SignalMessageReceived signal_message_received_;
  SignalRpcRequestReceived signal_rpc_request_received_;
  SignalRpcResponseReceived signal_rpc_response_received_;
  SignalConnectionDown signal_connection_down_;
  SignalSend signal_send_;
  SignalReceive signal_receive_;
  SignalStats signal_stats_;
 private:
  Signals(const Signals&);
  Signals& operator=(const Signals&);
};
}  // namespace transport
#endif  // MAIDSAFE_TRANSPORT_SIGNALS_INL_H_