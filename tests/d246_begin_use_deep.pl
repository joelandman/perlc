use lib 'tests/lib';
use ParentBegin;
print "show=", ChildBegin::show(), "\n";
print "num=", (ChildBegin::FLAG() > 1) ? "Y" : "N", "\n";
print "eq0=", (ChildBegin::FLAG() == 0) ? "Y" : "N", "\n";
BEGIN { *LocalFlag = sub () { 0 } unless defined &LocalFlag }
print "local=", (LocalFlag and "T") || "F", "\n";
print "bare=", (ParentBegin::FLAG and "T") || "F", "\n";
