/* Copyright (c) 2011 maidsafe.net limited
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
#include <bitset>
#include <memory>

#include "gtest/gtest.h"
#include "boost/lexical_cast.hpp"
#include "boost/thread/mutex.hpp"
#include "boost/thread.hpp"
#include "boost/asio/io_service.hpp"
#include "boost/enable_shared_from_this.hpp"

#include "maidsafe-dht/transport/tcp_transport.h"
#include "maidsafe/common/crypto.h"
#include "maidsafe/common/utils.h"
#include "maidsafe-dht/kademlia/config.h"
#include "maidsafe-dht/kademlia/rpcs.pb.h"
#include "maidsafe-dht/kademlia/securifier.h"
#include "maidsafe-dht/kademlia/utils.h"
#include "maidsafe-dht/kademlia/service.h"
#include "maidsafe-dht/kademlia/rpcs.h"
#include "maidsafe-dht/kademlia/node_id.h"
#include "maidsafe-dht/kademlia/routing_table.h"
#include "maidsafe-dht/kademlia/message_handler.h"
#include "maidsafe-dht/kademlia/alternative_store.h"

namespace maidsafe {

namespace kademlia {

namespace test {

static const boost::uint16_t k = 16;

void TestPingCallback(RankInfoPtr,
                      int callback_code,
                      bool *done,
                      int *response_code) {
  *done = true;
  *response_code = callback_code;
}

void TestFindNodesCallback(RankInfoPtr,
                           int callback_code,
                           std::vector<Contact> contacts,
                           std::vector<Contact> *contact_list,
                           bool *done,
                           int *response_code) {
  *done = true;
  *response_code = callback_code;
  *contact_list = contacts;
}

void TestStoreCallback(RankInfoPtr,
                       int callback_code,
                       bool *done,
                       int *response_code) {
  *done = true;
  *response_code = callback_code;
}

void TestFindValueCallback(RankInfoPtr,
                           int callback_code,
                           std::vector<std::string> values,
                           std::vector<Contact> contacts,
                           Contact alternative_value_holder,
                           std::vector<std::string> *return_values,
                           std::vector<Contact> *return_contacts,
                           bool *done,
                           int *response_code) {
  *done = true;
  *response_code = callback_code;
  *return_values = values;
  *return_contacts = contacts;
}

class AlternativeStoreTrue: public AlternativeStore {
 public:
  virtual ~AlternativeStoreTrue() {}
  virtual bool Has(const std::string&) {
    return true;
  }
};

typedef std::shared_ptr<AlternativeStoreTrue> AlternativeStoreTruePtr;

class CreateContactAndNodeId {
 public:
  CreateContactAndNodeId() : contact_(), node_id_(NodeId::kRandomId),
                   routing_table_(new RoutingTable(node_id_, test::k)) {}

  NodeId GenerateUniqueRandomId(const NodeId& holder, const int& pos) {
    std::string holder_id = holder.ToStringEncoded(NodeId::kBinary);
    std::bitset<kKeySizeBits> holder_id_binary_bitset(holder_id);
    NodeId new_node;
    std::string new_node_string;
    bool repeat(true);
    boost::uint16_t times_of_try(0);
    // generate a random ID and make sure it has not been generated previously
    do {
      new_node = NodeId(NodeId::kRandomId);
      std::string new_id = new_node.ToStringEncoded(NodeId::kBinary);
      std::bitset<kKeySizeBits> binary_bitset(new_id);
      for (int i = kKeySizeBits - 1; i >= pos; --i)
        binary_bitset[i] = holder_id_binary_bitset[i];
      binary_bitset[pos].flip();
      new_node_string = binary_bitset.to_string();
      new_node = NodeId(new_node_string, NodeId::kBinary);
      // make sure the new contact not already existed in the routing table
      Contact result;
      routing_table_->GetContact(new_node, &result);
      if (result == Contact())
        repeat = false;
      ++times_of_try;
    } while (repeat && (times_of_try < 1000));
    // prevent deadlock, throw out an error message in case of deadlock
    if (times_of_try == 1000)
      EXPECT_LT(1000, times_of_try);
    return new_node;
  }

  Contact GenerateUniqueContact(const NodeId& holder, const int& pos,
                                RoutingTableContactsContainer& gnerated_nodes,
                                NodeId target) {
    std::string holder_id = holder.ToStringEncoded(NodeId::kBinary);
    std::bitset<kKeySizeBits> holder_id_binary_bitset(holder_id);
    NodeId new_node;
    std::string new_node_string;
    bool repeat(true);
    boost::uint16_t times_of_try(0);
    Contact new_contact;
    // generate a random contact and make sure it has not been generated
    // within the previously record
    do {
      new_node = NodeId(NodeId::kRandomId);
      std::string new_id = new_node.ToStringEncoded(NodeId::kBinary);
      std::bitset<kKeySizeBits> binary_bitset(new_id);
      for (int i = kKeySizeBits - 1; i >= pos; --i)
        binary_bitset[i] = holder_id_binary_bitset[i];
      binary_bitset[pos].flip();
      new_node_string = binary_bitset.to_string();
      new_node = NodeId(new_node_string, NodeId::kBinary);

      // make sure the new one hasn't been set as down previously
      ContactsById key_indx = gnerated_nodes.get<NodeIdTag>();
      auto it = key_indx.find(new_node);
      if (it == key_indx.end()) {
        new_contact = ComposeContact(new_node, 5000);
        RoutingTableContact new_routing_table_contact(new_contact,
                                                      target,
                                                      0);
        gnerated_nodes.insert(new_routing_table_contact);
        repeat = false;
      }
      ++times_of_try;
    } while (repeat && (times_of_try < 1000));
    // prevent deadlock, throw out an error message in case of deadlock
    if (times_of_try == 1000)
      EXPECT_LT(1000, times_of_try);
    return new_contact;
  }

  NodeId GenerateRandomId(const NodeId& holder, const int& pos) {
    std::string holder_id = holder.ToStringEncoded(NodeId::kBinary);
    std::bitset<kKeySizeBits> holder_id_binary_bitset(holder_id);
    NodeId new_node;
    std::string new_node_string;

    new_node = NodeId(NodeId::kRandomId);
    std::string new_id = new_node.ToStringEncoded(NodeId::kBinary);
    std::bitset<kKeySizeBits> binary_bitset(new_id);
    for (int i = kKeySizeBits - 1; i >= pos; --i)
      binary_bitset[i] = holder_id_binary_bitset[i];
    binary_bitset[pos].flip();
    new_node_string = binary_bitset.to_string();
    new_node = NodeId(new_node_string, NodeId::kBinary);

    return new_node;
  }

  Contact ComposeContact(const NodeId& node_id,
                         boost::uint16_t port) {
    std::string ip("127.0.0.1");
    std::vector<transport::Endpoint> local_endpoints;
    transport::Endpoint end_point(ip, port);
    local_endpoints.push_back(end_point);
    Contact contact(node_id, end_point, local_endpoints, end_point, false,
                    false, "", "", "");
    return contact;
  }

  Contact ComposeContactWithKey(const NodeId& node_id,
                                boost::uint16_t port,
                                const crypto::RsaKeyPair& crypto_key) {
    std::string ip("127.0.0.1");
    std::vector<transport::Endpoint> local_endpoints;
    transport::Endpoint end_point(ip, port);
    local_endpoints.push_back(end_point);
    Contact contact(node_id, end_point, local_endpoints, end_point, false,
                    false, node_id.String(), crypto_key.public_key(), "");
    IP ipa = IP::from_string(ip);
    contact.SetPreferredEndpoint(ipa);
    return contact;
  }

  void PopulateContactsVector(int count,
                              const int& pos,
                              std::vector<Contact> *contacts) {
    for (int i = 0; i < count; ++i) {
      NodeId contact_id = GenerateRandomId(node_id_, pos);
      Contact contact = ComposeContact(contact_id, 5000);
      contacts->push_back(contact);
    }
  }

  Contact contact_;
  kademlia::NodeId node_id_;
  std::shared_ptr<RoutingTable> routing_table_;
};


class RpcsTest: public CreateContactAndNodeId, public testing::Test {
 public:
  RpcsTest() : node_id_(NodeId::kRandomId),
               routing_table_(new RoutingTable(node_id_, test::k)),
               data_store_(new kademlia::DataStore(bptime::seconds(3600))),
               alternative_store_(),
               asio_service_(new boost::asio::io_service()),
               local_asio_(new boost::asio::io_service()),
               rank_info_(),
               contacts_(),
               transport_() { }

  static void SetUpTestCase() {
    sender_crypto_key_id_.GenerateKeys(4096);
    receiver_crypto_key_id_.GenerateKeys(4096);
  }

  virtual void SetUp() {
    // rpcs setup
    rpcs_securifier_ = std::shared_ptr<Securifier>(
        new Securifier("", sender_crypto_key_id_.public_key(),
                        sender_crypto_key_id_.private_key()));
    rpcs_= std::shared_ptr<Rpcs>(new Rpcs(asio_service_, rpcs_securifier_));
    NodeId rpcs_node_id = GenerateRandomId(node_id_, 502);
    rpcs_contact_ = ComposeContactWithKey(rpcs_node_id,
                                          5010,
                                          sender_crypto_key_id_);
    rpcs_->set_contact(rpcs_contact_);
    // service setup
    service_securifier_ = std::shared_ptr<Securifier>(
        new Securifier("", receiver_crypto_key_id_.public_key(),
                       receiver_crypto_key_id_.private_key()));
    NodeId service_node_id = GenerateRandomId(node_id_, 503);
    service_contact_ = ComposeContactWithKey(service_node_id,
                                             5011,
                                             receiver_crypto_key_id_);
    service_ = std::shared_ptr<Service>(new Service(routing_table_,
                                                    data_store_,
                                                    alternative_store_,
                                                    service_securifier_,
                                                    k));
    service_->set_node_contact(service_contact_);
    service_->set_node_joined(true);
    transport_.reset(new transport::TcpTransport(*local_asio_));
    handler_ = std::shared_ptr<MessageHandler>(
        new MessageHandler(service_securifier_));
    service_->ConnectToSignals(transport_, handler_);
    transport_->StartListening(service_contact_.endpoint());
  }
  virtual void TearDown() { }

  void ListenPort() {
    local_asio_->run();
  }

  void PopulateRoutingTable(boost::uint16_t count) {
    for (int num_contact = 0; num_contact < count; ++num_contact) {
      NodeId contact_id(NodeId::kRandomId);
      Contact contact = ComposeContact(contact_id, 5000);
      AddContact(contact, rank_info_);
      contacts_.push_back(contact);
    }
  }

  void AddContact(const Contact& contact, const RankInfoPtr rank_info) {
    routing_table_->AddContact(contact, rank_info);
    routing_table_->SetValidated(contact.node_id(), true);
  }

  KeyValueSignature MakeKVS(const crypto::RsaKeyPair &rsa_key_pair,
                            const size_t &value_size,
                            std::string key,
                            std::string value) {
    if (key.empty())
      key = crypto::Hash<crypto::SHA512>(RandomString(1024));
    if (value.empty()) {
      value.reserve(value_size);
      std::string temp = RandomString((value_size > 1024) ? 1024 : value_size);
      while (value.size() < value_size)
        value += temp;
      value = value.substr(0, value_size);
    }
    std::string signature = crypto::AsymSign(value, rsa_key_pair.private_key());
    return KeyValueSignature(key, value, signature);
  }

  protobuf::StoreRequest MakeStoreRequest(const Contact& sender,
                            const KeyValueSignature& kvs,
                            const crypto::RsaKeyPair& crypto_key_data) {
    protobuf::StoreRequest store_request;
    store_request.mutable_sender()->CopyFrom(ToProtobuf(sender));
    store_request.set_key(kvs.key);
    store_request.mutable_signed_value()->set_signature(kvs.signature);
    store_request.mutable_signed_value()->set_value(kvs.value);
    store_request.set_ttl(3600*24);
    store_request.set_signing_public_key_id(
        crypto::Hash<crypto::SHA512>(crypto_key_data.public_key() +
            crypto::AsymSign(crypto_key_data.public_key(),
                            crypto_key_data.private_key())));
    return store_request;
  }

  boost::uint16_t KDistanceTo(const NodeId &lhs, const NodeId &rhs) {
    boost::uint16_t distance = 0;
    std::string this_id_binary = lhs.ToStringEncoded(NodeId::kBinary);
    std::string rhs_id_binary = rhs.ToStringEncoded(NodeId::kBinary);
    std::string::const_iterator this_it = this_id_binary.begin();
    std::string::const_iterator rhs_it = rhs_id_binary.begin();
    for (; ((this_it != this_id_binary.end()) && (*this_it == *rhs_it));
        ++this_it, ++rhs_it)
      ++distance;
    return distance;
  }

  int GetDistance(const std::vector<Contact> &list, int test) {
    int low(0), high(0);
    boost::uint16_t distance = KDistanceTo(service_contact_.node_id(),
                                           list[0].node_id());
    low = distance;
    auto it = list.begin();
    while (it != list.end()) {
      distance = KDistanceTo(service_contact_.node_id(), (*it).node_id());
      if (distance > high)
        high = distance;
      else if (distance < low)
        low = distance;
      ++it;
    }
    if (test > 0)
      return high;
    else
      return low;
  }

 protected:
  kademlia::NodeId  node_id_;
  std::shared_ptr<RoutingTable> routing_table_;
  std::shared_ptr<DataStore> data_store_;
  AlternativeStorePtr alternative_store_;
  SecurifierPtr service_securifier_;
  std::shared_ptr<Service> service_;
  SecurifierPtr rpcs_securifier_;
  IoServicePtr asio_service_;
  IoServicePtr local_asio_;
  std::shared_ptr<Rpcs> rpcs_;
  Contact rpcs_contact_;
  Contact service_contact_;
  static crypto::RsaKeyPair sender_crypto_key_id_;
  static crypto::RsaKeyPair receiver_crypto_key_id_;
  RankInfoPtr rank_info_;
  std::vector<Contact> contacts_;
  TransportPtr transport_;
  MessageHandlerPtr handler_;
};

crypto::RsaKeyPair RpcsTest::sender_crypto_key_id_;
crypto::RsaKeyPair RpcsTest::receiver_crypto_key_id_;

TEST_F(RpcsTest, BEH_KAD_PingNoTarget) {
  bool done(false);
  int response_code(0);

  rpcs_->Ping(rpcs_securifier_, rpcs_contact_,
              boost::bind(&TestPingCallback, _1, _2, &done, &response_code),
              kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  asio_service_->stop();

  ASSERT_GT(0, response_code);
}

TEST_F(RpcsTest, BEH_KAD_PingTarget) {
  bool done(false);
  int response_code(0);
  boost::thread th(boost::bind(&RpcsTest::ListenPort, this));

  rpcs_->Ping(rpcs_securifier_, service_contact_,
              boost::bind(&TestPingCallback, _1, _2, &done, &response_code),
              kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  asio_service_->stop();
  local_asio_->stop();
  th.join();

  ASSERT_EQ(0, response_code);
}

TEST_F(RpcsTest, BEH_KAD_FindNodesEmptyRT) {
  // tests FindNodes using empty routing table
  bool done(false);
  int response_code(0);
  std::vector<Contact> contact_list;
  Key key = service_contact_.node_id();
  boost::thread th(boost::bind(&RpcsTest::ListenPort, this));

  rpcs_->FindNodes(key, rpcs_securifier_, service_contact_,
                   boost::bind(&TestFindNodesCallback, _1, _2, _3,
                               &contact_list, &done, &response_code),
                   kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  asio_service_->stop();
  local_asio_->stop();
  th.join();

  ASSERT_EQ(0, contact_list.size());
  ASSERT_EQ(0, response_code);
}

TEST_F(RpcsTest, BEH_KAD_FindNodesPopulatedRTnoNode) {
  // tests FindNodes with a populated routing table not containing the node
  // being sought
  bool done(false);
  int response_code(0);
  std::vector<Contact> contact_list;
  PopulateRoutingTable(2*k);
  Key key = service_contact_.node_id();
  boost::thread th(boost::bind(&RpcsTest::ListenPort, this));

  rpcs_->FindNodes(key, rpcs_securifier_, service_contact_,
                   boost::bind(&TestFindNodesCallback, _1, _2, _3,
                               &contact_list, &done, &response_code),
                   kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  asio_service_->stop();
  local_asio_->stop();
  th.join();

  bool found(false);
  std::sort(contact_list.begin(), contact_list.end());
  auto it = contact_list.begin();
  while (it != contact_list.end()) {
    if ((*it).node_id() == service_contact_.node_id())
      found = true;
    for (int i = 0; i < contacts_.size(); i++) {
      if ((*it).node_id() == contacts_[i].node_id())
        contacts_.erase(contacts_.begin()+i);
      }
    ++it;
  }

  ASSERT_FALSE(found);
  ASSERT_GE(GetDistance(contact_list, 0), GetDistance(contacts_, 1));
  ASSERT_EQ(k, contact_list.size());
  ASSERT_EQ(0, response_code);
}

TEST_F(RpcsTest, BEH_KAD_FindNodesPopulatedRTwithNode) {
  // tests FindNodes with a populated routing table which contains the node
  // being sought
  bool done(false);
  int response_code(0);
  PopulateRoutingTable(2*k);
  std::vector<Contact> contact_list;
  AddContact(service_contact_, rank_info_);
  Key key = service_contact_.node_id();
  boost::thread th(boost::bind(&RpcsTest::ListenPort, this));

  rpcs_->FindNodes(key, rpcs_securifier_, service_contact_,
                   boost::bind(&TestFindNodesCallback, _1, _2, _3,
                               &contact_list, &done, &response_code),
                   kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  asio_service_->stop();
  local_asio_->stop();
  th.join();

  bool found(false);
  auto it = contact_list.begin();
  while (it != contact_list.end()) {
    if ((*it).node_id() == service_contact_.node_id())
      found = true;
    for (int i = 0; i < contacts_.size(); i++) {
      if ((*it).node_id() == contacts_[i].node_id())
        contacts_.erase(contacts_.begin()+i);
      }
    ++it;
  }

  ASSERT_TRUE(found);
  ASSERT_GE(GetDistance(contact_list, 0), GetDistance(contacts_, 1));
  ASSERT_EQ(k, contact_list.size());
  ASSERT_EQ(0, response_code);
}

TEST_F(RpcsTest, BEH_KAD_StoreAndFindValue) {
  bool done(false);
  int response_code(0);
  PopulateRoutingTable(2*k);
  Key key = rpcs_contact_.node_id();
  boost::thread th(boost::bind(&RpcsTest::ListenPort, this));
  KeyValueSignature kvs = MakeKVS(sender_crypto_key_id_, 1024,
                                  key.String(), "");
  boost::posix_time::seconds ttl(3600);

  // attempt to find value before any stored
  std::vector<std::string> return_values;
  std::vector<Contact> return_contacts;
  done = false;
  response_code = 0;
  rpcs_->FindValue(key, rpcs_securifier_, service_contact_,
                   boost::bind(&TestFindValueCallback, _1, _2, _3, _4, _5,
                               &return_values,
                               &return_contacts,
                               &done, &response_code),
                   kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  ASSERT_EQ(0, response_code);
  ASSERT_EQ(0, return_values.size());
  ASSERT_EQ(k, return_contacts.size());
  asio_service_->reset();

  rpcs_->Store(key, kvs.value, kvs.signature, ttl, rpcs_securifier_,
               service_contact_,
               boost::bind(&TestStoreCallback, _1, _2,
                           &done, &response_code),
               kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  ASSERT_EQ(0, response_code);
  asio_service_->reset();

  // attempt to retrieve value stored
  return_values.clear();
  return_contacts.clear();
  done = false;
  response_code = 0;
  rpcs_->FindValue(key, rpcs_securifier_, service_contact_,
                   boost::bind(&TestFindValueCallback, _1, _2, _3, _4, _5,
                               &return_values,
                               &return_contacts,
                               &done, &response_code),
                   kTcp);
  asio_service_->run();
  while (!done)
    boost::this_thread::sleep(boost::posix_time::milliseconds(100));
  ASSERT_EQ(0, response_code);
  ASSERT_EQ(kvs.value, return_values[0]);
  ASSERT_EQ(0, return_contacts.size());

  asio_service_->stop();
  local_asio_->stop();
  th.join();
}

}  // namespace test

}  // namespace kademlia

}  // namespace maidsafe
