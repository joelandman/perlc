use lib 'tests/lib';
use ParentBegin;
print "P=", ParentBegin::FLAG(), "\n";
print "Cdef=", defined(&ChildBegin::FLAG) ? "Y" : "N", "\n";
print "C=", ChildBegin::FLAG(), "\n";
print "show=", ChildBegin::show(), "\n";
