#include <fstream>
#include <limits>
#include <future>
#include <thread>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "k230/bridge/companion_dispatcher.hpp"
#include "k230/companion_v2.hpp"
#include "k230/inspector/age_policy.hpp"
#include "k230/inspector/protection_session.hpp"

using namespace k230;
using namespace k230::inspector;
using companion::Json;
namespace {
Json fixture(const std::string& name) {
  std::ifstream input(std::string(K230_V2_FIXTURES)+"/"+name+".json");
  if (!input) throw std::runtime_error("fixture_missing");
  return Json::parse(input);
}
AnalysisBatch sample(std::int64_t pts, double p=.65, double h=0, double s=0) {
  return {pts,360,800,"screen-one","com.example.viewer",true,false,
    {{{20,100,160,300},"Image",{p,h,s},true,false}}};
}
AgeDecision chain(AgePolicy& policy, int count, std::int64_t start, std::int64_t spacing,
                  double p=.65, double h=0, double s=0) {
  AgeDecision result;
  for (int i=0;i<count;++i) result=policy.evaluate(sample(start+i*spacing,p,h,s));
  return result;
}
struct SessionHarness {
  std::shared_ptr<ipc::ControlQueue> queue=std::make_shared<ipc::ControlQueue>();
  ProtectionSession session{queue,true};
  Json state=fixture("state");
  std::int64_t seq=2;
  SessionHarness() {
    session.receive(fixture("hello"));
    auto bind=queue->pop(std::chrono::milliseconds(0));
    if (!bind || bind->at("type")!="bind") throw std::runtime_error("bind_missing");
    auto bound=fixture("bound"); bound["stream_id"]=session.stream_id();
    session.receive(bound); state["stream_id"]=session.stream_id();
  }
  void analyze(std::int64_t pts,double p=.65,double h=0) {
    analyze(sample(pts,p,h));
  }
  void analyze(AnalysisBatch batch) {
    const auto pts=batch.pts_us;
    state["seq"]=std::to_string(++seq); state["phone_time_us"]=std::to_string(pts);
    state["screen"]["sampled_at_us"]=std::to_string(pts);
    session.receive(state); session.analyze(std::move(batch));
  }
  Json ack(const Json& command,const std::string& status="executed") {
    auto result=fixture("ack");
    for (auto key:{"session_id","stream_id","event_id","action_revision","screen_token","requested_stage"}) result[key]=command.at(key);
    result["seq"]=std::to_string(++seq); result["request_seq"]=command.at("seq");
    result["status"]=status; result["executed_stage"]=status=="executed"?command.at("requested_stage"):Json(0);
    result["executed_action"]=status=="executed"?command.at("requested_action"):Json("none");
    result["executed_at_us"]=status=="executed"?command.at("pts_us"):Json(nullptr);
    result["display_rects"]=Json::array(); result["error"]=status=="executed"?Json(nullptr):Json("action_failed");
    return result;
  }
};
class Listener {
 public:
  Listener() {
    fd=::socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (fd<0 || ::bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || ::listen(fd,1))
      throw std::runtime_error("listen_failed");
    socklen_t size=sizeof(address);
    if (::getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)) throw std::runtime_error("port_failed");
    port=ntohs(address.sin_port);
  }
  ~Listener() { ::close(fd); }
  int accept() { pollfd p{fd,POLLIN,0}; return ::poll(&p,1,1500)>0?::accept(fd,nullptr,nullptr):-1; }
  int fd=-1;
  std::uint16_t port=0;
};
std::optional<Json> read_line(int peer) {
  std::string bytes;
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while (bytes.size()<companion::kMaxLine && std::chrono::steady_clock::now()<deadline) {
    pollfd p{peer,POLLIN,0}; if (::poll(&p,1,50)<=0) continue;
    char c=0; if (::recv(peer,&c,1,0)!=1) return std::nullopt;
    if (c=='\n') return companion::parse(bytes);
    bytes+=c;
  }
  return std::nullopt;
}
bool write_line(int peer,const Json& record) {
  auto bytes=companion::line(record);
  return ::send(peer,bytes.data(),bytes.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(bytes.size());
}
}

TEST(AgePolicyV2, ReplaysSharedSyntheticPolicyTraces) {
  const auto traces=fixture("policy-traces");
  for (const auto& c:traces.at("cases")) {
    SCOPED_TRACE(c.at("name").get<std::string>());
    AgePolicy policy; ASSERT_TRUE(policy.set_profile(c.at("age"),1));
    auto result=chain(policy,c.at("count"),10000000,c.at("spacing_us"),c.at("porn"),c.at("hentai"),c.at("sexy"));
    EXPECT_EQ(result.stage,c.at("expected_stage").get<int>());
  }
}
TEST(AgePolicyV2, InvalidAgesAndRevisionRollbackRetainLastValidProfile) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(13,2));
  for (int age:{-1,9,16,100}) EXPECT_FALSE(policy.set_profile(age,3));
  EXPECT_FALSE(policy.set_profile(12,1)); EXPECT_FALSE(policy.set_profile(12,2));
  ASSERT_TRUE(policy.profile()); EXPECT_EQ(policy.profile()->age,13); EXPECT_EQ(policy.profile()->revision,2);
  auto p=companion::profile_json(*policy.profile()); p["age"]=13.1;
  EXPECT_THROW(companion::profile_from_json(p),std::exception);
  p["age"]="13"; EXPECT_THROW(companion::profile_from_json(p),std::exception);
}
TEST(AgePolicyV2, RegionAndScreenChangesDuplicatePtsAndGapsCannotManufactureProof) {
  for (int mode=0;mode<7;++mode) {
    SCOPED_TRACE(mode); AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
    chain(policy,2,10000000,200000);
    auto batch=sample(10400000);
    if (mode==0) batch.pts_us=10200000;
    if (mode==1) batch.pts_us=10800000;
    if (mode==2) batch.identity="new-screen";
    if (mode==3) batch.regions[0].crop.x=180;
    if (mode==4) batch.regions[0].complete=false;
    if (mode==5) batch.discontinuity=true;
    if (mode==6) batch.regions[0].scores.porn=std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(policy.evaluate(batch).stage,0);
  }
}
TEST(AgePolicyV2, OneRegionFailureDoesNotHideIndependentPositiveEvidenceAndPartialIsNotSafe) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  AgeDecision result;
  for (int i=0;i<3;++i) {
    auto batch=sample(10000000+i*200000); batch.complete=false;
    batch.regions.push_back({{200,100,100,300},"Image",{},false,false});
    result=policy.evaluate(batch);
  }
  EXPECT_EQ(result.stage,1); EXPECT_FALSE(result.safe);
  policy.reset_evidence(); auto empty=sample(12000000,0); empty.regions.clear();
  EXPECT_FALSE(policy.evaluate(empty).safe);
  EXPECT_TRUE(policy.evaluate(sample(12200000,0,0,1)).safe);
}
TEST(AgePolicyV2, ALowerHentaiWinnerCannotPreemptIndependentPornHomeProof) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  for (int i=0;i<=10;++i) {
    auto batch=sample(10000000+i*100000,.90);
    batch.regions.push_back({{200,100,100,300},"Image",{.01,.98,0},true,false});
    auto result=policy.evaluate(batch);
    EXPECT_EQ(result.stage,i==10?3:0);
    if (i==10) { ASSERT_EQ(result.regions.size(),1u); EXPECT_GE(result.regions[0].region.scores.porn,.60); }
  }
}
TEST(AgePolicyV2, DeferralIsBoundedAndLowObservationBreaksExitProof) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  EXPECT_EQ(chain(policy,3,10000000,200000,.95).stage,0);
  auto result=policy.evaluate(sample(10600000,.85));
  EXPECT_EQ(result.stage,2); // Currently supported shield; no exit evidence survives the dip.
  policy.reset_evidence();
  EXPECT_EQ(chain(policy,3,12000000,400000,.95).stage,0);
  EXPECT_EQ(policy.evaluate(sample(13000000,.95)).stage,0);
  EXPECT_EQ(policy.evaluate(sample(13200000,.95)).stage,3);
  policy.reset_evidence();
  for (int i=0;i<5;++i) EXPECT_EQ(policy.evaluate(sample(14000000+i*500000,.95)).stage,i==4?3:0);
}
TEST(AgePolicyV2, BrokenExitChainsAndNewTracksHaveTheirOwnNonSlidingDeferral) {
  for (bool identity_change:{false,true}) {
    AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
    EXPECT_EQ(chain(policy,3,10000000,200000,.95).stage,0);
    if (!identity_change) { EXPECT_EQ(policy.evaluate(sample(10500000,0)).stage,0); }
    for (int i=0;i<11;++i) {
      auto batch=sample(10600000+i*100000,.95);
      if (identity_change) batch.identity="new-screen";
      EXPECT_EQ(policy.evaluate(batch).stage,i==10?3:0);
    }
  }
}
TEST(AgePolicyV2, RepetitionDeduplicatesExecutedEpisodesAndNeverCreatesHome) {
  for (int age:{12,13}) {
    AgePolicy policy; ASSERT_TRUE(policy.set_profile(age,1));
    auto first=companion::uuid(); policy.executed(first,"com.example.viewer",1,9900000);
    policy.executed(first,"com.example.viewer",1,9900001);
    auto result=chain(policy,3,10000000,200000,.75);
    EXPECT_EQ(result.stage,age==12?2:1);
    if (age==13) {
      policy.executed(companion::uuid(),"com.example.viewer",2,10300000);
      policy.reset_evidence(); EXPECT_EQ(chain(policy,3,10500000,200000,.75).stage,2);
    }
    ASSERT_TRUE(policy.set_profile(age,2)); EXPECT_EQ(chain(policy,3,12000000,200000,.75).stage,1);
  }
}
TEST(AgePolicyV2, AmbiguousAssociationAndRouteChangesResetEvidence) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  auto batch=sample(10000000); batch.regions.push_back(batch.regions[0]);
  policy.evaluate(batch); batch.pts_us=10200000; policy.evaluate(batch);
  batch.pts_us=10400000; EXPECT_EQ(policy.evaluate(batch).stage,0);
  policy.reset_evidence(); chain(policy,4,11000000,200000,.01,.98);
  EXPECT_EQ(policy.evaluate(sample(11800000,.90)).stage,0);
}
TEST(AgePolicyV2, TrackIdentitySurvivesDetectionReorderingAndRetainsEveryCoverCrop) {
  AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  AgeDecision result;
  for (int i=0;i<3;++i) {
    auto batch=sample(10000000+i*200000);
    batch.regions.push_back({{200,100,100,300},"Image",{.70,0,0},true,false});
    if (i==1) std::reverse(batch.regions.begin(),batch.regions.end());
    result=policy.evaluate(batch);
  }
  EXPECT_EQ(result.stage,1); ASSERT_EQ(result.regions.size(),2u);
  EXPECT_NE(result.regions[0].track_id,result.regions[1].track_id);
  EXPECT_GT(result.regions[0].region.scores.porn,result.regions[1].region.scores.porn);
}
TEST(AgePolicyV2, HentaiRepetitionProfileChangeAndRestartCannotAuthorizeHome) {
  for (int age:{12,13}) {
    AgePolicy policy; ASSERT_TRUE(policy.set_profile(age,1));
    for (int i=0;i<4;++i) policy.executed(companion::uuid(),"com.example.viewer",1,9900000+i);
    EXPECT_EQ(chain(policy,11,10000000,100000,.01,.98).stage,2);
    ASSERT_TRUE(policy.set_profile(age,2));
    EXPECT_EQ(chain(policy,11,12000000,100000,.01,.98).stage,2);
    AgePolicy restarted; ASSERT_TRUE(restarted.set_profile(age,2));
    EXPECT_EQ(chain(restarted,11,14000000,100000,.01,.98).stage,2);
  }
}
TEST(CompanionV2, RoundTripsAllSevenExactMessageFixturesThroughSocketAndControlRecords) {
  for (auto name:{"hello","bind","bound","state","decision","ack","released"}) {
    SCOPED_TRACE(name); auto value=fixture(name);
    auto encoded=companion::line(value); encoded.pop_back();
    EXPECT_EQ(companion::parse(encoded),value);
    auto record=companion::control_record(value); ASSERT_TRUE(companion::decode_control(record));
    EXPECT_EQ(*companion::decode_control(record),value);
    record[3]=1; EXPECT_FALSE(companion::decode_control(record));
  }
}
TEST(CompanionV2, RejectsMalformedDuplicateKeysTypesUnknownFieldsAndBounds) {
  auto value=fixture("hello"); auto text=value.dump();
  for (const auto& bad:std::vector<std::string>{"", "[]", "null", "{}",text+"{}","\xef\xbb\xbf"+text,text+"\r",text+"\n"})
    EXPECT_THROW(companion::parse(bad),std::exception);
  EXPECT_THROW(companion::parse("{\"v\":2,\"v\":2,\"type\":\"hello\"}"),std::exception);
  value["unexpected"]=true; EXPECT_THROW(companion::parse(value.dump()),std::exception);
  value=fixture("decision"); value["requested_stage"]=true; EXPECT_THROW(companion::validate(value),std::exception);
  value=fixture("decision"); value["seq"]="9223372036854775808"; EXPECT_THROW(companion::validate(value),std::exception);
  value=fixture("decision"); value["regions"][0]["scores"]["explicit_score"]=.99; EXPECT_THROW(companion::validate(value),std::exception);
  value=fixture("decision"); value["regions"][0]["scores"]["porn"]=-.1; EXPECT_THROW(companion::validate(value),std::exception);
  EXPECT_THROW(companion::decimal("01"),std::exception);
  EXPECT_THROW(companion::decimal(1),std::exception);
}
TEST(CompanionV2, IncrementalLineFramingHasAbsoluteDeadlineAndBoundedBuffer) {
  auto line=companion::line(fixture("hello")); companion::LineAssembler assembler;
  EXPECT_TRUE(assembler.feed(reinterpret_cast<const std::uint8_t*>(line.data()),1,0).empty());
  EXPECT_THROW(assembler.check_deadline(500001),std::exception);
  assembler={}; std::vector<Json> received;
  for (std::size_t i=0;i<line.size();++i) {
    auto records=assembler.feed(reinterpret_cast<const std::uint8_t*>(line.data()+i),1,i);
    received.insert(received.end(),records.begin(),records.end());
  }
  ASSERT_EQ(received.size(),1u); EXPECT_EQ(received[0],fixture("hello"));
  EXPECT_NO_THROW(assembler.check_deadline(999999999));
  auto huge=std::string(16384,'a');
  EXPECT_THROW(assembler.feed(reinterpret_cast<const std::uint8_t*>(huge.data()),huge.size(),1000000000),std::exception);
}
TEST(CompanionV2, GeometryExpiryScreenClockAndCoveredPixelsAreDefenseInDepthGates) {
  auto decision=fixture("decision"), state=fixture("state");
  auto rectangles=companion::validate_decision(decision,state,10450000,true);
  ASSERT_EQ(rectangles.size(),1u); EXPECT_EQ(rectangles[0],(companion::Rect{60,300,480,900}));
  EXPECT_THROW(companion::validate_decision(decision,state,10450000,false),std::exception);
  EXPECT_THROW(companion::validate_decision(decision,state,12000000,true),std::exception);
  auto wrong=decision; wrong["screen_token"]=companion::uuid();
  EXPECT_THROW(companion::validate_decision(wrong,state,10450000,true),std::exception);
  wrong=decision; wrong["transform"]["viewport_display_px"]["width"]=1070;
  EXPECT_THROW(companion::validate_decision(wrong,state,10450000,true),std::exception);
  state["protection"]={{"stage",1},{"event_id",decision.at("event_id")},{"applied_at_us","10300000"},{"covered_rects",Json::array({companion::rect_json(rectangles[0])})},{"release_pending",false}};
  EXPECT_THROW(companion::validate_decision(decision,state,10450000,true),std::exception);
}
TEST(CompanionV2, HentaiEvidenceCannotRequestHomeEvenWithForgedStage) {
  auto decision=fixture("decision"), state=fixture("state");
  decision["requested_stage"]=3; decision["requested_action"]="home";
  decision["regions"][0]["evidence"]["route"]="hentai_dominant";
  EXPECT_THROW(companion::validate_decision(decision,state,10450000,true),std::exception);
}
TEST(ProtectionSessionV2, HighPornReachesHomeAsItsFirstActionAndNeedsExecutionAck) {
  SessionHarness harness;
  for (int i=0;i<10;++i) {
    harness.analyze(10000000+i*100000,.95);
    EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  }
  harness.analyze(11000000,.95);
  auto command=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(command);
  EXPECT_EQ(command->at("requested_stage"),3); EXPECT_EQ(command->at("requested_action"),"home");
  EXPECT_EQ(command->at("action_revision"),"1"); EXPECT_EQ(harness.session.stats().executed,0u);
  harness.session.receive(harness.ack(*command)); EXPECT_EQ(harness.session.stats().executed,1u);
}
TEST(ProtectionSessionV2, NoClockOptInMeansNoBindAndNoIntervention) {
  auto queue=std::make_shared<ipc::ControlQueue>(); ProtectionSession session(queue,false);
  session.receive(fixture("hello")); session.analyze(sample(10000000,.95));
  EXPECT_FALSE(queue->pop(std::chrono::milliseconds(0))); EXPECT_EQ(session.stats().submitted,0u);
}
TEST(ProtectionSessionV2, FreshHighPornEpisodeAfterLowResetStillHasHomeAsFirstAction) {
  SessionHarness harness;
  for (int i=0;i<5;++i) harness.analyze(10000000+i*100000,.95);
  harness.analyze(10500000,0);
  for (int i=0;i<10;++i) {
    harness.analyze(10600000+i*100000,.95);
    EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  }
  harness.analyze(11600000,.95);
  auto command=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(command);
  EXPECT_EQ(command->at("requested_stage"),3); EXPECT_EQ(command->at("action_revision"),"1");
}
TEST(ProtectionSessionV2, ExplicitNonExecutionPermitsFreshProofButUnknownRemainsConservative) {
  for (const std::string status:{"rejected","failed","lost"}) {
    SCOPED_TRACE(status); SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000,.85);
    auto shield=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(shield);
    EXPECT_EQ(shield->at("requested_stage"),2);
    if (status=="lost") {
      harness.session.disconnected(); harness.session.receive(fixture("hello"));
      auto bind=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(bind);
      auto bound=fixture("bound"); bound["stream_id"]=harness.session.stream_id();
      harness.session.receive(bound);
    } else harness.session.receive(harness.ack(*shield,status));
    for (int i=0;i<3;++i) harness.analyze(10600000+i*200000);
    auto retry=harness.queue->pop(std::chrono::milliseconds(0));
    if (status=="lost") {
      EXPECT_FALSE(retry); EXPECT_EQ(harness.session.stats().unknown,1u);
    } else {
      ASSERT_TRUE(retry); EXPECT_EQ(retry->at("requested_stage"),1);
      EXPECT_EQ(retry->at("action_revision"),"2");
      EXPECT_EQ(harness.session.stats().failed,1u); EXPECT_EQ(harness.session.stats().executed,0u);
    }
  }
}
TEST(ProtectionSessionV2, FailedRevisionNeverClearsAnEarlierConfirmedOrNativeMask) {
  for (bool native_confirmation:{false,true}) {
    SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
    auto first=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(first);
    if (native_confirmation) {
      auto& protection=harness.state["protection"];
      protection["stage"]=1; protection["event_id"]=first->at("event_id");
      protection["applied_at_us"]=first->at("pts_us");
      protection["covered_rects"]=Json::array({companion::rect_json({60,300,480,900})});
      harness.analyze(10500000);
      harness.session.receive(harness.ack(*first,"failed"));
    } else {
      harness.session.receive(harness.ack(*first));
      for (int i=0;i<3;++i) {
        auto batch=sample(10600000+i*200000); batch.regions[0].crop={200,100,100,300};
        harness.analyze(std::move(batch));
      }
      auto second=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(second);
      harness.session.receive(harness.ack(*second,"failed"));
    }
    for (int i=0;i<12;++i) harness.analyze(11200000+i*100000,.95);
    EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  }
}
TEST(ProtectionSessionV2, EightSimultaneousCoversUseOneBoundedRecordAndOneRevision) {
  SessionHarness harness;
  for (int i=0;i<5;++i) {
    auto batch=sample(10000000+i*100000); batch.regions.clear();
    for (int j=0;j<8;++j) batch.regions.push_back({{10+(j%2)*180,20+(j/2)*180,150,160},"Image",{.65,0,0},true,false});
    harness.analyze(std::move(batch));
  }
  auto command=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(command);
  EXPECT_EQ(command->at("regions").size(),8u); EXPECT_EQ(command->at("action_revision"),"1");
  EXPECT_LE(companion::line(*command).size(),companion::kMaxLine);
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0))); EXPECT_EQ(harness.session.stats().submitted,1u);
}
TEST(ProtectionSessionV2, OversizedMultiCropDecisionIsRejectedWholeWithoutPartialClaims) {
  SessionHarness harness;
  for (int i=0;i<32;++i) ASSERT_TRUE(harness.queue->push(fixture("hello")));
  for (int i=0;i<32;++i) {
    auto batch=sample(10000000+i*100000); batch.regions.clear();
    for (int j=0;j<8;++j) batch.regions.push_back({{10+(j%2)*180,20+(j/2)*180,150,160},"Image",{.65,0,0},true,false});
    harness.analyze(std::move(batch));
  }
  for (int i=0;i<32;++i) {
    auto item=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(item);
    EXPECT_EQ(item->at("type"),"hello");
  }
  auto oversized=sample(13200000); oversized.regions.clear();
  for (int j=0;j<8;++j) oversized.regions.push_back({{10+(j%2)*180,20+(j/2)*180,150,160},"Image",{.65,0,0},true,false});
  const auto rejected=harness.session.stats().rejected;
  harness.analyze(std::move(oversized));
  EXPECT_EQ(harness.session.stats().submitted,0u); EXPECT_GT(harness.session.stats().rejected,rejected);
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  harness.analyze(13300000,0);
  for (int i=0;i<3;++i) harness.analyze(13400000+i*200000);
  auto fresh=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->at("action_revision"),"1"); EXPECT_EQ(fresh->at("regions").size(),1u);
}
TEST(ProtectionSessionV2, SafeCoveredPixelsAndReconnectNeverClearOrEscalateProtection) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
  auto cover=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(cover);
  harness.session.receive(harness.ack(*cover));
  for (int i=0;i<12;++i) harness.analyze(10500000+i*100000,.95);
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  harness.analyze(12000000,0); EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  harness.session.disconnected(); harness.analyze(12200000,.95);
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
}
TEST(ProtectionSessionV2, FailedAndWrongEventAcksCannotClaimExecution) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
  auto cover=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(cover);
  auto ack=harness.ack(*cover); ack["event_id"]=companion::uuid(); harness.session.receive(ack);
  EXPECT_EQ(harness.session.stats().executed,0u); EXPECT_EQ(harness.session.stats().rejected,1u);
  harness.session.receive(harness.ack(*cover,"failed"));
  EXPECT_EQ(harness.session.stats().failed,1u); EXPECT_EQ(harness.session.stats().executed,0u);
}
TEST(ProtectionSessionV2, OnlyACorrelatedNativeReleaseAllowsFreshEvidenceAndNewEpisode) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
  auto cover=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(cover);
  harness.session.receive(harness.ack(*cover));
  auto released=fixture("released");
  released["stream_id"]=harness.session.stream_id(); released["event_id"]=cover->at("event_id");
  released["seq"]=std::to_string(++harness.seq); released["action_revision"]="2";
  harness.session.receive(released); EXPECT_EQ(harness.session.stats().rejected,1u);
  released["seq"]=std::to_string(++harness.seq); released["action_revision"]="1";
  harness.session.receive(released);
  harness.state["screen"]["screen_token"]=released.at("new_screen_token");
  harness.state["screen"]["valid_from_us"]="12000000";
  for (int i=0;i<3;++i) harness.analyze(12000000+i*200000);
  auto next=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(next);
  EXPECT_NE(next->at("event_id"),cover->at("event_id")); EXPECT_EQ(next->at("action_revision"),"1");
  EXPECT_EQ(next->at("requested_stage"),2); // New executed episode permits shield repetition, never HOME.
}
TEST(ControlQueueV2, BoundRejectsRatherThanEvictsAndCloseWakesReaders) {
  ipc::ControlQueue queue;
  for (int i=0;i<32;++i) EXPECT_TRUE(queue.push(fixture("hello")));
  EXPECT_FALSE(queue.push(fixture("hello"))); queue.close(); EXPECT_FALSE(queue.push(fixture("hello")));
  for (int i=0;i<32;++i) EXPECT_TRUE(queue.pop(std::chrono::milliseconds(0)));
  EXPECT_FALSE(queue.pop(std::chrono::milliseconds(0))); EXPECT_TRUE(queue.closed());
}
TEST(CompanionDispatcherV2, NegotiatesBoundStateDecisionAndExecutionAckOverRealLoopback) {
  Listener listener;
  auto queue=std::make_shared<ipc::ControlQueue>();
  std::promise<Json> acked; auto acked_future=acked.get_future();
  bridge::CompanionConfig config; config.local_port=listener.port;
  bridge::CompanionDispatcher dispatcher(config,bridge::AdbController{},"",queue,[&](const Json& record) {
    if (record.at("type")=="hello") queue->push(fixture("bind"));
    if (record.at("type")=="state") queue->push(fixture("decision"));
    if (record.at("type")=="ack") acked.set_value(record);
  },[]{});
  auto server=std::async(std::launch::async,[&] {
    int peer=listener.accept(); if (peer<0) return false;
    auto run=[&] {
      if (::send(peer,"K",1,MSG_NOSIGNAL)!=1 || !write_line(peer,fixture("hello"))) return false;
      auto bind=read_line(peer); if (!bind || bind->at("type")!="bind") return false;
      if (!write_line(peer,fixture("bound")) || !write_line(peer,fixture("state"))) return false;
      auto decision=read_line(peer); if (!decision || *decision!=fixture("decision")) return false;
      if (!write_line(peer,fixture("ack"))) return false;
      return acked_future.wait_for(std::chrono::seconds(1))==std::future_status::ready;
    };
    bool passed=run(); ::close(peer); return passed;
  });
  ASSERT_TRUE(dispatcher.start());
  EXPECT_TRUE(server.get()); dispatcher.stop();
  EXPECT_EQ(dispatcher.sent(),2u);
  ASSERT_EQ(acked_future.wait_for(std::chrono::milliseconds(0)),std::future_status::ready);
  EXPECT_EQ(acked_future.get().at("status"),"executed");
}
TEST(CompanionDispatcherV2, KOnlyLegacyGreetingCanBeStoppedWithinBoundWithoutFallback) {
  Listener listener; auto queue=std::make_shared<ipc::ControlQueue>();
  bridge::CompanionConfig config; config.local_port=listener.port;
  std::promise<void> greeted; auto ready=greeted.get_future(); std::atomic<unsigned> received{0};
  auto server=std::async(std::launch::async,[&] {
    int peer=listener.accept(); if (peer<0) return false;
    ::send(peer,"K",1,MSG_NOSIGNAL); greeted.set_value();
    pollfd p{peer,POLLIN,0}; ::poll(&p,1,1500); ::close(peer); return true;
  });
  bridge::CompanionDispatcher dispatcher(config,bridge::AdbController{},"",queue,
    [&](const Json&) { ++received; },[]{});
  dispatcher.start(); ASSERT_EQ(ready.wait_for(std::chrono::seconds(1)),std::future_status::ready);
  auto start=std::chrono::steady_clock::now(); dispatcher.stop();
  EXPECT_LT(std::chrono::steady_clock::now()-start,std::chrono::seconds(1));
  EXPECT_TRUE(server.get()); EXPECT_EQ(received,0u); EXPECT_EQ(dispatcher.sent(),0u);
}
TEST(LegacyDiagnosticsV2, LocalOnlyModeObservesButNeverOpensOrSendsCompetingV1Socket) {
  auto queue=std::make_shared<ipc::InProcessQueue<Verdict>>(2);
  bridge::CompanionConfig config; config.local_only=true;
  bridge::VerdictDispatcher dispatcher(config,bridge::AdbController{},"",queue);
  std::promise<void> observed; auto signal=observed.get_future();
  dispatcher.set_observer([&](const auto&) { observed.set_value(); });
  Verdict verdict; verdict.action=Action::Warn; queue->push(std::move(verdict)); queue->close();
  dispatcher.start(); EXPECT_EQ(signal.wait_for(std::chrono::seconds(1)),std::future_status::ready);
  dispatcher.stop();
  EXPECT_EQ(dispatcher.sent(),0u); EXPECT_EQ(dispatcher.dropped(),0u);
}
