#include "config_codec.h"
#include "macro_runner.h"
#include "keys.h"
#include "connection.h"
#include "access.h"
#include "ble_reconnect.h"
#include "network_state.h"
#include "ota_release.h"
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace sw;
namespace {
int passed = 0;
bool transportReady = true, transportSuccess = true;
bool keyboardSubscribed = true, mediaSubscribed = true;
std::vector<std::string> reports;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void test(const char* name, const std::function<void()>& fn) {
  try { fn(); ++passed; std::cout << "PASS " << name << '\n'; }
  catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; std::exit(1); }
}
JsonDocument releaseFixture() {
  JsonDocument doc;
  doc["tag_name"] = "v1.10.0";
  doc["draft"] = false;
  doc["prerelease"] = false;
  auto asset = doc["assets"].to<JsonArray>().add<JsonObject>();
  asset["name"] = kFirmwareAsset;
  asset["state"] = "uploaded";
  asset["id"] = uint64_t{4294967300};
  asset["size"] = 1540000;
  asset["digest"] = "sha256:" + std::string(64, 'a');
  return doc;
}
struct Harness {
  Config cfg = defaults(); GestureEngine engine;
  uint32_t now = 0;
  std::vector<std::pair<Gesture,std::string>> events;
  void feed(int candidate, uint32_t duration) {
    for (uint32_t t = 0; t < duration; t += 5, now += 5)
      engine.update(now, candidate, cfg, [&](int, Gesture g, const std::string& a) { if(a!="none") events.emplace_back(g,a); });
  }
  void idle(uint32_t duration=50) { feed(kIdle,duration); }
};
}
namespace sw {
bool hidReady() { return hidKeyboardReady() || hidMediaReady(); }
bool hidKeyboardReady() { return transportReady && keyboardSubscribed; }
bool hidMediaReady() { return transportReady && mediaSubscribed; }
bool sendKey(uint8_t key,uint8_t modifiers) { reports.push_back(key ? "key:"+std::to_string(key)+":"+std::to_string(modifiers) : "key-release"); return hidKeyboardReady()&&transportSuccess; }
bool sendMedia(uint16_t usage) { reports.push_back(usage ? "media:"+std::to_string(usage) : "media-release"); return hidMediaReady()&&transportSuccess; }
void releaseHid() { reports.push_back("release"); }
}
int main(int argc,char** argv) {
  if(argc>1&&std::string(argv[1])=="--fixture") { JsonDocument doc;toJson(defaults(),doc.to<JsonObject>());std::string json;serializeJson(doc,json);std::cout<<json;return 0; }
  const std::string argument = argc>1 ? argv[1] : "";
  if(argument=="--validate" || argument=="--patch") {
    std::string data((std::istreambuf_iterator<char>(std::cin)),std::istreambuf_iterator<char>());
    JsonDocument d; Config c; std::string error;
    bool ok=!deserializeJson(d,data);
    if (ok && argument=="--patch") {
      Config current;
      ok=fromJson(d["current"].as<JsonVariantConst>(),current,error) &&
        patchConfig(d["changes"].as<JsonObjectConst>(),current,c,error);
    } else if (ok) {
      ok=fromJson(d.as<JsonVariantConst>(),c,error);
    }
    JsonDocument out;
    if(ok)toJson(c,out.to<JsonObject>());else out["error"]=error.empty()?"invalid JSON":error;
    std::string json;serializeJson(out,json);std::cout<<json;return ok?0:1;
  }
  test("Wi-Fi waits for disconnect before beginning a new connection",[]{NetworkSchedule n;n.configure(0,true);require(n.update(0,true)==NetworkAction::None,"old IP accepted");require(n.phase()==NetworkPhase::Settling,"settle phase lost");require(n.update(299,false)==NetworkAction::None,"begin too early");require(n.update(300,false)==NetworkAction::Begin,"begin missing");});
  test("Wi-Fi association and DHCP are not interrupted by 15-second retries",[]{NetworkSchedule n;n.configure(0,true);n.update(300,false);require(n.update(15000,false)==NetworkAction::None,"interrupted attempt");require(n.update(30299,false)==NetworkAction::None,"attempt ended early");require(n.update(30300,false)==NetworkAction::Disconnect,"timeout missing");require(n.update(30599,false)==NetworkAction::None,"retry before disconnect settled");require(n.update(30600,false)==NetworkAction::Begin,"retry missing");});
  test("connected Wi-Fi is not restarted",[]{NetworkSchedule n;n.configure(0,true);n.update(300,false);n.update(2000,true);require(n.phase()==NetworkPhase::Connected,"no connected state");require(n.update(3600000,true)==NetworkAction::None,"connected network restarted");});
  test("Wi-Fi reconnects after losing its address",[]{
    NetworkSchedule n;
    n.configure(0,true);
    n.update(300,false);
    n.update(2000,true);
    require(n.update(4000,false)==NetworkAction::Disconnect,"lost network not reset");
    require(n.update(4299,false)==NetworkAction::None,"reconnect started early");
    require(n.update(4300,false)==NetworkAction::Begin,"did not reconnect");
    n.update(5000,true);
    require(n.phase()==NetworkPhase::Connected,"did not recover");
  });
  test("clearing Wi-Fi disables all connection attempts",[]{NetworkSchedule n;n.configure(0,true);n.update(300,false);n.configure(500,false);require(n.update(100000,false)==NetworkAction::None,"disabled network retried");});
  test("Wi-Fi connection timing survives millis wrap",[]{NetworkSchedule n;n.configure(UINT32_MAX-200,true);require(n.update(98,false)==NetworkAction::None,"wrap started early");require(n.update(99,false)==NetworkAction::Begin,"wrap begin missing");require(n.update(30099,false)==NetworkAction::Disconnect,"wrap timeout missing");});
  test("BLE starts fast advertising at boot and after each disconnect", [] {
    BleReconnectSchedule schedule;
    require(schedule.fastAdvertising(), "boot did not request fast advertising");
    schedule.advertisingStarted();
    require(!schedule.fastAdvertising(), "advertising timeout would request a second fast window");
    schedule.disconnected();
    require(schedule.fastAdvertising(), "disconnect did not restart fast advertising");
  });
  test("BLE refreshes services before bounded reconnects with increasing waits", [] {
    BleReconnectSchedule schedule;
    using Action = BleRecoveryAction;
    using Phase = BleLinkPhase;
    require(schedule.update(0, Phase::AwaitingReports) == Action::None, "first attempt too early");
    require(schedule.update(2999, Phase::AwaitingReports) == Action::None, "reports did not get time to restore");
    require(schedule.update(3000, Phase::AwaitingReports) == Action::RefreshServices, "did not refresh services");
    require(schedule.update(12999, Phase::AwaitingReports) == Action::None, "refresh did not get a full timeout");
    require(schedule.update(13000, Phase::AwaitingReports) == Action::Reconnect && schedule.retryUsed(), "stalled link was not retried");
    schedule.disconnected();
    require(schedule.update(14000, Phase::AwaitingReports) == Action::None, "retried on connection");
    require(schedule.update(17000, Phase::AwaitingReports) == Action::RefreshServices, "second link did not get a refresh");
    require(schedule.update(36999, Phase::AwaitingReports) == Action::None, "second attempt ignored the backoff");
    require(schedule.update(37000, Phase::AwaitingReports) == Action::Reconnect && !schedule.retryExhausted(), "missing second retry");
    schedule.disconnected();
    schedule.update(38000, Phase::AwaitingReports);
    require(schedule.update(41000, Phase::AwaitingReports) == Action::RefreshServices, "third link did not get a refresh");
    require(schedule.update(70999, Phase::AwaitingReports) == Action::None, "third attempt ignored the backoff");
    require(schedule.update(71000, Phase::AwaitingReports) == Action::Reconnect && schedule.retryExhausted(), "retry limit not reached");
    schedule.disconnected();
    require(schedule.update(90000, Phase::AwaitingReports) == Action::None && schedule.update(120000, Phase::AwaitingReports) == Action::None, "entered a reconnect loop");
    require(schedule.update(120001, Phase::Ready) == Action::None && !schedule.retryUsed(), "healthy link did not reset recovery");
    require(schedule.update(130000, Phase::AwaitingReports) == Action::None, "healthy reports did not clear the wait timer");
    require(schedule.update(133000, Phase::AwaitingReports) == Action::RefreshServices, "new outage could not refresh services");
    schedule.resetRecovery();
    require(schedule.update(140000, Phase::AwaitingReports) == Action::None, "manual reset kept the old wait timer");
    require(schedule.update(143000, Phase::AwaitingReports) == Action::RefreshServices, "manual reset kept the old recovery step");
  });
  test("BLE can recover a stalled encryption and allows a slow bonded iPad to finish securing the link", [] {
    BleReconnectSchedule schedule;
    using Action = BleRecoveryAction;
    using Phase = BleLinkPhase;
    schedule.update(0, Phase::Encrypting);
    require(schedule.update(31440, Phase::Encrypting) == Action::None, "recovery interrupts the observed encryption delay");
    require(schedule.update(34999, Phase::Encrypting) == Action::None, "encryption timeout fired early");
    require(schedule.update(35000, Phase::Encrypting) == Action::Reconnect, "stalled encryption left the link unusable");
    schedule.disconnected();
    schedule.update(36000, Phase::Encrypting);
    require(schedule.update(68000, Phase::AwaitingReports) == Action::None, "encryption consumed the report wait");
    require(schedule.update(71000, Phase::AwaitingReports) == Action::RefreshServices, "report recovery did not follow encryption");
    require(schedule.update(71100, Phase::Ready) == Action::None && !schedule.retryUsed(), "fully ready link did not reset retries");
  });
  test("BLE stalled link timeout survives millis wrap", [] {
    BleReconnectSchedule schedule;
    constexpr uint32_t start = UINT32_MAX - 200;
    require(schedule.update(start, BleLinkPhase::AwaitingReports) == BleRecoveryAction::None, "started recovery before wrap");
    require(schedule.update(2798, BleLinkPhase::AwaitingReports) == BleRecoveryAction::None, "wrapped timeout fired early");
    require(schedule.update(2799, BleLinkPhase::AwaitingReports) == BleRecoveryAction::RefreshServices, "wrapped timeout did not fire");
  });
  test("all local clients can read configuration without login",[]{require(checkAccess(true,false,false),"local read rejected");require(checkAccess(true,true,true),"local write rejected");});
  test("local writes still need the automatic CSRF token",[]{require(!checkAccess(true,true,false),"write without token accepted");require(checkAccess(true,false,false),"read needs token");});
  test("foreign hostnames are rejected for reads and writes",[]{require(!checkAccess(false,false,false),"foreign read accepted");require(!checkAccess(false,true,true),"token bypassed host restriction");});
  test("host validation accepts the local IP and mDNS name only",[]{require(canonicalLocalHost("192.168.4.1","192.168.4.1"),"AP IP rejected");require(canonicalLocalHost("192.168.1.20:80","192.168.1.20"),"LAN IP rejected");require(canonicalLocalHost("steering-sw.local","192.168.1.20"),"mDNS rejected");require(!canonicalLocalHost("example.com","192.168.4.1"),"foreign hostname accepted");require(!canonicalLocalHost("192.168.4.1.example.com","192.168.4.1"),"suffix accepted");require(!canonicalLocalHost("192.168.4.1","192.168.1.20"),"wrong interface accepted");});
  test("setup button ignores startup hold until released",[]{SetupButton b;require(!b.update(0,true)&&!b.update(4000,true),"startup hold accepted");b.update(4100,false);require(!b.update(4200,true),"pressed early");require(b.update(7200,true),"long press not detected");});
  test("setup button triggers once at three seconds and rearms on release",[]{SetupButton b;b.update(0,false);b.update(10,true);require(!b.update(3009,true),"early trigger");require(b.update(3010,true),"missing trigger");require(!b.update(9000,true),"repeated trigger");b.update(9010,false);b.update(9020,true);require(b.update(12020,true),"did not rearm");});
  test("setup button short press does not open Wi-Fi",[]{SetupButton b;b.update(0,false);b.update(10,true);require(!b.update(2000,true),"short trigger");b.update(2010,false);b.update(2500,true);require(!b.update(4500,true),"short presses combined");});
  test("setup button duration handles millis wrap",[]{SetupButton b;b.update(UINT32_MAX-100,false);b.update(UINT32_MAX-50,true);require(!b.update(100,true),"wrap early");require(b.update(3000,true),"wrap missed");});
  test("OTA selects a newer stable release using numeric version ordering",[]{
    auto doc = releaseFixture(); FirmwareRelease release;
    const auto view = doc.as<JsonVariantConst>();
    require(readFirmwareRelease(view,"1.9.9",0x180000,release)==ReleaseResult::Available,"newer release rejected");
    require(std::string_view(release.version)=="1.10.0" && release.assetId==4294967300 && release.digest[31]==0xaa,"bad asset data");
    require(readFirmwareRelease(view,"1.10.0",0x180000,release)==ReleaseResult::Current,"same version offered");
    require(readFirmwareRelease(view,"2.0.0",0x180000,release)==ReleaseResult::Current,"downgrade offered");
  });
  test("OTA rejects unofficial versions, wrong assets, oversized binaries and bad digests",[]{
    FirmwareRelease release;
    for (const char* tag : {"v1.0.1-rc1", "v1.01.1", "v1.0.1.9", "1.0.1"}) {
      auto doc = releaseFixture(); doc["tag_name"] = tag;
      require(readFirmwareRelease(doc.as<JsonVariantConst>(),"1.0.0",0x180000,release)==ReleaseResult::Invalid,"malformed tag accepted");
    }
    auto doc = releaseFixture();
    require(readFirmwareRelease(doc.as<JsonVariantConst>(),"1.0.0",0x170000,release)==ReleaseResult::TooLarge,"oversized binary accepted");
    doc["prerelease"] = true;
    require(readFirmwareRelease(doc.as<JsonVariantConst>(),"1.0.0",0x180000,release)==ReleaseResult::Invalid,"prerelease accepted");
    doc["prerelease"] = false;
    doc["assets"][0]["name"] = "bootloader.bin";
    require(readFirmwareRelease(doc.as<JsonVariantConst>(),"1.0.0",0x180000,release)==ReleaseResult::Invalid,"wrong asset accepted");
    doc["assets"][0]["name"] = kFirmwareAsset;
    doc["assets"][0]["digest"] = "sha256:" + std::string(64, 'z');
    require(readFirmwareRelease(doc.as<JsonVariantConst>(),"1.0.0",0x180000,release)==ReleaseResult::Invalid,"invalid SHA-256 accepted");
  });
  test("OTA only accepts HTTPS redirects to known GitHub asset hosts",[]{
    require(isGithubAssetRedirect("https://release-assets.githubusercontent.com/asset?id=123"),"valid asset redirect rejected");
    for (const char* url : {"http://release-assets.githubusercontent.com/a", "https://release-assets.githubusercontent.com@elsewhere.test/a",
                            "https://release-assets.githubusercontent.com.elsewhere.test/a", "https://elsewhere.test/a", "https://release-assets.githubusercontent.com/a#fragment"}) {
      require(!isGithubAssetRedirect(url),"unsafe redirect accepted");
    }
    require(isGithubToken("github_pat_fixture123") && !isGithubToken("ghp_fixture123") && !isGithubToken("github_pat_abc\r\n"),
            "token format validation failed");
  });
  test("OTA reserves enough send buffer for a signed GitHub download URL",[]{
    require(githubRequestTxBufferSize(80)==512,"short URLs grew the default buffer");
    const std::string url = "https://release-assets.githubusercontent.com/path?sig=" + std::string(900, 'a');
    const auto requestBytes = std::string("GET ").size() + url.size() + std::string(" HTTP/1.1\r\n").size();
    require(isGithubAssetRedirect(url) && githubRequestTxBufferSize(url.size())>requestBytes,"signed URL would overflow the send buffer");
  });
  test("reference preset is valid and output is disabled",[]{auto c=defaults();require(validate(c).empty(),"invalid defaults");require(!c.outputEnabled,"unexpected output");require(c.buttons.size()==9,"button count");});
  test("JSON round trip preserves configuration",[]{auto c=defaults();c.name="別の車";c.channels[0].pullup=470;JsonDocument d;toJson(c,d.to<JsonObject>());Config out;std::string err;require(fromJson(d.as<JsonVariantConst>(),out,err),err.c_str());require(out.name==c.name&&out.channels[0].pullup==470,"lost fields");});
  test("Bluetooth settings preserve defaults for old backups and validate stored names",[]{
    auto c=defaults();
    JsonDocument d;
    toJson(c,d.to<JsonObject>());
    Config out;
    std::string error;
    auto decode=[&]{return fromJson(d.as<JsonVariantConst>(),out,error);};
    d.remove("ble");
    require(decode() && out.ble.enabled && out.ble.name==c.ble.name,"legacy defaults lost");
    d["ble"]["name"]="運転席 Steering";
    d["ble"]["enabled"]=false;
    require(decode() && !out.ble.enabled && out.ble.name=="運転席 Steering","changed Bluetooth settings not read");
    d["ble"]["name"]=std::string(kMaxBleNameBytes+1,'a');
    require(!decode(),"oversized advertising name accepted");
    d["ble"]["name"]="bad\nname";
    require(!decode(),"control character accepted");
    d["ble"]["name"]="   ";
    require(!decode(),"blank advertising name accepted");
    d["ble"]=nullptr;
    require(!decode(),"null Bluetooth settings accepted");
  });
  test("legacy input units and thresholds survive loading",[]{
    auto c=defaults();
    c.name="クロスビー / 汎用";
    c.channels[0].mode="adc";
    c.channels[0].idleCenter=3890;
    c.channels[1].mode="ohm";
    c.channels[1].idleCenter=5187;
    c.buttons[0].center=570;
    c.buttons[3].center=57.5;
    c.buttons[3].tolerance=5;
    JsonDocument d;
    toJson(c,d.to<JsonObject>());
    Config out;
    std::string error;
    const bool decoded=fromJson(d.as<JsonVariantConst>(),out,error);
    require(decoded,error.c_str());
    require(out.name==defaults().name,"old preset label was not updated");
    require(out.channels[0].mode=="adc"&&out.channels[0].idleCenter==3890&&out.buttons[0].center==570,"ADC thresholds changed");
    require(out.channels[1].mode=="ohm"&&out.channels[1].idleCenter==5187&&out.buttons[3].center==57.5&&out.buttons[3].tolerance==5,"resistance thresholds changed");
  });
  test("JSON rejects bad types, unknown versions, null and incomplete data",[]{
    for(int mutation=0;mutation<5;++mutation){JsonDocument d;toJson(defaults(),d.to<JsonObject>());if(mutation==0)d["timing"]["longMs"]="800";if(mutation==1)d["version"]=2;if(mutation==2)d["channels"][0]["pullup"]=nullptr;if(mutation==3)d.remove("buttons");if(mutation==4)d["timing"]["longMs"]=800.5;Config out;std::string err;require(!fromJson(d.as<JsonVariantConst>(),out,err),"accepted invalid JSON");}
  });
  test("JSON rejects embedded NUL in identifiers",[]{JsonDocument d;toJson(defaults(),d.to<JsonObject>());std::string bad("x\0y",3);d["buttons"][0]["id"]=bad;Config out;std::string err;require(!fromJson(d.as<JsonVariantConst>(),out,err),"accepted NUL");});
  test("JSON excludes secrets and requires preset macro definitions",[]{JsonDocument d;toJson(defaults(),d.to<JsonObject>());require(d["passcode"].isNull()&&d["wifiPassword"].isNull()&&d["adminPassword"].isNull(),"secret field");d["macros"].as<JsonArray>().remove(0);Config out;std::string err;require(!fromJson(d.as<JsonVariantConst>(),out,err),"missing macro accepted");});
  test("same-channel overlap rejected, cross-channel centers allowed",[]{auto c=defaults();c.buttons[1].center=c.buttons[0].center;require(!validate(c).empty(),"overlap accepted");c=defaults();c.buttons[0].center=c.buttons[3].center;require(validate(c).empty(),"cross channel rejected");});
  test("idle overlap, duplicate IDs, invalid channels and nonfinite values rejected",[]{auto c=defaults();c.buttons[0].center=c.channels[0].idleCenter;require(!validate(c).empty(),"idle overlap");c=defaults();c.buttons[0].id=c.buttons[1].id;require(!validate(c).empty(),"duplicate id");c=defaults();c.buttons[0].channel=2;require(!validate(c).empty(),"channel");c=defaults();c.channels[0].pullup=NAN;require(!validate(c).empty(),"NaN");});
  test("16 buttons per channel enforced",[]{auto c=defaults();c.buttons.clear();for(int i=0;i<17;++i){Button b;b.id="b"+std::to_string(i);b.name=b.id;b.center=100+i*100;b.tolerance=10;c.buttons.push_back(b);}require(!validate(c).empty(),"17 buttons accepted");c.buttons.back().channel=1;require(validate(c).empty(),"16 buttons rejected");});
  test("passcode cannot bind to short or double tap",[]{auto c=defaults();c.buttons[0].shortAction="passcode";require(!validate(c).empty(),"short accepted");c.buttons[0].shortAction="none";c.buttons[0].doubleAction="passcode";require(!validate(c).empty(),"double accepted");});
  test("volume buttons cannot ambiguously bind double or long actions",[]{auto c=defaults();c.buttons[4].longAction="maps";require(!validate(c).empty(),"ambiguous volume");});
  test("macros reject unsupported text, keys, references and excessive duration",[]{auto c=defaults();Step s;s.type="text";s.text="日本語";c.macros[0].steps={s};require(!validate(c).empty(),"unicode accepted");s.type="chord";s.key=255;c.macros[0].steps={s};require(!validate(c).empty(),"key accepted");s.type="wait";s.ms=10000;c.macros[0].steps={s,s,s,s};require(!validate(c).empty(),"long macro accepted");c=defaults();c.buttons[0].shortAction="macro:missing";require(!validate(c).empty(),"bad reference");});
  test("ohm formula and changing circuit constants",[]{require(std::abs(resistance(1650,3300,330)-330)<.01,"divider");require(std::abs(resistance(1650,3300,470)-470)<.01,"pullup");require(std::isnan(resistance(3300,3300,330)),"zero divisor");require(std::isnan(resistance(3100,3300,330,true)),"saturation");});
  test("voltage classification uses millivolts with the saved hysteresis",[]{
    auto c=defaults(); const auto& b=c.buttons[0]; const auto& ch=c.channels[0];
    require(readingValue(ch,570,b.center,57.6)==b.center,"incorrect voltage units");
    require(classify(c,0,b.center)==0,"center");
    require(classify(c,0,b.center+b.tolerance+1,0)==0,"hysteresis");
    require(classify(c,0,b.center+b.tolerance+ch.hysteresis+10,0)==kUnknown,"gap");
    require(classify(c,0,ch.idleCenter)==kIdle,"idle");
    require(classify(c,0,NAN,0)==kUnknown,"invalid retained");
  });
  test("resistance mode uses independently editable ohm ranges",[]{auto c=defaults();c.channels[0].mode="ohm";c.channels[0].idleCenter=5211;c.channels[0].idleTolerance=150;c.buttons[0].center=57.6;c.buttons[0].tolerance=5;c.buttons[1].center=243.3;c.buttons[1].tolerance=10;c.buttons[2].center=753;c.buttons[2].tolerance=20;require(validate(c).empty(),"ohm validation");require(classify(c,0,58)==0,"ohm match");require(classify(c,0,5211)==kIdle,"ohm idle");});
  test("boot held button cannot trigger before neutral",[]{Harness h;h.feed(0,1500);h.idle();require(h.events.empty(),"boot press triggered");h.feed(0,100);h.idle();require(h.events.size()==1,"normal press missing");});
  test("short glitches do not trigger",[]{Harness h;h.idle();h.feed(0,15);h.idle(500);require(h.events.empty(),"glitch triggered");});
  test("short tap sends once on release",[]{Harness h;h.idle();h.feed(0,100);h.idle(1000);require(h.events.size()==1&&h.events[0].second=="switch_app","tap");});
  test("single MODE tap waits for double-click interval",[]{Harness h;h.idle();h.feed(6,100);h.idle(100);require(h.events.empty(),"single early");h.idle(300);require(h.events.size()==1&&h.events[0].second=="home","single missing");});
  test("MODE double tap emits only double action",[]{Harness h;h.idle();h.feed(6,100);h.idle(100);h.feed(6,100);h.idle(500);require(h.events.size()==1&&h.events[0].second=="app_list","double");});
  test("second press may extend past double interval without an extra single",[]{Harness h;h.idle();h.feed(6,100);h.idle(100);h.feed(6,400);h.idle(500);require(h.events.size()==1&&h.events[0].second=="app_list","held double");});
  test("long press suppresses both short and double actions",[]{Harness h;h.idle();h.feed(6,1500);h.idle(500);require(h.events.size()==1&&h.events[0].first==Gesture::Long&&h.events[0].second=="siri","long");});
  test("second long press suppresses pending first short",[]{Harness h;h.idle();h.feed(6,100);h.idle(100);h.feed(6,1000);h.idle(500);require(h.events.size()==1&&h.events[0].second=="siri","second long");});
  test("volume initial response and repeat cadence",[]{Harness h;h.idle();h.feed(4,440);require(h.events.size()==1,"early repeat");h.feed(4,150);require(h.events.size()==3,"repeat count");h.idle(500);require(h.events.size()==3,"repeat after release");});
  test("invalid samples cancel pending input rather than generating a tap",[]{Harness h;h.idle();h.feed(0,100);h.feed(kUnknown,200);h.idle(500);require(h.events.empty(),"invalid release generated action");});
  test("direct button transition requires a neutral release",[]{Harness h;h.idle();h.feed(0,100);h.feed(1,1000);h.idle(500);require(h.events.size()==1&&h.events[0].second=="switch_app","direct transition triggered second");});
  test("reset discards pending tap and requires neutral",[]{Harness h;h.idle();h.feed(6,100);h.idle(50);h.engine.reset();h.feed(6,1500);require(h.events.empty(),"reset replay");h.idle(500);require(h.events.empty(),"reset release triggered");});
  test("millis wrap preserves long and repeat timing",[]{Harness h;h.now=UINT32_MAX-200;h.idle();h.feed(6,1000);h.idle(500);require(h.events.size()==1&&h.events[0].second=="siri","wrap");});
  test("passcode gate rejects non-long and enforces 10 seconds across wrap",[]{PasscodeGate g;require(!g.allow(0,Gesture::Short),"short");require(g.allow(UINT32_MAX-1000,Gesture::Long),"first");require(!g.allow(500,Gesture::Long),"cooldown");require(g.allow(10000,Gesture::Long),"after cooldown");});
  test("ACC requires prior ON and emits once per OFF",[]{AccDetector a;require(!a.update(0,0,1000,100),"boot off");a.update(10,2000,1000,100);require(!a.update(20,0,1000,100),"early");require(a.update(120,0,1000,100),"fall");require(!a.update(500,0,1000,100),"repeated");a.update(600,2000,1000,100);a.update(700,0,1000,100);require(a.update(800,0,1000,100),"next cycle");});
  test("ASCII supports every printable character",[]{for(int c=32;c<127;++c){uint8_t key=0,mod=0;require(asciiKey(c,key,mod)&&key>=4&&key<=115,"ascii missing");}uint8_t key,mod;require(!asciiKey(128,key,mod),"non ascii");});
  test("macro runner sends press, release, wait, text in order",[]{transportReady=transportSuccess=true;reports.clear();MacroRunner r;Step a;a.type="chord";a.key=30;a.modifiers=7;Step w;w.type="wait";w.ms=100;Step t;t.type="text";t.text="A0";require(r.start({a,w,t}),"start");for(uint32_t now=0;now<500;now+=5)r.tick(now);require(!r.busy(),"not completed");std::vector<std::string> expected={"key:30:7","key-release","key:4:2","key-release","key:39:0","key-release","release"};require(reports==expected,"report order");});
  test("keyboard and media subscriptions allow their own actions independently", [] {
    Step key; key.type = "chord"; key.key = 30;
    Step media; media.type = "media"; media.usage = 233;
    MacroRunner runner;
    reports.clear();
    mediaSubscribed = false;
    require(!runner.start({key, media}) && reports.empty(), "partial macro accepted");
    require(runner.start({key}), "keyboard-only action blocked");
    for (uint32_t now = 0; now < 100; now += 5) runner.tick(now);
    require(reports == std::vector<std::string>{"key:30:0", "key-release", "release"}, "wrong keyboard release");
    reports.clear();
    keyboardSubscribed = false;
    require(!runner.start({media}), "action accepted without notifications");
    mediaSubscribed = true;
    require(runner.start({media}), "media-only action blocked");
    for (uint32_t now = 0; now < 100; now += 5) runner.tick(now);
    require(reports == std::vector<std::string>{"media:233", "media-release", "release"}, "wrong media release");
    keyboardSubscribed = true;
  });
  test("disconnect cancels macro and cannot replay on reconnect",[]{transportReady=transportSuccess=true;reports.clear();MacroRunner r;Step s;s.type="text";s.text="1234";r.start({s});r.tick(0);transportReady=false;require(r.tick(5)==-1&&!r.busy(),"not cancelled");size_t n=reports.size();transportReady=true;r.tick(500);require(reports.size()==n,"replayed");});
  test("failed notification aborts macro instead of retrying text",[]{transportReady=true;transportSuccess=false;reports.clear();MacroRunner r;Step s;s.type="text";s.text="1234";r.start({s});require(r.tick(0)==-1&&!r.busy(),"not aborted");transportSuccess=true;r.tick(1000);require(reports.size()==2,"retried");});
  std::cout<<passed<<" tests passed\n";
}
