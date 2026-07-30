/*
 * Copyright (c) 2025 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdexec/execution.hpp>

#include <stdexcept>

namespace ex = STDEXEC;

auto main() -> int
{
  auto snd = ex::just()
           | ex::let_async_scope_with_error<std::runtime_error>(
               [](auto token) noexcept
               {
                 ex::spawn(ex::just_error(42), token);  // int is not in the error list
               });
  // build error: not compatible with the let_async_scope error list
  STDEXEC::sync_wait(std::move(snd));
}
