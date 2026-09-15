// Round trip through tools/mock_nano.py over a real pty: SET lines must come
// back as EVT:CFG echoes with the firmware clamps applied, the boot dump must
// precede the status frames, and a bad SET must yield a WARNING line. This
// exercises SerialPort + frame_parser + the mock together (no ROS).
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "frame_parser.hpp"
#include "serial_port.hpp"

#ifndef MOCK_NANO_PATH
#error "MOCK_NANO_PATH must point at tools/mock_nano.py"
#endif

namespace
{

using namespace std::chrono_literals;

class MockNanoProcess
{
public:
  explicit MockNanoProcess(const std::string & link)
  : link_(link)
  {
    pid_ = fork();
    if (pid_ == 0) {
      execlp(
        "python3", "python3", MOCK_NANO_PATH, "--link", link.c_str(),
        "--seat-after", "9999", "--run-seconds", "30", static_cast<char *>(nullptr));
      _exit(127);
    }
  }
  ~MockNanoProcess()
  {
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      waitpid(pid_, nullptr, 0);
    }
    std::filesystem::remove(link_);
  }
  bool wait_for_link(std::chrono::milliseconds timeout) const
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (std::filesystem::exists(link_)) {
        return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

private:
  std::string link_;
  pid_t pid_{-1};
};

// Reads lines until `pred` accepts one (returned) or the timeout expires.
template<typename Pred>
std::optional<std::string> read_until(
  mowbot_dock::SerialPort & port, std::vector<std::string> & all,
  std::chrono::milliseconds timeout, Pred pred)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<std::string> lines;
    if (!port.read_lines(lines)) {
      return std::nullopt;
    }
    for (const auto & l : lines) {
      all.push_back(l);
      if (pred(l)) {
        return l;
      }
    }
    std::this_thread::sleep_for(10ms);
  }
  return std::nullopt;
}

std::string unique_link()
{
  return "/tmp/mock_nano_test_" + std::to_string(getpid());
}

}  // namespace

TEST(MockNanoRoundTrip, BootDumpThenSetGetAndWarning)
{
  const std::string link = unique_link();
  MockNanoProcess mock(link);
  ASSERT_TRUE(mock.wait_for_link(5000ms)) << "mock_nano.py did not create " << link;
  // The pty exists before the mock writes its boot lines, but give the
  // symlink a moment to settle before opening (open() flushes pending input).
  std::this_thread::sleep_for(50ms);

  mowbot_dock::SerialPort port;
  ASSERT_TRUE(port.open(link, 115200)) << port.last_error();

  std::vector<std::string> seen;
  // Boot dump: EVT:BOOT, EVT:VCC, four EVT:CFG defaults, then status frames.
  // (open() flushed whatever was already buffered, so ask for a fresh dump.)
  ASSERT_TRUE(port.write_line("GET"));
  int cfg_lines = 0;
  const auto last_cfg = read_until(
    port, seen, 3000ms, [&](const std::string & l) {
      if (mowbot_dock::parse_config_event(l)) {
        ++cfg_lines;
      }
      return cfg_lines == 4;
    });
  ASSERT_TRUE(last_cfg.has_value()) << "expected four EVT:CFG lines after GET";
  bool saw_default_complete_s = false;
  for (const auto & l : seen) {
    if (const auto e = mowbot_dock::parse_config_event(l)) {
      if (e->name == "COMPLETE_S") {
        EXPECT_DOUBLE_EQ(e->value, 300.0);
        saw_default_complete_s = true;
      }
    }
  }
  EXPECT_TRUE(saw_default_complete_s);

  // SET inside the clamp: echoed verbatim.
  ASSERT_TRUE(port.write_line("SET:COMPLETE_S:120"));
  auto echo = read_until(
    port, seen, 3000ms, [](const std::string & l) {return l.rfind("EVT:CFG:", 0) == 0;});
  ASSERT_TRUE(echo.has_value());
  auto evt = mowbot_dock::parse_config_event(*echo);
  ASSERT_TRUE(evt.has_value());
  EXPECT_EQ(evt->name, "COMPLETE_S");
  EXPECT_DOUBLE_EQ(evt->value, 120.0);

  // SET beyond the clamp: the echo carries the clamped value.
  ASSERT_TRUE(port.write_line("SET:COMPLETE_S:9999"));
  echo = read_until(
    port, seen, 3000ms, [](const std::string & l) {return l.rfind("EVT:CFG:", 0) == 0;});
  ASSERT_TRUE(echo.has_value());
  evt = mowbot_dock::parse_config_event(*echo);
  ASSERT_TRUE(evt.has_value());
  EXPECT_DOUBLE_EQ(evt->value, 3600.0);

  // Double with the firmware's precision.
  ASSERT_TRUE(port.write_line("SET:COMPLETE_A:0.70"));
  echo = read_until(
    port, seen, 3000ms, [](const std::string & l) {return l.rfind("EVT:CFG:", 0) == 0;});
  ASSERT_TRUE(echo.has_value());
  evt = mowbot_dock::parse_config_event(*echo);
  ASSERT_TRUE(evt.has_value());
  EXPECT_EQ(evt->name, "COMPLETE_A");
  EXPECT_EQ(evt->text, "0.70");

  // Unknown name: WARNING:SET, and the status frames keep flowing.
  ASSERT_TRUE(port.write_line("SET:NOPE:1"));
  const auto warning = read_until(
    port, seen, 3000ms, [](const std::string & l) {return mowbot_dock::is_warning_line(l);});
  ASSERT_TRUE(warning.has_value());
  EXPECT_NE(warning->find("SET"), std::string::npos);

  // The parameter traffic must not stall the 10 Hz status stream.
  const auto frame = read_until(
    port, seen, 2000ms, [](const std::string & l) {
      return mowbot_dock::parse_status_frame(l).has_value();
    });
  ASSERT_TRUE(frame.has_value()) << "no status frame after the parameter exchange";
  EXPECT_EQ(mowbot_dock::parse_status_frame(*frame)->state, 0);
}
