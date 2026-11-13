#include "fastmm/venues/md_copy.hpp"

#include "fastmm/venues/venue.hpp"

namespace fastmm::venues {

std::unique_ptr<MdCopy> Venue::make_md_copy(EventSink& /*sink*/, UpdateFilter& /*filter*/) {
  return nullptr;
}

}  // namespace fastmm::venues
