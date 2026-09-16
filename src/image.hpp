#pragma once
#include "common.hpp"
#include <span>
namespace cv {
// Premultiplied BGRA, static first frame, bounded decoding; called on COM initialized worker.
std::shared_ptr<Pixels> decodeImage(std::span<const uint8_t> data, const Cancel& cancel);
}
