// fastmm-live plus this project's connector: register the venue, then hand over to FastMM's
// command line. `kind = "echo"` in a configuration now selects it.
#include "echo/echo_venue.hpp"

#include <fastmm/cli/live.hpp>

int main(int argc, char** argv) {
  echo::register_echo_venue();
  return fastmm::cli::live(argc, argv);
}
