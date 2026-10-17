// Review 4. scripts/diff_soak.sh starts four `orderbook diff` runs at a time with `&` and cleans up with
// `trap 'rm -rf "$work"' EXIT`. Two things go wrong when it is stopped with Ctrl-C (SIGINT to its process group):
//
// - The runs keep going. In a non-interactive shell an asynchronous command ignores SIGINT (POSIX, "Asynchronous
//   Lists"), so the script dies and its four 50-million-command runs carry on with nobody waiting for them.
// - Under dash, which is /bin/sh on the Ubuntu runner, an EXIT trap doesn't run when the shell is killed by a signal,
//   so the work directory stays in $TMPDIR. (bash, macOS's /bin/sh, does run it.)
//
// And `scripts/diff_soak.sh [ORDERBOOK_BINARY]` resolves a relative ORDERBOOK_BINARY against the repository root,
// after its `cd`, not against the directory it was run from: `cd build/release && ../../scripts/diff_soak.sh
// ./orderbook` says "no binary at ./orderbook" and exits 2.
//
// The tests stand a small shell script in for the binary, so they take a few seconds and no CPU.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path script() { return fs::path(ORDERBOOK_SCENARIO_DIR).parent_path().parent_path() / "scripts" / "diff_soak.sh"; }

struct Sandbox {
  fs::path root;
  Sandbox() {
    root = fs::temp_directory_path() / ("review4_soak_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "tmp");
    fs::create_directories(root / "pids");
  }
  ~Sandbox() {
    for (const pid_t pid : pids()) ::kill(pid, SIGKILL);
    fs::remove_all(root);
  }
  // A stand-in for `orderbook`: records its pid, then sleeps (or, if `quick`, says the books agree at once).
  fs::path binary(bool quick) const {
    const fs::path path = root / (quick ? "quick_orderbook" : "slow_orderbook");
    std::ofstream out(path);
    out << "#!/bin/sh\n";
    if (quick) {
      out << "echo \"commands $3: the same\"\nexit 0\n";
    } else {
      out << "echo $$ > \"" << (root / "pids").string() << "/$2.pid\"\nexec sleep 30\n";
    }
    out.close();
    fs::permissions(path, fs::perms::owner_all);
    return path;
  }
  std::vector<pid_t> pids() const {
    std::vector<pid_t> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root / "pids", error)) {
      std::ifstream in(entry.path());
      long pid = 0;
      if (in >> pid && pid > 0) found.push_back(static_cast<pid_t>(pid));
    }
    return found;
  }
  std::vector<std::string> work_dirs() const {
    std::vector<std::string> found;
    for (const auto& entry : fs::directory_iterator(root / "tmp")) found.push_back(entry.path().filename().string());
    return found;
  }
};

// Starts `shell script binary` in its own process group with TMPDIR in the sandbox, waits for the first wave's four
// runs to start, sends SIGINT to the group as a terminal's Ctrl-C would, and waits for the script to exit.
void interrupt_a_soak(const Sandbox& box, const std::string& shell) {
  const fs::path binary = box.binary(false);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::setpgid(0, 0);
    ::setenv("TMPDIR", (box.root / "tmp").c_str(), 1);
    ::execl(shell.c_str(), shell.c_str(), script().c_str(), binary.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::setpgid(child, child);
  for (int i = 0; i < 100 && box.pids().size() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (box.pids().size() < 4) {
    ::kill(-child, SIGKILL);
    int ignored = 0;
    ::waitpid(child, &ignored, 0);
    FAIL() << "the first wave didn't start";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ::kill(-child, SIGINT);
  int status = 0;
  pid_t done = 0;
  for (int i = 0; i < 100 && done == 0; ++i) {
    done = ::waitpid(child, &status, WNOHANG);
    if (done == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (done == 0) {
    ::kill(-child, SIGKILL);
    ::waitpid(child, &status, 0);
    FAIL() << "the script ignored Ctrl-C";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

std::vector<pid_t> still_running(const std::vector<pid_t>& pids) {
  std::vector<pid_t> running;
  for (const pid_t pid : pids) {
    if (::kill(pid, 0) == 0) running.push_back(pid);
  }
  return running;
}

TEST(Review4Soak, CtrlCStopsTheRunsItStarted) {
  const Sandbox box;
  interrupt_a_soak(box, "/bin/sh");
  EXPECT_EQ(still_running(box.pids()).size(), 0U)
      << "after Ctrl-C the script is gone but its `orderbook diff` runs are still going";
}

TEST(Review4Soak, CtrlCUnderDashLeavesNoWorkDirectory) {
  if (!fs::exists("/bin/dash")) GTEST_SKIP() << "no /bin/dash (Ubuntu's /bin/sh) here";
  const Sandbox box;
  interrupt_a_soak(box, "/bin/dash");
  EXPECT_TRUE(box.work_dirs().empty()) << "left behind in $TMPDIR: " << box.work_dirs().front();
}

TEST(Review4Soak, ARelativeBinaryIsTheCallersPath) {
  const Sandbox box;
  box.binary(true);
  const std::string command = "cd '" + box.root.string() + "' && TMPDIR='" + (box.root / "tmp").string() +
                              "' /bin/sh '" + script().string() + "' ./quick_orderbook >/dev/null 2>&1";
  const int status = std::system(command.c_str());
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "`diff_soak.sh ./quick_orderbook` from the binary's directory didn't run it";
}

}  // namespace
