#include "shortcog/shortcog.hpp"

namespace shortcog {

void register_driver() {
    // TODO
    // - Identify returns true only if SHORTCOG_HEADER open option is present
    // - Open decodes the blob, builds the synthetic GDALDataset, zero I/O
    // - IRasterIO dispatches single pixel, parallel tile, and unaligned paths
}

}  // namespace shortcog
