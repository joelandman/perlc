# D261: compile-time use finds a non-native module in the host privlib
# without -I. Pod::Escapes is not on the native-module skip list.
use Pod::Escapes;
print "sub=", (defined &Pod::Escapes::e2char ? 1 : 0), "\n";
