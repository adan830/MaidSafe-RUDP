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

#include <algorithm>

#include "maidsafe/dht/log.h"
#include "maidsafe/dht/kademlia/node_impl.h"
#include "maidsafe/dht/kademlia/datastore.h"
#ifdef __MSVC__
#  pragma warning(push)
#  pragma warning(disable: 4127 4244 4267)
#endif
#include "maidsafe/dht/kademlia/kademlia.pb.h"
#ifdef __MSVC__
#  pragma warning(pop)
#endif
#include "maidsafe/dht/kademlia/node_id.h"
#include "maidsafe/dht/kademlia/rpcs.h"
#include "maidsafe/dht/kademlia/routing_table.h"
#include "maidsafe/dht/kademlia/securifier.h"
#include "maidsafe/dht/kademlia/service.h"
#include "maidsafe/dht/kademlia/utils.h"

namespace arg = std::placeholders;

namespace maidsafe {

namespace dht {

namespace kademlia {

// some tools which will be used in the implementation of Node::Impl class

Node::Impl::Impl(AsioService &asio_service,                   // NOLINT (Fraser)
                 TransportPtr listening_transport,
                 MessageHandlerPtr message_handler,
                 SecurifierPtr default_securifier,
                 AlternativeStorePtr alternative_store,
                 bool client_only_node,
                 const uint16_t &k,
                 const uint16_t &alpha,
                 const uint16_t &beta,
                 const boost::posix_time::time_duration &mean_refresh_interval)
    : asio_service_(asio_service),
      listening_transport_(listening_transport),
      message_handler_(message_handler),
      default_securifier_(default_securifier),
      alternative_store_(alternative_store),
      on_online_status_change_(new OnOnlineStatusChangePtr::element_type),
      client_only_node_(client_only_node),
      k_(k),
      threshold_((k_ * 3) / 4),
      kAlpha_(alpha),
      kBeta_(beta),
      kMeanRefreshInterval_(mean_refresh_interval.is_special() ? 3600 :
                            mean_refresh_interval.total_seconds()),
      data_store_(new DataStore(kMeanRefreshInterval_)),
      service_(),
      routing_table_(),
      rpcs_(new Rpcs(asio_service_, default_securifier)),
      contact_(),
      joined_(false),
      refresh_routine_started_(false),
      stopping_(false),
      report_down_contact_(new ReportDownContactPtr::element_type),
      mutex_(),
      condition_downlist_(),
      down_contacts_(),
      thread_group_(),
      refresh_thread_running_(false),
      downlist_thread_running_(false),
      validate_contact_running_(false) {}

Node::Impl::~Impl() {
  if (joined_)
    Leave(NULL);
}

void Node::Impl::Join(const NodeId &node_id,
                      const std::vector<Contact> &bootstrap_contacts,
                      JoinFunctor callback) {
  if (bootstrap_contacts.empty()) {
    callback(-1);
    return;
  }

  if (!client_only_node_ && listening_transport_->listening_port() == 0) {
    callback(-1);
    return;
  }

  // TODO(Fraser#5#): 2011-07-08 - Need to update code for local endpoints.
  std::vector<transport::Endpoint> local_endpoints;
  // Create contact_ inforrmation for node and set contact for Rpcs
  transport::Endpoint endpoint;
  endpoint.ip = listening_transport_->transport_details().endpoint.ip;
  endpoint.port = listening_transport_->transport_details().endpoint.port;
  local_endpoints.push_back(endpoint);
  Contact contact(node_id, endpoint, local_endpoints,
                  listening_transport_->transport_details().rendezvous_endpoint,
                  false, false, default_securifier_->kSigningKeyId(),
                  default_securifier_->kSigningPublicKey(), "");
  contact_ = contact;

  if (!client_only_node_) {
    rpcs_->set_contact(contact_);
  } else {
    protobuf::Contact proto_c(ToProtobuf(contact_));
    proto_c.set_node_id(NodeId().String());
    Contact c = FromProtobuf(proto_c);
    rpcs_->set_contact(c);
  }

  if (!routing_table_) {
    routing_table_.reset(new RoutingTable(node_id, k_));
    routing_table_->ping_oldest_contact()->connect(
        std::bind(&Node::Impl::PingOldestContact, this, arg::_1, arg::_2,
                  arg::_3));
    routing_table_->validate_contact()->connect(
        std::bind(&Node::Impl::ValidateContact, this, arg::_1));
    validate_contact_running_ = true;
  }
  if (bootstrap_contacts.size() == 1 &&
      bootstrap_contacts[0].node_id() == node_id) {
    // This is the first node on the network.
    std::vector<Contact> contacts;
    boost::thread(&Node::Impl::JoinFindNodesCallback, this, 0, contacts,
                  bootstrap_contacts, node_id, callback);
    return;
  }

  std::vector<Contact> temp_bootstrap_contacts(bootstrap_contacts);
  SortContacts(node_id, &temp_bootstrap_contacts);

  std::vector<Contact> search_contact;
  search_contact.push_back(temp_bootstrap_contacts.front());
  temp_bootstrap_contacts.erase(temp_bootstrap_contacts.begin());
  FindNodesArgsPtr find_nodes_args(new FindNodesArgs(node_id,
      std::bind(&Node::Impl::JoinFindNodesCallback, this, arg::_1, arg::_2,
                temp_bootstrap_contacts, node_id, callback)));
  AddContactsToContainer<FindNodesArgs>(search_contact, find_nodes_args);
  IterativeSearch<FindNodesArgs>(find_nodes_args);
}

void Node::Impl::JoinFindNodesCallback(
    const int &result,
    const std::vector<Contact> &/*returned_contacts*/,
    std::vector<Contact> bootstrap_contacts,
    const NodeId &node_id,
    JoinFunctor callback) {
  if (result < 0) {
    if (bootstrap_contacts.empty()) {
      callback(result);
      return;
    }
    std::vector<Contact> search_contact;
    search_contact.push_back(bootstrap_contacts.front());
    bootstrap_contacts.erase(bootstrap_contacts.begin());
    FindNodesArgsPtr find_nodes_args(new FindNodesArgs(node_id,
        std::bind(&Node::Impl::JoinFindNodesCallback, this, arg::_1, arg::_2,
                  bootstrap_contacts, node_id, callback)));
    AddContactsToContainer<FindNodesArgs>(search_contact, find_nodes_args);
    IterativeSearch<FindNodesArgs>(find_nodes_args);
  } else {
    joined_ = true;
    thread_group_.reset(new boost::thread_group());
    if (!client_only_node_) {
      service_.reset(new Service(routing_table_, data_store_,
                                 alternative_store_, default_securifier_, k_));
      service_->set_node_joined(true);
      service_->set_node_contact(contact_);
      service_->ConnectToSignals(message_handler_);
      thread_group_->create_thread(std::bind(&Node::Impl::RefreshDataStore,
                                             this));
      refresh_thread_running_ = true;
    }
    // Connect the ReportDown Signal
    report_down_contact_->connect(
        ReportDownContactPtr::element_type::slot_type(
            &Node::Impl::ReportDownContact, this, _1));
    // Startup the thread to monitor the downlist queue
    thread_group_->create_thread(
        std::bind(&Node::Impl::MonitoringDownlistThread, this));
    downlist_thread_running_ = true;
    data_store_->set_debug_name(contact_.node_id().
        ToStringEncoded(NodeId::kHex).substr(0, 10));
    callback(0);
  }
}

void Node::Impl::Leave(std::vector<Contact> *bootstrap_contacts) {
  joined_ = false;
  if (thread_group_)  {
    thread_group_->interrupt_all();
    thread_group_->join_all();
    thread_group_.reset();
  }
  refresh_thread_running_ = false;
  downlist_thread_running_ = false;
  GetBootstrapContacts(bootstrap_contacts);
  if (!rpcs_)
    rpcs_.reset();
  if (!service_)
    service_.reset();
  if (!routing_table_)
    routing_table_.reset();
}

void Node::Impl::Store(const Key &key,
                       const std::string &value,
                       const std::string &signature,
                       const boost::posix_time::time_duration &ttl,
                       SecurifierPtr securifier,
                       StoreFunctor callback) {
  if (!securifier)
    securifier = default_securifier_;
  std::shared_ptr<StoreArgs> sa(new StoreArgs(callback));
  FindNodes(key, std::bind(&Node::Impl::OperationFindNodesCB<StoreArgs>, this,
                           arg::_1, arg::_2,
                           key, value, signature, ttl,
                           securifier, sa));
}

void Node::Impl::Delete(const Key &key,
                        const std::string &value,
                        const std::string &signature,
                        SecurifierPtr securifier,
                        DeleteFunctor callback) {
  if (!securifier)
    securifier = default_securifier_;
  std::shared_ptr<DeleteArgs> da(new DeleteArgs(callback));
  boost::posix_time::time_duration ttl;
  FindNodes(key, std::bind(&Node::Impl::OperationFindNodesCB<DeleteArgs>, this,
                           arg::_1, arg::_2,
                           key, value, signature, ttl,
                           securifier, da));
}

void Node::Impl::Update(const Key &key,
                        const std::string &new_value,
                        const std::string &new_signature,
                        const std::string &old_value,
                        const std::string &old_signature,
                        SecurifierPtr securifier,
                        const boost::posix_time::time_duration &ttl,
                        UpdateFunctor callback) {
  if (!securifier)
    securifier = default_securifier_;
  std::shared_ptr<UpdateArgs> ua(new UpdateArgs(new_value, new_signature,
                                                 old_value, old_signature,
                                                 callback));
  FindNodes(key, std::bind(&Node::Impl::OperationFindNodesCB<UpdateArgs>, this,
                           arg::_1, arg::_2,
                           key, "", "", ttl,
                           securifier, ua));
}

template <class T>
void Node::Impl::OperationFindNodesCB(int result_size,
                               const std::vector<Contact> &contacts,
                               const Key &key,
                               const std::string &value,
                               const std::string &signature,
                               const boost::posix_time::time_duration &ttl,
                               SecurifierPtr securifier,
                               std::shared_ptr<T> args) {
//  boost::mutex::scoped_lock loch_surlaplage(args->mutex);
  if (result_size < threshold_) {
    if (result_size < 0) {
      args->callback(-1);
    } else {
      args->callback(-3);
    }
  } else {
    auto it = contacts.begin();
    auto it_end = contacts.end();
    while (it != it_end) {
        NodeGroupTuple nct((*it), key);
        nct.search_state = kSelectedAlpha;
        args->node_group.insert(nct);
        ++it;
    }

    it = contacts.begin();
    while (it != it_end) {
      std::shared_ptr<RpcArgs> rpc(new RpcArgs((*it), args));
      switch (args->operation_type) {
        case kOpDelete:
          rpcs_->Delete(key, value, signature, securifier, (*it),
                        std::bind(&Node::Impl::DeleteResponse<DeleteArgs>, this,
                                  arg::_1, arg::_2, rpc),
                        kTcp);
          break;
        case kOpStore: {
          boost::posix_time::seconds ttl_s(ttl.total_seconds());
          rpcs_->Store(key, value, signature, ttl_s, securifier, (*it),
                       std::bind(&Node::Impl::StoreResponse, this,
                                 arg::_1, arg::_2, rpc, key, value,
                                 signature, securifier),
                       kTcp);
          }
          break;
        case kOpUpdate: {
          std::shared_ptr<UpdateArgs> ua =
            std::dynamic_pointer_cast<UpdateArgs> (args);
          boost::posix_time::seconds ttl_s(ttl.seconds());
          rpcs_->Store(key, ua->new_value, ua->new_signature, ttl_s,
                      securifier, (*it),
                      std::bind(&Node::Impl::UpdateStoreResponse, this,
                                arg::_1, arg::_2, rpc, key, securifier),
                      kTcp);
          }
          break;
          default: break;
      }
      ++it;
    }
  }
}

void Node::Impl::StoreResponse(RankInfoPtr rank_info,
                               int response_code,
                               std::shared_ptr<RpcArgs> srpc,
                               const Key &key,
                               const std::string &value,
                               const std::string &signature,
                               SecurifierPtr securifier) {
  std::shared_ptr<StoreArgs> sa =
      std::static_pointer_cast<StoreArgs> (srpc->rpc_args);
  boost::mutex::scoped_lock loch_surlaplage(sa->mutex);
  NodeSearchState mark(kContacted);
  if (response_code < 0) {
    mark = kDown;
    // fire a signal here to notify this contact is down
    (*report_down_contact_)(srpc->contact);
  } else {
    routing_table_->AddContact(srpc->contact, RankInfoPtr());
  }
  // Mark the enquired contact
  NodeGroupByNodeId key_node_indx = sa->node_group.get<NodeGroupTuple::Id>();
  auto it_tuple = key_node_indx.find(srpc->contact.node_id());
  key_node_indx.modify(it_tuple, ChangeState(mark));

  auto pit_pending =
      sa->node_group.get<NodeGroupTuple::SearchState>().
      equal_range(kSelectedAlpha);
  int num_of_pending =
      static_cast<int>(std::distance(pit_pending.first, pit_pending.second));

  auto pit_contacted =
      sa->node_group.get<NodeGroupTuple::SearchState>().equal_range(kContacted);
  int num_of_contacted = static_cast<int>(std::distance(pit_contacted.first,
                                          pit_contacted.second));

  auto pit_down =
      sa->node_group.get<NodeGroupTuple::SearchState>().equal_range(kDown);
  int num_of_down = static_cast<int>(std::distance(pit_down.first,
                                                   pit_down.second));

  if (!sa->called_back) {
    if (num_of_down > (k_ - threshold_)) {
      // report back a failure once has more down contacts than the margin
      sa->called_back = true;
      sa->callback(-2);
    } else if (num_of_contacted >= threshold_) {
      // report back once has enough succeed contacts
      sa->called_back = true;
      sa->callback(num_of_contacted);
      return;
    }
  }
  // delete those succeeded contacts if a failure was report back
  // the response for the last responded contact shall be responsible to do it
  if ((num_of_pending == 0) && (num_of_contacted < threshold_)) {
    auto it = pit_down.first;
    while (it != pit_down.second) {
      rpcs_->Delete(key, value, signature, securifier, (*it).contact,
                    std::bind(&Node::Impl::SingleDeleteResponse,
                              this, arg::_1, arg::_2, (*it).contact),
                    kTcp);
      ++it;
    }
  }
}

void Node::Impl::SingleDeleteResponse(RankInfoPtr rank_info,
                                      int response_code,
                                      const Contact &contact) {
  if (response_code < 0) {
    // fire a signal here to notify this contact is down
    (*report_down_contact_)(contact);
  }
}

template <class T>
void Node::Impl::DeleteResponse(RankInfoPtr rank_info,
                                int response_code,
                                std::shared_ptr<RpcArgs> drpc) {
  std::shared_ptr<T> da = std::static_pointer_cast<T> (drpc->rpc_args);
  // called_back flag needs to be protected by the mutex lock
  boost::mutex::scoped_lock loch_surlaplage(da->mutex);
  if (da->called_back)
    return;

  NodeSearchState mark(kContacted);
  if (response_code < 0) {
    mark = kDown;
    // fire a signal here to notify this contact is down
    (*report_down_contact_)(drpc->contact);
  } else {
    routing_table_->AddContact(drpc->contact, RankInfoPtr());
  }
  // Mark the enquired contact
  NodeGroupByNodeId key_node_indx =
      drpc->rpc_args->node_group.get<NodeGroupTuple::Id>();
  auto it_tuple = key_node_indx.find(drpc->contact.node_id());
  key_node_indx.modify(it_tuple, ChangeState(mark));

  auto pit_pending =
      drpc->rpc_args->node_group.get<NodeGroupTuple::SearchState>().
      equal_range(kSelectedAlpha);
//  int num_of_pending = std::distance(pit_pending.first, pit_pending.second);

  auto pit_contacted =
      drpc->rpc_args->node_group.get<NodeGroupTuple::SearchState>().
      equal_range(kContacted);
  int num_of_contacted = static_cast<int>(std::distance(pit_contacted.first,
                                                        pit_contacted.second));

  auto pit_down = drpc->rpc_args->node_group.get<NodeGroupTuple::SearchState>().
                  equal_range(kDown);
  int num_of_down = static_cast<int>(std::distance(pit_down.first,
                                                   pit_down.second));

  if (num_of_down > (k_ - threshold_)) {
    // report back a failure once has more down contacts than the margin
    da->callback(-2);
    da->called_back = true;
  }
  if (num_of_contacted >= threshold_) {
    // report back once has enough succeed contacts
    da->callback(num_of_contacted);
    da->called_back = true;
  }

  // by far only report failure defined, unlike to what happens in Store,
  // there is no restore (undo those success deleted) operation in delete
}

void Node::Impl::UpdateStoreResponse(RankInfoPtr rank_info,
                                     int response_code,
                                     std::shared_ptr<RpcArgs> urpc,
                                     const Key &key,
                                     SecurifierPtr securifier) {
  std::shared_ptr<UpdateArgs> ua =
      std::static_pointer_cast<UpdateArgs> (urpc->rpc_args);
  if (response_code < 0) {
    boost::mutex::scoped_lock loch_surlaplage(ua->mutex);
    // once store operation failed, the contact will be marked as DOWN
    // and no DELETE operation for that contact will be executed
    NodeGroupByNodeId key_node_indx = ua->node_group.get<NodeGroupTuple::Id>();
    auto it_tuple = key_node_indx.find(urpc->contact.node_id());
    key_node_indx.modify(it_tuple, ChangeState(kDown));

    // ensure this down contact is not the last one, prevent a deadend
    auto pit_pending = ua->node_group.get<NodeGroupTuple::SearchState>().
                       equal_range(kSelectedAlpha);
    int num_of_total_pending = static_cast<int>(std::distance(pit_pending.first,
                                                pit_pending.second));
    if (num_of_total_pending == 0) {
      ua->callback(-2);
      ua->called_back = true;
    }
    // fire a signal here to notify this contact is down
    (*report_down_contact_)(urpc->contact);
  } else {
    routing_table_->AddContact(urpc->contact, RankInfoPtr());
    rpcs_->Delete(key, ua->old_value, ua->old_signature,
                  securifier, urpc->contact,
                  std::bind(&Node::Impl::DeleteResponse<UpdateArgs>, this,
                            arg::_1, arg::_2, urpc),
                  kTcp);
  }
}

void Node::Impl::FindValue(const Key &key,
                           SecurifierPtr securifier,
                           FindValueFunctor callback) {
  FindValueArgsPtr find_value_args(new FindValueArgs(key, securifier,
                                                     callback));
  // initialize with local k closest as a seed
  std::vector<Contact> close_nodes, excludes;
  routing_table_->GetCloseContacts(key, k_, excludes, &close_nodes);
  AddContactsToContainer<FindValueArgs>(close_nodes, find_value_args);
  IterativeSearch<FindValueArgs>(find_value_args);
}

void Node::Impl::GetContact(const NodeId &node_id,
                            GetContactFunctor callback) {
  FindNodes(node_id,
            std::bind(&Node::Impl::GetContactCallBack, this,
                      arg::_1, arg::_2, node_id, callback));
}

void Node::Impl::GetContactCallBack(int /*result_size*/,
                                    const std::vector<Contact> &closest,
                                    const NodeId &node_id,
                                    GetContactFunctor callback) {
  auto it = closest.begin();
  while (it != closest.end()) {
    if ((*it).node_id() == node_id) {
      callback(1, (*it));
      return;
    }
    ++it;
  }
  callback(-1, Contact());
}

void Node::Impl::SetLastSeenToNow(const Contact &contact) {
  Contact result;
  routing_table_->GetContact(contact.node_id(), &result);
  if (result == Contact())
    return;
  // If the contact exists in the routing table, add it again will set its
  // last_seen to now
  routing_table_->AddContact(contact, RankInfoPtr());
}

void Node::Impl::IncrementFailedRpcs(const Contact &contact) {
  routing_table_->IncrementFailedRpcCount(contact.node_id());
}

void Node::Impl::UpdateRankInfo(const Contact &contact,
                                RankInfoPtr rank_info) {
  routing_table_->UpdateRankInfo(contact.node_id(), rank_info);
}

RankInfoPtr Node::Impl::GetLocalRankInfo(const Contact &contact) const {
  return routing_table_->GetLocalRankInfo(contact);
}

void Node::Impl::GetAllContacts(std::vector<Contact> *contacts) {
  routing_table_->GetAllContacts(contacts);
}

void Node::Impl::GetBootstrapContacts(std::vector<Contact> *contacts) {
  routing_table_->GetBootstrapContacts(contacts);
}

Contact Node::Impl::contact() const {
  return contact_;
}

bool Node::Impl::joined() const {
  return joined_;
}

AlternativeStorePtr Node::Impl::alternative_store() {
  return alternative_store_;
}

OnOnlineStatusChangePtr Node::Impl::on_online_status_change() {
  return on_online_status_change_;
}

bool Node::Impl::client_only_node() const {
  return client_only_node_;
}

uint16_t Node::Impl::k() const {
  return k_;
}

uint16_t Node::Impl::alpha() const {
  return kAlpha_;
}

uint16_t Node::Impl::beta() const {
  return kBeta_;
}

boost::posix_time::time_duration Node::Impl::mean_refresh_interval() const {
  return kMeanRefreshInterval_;
}

bool Node::Impl::refresh_thread_running() const {
  return refresh_thread_running_;
}

bool Node::Impl::downlist_thread_running() const {
  return downlist_thread_running_;
}

//  void Node::Impl::StoreRefreshCallback(RankInfoPtr rank_info,
//                                        const int &result) {
//    //  if result is not success then make downlist
//  }

void Node::Impl::PostStoreRefresh(const KeyValueTuple &key_value_tuple) {
  FindNodes(NodeId(key_value_tuple.key()), std::bind(
      &Node::Impl::StoreRefresh, this, arg::_1, arg::_2, key_value_tuple));
}

void Node::Impl::StoreRefresh(int result,
                              std::vector<Contact> contacts,
                              const KeyValueTuple &key_value_tuple) {
  // if (result != 0)
  //   return;

  size_t size(contacts.size());
  for (size_t i = 0; i != size; ++i) {
    if (contacts[i].node_id() != contact_.node_id()) {
      std::function<void(RankInfoPtr, const int&)> store_refresh =
          std::bind(&Node::Impl::StoreRefreshCallback, this, arg::_1, result,
                    std::cref(contacts[i]));
      rpcs_->StoreRefresh(key_value_tuple.request_and_signature.first,
                          key_value_tuple.request_and_signature.second,
                          default_securifier_, contacts[i], store_refresh,
                          kTcp);
    }
  }
}

void Node::Impl::StoreRefreshCallback(RankInfoPtr rank_info, const int &result,
                                      const Contact &contact) {
  if (result != 0) {
    down_contacts_.push_back(contact.node_id());
    ReportDownContact(contact);
  }
}

void Node::Impl::RefreshDataStore() {
  std::vector<KeyValueTuple> key_value_tuples;
  while (joined_) {
    Sleep(boost::posix_time::milliseconds(10000));
    data_store_->Refresh(&key_value_tuples);
    std::for_each(key_value_tuples.begin(), key_value_tuples.end(),
                  std::bind(&Node::Impl::PostStoreRefresh, this, arg:: _1));
  }
}

void Node::Impl::EnablePingOldestContact() {
  // Connect the ping_oldest_contact signal in the routing table
  if (!validate_contact_running_) {
    routing_table_->ping_oldest_contact()->connect(
        std::bind(&Node::Impl::PingOldestContact,
                  this, arg::_1, arg::_2, arg::_3));
    validate_contact_running_ = true;
  }
}

void Node::Impl::EnableValidateContact() {
  // Connect the validate_contact signal in the routing table
  if (!validate_contact_running_) {
    routing_table_->validate_contact()->connect(
        std::bind(&Node::Impl::ValidateContact, this, arg::_1));
    validate_contact_running_ = true;
  }
}

// TODO(qi.ma@maidsafe.net): the info of the node reporting these k-closest
// contacts will need to be recorded during the FindValue process once the
// CACHE methodology is decided
template <class T>
void Node::Impl::AddContactsToContainer(const std::vector<Contact> contacts,
                                        std::shared_ptr<T> find_args) {
  // Only insert the tuple when it does not existe in the container
  boost::mutex::scoped_lock lock(find_args->mutex);
  NodeGroupByNodeId key_node_indx =
      find_args->node_group.template get<NodeGroupTuple::Id>();
  for (size_t n = 0; n < contacts.size(); ++n) {
    auto it_tuple = key_node_indx.find(contacts[n].node_id());
    if (it_tuple == key_node_indx.end()) {
      NodeGroupTuple nct(contacts[n], find_args->key);
      find_args->node_group.insert(nct);
    }
  }
}

template <class T>
bool Node::Impl::HandleIterationStructure(
    const Contact &contact,
    std::shared_ptr<T> find_args,
    NodeSearchState mark,
    int *response_code,
    std::vector<Contact> *closest_contacts,
    bool *cur_iteration_done,
    bool *called_back) {
  bool result = false;
  boost::mutex::scoped_lock loch_surlaplage(find_args->mutex);

  // Mark the enquired contact
  NodeGroupByNodeId key_node_indx =
      find_args->node_group.template get<NodeGroupTuple::Id>();
  auto it_tuple = key_node_indx.find(contact.node_id());
  key_node_indx.modify(it_tuple, ChangeState(mark));

  NodeGroupByDistance distance_node_indx =
      find_args->node_group.template get<NodeGroupTuple::Distance>();
  auto it = distance_node_indx.begin();
  auto it_end = distance_node_indx.end();
  int num_new_contacts(0);
  int num_candidates(0);
  while ((it != it_end) && (num_candidates < k_)) {
    if ((*it).search_state == kNew)
      ++num_new_contacts;
    if ((*it).search_state != kDown)
      ++num_candidates;
    ++it;
  }

  // To tell if the current iteration is done or not, only need to test:
  //    if number of pending(waiting for response) contacts
  //    is not greater than (kAlpha_ - kBeta_)
  // always check with the latest round, no need to worry about the previous
  auto pit = find_args->node_group.template
                 get<NodeGroupTuple::StateAndRound>().equal_range(
                 boost::make_tuple(kSelectedAlpha, find_args->round));
  int num_of_round_pending = static_cast<int>(std::distance(pit.first,
                                                            pit.second));
  if (num_of_round_pending <= (kAlpha_ - kBeta_))
    *cur_iteration_done = true;

  auto pit_pending =
      find_args->node_group.template
      get<NodeGroupTuple::SearchState>().equal_range(kSelectedAlpha);
  int num_of_total_pending = static_cast<int>(std::distance(pit_pending.first,
                                              pit_pending.second));
  {
    //     no kNew contacts among the top K
    // And no kSelectedAlpha (pending) contacts in total
    if ((num_new_contacts == 0) && (num_of_total_pending == 0))
      *called_back = true;
  }
  {
    // To prevent the situation that may keep requesting contacts if there
    // is any pending contacts, the request will be halted once got k-closest
    // contacted in the result (i.e. wait till all pending contacts cleared)
    if ((num_candidates == k_) && (num_of_total_pending != 0))
      *cur_iteration_done = false;
  }

  // If the search can be stopped, then we callback (report the result list)
  if (*called_back) {
    auto it = distance_node_indx.begin();
    auto it_end = distance_node_indx.end();
    while ((it != it_end) && (closest_contacts->size() < k_)) {
      if ((*it).search_state == kContacted)
        closest_contacts->push_back((*it).contact);
      ++it;
    }
    *response_code = static_cast<int>(closest_contacts->size());
    // main part of memory resource in find_args can be released here
    find_args->node_group.clear();
  }
  result = true;
  return result;
}

void Node::Impl::FindNodes(const Key &key, FindNodesFunctor callback) {
  std::vector<Contact> close_nodes, excludes;
  FindNodesArgsPtr find_nodes_args(new FindNodesArgs(key, callback));

  // initialize with local k closest as a seed
  routing_table_->GetCloseContacts(key, k_, excludes, &close_nodes);
  AddContactsToContainer<FindNodesArgs>(close_nodes, find_nodes_args);
  IterativeSearch<FindNodesArgs>(find_nodes_args);
}

template <class T>
void Node::Impl::IterativeSearch(std::shared_ptr<T> find_args) {
  boost::mutex::scoped_lock loch_surlaplage(find_args->mutex);
  auto pit = find_args->node_group.template
             get<NodeGroupTuple::StateAndDistance>().equal_range(
             boost::make_tuple(kNew));
  int num_of_candidates = static_cast<int>(std::distance(pit.first,
                                                         pit.second));

  if (num_of_candidates == 0) {
     // All contacted or in waitingresponse state, then just do nothing here
    return;
  }
  // find Alpha closest contacts to enquire
  // or all the left contacts if less than Alpha contacts haven't been tried
  uint16_t counter = 0;
  auto it_begin = pit.first;
  auto it_end = pit.second;
  std::vector<NodeId> to_contact;
  while ((it_begin != it_end) && (counter < kAlpha_)) {
    // note, change the state value here may cause re-sorting of the
    // multi-index container. So we can only collect the node_id of the
    // contacts need to be changed, then change their state value later
    to_contact.push_back((*it_begin).contact_id);
    ++it_begin;
    ++counter;
  }

  NodeGroupByNodeId key_node_indx = find_args->node_group.template
                                    get<NodeGroupTuple::Id>();
  // Update contacts' state
  for (auto it = to_contact.begin(); it != to_contact.end(); ++it) {
    auto it_tuple = key_node_indx.find(*it);
    key_node_indx.modify(it_tuple, ChangeState(kSelectedAlpha));
    key_node_indx.modify(it_tuple, ChangeRound(find_args->round+1));
  }
  ++find_args->round;
  // Better to change the value in a bunch and then issue RPCs in a bunch
  // to avoid any possibilities of cross-interference
  for (auto it = to_contact.begin(); it != to_contact.end(); ++it) {
    auto it_tuple = key_node_indx.find(*it);
    std::shared_ptr<RpcArgs> find_rpc_args(new RpcArgs((*it_tuple).contact,
                                                       find_args));
    switch (find_args->operation_type) {
      case kOpFindNode: {
          rpcs_->FindNodes(find_args->key, default_securifier_,
                           (*it_tuple).contact,
                           std::bind(&Node::Impl::IterativeSearchNodeResponse,
                                     this, arg::_1, arg::_2, arg::_3,
                                     find_rpc_args),
                           kTcp);
        }
        break;
      case kOpFindValue: {
          std::shared_ptr<FindValueArgs> find_value_args =
              std::dynamic_pointer_cast<FindValueArgs>(find_args);
          rpcs_->FindValue(find_value_args->key, find_value_args->securifier,
                           (*it_tuple).contact,
                           std::bind(&Node::Impl::IterativeSearchValueResponse,
                                     this, arg::_1, arg::_2, arg::_3, arg::_4,
                                     arg::_5, find_rpc_args),
                           kTcp);
        }
        break;
      default: break;
    }
  }
}

void Node::Impl::IterativeSearchValueResponse(
    RankInfoPtr rank_info,
    int result,
    const std::vector<std::string> &values,
    const std::vector<Contact> &contacts,
    const Contact &alternative_store,
    std::shared_ptr<RpcArgs> find_value_rpc_args) {
  FindValueArgsPtr find_value_args =
      std::static_pointer_cast<FindValueArgs> (find_value_rpc_args->rpc_args);
  if (find_value_args->called_back)
    return;
  // once got some result, terminate the search and report the result back
  // immediately
  bool curr_iteration_done(false), called_back(false);
  int response_code(0);
  std::vector<Contact> closest_contacts;
  if (!values.empty()) {
    called_back = true;
    response_code = static_cast<int>(values.size());
  } else {
    NodeSearchState mark(kContacted);
    if (result < 0) {
      mark = kDown;
      // fire a signal here to notify this contact is down
      (*report_down_contact_)(find_value_rpc_args->contact);
    } else {
      routing_table_->AddContact(find_value_rpc_args->contact, RankInfoPtr());
      AddContactsToContainer<FindValueArgs>(contacts, find_value_args);
    }

    if (!HandleIterationStructure<FindValueArgs>(find_value_rpc_args->contact,
                                                 find_value_args, mark,
                                                 &response_code,
                                                 &closest_contacts,
                                                 &curr_iteration_done,
                                                 &called_back)) {
      DLOG(ERROR) << "Structure handling in iteration failed";
    }
    response_code = -2;
    if ((!called_back) && (curr_iteration_done))
      IterativeSearch<FindValueArgs>(find_value_args);
  }

  if (called_back) {
    boost::mutex::scoped_lock loch_surlaplage(find_value_args->mutex);
    // TODO(qi.ma@maidsafe.net): the cache contact shall be populated once the
    // methodology of CACHE is decided
    Contact cache_contact;
    FindValueReturns find_value_returns = { response_code, values,
        closest_contacts, alternative_store, cache_contact };
    find_value_args->callback(find_value_returns);
    find_value_args->called_back = true;
  }
}

void Node::Impl::IterativeSearchNodeResponse(
    RankInfoPtr rank_info,
    int result,
    const std::vector<Contact> &contacts,
    std::shared_ptr<RpcArgs> fnrpc) {
  FindNodesArgsPtr find_nodes_args =
      std::static_pointer_cast<FindNodesArgs>(fnrpc->rpc_args);

  // If already called_back, i.e. result has already been reported
  // then do nothing, just return
  if (find_nodes_args->called_back) {
    return;
  }
  bool curr_iteration_done(false), called_back(false);
  int response_code(0);
  std::vector<Contact> closest_contacts;
  NodeSearchState mark(kContacted);
  if (result < 0) {
    mark = kDown;
    // fire a signal here to notify this contact is down
    (*report_down_contact_)(fnrpc->contact);
    boost::mutex::scoped_lock lock(find_nodes_args->mutex);
    if (find_nodes_args->node_group.size() == 1) {
      find_nodes_args->callback(-1, closest_contacts);
      find_nodes_args->node_group.clear();
      return;
    }
  } else {
    routing_table_->AddContact(fnrpc->contact, RankInfoPtr());
    AddContactsToContainer<FindNodesArgs>(contacts, find_nodes_args);
//    for (size_t i = 0; i < contacts.size(); ++i)
//      routing_table_->AddContact(contacts[i], rank_info);
//    RankInfoPtr rank_info;
//    routing_table_->AddContact(fnrpc->contact, rank_info);
  }

  if (!HandleIterationStructure<FindNodesArgs>(fnrpc->contact, find_nodes_args,
                                               mark, &response_code,
                                               &closest_contacts,
                                               &curr_iteration_done,
                                               &called_back)) {
    DLOG(WARNING) << "Failed to handle result for the iteration" << std::endl;
  }

  if (!called_back) {
    if (curr_iteration_done)
      IterativeSearch<FindNodesArgs>(find_nodes_args);
  } else {
    boost::mutex::scoped_lock loch_surlaplage(find_nodes_args->mutex);
    find_nodes_args->callback(response_code, closest_contacts);
    find_nodes_args->called_back = true;
  }
}

void Node::Impl::PingOldestContact(const Contact &oldest_contact,
                                   const Contact &replacement_contact,
                                   RankInfoPtr replacement_rank_info) {
  Rpcs::PingFunctor callback(std::bind(&Node::Impl::PingOldestContactCallback,
                                       this, oldest_contact, arg::_1, arg::_2,
                                       replacement_contact,
                                       replacement_rank_info));
  rpcs_->Ping(SecurifierPtr(), oldest_contact, callback, kTcp);
}

void Node::Impl::PingOldestContactCallback(Contact oldest_contact,
                                           RankInfoPtr oldest_rank_info,
                                           const int &result,
                                           Contact replacement_contact,
                                           RankInfoPtr replacement_rank_info) {
  if (result < 0) {
    // Increase the RPCfailure of the oldest_contact by one, and then try to
    // add the new contact again
    routing_table_->IncrementFailedRpcCount(oldest_contact.node_id());
    routing_table_->IncrementFailedRpcCount(oldest_contact.node_id());
    routing_table_->AddContact(replacement_contact, replacement_rank_info);
    routing_table_->SetValidated(replacement_contact.node_id(), true);
  } else {
    // Add the oldest_contact again to update its last_seen to now
    routing_table_->AddContact(oldest_contact, oldest_rank_info);
  }
}

void Node::Impl::ReportDownContact(const Contact &down_contact) {
  routing_table_->IncrementFailedRpcCount(down_contact.node_id());
  boost::mutex::scoped_lock loch_surlaplage(mutex_);
  down_contacts_.push_back(down_contact.node_id());
  condition_downlist_.notify_one();
}

void Node::Impl::MonitoringDownlistThread() {
  while (joined_) {
    boost::mutex::scoped_lock loch_surlaplage(mutex_);
    while (down_contacts_.empty() && joined_) {
      condition_downlist_.wait(loch_surlaplage);
    }

    // report the downlist to local k-closest contacts
//    std::vector<Contact> close_nodes, excludes;
//    routing_table_->GetContactsClosestToOwnId(k_, excludes, &close_nodes);
//    auto it = close_nodes.begin();
//    auto it_end = close_nodes.end();
//    while (it != it_end) {
//      rpcs_->Downlist(down_contacts_, default_securifier_, (*it), kTcp);
//      ++it;
//    }
//    down_contacts_.clear();
  }
}

void Node::Impl::ValidateContact(const Contact &contact) {
  GetPublicKeyAndValidationCallback callback(
      std::bind(&Node::Impl::ValidateContactCallback, this, contact, arg::_1,
                arg::_2));
  default_securifier_->GetPublicKeyAndValidation(contact.public_key_id(),
                                                 callback);
}

void Node::Impl::ValidateContactCallback(Contact contact,
                                         std::string public_key,
                                         std::string public_key_validation) {
  bool valid = default_securifier_->Validate("", "", contact.public_key_id(),
                                             public_key, public_key_validation,
                                             contact.node_id().String());
  routing_table_->SetValidated(contact.node_id(), valid);
}

void Node::Impl::SetService(std::shared_ptr<Service> service) {
  service_ = service;
  service_->GetPingDownListSignalHandler()->connect(std::bind(
                      &Node::Impl::PingDownlistContact, this, arg::_1));
}

void Node::Impl::PingDownlistContact(const Contact &contact) {
  Rpcs::PingFunctor callback(std::bind(
                                &Node::Impl::PingDownlistContactCallback,
                                this, contact, arg::_1, arg::_2));
  rpcs_->Ping(SecurifierPtr(), contact, callback, kTcp);
}

void Node::Impl::PingDownlistContactCallback(Contact contact,
                                             RankInfoPtr rank_info,
                                             const int &result) {
  if (result < 0) {
    // Increase the RPCfailure of the downlist contact by one
    routing_table_->IncrementFailedRpcCount(contact.node_id());
  } else {
    // Add the oldest_contact again to update its last_seen to now
    routing_table_->AddContact(contact, rank_info);
  }
}

}  // namespace kademlia

}  // namespace dht

}  // namespace maidsafe
