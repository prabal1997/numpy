#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define _MULTIARRAYMODULE
#define NPY_NO_PREFIX
#include "numpy/arrayobject.h"
#include "numpy/arrayscalars.h"

#include "npy_config.h"

#include "npy_3kcompat.h"

#include "buffer.h"

/*************************************************************************
 ****************   Implement Buffer Protocol ****************************
 *************************************************************************/

/* removed multiple segment interface */

static Py_ssize_t
array_getsegcount(PyArrayObject *self, Py_ssize_t *lenp)
{
    if (lenp) {
        *lenp = PyArray_NBYTES(self);
    }
    if (PyArray_ISONESEGMENT(self)) {
        return 1;
    }
    if (lenp) {
        *lenp = 0;
    }
    return 0;
}

static Py_ssize_t
array_getreadbuf(PyArrayObject *self, Py_ssize_t segment, void **ptrptr)
{
    if (segment != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "accessing non-existing array segment");
        return -1;
    }
    if (PyArray_ISONESEGMENT(self)) {
        *ptrptr = self->data;
        return PyArray_NBYTES(self);
    }
    PyErr_SetString(PyExc_ValueError, "array is not a single segment");
    *ptrptr = NULL;
    return -1;
}


static Py_ssize_t
array_getwritebuf(PyArrayObject *self, Py_ssize_t segment, void **ptrptr)
{
    if (PyArray_CHKFLAGS(self, WRITEABLE)) {
        return array_getreadbuf(self, segment, (void **) ptrptr);
    }
    else {
        PyErr_SetString(PyExc_ValueError, "array cannot be "
                        "accessed as a writeable buffer");
        return -1;
    }
}

static Py_ssize_t
array_getcharbuf(PyArrayObject *self, Py_ssize_t segment, constchar **ptrptr)
{
    return array_getreadbuf(self, segment, (void **) ptrptr);
}


/*************************************************************************
 * PEP 3118 buffer protocol
 *************************************************************************/


/*
 * Note: because for backward compatibility we cannot define bf_releasebuffer,
 * we must go through some additional contortions in bf_getbuffer.
 */

#if PY_VERSION_HEX >= 0x02060000

/*
 * Format string translator
 */

typedef struct {
    char *s;
    int allocated;
    int pos;
} _tmp_string;

typedef struct {
    char *format;
    int nd;
    Py_ssize_t *strides;
    Py_ssize_t *shape;
} _buffer_data;

static int
_append_char(_tmp_string *s, char c)
{
    char *p;
    if (s->s == NULL) {
        s->s = (char*)malloc(16);
        s->pos = 0;
        s->allocated = 16;
    }
    if (s->pos >= s->allocated) {
        p = (char*)realloc(s->s, 2*s->allocated);
        if (p == NULL) {
            PyErr_SetString(PyExc_MemoryError, "memory allocation failed");
            return -1;
        }
        s->s = p;
        s->allocated *= 2;
    }
    s->s[s->pos] = c;
    ++s->pos;
    return 0;
}

static int
_append_str(_tmp_string *s, char *c)
{
    while (*c != '\0') {
        if (_append_char(s, *c)) return -1;
        ++c;
    }
}

static int
_buffer_format_string(PyArray_Descr *descr, _tmp_string *str, int *offset)
{
    PyObject *s;
    int k;
    int zero_offset = 0;

    if (descr->subarray) {
        PyErr_SetString(PyExc_ValueError,
                        "data types with sub-arrays cannot be exported as "
                        "buffers");
        return -1;
    }
    else if (PyDataType_HASFIELDS(descr)) {
        _append_str(str, "T{");
        for (k = 0; k < PyTuple_GET_SIZE(descr->names); ++k) {
            PyObject *name, *item, *offset_obj, *tmp;
            PyArray_Descr *child;
            char *p;
            Py_ssize_t len;
            int new_offset;

            name = PyTuple_GET_ITEM(descr->names, k);
            item = PyDict_GetItem(descr->fields, name);

            child = (PyArray_Descr*)PyTuple_GetItem(item, 0);
            offset_obj = PyTuple_GetItem(item, 1);
            new_offset = PyInt_AsLong(offset_obj);

            /* Insert padding manually */
            while (*offset < new_offset) {
                _append_char(str, 'x');
                ++*offset;
            }
            *offset += child->elsize;

            /* Insert child item */
            _buffer_format_string(child, str, offset);

            /* Insert field name */
#if defined(NPY_PY3K)
#warning XXX -- should it use UTF-8 here?
            tmp = PyUnicode_AsUTF8String(name);
#else
            tmp = name;
#endif
            if (tmp == NULL || PyBytes_AsStringAndSize(tmp, &p, &len) < 0) {
                PyErr_SetString(PyExc_ValueError, "invalid field name");
                return -1;
            }
            _append_char(str, ':');
            while (len > 0) {
                if (*p == ':') {
                    Py_DECREF(tmp);
                    PyErr_SetString(PyExc_ValueError,
                                    "':' is not an allowed character in buffer "
                                    "field names");
                    return -1;
                }
                _append_char(str, *p);
                ++p;
                --len;
            }
            _append_char(str, ':');
#if defined(NPY_PY3K)
            Py_DECREF(tmp);
#endif
        }
        _append_char(str, '}');
    }
    else {
        if (descr->byteorder == '<' || descr->byteorder == '>' ||
            descr->byteorder == '=') {
            _append_char(str, descr->byteorder);
        }

        switch (descr->type_num) {
        case NPY_BYTE:         if (_append_char(str, 'b')) return -1; break;
        case NPY_UBYTE:        if (_append_char(str, 'B')) return -1; break;
        case NPY_SHORT:        if (_append_char(str, 'h')) return -1; break;
        case NPY_USHORT:       if (_append_char(str, 'H')) return -1; break;
        case NPY_INT:          if (_append_char(str, 'i')) return -1; break;
        case NPY_UINT:         if (_append_char(str, 'I')) return -1; break;
        case NPY_LONG:         if (_append_char(str, 'l')) return -1; break;
        case NPY_ULONG:        if (_append_char(str, 'L')) return -1; break;
        case NPY_LONGLONG:     if (_append_char(str, 'q')) return -1; break;
        case NPY_ULONGLONG:    if (_append_char(str, 'Q')) return -1; break;
        case NPY_FLOAT:        if (_append_char(str, 'f')) return -1; break;
        case NPY_DOUBLE:       if (_append_char(str, 'd')) return -1; break;
        case NPY_LONGDOUBLE:   if (_append_char(str, 'g')) return -1; break;
        case NPY_CFLOAT:       if (_append_str(str, "Zf")) return -1; break;
        case NPY_CDOUBLE:      if (_append_str(str, "Zd")) return -1; break;
        case NPY_CLONGDOUBLE:  if (_append_str(str, "Zg")) return -1; break;
        case NPY_STRING: {
            char buf[128];
            PyOS_snprintf(buf, sizeof(buf), "%ds", descr->elsize);
            if (_append_str(str, buf)) return -1;
            break;
        }
        case NPY_UNICODE: {
            /* Numpy Unicode is always 4-byte */
            char buf[128];
            assert(descr->elsize % 4 == 0);
            PyOS_snprintf(buf, sizeof(buf), "%dw", descr->elsize / 4);
            if (_append_str(str, buf)) return -1;
            break;
        }
        case NPY_OBJECT:       if (_append_char(str, 'O')) return -1; break;
        default:
            PyErr_Format(PyExc_ValueError, "unknown dtype code %d",
                         descr->type_num);
            return -1;
        }
    }

    return 0;
}

/*
 * Retrieving buffers
 */

static int
array_getbuffer(PyObject *obj, Py_buffer *view, int flags)
{
    PyArrayObject *self;
    _buffer_data *cache = NULL;

    self = (PyArrayObject*)obj;

    if (view == NULL) {
        return -1;
    }

    if (self->buffer_info == NULL) {
        cache = (_buffer_data*)malloc(sizeof(_buffer_data));
        cache->nd = 0;
        cache->format = NULL;
        cache->strides = NULL;
        cache->shape = NULL;
        self->buffer_info = cache;
    }
    else {
        cache = self->buffer_info;
    }

    view->format = NULL;
    view->shape = NULL;

    if ((flags & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS &&
        !PyArray_CHKFLAGS(self, NPY_C_CONTIGUOUS)) {
        PyErr_SetString(PyExc_ValueError, "ndarray is not C contiguous");
        goto fail;
    }
    if ((flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS &&
        !PyArray_CHKFLAGS(self, NPY_F_CONTIGUOUS)) {
        PyErr_SetString(PyExc_ValueError, "ndarray is not Fortran contiguous");
        goto fail;
    }
    if ((flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS
        && !PyArray_ISONESEGMENT(self)) {
        PyErr_SetString(PyExc_ValueError, "ndarray is not contiguous");
        goto fail;
    }

    view->buf = PyArray_DATA(self);
    view->suboffsets = NULL;
    view->itemsize = PyArray_ITEMSIZE(self);
    view->readonly = !PyArray_ISWRITEABLE(self);
    view->internal = NULL;
    view->len = PyArray_NBYTES(self);

    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT) {
#warning XXX -- assumes descriptors are immutable
        if (PyArray_DESCR(self)->buffer_format == NULL) {
            int offset = 0;
            _tmp_string fmt = {0,0,0};
            if (_buffer_format_string(PyArray_DESCR(self), &fmt, &offset)) {
                goto fail;
            }
            _append_char(&fmt, '\0');
            PyArray_DESCR(self)->buffer_format = fmt.s;
        }
        view->format = PyArray_DESCR(self)->buffer_format;
    }
    else {
        view->format = NULL;
    }

    if ((flags & PyBUF_STRIDED) == PyBUF_STRIDED) {
        int shape_changed = 0;
        int k;

        if (cache->nd != PyArray_NDIM(self)) {
            shape_changed = 1;
        }
        else {
            for (k = 0; k < PyArray_NDIM(self); ++k) {
                if (cache->shape[k] != PyArray_DIMS(self)[k]
                    || cache->strides[k] != PyArray_STRIDES(self)[k]) {
                    shape_changed = 1;
                    break;
                }
            }
        }

        if (cache->strides == NULL || shape_changed) {
            if (cache->shape != NULL) {
                free(cache->shape);
            }
            cache->nd = PyArray_NDIM(self);
            cache->shape = (Py_ssize_t*)malloc(sizeof(Py_ssize_t)
                                               * PyArray_NDIM(self) * 2 + 1);
            cache->strides = cache->shape + PyArray_NDIM(self);
            for (k = 0; k < PyArray_NDIM(self); ++k) {
                cache->shape[k] = PyArray_DIMS(self)[k];
                cache->strides[k] = PyArray_STRIDES(self)[k];
            }
        }

        view->ndim = PyArray_NDIM(self);
        view->shape = cache->shape;
        view->strides = cache->strides;
    }
    else if (PyArray_ISONESEGMENT(self)) {
        view->ndim = 0;
        view->shape = NULL;
        view->strides = NULL;
    }
    else {
        PyErr_SetString(PyExc_ValueError, "ndarray is not single-segment");
        goto fail;
    }

    view->obj = self;
    Py_INCREF(self);

    return 0;

fail:
    return -1;
}


/*
 * NOTE: for backward compatibility (esp. with PyArg_ParseTuple("s#", ...))
 * we do *not* define bf_releasebuffer at all.
 *
 * Instead, any extra data allocated with the buffer is released only in
 * array_dealloc.
 *
 * Ensuring that the buffer stays in place is taken care by refcounting;
 * ndarrays do not reallocate if there are references to them, and a buffer
 * view holds one reference.
 */

NPY_NO_EXPORT void
_array_dealloc_buffer_info(PyArrayObject *self)
{
    _buffer_data *cache;

    cache = self->buffer_info;

    if (cache != NULL) {
        if (cache->shape) {
            free(cache->shape);
        }
        free(cache);
        self->buffer_info = NULL;
    }
}

#else

NPY_NO_EXPORT void
_array_dealloc_buffer_info(PyArrayObject *self)
{
}

#endif

/*************************************************************************/

NPY_NO_EXPORT PyBufferProcs array_as_buffer = {
#if !defined(NPY_PY3K)
#if PY_VERSION_HEX >= 0x02050000
    (readbufferproc)array_getreadbuf,       /*bf_getreadbuffer*/
    (writebufferproc)array_getwritebuf,     /*bf_getwritebuffer*/
    (segcountproc)array_getsegcount,        /*bf_getsegcount*/
    (charbufferproc)array_getcharbuf,       /*bf_getcharbuffer*/
#else
    (getreadbufferproc)array_getreadbuf,    /*bf_getreadbuffer*/
    (getwritebufferproc)array_getwritebuf,  /*bf_getwritebuffer*/
    (getsegcountproc)array_getsegcount,     /*bf_getsegcount*/
    (getcharbufferproc)array_getcharbuf,    /*bf_getcharbuffer*/
#endif
#endif
#if PY_VERSION_HEX >= 0x02060000
    (getbufferproc)array_getbuffer,
    (releasebufferproc)0,
#endif
};
