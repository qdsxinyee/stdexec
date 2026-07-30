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
#include "__just.hpp"
#include "__let.hpp"
#include "__meta.hpp"
#include "__receivers.hpp"
#include "__schedulers.hpp"
#include "__sender_adaptor_closure.hpp"
#include "__senders.hpp"
#include "__stop_token.hpp"
#include "__transform_completion_signatures.hpp"
#include "__type_traits.hpp"
#include "__upon_error.hpp"
#include "__utility.hpp"
#include "__variant.hpp"
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

    // Compute the let-async-scope-env from the child sender and the receiver env,
    // per P3296R6. This matches the secondary env used by let_value:
    // - use the child sender's value completion scheduler if available,
    // - otherwise use the child sender's completion domain if it is not the default domain,
    // - otherwise an empty env.
    template <class _Child, class _Env>
    STDEXEC_ATTRIBUTE(nodiscard, always_inline, host, device)
    constexpr auto __make_let_scope_env(_Child&& __child, _Env&& __env)
    {
      return __mk_secondary_env_t<set_value_t>{}(__copy_cvref_fn<_Child>{},
                                                 static_cast<_Child&&>(__child),
                                                 static_cast<_Env&&>(__env));
    }

    template <class _Child, class _Env>
    using __let_scope_env_t = __secondary_env_t<_Child, _Env, set_value_t>;

    // error-variant-type<Errors...> per P3296R6: std::monostate for an empty
    // error list, otherwise a std::variant of the error types with duplicates
    // removed.
    template <class... _Errors>
    struct __error_variant_impl
    {
      using type = __mcall<__munique<__qq<std::variant>>, _Errors...>;
    };

    template <>
    struct __error_variant_impl<>
    {
      using type = std::monostate;
    };

    template <class _Env, class... _Errors>
    struct __token
    {
      using __state_t     = __state<_Env, _Errors...>;
      using __scope_env_t = _Env;

      template <class _Error>
      static constexpr bool __error_allowed_v = (std::is_same_v<_Error, _Errors> || ...);

      static constexpr bool __any_error_allowed_v = sizeof...(_Errors) == 1
                                                 && (std::is_same_v<std::exception_ptr, _Errors>
                                                     && ...);

      __token() = default;

      explicit __token(std::shared_ptr<__state_t> __state) noexcept
        : __state_(std::move(__state))
      { }

      [[nodiscard]]
      auto try_associate() const noexcept -> decltype(auto)
      {
        return __state_->__scope_.get_token().try_associate();
      }

      void __request_stop() const noexcept
      {
        __state_->request_stop();
      }

      template <class _Sender>
        requires sender<_Sender>
      [[nodiscard]]
      auto wrap(_Sender&& __sndr) const
      {
        // NOTE: P3296R6 requires a distinct spawn overload that rejects (at compile
        // time) senders whose error completions are incompatible with the scope's
        // error list. That check is not implemented here: it cannot live in wrap(),
        // because the scope_token concept probes wrap() with a test sender whose
        // error type is std::exception_ptr. It requires a dedicated spawn overload,
        // which is a separate, larger change. At run time, incompatible errors are
        // caught by upon_error below and dropped by record_error().
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

    // A sender that holds either the sender returned by the user's function or
    // a just_error(std::exception_ptr) sender. Used to implement the error path
    // when the function passed to let_async_scope throws an exception.
    template <class _Sndr, class _Rcvr>
    struct __fn_or_error_opstate
    {
      using _ErrorSndr = decltype(STDEXEC::just_error(std::declval<std::exception_ptr>()));
      using _Op1       = connect_result_t<_Sndr, _Rcvr>;
      using _Op2       = connect_result_t<_ErrorSndr, _Rcvr>;

      __variant<_Op1, _Op2> __op_{__no_init};

      template <class _S, class _R>
      __fn_or_error_opstate(_S&& __sndr, _R&& __rcvr)
      {
        if constexpr (std::is_same_v<__decay_t<_S>, _Sndr>)
        {
          __op_.template __emplace_from<0>(STDEXEC::connect,
                                           static_cast<_S&&>(__sndr),
                                           static_cast<_R&&>(__rcvr));
        }
        else
        {
          __op_.template __emplace_from<1>(STDEXEC::connect,
                                           static_cast<_S&&>(__sndr),
                                           static_cast<_R&&>(__rcvr));
        }
      }

      __fn_or_error_opstate(__fn_or_error_opstate&&)                  = delete;
      __fn_or_error_opstate(__fn_or_error_opstate const &)            = delete;
      __fn_or_error_opstate& operator=(__fn_or_error_opstate&&)       = delete;
      __fn_or_error_opstate& operator=(__fn_or_error_opstate const &) = delete;

      void start() & noexcept
      {
        STDEXEC::__visit([]<class _Op>(_Op& __op) { STDEXEC::start(__op); }, __op_);
      }
    };

    template <class _Sndr>
    struct __fn_or_error_sender
    {
      using sender_concept = sender_tag;
      using _ErrorSndr     = decltype(STDEXEC::just_error(std::declval<std::exception_ptr>()));

      std::variant<_Sndr, _ErrorSndr> __sndr_;

      template <class _Self, class... _Env>
      static consteval auto get_completion_signatures()
      {
        return STDEXEC::__concat_completion_signatures(
          STDEXEC::get_completion_signatures<__copy_cvref_t<_Self, _Sndr>, _Env...>(),
          STDEXEC::get_completion_signatures<__copy_cvref_t<_Self, _ErrorSndr>, _Env...>());
      }

      template <class _Receiver>
      auto connect(_Receiver&& __rcvr) &&
      {
        using _Rcvr2   = __decay_t<_Receiver>;
        using _Opstate = __fn_or_error_opstate<_Sndr, _Rcvr2>;

        if (__sndr_.index() == 0)
        {
          return _Opstate(std::get<0>(std::move(__sndr_)), static_cast<_Receiver&&>(__rcvr));
        }
        else
        {
          return _Opstate(std::get<1>(std::move(__sndr_)), static_cast<_Receiver&&>(__rcvr));
        }
      }
    };

    // nest: run a sender while holding an association on the scope, so the
    // scope's join() cannot complete before the nested sender does. The
    // association is released as soon as the nested sender completes.
    template <class _Rcvr, class _Assoc>
    struct __nest_rcvr
    {
      using receiver_concept = receiver_tag;

      _Rcvr  __rcvr_;
      _Assoc __assoc_;

      template <class... _Args>
      void set_value(_Args&&... __args) && noexcept
      {
        __assoc_ = _Assoc{};
        STDEXEC::set_value(static_cast<_Rcvr&&>(__rcvr_), static_cast<_Args&&>(__args)...);
      }

      template <class _Error>
      void set_error(_Error&& __err) && noexcept
      {
        __assoc_ = _Assoc{};
        STDEXEC::set_error(static_cast<_Rcvr&&>(__rcvr_), static_cast<_Error&&>(__err));
      }

      void set_stopped() && noexcept
      {
        __assoc_ = _Assoc{};
        STDEXEC::set_stopped(static_cast<_Rcvr&&>(__rcvr_));
      }

      [[nodiscard]]
      auto get_env() const noexcept -> decltype(STDEXEC::get_env(__rcvr_))
      {
        return STDEXEC::get_env(__rcvr_);
      }
    };

    template <class _Sndr, class _Assoc>
    struct __nest_sender
    {
      using sender_concept = sender_tag;

      template <class _Self, class... _Env>
      static consteval auto get_completion_signatures()
      {
        return STDEXEC::get_completion_signatures<__copy_cvref_t<_Self, _Sndr>, _Env...>();
      }

      template <class _Rcvr>
      auto connect(_Rcvr&& __rcvr) &&
      {
        return STDEXEC::connect(
          static_cast<_Sndr&&>(__sndr_),
          __nest_rcvr<__decay_t<_Rcvr>, _Assoc>{static_cast<_Rcvr&&>(__rcvr),
                                                static_cast<_Assoc&&>(__assoc_)});
      }

      _Sndr  __sndr_;
      _Assoc __assoc_;
    };

    template <class _Sndr,
              class _BindFn,
              class _Rcvr,
              class _StateEnv,
              class _Env,
              class... _Errors>
    struct __opstate;

    template <class _Env, class... _Errors>
    struct __state
    {
      using __env_t           = _Env;
      using __error_variant_t = typename __error_variant_impl<_Errors...>::type;

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

      _Rcvr&                     __rcvr_;
      std::shared_ptr<__state_t> __state_;

      template <class... _Args>
      void set_value(_Args&&... __args) && noexcept
      {
        if constexpr (!std::is_same_v<typename __state_t::__error_variant_t, std::monostate>)
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
            return;
          }
        }
        STDEXEC::set_value(static_cast<_Rcvr&&>(__rcvr_), static_cast<_Args&&>(__args)...);
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

    // The operation state. The predecessor runs through let_value with a
    // binding function that, when the predecessor completes with values,
    // eagerly applies the user's function, nests the resulting sender on the
    // scope, and joins the scope (see __sender::connect). Because the function
    // is applied and the nest association is taken before join() starts, work
    // spawned through the scope token is always registered with the scope
    // before it can be joined, so join() waits for it.
    template <class _Sndr,
              class _BindFn,
              class _Rcvr,
              class _StateEnv,
              class _Env,
              class... _Errors>
    struct __opstate
    {
      using _State = __state<_StateEnv, _Errors...>;
      using _Rcvr2 = __receiver<_Rcvr, _State>;

      using _Composed = decltype(STDEXEC::let_value(__declval<_Sndr>(), __declval<_BindFn>()));
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

      template <class _S, class _B, class _R>
      __opstate(_S&& __sndr, _B&& __bind, _R&& __rcvr, std::shared_ptr<_State> __state)
        : __state_(std::move(__state))
        , __rcvr_(static_cast<_R&&>(__rcvr))
        , __stop_token_(get_stop_token(__env::__join(__state_->__env_, STDEXEC::get_env(__rcvr_))))
        , __bind_(static_cast<_B&&>(__bind))
        , __op_(STDEXEC::connect(STDEXEC::let_value(static_cast<_S&&>(__sndr),
                                                    static_cast<_BindFn&&>(__bind_)),
                                 _Rcvr2{__rcvr_, __state_}))
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
      _Rcvr                            __rcvr_;
      stop_token_of_t<_Env>            __stop_token_;
      _BindFn                          __bind_;
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
        using __rcvr_env  = __mfront<_Env..., env<>>;
        using __scope_env = __let_scope_env_t<_Child, __rcvr_env>;
        using __env       = __join_env_t<__scope_env, __rcvr_env>;
        using _Token      = __token<__decay_t<__scope_env>, _Errors...>;

        auto __child_sigs = STDEXEC::get_completion_signatures<_Child, __env>();

        auto __value_fn = []<class... _Args>()
        {
          // Equivalent of P3296R6's check-types for one value completion:
          static_assert(__decay_copyable<_Args...>,
                        "let_async_scope: the predecessor sender's value results must be "
                        "decay-copyable");
          static_assert(__invocable<_Fn, _Token, __decay_t<_Args>&...>,
                        "let_async_scope: the function must be invocable with the scope token "
                        "and the predecessor sender's value results");
          if constexpr (__invocable<_Fn, _Token, __decay_t<_Args>&...>)
          {
            // P3296R6: if the error list does not contain std::exception_ptr, the
            // function must be declared noexcept.
            if constexpr (!((std::is_same_v<std::exception_ptr, _Errors>) || ...))
            {
              static_assert(__nothrow_invocable<_Fn, _Token, __decay_t<_Args>&...>,
                            "let_async_scope_with_error: when the error list does not contain "
                            "std::exception_ptr, the function must be noexcept");
            }
            using _Inner = __invoke_result_t<_Fn, _Token, __decay_t<_Args>&...>;
            if constexpr (std::is_void_v<_Inner>)
            {
              return completion_signatures<set_value_t()>{};
            }
            else
            {
              static_assert(sender<_Inner>,
                            "let_async_scope: the function must return a sender or void");
              static_assert(sender_in<_Inner, __env>,
                            "let_async_scope: the sender returned by the function is not valid "
                            "in the let_async_scope environment");
              return STDEXEC::get_completion_signatures<_Inner, __env>();
            }
          }
          else
          {
            return completion_signatures<>{};
          }
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
        using _RcvrEnv     = env_of_t<_Receiver>;
        using _LetScopeEnv = __let_scope_env_t<_Sndr, _RcvrEnv>;
        using _Env         = __join_env_t<_LetScopeEnv, _RcvrEnv>;
        using _State       = __state<_LetScopeEnv, _Errors...>;
        using _Token       = __token<_LetScopeEnv, _Errors...>;

        auto __let_scope_env = __make_let_scope_env(__sndr_, STDEXEC::get_env(__rcvr));
        auto __state         = std::make_shared<_State>(std::move(__let_scope_env));

        auto __wrapped =
          [__fn = static_cast<_Fn&&>(__fn_), __token = _Token(__state)](auto&&... __args) mutable
        {
          using _Result = std::invoke_result_t<_Fn, _Token, decltype(__args)&...>;
          if constexpr (std::is_void_v<_Result>)
          {
            using _Sndr2 = decltype(STDEXEC::just());
            try
            {
              std::move(__fn)(__token, static_cast<decltype(__args)&&>(__args)...);
              return __fn_or_error_sender<_Sndr2>{STDEXEC::just()};
            }
            catch (...)
            {
              __token.__request_stop();
              return __fn_or_error_sender<_Sndr2>{STDEXEC::just_error(std::current_exception())};
            }
          }
          else
          {
            try
            {
              return __fn_or_error_sender<_Result>{
                std::move(__fn)(__token, static_cast<decltype(__args)&&>(__args)...)};
            }
            catch (...)
            {
              __token.__request_stop();
              return __fn_or_error_sender<_Result>{STDEXEC::just_error(std::current_exception())};
            }
          }
        };

        // let-async-scope-bind (P3296R6): apply the function, nest the
        // resulting sender on the scope by holding an association on the scope
        // for as long as the sender runs, and only then start the scope's
        // join(). Applying the function and taking the association before
        // join() starts ensures that work spawned through the token -- by the
        // function itself, or through a copy of the token while the function's
        // sender runs -- registers with the scope before it can be joined, so
        // join() waits for it instead of completing with the work in flight.
        auto __bind = [__wrapped = std::move(__wrapped), __state](auto&&... __args) mutable
        {
          auto __sndr2  = __wrapped(static_cast<decltype(__args)&&>(__args)...);
          auto __assoc  = __state->get_token().try_associate();
          auto __nested = __state->get_token().wrap(static_cast<decltype(__sndr2)&&>(__sndr2));
          return STDEXEC::when_all(__nest_sender<decltype(__nested), decltype(__assoc)>{
                                     static_cast<decltype(__nested)&&>(__nested),
                                     static_cast<decltype(__assoc)&&>(__assoc)},
                                   __state->join());
        };

        using _Bind = __decay_t<decltype(__bind)>;
        using _Opstate =
          __opstate<_Sndr, _Bind, __decay_t<_Receiver>, _LetScopeEnv, _Env, _Errors...>;

        return _Opstate(static_cast<_Sndr&&>(__sndr_),
                        std::move(__bind),
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

  //! @brief A pipeable sender adaptor that introduces an async scope with a
  //!        user-specified set of error types.
  //!
  //! Creates an internal @c stdexec::counting_scope, invokes @c __fn with a
  //! scope token and the predecessor's value completions, and does not complete
  //! until all work spawned through the token has finished. If any spawned work
  //! errors, the returned sender completes with the recorded error.
  //!
  //! @see stdexec::let_async_scope
  template <class... _Errors>
  struct let_async_scope_with_error_t : __let_async_scope::__let_async_scope_t<_Errors...>
  { };

  //! @brief Like @c let_async_scope, but with a user-specified set of error types.
  template <class... _Errors>
  inline constexpr let_async_scope_with_error_t<_Errors...> let_async_scope_with_error{};

  //! @brief A pipeable sender adaptor that introduces an async scope.
  //!
  //! Equivalent to @c let_async_scope_with_error<std::exception_ptr>: errors
  //! raised by spawned work are wrapped with @c std::exception_ptr.
  inline constexpr let_async_scope_with_error_t<std::exception_ptr> let_async_scope{};
}  // namespace STDEXEC

#include "__epilogue.hpp"
