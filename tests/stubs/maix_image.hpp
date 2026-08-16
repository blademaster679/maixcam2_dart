#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace maix {
namespace image {

enum class Format {
    FMT_RGB888 = 0,
    FMT_GRAYSCALE = 12,
};

class Blob {
public:
    Blob(int x, int y, int width, int height,
         float center_x, float center_y,
         int pixels = -1, float roundness = 1.0F)
        : x_(x), y_(y), width_(width), height_(height),
          center_x_(center_x), center_y_(center_y),
          pixels_(pixels < 0 ? width * height : pixels),
          roundness_(roundness)
    {
    }

    int x() { return x_; }
    int y() { return y_; }
    int w() { return width_; }
    int h() { return height_; }
    float cxf() { return center_x_; }
    float cyf() { return center_y_; }
    int pixels() { return pixels_; }
    int area() { return width_ * height_; }
    float density()
    {
        return width_ > 0 && height_ > 0
                   ? static_cast<float>(pixels_) / (width_ * height_)
                   : 0.0F;
    }
    float roundness() { return roundness_; }
    int perimeter() { return 2 * (width_ + height_); }

private:
    int x_;
    int y_;
    int width_;
    int height_;
    float center_x_;
    float center_y_;
    int pixels_;
    float roundness_;
};

class Image {
public:
    Image(int width, int height, Format format = Format::FMT_RGB888)
        : width_(width), height_(height), format_(format),
          data_(static_cast<std::size_t>(width) * height * 3U, 0)
    {
    }

    Format format() { return format_; }
    int width() { return width_; }
    int height() { return height_; }
    int data_size() { return static_cast<int>(data_.size()); }
    void *data() { return data_.data(); }

    void set_pixel(int x, int y, uint8_t red, uint8_t green, uint8_t blue)
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) {
            return;
        }
        const std::size_t offset =
            (static_cast<std::size_t>(y) * width_ + x) * 3U;
        data_[offset] = red;
        data_[offset + 1] = green;
        data_[offset + 2] = blue;
    }

    void fill_rect(int x, int y, int width, int height,
                   uint8_t red, uint8_t green, uint8_t blue)
    {
        for (int py = std::max(0, y); py < std::min(height_, y + height); ++py) {
            for (int px = std::max(0, x); px < std::min(width_, x + width); ++px) {
                set_pixel(px, py, red, green, blue);
            }
        }
    }

    void set_blobs(std::vector<Blob> halo, std::vector<Blob> core)
    {
        halo_blobs_ = std::move(halo);
        core_blobs_ = std::move(core);
        find_blobs_calls_ = 0;
    }

    std::vector<Blob> find_blobs(
        std::vector<std::vector<int>> = {}, bool = false,
        std::vector<int> = {}, int = 2, int = 1, int = 10, int = 10,
        bool = false, int = 0, int = 0, int = 0)
    {
        return find_blobs_calls_++ % 2 == 0 ? halo_blobs_ : core_blobs_;
    }

private:
    int width_;
    int height_;
    Format format_;
    std::vector<uint8_t> data_;
    std::vector<Blob> halo_blobs_;
    std::vector<Blob> core_blobs_;
    int find_blobs_calls_ = 0;
};

}  // namespace image
}  // namespace maix
