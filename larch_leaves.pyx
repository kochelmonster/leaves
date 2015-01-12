#cython: boundscheck=False, profile=False
#@+leo-ver=5-thin
#@+node:michael.20150106224503.15: * @file larch_leaves.pyx
#@@first
#@@language cython
#@@tabwidth -4
from libcpp.string cimport string
from libcpp cimport bool 
from cpython.ref cimport Py_XINCREF, Py_XDECREF, PyObject
from cpython.bytes cimport PyBytes_AS_STRING, PyBytes_GET_SIZE

cdef extern from "larch/leaves.h" namespace "larch_leaves":
    cdef cppclass Slice:
        Slice() nogil
        Slice(const char *data, size_t size) nogil
    
        const char* data() nogil
        size_t size() nogil
        bool empty() nogil

    cdef cppclass MemoryDatabase:
        bool is_valid() nogil
        size_t count() nogil
        void find(const Slice& key)  nogil
        void first() nogil
        void last() nogil
        void next() nogil
        void prev() nogil
        Slice key() nogil except+
        Slice value() nogil except+
        void set_value(const Slice& value) nogil except+
        void remove() nogil except+
  
    cdef MemoryDatabase* create "larch_leaves::MemoryDatabase::create"()  nogil except+


cdef class _MemoryDatabase:
    cdef MemoryDatabase *pthis
        
    def __init__(self):
        self.pthis = create()
        
    def __dealloc__(self):
        del self.pthis
        self.pthis = NULL
        
    def __len__(self):
        return self.pthis.count()
        
    def is_valid(self):
        return self.pthis.is_valid()
        
    def first(self):
        self.pthis.first()
        return self.pthis.is_valid()
        
    def last(self):
        self.pthis.last()
        return self.pthis.is_valid()
        
    def next(self):
        self.pthis.next()
        return self.pthis.is_valid()
        
    def prev(self):
        self.prev()
        return self.pthis.is_valid()
    
    cpdef value(self):
        cdef:
            PyObject* value
            Slice bvalue
            
        bvalue = self.pthis.value()
        value = (<PyObject**>bvalue.data())[0]
        return <object>value
        
    cpdef set_value(self, obj):
        cdef:
            PyObject* value
            Slice bvalue
            
        if self.pthis.is_valid():
            bvalue = self.pthis.value()
            value = (<PyObject**>bvalue.data())[0]
            Py_XDECREF(value)
            
        value = <PyObject*>obj
        Py_XINCREF(value)
        self.pthis.set_value(Slice(<const char*>value, sizeof(PyObject*)))
        
    def remove(self):
        self.pthis.remove()
        
    def handle(self):
        return <long>self.pthis
        
        
cdef class MemoryDatabaseBytes(_MemoryDatabase):
    cdef int find_count
        
    def __init__(self):
        super(MemoryDatabaseBytes, self).__init__()
        self.find_count = 0
        
    cpdef bool find(self, bytes key):
        self.find_count += 1
        self.pthis.find(
            Slice(PyBytes_AS_STRING(key), PyBytes_GET_SIZE(key)))
        return self.pthis.is_valid()
        
    cpdef key(self):
        cdef Slice skey;
        skey = self.pthis.key()
        return skey.data()[:skey.size()]
        
    def __getitem__(self, bytes key):
        if (self.find(key)):
            return self.value()
            
        raise KeyError(key)
        
    def __setitem__(self, bytes key, obj):
        self.find(key)
        self.set_value(obj)
        
    def __delitem__(self, bytes key):
        self.find(key)
        self.pthis.remove()
        
    def __iter__(self):
        cdef:
            bytes last_key
            int last_find_count = self.find_count
            
        self.pthis.first()
        while self.pthis.is_valid():
            last_key = self.key()
            yield last_key, self.value()
            
            if self.find_count != last_find_count:
                # bring back to the last iter position
                self.find(last_key)
                last_find_count = self.find_count
            
            self.pthis.next()
        
#@-leo
