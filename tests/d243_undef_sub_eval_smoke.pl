eval { NoSuch::func(1); };
print "caught=[$@]";
print "alive\n";
