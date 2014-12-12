/*
 *  Copyright 2011 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/p2p/base/constants.h"
#include "webrtc/p2p/base/fakesession.h"
#include "webrtc/p2p/base/p2ptransport.h"
#include "webrtc/libjingle/session/parsing.h"
#include "webrtc/p2p/base/rawtransport.h"
#include "webrtc/libjingle/session/sessionmessages.h"
#include "webrtc/libjingle/xmllite/xmlelement.h"
#include "webrtc/libjingle/xmpp/constants.h"
#include "webrtc/base/fakesslidentity.h"
#include "webrtc/base/gunit.h"
#include "webrtc/base/network.h"
#include "webrtc/base/thread.h"

using cricket::Candidate;
using cricket::Candidates;
using cricket::Transport;
using cricket::FakeTransport;
using cricket::TransportChannel;
using cricket::FakeTransportChannel;
using cricket::IceRole;
using cricket::TransportDescription;
using cricket::WriteError;
using cricket::ParseError;
using rtc::SocketAddress;

static const char kIceUfrag1[] = "TESTICEUFRAG0001";
static const char kIcePwd1[] = "TESTICEPWD00000000000001";

static const char kIceUfrag2[] = "TESTICEUFRAG0002";
static const char kIcePwd2[] = "TESTICEPWD00000000000002";

class TransportTest : public testing::Test,
                      public sigslot::has_slots<> {
 public:
  TransportTest()
      : thread_(rtc::Thread::Current()),
        transport_(new FakeTransport(
            thread_, thread_, "test content name", NULL)),
        channel_(NULL),
        connecting_signalled_(false),
        completed_(false),
        failed_(false) {
    transport_->SignalConnecting.connect(this, &TransportTest::OnConnecting);
    transport_->SignalCompleted.connect(this, &TransportTest::OnCompleted);
    transport_->SignalFailed.connect(this, &TransportTest::OnFailed);
  }
  ~TransportTest() {
    transport_->DestroyAllChannels();
  }
  bool SetupChannel() {
    channel_ = CreateChannel(1);
    return (channel_ != NULL);
  }
  FakeTransportChannel* CreateChannel(int component) {
    return static_cast<FakeTransportChannel*>(
        transport_->CreateChannel(component));
  }
  void DestroyChannel() {
    transport_->DestroyChannel(1);
    channel_ = NULL;
  }

 protected:
  void OnConnecting(Transport* transport) {
    connecting_signalled_ = true;
  }
  void OnCompleted(Transport* transport) {
    completed_ = true;
  }
  void OnFailed(Transport* transport) {
    failed_ = true;
  }

  rtc::Thread* thread_;
  rtc::scoped_ptr<FakeTransport> transport_;
  FakeTransportChannel* channel_;
  bool connecting_signalled_;
  bool completed_;
  bool failed_;
};

class FakeCandidateTranslator : public cricket::CandidateTranslator {
 public:
  void AddMapping(int component, const std::string& channel_name) {
    name_to_component[channel_name] = component;
    component_to_name[component] = channel_name;
  }

  bool GetChannelNameFromComponent(
      int component, std::string* channel_name) const {
    if (component_to_name.find(component) == component_to_name.end()) {
      return false;
    }
    *channel_name = component_to_name.find(component)->second;
    return true;
  }
  bool GetComponentFromChannelName(
      const std::string& channel_name, int* component) const {
    if (name_to_component.find(channel_name) == name_to_component.end()) {
      return false;
    }
    *component = name_to_component.find(channel_name)->second;
    return true;
  }

  std::map<std::string, int> name_to_component;
  std::map<int, std::string> component_to_name;
};

// Test that calling ConnectChannels triggers an OnConnecting signal.
TEST_F(TransportTest, TestConnectChannelsDoesSignal) {
  EXPECT_TRUE(SetupChannel());
  transport_->ConnectChannels();
  EXPECT_FALSE(connecting_signalled_);

  EXPECT_TRUE_WAIT(connecting_signalled_, 100);
}

// Test that DestroyAllChannels kills any pending OnConnecting signals.
TEST_F(TransportTest, TestDestroyAllClearsPosts) {
  EXPECT_TRUE(transport_->CreateChannel(1) != NULL);

  transport_->ConnectChannels();
  transport_->DestroyAllChannels();

  thread_->ProcessMessages(0);
  EXPECT_FALSE(connecting_signalled_);
}

// This test verifies channels are created with proper ICE
// role, tiebreaker and remote ice mode and credentials after offer and
// answer negotiations.
TEST_F(TransportTest, TestChannelIceParameters) {
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLING);
  transport_->SetIceTiebreaker(99U);
  cricket::TransportDescription local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(local_desc,
                                                       cricket::CA_OFFER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());
  EXPECT_TRUE(SetupChannel());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
  EXPECT_EQ(cricket::ICEMODE_FULL, channel_->remote_ice_mode());
  EXPECT_EQ(kIceUfrag1, channel_->ice_ufrag());
  EXPECT_EQ(kIcePwd1, channel_->ice_pwd());

  cricket::TransportDescription remote_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(remote_desc,
                                                        cricket::CA_ANSWER,
                                                        NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
  EXPECT_EQ(99U, channel_->IceTiebreaker());
  EXPECT_EQ(cricket::ICEMODE_FULL, channel_->remote_ice_mode());
  // Changing the transport role from CONTROLLING to CONTROLLED.
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLED);
  EXPECT_EQ(cricket::ICEROLE_CONTROLLED, channel_->GetIceRole());
  EXPECT_EQ(cricket::ICEMODE_FULL, channel_->remote_ice_mode());
  EXPECT_EQ(kIceUfrag1, channel_->remote_ice_ufrag());
  EXPECT_EQ(kIcePwd1, channel_->remote_ice_pwd());
}

// Verifies that IceCredentialsChanged returns true when either ufrag or pwd
// changed, and false in other cases.
TEST_F(TransportTest, TestIceCredentialsChanged) {
  EXPECT_TRUE(cricket::IceCredentialsChanged("u1", "p1", "u2", "p2"));
  EXPECT_TRUE(cricket::IceCredentialsChanged("u1", "p1", "u2", "p1"));
  EXPECT_TRUE(cricket::IceCredentialsChanged("u1", "p1", "u1", "p2"));
  EXPECT_FALSE(cricket::IceCredentialsChanged("u1", "p1", "u1", "p1"));
}

// This test verifies that the callee's ICE role changes from controlled to
// controlling when the callee triggers an ICE restart.
TEST_F(TransportTest, TestIceControlledToControllingOnIceRestart) {
  EXPECT_TRUE(SetupChannel());
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLED);

  cricket::TransportDescription desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(desc,
                                                        cricket::CA_OFFER,
                                                        NULL));
  ASSERT_TRUE(transport_->SetLocalTransportDescription(desc,
                                                       cricket::CA_ANSWER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLED, transport_->ice_role());

  cricket::TransportDescription new_local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag2, kIcePwd2);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(new_local_desc,
                                                       cricket::CA_OFFER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
}

// This test verifies that the caller's ICE role changes from controlling to
// controlled when the callee triggers an ICE restart.
TEST_F(TransportTest, TestIceControllingToControlledOnIceRestart) {
  EXPECT_TRUE(SetupChannel());
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLING);

  cricket::TransportDescription desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(desc,
                                                       cricket::CA_OFFER,
                                                       NULL));
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(desc,
                                                        cricket::CA_ANSWER,
                                                        NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());

  cricket::TransportDescription new_local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag2, kIcePwd2);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(new_local_desc,
                                                       cricket::CA_ANSWER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLED, transport_->ice_role());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLED, channel_->GetIceRole());
}

// This test verifies that the caller's ICE role is still controlling after the
// callee triggers ICE restart if the callee's ICE mode is LITE.
TEST_F(TransportTest, TestIceControllingOnIceRestartIfRemoteIsIceLite) {
  EXPECT_TRUE(SetupChannel());
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLING);

  cricket::TransportDescription desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(desc,
                                                       cricket::CA_OFFER,
                                                       NULL));

  cricket::TransportDescription remote_desc(
      cricket::NS_JINGLE_ICE_UDP, std::vector<std::string>(),
      kIceUfrag1, kIcePwd1, cricket::ICEMODE_LITE,
      cricket::CONNECTIONROLE_NONE, NULL, cricket::Candidates());
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(remote_desc,
                                                        cricket::CA_ANSWER,
                                                        NULL));

  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());

  cricket::TransportDescription new_local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag2, kIcePwd2);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(new_local_desc,
                                                       cricket::CA_ANSWER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
}

// This test verifies that the Completed and Failed states can be reached.
TEST_F(TransportTest, TestChannelCompletedAndFailed) {
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLING);
  cricket::TransportDescription local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(local_desc,
                                                       cricket::CA_OFFER,
                                                       NULL));
  EXPECT_TRUE(SetupChannel());

  cricket::TransportDescription remote_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(remote_desc,
                                                        cricket::CA_ANSWER,
                                                        NULL));

  channel_->SetConnectionCount(2);
  channel_->SignalCandidatesAllocationDone(channel_);
  channel_->SetWritable(true);
  EXPECT_TRUE_WAIT(transport_->all_channels_writable(), 100);
  // ICE is not yet completed because there is still more than one connection.
  EXPECT_FALSE(completed_);
  EXPECT_FALSE(failed_);

  // When the connection count drops to 1, SignalCompleted should be emitted,
  // and completed() should be true.
  channel_->SetConnectionCount(1);
  EXPECT_TRUE_WAIT(completed_, 100);
  completed_ = false;

  // When the connection count drops to 0, SignalFailed should be emitted, and
  // completed() should be false.
  channel_->SetConnectionCount(0);
  EXPECT_TRUE_WAIT(failed_, 100);
  EXPECT_FALSE(completed_);
}

// Tests channel role is reversed after receiving ice-lite from remote.
TEST_F(TransportTest, TestSetRemoteIceLiteInOffer) {
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLED);
  cricket::TransportDescription remote_desc(
      cricket::NS_JINGLE_ICE_UDP, std::vector<std::string>(),
      kIceUfrag1, kIcePwd1, cricket::ICEMODE_LITE,
      cricket::CONNECTIONROLE_ACTPASS, NULL, cricket::Candidates());
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(remote_desc,
                                                        cricket::CA_OFFER,
                                                        NULL));
  cricket::TransportDescription local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(local_desc,
                                                       cricket::CA_ANSWER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());
  EXPECT_TRUE(SetupChannel());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
  EXPECT_EQ(cricket::ICEMODE_LITE, channel_->remote_ice_mode());
}

// Tests ice-lite in remote answer.
TEST_F(TransportTest, TestSetRemoteIceLiteInAnswer) {
  transport_->SetIceRole(cricket::ICEROLE_CONTROLLING);
  cricket::TransportDescription local_desc(
      cricket::NS_JINGLE_ICE_UDP, kIceUfrag1, kIcePwd1);
  ASSERT_TRUE(transport_->SetLocalTransportDescription(local_desc,
                                                       cricket::CA_OFFER,
                                                       NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, transport_->ice_role());
  EXPECT_TRUE(SetupChannel());
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
  // Channels will be created in ICEFULL_MODE.
  EXPECT_EQ(cricket::ICEMODE_FULL, channel_->remote_ice_mode());
  cricket::TransportDescription remote_desc(
      cricket::NS_JINGLE_ICE_UDP, std::vector<std::string>(),
      kIceUfrag1, kIcePwd1, cricket::ICEMODE_LITE,
      cricket::CONNECTIONROLE_NONE, NULL, cricket::Candidates());
  ASSERT_TRUE(transport_->SetRemoteTransportDescription(remote_desc,
                                                        cricket::CA_ANSWER,
                                                        NULL));
  EXPECT_EQ(cricket::ICEROLE_CONTROLLING, channel_->GetIceRole());
  // After receiving remote description with ICEMODE_LITE, channel should
  // have mode set to ICEMODE_LITE.
  EXPECT_EQ(cricket::ICEMODE_LITE, channel_->remote_ice_mode());
}

// Tests that we can properly serialize/deserialize candidates.
TEST_F(TransportTest, TestP2PTransportWriteAndParseCandidate) {
  Candidate test_candidate("", 1, "udp",
                           rtc::SocketAddress("2001:db8:fefe::1", 9999),
                           738197504, "abcdef", "ghijkl", "foo", 50, "");
  test_candidate.set_network_name("testnet");
  Candidate test_candidate2("", 2, "tcp",
                            rtc::SocketAddress("192.168.7.1", 9999), 1107296256,
                            "mnopqr", "stuvwx", "bar", 100, "");
  test_candidate2.set_network_name("testnet2");
  rtc::SocketAddress host_address("www.google.com", 24601);
  host_address.SetResolvedIP(rtc::IPAddress(0x0A000001));
  Candidate test_candidate3("", 3, "spdy", host_address, 1476395008, "yzabcd",
                            "efghij", "baz", 150, "");
  test_candidate3.set_network_name("testnet3");
  WriteError write_error;
  ParseError parse_error;
  rtc::scoped_ptr<buzz::XmlElement> elem;
  cricket::Candidate parsed_candidate;
  cricket::P2PTransportParser parser;

  FakeCandidateTranslator translator;
  translator.AddMapping(1, "test");
  translator.AddMapping(2, "test2");
  translator.AddMapping(3, "test3");

  EXPECT_TRUE(parser.WriteGingleCandidate(test_candidate, &translator,
                                          elem.accept(), &write_error));
  EXPECT_EQ("", write_error.text);
  EXPECT_EQ("test", elem->Attr(buzz::QN_NAME));
  EXPECT_EQ("udp", elem->Attr(cricket::QN_PROTOCOL));
  EXPECT_EQ("2001:db8:fefe::1", elem->Attr(cricket::QN_ADDRESS));
  EXPECT_EQ("9999", elem->Attr(cricket::QN_PORT));
  EXPECT_EQ("0.34", elem->Attr(cricket::QN_PREFERENCE));
  EXPECT_EQ("abcdef", elem->Attr(cricket::QN_USERNAME));
  EXPECT_EQ("ghijkl", elem->Attr(cricket::QN_PASSWORD));
  EXPECT_EQ("foo", elem->Attr(cricket::QN_TYPE));
  EXPECT_EQ("testnet", elem->Attr(cricket::QN_NETWORK));
  EXPECT_EQ("50", elem->Attr(cricket::QN_GENERATION));

  EXPECT_TRUE(parser.ParseGingleCandidate(elem.get(), &translator,
                                          &parsed_candidate, &parse_error));
  EXPECT_TRUE(test_candidate.IsEquivalent(parsed_candidate));

  EXPECT_TRUE(parser.WriteGingleCandidate(test_candidate2, &translator,
                                          elem.accept(), &write_error));
  EXPECT_EQ("test2", elem->Attr(buzz::QN_NAME));
  EXPECT_EQ("tcp", elem->Attr(cricket::QN_PROTOCOL));
  EXPECT_EQ("192.168.7.1", elem->Attr(cricket::QN_ADDRESS));
  EXPECT_EQ("9999", elem->Attr(cricket::QN_PORT));
  EXPECT_EQ("0.51", elem->Attr(cricket::QN_PREFERENCE));
  EXPECT_EQ("mnopqr", elem->Attr(cricket::QN_USERNAME));
  EXPECT_EQ("stuvwx", elem->Attr(cricket::QN_PASSWORD));
  EXPECT_EQ("bar", elem->Attr(cricket::QN_TYPE));
  EXPECT_EQ("testnet2", elem->Attr(cricket::QN_NETWORK));
  EXPECT_EQ("100", elem->Attr(cricket::QN_GENERATION));

  EXPECT_TRUE(parser.ParseGingleCandidate(elem.get(), &translator,
                                          &parsed_candidate, &parse_error));
  EXPECT_TRUE(test_candidate2.IsEquivalent(parsed_candidate));

  // Check that an ip is preferred over hostname.
  EXPECT_TRUE(parser.WriteGingleCandidate(test_candidate3, &translator,
                                          elem.accept(), &write_error));
  EXPECT_EQ("test3", elem->Attr(cricket::QN_NAME));
  EXPECT_EQ("spdy", elem->Attr(cricket::QN_PROTOCOL));
  EXPECT_EQ("10.0.0.1", elem->Attr(cricket::QN_ADDRESS));
  EXPECT_EQ("24601", elem->Attr(cricket::QN_PORT));
  EXPECT_EQ("0.69", elem->Attr(cricket::QN_PREFERENCE));
  EXPECT_EQ("yzabcd", elem->Attr(cricket::QN_USERNAME));
  EXPECT_EQ("efghij", elem->Attr(cricket::QN_PASSWORD));
  EXPECT_EQ("baz", elem->Attr(cricket::QN_TYPE));
  EXPECT_EQ("testnet3", elem->Attr(cricket::QN_NETWORK));
  EXPECT_EQ("150", elem->Attr(cricket::QN_GENERATION));

  EXPECT_TRUE(parser.ParseGingleCandidate(elem.get(), &translator,
                                          &parsed_candidate, &parse_error));
  EXPECT_TRUE(test_candidate3.IsEquivalent(parsed_candidate));
}

TEST_F(TransportTest, TestGetStats) {
  EXPECT_TRUE(SetupChannel());
  cricket::TransportStats stats;
  EXPECT_TRUE(transport_->GetStats(&stats));
  // Note that this tests the behavior of a FakeTransportChannel.
  ASSERT_EQ(1U, stats.channel_stats.size());
  EXPECT_EQ(1, stats.channel_stats[0].component);
  transport_->ConnectChannels();
  EXPECT_TRUE(transport_->GetStats(&stats));
  ASSERT_EQ(1U, stats.channel_stats.size());
  EXPECT_EQ(1, stats.channel_stats[0].component);
}
