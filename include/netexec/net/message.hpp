// netexec/net/message.hpp                                               -*-C++-*-
// TAPS message abstraction.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace netexec::net {

class message {
  public:
    message() = default;

    explicit message(std::string str)
        : data_(reinterpret_cast<const std::byte*>(str.data()),
                reinterpret_cast<const std::byte*>(str.data()) + str.size()) {}

    explicit message(std::vector<std::byte> data)
        : data_(std::move(data)) {}

    message(const std::byte* data, std::size_t size)
        : data_(data, data + size) {}

    auto data() const -> const std::byte* { return data_.data(); }
    auto size() const -> std::size_t { return data_.size(); }
    auto empty() const -> bool { return data_.empty(); }

  private:
    std::vector<std::byte> data_;
};

} // namespace netexec::net
