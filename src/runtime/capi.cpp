// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* method_cls;

#define MAKE_CHECK(NAME, cls_name)                                                                                     \
    extern "C" bool Py##NAME##_Check(PyObject* op) noexcept { return isSubclass(op->cls, cls_name); }
#define MAKE_CHECK2(NAME, cls_name)                                                                                    \
    extern "C" bool _Py##NAME##_Check(PyObject* op) noexcept { return isSubclass(op->cls, cls_name); }

MAKE_CHECK2(Int, int_cls)
MAKE_CHECK2(String, str_cls)
MAKE_CHECK(Long, long_cls)
MAKE_CHECK(List, list_cls)
MAKE_CHECK(Tuple, tuple_cls)
MAKE_CHECK(Dict, dict_cls)
MAKE_CHECK(Slice, slice_cls)
MAKE_CHECK(Type, type_cls)

#ifdef Py_USING_UNICODE
MAKE_CHECK(Unicode, unicode_cls)
#endif

#undef MAKE_CHECK
#undef MAKE_CHECK2

extern "C" bool _PyIndex_Check(PyObject* op) noexcept {
    // TODO this is wrong (the CPython version checks for things that can be coerced to a number):
    return PyInt_Check(op);
}

extern "C" {
int Py_Py3kWarningFlag;
}

BoxedClass* capifunc_cls;

BoxedClass* wrapperdescr_cls, *wrapperobject_cls;

Box* BoxedWrapperDescriptor::__get__(BoxedWrapperDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == wrapperdescr_cls, "");

    if (inst == None)
        return self;

    if (!isSubclass(inst->cls, self->type))
        raiseExcHelper(TypeError, "Descriptor '' for '%s' objects doesn't apply to '%s' object",
                       getFullNameOfClass(self->type).c_str(), getFullTypeName(inst).c_str());

    return new BoxedWrapperObject(self, inst);
}

// copied from CPython's getargs.c:
extern "C" int PyBuffer_FillInfo(Py_buffer* view, PyObject* obj, void* buf, Py_ssize_t len, int readonly,
                                 int flags) noexcept {
    if (view == NULL)
        return 0;
    if (((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) && (readonly == 1)) {
        // Don't support PyErr_SetString yet:
        assert(0);
        // PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        // return -1;
    }

    view->obj = obj;
    if (obj)
        Py_INCREF(obj);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
        view->format = "B";
    view->ndim = 1;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND)
        view->shape = &(view->len);
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
        view->strides = &(view->itemsize);
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

extern "C" void PyBuffer_Release(Py_buffer* view) noexcept {
    if (!view->buf) {
        assert(!view->obj);
        return;
    }

    PyObject* obj = view->obj;
    assert(obj);
    assert(obj->cls == str_cls);
    if (obj && Py_TYPE(obj)->tp_as_buffer && Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer)
        Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer(obj, view);
    Py_XDECREF(obj);
    view->obj = NULL;
}

extern "C" void _PyErr_BadInternalCall(const char* filename, int lineno) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyVarObject* PyObject_InitVar(PyVarObject* op, PyTypeObject* tp, Py_ssize_t size) noexcept {
    assert(gc::isValidGCObject(op));
    assert(gc::isValidGCObject(tp));

    RELEASE_ASSERT(op, "");
    RELEASE_ASSERT(tp, "");
    Py_TYPE(op) = tp;
    op->ob_size = size;
    return op;
}

extern "C" void PyObject_Free(void* p) noexcept {
    gc::gc_free(p);
    ASSERT(0, "I think this is good enough but I'm not sure; should test");
}

extern "C" PyObject* PyObject_CallObject(PyObject* obj, PyObject* args) noexcept {
    RELEASE_ASSERT(args, ""); // actually it looks like this is allowed to be NULL
    RELEASE_ASSERT(args->cls == tuple_cls, "");

    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        Box* r = runtimeCall(obj, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
        return r;
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyObject_CallMethod(PyObject* o, char* name, char* format, ...) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyObject_CallMethod_SizeT(PyObject* o, char* name, char* format, ...) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyObject_Size(PyObject* o) noexcept {
    try {
        return len(o)->n;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" PyObject* PyObject_GetIter(PyObject* o) noexcept {
    try {
        return getiter(o);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyObject_Repr(PyObject* obj) noexcept {
    try {
        return repr(obj);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyObject_Format(PyObject* obj, PyObject* format_spec) noexcept {
    PyObject* empty = NULL;
    PyObject* result = NULL;
#ifdef Py_USING_UNICODE
    int spec_is_unicode;
    int result_is_unicode;
#endif

    /* If no format_spec is provided, use an empty string */
    if (format_spec == NULL) {
        empty = PyString_FromStringAndSize(NULL, 0);
        format_spec = empty;
    }

/* Check the format_spec type, and make sure it's str or unicode */
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(format_spec))
        spec_is_unicode = 1;
    else if (PyString_Check(format_spec))
        spec_is_unicode = 0;
    else {
#else
    if (!PyString_Check(format_spec)) {
#endif
        PyErr_Format(PyExc_TypeError, "format expects arg 2 to be string "
                                      "or unicode, not %.100s",
                     Py_TYPE(format_spec)->tp_name);
        goto done;
    }

    /* Check for a __format__ method and call it. */
    if (PyInstance_Check(obj)) {
        /* We're an instance of a classic class */
        PyObject* bound_method = PyObject_GetAttrString(obj, "__format__");
        if (bound_method != NULL) {
            result = PyObject_CallFunctionObjArgs(bound_method, format_spec, NULL);
            Py_DECREF(bound_method);
        } else {
            PyObject* self_as_str = NULL;
            PyObject* format_method = NULL;
            Py_ssize_t format_len;

            PyErr_Clear();
/* Per the PEP, convert to str (or unicode,
   depending on the type of the format
   specifier).  For new-style classes, this
   logic is done by object.__format__(). */
#ifdef Py_USING_UNICODE
            if (spec_is_unicode) {
                format_len = PyUnicode_GET_SIZE(format_spec);
                self_as_str = PyObject_Unicode(obj);
            } else
#endif
            {
                format_len = PyString_GET_SIZE(format_spec);
                self_as_str = PyObject_Str(obj);
            }
            if (self_as_str == NULL)
                goto done1;

            if (format_len > 0) {
                /* See the almost identical code in
                   typeobject.c for new-style
                   classes. */
                if (PyErr_WarnEx(PyExc_PendingDeprecationWarning, "object.__format__ with a non-empty "
                                                                  "format string is deprecated",
                                 1) < 0) {
                    goto done1;
                }
                /* Eventually this will become an
                   error:
                PyErr_Format(PyExc_TypeError,
                   "non-empty format string passed to "
                   "object.__format__");
                goto done1;
                */
            }

            /* Then call str.__format__ on that result */
            format_method = PyObject_GetAttrString(self_as_str, "__format__");
            if (format_method == NULL) {
                goto done1;
            }
            result = PyObject_CallFunctionObjArgs(format_method, format_spec, NULL);
        done1:
            Py_XDECREF(self_as_str);
            Py_XDECREF(format_method);
            if (result == NULL)
                goto done;
        }
    } else {
        /* Not an instance of a classic class, use the code
           from py3k */
        static PyObject* format_cache = NULL;

        /* Find the (unbound!) __format__ method (a borrowed
           reference) */
        PyObject* method = _PyObject_LookupSpecial(obj, "__format__", &format_cache);
        if (method == NULL) {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_TypeError, "Type %.100s doesn't define __format__", Py_TYPE(obj)->tp_name);
            goto done;
        }
        /* And call it. */
        result = PyObject_CallFunctionObjArgs(method, format_spec, NULL);
        Py_DECREF(method);
    }

    if (result == NULL)
        goto done;

/* Check the result type, and make sure it's str or unicode */
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(result))
        result_is_unicode = 1;
    else if (PyString_Check(result))
        result_is_unicode = 0;
    else {
#else
    if (!PyString_Check(result)) {
#endif
        PyErr_Format(PyExc_TypeError, "%.100s.__format__ must return string or "
                                      "unicode, not %.100s",
                     Py_TYPE(obj)->tp_name, Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        result = NULL;
        goto done;
    }

/* Convert to unicode, if needed.  Required if spec is unicode
   and result is str */
#ifdef Py_USING_UNICODE
    if (spec_is_unicode && !result_is_unicode) {
        PyObject* tmp = PyObject_Unicode(result);
        /* This logic works whether or not tmp is NULL */
        Py_DECREF(result);
        result = tmp;
    }
#endif

done:
    Py_XDECREF(empty);
    return result;
}


extern "C" PyObject* PyObject_GetAttr(PyObject* o, PyObject* attr_name) noexcept {
    if (!isSubclass(attr_name->cls, str_cls)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr_name)->tp_name);
        return NULL;
    }

    try {
        return getattr(o, static_cast<BoxedString*>(attr_name)->s.c_str());
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyObject_GenericGetAttr(PyObject* o, PyObject* name) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_GetItem(PyObject* o, PyObject* key) noexcept {
    try {
        return getitem(o, key);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_SetItem(PyObject* o, PyObject* key, PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_DelItem(PyObject* o, PyObject* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_RichCompare(PyObject* o1, PyObject* o2, int opid) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" {
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
}

extern "C" long PyObject_Hash(PyObject* o) noexcept {
    try {
        return hash(o)->n;
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" long PyObject_HashNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'", Py_TYPE(self)->tp_name);
    return -1;
}

extern "C" PyObject* _PyObject_NextNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable", Py_TYPE(self)->tp_name);
    return NULL;
}

extern "C" long _Py_HashPointer(void* p) noexcept {
    long x;
    size_t y = (size_t)p;
    /* bottom 3 or 4 bits are likely to be 0; rotate y by 4 to avoid
       excessive hash collisions for dicts and sets */
    y = (y >> 4) | (y << (8 * SIZEOF_VOID_P - 4));
    x = (long)y;
    if (x == -1)
        x = -2;
    return x;
}

extern "C" int PyObject_IsTrue(PyObject* o) noexcept {
    try {
        return nonzero(o);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}


extern "C" int PyObject_Not(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyEval_CallObjectWithKeywords(PyObject* func, PyObject* arg, PyObject* kw) noexcept {
    PyObject* result;

    if (arg == NULL) {
        arg = PyTuple_New(0);
        if (arg == NULL)
            return NULL;
    } else if (!PyTuple_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    } else
        Py_INCREF(arg);

    if (kw != NULL && !PyDict_Check(kw)) {
        PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary");
        Py_DECREF(arg);
        return NULL;
    }

    result = PyObject_Call(func, arg, kw);
    Py_DECREF(arg);
    return result;
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) noexcept {
    try {
        if (kw)
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
        else
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" void PyObject_ClearWeakRefs(PyObject* object) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_GetBuffer(PyObject* exporter, Py_buffer* view, int flags) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_Print(PyObject* obj, FILE* fp, int flags) noexcept {
    Py_FatalError("unimplemented");
};

extern "C" PyObject* PySequence_Repeat(PyObject* o, Py_ssize_t count) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_InPlaceConcat(PyObject* o1, PyObject* o2) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_InPlaceRepeat(PyObject* o, Py_ssize_t count) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_GetItem(PyObject* o, Py_ssize_t i) noexcept {
    try {
        // Not sure if this is really the same:
        return getitem(o, boxInt(i));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PySequence_GetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    try {
        // Not sure if this is really the same:
        return getitem(o, new BoxedSlice(boxInt(i1), boxInt(i2), None));
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" int PySequence_SetItem(PyObject* o, Py_ssize_t i, PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_DelItem(PyObject* o, Py_ssize_t i) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_SetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2, PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_DelSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PySequence_Count(PyObject* o, PyObject* value) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_Contains(PyObject* o, PyObject* value) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PySequence_Index(PyObject* o, PyObject* value) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Tuple(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Fast(PyObject* o, const char* m) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyIter_Next(PyObject* iter) noexcept {
    static const std::string next_str("next");
    try {
        return callattr(iter, &next_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = false }),
                        ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyCallable_Check(PyObject* x) noexcept {
    if (x == NULL)
        return 0;

    static const std::string call_attr("__call__");
    return typeLookup(x->cls, call_attr, NULL) != NULL;
}

extern "C" int Py_FlushLine(void) noexcept {
    PyObject* f = PySys_GetObject("stdout");
    if (f == NULL)
        return 0;
    if (!PyFile_SoftSpace(f, 0))
        return 0;
    return PyFile_WriteString("\n", f);
}

extern "C" void PyErr_NormalizeException(PyObject** exc, PyObject** val, PyObject** tb) noexcept {
    PyObject* type = *exc;
    PyObject* value = *val;
    PyObject* inclass = NULL;
    PyObject* initial_tb = NULL;
    PyThreadState* tstate = NULL;

    if (type == NULL) {
        /* There was no exception, so nothing to do. */
        return;
    }

    /* If PyErr_SetNone() was used, the value will have been actually
       set to NULL.
    */
    if (!value) {
        value = Py_None;
        Py_INCREF(value);
    }

    if (PyExceptionInstance_Check(value))
        inclass = PyExceptionInstance_Class(value);

    /* Normalize the exception so that if the type is a class, the
       value will be an instance.
    */
    if (PyExceptionClass_Check(type)) {
        /* if the value was not an instance, or is not an instance
           whose class is (or is derived from) type, then use the
           value as an argument to instantiation of the type
           class.
        */
        if (!inclass || !PyObject_IsSubclass(inclass, type)) {
            PyObject* args, *res;

            if (value == Py_None)
                args = PyTuple_New(0);
            else if (PyTuple_Check(value)) {
                Py_INCREF(value);
                args = value;
            } else
                args = PyTuple_Pack(1, value);

            if (args == NULL)
                goto finally;
            res = PyEval_CallObject(type, args);
            Py_DECREF(args);
            if (res == NULL)
                goto finally;
            Py_DECREF(value);
            value = res;
        }
        /* if the class of the instance doesn't exactly match the
           class of the type, believe the instance
        */
        else if (inclass != type) {
            Py_DECREF(type);
            type = inclass;
            Py_INCREF(type);
        }
    }
    *exc = type;
    *val = value;
    return;
finally:
    Py_DECREF(type);
    Py_DECREF(value);
    /* If the new exception doesn't set a traceback and the old
       exception had a traceback, use the old traceback for the
       new exception.  It's better than nothing.
    */
    initial_tb = *tb;
    PyErr_Fetch(exc, val, tb);
    if (initial_tb != NULL) {
        if (*tb == NULL)
            *tb = initial_tb;
        else
            Py_DECREF(initial_tb);
    }
    /* normalize recursively */
    tstate = PyThreadState_GET();
    if (++tstate->recursion_depth > Py_GetRecursionLimit()) {
        --tstate->recursion_depth;
        /* throw away the old exception... */
        Py_DECREF(*exc);
        Py_DECREF(*val);
        /* ... and use the recursion error instead */
        *exc = PyExc_RuntimeError;
        *val = PyExc_RecursionErrorInst;
        Py_INCREF(*exc);
        Py_INCREF(*val);
        /* just keeping the old traceback */
        return;
    }
    PyErr_NormalizeException(exc, val, tb);
    --tstate->recursion_depth;
}

void setCAPIException(const ExcInfo& e) {
    cur_thread_state.curexc_type = e.type;
    cur_thread_state.curexc_value = e.value;
    cur_thread_state.curexc_traceback = e.traceback;
}

void checkAndThrowCAPIException() {
    Box* _type = cur_thread_state.curexc_type;
    if (!_type)
        assert(!cur_thread_state.curexc_value);

    if (_type) {
        RELEASE_ASSERT(cur_thread_state.curexc_traceback == NULL, "unsupported");
        BoxedClass* type = static_cast<BoxedClass*>(_type);
        assert(isInstance(_type, type_cls) && isSubclass(static_cast<BoxedClass*>(type), BaseException)
               && "Only support throwing subclass of BaseException for now");

        Box* value = cur_thread_state.curexc_value;
        if (!value)
            value = None;

        // This is similar to PyErr_NormalizeException:
        if (!isInstance(value, type)) {
            if (value->cls == tuple_cls) {
                value = runtimeCall(cur_thread_state.curexc_type, ArgPassSpec(0, 0, true, false), value, NULL, NULL,
                                    NULL, NULL);
            } else if (value == None) {
                value = runtimeCall(cur_thread_state.curexc_type, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            } else {
                value = runtimeCall(cur_thread_state.curexc_type, ArgPassSpec(1), value, NULL, NULL, NULL, NULL);
            }
        }

        RELEASE_ASSERT(value->cls == type, "unsupported");

        PyErr_Clear();
        raiseExc(value);
    }
}

extern "C" void PyErr_Restore(PyObject* type, PyObject* value, PyObject* traceback) noexcept {
    cur_thread_state.curexc_type = type;
    cur_thread_state.curexc_value = value;
    cur_thread_state.curexc_traceback = traceback;
}

extern "C" void PyErr_Clear() noexcept {
    PyErr_Restore(NULL, NULL, NULL);
}

extern "C" void PyErr_SetString(PyObject* exception, const char* string) noexcept {
    PyErr_SetObject(exception, boxStrConstant(string));
}

extern "C" void PyErr_SetObject(PyObject* exception, PyObject* value) noexcept {
    PyErr_Restore(exception, value, NULL);
}

extern "C" PyObject* PyErr_Format(PyObject* exception, const char* format, ...) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyErr_NoMemory() noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyErr_CheckSignals() noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyExceptionClass_Check(PyObject* o) noexcept {
    return PyClass_Check(o) || (PyType_Check(o) && isSubclass(static_cast<BoxedClass*>(o), BaseException));
}

extern "C" int PyExceptionInstance_Check(PyObject* o) noexcept {
    return PyInstance_Check(o) || isSubclass(o->cls, BaseException);
}

extern "C" const char* PyExceptionClass_Name(PyObject* o) noexcept {
    return PyClass_Check(o) ? PyString_AS_STRING(static_cast<BoxedClassobj*>(o)->name)
                            : static_cast<BoxedClass*>(o)->tp_name;
}

extern "C" PyObject* PyExceptionInstance_Class(PyObject* o) noexcept {
    return PyInstance_Check(o) ? (Box*)static_cast<BoxedInstance*>(o)->inst_cls : o->cls;
}

extern "C" int PyTraceBack_Print(PyObject* v, PyObject* f) noexcept {
    Py_FatalError("unimplemented");
}

#define Py_DEFAULT_RECURSION_LIMIT 1000
static int recursion_limit = Py_DEFAULT_RECURSION_LIMIT;
extern "C" {
int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;
}

/* the macro Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
extern "C" int _Py_CheckRecursiveCall(const char* where) noexcept {
    PyThreadState* tstate = PyThreadState_GET();

#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        PyErr_SetString(PyExc_MemoryError, "Stack overflow");
        return -1;
    }
#endif
    if (tstate->recursion_depth > recursion_limit) {
        --tstate->recursion_depth;
        PyErr_Format(PyExc_RuntimeError, "maximum recursion depth exceeded%s", where);
        return -1;
    }
    _Py_CheckRecursionLimit = recursion_limit;
    return 0;
}

extern "C" int Py_GetRecursionLimit(void) noexcept {
    return recursion_limit;
}

extern "C" void Py_SetRecursionLimit(int new_limit) noexcept {
    recursion_limit = new_limit;
    _Py_CheckRecursionLimit = recursion_limit;
}

extern "C" int PyErr_GivenExceptionMatches(PyObject* err, PyObject* exc) noexcept {
    if (err == NULL || exc == NULL) {
        /* maybe caused by "import exceptions" that failed early on */
        return 0;
    }
    if (PyTuple_Check(exc)) {
        Py_ssize_t i, n;
        n = PyTuple_Size(exc);
        for (i = 0; i < n; i++) {
            /* Test recursively */
            if (PyErr_GivenExceptionMatches(err, PyTuple_GET_ITEM(exc, i))) {
                return 1;
            }
        }
        return 0;
    }
    /* err might be an instance, so check its class. */
    if (PyExceptionInstance_Check(err))
        err = PyExceptionInstance_Class(err);

    if (PyExceptionClass_Check(err) && PyExceptionClass_Check(exc)) {
        int res = 0, reclimit;
        PyObject* exception, *value, *tb;
        PyErr_Fetch(&exception, &value, &tb);
        /* Temporarily bump the recursion limit, so that in the most
           common case PyObject_IsSubclass will not raise a recursion
           error we have to ignore anyway.  Don't do it when the limit
           is already insanely high, to avoid overflow */
        reclimit = Py_GetRecursionLimit();
        if (reclimit < (1 << 30))
            Py_SetRecursionLimit(reclimit + 5);
        res = PyObject_IsSubclass(err, exc);
        Py_SetRecursionLimit(reclimit);
        /* This function must not fail, so print the error here */
        if (res == -1) {
            PyErr_WriteUnraisable(err);
            res = 0;
        }
        PyErr_Restore(exception, value, tb);
        return res;
    }

    return err == exc;
}

extern "C" int PyErr_ExceptionMatches(PyObject* exc) noexcept {
    return PyErr_GivenExceptionMatches(PyErr_Occurred(), exc);
}

extern "C" PyObject* PyErr_Occurred() noexcept {
    return cur_thread_state.curexc_type;
}

extern "C" int PyErr_WarnEx(PyObject* category, const char* text, Py_ssize_t stacklevel) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyImport_Import(PyObject* module_name) noexcept {
    RELEASE_ASSERT(module_name, "");
    RELEASE_ASSERT(module_name->cls == str_cls, "");

    try {
        return import(-1, None, &static_cast<BoxedString*>(module_name)->s);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}


extern "C" PyObject* PyCallIter_New(PyObject* callable, PyObject* sentinel) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" void* PyMem_Malloc(size_t sz) noexcept {
    return gc_compat_malloc(sz);
}

extern "C" void* PyMem_Realloc(void* ptr, size_t sz) noexcept {
    return gc_compat_realloc(ptr, sz);
}

extern "C" void PyMem_Free(void* ptr) noexcept {
    gc_compat_free(ptr);
}

extern "C" int PyNumber_Check(PyObject* obj) noexcept {
    assert(obj && obj->cls);

    // Our check, since we don't currently fill in tp_as_number:
    if (isSubclass(obj->cls, int_cls) || isSubclass(obj->cls, long_cls))
        return true;

    // The CPython check:
    return obj->cls->tp_as_number && (obj->cls->tp_as_number->nb_int || obj->cls->tp_as_number->nb_float);
}

extern "C" PyObject* PyNumber_Add(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Add);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Subtract(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Sub);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Multiply(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mult);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divide(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Div);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_FloorDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_TrueDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Remainder(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mod);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divmod(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Power(PyObject*, PyObject*, PyObject* o3) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Negative(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Positive(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Absolute(PyObject* o) noexcept {
    try {
        return abs_(o);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Invert(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Lshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Rshift(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::RShift);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_And(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::BitAnd);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Xor(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Or(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAdd(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceSubtract(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceMultiply(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceFloorDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceTrueDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRemainder(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlacePower(PyObject*, PyObject*, PyObject* o3) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceLshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAnd(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceXor(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceOr(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_Coerce(PyObject**, PyObject**) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_CoerceEx(PyObject**, PyObject**) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Int(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Long(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Float(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Index(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_ToBase(PyObject* n, int base) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyNumber_AsSsize_t(PyObject* o, PyObject* exc) noexcept {
    RELEASE_ASSERT(o->cls != long_cls, "unhandled");

    RELEASE_ASSERT(isSubclass(o->cls, int_cls), "??");
    int64_t n = static_cast<BoxedInt*>(o)->n;
    static_assert(sizeof(n) == sizeof(Py_ssize_t), "");
    return n;
}

extern "C" Py_ssize_t PyUnicode_GET_SIZE(PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_GET_DATA_SIZE(PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE* PyUnicode_AS_UNICODE(PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" const char* PyUnicode_AS_DATA(PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyBuffer_IsContiguous(Py_buffer* view, char fort) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyOS_snprintf(char* str, size_t size, const char* format, ...) noexcept {
    int rc;
    va_list va;

    va_start(va, format);
    rc = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return rc;
}

extern "C" int PyOS_vsnprintf(char* str, size_t size, const char* format, va_list va) noexcept {
    int len; /* # bytes written, excluding \0 */
#ifdef HAVE_SNPRINTF
#define _PyOS_vsnprintf_EXTRA_SPACE 1
#else
#define _PyOS_vsnprintf_EXTRA_SPACE 512
    char* buffer;
#endif
    assert(str != NULL);
    assert(size > 0);
    assert(format != NULL);
    /* We take a size_t as input but return an int.  Sanity check
     * our input so that it won't cause an overflow in the
     * vsnprintf return value or the buffer malloc size.  */
    if (size > INT_MAX - _PyOS_vsnprintf_EXTRA_SPACE) {
        len = -666;
        goto Done;
    }

#ifdef HAVE_SNPRINTF
    len = vsnprintf(str, size, format, va);
#else
    /* Emulate it. */
    buffer = (char*)PyMem_MALLOC(size + _PyOS_vsnprintf_EXTRA_SPACE);
    if (buffer == NULL) {
        len = -666;
        goto Done;
    }

    len = vsprintf(buffer, format, va);
    if (len < 0)
        /* ignore the error */;

    else if ((size_t)len >= size + _PyOS_vsnprintf_EXTRA_SPACE)
        Py_FatalError("Buffer overflow in PyOS_snprintf/PyOS_vsnprintf");

    else {
        const size_t to_copy = (size_t)len < size ? (size_t)len : size - 1;
        assert(to_copy < size);
        memcpy(str, buffer, to_copy);
        str[to_copy] = '\0';
    }
    PyMem_FREE(buffer);
#endif
Done:
    if (size > 0)
        str[size - 1] = '\0';
    return len;
#undef _PyOS_vsnprintf_EXTRA_SPACE
}

extern "C" void PyOS_AfterFork(void) noexcept {
    // TODO CPython does a number of things after a fork:
    // - clears pending signals
    // - updates the cached "main_pid"
    // - reinitialize and reacquire the GIL
    // - reinitialize the import lock
    // - change the definition of the main thread to the current thread
    // - call threading._after_fork
    // Also see PyEval_ReInitThreads

    // Should we disable finalizers after a fork?
    // In CPython, I think all garbage from other threads will never be freed and
    // their destructors never run.  I think for us, we will presumably collect it
    // and run the finalizers.  It's probably just safer to run no finalizers?

    // Our handling right now is pretty minimal... you better just call exec().

    PyEval_ReInitThreads();
    _PyImport_ReInitLock();
}

extern "C" {
static int dev_urandom_python(char* buffer, Py_ssize_t size) noexcept {
    int fd;
    Py_ssize_t n;

    if (size <= 0)
        return 0;

    Py_BEGIN_ALLOW_THREADS fd = ::open("/dev/urandom", O_RDONLY);
    Py_END_ALLOW_THREADS if (fd < 0) {
        if (errno == ENOENT || errno == ENXIO || errno == ENODEV || errno == EACCES)
            PyErr_SetString(PyExc_NotImplementedError, "/dev/urandom (or equivalent) not found");
        else
            PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    Py_BEGIN_ALLOW_THREADS do {
        do {
            n = read(fd, buffer, (size_t)size);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            break;
        buffer += n;
        size -= (Py_ssize_t)n;
    }
    while (0 < size)
        ;
    Py_END_ALLOW_THREADS

        if (n <= 0) {
        /* stop on error or if read(size) returned 0 */
        if (n < 0)
            PyErr_SetFromErrno(PyExc_OSError);
        else
            PyErr_Format(PyExc_RuntimeError, "Failed to read %zi bytes from /dev/urandom", size);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
}

extern "C" PyObject* PyThreadState_GetDict(void) noexcept {
    Box* dict = cur_thread_state.dict;
    if (!dict) {
        dict = cur_thread_state.dict = new BoxedDict();
    }
    return dict;
}

extern "C" int _PyOS_URandom(void* buffer, Py_ssize_t size) noexcept {
    if (size < 0) {
        PyErr_Format(PyExc_ValueError, "negative argument not allowed");
        return -1;
    }
    if (size == 0)
        return 0;

#ifdef MS_WINDOWS
    return win32_urandom((unsigned char*)buffer, size, 1);
#else
#ifdef __VMS
    return vms_urandom((unsigned char*)buffer, size, 1);
#else
    return dev_urandom_python((char*)buffer, size);
#endif
#endif
}

BoxedModule* importTestExtension(const std::string& name) {
    std::string pathname_name = "test/test_extension/" + name + ".pyston.so";
    const char* pathname = pathname_name.c_str();
    void* handle = dlopen(pathname, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    std::string initname = "init" + name;
    void (*init)() = (void (*)())dlsym(handle, initname.c_str());

    char* error;
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    assert(init);
    (*init)();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStrConstant(name.c_str());
    Box* _m = sys_modules->d[s];
    RELEASE_ASSERT(_m, "module failed to initialize properly?");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    m->setattr("__file__", boxStrConstant(pathname), NULL);
    m->fn = pathname;
    return m;
}

void setupCAPI() {
    capifunc_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, true)));

    capifunc_cls->freeze();

    method_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedMethodDescriptor), false);
    method_cls->giveAttr("__name__", boxStrConstant("method"));
    method_cls->giveAttr("__get__",
                         new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__get__, UNKNOWN, 3)));
    method_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2,
                                                                     0, true, true)));
    method_cls->freeze();

    wrapperdescr_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedWrapperDescriptor), false);
    wrapperdescr_cls->giveAttr("__name__", boxStrConstant("wrapper_descriptor"));
    wrapperdescr_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__get__, UNKNOWN, 3)));
    wrapperdescr_cls->freeze();

    wrapperobject_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedWrapperObject), false);
    wrapperobject_cls->giveAttr("__name__", boxStrConstant("method-wrapper"));
    wrapperobject_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperObject::__call__, UNKNOWN, 1, 0, true, true)));
    wrapperobject_cls->freeze();
}

void teardownCAPI() {
}
}
