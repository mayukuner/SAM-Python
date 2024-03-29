/*
    pybind11/cast.h: Partial template specializations to cast between
    C++ and Python types

    Copyright (c) 2016 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#include "pytypes.h"
#include "typeid.h"
#include "descr.h"
#include <array>
#include <limits>

NAMESPACE_BEGIN(pybind11)
NAMESPACE_BEGIN(detail)
inline PyTypeObject *make_static_property_type();
inline PyTypeObject *make_default_metaclass();

/// Additional type information which does not fit into the PyTypeObject
struct type_info {
    PyTypeObject *type;
    const std::type_info *cpptype;
    size_t type_size;
    void *(*operator_new)(size_t);
    void (*init_holder)(PyObject *, const void *);
    void (*dealloc)(PyObject *);
    std::vector<PyObject *(*)(PyObject *, PyTypeObject *)> implicit_conversions;
    std::vector<std::pair<const std::type_info *, void *(*)(void *)>> implicit_casts;
    std::vector<bool (*)(PyObject *, void *&)> *direct_conversions;
    buffer_info *(*get_buffer)(PyObject *, void *) = nullptr;
    void *get_buffer_data = nullptr;
    /** A simple type never occurs as a (direct or indirect) parent
     * of a class that makes use of multiple inheritance */
    bool simple_type : 1;
    /* True if there is no multiple inheritance in this type's inheritance tree */
    bool simple_ancestors : 1;
    /* for base vs derived holder_type checks */
    bool default_holder : 1;
};

// Store the static internals pointer in a version-specific function so that we're guaranteed it
// will be distinct for modules compiled for different pybind11 versions.  Without this, some
// compilers (i.e. gcc) can use the same static pointer storage location across different .so's,
// even though the `get_internals()` function itself is local to each shared object.
template <int = PYBIND11_VERSION_MAJOR, int = PYBIND11_VERSION_MINOR>
internals *&get_internals_ptr() { static internals *internals_ptr = nullptr; return internals_ptr; }

PYBIND11_NOINLINE inline internals &get_internals() {
    internals *&internals_ptr = get_internals_ptr();
    if (internals_ptr)
        return *internals_ptr;
    handle builtins(PyEval_GetBuiltins());
    const char *id = PYBIND11_INTERNALS_ID;
    if (builtins.contains(id) && isinstance<capsule>(builtins[id])) {
        internals_ptr = capsule(builtins[id]);
    } else {
        internals_ptr = new internals();
        #if defined(WITH_THREAD)
            PyEval_InitThreads();
            PyThreadState *tstate = PyThreadState_Get();
            internals_ptr->tstate = PyThread_create_key();
            PyThread_set_key_value(internals_ptr->tstate, tstate);
            internals_ptr->istate = tstate->interp;
        #endif
        builtins[id] = capsule(internals_ptr);
        internals_ptr->registered_exception_translators.push_front(
            [](std::exception_ptr p) -> void {
                try {
                    if (p) std::rethrow_exception(p);
                } catch (error_already_set &e)           { e.restore();                                    return;
                } catch (const builtin_exception &e)     { e.set_error();                                  return;
                } catch (const std::bad_alloc &e)        { PyErr_SetString(PyExc_MemoryError,   e.what()); return;
                } catch (const std::domain_error &e)     { PyErr_SetString(PyExc_ValueError,    e.what()); return;
                } catch (const std::invalid_argument &e) { PyErr_SetString(PyExc_ValueError,    e.what()); return;
                } catch (const std::length_error &e)     { PyErr_SetString(PyExc_ValueError,    e.what()); return;
                } catch (const std::out_of_range &e)     { PyErr_SetString(PyExc_IndexError,    e.what()); return;
                } catch (const std::range_error &e)      { PyErr_SetString(PyExc_ValueError,    e.what()); return;
                } catch (const std::exception &e)        { PyErr_SetString(PyExc_RuntimeError,  e.what()); return;
                } catch (...) {
                    PyErr_SetString(PyExc_RuntimeError, "Caught an unknown exception!");
                    return;
                }
            }
        );
        internals_ptr->static_property_type = make_static_property_type();
        internals_ptr->default_metaclass = make_default_metaclass();
    }
    return *internals_ptr;
}

PYBIND11_NOINLINE inline detail::type_info* get_type_info(PyTypeObject *type) {
    auto const &type_dict = get_internals().registered_types_py;
    do {
        auto it = type_dict.find(type);
        if (it != type_dict.end())
            return (detail::type_info *) it->second;
        type = type->tp_base;
        if (!type)
            return nullptr;
    } while (true);
}

PYBIND11_NOINLINE inline detail::type_info *get_type_info(const std::type_info &tp,
                                                          bool throw_if_missing = false) {
    auto &types = get_internals().registered_types_cpp;

    auto it = types.find(std::type_index(tp));
    if (it != types.end())
        return (detail::type_info *) it->second;
    if (throw_if_missing) {
        std::string tname = tp.name();
        detail::clean_type_id(tname);
        pybind11_fail("pybind11::detail::get_type_info: unable to find type info for \"" + tname + "\"");
    }
    return nullptr;
}

PYBIND11_NOINLINE inline handle get_type_handle(const std::type_info &tp, bool throw_if_missing) {
    detail::type_info *type_info = get_type_info(tp, throw_if_missing);
    return handle(type_info ? ((PyObject *) type_info->type) : nullptr);
}

PYBIND11_NOINLINE inline bool isinstance_generic(handle obj, const std::type_info &tp) {
    handle type = detail::get_type_handle(tp, false);
    if (!type)
        return false;
    return isinstance(obj, type);
}

PYBIND11_NOINLINE inline std::string error_string() {
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "Unknown internal error occurred");
        return "Unknown internal error occurred";
    }

    error_scope scope; // Preserve error state

    std::string errorString;
    if (scope.type) {
        errorString += handle(scope.type).attr("__name__").cast<std::string>();
        errorString += ": ";
    }
    if (scope.value)
        errorString += (std::string) str(scope.value);

    PyErr_NormalizeException(&scope.type, &scope.value, &scope.trace);

#if PY_MAJOR_VERSION >= 3
    if (scope.trace != nullptr)
        PyException_SetTraceback(scope.value, scope.trace);
#endif

#if !defined(PYPY_VERSION)
    if (scope.trace) {
        PyTracebackObject *trace = (PyTracebackObject *) scope.trace;

        /* Get the deepest trace possible */
        while (trace->tb_next)
            trace = trace->tb_next;

        PyFrameObject *frame = trace->tb_frame;
        errorString += "\n\nAt:\n";
        while (frame) {
            int lineno = PyFrame_GetLineNumber(frame);
            errorString +=
                "  " + handle(frame->f_code->co_filename).cast<std::string>() +
                "(" + std::to_string(lineno) + "): " +
                handle(frame->f_code->co_name).cast<std::string>() + "\n";
            frame = frame->f_back;
        }
        trace = trace->tb_next;
    }
#endif

    return errorString;
}

PYBIND11_NOINLINE inline handle get_object_handle(const void *ptr, const detail::type_info *type ) {
    auto &instances = get_internals().registered_instances;
    auto range = instances.equal_range(ptr);
    for (auto it = range.first; it != range.second; ++it) {
        auto instance_type = detail::get_type_info(Py_TYPE(it->second));
        if (instance_type && instance_type == type)
            return handle((PyObject *) it->second);
    }
    return handle();
}

inline PyThreadState *get_thread_state_unchecked() {
#if defined(PYPY_VERSION)
    return PyThreadState_GET();
#elif PY_VERSION_HEX < 0x03000000
    return _PyThreadState_Current;
#elif PY_VERSION_HEX < 0x03050000
    return (PyThreadState*) _Py_atomic_load_relaxed(&_PyThreadState_Current);
#elif PY_VERSION_HEX < 0x03050200
    return (PyThreadState*) _PyThreadState_Current.value;
#else
    return _PyThreadState_UncheckedGet();
#endif
}

// Forward declarations
inline void keep_alive_impl(handle nurse, handle patient);
inline void register_instance(void *self, const type_info *tinfo);
inline PyObject *make_new_instance(PyTypeObject *type, bool allocate_value = true);

class type_caster_generic {
public:
    PYBIND11_NOINLINE type_caster_generic(const std::type_info &type_info)
     : typeinfo(get_type_info(type_info)) { }

    PYBIND11_NOINLINE bool load(handle src, bool convert) {
        if (!src)
            return false;
        return load(src, convert, Py_TYPE(src.ptr()));
    }

    bool load(handle src, bool convert, PyTypeObject *tobj) {
        if (!src || !typeinfo)
            return false;
        if (src.is_none()) {
            // Defer accepting None to other overloads (if we aren't in convert mode):
            if (!convert) return false;
            value = nullptr;
            return true;
        }

        if (typeinfo->simple_type) { /* Case 1: no multiple inheritance etc. involved */
            /* Check if we can safely perform a reinterpret-style cast */
            if (PyType_IsSubtype(tobj, typeinfo->type)) {
                value = reinterpret_cast<instance<void> *>(src.ptr())->value;
                return true;
            }
        } else { /* Case 2: multiple inheritance */
            /* Check if we can safely perform a reinterpret-style cast */
            if (tobj == typeinfo->type) {
                value = reinterpret_cast<instance<void> *>(src.ptr())->value;
                return true;
            }

            /* If this is a python class, also check the parents recursively */
            auto const &type_dict = get_internals().registered_types_py;
            bool new_style_class = PyType_Check((PyObject *) tobj);
            if (type_dict.find(tobj) == type_dict.end() && new_style_class && tobj->tp_bases) {
                auto parents = reinterpret_borrow<tuple>(tobj->tp_bases);
                for (handle parent : parents) {
                    bool result = load(src, convert, (PyTypeObject *) parent.ptr());
                    if (result)
                        return true;
                }
            }

            /* Try implicit casts */
            for (auto &cast : typeinfo->implicit_casts) {
                type_caster_generic sub_caster(*cast.first);
                if (sub_caster.load(src, convert)) {
                    value = cast.second(sub_caster.value);
                    return true;
                }
            }
        }

        /* Perform an implicit conversion */
        if (convert) {
            for (auto &converter : typeinfo->implicit_conversions) {
                temp = reinterpret_steal<object>(converter(src.ptr(), typeinfo->type));
                if (load(temp, false))
                    return true;
            }
            for (auto &converter : *typeinfo->direct_conversions) {
                if (converter(src.ptr(), value))
                    return true;
            }
        }
        return false;
    }

    PYBIND11_NOINLINE static handle cast(const void *_src, return_value_policy policy, handle parent,
                                         const detail::type_info *tinfo,
                                         void *(*copy_constructor)(const void *),
                                         void *(*move_constructor)(const void *),
                                         const void *existing_holder = nullptr) {
        if (!tinfo) // no type info: error will be set already
            return handle();

        void *src = const_cast<void *>(_src);
        if (src == nullptr)
            return none().release();

        auto it_instances = get_internals().registered_instances.equal_range(src);
        for (auto it_i = it_instances.first; it_i != it_instances.second; ++it_i) {
            auto instance_type = detail::get_type_info(Py_TYPE(it_i->second));
            if (instance_type && instance_type == tinfo)
                return handle((PyObject *) it_i->second).inc_ref();
        }

        auto inst = reinterpret_steal<object>(make_new_instance(tinfo->type, false /* don't allocate value */));

        auto wrapper = (instance<void> *) inst.ptr();

        wrapper->value = nullptr;
        wrapper->owned = false;

        switch (policy) {
            case return_value_policy::automatic:
            case return_value_policy::take_ownership:
                wrapper->value = src;
                wrapper->owned = true;
                break;

            case return_value_policy::automatic_reference:
            case return_value_policy::reference:
                wrapper->value = src;
                wrapper->owned = false;
                break;

            case return_value_policy::copy:
                if (copy_constructor)
                    wrapper->value = copy_constructor(src);
                else
                    throw cast_error("return_value_policy = copy, but the "
                                     "object is non-copyable!");
                wrapper->owned = true;
                break;

            case return_value_policy::move:
                if (move_constructor)
                    wrapper->value = move_constructor(src);
                else if (copy_constructor)
                    wrapper->value = copy_constructor(src);
                else
                    throw cast_error("return_value_policy = move, but the "
                                     "object is neither movable nor copyable!");
                wrapper->owned = true;
                break;

            case return_value_policy::reference_internal:
                wrapper->value = src;
                wrapper->owned = false;
                detail::keep_alive_impl(inst, parent);
                break;

            default:
                throw cast_error("unhandled return_value_policy: should not happen!");
        }

        register_instance(wrapper, tinfo);
        tinfo->init_holder(inst.ptr(), existing_holder);

        return inst.release();
    }

protected:

    // Called to do type lookup and wrap the pointer and type in a pair when a dynamic_cast
    // isn't needed or can't be used.  If the type is unknown, sets the error and returns a pair
    // with .second = nullptr.  (p.first = nullptr is not an error: it becomes None).
    PYBIND11_NOINLINE static std::pair<const void *, const type_info *> src_and_type(
            const void *src, const std::type_info *cast_type, const std::type_info *rtti_type = nullptr) {
        auto &internals = get_internals();
        auto it = internals.registered_types_cpp.find(std::type_index(*cast_type));
        if (it != internals.registered_types_cpp.end())
            return {src, (const type_info *) it->second};

        // Not found, set error:
        std::string tname = (rtti_type ? rtti_type : cast_type)->name();
        detail::clean_type_id(tname);
        std::string msg = "Unregistered type : " + tname;
        PyErr_SetString(PyExc_TypeError, msg.c_str());
        return {nullptr, nullptr};
    }

    const type_info *typeinfo = nullptr;
    void *value = nullptr;
    object temp;
};

/* Determine suitable casting operator */
template <typename T>
using cast_op_type = typename std::conditional<std::is_pointer<typename std::remove_reference<T>::type>::value,
    typename std::add_pointer<intrinsic_t<T>>::type,
    typename std::add_lvalue_reference<intrinsic_t<T>>::type>::type;

// std::is_copy_constructible isn't quite enough: it lets std::vector<T> (and similar) through when
// T is non-copyable, but code containing such a copy constructor fails to actually compile.
template <typename T, typename SFINAE = void> struct is_copy_constructible : std::is_copy_constructible<T> {};

// Specialization for types that appear to be copy constructible but also look like stl containers
// (we specifically check for: has `value_type` and `reference` with `reference = value_type&`): if
// so, copy constructability depends on whether the value_type is copy constructible.
template <typename Container> struct is_copy_constructible<Container, enable_if_t<
        std::is_copy_constructible<Container>::value &&
        std::is_same<typename Container::value_type &, typename Container::reference>::value
    >> : std::is_copy_constructible<typename Container::value_type> {};

/// Generic type caster for objects stored on the heap
template <typename type> class type_caster_base : public type_caster_generic {
    using itype = intrinsic_t<type>;
public:
    static PYBIND11_DESCR name() { return type_descr(_<type>()); }

    type_caster_base() : type_caster_base(typeid(type)) { }
    explicit type_caster_base(const std::type_info &info) : type_caster_generic(info) { }

    static handle cast(const itype &src, return_value_policy policy, handle parent) {
        if (policy == return_value_policy::automatic || policy == return_value_policy::automatic_reference)
            policy = return_value_policy::copy;
        return cast(&src, policy, parent);
    }

    static handle cast(itype &&src, return_value_policy, handle parent) {
        return cast(&src, return_value_policy::move, parent);
    }

    // Returns a (pointer, type_info) pair taking care of necessary RTTI type lookup for a
    // polymorphic type.  If the instance isn't derived, returns the non-RTTI base version.
    template <typename T = itype, enable_if_t<std::is_polymorphic<T>::value, int> = 0>
    static std::pair<const void *, const type_info *> src_and_type(const itype *src) {
        const void *vsrc = src;
        auto &internals = get_internals();
        auto cast_type = &typeid(itype);
        const std::type_info *instance_type = nullptr;
        if (vsrc) {
            instance_type = &typeid(*src);
            if (instance_type != cast_type) {
                // This is a base pointer to a derived type; if it is a pybind11-registered type, we
                // can get the correct derived pointer (which may be != base pointer) by a
                // dynamic_cast to most derived type:
                auto it = internals.registered_types_cpp.find(std::type_index(*instance_type));
                if (it != internals.registered_types_cpp.end())
                    return {dynamic_cast<const void *>(src), (const type_info *) it->second};
            }
        }
        // Otherwise we have either a nullptr, an `itype` pointer, or an unknown derived pointer, so
        // don't do a cast
        return type_caster_generic::src_and_type(vsrc, cast_type, instance_type);
    }

    // Non-polymorphic type, so no dynamic casting; just call the generic version directly
    template <typename T = itype, enable_if_t<!std::is_polymorphic<T>::value, int> = 0>
    static std::pair<const void *, const type_info *> src_and_type(const itype *src) {
        return type_caster_generic::src_and_type(src, &typeid(itype));
    }

    static handle cast(const itype *src, return_value_policy policy, handle parent) {
        auto st = src_and_type(src);
        return type_caster_generic::cast(
            st.first, policy, parent, st.second,
            make_copy_constructor(src), make_move_constructor(src));
    }

    static handle cast_holder(const itype *src, const void *holder) {
        auto st = src_and_type(src);
        return type_caster_generic::cast(
            st.first, return_value_policy::take_ownership, {}, st.second,
            nullptr, nullptr, holder);
    }

    template <typename T> using cast_op_type = pybind11::detail::cast_op_type<T>;

    operator itype*() { return (type *) value; }
    operator itype&() { if (!value) throw reference_cast_error(); return *((itype *) value); }

protected:
    typedef void *(*Constructor)(const void *stream);
#if !defined(_MSC_VER)
    /* Only enabled when the types are {copy,move}-constructible *and* when the type
       does not have a private operator new implementaton. */
    template <typename T = type, typename = enable_if_t<is_copy_constructible<T>::value>> static auto make_copy_constructor(const T *value) -> decltype(new T(*value), Constructor(nullptr)) {
        return [](const void *arg) -> void * { return new T(*((const T *) arg)); }; }
    template <typename T = type> static auto make_move_constructor(const T *value) -> decltype(new T(std::move(*((T *) value))), Constructor(nullptr)) {
        return [](const void *arg) -> void * { return (void *) new T(std::move(*const_cast<T *>(reinterpret_cast<const T *>(arg)))); }; }
#else
    /* Visual Studio 2015's SFINAE implementation doesn't yet handle the above robustly in all situations.
       Use a workaround that only tests for constructibility for now. */
    template <typename T = type, typename = enable_if_t<is_copy_constructible<T>::value>>
    static Constructor make_copy_constructor(const T *value) {
        return [](const void *arg) -> void * { return new T(*((const T *)arg)); }; }
    template <typename T = type, typename = enable_if_t<std::is_move_constructible<T>::value>>
    static Constructor make_move_constructor(const T *value) {
        return [](const void *arg) -> void * { return (void *) new T(std::move(*((T *)arg))); }; }
#endif

    static Constructor make_copy_constructor(...) { return nullptr; }
    static Constructor make_move_constructor(...) { return nullptr; }
};

template <typename type, typename SFINAE = void> class type_caster : public type_caster_base<type> { };
template <typename type> using make_caster = type_caster<intrinsic_t<type>>;

// Shortcut for calling a caster's `cast_op_type` cast operator for casting a type_caster to a T
template <typename T> typename make_caster<T>::template cast_op_type<T> cast_op(make_caster<T> &caster) {
    return caster.operator typename make_caster<T>::template cast_op_type<T>();
}
template <typename T> typename make_caster<T>::template cast_op_type<T> cast_op(make_caster<T> &&caster) {
    return cast_op<T>(caster);
}

template <typename type> class type_caster<std::reference_wrapper<type>> : public type_caster_base<type> {
public:
    static handle cast(const std::reference_wrapper<type> &src, return_value_policy policy, handle parent) {
        return type_caster_base<type>::cast(&src.get(), policy, parent);
    }
    template <typename T> using cast_op_type = std::reference_wrapper<type>;
    operator std::reference_wrapper<type>() { return std::ref(*((type *) this->value)); }
};

#define PYBIND11_TYPE_CASTER(type, py_name) \
    protected: \
        type value; \
    public: \
        static PYBIND11_DESCR name() { return type_descr(py_name); } \
        static handle cast(const type *src, return_value_policy policy, handle parent) { \
            if (!src) return none().release(); \
            return cast(*src, policy, parent); \
        } \
        operator type*() { return &value; } \
        operator type&() { return value; } \
        template <typename _T> using cast_op_type = pybind11::detail::cast_op_type<_T>


template <typename CharT> using is_std_char_type = any_of<
    std::is_same<CharT, char>, /* std::string */
    std::is_same<CharT, char16_t>, /* std::u16string */
    std::is_same<CharT, char32_t>, /* std::u32string */
    std::is_same<CharT, wchar_t> /* std::wstring */
>;

template <typename T>
struct type_caster<T, enable_if_t<std::is_arithmetic<T>::value && !is_std_char_type<T>::value>> {
    using _py_type_0 = conditional_t<sizeof(T) <= sizeof(long), long, long long>;
    using _py_type_1 = conditional_t<std::is_signed<T>::value, _py_type_0, typename std::make_unsigned<_py_type_0>::type>;
    using py_type = conditional_t<std::is_floating_point<T>::value, double, _py_type_1>;
public:

    bool load(handle src, bool convert) {
        py_type py_value;

        if (!src)
            return false;

        if (std::is_floating_point<T>::value) {
            if (convert || PyFloat_Check(src.ptr()))
                py_value = (py_type) PyFloat_AsDouble(src.ptr());
            else
                return false;
        } else if (sizeof(T) <= sizeof(long)) {
            if (PyFloat_Check(src.ptr()))
                return false;
            if (std::is_signed<T>::value)
                py_value = (py_type) PyLong_AsLong(src.ptr());
            else
                py_value = (py_type) PyLong_AsUnsignedLong(src.ptr());
        } else {
            if (PyFloat_Check(src.ptr()))
                return false;
            if (std::is_signed<T>::value)
                py_value = (py_type) PYBIND11_LONG_AS_LONGLONG(src.ptr());
            else
                py_value = (py_type) PYBIND11_LONG_AS_UNSIGNED_LONGLONG(src.ptr());
        }

        if ((py_value == (py_type) -1 && PyErr_Occurred()) ||
            (std::is_integral<T>::value && sizeof(py_type) != sizeof(T) &&
               (py_value < (py_type) std::numeric_limits<T>::min() ||
                py_value > (py_type) std::numeric_limits<T>::max()))) {
#if PY_VERSION_HEX < 0x03000000 && !defined(PYPY_VERSION)
            bool type_error = PyErr_ExceptionMatches(PyExc_SystemError);
#else
            bool type_error = PyErr_ExceptionMatches(PyExc_TypeError);
#endif
            PyErr_Clear();
            if (type_error && convert && PyNumber_Check(src.ptr())) {
                auto tmp = reinterpret_borrow<object>(std::is_floating_point<T>::value
                                                      ? PyNumber_Float(src.ptr())
                                                      : PyNumber_Long(src.ptr()));
                PyErr_Clear();
                return load(tmp, false);
            }
            return false;
        }

        value = (T) py_value;
        return true;
    }

    static handle cast(T src, return_value_policy /* policy */, handle /* parent */) {
        if (std::is_floating_point<T>::value) {
            return PyFloat_FromDouble((double) src);
        } else if (sizeof(T) <= sizeof(long)) {
            if (std::is_signed<T>::value)
                return PyLong_FromLong((long) src);
            else
                return PyLong_FromUnsignedLong((unsigned long) src);
        } else {
            if (std::is_signed<T>::value)
                return PyLong_FromLongLong((long long) src);
            else
                return PyLong_FromUnsignedLongLong((unsigned long long) src);
        }
    }

    PYBIND11_TYPE_CASTER(T, _<std::is_integral<T>::value>("int", "float"));
};

template<typename T> struct void_caster {
public:
    bool load(handle src, bool) {
        if (src && src.is_none())
            return true;
        return false;
    }
    static handle cast(T, return_value_policy /* policy */, handle /* parent */) {
        return none().inc_ref();
    }
    PYBIND11_TYPE_CASTER(T, _("None"));
};

template <> class type_caster<void_type> : public void_caster<void_type> {};

template <> class type_caster<void> : public type_caster<void_type> {
public:
    using type_caster<void_type>::cast;

    bool load(handle h, bool) {
        if (!h) {
            return false;
        } else if (h.is_none()) {
            value = nullptr;
            return true;
        }

        /* Check if this is a capsule */
        if (isinstance<capsule>(h)) {
            value = reinterpret_borrow<capsule>(h);
            return true;
        }

        /* Check if this is a C++ type */
        if (get_type_info((PyTypeObject *) h.get_type().ptr())) {
            value = ((instance<void> *) h.ptr())->value;
            return true;
        }

        /* Fail */
        return false;
    }

    static handle cast(const void *ptr, return_value_policy /* policy */, handle /* parent */) {
        if (ptr)
            return capsule(ptr).release();
        else
            return none().inc_ref();
    }

    template <typename T> using cast_op_type = void*&;
    operator void *&() { return value; }
    static PYBIND11_DESCR name() { return type_descr(_("capsule")); }
private:
    void *value = nullptr;
};

template <> class type_caster<std::nullptr_t> : public void_caster<std::nullptr_t> { };

template <> class type_caster<bool> {
public:
    bool load(handle src, bool) {
        if (!src) return false;
        else if (src.ptr() == Py_True) { value = true; return true; }
        else if (src.ptr() == Py_False) { value = false; return true; }
        else return false;
    }
    static handle cast(bool src, return_value_policy /* policy */, handle /* parent */) {
        return handle(src ? Py_True : Py_False).inc_ref();
    }
    PYBIND11_TYPE_CASTER(bool, _("bool"));
};

// Helper class for UTF-{8,16,32} C++ stl strings:
template <typename CharT, class Traits, class Allocator>
struct type_caster<std::basic_string<CharT, Traits, Allocator>, enable_if_t<is_std_char_type<CharT>::value>> {
    // Simplify life by being able to assume standard char sizes (the standard only guarantees
    // minimums), but Python requires exact sizes
    static_assert(!std::is_same<CharT, char>::value || sizeof(CharT) == 1, "Unsupported char size != 1");
    static_assert(!std::is_same<CharT, char16_t>::value || sizeof(CharT) == 2, "Unsupported char16_t size != 2");
    static_assert(!std::is_same<CharT, char32_t>::value || sizeof(CharT) == 4, "Unsupported char32_t size != 4");
    // wchar_t can be either 16 bits (Windows) or 32 (everywhere else)
    static_assert(!std::is_same<CharT, wchar_t>::value || sizeof(CharT) == 2 || sizeof(CharT) == 4,
            "Unsupported wchar_t size != 2/4");
    static constexpr size_t UTF_N = 8 * sizeof(CharT);

    using StringType = std::basic_string<CharT, Traits, Allocator>;

    bool load(handle src, bool) {
#if PY_MAJOR_VERSION < 3
        object temp;
#endif
        handle load_src = src;
        if (!src) {
            return false;
        } else if (!PyUnicode_Check(load_src.ptr())) {
#if PY_MAJOR_VERSION >= 3
            return load_bytes(load_src);
#else
            // The below is a guaranteed failure in Python 3 when PyUnicode_Check returns false
            if (!PYBIND11_BYTES_CHECK(load_src.ptr()))
                return false;
            temp = reinterpret_steal<object>(PyUnicode_FromObject(load_src.ptr()));
            if (!temp) { PyErr_Clear(); return false; }
            load_src = temp;
#endif
        }

        object utfNbytes = reinterpret_steal<object>(PyUnicode_AsEncodedString(
            load_src.ptr(), UTF_N == 8 ? "utf-8" : UTF_N == 16 ? "utf-16" : "utf-32", nullptr));
        if (!utfNbytes) { PyErr_Clear(); return false; }

        const CharT *buffer = reinterpret_cast<const CharT *>(PYBIND11_BYTES_AS_STRING(utfNbytes.ptr()));
        size_t length = (size_t) PYBIND11_BYTES_SIZE(utfNbytes.ptr()) / sizeof(CharT);
        if (UTF_N > 8) { buffer++; length--; } // Skip BOM for UTF-16/32
        value = StringType(buffer, length);
        return true;
    }

    static handle cast(const StringType &src, return_value_policy /* policy */, handle /* parent */) {
        const char *buffer = reinterpret_cast<const char *>(src.c_str());
        ssize_t nbytes = ssize_t(src.size() * sizeof(CharT));
        handle s = decode_utfN(buffer, nbytes);
        if (!s) throw error_already_set();
        return s;
    }

    PYBIND11_TYPE_CASTER(StringType, _(PYBIND11_STRING_NAME));

private:
    static handle decode_utfN(const char *buffer, ssize_t nbytes) {
#if !defined(PYPY_VERSION)
        return
            UTF_N == 8  ? PyUnicode_DecodeUTF8(buffer, nbytes, nullptr) :
            UTF_N == 16 ? PyUnicode_DecodeUTF16(buffer, nbytes, nullptr, nullptr) :
                          PyUnicode_DecodeUTF32(buffer, nbytes, nullptr, nullptr);
#else
        // PyPy seems to have multiple problems related to PyUnicode_UTF*: the UTF8 version
        // sometimes segfaults for unknown reasons, while the UTF16 and 32 versions require a
        // non-const char * arguments, which is also a nuissance, so bypass the whole thing by just
        // passing the encoding as a string value, which works properly:
        return PyUnicode_Decode(buffer, nbytes, UTF_N == 8 ? "utf-8" : UTF_N == 16 ? "utf-16" : "utf-32", nullptr);
#endif
    }

#if PY_MAJOR_VERSION >= 3
    // In Python 3, when loading into a std::string or char*, accept a bytes object as-is (i.e.
    // without any encoding/decoding attempt).  For other C++ char sizes this is a no-op.  Python 2,
    // which supports loading a unicode from a str, doesn't take this path.
    template <typename C = CharT>
    bool load_bytes(enable_if_t<sizeof(C) == 1, handle> src) {
        if (PYBIND11_BYTES_CHECK(src.ptr())) {
            // We were passed a Python 3 raw bytes; accept it into a std::string or char*
            // without any encoding attempt.
            const char *bytes = PYBIND11_BYTES_AS_STRING(src.ptr());
            if (bytes) {
                value = StringType(bytes, (size_t) PYBIND11_BYTES_SIZE(src.ptr()));
                return true;
            }
        }

        return false;
    }
    template <typename C = CharT>
    bool load_bytes(enable_if_t<sizeof(C) != 1, handle>) { return false; }
#endif
};

// Type caster for C-style strings.  We basically use a std::string type caster, but also add the
// ability to use None as a nullptr char* (which the string caster doesn't allow).
template <typename CharT> struct type_caster<CharT, enable_if_t<is_std_char_type<CharT>::value>> {
    using StringType = std::basic_string<CharT>;
    using StringCaster = type_caster<StringType>;
    StringCaster str_caster;
    bool none = false;
public:
    bool load(handle src, bool convert) {
        if (!src) return false;
        if (src.is_none()) {
            // Defer accepting None to other overloads (if we aren't in convert mode):
            if (!convert) return false;
            none = true;
            return true;
        }
        return str_caster.load(src, convert);
    }

    static handle cast(const CharT *src, return_value_policy policy, handle parent) {
        if (src == nullptr) return pybind11::none().inc_ref();
        return StringCaster::cast(StringType(src), policy, parent);
    }

    static handle cast(CharT src, return_value_policy policy, handle parent) {
        if (std::is_same<char, CharT>::value) {
            handle s = PyUnicode_DecodeLatin1((const char *) &src, 1, nullptr);
            if (!s) throw error_already_set();
            return s;
        }
        return StringCaster::cast(StringType(1, src), policy, parent);
    }

    operator CharT*() { return none ? nullptr : const_cast<CharT *>(static_cast<StringType &>(str_caster).c_str()); }
    operator CharT() {
        if (none)
            throw value_error("Cannot convert None to a character");

        auto &value = static_cast<StringType &>(str_caster);
        size_t str_len = value.size();
        if (str_len == 0)
            throw value_error("Cannot convert empty string to a character");

        // If we're in UTF-8 mode, we have two possible failures: one for a unicode character that
        // is too high, and one for multiple unicode characters (caught later), so we need to figure
        // out how long the first encoded character is in bytes to distinguish between these two
        // errors.  We also allow want to allow unicode characters U+0080 through U+00FF, as those
        // can fit into a single char value.
        if (StringCaster::UTF_N == 8 && str_len > 1 && str_len <= 4) {
            unsigned char v0 = static_cast<unsigned char>(value[0]);
            size_t char0_bytes = !(v0 & 0x80) ? 1 : // low bits only: 0-127
                (v0 & 0xE0) == 0xC0 ? 2 : // 0b110xxxxx - start of 2-byte sequence
                (v0 & 0xF0) == 0xE0 ? 3 : // 0b1110xxxx - start of 3-byte sequence
                4; // 0b11110xxx - start of 4-byte sequence

            if (char0_bytes == str_len) {
                // If we have a 128-255 value, we can decode it into a single char:
                if (char0_bytes == 2 && (v0 & 0xFC) == 0xC0) { // 0x110000xx 0x10xxxxxx
                    return static_cast<CharT>(((v0 & 3) << 6) + (static_cast<unsigned char>(value[1]) & 0x3F));
                }
                // Otherwise we have a single character, but it's > U+00FF
                throw value_error("Character code point not in range(0x100)");
            }
        }

        // UTF-16 is much easier: we can only have a surrogate pair for values above U+FFFF, thus a
        // surrogate pair with total length 2 instantly indicates a range error (but not a "your
        // string was too long" error).
        else if (StringCaster::UTF_N == 16 && str_len == 2) {
            char16_t v0 = static_cast<char16_t>(value[0]);
            if (v0 >= 0xD800 && v0 < 0xE000)
                throw value_error("Character code point not in range(0x10000)");
        }

        if (str_len != 1)
            throw value_error("Expected a character, but multi-character string found");

        return value[0];
    }

    static PYBIND11_DESCR name() { return type_descr(_(PYBIND11_STRING_NAME)); }
    template <typename _T> using cast_op_type = typename std::remove_reference<pybind11::detail::cast_op_type<_T>>::type;
};

template <typename T1, typename T2> class type_caster<std::pair<T1, T2>> {
    typedef std::pair<T1, T2> type;
public:
    bool load(handle src, bool convert) {
        if (!isinstance<sequence>(src))
            return false;
        const auto seq = reinterpret_borrow<sequence>(src);
        if (seq.size() != 2)
            return false;
        return first.load(seq[0], convert) && second.load(seq[1], convert);
    }

    static handle cast(const type &src, return_value_policy policy, handle parent) {
        auto o1 = reinterpret_steal<object>(make_caster<T1>::cast(src.first, policy, parent));
        auto o2 = reinterpret_steal<object>(make_caster<T2>::cast(src.second, policy, parent));
        if (!o1 || !o2)
            return handle();
        tuple result(2);
        PyTuple_SET_ITEM(result.ptr(), 0, o1.release().ptr());
        PyTuple_SET_ITEM(result.ptr(), 1, o2.release().ptr());
        return result.release();
    }

    static PYBIND11_DESCR name() {
        return type_descr(
            _("Tuple[") + make_caster<T1>::name() + _(", ") + make_caster<T2>::name() + _("]")
        );
    }

    template <typename T> using cast_op_type = type;

    operator type() {
        return type(cast_op<T1>(first), cast_op<T2>(second));
    }
protected:
    make_caster<T1> first;
    make_caster<T2> second;
};

template <typename... Tuple> class type_caster<std::tuple<Tuple...>> {
    using type = std::tuple<Tuple...>;
    using indices = make_index_sequence<sizeof...(Tuple)>;
    static constexpr auto size = sizeof...(Tuple);

public:
    bool load(handle src, bool convert) {
        if (!isinstance<sequence>(src))
            return false;
        const auto seq = reinterpret_borrow<sequence>(src);
        if (seq.size() != size)
            return false;
        return load_impl(seq, convert, indices{});
    }

    static handle cast(const type &src, return_value_policy policy, handle parent) {
        return cast_impl(src, policy, parent, indices{});
    }

    static PYBIND11_DESCR name() {
        return type_descr(_("Tuple[") + detail::concat(make_caster<Tuple>::name()...) + _("]"));
    }

    template <typename T> using cast_op_type = type;

    operator type() { return implicit_cast(indices{}); }

protected:
    template <size_t... Is>
    type implicit_cast(index_sequence<Is...>) { return type(cast_op<Tuple>(std::get<Is>(value))...); }

    static constexpr bool load_impl(const sequence &, bool, index_sequence<>) { return true; }

    template <size_t... Is>
    bool load_impl(const sequence &seq, bool convert, index_sequence<Is...>) {
        for (bool r : {std::get<Is>(value).load(seq[Is], convert)...})
            if (!r)
                return false;
        return true;
    }

    static handle cast_impl(const type &, return_value_policy, handle,
                            index_sequence<>) { return tuple().release(); }

    /* Implementation: Convert a C++ tuple into a Python tuple */
    template <size_t... Is>
    static handle cast_impl(const type &src, return_value_policy policy, handle parent, index_sequence<Is...>) {
        std::array<object, size> entries {{
            reinterpret_steal<object>(make_caster<Tuple>::cast(std::get<Is>(src), policy, parent))...
        }};
        for (const auto &entry: entries)
            if (!entry)
                return handle();
        tuple result(size);
        int counter = 0;
        for (auto & entry: entries)
            PyTuple_SET_ITEM(result.ptr(), counter++, entry.release().ptr());
        return result.release();
    }

    std::tuple<make_caster<Tuple>...> value;
};

/// Helper class which abstracts away certain actions. Users can provide specializations for
/// custom holders, but it's only necessary if the type has a non-standard interface.
template <typename T>
struct holder_helper {
    static auto get(const T &p) -> decltype(p.get()) { return p.get(); }
};

/// Type caster for holder types like std::shared_ptr, etc.
template <typename type, typename holder_type>
struct copyable_holder_caster : public type_caster_base<type> {
public:
    using base = type_caster_base<type>;
    static_assert(std::is_base_of<base, type_caster<type>>::value,
            "Holder classes are only supported for custom types");
    using base::base;
    using base::cast;
    using base::typeinfo;
    using base::value;
    using base::temp;

    PYBIND11_NOINLINE bool load(handle src, bool convert) {
        return load(src, convert, Py_TYPE(src.ptr()));
    }

    bool load(handle src, bool convert, PyTypeObject *tobj) {
        if (!src || !typeinfo)
            return false;
        if (src.is_none()) {
            // Defer accepting None to other overloads (if we aren't in convert mode):
            if (!convert) return false;
            value = nullptr;
            return true;
        }

        if (typeinfo->default_holder)
            throw cast_error("Unable to load a custom holder type from a default-holder instance");

        if (typeinfo->simple_type) { /* Case 1: no multiple inheritance etc. involved */
            /* Check if we can safely perform a reinterpret-style cast */
            if (PyType_IsSubtype(tobj, typeinfo->type))
                return load_value_and_holder(src);
        } else { /* Case 2: multiple inheritance */
            /* Check if we can safely perform a reinterpret-style cast */
            if (tobj == typeinfo->type)
                return load_value_and_holder(src);

            /* If this is a python class, also check the parents recursively */
            auto const &type_dict = get_internals().registered_types_py;
            bool new_style_class = PyType_Check((PyObject *) tobj);
            if (type_dict.find(tobj) == type_dict.end() && new_style_class && tobj->tp_bases) {
                auto parents = reinterpret_borrow<tuple>(tobj->tp_bases);
                for (handle parent : parents) {
                    bool result = load(src, convert, (PyTypeObject *) parent.ptr());
                    if (result)
                        return true;
                }
            }

            if (try_implicit_casts(src, convert))
                return true;
        }

        if (convert) {
            for (auto &converter : typeinfo->implicit_conversions) {
                temp = reinterpret_steal<object>(converter(src.ptr(), typeinfo->type));
                if (load(temp, false))
                    return true;
            }
        }

        return false;
    }

    bool load_value_and_holder(handle src) {
        auto inst = (instance<type, holder_type> *) src.ptr();
        value = (void *) inst->value;
        if (inst->holder_constructed) {
            holder = inst->holder;
            return true;
        } else {
            throw cast_error("Unable to cast from non-held to held instance (T& to Holder<T>) "
#if defined(NDEBUG)
                             "(compile in debug mode for type information)");
#else
                             "of type '" + type_id<holder_type>() + "''");
#endif
        }
    }

    template <typename T = holder_type, detail::enable_if_t<!std::is_constructible<T, const T &, type*>::value, int> = 0>
    bool try_implicit_casts(handle, bool) { return false; }

    template <typename T = holder_type, detail::enable_if_t<std::is_constructible<T, const T &, type*>::value, int> = 0>
    bool try_implicit_casts(handle src, bool convert) {
        for (auto &cast : typeinfo->implicit_casts) {
            copyable_holder_caster sub_caster(*cast.first);
            if (sub_caster.load(src, convert)) {
                value = cast.second(sub_caster.value);
                holder = holder_type(sub_caster.holder, (type *) value);
                return true;
            }
        }
        return false;
    }

    explicit operator type*() { return this->value; }
    explicit operator type&() { return *(this->value); }
    explicit operator holder_type*() { return &holder; }

    // Workaround for Intel compiler bug
    // see pybind11 issue 94
    #if defined(__ICC) || defined(__INTEL_COMPILER)
    operator holder_type&() { return holder; }
    #else
    explicit operator holder_type&() { return holder; }
    #endif

    static handle cast(const holder_type &src, return_value_policy, handle) {
        const auto *ptr = holder_helper<holder_type>::get(src);
        return type_caster_base<type>::cast_holder(ptr, &src);
    }

protected:
    holder_type holder;
};

/// Specialize for the common std::shared_ptr, so users don't need to
template <typename T>
class type_caster<std::shared_ptr<T>> : public copyable_holder_caster<T, std::shared_ptr<T>> { };

template <typename type, typename holder_type>
struct move_only_holder_caster {
    static_assert(std::is_base_of<type_caster_base<type>, type_caster<type>>::value,
            "Holder classes are only supported for custom types");

    static handle cast(holder_type &&src, return_value_policy, handle) {
        auto *ptr = holder_helper<holder_type>::get(src);
        return type_caster_base<type>::cast_holder(ptr, &src);
    }
    static PYBIND11_DESCR name() { return type_caster_base<type>::name(); }
};

template <typename type, typename deleter>
class type_caster<std::unique_ptr<type, deleter>>
    : public move_only_holder_caster<type, std::unique_ptr<type, deleter>> { };

template <typename type, typename holder_type>
using type_caster_holder = conditional_t<std::is_copy_constructible<holder_type>::value,
                                         copyable_holder_caster<type, holder_type>,
                                         move_only_holder_caster<type, holder_type>>;

template <typename T, bool Value = false> struct always_construct_holder { static constexpr bool value = Value; };

/// Create a specialization for custom holder types (silently ignores std::shared_ptr)
#define PYBIND11_DECLARE_HOLDER_TYPE(type, holder_type, ...) \
    namespace pybind11 { namespace detail { \
    template <typename type> \
    struct always_construct_holder<holder_type> : always_construct_holder<void, ##__VA_ARGS__>  { }; \
    template <typename type> \
    class type_caster<holder_type, enable_if_t<!is_shared_ptr<holder_type>::value>> \
        : public type_caster_holder<type, holder_type> { }; \
    }}

// PYBIND11_DECLARE_HOLDER_TYPE holder types:
template <typename base, typename holder> struct is_holder_type :
    std::is_base_of<detail::type_caster_holder<base, holder>, detail::type_caster<holder>> {};
// Specialization for always-supported unique_ptr holders:
template <typename base, typename deleter> struct is_holder_type<base, std::unique_ptr<base, deleter>> :
    std::true_type {};

template <typename T> struct handle_type_name { static PYBIND11_DESCR name() { return _<T>(); } };
template <> struct handle_type_name<bytes> { static PYBIND11_DESCR name() { return _(PYBIND11_BYTES_NAME); } };
template <> struct handle_type_name<args> { static PYBIND11_DESCR name() { return _("*args"); } };
template <> struct handle_type_name<kwargs> { static PYBIND11_DESCR name() { return _("**kwargs"); } };

template <typename type>
struct pyobject_caster {
    template <typename T = type, enable_if_t<std::is_same<T, handle>::value, int> = 0>
    bool load(handle src, bool /* convert */) { value = src; return static_cast<bool>(value); }

    template <typename T = type, enable_if_t<std::is_base_of<object, T>::value, int> = 0>
    bool load(handle src, bool /* convert */) {
        if (!isinstance<type>(src))
            return false;
        value = reinterpret_borrow<type>(src);
        return true;
    }

    static handle cast(const handle &src, return_value_policy /* policy */, handle /* parent */) {
        return src.inc_ref();
    }
    PYBIND11_TYPE_CASTER(type, handle_type_name<type>::name());
};

template <typename T>
class type_caster<T, enable_if_t<is_pyobject<T>::value>> : public pyobject_caster<T> { };

// Our conditions for enabling moving are quite restrictive:
// At compile time:
// - T needs to be a non-const, non-pointer, non-reference type
// - type_caster<T>::operator T&() must exist
// - the type must be move constructible (obviously)
// At run-time:
// - if the type is non-copy-constructible, the object must be the sole owner of the type (i.e. it
//   must have ref_count() == 1)h
// If any of the above are not satisfied, we fall back to copying.
template <typename T> using move_is_plain_type = satisfies_none_of<T,
    std::is_void, std::is_pointer, std::is_reference, std::is_const
>;
template <typename T, typename SFINAE = void> struct move_always : std::false_type {};
template <typename T> struct move_always<T, enable_if_t<all_of<
    move_is_plain_type<T>,
    negation<std::is_copy_constructible<T>>,
    std::is_move_constructible<T>,
    std::is_same<decltype(std::declval<make_caster<T>>().operator T&()), T&>
>::value>> : std::true_type {};
template <typename T, typename SFINAE = void> struct move_if_unreferenced : std::false_type {};
template <typename T> struct move_if_unreferenced<T, enable_if_t<all_of<
    move_is_plain_type<T>,
    negation<move_always<T>>,
    std::is_move_constructible<T>,
    std::is_same<decltype(std::declval<make_caster<T>>().operator T&()), T&>
>::value>> : std::true_type {};
template <typename T> using move_never = none_of<move_always<T>, move_if_unreferenced<T>>;

// Detect whether returning a `type` from a cast on type's type_caster is going to result in a
// reference or pointer to a local variable of the type_caster.  Basically, only
// non-reference/pointer `type`s and reference/pointers from a type_caster_generic are safe;
// everything else returns a reference/pointer to a local variable.
template <typename type> using cast_is_temporary_value_reference = bool_constant<
    (std::is_reference<type>::value || std::is_pointer<type>::value) &&
    !std::is_base_of<type_caster_generic, make_caster<type>>::value
>;

// When a value returned from a C++ function is being cast back to Python, we almost always want to
// force `policy = move`, regardless of the return value policy the function/method was declared
// with.  Some classes (most notably Eigen::Ref and related) need to avoid this, and so can do so by
// specializing this struct.
template <typename Return, typename SFINAE = void> struct return_value_policy_override {
    static return_value_policy policy(return_value_policy p) {
        return !std::is_lvalue_reference<Return>::value && !std::is_pointer<Return>::value
            ? return_value_policy::move : p;
    }
};

// Basic python -> C++ casting; throws if casting fails
template <typename T, typename SFINAE> type_caster<T, SFINAE> &load_type(type_caster<T, SFINAE> &conv, const handle &handle) {
    if (!conv.load(handle, true)) {
#if defined(NDEBUG)
        throw cast_error("Unable to cast Python instance to C++ type (compile in debug mode for details)");
#else
        throw cast_error("Unable to cast Python instance of type " +
            (std::string) str(handle.get_type()) + " to C++ type '" + type_id<T>() + "''");
#endif
    }
    return conv;
}
// Wrapper around the above that also constructs and returns a type_caster
template <typename T> make_caster<T> load_type(const handle &handle) {
    make_caster<T> conv;
    load_type(conv, handle);
    return conv;
}

NAMESPACE_END(detail)

// pytype -> C++ type
template <typename T, detail::enable_if_t<!detail::is_pyobject<T>::value, int> = 0>
T cast(const handle &handle) {
    using namespace detail;
    static_assert(!cast_is_temporary_value_reference<T>::value,
            "Unable to cast type to reference: value is local to type caster");
    return cast_op<T>(load_type<T>(handle));
}

// pytype -> pytype (calls converting constructor)
template <typename T, detail::enable_if_t<detail::is_pyobject<T>::value, int> = 0>
T cast(const handle &handle) { return T(reinterpret_borrow<object>(handle)); }

// C++ type -> py::object
template <typename T, detail::enable_if_t<!detail::is_pyobject<T>::value, int> = 0>
object cast(const T &value, return_value_policy policy = return_value_policy::automatic_reference,
            handle parent = handle()) {
    if (policy == return_value_policy::automatic)
        policy = std::is_pointer<T>::value ? return_value_policy::take_ownership : return_value_policy::copy;
    else if (policy == return_value_policy::automatic_reference)
        policy = std::is_pointer<T>::value ? return_value_policy::reference : return_value_policy::copy;
    return reinterpret_steal<object>(detail::make_caster<T>::cast(value, policy, parent));
}

template <typename T> T handle::cast() const { return pybind11::cast<T>(*this); }
template <> inline void handle::cast() const { return; }

template <typename T>
detail::enable_if_t<!detail::move_never<T>::value, T> move(object &&obj) {
    if (obj.ref_count() > 1)
#if defined(NDEBUG)
        throw cast_error("Unable to cast Python instance to C++ rvalue: instance has multiple references"
            " (compile in debug mode for details)");
#else
        throw cast_error("Unable to move from Python " + (std::string) str(obj.get_type()) +
                " instance to C++ " + type_id<T>() + " instance: instance has multiple references");
#endif

    // Move into a temporary and return that, because the reference may be a local value of `conv`
    T ret = std::move(detail::load_type<T>(obj).operator T&());
    return ret;
}

// Calling cast() on an rvalue calls pybind::cast with the object rvalue, which does:
// - If we have to move (because T has no copy constructor), do it.  This will fail if the moved
//   object has multiple references, but trying to copy will fail to compile.
// - If both movable and copyable, check ref count: if 1, move; otherwise copy
// - Otherwise (not movable), copy.
template <typename T> detail::enable_if_t<detail::move_always<T>::value, T> cast(object &&object) {
    return move<T>(std::move(object));
}
template <typename T> detail::enable_if_t<detail::move_if_unreferenced<T>::value, T> cast(object &&object) {
    if (object.ref_count() > 1)
        return cast<T>(object);
    else
        return move<T>(std::move(object));
}
template <typename T> detail::enable_if_t<detail::move_never<T>::value, T> cast(object &&object) {
    return cast<T>(object);
}

template <typename T> T object::cast() const & { return pybind11::cast<T>(*this); }
template <typename T> T object::cast() && { return pybind11::cast<T>(std::move(*this)); }
template <> inline void object::cast() const & { return; }
template <> inline void object::cast() && { return; }

NAMESPACE_BEGIN(detail)

// Declared in pytypes.h:
template <typename T, enable_if_t<!is_pyobject<T>::value, int>>
object object_or_cast(T &&o) { return pybind11::cast(std::forward<T>(o)); }

struct overload_unused {}; // Placeholder type for the unneeded (and dead code) static variable in the OVERLOAD_INT macro
template <typename ret_type> using overload_caster_t = conditional_t<
    cast_is_temporary_value_reference<ret_type>::value, make_caster<ret_type>, overload_unused>;

// Trampoline use: for reference/pointer types to value-converted values, we do a value cast, then
// store the result in the given variable.  For other types, this is a no-op.
template <typename T> enable_if_t<cast_is_temporary_value_reference<T>::value, T> cast_ref(object &&o, make_caster<T> &caster) {
    return cast_op<T>(load_type(caster, o));
}
template <typename T> enable_if_t<!cast_is_temporary_value_reference<T>::value, T> cast_ref(object &&, overload_unused &) {
    pybind11_fail("Internal error: cast_ref fallback invoked"); }

// Trampoline use: Having a pybind11::cast with an invalid reference type is going to static_assert, even
// though if it's in dead code, so we provide a "trampoline" to pybind11::cast that only does anything in
// cases where pybind11::cast is valid.
template <typename T> enable_if_t<!cast_is_temporary_value_reference<T>::value, T> cast_safe(object &&o) {
    return pybind11::cast<T>(std::move(o)); }
template <typename T> enable_if_t<cast_is_temporary_value_reference<T>::value, T> cast_safe(object &&) {
    pybind11_fail("Internal error: cast_safe fallback invoked"); }
template <> inline void cast_safe<void>(object &&) {}

NAMESPACE_END(detail)

template <return_value_policy policy = return_value_policy::automatic_reference,
          typename... Args> tuple make_tuple(Args&&... args_) {
    constexpr size_t size = sizeof...(Args);
    std::array<object, size> args {
        { reinterpret_steal<object>(detail::make_caster<Args>::cast(
            std::forward<Args>(args_), policy, nullptr))... }
    };
    for (size_t i = 0; i < args.size(); i++) {
        if (!args[i]) {
#if defined(NDEBUG)
            throw cast_error("make_tuple(): unable to convert arguments to Python object (compile in debug mode for details)");
#else
            std::array<std::string, size> argtypes { {type_id<Args>()...} };
            throw cast_error("make_tuple(): unable to convert argument of type '" +
                argtypes[i] + "' to Python object");
#endif
        }
    }
    tuple result(size);
    int counter = 0;
    for (auto &arg_value : args)
        PyTuple_SET_ITEM(result.ptr(), counter++, arg_value.release().ptr());
    return result;
}

/// \ingroup annotations
/// Annotation for arguments
struct arg {
    /// Constructs an argument with the name of the argument; if null or omitted, this is a positional argument.
    constexpr explicit arg(const char *name = nullptr) : name(name), flag_noconvert(false) { }
    /// Assign a value to this argument
    template <typename T> arg_v operator=(T &&value) const;
    /// Indicate that the type should not be converted in the type caster
    arg &noconvert(bool flag = true) { flag_noconvert = flag; return *this; }

    const char *name; ///< If non-null, this is a named kwargs argument
    bool flag_noconvert : 1; ///< If set, do not allow conversion (requires a supporting type caster!)
};

/// \ingroup annotations
/// Annotation for arguments with values
struct arg_v : arg {
private:
    template <typename T>
    arg_v(arg &&base, T &&x, const char *descr = nullptr)
        : arg(base),
          value(reinterpret_steal<object>(
              detail::make_caster<T>::cast(x, return_value_policy::automatic, {})
          )),
          descr(descr)
#if !defined(NDEBUG)
        , type(type_id<T>())
#endif
    { }

public:
    /// Direct construction with name, default, and description
    template <typename T>
    arg_v(const char *name, T &&x, const char *descr = nullptr)
        : arg_v(arg(name), std::forward<T>(x), descr) { }

    /// Called internally when invoking `py::arg("a") = value`
    template <typename T>
    arg_v(const arg &base, T &&x, const char *descr = nullptr)
        : arg_v(arg(base), std::forward<T>(x), descr) { }

    /// Same as `arg::noconvert()`, but returns *this as arg_v&, not arg&
    arg_v &noconvert(bool flag = true) { arg::noconvert(flag); return *this; }

    /// The default value
    object value;
    /// The (optional) description of the default value
    const char *descr;
#if !defined(NDEBUG)
    /// The C++ type name of the default value (only available when compiled in debug mode)
    std::string type;
#endif
};

template <typename T>
arg_v arg::operator=(T &&value) const { return {std::move(*this), std::forward<T>(value)}; }

/// Alias for backward compatibility -- to be removed in version 2.0
template <typename /*unused*/> using arg_t = arg_v;

inline namespace literals {
/** \rst
    String literal version of `arg`
 \endrst */
constexpr arg operator"" _a(const char *name, size_t) { return arg(name); }
}

NAMESPACE_BEGIN(detail)

// forward declaration
struct function_record;

/// Internal data associated with a single function call
struct function_call {
    function_call(function_record &f, handle p); // Implementation in attr.h

    /// The function data:
    const function_record &func;

    /// Arguments passed to the function:
    std::vector<handle> args;

    /// The `convert` value the arguments should be loaded with
    std::vector<bool> args_convert;

    /// The parent, if any
    handle parent;
};


/// Helper class which loads arguments for C++ functions called from Python
template <typename... Args>
class argument_loader {
    using indices = make_index_sequence<sizeof...(Args)>;

    template <typename Arg> using argument_is_args   = std::is_same<intrinsic_t<Arg>, args>;
    template <typename Arg> using argument_is_kwargs = std::is_same<intrinsic_t<Arg>, kwargs>;
    // Get args/kwargs argument positions relative to the end of the argument list:
    static constexpr auto args_pos = constexpr_first<argument_is_args, Args...>() - (int) sizeof...(Args),
                        kwargs_pos = constexpr_first<argument_is_kwargs, Args...>() - (int) sizeof...(Args);

    static constexpr bool args_kwargs_are_last = kwargs_pos >= - 1 && args_pos >= kwargs_pos - 1;

    static_assert(args_kwargs_are_last, "py::args/py::kwargs are only permitted as the last argument(s) of a function");

public:
    static constexpr bool has_kwargs = kwargs_pos < 0;
    static constexpr bool has_args = args_pos < 0;

    static PYBIND11_DESCR arg_names() { return detail::concat(make_caster<Args>::name()...); }

    bool load_args(function_call &call) {
        return load_impl_sequence(call, indices{});
    }

    template <typename Return, typename Guard, typename Func>
    enable_if_t<!std::is_void<Return>::value, Return> call(Func &&f) {
        return call_impl<Return>(std::forward<Func>(f), indices{}, Guard{});
    }

    template <typename Return, typename Guard, typename Func>
    enable_if_t<std::is_void<Return>::value, void_type> call(Func &&f) {
        call_impl<Return>(std::forward<Func>(f), indices{}, Guard{});
        return void_type();
    }

private:

    static bool load_impl_sequence(function_call &, index_sequence<>) { return true; }

    template <size_t... Is>
    bool load_impl_sequence(function_call &call, index_sequence<Is...>) {
        for (bool r : {std::get<Is>(value).load(call.args[Is], call.args_convert[Is])...})
            if (!r)
                return false;
        return true;
    }

    template <typename Return, typename Func, size_t... Is, typename Guard>
    Return call_impl(Func &&f, index_sequence<Is...>, Guard &&) {
        return std::forward<Func>(f)(cast_op<Args>(std::get<Is>(value))...);
    }

    std::tuple<make_caster<Args>...> value;
};

/// Helper class which collects only positional arguments for a Python function call.
/// A fancier version below can collect any argument, but this one is optimal for simple calls.
template <return_value_policy policy>
class simple_collector {
public:
    template <typename... Ts>
    explicit simple_collector(Ts &&...values)
        : m_args(pybind11::make_tuple<policy>(std::forward<Ts>(values)...)) { }

    const tuple &args() const & { return m_args; }
    dict kwargs() const { return {}; }

    tuple args() && { return std::move(m_args); }

    /// Call a Python function and pass the collected arguments
    object call(PyObject *ptr) const {
        PyObject *result = PyObject_CallObject(ptr, m_args.ptr());
        if (!result)
            throw error_already_set();
        return reinterpret_steal<object>(result);
    }

private:
    tuple m_args;
};

/// Helper class which collects positional, keyword, * and ** arguments for a Python function call
template <return_value_policy policy>
class unpacking_collector {
public:
    template <typename... Ts>
    explicit unpacking_collector(Ts &&...values) {
        // Tuples aren't (easily) resizable so a list is needed for collection,
        // but the actual function call strictly requires a tuple.
        auto args_list = list();
        int _[] = { 0, (process(args_list, std::forward<Ts>(values)), 0)... };
        ignore_unused(_);

        m_args = std::move(args_list);
    }

    const tuple &args() const & { return m_args; }
    const dict &kwargs() const & { return m_kwargs; }

    tuple args() && { return std::move(m_args); }
    dict kwargs() && { return std::move(m_kwargs); }

    /// Call a Python function and pass the collected arguments
    object call(PyObject *ptr) const {
        PyObject *result = PyObject_Call(ptr, m_args.ptr(), m_kwargs.ptr());
        if (!result)
            throw error_already_set();
        return reinterpret_steal<object>(result);
    }

private:
    template <typename T>
    void process(list &args_list, T &&x) {
        auto o = reinterpret_steal<object>(detail::make_caster<T>::cast(std::forward<T>(x), policy, {}));
        if (!o) {
#if defined(NDEBUG)
            argument_cast_error();
#else
            argument_cast_error(std::to_string(args_list.size()), type_id<T>());
#endif
        }
        args_list.append(o);
    }

    void process(list &args_list, detail::args_proxy ap) {
        for (const auto &a : ap)
            args_list.append(a);
    }

    void process(list &/*args_list*/, arg_v a) {
        if (!a.name)
#if defined(NDEBUG)
            nameless_argument_error();
#else
            nameless_argument_error(a.type);
#endif

        if (m_kwargs.contains(a.name)) {
#if defined(NDEBUG)
            multiple_values_error();
#else
            multiple_values_error(a.name);
#endif
        }
        if (!a.value) {
#if defined(NDEBUG)
            argument_cast_error();
#else
            argument_cast_error(a.name, a.type);
#endif
        }
        m_kwargs[a.name] = a.value;
    }

    void process(list &/*args_list*/, detail::kwargs_proxy kp) {
        if (!kp)
            return;
        for (const auto &k : reinterpret_borrow<dict>(kp)) {
            if (m_kwargs.contains(k.first)) {
#if defined(NDEBUG)
                multiple_values_error();
#else
                multiple_values_error(str(k.first));
#endif
            }
            m_kwargs[k.first] = k.second;
        }
    }

    [[noreturn]] static void nameless_argument_error() {
        throw type_error("Got kwargs without a name; only named arguments "
                         "may be passed via py::arg() to a python function call. "
                         "(compile in debug mode for details)");
    }
    [[noreturn]] static void nameless_argument_error(std::string type) {
        throw type_error("Got kwargs without a name of type '" + type + "'; only named "
                         "arguments may be passed via py::arg() to a python function call. ");
    }
    [[noreturn]] static void multiple_values_error() {
        throw type_error("Got multiple values for keyword argument "
                         "(compile in debug mode for details)");
    }

    [[noreturn]] static void multiple_values_error(std::string name) {
        throw type_error("Got multiple values for keyword argument '" + name + "'");
    }

    [[noreturn]] static void argument_cast_error() {
        throw cast_error("Unable to convert call argument to Python object "
                         "(compile in debug mode for details)");
    }

    [[noreturn]] static void argument_cast_error(std::string name, std::string type) {
        throw cast_error("Unable to convert call argument '" + name
                         + "' of type '" + type + "' to Python object");
    }

private:
    tuple m_args;
    dict m_kwargs;
};

/// Collect only positional arguments for a Python function call
template <return_value_policy policy, typename... Args,
          typename = enable_if_t<all_of<is_positional<Args>...>::value>>
simple_collector<policy> collect_arguments(Args &&...args) {
    return simple_collector<policy>(std::forward<Args>(args)...);
}

/// Collect all arguments, including keywords and unpacking (only instantiated when needed)
template <return_value_policy policy, typename... Args,
          typename = enable_if_t<!all_of<is_positional<Args>...>::value>>
unpacking_collector<policy> collect_arguments(Args &&...args) {
    // Following argument order rules for generalized unpacking according to PEP 448
    static_assert(
        constexpr_last<is_positional, Args...>() < constexpr_first<is_keyword_or_ds, Args...>()
        && constexpr_last<is_s_unpacking, Args...>() < constexpr_first<is_ds_unpacking, Args...>(),
        "Invalid function call: positional args must precede keywords and ** unpacking; "
        "* unpacking must precede ** unpacking"
    );
    return unpacking_collector<policy>(std::forward<Args>(args)...);
}

template <typename Derived>
template <return_value_policy policy, typename... Args>
object object_api<Derived>::operator()(Args &&...args) const {
    return detail::collect_arguments<policy>(std::forward<Args>(args)...).call(derived().ptr());
}

template <typename Derived>
template <return_value_policy policy, typename... Args>
object object_api<Derived>::call(Args &&...args) const {
    return operator()<policy>(std::forward<Args>(args)...);
}

NAMESPACE_END(detail)

#define PYBIND11_MAKE_OPAQUE(Type) \
    namespace pybind11 { namespace detail { \
        template<> class type_caster<Type> : public type_caster_base<Type> { }; \
    }}

NAMESPACE_END(pybind11)
