#ifndef COCAINE_TRACK_HPP
#define COCAINE_TRACK_HPP

#include <algorithm>

namespace cocaine { namespace helpers {
    template<typename T, void (*D)(T)> struct track {
        public:
            track(T object):
                m_object(object)
            {}

            ~track() {
                destroy();
            }

            inline track<T, D>& operator=(T object) {
                destroy();
                m_object = object;
                return *this;
            }

            inline track<T, D>& operator=(track<T, D>& other) {
                destroy();
                m_object = other.release();
                return *this;
            } 

            inline T operator*() {
                return m_object;
            }

            inline const T operator*() const {
                return m_object;
            }

            inline T* operator&() {
                return &m_object;
            }

            inline const T* operator&() const {
                return &m_object;
            }

            inline T operator->() {
                return m_object;
            }

            inline const T operator->() const {
                return m_object;
            }

            inline operator T() {
                return m_object;
            }

            inline operator const T() const {
                return m_object;
            }

            inline bool valid() const {
                return (m_object != NULL);
            }

            inline T release() {
                T tmp = NULL;
                std::swap(tmp, m_object);
                return tmp;
            }

        private:
            void destroy() {
                if(m_object) {
                    D(m_object);
                }
            }

            T m_object;
    };
}}

#endif
