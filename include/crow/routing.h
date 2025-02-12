#pragma once

#include <cstdint>
#include <utility>
#include <tuple>
#include <unordered_map>
#include <memory>
#include <boost/lexical_cast.hpp>
#include <vector>

#include "crow/common.h"
#include "crow/http_response.h"
#include "crow/http_request.h"
#include "crow/utility.h"
#include "crow/logging.h"
#include "crow/websocket.h"

namespace crow
{
    /// A base class for all rules.

    /// Used to provide a common interface for code dealing with different types of rules.
    /// A Rule provides a URL, allowed HTTP methods, and handlers.
    class BaseRule
    {
    public:
        BaseRule(std::string rule)
            : rule_(std::move(rule))
        {
        }

        virtual ~BaseRule()
        {
        }

        virtual void validate() = 0;
        std::unique_ptr<BaseRule> upgrade()
        {
            if (rule_to_upgrade_)
                return std::move(rule_to_upgrade_);
            return {};
        }

        virtual void handle(const request&, response&, const routing_params&) = 0;
        virtual void handle_upgrade(const request&, response& res, SocketAdaptor&&)
        {
            res = response(404);
            res.end();
        }
#ifdef CROW_ENABLE_SSL
        virtual void handle_upgrade(const request&, response& res, SSLAdaptor&&)
        {
            res = response(404);
            res.end();
        }
#endif

        uint32_t get_methods()
        {
            return methods_;
        }

        template <typename F>
        void foreach_method(F f)
        {
            for(uint32_t method = 0, method_bit = 1; method < static_cast<uint32_t>(HTTPMethod::InternalMethodCount); method++, method_bit<<=1)
            {
                if (methods_ & method_bit)
                    f(method);
            }
        }

        const std::string& rule() { return rule_; }

    protected:
        uint32_t methods_{1<<static_cast<int>(HTTPMethod::Get)};

        std::string rule_;
        std::string name_;

        std::unique_ptr<BaseRule> rule_to_upgrade_;

        friend class Router;
        template <typename T>
        friend struct RuleParameterTraits;
    };


    namespace detail
    {
        namespace routing_handler_call_helper
        {
            template <typename T, int Pos>
            struct call_pair
            {
                using type = T;
                static const int pos = Pos;
            };

            template <typename H1>
            struct call_params
            {
                H1& handler;
                const routing_params& params;
                const request& req;
                response& res;
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename S1, typename S2>
            struct call
            {
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename ... Args1, typename ... Args2>
            struct call<F, NInt, NUint, NDouble, NString, black_magic::S<int64_t, Args1...>, black_magic::S<Args2...>>
            {
                void operator()(F cparams)
                {
                    using pushed = typename black_magic::S<Args2...>::template push_back<call_pair<int64_t, NInt>>;
                    call<F, NInt+1, NUint, NDouble, NString,
                        black_magic::S<Args1...>, pushed>()(cparams);
                }
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename ... Args1, typename ... Args2>
            struct call<F, NInt, NUint, NDouble, NString, black_magic::S<uint64_t, Args1...>, black_magic::S<Args2...>>
            {
                void operator()(F cparams)
                {
                    using pushed = typename black_magic::S<Args2...>::template push_back<call_pair<uint64_t, NUint>>;
                    call<F, NInt, NUint+1, NDouble, NString,
                        black_magic::S<Args1...>, pushed>()(cparams);
                }
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename ... Args1, typename ... Args2>
            struct call<F, NInt, NUint, NDouble, NString, black_magic::S<double, Args1...>, black_magic::S<Args2...>>
            {
                void operator()(F cparams)
                {
                    using pushed = typename black_magic::S<Args2...>::template push_back<call_pair<double, NDouble>>;
                    call<F, NInt, NUint, NDouble+1, NString,
                        black_magic::S<Args1...>, pushed>()(cparams);
                }
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename ... Args1, typename ... Args2>
            struct call<F, NInt, NUint, NDouble, NString, black_magic::S<std::string, Args1...>, black_magic::S<Args2...>>
            {
                void operator()(F cparams)
                {
                    using pushed = typename black_magic::S<Args2...>::template push_back<call_pair<std::string, NString>>;
                    call<F, NInt, NUint, NDouble, NString+1,
                        black_magic::S<Args1...>, pushed>()(cparams);
                }
            };

            template <typename F, int NInt, int NUint, int NDouble, int NString, typename ... Args1>
            struct call<F, NInt, NUint, NDouble, NString, black_magic::S<>, black_magic::S<Args1...>>
            {
                void operator()(F cparams)
                {
                    cparams.handler(
                        cparams.req,
                        cparams.res,
                        cparams.params.template get<typename Args1::type>(Args1::pos)...
                    );
                }
            };

            template <typename Func, typename ... ArgsWrapped>
            struct Wrapped
            {
                template <typename ... Args>
                void set_(Func f, typename std::enable_if<
                    !std::is_same<typename std::tuple_element<0, std::tuple<Args..., void>>::type, const request&>::value
                , int>::type = 0)
                {
                    handler_ = (
#ifdef CROW_CAN_USE_CPP14
                        [f = std::move(f)]
#else
                        [f]
#endif
                        (const request&, response& res, Args... args){
                            res = response(f(args...));
                            res.end();
                        });
                }

                template <typename Req, typename ... Args>
                struct req_handler_wrapper
                {
                    req_handler_wrapper(Func f)
                        : f(std::move(f))
                    {
                    }

                    void operator()(const request& req, response& res, Args... args)
                    {
                        res = response(f(req, args...));
                        res.end();
                    }

                    Func f;
                };

                template <typename ... Args>
                void set_(Func f, typename std::enable_if<
                        std::is_same<typename std::tuple_element<0, std::tuple<Args..., void>>::type, const request&>::value &&
                        !std::is_same<typename std::tuple_element<1, std::tuple<Args..., void, void>>::type, response&>::value
                        , int>::type = 0)
                {
                    handler_ = req_handler_wrapper<Args...>(std::move(f));
                    /*handler_ = (
                        [f = std::move(f)]
                        (const request& req, response& res, Args... args){
                             res = response(f(req, args...));
                             res.end();
                        });*/
                }

                template <typename ... Args>
                void set_(Func f, typename std::enable_if<
                        std::is_same<typename std::tuple_element<0, std::tuple<Args..., void>>::type, const request&>::value &&
                        std::is_same<typename std::tuple_element<1, std::tuple<Args..., void, void>>::type, response&>::value
                        , int>::type = 0)
                {
                    handler_ = std::move(f);
                }

                template <typename ... Args>
                struct handler_type_helper
                {
                    using type = std::function<void(const crow::request&, crow::response&, Args...)>;
                    using args_type = black_magic::S<typename black_magic::promote_t<Args>...>;
                };

                template <typename ... Args>
                struct handler_type_helper<const request&, Args...>
                {
                    using type = std::function<void(const crow::request&, crow::response&, Args...)>;
                    using args_type = black_magic::S<typename black_magic::promote_t<Args>...>;
                };

                template <typename ... Args>
                struct handler_type_helper<const request&, response&, Args...>
                {
                    using type = std::function<void(const crow::request&, crow::response&, Args...)>;
                    using args_type = black_magic::S<typename black_magic::promote_t<Args>...>;
                };

                typename handler_type_helper<ArgsWrapped...>::type handler_;

                void operator()(const request& req, response& res, const routing_params& params)
                {
                    detail::routing_handler_call_helper::call<
                        detail::routing_handler_call_helper::call_params<
                            decltype(handler_)>,
                        0, 0, 0, 0,
                        typename handler_type_helper<ArgsWrapped...>::args_type,
                        black_magic::S<>
                    >()(
                        detail::routing_handler_call_helper::call_params<
                            decltype(handler_)>
                        {handler_, params, req, res}
                   );
                }
            };

        }
    }


    class CatchallRule
    {
    public:
        CatchallRule(){}

        template <typename Func>
        typename std::enable_if<black_magic::CallHelper<Func, black_magic::S<>>::value, void>::type
        operator()(Func&& f)
        {
            static_assert(!std::is_same<void, decltype(f())>::value,
                "Handler function cannot have void return type; valid return types: string, int, crow::response, crow::returnable");

            handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const request&, response& res){
                    res = response(f());
                    res.end();
                });

        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<>>::value &&
            black_magic::CallHelper<Func, black_magic::S<crow::request>>::value,
            void>::type
        operator()(Func&& f)
        {
            static_assert(!std::is_same<void, decltype(f(std::declval<crow::request>()))>::value,
                "Handler function cannot have void return type; valid return types: string, int, crow::response, crow::returnable");

            handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const crow::request& req, crow::response& res){
                    res = response(f(req));
                    res.end();
                });
        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::request>>::value &&
            black_magic::CallHelper<Func, black_magic::S<crow::response&>>::value,
        void>::type
        operator()(Func&& f)
        {
          static_assert(std::is_same<void, decltype(f(std::declval<crow::response&>()))>::value,
                        "Handler function with response argument should have void return type");
          handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const crow::request&, crow::response& res){
                  f(res);
                });
        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::request>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::response&>>::value,
            void>::type
        operator()(Func&& f)
        {
            static_assert(std::is_same<void, decltype(f(std::declval<crow::request>(), std::declval<crow::response&>()))>::value,
                "Handler function with response argument should have void return type");

                handler_ = std::move(f);
        }

        bool has_handler()
        {
            return (handler_ != nullptr);
        }

    protected:
        friend class Router;
    private:
        std::function<void(const crow::request&, crow::response&)> handler_;
    };


    /// A rule dealing with websockets.

    /// Provides the interface for the user to put in the necessary handlers for a websocket to work.
    ///
    class WebSocketRule : public BaseRule
    {
        using self_t = WebSocketRule;
    public:
        WebSocketRule(std::string rule)
            : BaseRule(std::move(rule))
        {
        }

        void validate() override
        {
        }

        void handle(const request&, response& res, const routing_params&) override
        {
            res = response(404);
            res.end();
        }

        void handle_upgrade(const request& req, response&, SocketAdaptor&& adaptor) override
        {
            new crow::websocket::Connection<SocketAdaptor>(req, std::move(adaptor), open_handler_, message_handler_, close_handler_, error_handler_, accept_handler_);
        }
#ifdef CROW_ENABLE_SSL
        void handle_upgrade(const request& req, response&, SSLAdaptor&& adaptor) override
        {
            new crow::websocket::Connection<SSLAdaptor>(req, std::move(adaptor), open_handler_, message_handler_, close_handler_, error_handler_, accept_handler_);
        }
#endif

        template <typename Func>
        self_t& onopen(Func f)
        {
            open_handler_ = f;
            return *this;
        }

        template <typename Func>
        self_t& onmessage(Func f)
        {
            message_handler_ = f;
            return *this;
        }

        template <typename Func>
        self_t& onclose(Func f)
        {
            close_handler_ = f;
            return *this;
        }

        template <typename Func>
        self_t& onerror(Func f)
        {
            error_handler_ = f;
            return *this;
        }

        template <typename Func>
        self_t& onaccept(Func f)
        {
            accept_handler_ = f;
            return *this;
        }

    protected:
        std::function<void(crow::websocket::connection&)> open_handler_;
        std::function<void(crow::websocket::connection&, const std::string&, bool)> message_handler_;
        std::function<void(crow::websocket::connection&, const std::string&)> close_handler_;
        std::function<void(crow::websocket::connection&)> error_handler_;
        std::function<bool(const crow::request&)> accept_handler_;
    };

    /// Allows the user to assign parameters using functions.
    ///
    /// `rule.name("name").methods(HTTPMethod::POST)`
    template <typename T>
    struct RuleParameterTraits
    {
        using self_t = T;
        WebSocketRule& websocket()
        {
            auto p =new WebSocketRule(static_cast<self_t*>(this)->rule_);
            static_cast<self_t*>(this)->rule_to_upgrade_.reset(p);
            return *p;
        }

        self_t& name(std::string name) noexcept
        {
            static_cast<self_t*>(this)->name_ = std::move(name);
            return static_cast<self_t&>(*this);
        }

        self_t& methods(HTTPMethod method)
        {
            static_cast<self_t*>(this)->methods_ = 1 << static_cast<int>(method);
            return static_cast<self_t&>(*this);
        }

        template <typename ... MethodArgs>
        self_t& methods(HTTPMethod method, MethodArgs ... args_method)
        {
            methods(args_method...);
            static_cast<self_t*>(this)->methods_ |= 1 << static_cast<int>(method);
            return static_cast<self_t&>(*this);
        }

    };

    /// A rule that can change its parameters during runtime.
    class DynamicRule : public BaseRule, public RuleParameterTraits<DynamicRule>
    {
    public:

        DynamicRule(std::string rule)
            : BaseRule(std::move(rule))
        {
        }

        void validate() override
        {
            if (!erased_handler_)
            {
                throw std::runtime_error(name_ + (!name_.empty() ? ": " : "") + "no handler for url " + rule_);
            }
        }

        void handle(const request& req, response& res, const routing_params& params) override
        {
            erased_handler_(req, res, params);
        }

        template <typename Func>
        void operator()(Func f)
        {
#ifdef CROW_MSVC_WORKAROUND
            using function_t = utility::function_traits<decltype(&Func::operator())>;
#else
            using function_t = utility::function_traits<Func>;
#endif
            erased_handler_ = wrap(std::move(f), black_magic::gen_seq<function_t::arity>());
        }

        // enable_if Arg1 == request && Arg2 == response
        // enable_if Arg1 == request && Arg2 != resposne
        // enable_if Arg1 != request
#ifdef CROW_MSVC_WORKAROUND
        template <typename Func, size_t ... Indices>
#else
        template <typename Func, unsigned ... Indices>
#endif
        std::function<void(const request&, response&, const routing_params&)>
        wrap(Func f, black_magic::seq<Indices...>)
        {
#ifdef CROW_MSVC_WORKAROUND
            using function_t = utility::function_traits<decltype(&Func::operator())>;
#else
            using function_t = utility::function_traits<Func>;
#endif
            if (!black_magic::is_parameter_tag_compatible(
                black_magic::get_parameter_tag_runtime(rule_.c_str()),
                black_magic::compute_parameter_tag_from_args_list<
                    typename function_t::template arg<Indices>...>::value))
            {
                throw std::runtime_error("route_dynamic: Handler type is mismatched with URL parameters: " + rule_);
            }
            auto ret = detail::routing_handler_call_helper::Wrapped<Func, typename function_t::template arg<Indices>...>();
            ret.template set_<
                typename function_t::template arg<Indices>...
            >(std::move(f));
            return ret;
        }

        template <typename Func>
        void operator()(std::string name, Func&& f)
        {
            name_ = std::move(name);
            (*this).template operator()<Func>(std::forward(f));
        }
    private:
        std::function<void(const request&, response&, const routing_params&)> erased_handler_;

    };

    /// Default rule created when CROW_ROUTE is called.
    template <typename ... Args>
    class TaggedRule : public BaseRule, public RuleParameterTraits<TaggedRule<Args...>>
    {
    public:
        using self_t = TaggedRule<Args...>;

        TaggedRule(std::string rule)
            : BaseRule(std::move(rule))
        {
        }

        void validate() override
        {
            if (!handler_)
            {
                throw std::runtime_error(name_ + (!name_.empty() ? ": " : "") + "no handler for url " + rule_);
            }
        }

        template <typename Func>
        typename std::enable_if<black_magic::CallHelper<Func, black_magic::S<Args...>>::value, void>::type
        operator()(Func&& f)
        {
            static_assert(black_magic::CallHelper<Func, black_magic::S<Args...>>::value ||
                black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value ,
                "Handler type is mismatched with URL parameters");
            static_assert(!std::is_same<void, decltype(f(std::declval<Args>()...))>::value,
                "Handler function cannot have void return type; valid return types: string, int, crow::response, crow::returnable");

            handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const request&, response& res, Args ... args){
                    res = response(f(args...));
                    res.end();
                });
        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<Args...>>::value &&
            black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value,
            void>::type
        operator()(Func&& f)
        {
            static_assert(black_magic::CallHelper<Func, black_magic::S<Args...>>::value ||
                black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value,
                "Handler type is mismatched with URL parameters");
            static_assert(!std::is_same<void, decltype(f(std::declval<crow::request>(), std::declval<Args>()...))>::value,
                "Handler function cannot have void return type; valid return types: string, int, crow::response, crow::returnable");

            handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const crow::request& req, crow::response& res, Args ... args){
                    res = response(f(req, args...));
                    res.end();
                });
        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<Args...>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value &&
            black_magic::CallHelper<Func, black_magic::S<crow::response&, Args...>>::value,
        void>::type
        operator()(Func&& f)
        {
          static_assert(black_magic::CallHelper<Func, black_magic::S<Args...>>::value ||
              black_magic::CallHelper<Func, black_magic::S<crow::response&, Args...>>::value
              ,
              "Handler type is mismatched with URL parameters");
          static_assert(std::is_same<void, decltype(f(std::declval<crow::response&>(), std::declval<Args>()...))>::value,
                        "Handler function with response argument should have void return type");
          handler_ = (
#ifdef CROW_CAN_USE_CPP14
                [f = std::move(f)]
#else
                [f]
#endif
                (const crow::request&, crow::response& res, Args ... args){
                  f(res, args...);
                });
        }

        template <typename Func>
        typename std::enable_if<
            !black_magic::CallHelper<Func, black_magic::S<Args...>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value &&
            !black_magic::CallHelper<Func, black_magic::S<crow::response&, Args...>>::value,
            void>::type
        operator()(Func&& f)
        {
            static_assert(black_magic::CallHelper<Func, black_magic::S<Args...>>::value ||
                black_magic::CallHelper<Func, black_magic::S<crow::request, Args...>>::value ||
                black_magic::CallHelper<Func, black_magic::S<crow::request, crow::response&, Args...>>::value
                ,
                "Handler type is mismatched with URL parameters");
            static_assert(std::is_same<void, decltype(f(std::declval<crow::request>(), std::declval<crow::response&>(), std::declval<Args>()...))>::value,
                "Handler function with response argument should have void return type");

                handler_ = std::move(f);
        }

        template <typename Func>
        void operator()(std::string name, Func&& f)
        {
            name_ = std::move(name);
            (*this).template operator()<Func>(std::forward(f));
        }

        void handle(const request& req, response& res, const routing_params& params) override
        {
            detail::routing_handler_call_helper::call<
                detail::routing_handler_call_helper::call_params<
                    decltype(handler_)>,
                0, 0, 0, 0,
                black_magic::S<Args...>,
                black_magic::S<>
            >()(
                detail::routing_handler_call_helper::call_params<
                    decltype(handler_)>
                {handler_, params, req, res}
            );
        }

    private:
        std::function<void(const crow::request&, crow::response&, Args...)> handler_;

    };

    const int RULE_SPECIAL_REDIRECT_SLASH = 1;

    /// A search tree.
    class Trie
    {
    public:
        struct Node
        {
            unsigned rule_index{};
            std::string key;
            ParamType param = ParamType::MAX; // MAX = No param.
            std::vector<Node*> children;

            bool IsSimpleNode() const
            {
                return
                    !rule_index &&
                    children.size() < 2 &&
                    param == ParamType::MAX &&
                    std::all_of(std::begin(children), std::end(children), [](Node* x){ return x->param == ParamType::MAX; });
            }
        };


        Trie()
        {
        }

        ///Check whether or not the trie is empty.
        bool is_empty()
        {
            return head_.children.empty();
        }

        void optimize()
        {
            for (auto child: head_.children)
            {
                optimizeNode(child);
            }
        }


    private:
        void optimizeNode(Node* node)
        {
            if (node->children.empty())
                return;
            if (node->IsSimpleNode())
            {
                Node* child_temp = node->children[0];
                node->key = node->key + child_temp->key;
                node->rule_index = child_temp->rule_index;
                node->children = std::move(child_temp->children);
                delete(child_temp);
                optimizeNode(node);
            }
            else
            {
                for(auto& child : node->children)
                {
                    optimizeNode(child);
                }
            }
        }

        void debug_node_print(Node* node, int level)
        {
            if (node->param != ParamType::MAX)
            {
                switch(node->param)
                {
                    case ParamType::INT:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<int>";
                        break;
                    case ParamType::UINT:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<uint>";
                        break;
                    case ParamType::DOUBLE:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<double>";
                        break;
                    case ParamType::STRING:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<string>";
                        break;
                    case ParamType::PATH:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<path>";
                        break;
                    default:
                        CROW_LOG_DEBUG << std::string(2*level, ' ') << "<ERROR>";
                        break;
                }
            }
            else
                CROW_LOG_DEBUG << std::string(2*level, ' ') << node->key;

            for(auto& child : node->children)
            {
                debug_node_print(child, level+1);
            }
        }
    public:

        void debug_print()
        {
            CROW_LOG_DEBUG << "HEAD";
            for (auto& child : head_.children)
                debug_node_print(child, 1);
        }

        void validate()
        {
            if (!head_.IsSimpleNode())
                throw std::runtime_error("Internal error: Trie header should be simple!");
            optimize();
        }

        std::pair<unsigned, routing_params> find(const std::string& req_url, const Node* node = nullptr, unsigned pos = 0, routing_params* params = nullptr) const
        {
            //start params as an empty struct
            routing_params empty;
            if (params == nullptr)
                params = &empty;

            unsigned found{}; //The rule index to be found
            routing_params match_params; //supposedly the final matched parameters

            //start from the head node
            if (node == nullptr)
                node = &head_;

            //if the function was called on a node at the end of the string (the last recursion), return the nodes rule index, and whatever params were passed to the function
            if (pos == req_url.size())
                return {node->rule_index, *params};

            auto update_found = [&found, &match_params](std::pair<unsigned, routing_params>& ret)
            {
                if (ret.first && (!found || found > ret.first))
                {
                    found = ret.first;
                    match_params = std::move(ret.second);
                }
            };


            for(auto& child : node->children)
            {
                if (child->param != ParamType::MAX)
                {
                    if (child->param == ParamType::INT)
                    {
                        char c = req_url[pos];
                        if ((c >= '0' && c <= '9') || c == '+' || c == '-')
                        {
                            char* eptr;
                            errno = 0;
                            long long int value = strtoll(req_url.data()+pos, &eptr, 10);
                            if (errno != ERANGE && eptr != req_url.data()+pos)
                            {
                                params->int_params.push_back(value);
                                auto ret = find(req_url, child, eptr - req_url.data(), params);
                                update_found(ret);
                                params->int_params.pop_back();
                            }
                        }
                    }

                    else if (child->param == ParamType::UINT)
                    {
                        char c = req_url[pos];
                        if ((c >= '0' && c <= '9') || c == '+')
                        {
                            char* eptr;
                            errno = 0;
                            unsigned long long int value = strtoull(req_url.data()+pos, &eptr, 10);
                            if (errno != ERANGE && eptr != req_url.data()+pos)
                            {
                                params->uint_params.push_back(value);
                                auto ret = find(req_url, child, eptr - req_url.data(), params);
                                update_found(ret);
                                params->uint_params.pop_back();
                            }
                        }
                    }

                    else if (child->param == ParamType::DOUBLE)
                    {
                        char c = req_url[pos];
                        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')
                        {
                            char* eptr;
                            errno = 0;
                            double value = strtod(req_url.data()+pos, &eptr);
                            if (errno != ERANGE && eptr != req_url.data()+pos)
                            {
                                params->double_params.push_back(value);
                                auto ret = find(req_url, child, eptr - req_url.data(), params);
                                update_found(ret);
                                params->double_params.pop_back();
                            }
                        }
                    }

                    else if (child->param == ParamType::STRING)
                    {
                        size_t epos = pos;
                        for(; epos < req_url.size(); epos ++)
                        {
                            if (req_url[epos] == '/')
                                break;
                        }

                        if (epos != pos)
                        {
                            params->string_params.push_back(req_url.substr(pos, epos-pos));
                            auto ret = find(req_url, child, epos, params);
                            update_found(ret);
                            params->string_params.pop_back();
                        }
                    }

                    else if (child->param == ParamType::PATH)
                    {
                        size_t epos = req_url.size();

                        if (epos != pos)
                        {
                            params->string_params.push_back(req_url.substr(pos, epos-pos));
                            auto ret = find(req_url, child, epos, params);
                            update_found(ret);
                            params->string_params.pop_back();
                        }
                    }
                }

                else
                {
                    const std::string& fragment = child->key;
                    if (req_url.compare(pos, fragment.size(), fragment) == 0)
                    {
                        auto ret = find(req_url, child, pos + fragment.size(), params);
                        update_found(ret);
                    }
                }
            }
            return {found, match_params}; //Called after all the recursions have been done
        }

        void add(const std::string& url, unsigned rule_index)
        {
            Node* idx = &head_;

            for(unsigned i = 0; i < url.size(); i ++)
            {
                char c = url[i];
                if (c == '<')
                {
                    static struct ParamTraits
                    {
                        ParamType type;
                        std::string name;
                    } paramTraits[] =
                    {
                        { ParamType::INT, "<int>" },
                        { ParamType::UINT, "<uint>" },
                        { ParamType::DOUBLE, "<float>" },
                        { ParamType::DOUBLE, "<double>" },
                        { ParamType::STRING, "<str>" },
                        { ParamType::STRING, "<string>" },
                        { ParamType::PATH, "<path>" },
                    };

                    for(auto& x:paramTraits)
                    {
                        if (url.compare(i, x.name.size(), x.name) == 0)
                        {
                            bool found = false;
                            for (Node* child : idx->children)
                            {
                                if (child->param == x.type)
                                {
                                    idx = child;
                                    i += x.name.size();
                                    found = true;
                                    break;
                                }
                            }
                            if (found)
                                break;

                            auto new_node_idx = new_node(idx);
                            new_node_idx->param = x.type;
                            idx = new_node_idx;
                            i += x.name.size();
                            break;
                        }
                    }

                    i --;
                }
                else
                {
                    //This part assumes the tree is unoptimized (every node has a max 1 character key)
                    bool piece_found = false;
                    for (auto& child : idx->children)
                    {
                        if (child->key[0] == c)
                        {
                            idx = child;
                            piece_found = true;
                            break;
                        }
                    }
                    if (!piece_found)
                    {
                        auto new_node_idx = new_node(idx);
                        new_node_idx->key = c;
                        idx = new_node_idx;
                    }
                }
            }

            //check if the last node already has a value (exact url already in Trie)
            if (idx->rule_index)
                throw std::runtime_error("handler already exists for " + url);
            idx->rule_index = rule_index;
        }

        size_t get_size()
        {
            return get_size(&head_);
        }

        size_t get_size(Node* node)
        {
            unsigned size = 8; //rule_index and param
            size += (node->key.size()); //each character in the key is 1 byte
            for (auto child: node->children)
            {
                size += get_size(child);
            }
            return size;
        }


    private:

        Node* new_node(Node* parent)
        {
            auto& children = parent->children;
            children.resize(children.size()+1);
            children[children.size()-1] = new Node();
            return children[children.size()-1];
        }

        Node head_;
    };


    /// Handles matching requests to existing rules and upgrade requests.
    class Router
    {
    public:
        Router()
        {
        }

        DynamicRule& new_rule_dynamic(const std::string& rule)
        {
            auto ruleObject = new DynamicRule(rule);
            all_rules_.emplace_back(ruleObject);

            return *ruleObject;
        }

        template <uint64_t N>
        typename black_magic::arguments<N>::type::template rebind<TaggedRule>& new_rule_tagged(const std::string& rule)
        {
            using RuleT = typename black_magic::arguments<N>::type::template rebind<TaggedRule>;

            auto ruleObject = new RuleT(rule);
            all_rules_.emplace_back(ruleObject);

            return *ruleObject;
        }

        CatchallRule& catchall_rule()
        {
            return catchall_rule_;
        }

        void internal_add_rule_object(const std::string& rule, BaseRule* ruleObject)
        {
            bool has_trailing_slash = false;
            std::string rule_without_trailing_slash;
            if (rule.size() > 1 && rule.back() == '/')
            {
                has_trailing_slash = true;
                rule_without_trailing_slash = rule;
                rule_without_trailing_slash.pop_back();
            }

            ruleObject->foreach_method([&](int method)
                    {
                        per_methods_[method].rules.emplace_back(ruleObject);
                        per_methods_[method].trie.add(rule, per_methods_[method].rules.size() - 1);

                        // directory case:
                        //   request to '/about' url matches '/about/' rule
                        if (has_trailing_slash)
                        {
                            per_methods_[method].trie.add(rule_without_trailing_slash, RULE_SPECIAL_REDIRECT_SLASH);
                        }
                    });

        }

        void validate()
        {
            for(auto& rule:all_rules_)
            {
                if (rule)
                {
                    auto upgraded = rule->upgrade();
                    if (upgraded)
                        rule = std::move(upgraded);
                    rule->validate();
                    internal_add_rule_object(rule->rule(), rule.get());
                }
            }
            for(auto& per_method:per_methods_)
            {
                per_method.trie.validate();
            }
        }

        //TODO maybe add actual_method
        template <typename Adaptor>
        void handle_upgrade(const request& req, response& res, Adaptor&& adaptor)
        {
            if (req.method >= HTTPMethod::InternalMethodCount)
                return;

            auto& per_method = per_methods_[static_cast<int>(req.method)];
            auto& rules = per_method.rules;
            unsigned rule_index = per_method.trie.find(req.url).first;

            if (!rule_index)
            {
                for (auto& per_method: per_methods_)
                {
                    if (per_method.trie.find(req.url).first)
                    {
                        CROW_LOG_DEBUG << "Cannot match method " << req.url << " " << method_name(req.method);
                        res = response(405);
                        res.end();
                        return;
                    }
                }

                CROW_LOG_INFO << "Cannot match rules " << req.url;
                res = response(404);
                res.end();
                return;
            }

            if (rule_index >= rules.size())
                throw std::runtime_error("Trie internal structure corrupted!");

            if (rule_index == RULE_SPECIAL_REDIRECT_SLASH)
            {
                CROW_LOG_INFO << "Redirecting to a url with trailing slash: " << req.url;
                res = response(301);

                // TODO absolute url building
                if (req.get_header_value("Host").empty())
                {
                    res.add_header("Location", req.url + "/");
                }
                else
                {
                    res.add_header("Location", "http://" + req.get_header_value("Host") + req.url + "/");
                }
                res.end();
                return;
            }

            CROW_LOG_DEBUG << "Matched rule (upgrade) '" << rules[rule_index]->rule_ << "' " << static_cast<uint32_t>(req.method) << " / " << rules[rule_index]->get_methods();

            // any uncaught exceptions become 500s
            try
            {
                rules[rule_index]->handle_upgrade(req, res, std::move(adaptor));
            }
            catch(std::exception& e)
            {
                CROW_LOG_ERROR << "An uncaught exception occurred: " << e.what();
                res = response(500);
                res.end();
                return;
            }
            catch(...)
            {
                CROW_LOG_ERROR << "An uncaught exception occurred. The type was unknown so no information was available.";
                res = response(500);
                res.end();
                return;
            }
        }

        void handle(const request& req, response& res)
        {
            HTTPMethod method_actual = req.method;
            if (req.method >= HTTPMethod::InternalMethodCount)
                return;
            else if (req.method == HTTPMethod::Head)
            {
                method_actual = HTTPMethod::Get;
                res.is_head_response = true;
            }
            else if (req.method == HTTPMethod::Options)
            {
                std::string allow = "OPTIONS, HEAD, ";

                if (req.url == "/*")
                {
                    for(int i = 0; i < static_cast<int>(HTTPMethod::InternalMethodCount); i ++)
                    {
                        if (!per_methods_[i].trie.is_empty())
                        {
                            allow += method_name(static_cast<HTTPMethod>(i)) + ", ";
                        }
                    }
                        allow = allow.substr(0, allow.size()-2);
                        res = response(204);
                        res.set_header("Allow", allow);
                        res.manual_length_header = true;
                        res.end();
                        return;
                }
                else
                {
                    for(int i = 0; i < static_cast<int>(HTTPMethod::InternalMethodCount); i ++)
                    {
                        if (per_methods_[i].trie.find(req.url).first)
                        {
                            allow += method_name(static_cast<HTTPMethod>(i)) + ", ";
                        }
                    }
                    if (allow != "OPTIONS, HEAD, ")
                    {
                        allow = allow.substr(0, allow.size()-2);
                        res = response(204);
                        res.set_header("Allow", allow);
                        res.manual_length_header = true;
                        res.end();
                        return;
                    }
                    else
                    {
                        CROW_LOG_DEBUG << "Cannot match rules " << req.url;
                        res = response(404);
                        res.end();
                        return;
                    }
                }
            }

            auto& per_method = per_methods_[static_cast<int>(method_actual)];
            auto& trie = per_method.trie;
            auto& rules = per_method.rules;

            auto found = trie.find(req.url);

            unsigned rule_index = found.first;

            if (!rule_index)
            {
                for (auto& per_method: per_methods_)
                {
                    if (per_method.trie.find(req.url).first)
                    {
                        CROW_LOG_DEBUG << "Cannot match method " << req.url << " " << method_name(method_actual);
                        res = response(405);
                        res.end();
                        return;
                    }
                }

                if (catchall_rule_.has_handler())
                {
                    CROW_LOG_DEBUG << "Cannot match rules " << req.url << ". Redirecting to Catchall rule";
                    catchall_rule_.handler_(req, res);
                }
                else
                {
                    CROW_LOG_DEBUG << "Cannot match rules " << req.url;
                    res = response(404);
                }
                res.end();
                return;
            }

            if (rule_index >= rules.size())
                throw std::runtime_error("Trie internal structure corrupted!");

            if (rule_index == RULE_SPECIAL_REDIRECT_SLASH)
            {
                CROW_LOG_INFO << "Redirecting to a url with trailing slash: " << req.url;
                res = response(301);

                // TODO absolute url building
                if (req.get_header_value("Host").empty())
                {
                    res.add_header("Location", req.url + "/");
                }
                else
                {
                    res.add_header("Location", "http://" + req.get_header_value("Host") + req.url + "/");
                }
                res.end();
                return;
            }

            CROW_LOG_DEBUG << "Matched rule '" << rules[rule_index]->rule_ << "' " << static_cast<uint32_t>(req.method) << " / " << rules[rule_index]->get_methods();

            // any uncaught exceptions become 500s
            try
            {
                rules[rule_index]->handle(req, res, found.second);
            }
            catch(std::exception& e)
            {
                CROW_LOG_ERROR << "An uncaught exception occurred: " << e.what();
                res = response(500);
                res.end();
                return;
            }
            catch(...)
            {
                CROW_LOG_ERROR << "An uncaught exception occurred. The type was unknown so no information was available.";
                res = response(500);
                res.end();
                return;
            }
        }

        void debug_print()
        {
            for(int i = 0; i < static_cast<int>(HTTPMethod::InternalMethodCount); i ++)
            {
                CROW_LOG_DEBUG << method_name(static_cast<HTTPMethod>(i));
                per_methods_[i].trie.debug_print();
            }
        }

    private:
        CatchallRule catchall_rule_;

        struct PerMethod
        {
            std::vector<BaseRule*> rules;
            Trie trie;

            // rule index 0, 1 has special meaning; preallocate it to avoid duplication.
            PerMethod() : rules(2) {}
        };
        std::array<PerMethod, static_cast<int>(HTTPMethod::InternalMethodCount)> per_methods_;
        std::vector<std::unique_ptr<BaseRule>> all_rules_;

    };
}
