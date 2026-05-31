#include "shortcog/shortcog.hpp"

#include "cpl_error.h"

#include <cstddef>
#include <string>

namespace shortcog {

std::expected<ImageCube, std::string>
ImageCube::create(std::vector<std::shared_ptr<Image>> images)
{
    if (images.empty()) {
        return std::unexpected<std::string>("empty image list");
    }
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (!images[i]) {
            return std::unexpected<std::string>(
                "null image at index " + std::to_string(i + 1));
        }
    }

    const Header& ref = images[0]->header();
    for (std::size_t i = 1; i < images.size(); ++i) {
        const Header& h = images[i]->header();
        if (h.image_width != ref.image_width || h.image_length != ref.image_length) {
            return std::unexpected<std::string>(
                "image " + std::to_string(i + 1) + ": image size mismatch");
        }
        if (h.tile_width != ref.tile_width || h.tile_length != ref.tile_length) {
            return std::unexpected<std::string>(
                "image " + std::to_string(i + 1) + ": tile size mismatch");
        }
        if (h.samples_per_pixel != ref.samples_per_pixel) {
            return std::unexpected<std::string>(
                "image " + std::to_string(i + 1) + ": band count mismatch");
        }
        if (h.gdal_type != ref.gdal_type) {
            return std::unexpected<std::string>(
                "image " + std::to_string(i + 1) + ": dtype mismatch");
        }
        if (h.predictor != ref.predictor) {
            return std::unexpected<std::string>(
                "image " + std::to_string(i + 1) + ": predictor mismatch");
        }
    }

    return ImageCube(std::move(images));
}

ImageCube::ImageCube(std::vector<std::shared_ptr<Image>> images) noexcept
    : images_(std::move(images))
{}

std::size_t ImageCube::size() const noexcept
{
    return images_.size();
}

const Header& ImageCube::spec() const noexcept
{
    return images_[0]->header();
}

bool ImageCube::read(std::span<const int> n_index,
                     std::span<const int> bands,
                     int y_off, int y_size,
                     int x_off, int x_size,
                     const LayoutPlan& layout,
                     std::byte* dst) const
{
    const std::size_t bps      = spec().bytes_per_sample;
    const std::size_t n_stride = static_cast<std::size_t>(layout.sn) * bps;

    for (std::size_t k = 0; k < n_index.size(); ++k) {
        const int ni = n_index[k];
        if (ni < 1 || static_cast<std::size_t>(ni) > images_.size()) {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "ImageCube::read: n=%d out of range [1, %zu]",
                     ni, images_.size());
            return false;
        }
        std::byte* slice = dst + k * n_stride;
        if (!images_[static_cast<std::size_t>(ni - 1)]->read(
                bands, y_off, y_size, x_off, x_size, layout, slice)) {
            return false;
        }
    }
    return true;
}

}  // namespace shortcog