// SPDX-License-Identifier: GPL-3.0-or-later
// The models standalones download from this repository (models/, fetched over
// raw.githubusercontent.com by the firmware): each file the firmware names is
// here, and it loads and runs in the firmware's own code. A model link that
// 404s, or a file that doesn't load, reaches no standalone.
//   g++ -std=c++17 -I src test/test_models.cpp src/ui-touch/VoxRestoreSmall.cpp -lm
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ui-touch/VoxRestoreSmall.h"

static std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static size_t s_out = 0;
static bool sink(void*, const float* y, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!std::isfinite(y[i])) return false;
  s_out += n;
  return true;
}

int main() {
  int fails = 0;
  // Every models/<file> the firmware downloads.
  const std::string ui = slurp("src/ui-touch/UITask.cpp");
  std::set<std::string> named;
  const std::regex re("WADA_FORK_REPO \"/[^\"]*/models/([A-Za-z0-9._-]+)\"");
  for (std::sregex_iterator it(ui.begin(), ui.end(), re), end; it != end; ++it) named.insert((*it)[1]);
  if (named.empty()) { fprintf(stderr, "no models/ downloads found in UITask.cpp: update this test\n"); fails++; }
  for (const auto& n : named) {
    const std::string p = "models/" + n;
    const std::string b = slurp(p.c_str());
    if (b.empty()) { fprintf(stderr, "%s: the firmware downloads it, but it isn't in the repository\n", p.c_str()); fails++; continue; }
    printf("%s: %zu bytes\n", p.c_str(), b.size());
    if (n == "vox-restorer-small.bin") {
      VoxRestoreSmall rs;
      const char* err = nullptr;
      if (!rs.load((const uint8_t*)b.data(), b.size(), &err)) {
        fprintf(stderr, "%s doesn't load: %s\n", p.c_str(), err ? err : "?");
        fails++;
        continue;
      }
      std::vector<float> x(4000);
      for (size_t i = 0; i < x.size(); i++) x[i] = 0.3f * sinf(2 * 3.14159f * 220 * i / 8000.f);
      s_out = 0;
      if (!rs.run(x.data(), x.size(), sink, nullptr) || s_out < 7000) {
        fprintf(stderr, "%s loads but doesn't restore (%zu samples out)\n", p.c_str(), s_out);
        fails++;
        continue;
      }
      printf("  loads (dim %d, %d layers) and restores 0.5 s to %zu samples\n", rs.dim(), rs.layers(), s_out);
    }
  }
  if (fails) { fprintf(stderr, "models: %d problem(s)\n", fails); return 1; }
  printf("models: all present and working\n");
  return 0;
}
