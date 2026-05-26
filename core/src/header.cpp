#include "shortcog/shortcog.hpp"

namespace shortcog {

// TODO parse the header blob and validate.
// - magic and version match
// - tile_count == tiles_across * tiles_down * samples_per_pixel
// - every tile_byte_counts[i] > 0
// - reconstructed offsets strictly increasing
// reconstruction:
//   offset[0] = base_tiles_offset
//   offset[i+1] = offset[i] + tile_byte_counts[i] + 8

}  // namespace shortcog
