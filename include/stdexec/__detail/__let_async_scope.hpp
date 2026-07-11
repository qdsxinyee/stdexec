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
#pragma once

#include "__execution_fwd.hpp"

#include "__completion_signatures.hpp"
#include "__completion_signatures_of.hpp"
#include "__concepts.hpp"
#include "__counting_scopes.hpp"
#include "__env.hpp"
#include "__let.hpp"
#include "__meta.hpp"
#include "__receivers.hpp"
#include "__sender_adaptor_closure.hpp"
#include "__senders.hpp"
#include "__stop_token.hpp"
#include "__transform_completion_signatures.hpp"
#include "__type_traits.hpp"
#include "__upon_error.hpp"
#include "__utility.hpp"
#include "__when_all.hpp"
#include "__write_env.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

#include "__prologue.hpp"

namespace STDEXEC
{
  namespace __let_async_scope
  {
    template <class _Env, class... _Errors>
    struct __state;

    template <class _Rcvr, class _State>
    struct __receiver;

    // Compute the let-async-scope-env from the child sender, per P3296R4:
    // - use the child sender's value completion scheduler if available,
    // - otherwise use the child sender's domain if it is not the default domain,
    // - otherwise an empty env.
    template <class _Child>
    STDEXEC_ATTRIBUTE(nodiscard, always_inline, host, device)
    constexpr auto __make_let_scope_env(_Child&& __child)
    {
      using _ChildEnv = env_of_t<_Child>;
      if constexpr (__queryable_with<_ChildEnv, get_completion_scheduler_t<set_value_t>>)
      {
        auto __sched = get_completion_scheduler<set_value_t>(STDEXEC::get_env(__child));
        return env{
          prop{      get_scheduler, __sched},
          prop{get_start_scheduler, __sched}
        };
      }
      else
      {
        auto __domain = get_domain(STDEXEC::get_env(__child));
        if constexpr (std::is_same_v<decltype(__domain), default_domain>)
        {
          return env{};
        }
        else
        {
          return env{
            prop{get_domain, __domain}
          };
        }
      }
    }

    template <class _Child>
    using __let_scope_env_t = decltype(__make_let_scope_env(__declval<_Child>()));

    template <class _Env, class... _Errors>
    struct __token
    {
      using __state_t = __state<_Env, _Errors...>;

      __token() = default;

      explicit __token(std::shared_ptr<__state_t> __state) noexcept
        : __state_(std::move(__state))
      { }

      [[nodiscard]]
      auto try_associate() const noexcept -> decltype(auto)
      {
        return __state_->__scope_.get_token().try_associate();
      }

      template <class _Sender>
        requires sender<_Sender>
      [[nodiscard]]
      auto wrap(_Sender&& __sndr) const
      {
        auto __scope_tok = __state_->__scope_.get_token();
        return __scope_tok.wrap(write_env(upon_error(static_cast<_Sender&&>(__sndr),
                                                     [__state = __state_](auto&& __err) noexcept
                                                     {
                                                       __state->record_error(
                                                         static_cast<decltype(__err)&&>(__err));
                                                       __state->request_stop();
                                                     }),
                                          __state_->__env_));
      }

     private:
      std::shared_ptr<__state_t> __state_;
    };

    template <class _Sndr,
              class _WrappedFn,
              class _Rcvr,
              class _StateEnv,
              class _Env,
              class... _Errors>
    struct __opstate;

    template <class _Env, class... _Errors>
    struct __state
    {
      using __env_t           = _Env;
      using __error_variant_t = std::variant<_Errors...>;

      explicit __state(_Env __env)
        : __env_(static_cast<_Env&&>(__env))
      { }

      void request_stop() noexcept
      {
        __scope_.request_stop();
      }

      template <class _Error>
      void record_error(_Error&& __err) noexcept
      {
        std::lock_guard<std::mutex> __lock(__mutex_);
        if (!__error_)
        {
          __store_first_error(static_cast<_Error&&>(__err));
        }
      }

      [[nodiscard]]
      auto join() -> decltype(auto)
      {
        return __scope_.join();
      }

      [[nodiscard]]
      auto get_token() -> decltype(auto)
      {
        return __scope_.get_token();
      }

     private:
      template <class _Error>
      void __store_first_error(_Error&& __err) noexcept
      {
        if constexpr (std::is_constructible_v<__error_variant_t, _Error>)
        {
          try
          {
            __error_.emplace(static_cast<_Error&&>(__err));
          }
          catch (...)
          {
            __fallback();
          }
        }
        else if constexpr ((std::is_same_v<std::exception_ptr, _Errors> || ...))
        {
          try
          {
            __error_.emplace(std::make_exception_ptr(static_cast<_Error&&>(__err)));
          }
          catch (...)
          {
            __fallback();
          }
        }
      }

      void __fallback() noexcept
      {
        if constexpr ((std::is_same_v<std::exception_ptr, _Errors> || ...))
        {
          try
          {
            __error_.emplace(std::exception_ptr{});
          }
          catch (...)
          {
            // Nothing more we can do.
          }
        }
      }

      _Env                             __env_;
      counting_scope                   __scope_;
      std::mutex                       __mutex_;
      std::optional<__error_variant_t> __error_;

      friend struct __token<_Env, _Errors...>;
      template <class _R, class _S>
      friend struct __receiver;
      template <class, class, class, class, class, class...>
      friend struct __opstate;
    };

    template <class _Rcvr, class _State>
    struct __receiver
    {
      using receiver_concept = receiver_tag;
      using __state_t        = _State;

      _Rcvr                      __rcvr_;
      std::shared_ptr<__state_t> __state_;

      template <class... _Args>
      void set_value(_Args&&... __args) && noexcept
      {
        std::lock_guard<std::mutex> __lock(__state_->__mutex_);
        if (__state_->__error_)
        {
          std::visit(
            [&](auto&& __err)
            {
              STDEXEC::set_error(static_cast<_Rcvr&&>(__rcvr_),
                                 static_cast<decltype(__err)&&>(__err));
            },
            static_cast<typename __state_t::__error_variant_t&&>(*__state_->__error_));
        }
        else
        {
          STDEXEC::set_value(static_cast<_Rcvr&&>(__rcvr_), static_cast<_Args&&>(__args)...);
        }
      }

      template <class _Error>
      void set_error(_Error&& __err) && noexcept
      {
        __state_->request_stop();
        STDEXEC::set_error(static_cast<_Rcvr&&>(__rcvr_), static_cast<_Error&&>(__err));
      }

      void set_stopped() && noexcept
      {
        STDEXEC::set_stopped(static_cast<_Rcvr&&>(__rcvr_));
      }

      [[nodiscard]]
      auto
      get_env() const noexcept -> __join_env_t<typename __state_t::__env_t const &, env_of_t<_Rcvr>>
      {
        return __env::__join(static_cast<typename __state_t::__env_t const &>(__state_->__env_),
                             STDEXEC::get_env(__rcvr_));
      }
    };

    template <class _Sndr,
              class _WrappedFn,
              class _Rcvr,
              class _StateEnv,
              class _Env,
              class... _Errors>
    struct __opstate
    {
      using _BaseEnv = __fwd_env_t<__decay_t<env_of_t<_Rcvr>>>;
      using _State   = __state<_StateEnv, _Errors...>;
      using _Rcvr2   = __receiver<_Rcvr, _State>;

      using _Composed = decltype(STDEXEC::when_all(STDEXEC::let_value(__declval<_Sndr>(),
                                                                      __declval<_WrappedFn>()),
                                                   __declval<_State&>().join()));
      using _Op       = connect_result_t<_Composed, _Rcvr2>;

      struct __request_stop
      {
        std::shared_ptr<_State> __state_;

        void operator()() const noexcept
        {
          __state_->request_stop();
        }
      };

      using __stop_callback_t = stop_callback_for_t<stop_token_of_t<_Env>, __request_stop>;

      template <class _S, class _W, class _R>
      __opstate(_S&& __sndr, _W&& __wrapped, _R&& __rcvr, std::shared_ptr<_State> __state)
        : __state_(std::move(__state))
        , __stop_token_(get_stop_token(__env::__join(__state_->__env_, STDEXEC::get_env(__rcvr))))
        , __wrapped_(static_cast<_W&&>(__wrapped))
        , __op_(STDEXEC::connect(STDEXEC::when_all(STDEXEC::let_value(static_cast<_S&&>(__sndr),
                                                                      static_cast<_WrappedFn&&>(
                                                                        __wrapped_)),
                                                   __state_->join()),
                                 _Rcvr2{static_cast<_R&&>(__rcvr), __state_}))
      { }

      __opstate(__opstate&&)                  = delete;
      __opstate(__opstate const &)            = delete;
      __opstate& operator=(__opstate&&)       = delete;
      __opstate& operator=(__opstate const &) = delete;

      void start() & noexcept
      {
        __on_stop_.emplace(__stop_token_, __request_stop{__state_});
        STDEXEC::start(__op_);
      }

     private:
      std::shared_ptr<_State>          __state_;
      stop_token_of_t<_Env>            __stop_token_;
      _WrappedFn                       __wrapped_;
      std::optional<__stop_callback_t> __on_stop_;
      _Op                              __op_;
    };

    template <class _Sndr, class _Fn, class... _Errors>
    struct __sender
    {
      using sender_concept = sender_tag;

      template <class _Self, class... _Env>
      static consteval auto get_completion_signatures()
      {
        using _Child      = __copy_cvref_t<_Self, _Sndr>;
        using __base_env  = __fwd_env_t<__mfront<_Env..., env<>>>;
        using __scope_env = __let_scope_env_t<_Child>;
        using __env       = __join_env_t<__scope_env, __base_env>;
        using _Token      = __token<__decay_t<__scope_env>, _Errors...>;

        auto __child_sigs = STDEXEC::get_completion_signatures<_Child, __env>();

        auto __value_fn = []<class... _Args>()
        {
          using _Inner = __invoke_result_t<_Fn, _Token, __decay_t<_Args>&...>;
          return STDEXEC::get_completion_signatures<_Inner, __env>();
        };

        auto __extra_sigs = completion_signatures<set_error_t(_Errors)...,
                                                  set_error_t(std::exception_ptr),
                                                  set_stopped_t()>{};

        return STDEXEC::__transform_completion_signatures(__child_sigs,
                                                          __value_fn,
                                                          __keep_completion<set_error_t>{},
                                                          __keep_completion<set_stopped_t>{},
                                                          __extra_sigs);
      }

      template <class _Receiver>
      auto connect(_Receiver&& __rcvr) &&
      {
        using _BaseEnv     = __fwd_env_t<__decay_t<env_of_t<_Receiver>>>;
        using _LetScopeEnv = __let_scope_env_t<_Sndr>;
        using _Env         = __join_env_t<_LetScopeEnv, _BaseEnv>;
        using _State       = __state<_LetScopeEnv, _Errors...>;
        using _Token       = __token<_LetScopeEnv, _Errors...>;

        auto __let_scope_env = __make_let_scope_env(static_cast<_Sndr&&>(__sndr_));
        auto __state         = std::make_shared<_State>(std::move(__let_scope_env));

        auto __wrapped =
          [__fn = static_cast<_Fn&&>(__fn_), __token = _Token(__state)](auto&&... __args) mutable
        {
          return std::move(__fn)(__token, static_cast<decltype(__args)&&>(__args)...);
        };

        using _Wrapped = __decay_t<decltype(__wrapped)>;
        using _Opstate = __opstate<_Sndr, _Wrapped, _Receiver, _LetScopeEnv, _Env, _Errors...>;

        return _Opstate(static_cast<_Sndr&&>(__sndr_),
                        std::move(__wrapped),
                        static_cast<_Receiver&&>(__rcvr),
                        std::move(__state));
      }

      _Sndr __sndr_;
      _Fn   __fn_;
    };

    template <class... _Errors>
    struct __let_async_scope_t
    {
      template <sender _Sender, __movable_value _Fn>
      constexpr auto operator()(_Sender&& __sndr, _Fn&& __fn) const -> __well_formed_sender auto
      {
        return __sender<__decay_t<_Sender>, __decay_t<_Fn>, _Errors...>{static_cast<_Sender&&>(
                                                                          __sndr),
                                                                        static_cast<_Fn&&>(__fn)};
      }

      template <class _Fn>
      STDEXEC_ATTRIBUTE(always_inline)
      constexpr auto operator()(_Fn&& __fn) const
      {
        return __closure(*this, static_cast<_Fn&&>(__fn));
      }
    };
  }  // namespace __let_async_scope

  using __let_async_scope::__let_async_scope_t;

  //! @brief A pipeable sender adaptor that introduces an async scope.
  //!
  //! Creates an internal @c stdexec::counting_scope, invokes @c __fn with a
  //! scope token and the predecessor's value completions, and does not complete
  //! until all work spawned through the token has finished. If any spawned work
  //! errors, the returned sender completes with the recorded error.
  //!
  //! @c let_async_scope uses @c std::exception_ptr as its error type.
  //!
  //! @see stdexec::let_async_scope_with_error
  struct let_async_scope_t : __let_async_scope_t<std::exception_ptr>
  { };

  //! @brief Like @c let_async_scope, but with a user-specified set of error types.
  template <class... _Errors>
  inline constexpr __let_async_scope_t<_Errors...> let_async_scope_with_error{};

  //! @brief The customization point object for @c let_async_scope.
  inline constexpr let_async_scope_t let_async_scope{};
}  // namespace STDEXEC

#include "__epilogue.hpp"
