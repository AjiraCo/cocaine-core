/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef COCAINE_IO_DISPATCH_HPP
#define COCAINE_IO_DISPATCH_HPP

#include "cocaine/common.hpp"
#include "cocaine/hpack/header.hpp"
#include "cocaine/locked_ptr.hpp"
#include "cocaine/rpc/basic_dispatch.hpp"
#include "cocaine/rpc/slot/blocking.hpp"
#include "cocaine/rpc/slot/deferred.hpp"
#include "cocaine/rpc/slot/generic.hpp"
#include "cocaine/rpc/slot/streamed.hpp"
#include "cocaine/rpc/traversal.hpp"
#include "cocaine/traits/tuple.hpp"
#include "cocaine/utility/exchange.hpp"

#include <boost/mpl/transform.hpp>
#include <boost/mpl/lambda.hpp>

#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>
#include <boost/variant/variant.hpp>

#include <type_traits>
#include <vector>

namespace cocaine {

template<typename MetaFlag>
struct initial_state;

template<typename F, typename R>
struct executor_state;

template<typename Event, typename State = initial_state<void>>
struct slot_builder;

template<class Tag>
class dispatch:
    public io::basic_dispatch_t
{
    static const io::graph_root_t kProtocol;

    // Slot construction

    typedef typename boost::mpl::transform<
        typename io::messages<Tag>::type,
        typename boost::mpl::lambda<
            std::shared_ptr<io::basic_slot<boost::mpl::_1>>
        >::type
    >::type slot_types;

    typedef std::map<
        int,
        typename boost::make_variant_over<slot_types>::type
    > slot_map_t;

    synchronized<slot_map_t> m_slots;

    // Slot traits

    template<class T, class Event>
    struct is_slot: public std::false_type {};

    template<class T, class Event>
    struct is_slot<std::shared_ptr<T>, Event>: public std::is_base_of<io::basic_slot<Event>, T> {};

public:
    explicit
    dispatch(const std::string& name):
        basic_dispatch_t(name)
    {}

    template<class Event>
    slot_builder<Event>
    on();

    template<class Event, class F>
    dispatch<Tag>&
    on(F&& fn, typename boost::disable_if<is_slot<F, Event>>::type* = 0);

    template<class Event>
    dispatch&
    on(const std::shared_ptr<io::basic_slot<Event>>& ptr);

    template<class Event>
    void
    drop();

    void
    halt();

public:
    virtual
    boost::optional<io::dispatch_ptr_t>
    process(const io::decoder_t::message_type& message, const io::upstream_ptr_t& upstream) const;

    virtual
    auto
    root() const -> const io::graph_root_t& {
        return kProtocol;
    }

    virtual
    auto
    version() const -> int {
        return io::protocol<Tag>::version::value;
    }

    // Generic API

    template<class Visitor>
    auto
    process(int id, const Visitor& visitor) const -> typename Visitor::result_type;
};

template<class Tag>
const io::graph_root_t dispatch<Tag>::kProtocol = io::traverse<Tag>().get();

namespace aux {

// Slot selection

template<class R, class Event, class ForwardMeta>
struct select {
    typedef io::blocking_slot<Event, ForwardMeta> type;
};

template<class R, class Event, class ForwardMeta>
struct select<deferred<R>, Event, ForwardMeta> {
    typedef io::deferred_slot<deferred, Event, ForwardMeta> type;
};

template<class R, class Event, class ForwardMeta>
struct select<streamed<R>, Event, ForwardMeta> {
    typedef io::deferred_slot<streamed, Event, ForwardMeta> type;
};

template<class Event, class ForwardMeta>
struct select<typename io::basic_slot<Event>::result_type, Event, ForwardMeta> {
    typedef io::generic_slot<Event> type;
};

// Slot invocation with arguments provided as a MessagePack object

struct calling_visitor_t:
    public boost::static_visitor<boost::optional<io::dispatch_ptr_t>>
{
    calling_visitor_t(const std::vector<hpack::header_t>& headers_,
                      const msgpack::object& unpacked_,
                      const io::upstream_ptr_t& upstream_):
        headers(headers_),
        unpacked(unpacked_),
        upstream(upstream_)
    { }

    template<class Event>
    result_type
    operator()(const std::shared_ptr<io::basic_slot<Event>>& slot) const {
        typedef io::basic_slot<Event> slot_type;

        // Unpacked arguments storage.
        typename slot_type::tuple_type args;

        try {
            // NOTE: Unpacks the object into a tuple using the argument typelist unlike using plain
            // tuple type traits, in order to support parameter tags, like optional<T>.
            io::type_traits<typename io::event_traits<Event>::argument_type>::unpack(unpacked, args);
        } catch(const msgpack::type_error& e) {
            throw std::system_error(error::invalid_argument, e.what());
        }

        // Call the slot with the upstream constrained with the event's upstream protocol type tag.
        return result_type((*slot)(headers, std::move(args), typename slot_type::upstream_type(upstream)));
    }

private:
    const std::vector<hpack::header_t>& headers;
    const msgpack::object& unpacked;
    const io::upstream_ptr_t& upstream;
};

struct forward_meta_tag;

/// Wraps the given slot of type `F` eating meta argument depending on `MetaFlag`.
template<typename F, typename MetaFlag>
struct slot_wrapper;

template<typename F>
struct slot_wrapper<F, std::true_type> {
    F fn;

    explicit
    slot_wrapper(F fn) :
        fn(std::move(fn))
    {}

    template<typename... Args>
    auto operator()(const std::vector<hpack::header_t>& meta, Args&&... args) ->
        decltype(fn(meta, std::forward<Args>(args)...))
    {
        return fn(meta, std::forward<Args>(args)...);
    }
};

template<typename F>
struct slot_wrapper<F, std::false_type> {
    F fn;

    explicit
    slot_wrapper(F fn) :
        fn(std::move(fn))
    {}

    template<typename... Args>
    auto operator()(const std::vector<hpack::header_t>&, Args&&... args) ->
        decltype(fn(std::forward<Args>(args)...))
    {
        return fn(std::forward<Args>(args)...);
    }
};

} // namespace aux

template<typename Event, typename MetaFlag>
struct slot_builder<Event, initial_state<MetaFlag>> {
    typedef Event event_type;
    typedef typename event_type::tag tag_type;
    typedef typename std::is_same<MetaFlag, aux::forward_meta_tag>::type forward_meta;

    dispatch<tag_type>& d;

    explicit
    slot_builder(dispatch<tag_type>& d) :
        d(d)
    {}

    /// Consumes this builder, enabling forwarding frame meta information to the further event
    /// handler.
    auto provide_meta() && -> slot_builder<event_type, initial_state<aux::forward_meta_tag>> {
        return slot_builder<event_type, initial_state<aux::forward_meta_tag>>(std::move(*this).d);
    }

    /// Consumes this builder, setting the event handler.
    ///
    /// \param fn Event handler which will be invoked each time the new event comes.
    ///     Depending on previously called `provide_meta` method a handler may or may not accept
    ///     additional argument with meta information (aka headers).
    template<typename F>
    auto execute(F fn) && ->
        slot_builder<event_type, executor_state<aux::slot_wrapper<F, forward_meta>, typename result_of<F>::type>>
    {
        return slot_builder<event_type, executor_state<aux::slot_wrapper<F, forward_meta>, typename result_of<F>::type>>(
            aux::slot_wrapper<F, forward_meta>(std::move(fn)),
            &d
        );
    }
};

template<typename Event, typename F, typename R>
struct slot_builder<Event, executor_state<F, R>> {
    typedef Event event_type;
    typedef typename event_type::tag tag_type;

    F fn;
    dispatch<tag_type>* d;

    slot_builder(F fn, dispatch<tag_type>* d) :
        fn(std::move(fn)),
        d(d)
    {}

    ~slot_builder() noexcept(false) {
        typedef typename aux::select<
            R,
            event_type,
            std::true_type
        >::type slot_type;

        if (d) {
            d->template on<event_type>(std::make_shared<slot_type>(std::move(fn)));
        }
    }

    template<typename M, typename C>
    struct compose {
        // Middleware callable.
        M middleware;

        // Either the previous composed middleware or slot.
        C fn;

        compose(M middleware, C fn) :
            middleware(std::move(middleware)),
            fn(std::move(fn))
        {}

        template<typename... Args>
        auto operator()(Args&&... args) ->
            decltype(fn(std::forward<Args>(args)...))
        {
            return middleware(fn, Event(), std::forward<Args>(args)...);
        }
    };

    /// Consumes this builder, adding a new middleware.
    ///
    /// \tparam M Must satisfy Middleware concept.
    template<typename M>
    auto add_middleware(M&& middleware) && ->
        slot_builder<event_type, executor_state<compose<M, F>, R>>
    {
        return slot_builder<event_type, executor_state<compose<M, F>, R>>(
            compose<M, F>(std::forward<M>(middleware), std::move(fn)),
            utility::exchange(d, nullptr)
        );
    }
};

template<class Tag>
template<class Event, class F>
dispatch<Tag>&
dispatch<Tag>::on(F&& fn, typename boost::disable_if<is_slot<F, Event>>::type*) {
    typedef typename aux::select<
        typename result_of<F>::type,
        Event,
        std::false_type
    >::type slot_type;

    return on<Event>(std::make_shared<slot_type>(std::forward<F>(fn)));
}

template<class Tag>
template<class Event>
dispatch<Tag>&
dispatch<Tag>::on(const std::shared_ptr<io::basic_slot<Event>>& ptr) {
    typedef io::event_traits<Event> traits;

    if(!m_slots->insert(std::make_pair(traits::id, ptr)).second) {
        throw std::system_error(error::duplicate_slot, Event::alias());
    }

    return *this;
}

template<class Tag>
template<class Event>
slot_builder<Event, initial_state<void>>
dispatch<Tag>::on() {
    return slot_builder<Event>(*this);
}

template<class Tag>
template<class Event>
void
dispatch<Tag>::drop() {
    if(!m_slots->erase(io::event_traits<Event>::id)) {
        throw std::system_error(error::slot_not_found, Event::alias());
    }
}

template<class Tag>
void
dispatch<Tag>::halt() {
    m_slots->clear();
}

template<class Tag>
boost::optional<io::dispatch_ptr_t>
dispatch<Tag>::process(const io::decoder_t::message_type& message, const io::upstream_ptr_t& upstream) const {
    return process(message.type(), aux::calling_visitor_t(message.headers(), message.args(), upstream));
}

template<class Tag>
template<class Visitor>
typename Visitor::result_type
dispatch<Tag>::process(int id, const Visitor& visitor) const {
    typedef typename slot_map_t::mapped_type slot_ptr_type;

    const auto slot = m_slots.apply([&](const slot_map_t& mapping) -> slot_ptr_type {
        typename slot_map_t::const_iterator lb, ub;

        // NOTE: Using equal_range() here, instead of find() to check for slot existence and get the
        // slot pointer in one call instead of two.
        std::tie(lb, ub) = mapping.equal_range(id);

        if(lb != ub) {
            // NOTE: The slot pointer is copied here, allowing the handling code to unregister slots
            // via dispatch<T>::forget() without pulling the object from underneath itself.
            return lb->second;
        } else {
            throw std::system_error(error::slot_not_found);
        }
    });

    return boost::apply_visitor(visitor, slot);
}

} // namespace cocaine

#endif
