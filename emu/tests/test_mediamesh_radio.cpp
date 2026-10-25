// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh on the radio emulator: Wadamesh's MediaMeshRadio (src/helpers/
// MediaMeshRadio.h) on meshcomod's CustomSX1262Wrapper and the real RadioLib
// SX126x driver, each node on its own modelled SX1262 (emu/devices/sx1262.cpp),
// joined by the air broker (emu/air). One radio scanning two profiles: what it
// hears on each, and what it sends where.
//
// Each node runs in its own process: the wrapper's buffered receive (the drain
// task MediaMesh switches on) is file-static in meshcomod, one per process, as
// on a device.
//
//   test_mediamesh_radio <emu-air broker executable>
#include "devices/sx1262.h"
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include "helpers/MediaMeshRadio.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #c); } while (false)

namespace {

// The CascadiaMesh main mesh, and the MediaMesh 250 kHz default.
constexpr float kMainFreq = 910.525f, kMainBw = 62.5f;
constexpr uint8_t kMainSf = 7, kMainCr = 5;
mediamesh::Profile2 radio2Profile() {
  mediamesh::Profile2 p;
  p.mode = mediamesh::MODE_RXTX;
  p.p.freq = mediamesh::kDefaultFreq;
  p.p.bw = mediamesh::kDefaultBw;
  p.p.sf = mediamesh::kDefaultSf;
  p.p.cr = mediamesh::kDefaultCr;
  return p;
}

struct Board : mesh::MainBoard {
  uint16_t getBattMilliVolts() override { return 4000; }
  const char* getManufacturerName() const override { return "emu"; }
  void reboot() override {}
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

template <class P> void waitFor(P ready, uint64_t timeout_us) {
  const uint64_t start = emu::micros64();
  while (!ready()) {
    CHECK(emu::micros64() - start < timeout_us);
    delay(1);
  }
}

// The profiles the next packet goes out on (the sender's classifier).
uint8_t g_next_mask = 1;
uint8_t classify(const uint8_t*, int) { return g_next_mask; }

// What the receiver heard, by the profile MediaMeshRadio says it came in on.
uint32_t g_heard[2] = {0, 0};
uint32_t g_wrong_origin = 0;
void originSink(const uint8_t* raw, int len, uint8_t origin) {
  // Test packets carry the profile they were sent on in byte 1.
  if (len >= 2 && raw[0] == 0xE7) {
    if (raw[1] != origin) ++g_wrong_origin;
    ++g_heard[origin & 1];
  }
}

// startRecv is protected in RadioLibWrapper: the Dispatcher calls it.
struct Wrapper : MediaMeshRadio<CustomSX1262Wrapper> {
  using MediaMeshRadio<CustomSX1262Wrapper>::MediaMeshRadio;
  using MediaMeshRadio<CustomSX1262Wrapper>::startRecv;
};

struct Node {
  Sx1262Model chip;
  Module module;
  CustomSX1262 radio;
  Board board;
  Wrapper wrapper;
  explicit Node(AirPort* air, int base = 40)
      : module(base, base + 3, base + 2, base + 1, SPI), radio(&module), wrapper(radio, board) {
    digitalWrite(base, HIGH);
    chip.attach(base, base + 1, base + 2, base + 3, air);
    CHECK(radio.std_init(&SPI));
    wrapper.begin();
    wrapper.setParams(kMainFreq, kMainBw, kMainSf, kMainCr);
    wrapper.startRecv();
  }
  void mediaMesh() {
    wrapper.setMediaClassifier(classify);
    wrapper.setOriginSink(originSink);
    wrapper.mediaMeshConfigure(radio2Profile());
    CHECK(wrapper.mediaMeshScanning());
    // The scan measures its retunes before trusting its timing.
    waitFor([&] { return wrapper.mediaMeshTimingValid() && wrapper.mediaMeshSwitchSamples() >= mediamesh::kSwitchSamples; },
            5000000);
  }
  // Sends one packet on the profiles in `mask` and waits until it is out.
  void send(uint8_t mask, uint8_t seq) {
    uint8_t pkt[40];
    memset(pkt, seq, sizeof pkt);
    pkt[0] = 0xE7;
    pkt[1] = mask == 2 ? 1 : 0;
    g_next_mask = mask;
    CHECK(wrapper.startSendRaw(pkt, sizeof pkt));
    waitFor([&] { return wrapper.isSendComplete(); }, 2000000);
    wrapper.onSendFinished();
    wrapper.startRecv();
  }
};

unsigned freePort() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  socklen_t n = sizeof a;
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) == 0);
  close(fd);
  return ntohs(a.sin_port);
}

pid_t spawn(const std::function<int()>& body) {
  fflush(nullptr);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int rc = 1;
    try { rc = body(); } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << std::endl; }
    fflush(nullptr);
    _exit(rc);
  }
  return pid;
}

int waitChild(pid_t pid) {
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// N packets, alternating main and radio2, from a MediaMesh sender to a scanning
// receiver. Both lanes must arrive, each credited to the profile it came on.
constexpr int kPerLane = 20;
constexpr uint32_t kGapMs = 400;

int sender(unsigned port) {
  NetAir& air = *new NetAir("TX", uint16_t(port));
  waitFor([&] { return air.connected(); }, 5000000);
  // Never destroyed (as on a device): the scan and drain tasks run until _exit.
  Node& n = *new Node(&air);
  n.mediaMesh();
  delay(1500);   // the receiver is up and scanning
  for (int i = 0; i < 2 * kPerLane; ++i) {
    n.send(i % 2 ? 2 : 1, uint8_t(i));
    delay(kGapMs);
  }
  std::cout << "sender: TX main " << n.wrapper.mediaMeshTxCount(0) << ", radio2 " << n.wrapper.mediaMeshTxCount(1)
            << std::endl;
  CHECK(n.wrapper.mediaMeshTxCount(0) == kPerLane);
  CHECK(n.wrapper.mediaMeshTxCount(1) == kPerLane);
  return 0;
}

// `scan` false: radio2 off, the main profile only (the control).
int receiver(unsigned port, const char* name, bool scan, const char* result) {
  NetAir& air = *new NetAir(name, uint16_t(port));
  waitFor([&] { return air.connected(); }, 5000000);
  Node& n = *new Node(&air, 60);
  if (scan) n.mediaMesh();
  else n.wrapper.setOriginSink(originSink);
  std::array<uint8_t, 256> buf{};
  const uint64_t until = emu::micros64() + uint64_t(1500 + 2 * kPerLane * (kGapMs + 250) + 3000) * 1000;
  while (emu::micros64() < until) {
    n.wrapper.recvRaw(buf.data(), buf.size());
    delay(1);
  }
  std::ofstream(result) << g_heard[0] << ' ' << g_heard[1] << ' ' << g_wrong_origin << ' '
                        << n.wrapper.mediaMeshSwitches() << ' ' << n.wrapper.mediaMeshSwitchUs() << '\n';
  return 0;
}

struct Broker {
  pid_t pid = -1;
  ~Broker() { stop(); }
  void stop() {
    if (pid > 0) { kill(pid, SIGTERM); waitChild(pid); pid = -1; }
  }
};

void scanTest(const char* broker_exe) {
  char dir[] = "/tmp/emu-mediamesh-XXXXXX";
  CHECK(mkdtemp(dir));
  const std::string topology = std::string(dir) + "/topology", result = std::string(dir) + "/rx",
                    plain_result = std::string(dir) + "/plain";
  std::ofstream(topology) << "default -80 8 0\n";
  const unsigned port = freePort();
  Broker broker;
  broker.pid = spawn([&] {
    const std::string p = std::to_string(port);
    execl(broker_exe, broker_exe, "--port", p.c_str(), "--topology", topology.c_str(), nullptr);
    return 127;
  });
  const pid_t rx = spawn([&] { return receiver(port, "RX", true, result.c_str()); });
  const pid_t plain = spawn([&] { return receiver(port, "PLAIN", false, plain_result.c_str()); });
  const pid_t tx = spawn([&] { return sender(port); });
  CHECK(waitChild(tx) == 0);
  CHECK(waitChild(rx) == 0);
  CHECK(waitChild(plain) == 0);
  broker.stop();
  unsigned main = 0, r2 = 0, wrong = 0, switches = 0, sw_us = 0;
  std::ifstream in(result);
  CHECK(in >> main >> r2 >> wrong >> switches >> sw_us);
  std::cout << "receiver: main " << main << "/" << kPerLane << ", radio2 " << r2 << "/" << kPerLane << ", "
            << switches << " retunes (" << sw_us << " us each)" << std::endl;
  CHECK(wrong == 0);                    // each packet credited to the profile it came on
  CHECK(main >= kPerLane * 9 / 10);     // mikecarper's bench: 197/200 on a dual-profile scan
  CHECK(r2 >= kPerLane * 9 / 10);
  CHECK(switches > 100);                // it really was scanning
  // The control, radio2 off: everything on the main mesh, nothing from radio2.
  unsigned pm = 0, pr2 = 0, pw = 0, ps = 0, pus = 0;
  std::ifstream pin(plain_result);
  CHECK(pin >> pm >> pr2 >> pw >> ps >> pus);
  std::cout << "control (radio2 off): main " << pm << "/" << kPerLane << ", radio2 " << pr2 << std::endl;
  CHECK(pm >= kPerLane * 9 / 10 && pr2 == 0 && ps == 0);
  std::cout << "PASS MediaMesh scan hears both profiles and credits each packet to its own" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    CHECK(argc == 2);
    scanTest(argv[1]);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    return 1;
  }
}
