// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Imports.h"
#include <new>
#include <utility>

namespace window_sweaters::imports::detail {

bool ValidUtf8(std::string_view text);
bool ValidChartName(std::string_view name);
Result<std::string> HashContent(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint32_t> cells, Material material);
Result<std::monostate> ValidatePngDeflate(std::span<const std::uint8_t> encoded,
    std::uint32_t width, std::uint32_t height, unsigned depth, unsigned color_type, bool interlaced);

template<class T, class F> Result<T> CheckedAllocation(F&& function) {
    try {
        return std::forward<F>(function)();
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::ResourceLimit, "Import allocation failed"};
    }
}

} // namespace window_sweaters::imports::detail
