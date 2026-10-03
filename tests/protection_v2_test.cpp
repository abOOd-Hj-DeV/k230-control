#include <fstream>
#include <filesystem>
#include <limits>
#include <future>
#include <thread>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include "../apps/protection_source.hpp"
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
  std::chrono::microseconds clock_offset{0};
  ProtectionSession session{queue,true,[this] { return std::chrono::steady_clock::now()+clock_offset; }};
  Json state=fixture("state");
  std::int64_t seq=2;
  void probes() {
    for (int i=0;i<3;++i) {
      if (i) clock_offset+=std::chrono::milliseconds(500);
      session.observe_capture_pts(9400000+i*500000);
      auto probe=queue->pop(std::chrono::milliseconds(0));
      if (!probe || probe->at("type")!="bind") throw std::runtime_error("probe_missing");
    }
  }
  SessionHarness() {
    session.receive(fixture("hello"));
    auto bind=queue->pop(std::chrono::milliseconds(0));
    if (!bind || bind->at("type")!="bind") throw std::runtime_error("bind_missing");
    probes(); auto bound=fixture("bound"); bound["stream_id"]=session.stream_id(); bound["request_seq"]="4";
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
  void reconnect(bool reboot=false) {
    session.disconnected(); auto hello=fixture("hello"); hello["session_id"]=companion::uuid();
    if (reboot) hello["phone_boot_id"]=companion::uuid();
    session.receive(hello); auto bind=queue->pop(std::chrono::milliseconds(0));
    if (!bind) throw std::runtime_error("bind_missing");
    probes(); auto bound=fixture("bound"); bound["session_id"]=hello.at("session_id"); bound["stream_id"]=session.stream_id(); bound["request_seq"]="4";
    session.receive(bound); state["session_id"]=hello.at("session_id"); seq=2;
  }
  Json ack(const Json& command,const std::string& status="executed") {
    auto result=fixture("ack");
    for (auto key:{"session_id","stream_id","event_id","action_revision","screen_token","requested_stage"}) result[key]=command.at(key);
    result["seq"]=std::to_string(++seq); result["request_seq"]=command.at("seq");
    result["status"]=status; result["executed_stage"]=status=="executed"?command.at("requested_stage"):Json(0);
    result["executed_action"]=status=="executed"?command.at("requested_action"):Json("none");
    result["executed_at_us"]=status=="executed"?command.at("pts_us"):Json(nullptr);
    result["display_rects"]=Json::array(); result["error"]=status=="executed"?Json(nullptr):Json("action_failed");
    if (status=="executed" && command.at("requested_stage")==1) {
      for (const auto& region:command.at("regions")) result["display_rects"].push_back(companion::rect_json(
        companion::map_rect(companion::rect_from_json(region.at("crop_frame_px")),command.at("frame").at("width"),
          command.at("frame").at("height"),0,{0,0,1080,2400})));
    } else if (status=="executed" && command.at("requested_stage")==2)
      result["display_rects"].push_back(companion::rect_json({0,0,1080,2400}));
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
TEST(CaptureClockV2, ProtectionRequiresPinnedOfficialServerBytesAndVersion) {
  bridge::ScrcpyConfig config;
  const auto root=std::filesystem::path(K230_TEST_UI_MODEL).parent_path().parent_path();
  config.server_jar=(root/"assets"/"scrcpy-server").string(); EXPECT_TRUE(protection_source_verified(config));
  config.server_version="3.3.4"; EXPECT_FALSE(protection_source_verified(config));
  config.server_version="4.0"; config.server_jar=(root/"README.md").string(); EXPECT_FALSE(protection_source_verified(config));
  config.server_jar=(root/"missing-scrcpy-server").string(); EXPECT_FALSE(protection_source_verified(config));
  config.server_jar=root.string(); EXPECT_FALSE(protection_source_verified(config));
}
TEST(CaptureClockV2, CorrectPhoneNanoTimeAcceptsOnlyAfterThreeFreshSamplesAndOneSecond) {
  using Status=companion::CaptureClockVerifier::Status;
  companion::CaptureClockVerifier verifier(10000000);
  EXPECT_EQ(verifier.observe(9900000,10000000),Status::Pending);
  EXPECT_EQ(verifier.observe(10400000,10500000),Status::Pending);
  EXPECT_EQ(verifier.observe(10900000,11000000),Status::Accepted);
}
TEST(CaptureClockV2, WallClockConstantBackwardsFutureBootOffsetAndStaleSamplesReject) {
  using Status=companion::CaptureClockVerifier::Status;
  for (auto invalid:std::vector<std::int64_t>{1791050000000000,10000000,9999999,10550001,95000000,9749999,-1,INT64_MAX}) {
    SCOPED_TRACE(invalid); companion::CaptureClockVerifier verifier(10000000);
    EXPECT_EQ(verifier.observe(10000000,10000000),Status::Pending);
    EXPECT_EQ(verifier.observe(invalid,10500000),Status::Rejected);
    EXPECT_EQ(verifier.observe(11000000,11000000),Status::Rejected);
  }
}
TEST(CaptureClockV2, TooFewShortBatchedDriftingOrTimedOutProbesCannotVerify) {
  using Status=companion::CaptureClockVerifier::Status;
  companion::CaptureClockVerifier few(10000000);
  EXPECT_EQ(few.observe(10000000,10000000),Status::Pending); EXPECT_EQ(few.observe(11000000,11000000),Status::Pending);
  companion::CaptureClockVerifier short_window(10000000);
  for (int i=0;i<8;++i) EXPECT_EQ(short_window.observe(10000000+i*100000,10000000+i*100000),Status::Pending);
  EXPECT_EQ(short_window.observe(10800000,10800000),Status::Rejected);
  companion::CaptureClockVerifier batched(10000000);
  EXPECT_EQ(batched.observe(10000000,10500000),Status::Pending);
  EXPECT_EQ(batched.observe(10500000,10500001),Status::Pending);
  EXPECT_EQ(batched.observe(11000000,10950000),Status::Pending);
  companion::CaptureClockVerifier drift(10000000);
  EXPECT_EQ(drift.observe(10000000,10000000),Status::Pending);
  EXPECT_EQ(drift.observe(10500000,10600000),Status::Pending);
  EXPECT_EQ(drift.observe(11000000,11200000),Status::Rejected);
  companion::CaptureClockVerifier timeout(10000000);
  EXPECT_EQ(timeout.observe(13000001,13000001),Status::Rejected);
  companion::CaptureClockVerifier overflow(INT64_MAX-1100000);
  EXPECT_EQ(overflow.observe(INT64_MAX-1100000,INT64_MAX-1100000),Status::Pending);
  EXPECT_EQ(overflow.observe(INT64_MAX-600000,INT64_MAX-600000),Status::Pending);
  EXPECT_EQ(overflow.observe(INT64_MAX-100000,INT64_MAX-100000),Status::Accepted);
}
TEST(CompanionV2, BindProbesAndPendingBoundHaveStrictShapesAndErrors) {
  auto bind=fixture("bind"); bind["capture_pts_us"]="10000000"; EXPECT_NO_THROW(companion::validate(bind));
  bind.erase("capture_pts_us"); EXPECT_THROW(companion::validate(bind),std::exception);
  bind=fixture("bind"); bind["capture_pts_us"]=10000000; EXPECT_THROW(companion::validate(bind),std::exception);
  auto bound=fixture("bound"); bound["status"]="pending"; EXPECT_NO_THROW(companion::validate(bound));
  bound["error"]="clock_unverified"; EXPECT_THROW(companion::validate(bound),std::exception);
  bound["status"]="rejected"; EXPECT_NO_THROW(companion::validate(bound));
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
  state["protection"]={{"stage",1},{"event_id",decision.at("event_id")},{"action_revision","1"},{"target_screen_token",decision.at("screen_token")},{"applied_at_us","10300000"},{"covered_rects",Json::array({companion::rect_json(rectangles[0])})},{"release_pending",false}};
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
TEST(ProtectionSessionV2, OperatorAssertionCannotReplaceNativeClockAcceptance) {
  for (bool assertion:{false,true}) {
    auto queue=std::make_shared<ipc::ControlQueue>(); ProtectionSession session(queue,assertion);
    session.receive(fixture("hello")); auto initial=queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(initial);
    EXPECT_TRUE(initial->at("capture_pts_us").is_null());
    auto pending=fixture("bound"); pending["stream_id"]=session.stream_id(); pending["status"]="pending";
    session.receive(pending); session.observe_capture_pts(10000000);
    auto probe=queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(probe); EXPECT_EQ(probe->at("capture_pts_us"),"10000000");
    auto state=fixture("state"); state["stream_id"]=session.stream_id(); session.receive(state);
    session.analyze(sample(10000000,.95)); EXPECT_EQ(session.stats().submitted,0u);
    session.observe_capture_pts(10000000); EXPECT_GT(session.stats().rejected,0u);
    auto accepted=fixture("bound"); accepted["stream_id"]=session.stream_id(); accepted["seq"]="3"; accepted["request_seq"]="2";
    session.receive(accepted); EXPECT_EQ(session.stats().submitted,0u);
    EXPECT_FALSE(queue->pop(std::chrono::milliseconds(0)));
  }
}
TEST(ProtectionSessionV2, NativeAcceptedWithoutEnoughSourceProbesCannotAuthorizeDecisions) {
  for (bool assertion:{false,true}) {
    auto queue=std::make_shared<ipc::ControlQueue>(); ProtectionSession session(queue,assertion);
    session.receive(fixture("hello")); ASSERT_TRUE(queue->pop(std::chrono::milliseconds(0)));
    auto bound=fixture("bound"); bound["stream_id"]=session.stream_id(); session.receive(bound);
    auto state=fixture("state"); state["stream_id"]=session.stream_id();
    for (int i=0;i<11;++i) {
      state["seq"]=std::to_string(i+3); state["phone_time_us"]=std::to_string(10000000+i*100000);
      state["screen"]["sampled_at_us"]=state.at("phone_time_us");
      session.receive(state); session.analyze(sample(10000000+i*100000,.95));
    }
    EXPECT_EQ(session.stats().submitted,0u); EXPECT_FALSE(queue->pop(std::chrono::milliseconds(0)));
  }
}
TEST(ProtectionSessionV2, TenAndFiveHzDecodedStreamsVerifyWithoutExhaustingProbeBudget) {
  for (int interval:{100000,200000}) {
    SCOPED_TRACE(interval); auto queue=std::make_shared<ipc::ControlQueue>();
    auto offset=std::chrono::microseconds(0);
    ProtectionSession session(queue,false,[&] { return std::chrono::steady_clock::now()+offset; });
    session.receive(fixture("hello")); auto initial=queue->pop(std::chrono::milliseconds(0));
    ASSERT_TRUE(initial); EXPECT_TRUE(initial->at("capture_pts_us").is_null());
    companion::CaptureClockVerifier verifier(10020000); std::size_t probes=0; std::int64_t last=-1, phone=0;
    for (int elapsed=0;elapsed<=3000000 && verifier.status()!=companion::CaptureClockVerifier::Status::Accepted;elapsed+=interval) {
      offset=std::chrono::microseconds(elapsed); const auto pts=10000000+elapsed;
      session.observe_capture_pts(pts);
      auto probe=queue->pop(std::chrono::milliseconds(0)); if (!probe) continue;
      ++probes; EXPECT_EQ(probe->at("capture_pts_us"),std::to_string(pts)); EXPECT_GT(pts,last); last=pts;
      phone=pts+20000; auto status=verifier.observe(pts,phone);
      ASSERT_NE(status,companion::CaptureClockVerifier::Status::Rejected);
      auto bound=fixture("bound"); bound["stream_id"]=session.stream_id(); bound["seq"]=std::to_string(probes);
      bound["request_seq"]=probe->at("seq"); bound["phone_time_us"]=std::to_string(phone);
      bound["status"]=status==companion::CaptureClockVerifier::Status::Accepted?"accepted":"pending";
      session.receive(bound);
    }
    EXPECT_EQ(verifier.status(),companion::CaptureClockVerifier::Status::Accepted); EXPECT_EQ(probes,3u);
    EXPECT_LE(phone-10020000,1200000);
    auto state=fixture("state"); state["stream_id"]=session.stream_id(); state["seq"]=std::to_string(probes+1);
    state["phone_time_us"]=std::to_string(phone); state["screen"]["sampled_at_us"]=state.at("phone_time_us");
    session.receive(state); EXPECT_EQ(session.stats().rejected,0u);
  }
}
TEST(ProtectionSessionV2, UnsentOrLaterProbeCannotCompleteAnEarlierAcceptedWindow) {
  auto queue=std::make_shared<ipc::ControlQueue>(); auto offset=std::chrono::milliseconds(0);
  ProtectionSession session(queue,false,[&] { return std::chrono::steady_clock::now()+offset; });
  session.receive(fixture("hello")); ASSERT_TRUE(queue->pop(std::chrono::milliseconds(0)));
  for (auto pts:{10000000,10200000,10500000}) {
    session.observe_capture_pts(pts); ASSERT_TRUE(queue->pop(std::chrono::milliseconds(0)));
    if (pts != 10500000) offset+=std::chrono::milliseconds(500);
  }
  session.observe_capture_pts(11000000); EXPECT_FALSE(queue->pop(std::chrono::milliseconds(0)));
  auto bound=fixture("bound"); bound["stream_id"]=session.stream_id(); bound["seq"]="1"; bound["request_seq"]="4";
  session.receive(bound); EXPECT_EQ(session.stats().rejected,1u);
  offset+=std::chrono::milliseconds(500); session.observe_capture_pts(11500000); ASSERT_TRUE(queue->pop(std::chrono::milliseconds(0)));
  bound["seq"]="2"; session.receive(bound); EXPECT_EQ(session.stats().rejected,2u);
  bound["seq"]="3"; bound["request_seq"]="5"; session.receive(bound);
  auto state=fixture("state"); state["stream_id"]=session.stream_id(); state["seq"]="4"; session.receive(state);
  EXPECT_EQ(session.stats().rejected,2u);
}
TEST(ProtectionSessionV2, UnpairedOrUnavailableKeystoreCannotAccumulateInterventionEvidence) {
  for (const auto& health:std::vector<std::pair<std::string,std::string>>{{"keystore","locked"},{"keystore","failed"},
      {"pairing","unpaired"},{"pairing","revoked"},{"pairing","key_lost"}}) {
    SCOPED_TRACE(health.second); SessionHarness harness; harness.state["health"][health.first]=health.second;
    for (int i=0;i<11;++i) harness.analyze(10000000+i*100000,.95);
    EXPECT_EQ(harness.session.stats().submitted,0u); EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
    auto decision=fixture("decision"); decision["stream_id"]=harness.session.stream_id();
    EXPECT_THROW(companion::validate_decision(decision,harness.state,10450000,true),std::exception);
  }
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
      harness.reconnect();
      harness.state["protection"]={{"stage",2},{"event_id",shield->at("event_id")},{"action_revision",shield->at("action_revision")},
        {"target_screen_token",shield->at("screen_token")},{"applied_at_us",shield->at("pts_us")},
        {"covered_rects",Json::array({companion::rect_json({0,0,1080,2400})})},{"release_pending",false}};
    } else harness.session.receive(harness.ack(*shield,status));
    for (int i=0;i<3;++i) harness.analyze(10600000+i*200000);
    auto retry=harness.queue->pop(std::chrono::milliseconds(0));
    if (status=="lost") {
      EXPECT_FALSE(retry); EXPECT_EQ(harness.session.stats().unknown,1u);
    } else {
      EXPECT_FALSE(retry);
      for (int i=0;i<3;++i) harness.analyze(11200000+i*200000,.85);
      retry=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(retry);
      EXPECT_EQ(retry->at("requested_stage"),2);
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
      protection["action_revision"]=first->at("action_revision"); protection["target_screen_token"]=first->at("screen_token");
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
TEST(AgePolicyV2, RepetitionUsesPhoneExecutionTimeAndExcludesCurrentEpisode) {
  const auto current=companion::uuid(); AgePolicy policy; ASSERT_TRUE(policy.set_profile(12,1));
  policy.executed(current,"com.example.viewer",1,70000000);
  policy.executed(current,"com.example.viewer",2,70000100);
  AgeDecision result;
  for (int i=0;i<3;++i) result=policy.evaluate(sample(10000000+i*200000),current,70400000);
  EXPECT_EQ(result.stage,1); EXPECT_FALSE(result.regions.front().repetition);
  policy.reset_evidence();
  for (int i=0;i<3;++i) result=policy.evaluate(sample(11000000+i*200000),companion::uuid(),70400000);
  EXPECT_EQ(result.stage,2); EXPECT_TRUE(result.regions.front().repetition);
  policy.reset_evidence();
  for (int i=0;i<3;++i) result=policy.evaluate(sample(12000000+i*200000),companion::uuid(),130000000);
  EXPECT_EQ(result.stage,1);
}
TEST(ProtectionSessionV2, IndependentCoversInSameEpisodeDoNotManufactureRepetition) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
  auto first=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(first);
  harness.session.receive(harness.ack(*first));
  for (int i=0;i<3;++i) {
    auto batch=sample(10600000+i*200000); batch.regions[0].crop={200,100,100,300}; harness.analyze(batch);
  }
  auto second=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(second);
  EXPECT_EQ(second->at("requested_stage"),1); EXPECT_EQ(second->at("event_id"),first->at("event_id"));
  EXPECT_EQ(second->at("action_revision"),"2");
}
TEST(ProtectionSessionV2, ChangedScreenBlocksOldEventPromotionUntilVerifiedRelease) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
  auto first=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(first);
  harness.session.receive(harness.ack(*first));
  harness.state["screen"]["screen_token"]=companion::uuid();
  harness.state["screen"]["content_epoch"]="2"; harness.state["screen"]["package"]="com.example.other";
  for (int i=0;i<12;++i) {
    auto batch=sample(10600000+i*100000,.95); batch.regions[0].crop={200,100,100,300}; harness.analyze(batch);
  }
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  auto release=fixture("released"); release["stream_id"]=harness.session.stream_id(); release["event_id"]=first->at("event_id");
  release["seq"]=std::to_string(++harness.seq); release["new_screen_token"]=harness.state.at("screen").at("screen_token");
  harness.session.receive(release);
  for (int i=0;i<11;++i) harness.analyze(12000000+i*100000,.95);
  auto home=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(home);
  EXPECT_EQ(home->at("requested_stage"),3); EXPECT_NE(home->at("event_id"),first->at("event_id"));
}
TEST(ProtectionSessionV2, NativeZeroResolvesMasksWithoutErasingSuccessfulRepetitionWithinSameBoot) {
  for (int mode:{0,1,2}) {
    SCOPED_TRACE(mode); SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
    auto first=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(first);
    harness.session.receive(harness.ack(*first));
    if (mode) harness.reconnect(mode==2);
    harness.analyze(13000000,0); harness.analyze(13100000,0);
    for (int i=0;i<3;++i) harness.analyze(13200000+i*200000);
    auto next=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(next);
    EXPECT_NE(next->at("event_id"),first->at("event_id"));
    EXPECT_EQ(next->at("requested_stage"),mode==2?1:2);
    EXPECT_EQ(next->at("reason"),mode==2?"threshold":"repetition");
  }
}
TEST(ProtectionSessionV2, ReusedScreenTokenWithMutatedIdentityDisablesBinding) {
  for (auto key:{"content_epoch","package","window_id","rotation_deg","width","valid_from_us"}) {
    SCOPED_TRACE(key); SessionHarness harness; harness.analyze(10000000);
    if (std::string(key)=="package") harness.state["screen"][key]="com.example.other";
    else if (std::string(key)=="content_epoch") harness.state["screen"][key]="2";
    else if (std::string(key)=="valid_from_us") harness.state["screen"][key]="10000000";
    else harness.state["screen"][key]=std::string(key)=="rotation_deg"?90:100;
    for (int i=0;i<11;++i) harness.analyze(10200000+i*100000,.95);
    EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
    EXPECT_GT(harness.session.stats().rejected,0u);
  }
}
TEST(ProtectionSessionV2, AckTimeAndAppliedRectanglesMustCorrelateBeforeCountingExecution) {
  for (int mode=0;mode<5;++mode) {
    SCOPED_TRACE(mode); SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000);
    auto command=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(command);
    auto ack=harness.ack(*command);
    if (mode==0) ack["executed_at_us"]="10399999";
    if (mode==1) ack["executed_at_us"]="11000000";
    if (mode==2) ack["display_rects"]=Json::array();
    if (mode==3) ack["display_rects"][0]=companion::rect_json({61,300,480,900});
    if (mode==4) ack["request_seq"]="1";
    harness.session.receive(ack); EXPECT_EQ(harness.session.stats().executed,0u);
    EXPECT_EQ(harness.session.stats().rejected,1u);
    harness.session.receive(harness.ack(*command)); EXPECT_EQ(harness.session.stats().executed,1u);
    auto duplicate=harness.ack(*command); duplicate["status"]="duplicate";
    harness.session.receive(duplicate); EXPECT_EQ(harness.session.stats().executed,1u);
  }
}
TEST(ProtectionSessionV2, ReplayedHelloCannotResetAnActiveOrDisconnectedSession) {
  SessionHarness harness; harness.session.receive(fixture("hello"));
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  harness.session.disconnected(); harness.session.receive(fixture("hello"));
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0))); EXPECT_EQ(harness.session.stats().rejected,2u);
}
TEST(ProtectionSessionV2, FirstPostBindNativeZeroClearsOldClaimsAndPendingRemainsUnknown) {
  for (bool reboot:{false,true}) {
    SCOPED_TRACE(reboot); SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000,.85);
    auto shield=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(shield);
    harness.reconnect(reboot);
    if (reboot) harness.state["screen"]["valid_from_us"]="0";
    auto first=reboot?1000000:11000000;
    harness.analyze(first,.95); EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
    auto settled=first+100000;
    for (int i=0;i<12;++i) harness.analyze(settled+i*100000,.95);
    auto home=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(home);
    EXPECT_EQ(home->at("requested_stage"),3); EXPECT_EQ(home->at("action_revision"),"1");
    EXPECT_NE(home->at("event_id"),shield->at("event_id"));
    EXPECT_EQ(harness.session.stats().unknown,1u); EXPECT_EQ(harness.session.stats().executed,0u);
  }
}
TEST(CompanionV2, ImpossibleNativeProtectionGeometryAndExecutionFieldsAreRejected) {
  auto state=fixture("state"); state["protection"]["covered_rects"]=fixture("ack").at("display_rects");
  EXPECT_THROW(companion::validate(state),std::exception);
  state=fixture("state"); state["protection"]["stage"]=2; state["protection"]["event_id"]=companion::uuid();
  state["protection"]["action_revision"]="1"; state["protection"]["target_screen_token"]=companion::uuid();
  state["protection"]["applied_at_us"]="10400000";
  state["protection"]["covered_rects"]=Json::array({companion::rect_json({0,0,1080,2399})});
  EXPECT_THROW(companion::validate(state),std::exception);
  auto ack=fixture("ack"); ack["status"]="failed"; ack["executed_stage"]=0; ack["executed_action"]="none";
  ack["display_rects"]=Json::array(); ack["error"]="action_failed";
  EXPECT_THROW(companion::validate(ack),std::exception);
}
TEST(ProtectionSessionV2, ZeroStateBeforeExpiryNeverClearsAnInflightCommand) {
  SessionHarness harness;
  for (int i=0;i<3;++i) harness.analyze(10000000+i*200000,.85);
  auto shield=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(shield);
  for (int i=0;i<8;++i) harness.analyze(10500000+i*50000,.95);
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  EXPECT_EQ(harness.session.stats().unknown,0u); EXPECT_EQ(harness.session.stats().executed,0u);
  harness.session.receive(harness.ack(*shield)); EXPECT_EQ(harness.session.stats().executed,1u);
}
TEST(ProtectionSessionV2, LostReleaseOrExpiredAckCanRecoverFromReliableZeroStateWithoutReconnect) {
  for (bool executed:{false,true}) {
    SCOPED_TRACE(executed); SessionHarness harness;
    for (int i=0;i<3;++i) harness.analyze(10000000+i*200000,.85);
    auto shield=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(shield);
    if (executed) harness.session.receive(harness.ack(*shield));
    for (int i=0;i<12;++i) harness.analyze(13000000+i*100000,.95);
    auto home=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(home);
    EXPECT_EQ(home->at("requested_stage"),3); EXPECT_NE(home->at("event_id"),shield->at("event_id"));
    EXPECT_EQ(harness.session.stats().unknown,executed?0u:1u);
    EXPECT_EQ(harness.session.stats().executed,executed?1u:0u);
  }
}
TEST(ProtectionSessionV2, RestoredActiveCoverRevisionAllowsIndependentPornHomeAtNextRevision) {
  SessionHarness harness; harness.reconnect();
  auto& protection=harness.state["protection"];
  const auto event=companion::uuid();
  protection={{"stage",1},{"event_id",event},{"action_revision","7"},{"target_screen_token",harness.state.at("screen").at("screen_token")},
    {"applied_at_us","9900000"},{"covered_rects",Json::array({companion::rect_json({60,300,480,900})})},{"release_pending",false}};
  for (int i=0;i<11;++i) {
    auto batch=sample(10000000+i*100000,.95); batch.regions[0].crop={200,100,100,300}; harness.analyze(batch);
  }
  auto home=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(home);
  EXPECT_EQ(home->at("requested_stage"),3); EXPECT_EQ(home->at("event_id"),event); EXPECT_EQ(home->at("action_revision"),"8");
  harness.session.receive(harness.ack(*home)); EXPECT_EQ(harness.session.stats().executed,1u);
}
TEST(ProtectionSessionV2, RestoredProtectionKeepsOriginalTargetForVerifiedReleaseAfterContentChanges) {
  SessionHarness harness; harness.reconnect();
  auto original=companion::uuid(), event=companion::uuid();
  harness.state["protection"]={{"stage",1},{"event_id",event},{"action_revision","7"},{"target_screen_token",original},
    {"applied_at_us","9900000"},{"covered_rects",Json::array({companion::rect_json({60,300,480,900})})},{"release_pending",false}};
  for (int i=0;i<11;++i) {
    auto batch=sample(10000000+i*100000,.95); batch.regions[0].crop={200,100,100,300}; harness.analyze(batch);
  }
  EXPECT_FALSE(harness.queue->pop(std::chrono::milliseconds(0)));
  auto release=fixture("released"); release["stream_id"]=harness.session.stream_id(); release["session_id"]=harness.state.at("session_id");
  release["event_id"]=event; release["action_revision"]="7"; release["previous_screen_token"]=harness.state.at("screen").at("screen_token");
  release["seq"]=std::to_string(++harness.seq); harness.session.receive(release); EXPECT_EQ(harness.session.stats().rejected,1u);
  release["previous_screen_token"]=original; release["seq"]=std::to_string(++harness.seq); harness.session.receive(release);
  harness.state["protection"]=fixture("state").at("protection");
  for (int i=0;i<11;++i) harness.analyze(12000000+i*100000,.95);
  auto home=harness.queue->pop(std::chrono::milliseconds(0)); ASSERT_TRUE(home);
  EXPECT_EQ(home->at("requested_stage"),3); EXPECT_NE(home->at("event_id"),event); EXPECT_EQ(home->at("action_revision"),"1");
}
TEST(CompanionV2, NativeRecoveryFieldsAreRequiredAndNullableExactlyWhenInactive) {
  auto inactive=fixture("state");
  for (auto key:{"action_revision","target_screen_token"}) {
    auto bad=inactive; bad["protection"].erase(key); EXPECT_THROW(companion::validate(bad),std::exception);
    bad=inactive; bad["protection"][key]=std::string(key)=="action_revision"?Json("1"):Json(companion::uuid());
    EXPECT_THROW(companion::validate(bad),std::exception);
  }
  auto active=inactive; active["protection"]={{"stage",1},{"event_id",companion::uuid()},{"action_revision","7"},
    {"target_screen_token",companion::uuid()},{"applied_at_us","9900000"},{"covered_rects",Json::array({companion::rect_json({60,300,480,900})})},{"release_pending",false}};
  EXPECT_NO_THROW(companion::validate(active));
  for (auto key:{"action_revision","target_screen_token"}) {
    auto bad=active; bad["protection"][key]=nullptr; EXPECT_THROW(companion::validate(bad),std::exception);
  }
  for (auto revision:{"0","01","9223372036854775808"}) {
    auto bad=active; bad["protection"]["action_revision"]=revision; EXPECT_THROW(companion::validate(bad),std::exception);
  }
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
TEST(CompanionDispatcherV2, RepeatedRawPtsBindsNegotiatePendingThenVerifiedOverRealLoopback) {
  Listener listener; auto queue=std::make_shared<ipc::ControlQueue>();
  std::atomic<std::int64_t> offset{0};
  ProtectionSession session(queue,false,[&] { return std::chrono::steady_clock::now()+std::chrono::microseconds(offset.load()); });
  std::promise<void> hello_received, state_received; auto hello_future=hello_received.get_future(), state_future=state_received.get_future();
  bridge::CompanionConfig config; config.local_port=listener.port;
  bridge::CompanionDispatcher dispatcher(config,bridge::AdbController{},"",queue,[&](const Json& record) {
    session.receive(record);
    if (record.at("type")=="hello") hello_received.set_value();
    if (record.at("type")=="state") state_received.set_value();
  },[&] { session.disconnected(); });
  auto server=std::async(std::launch::async,[&] {
    int peer=listener.accept(); if (peer<0) return false;
    auto run=[&] {
      if (::send(peer,"K",1,MSG_NOSIGNAL)!=1 || !write_line(peer,fixture("hello"))) return false;
      auto initial=read_line(peer); if (!initial || !initial->at("capture_pts_us").is_null()) return false;
      companion::CaptureClockVerifier verifier(10020000);
      for (int i=0;i<3;++i) {
        auto probe=read_line(peer); if (!probe || probe->at("type")!="bind" || probe->at("stream_id")!=session.stream_id()) return false;
        auto status=verifier.observe(companion::decimal(probe->at("capture_pts_us")),10020000+i*500000);
        auto bound=fixture("bound"); bound["stream_id"]=session.stream_id(); bound["request_seq"]=probe->at("seq"); bound["seq"]=std::to_string(i+1);
        bound["status"]=status==companion::CaptureClockVerifier::Status::Accepted?"accepted":"pending";
        if (status==companion::CaptureClockVerifier::Status::Rejected || !write_line(peer,bound)) return false;
      }
      auto state=fixture("state"); state["stream_id"]=session.stream_id(); state["seq"]="4";
      return verifier.status()==companion::CaptureClockVerifier::Status::Accepted && write_line(peer,state);
    };
    auto ok=run(); ::close(peer); return ok;
  });
  dispatcher.start(); auto hello=hello_future.wait_for(std::chrono::seconds(2))==std::future_status::ready;
  if (hello) for (int i=0;i<3;++i) {
    if (i) offset+=500000;
    session.observe_capture_pts(10000000+i*500000);
  }
  auto state=state_future.wait_for(std::chrono::seconds(2))==std::future_status::ready;
  dispatcher.stop(); EXPECT_TRUE(hello); EXPECT_TRUE(state); EXPECT_TRUE(server.get()); EXPECT_EQ(dispatcher.sent(),4u);
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
