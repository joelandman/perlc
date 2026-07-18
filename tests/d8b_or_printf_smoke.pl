# D8b: EXPR or printf(...) parses as short-circuit statement
$x = 0;
$x or printf("zero\n");
$x = 1;
$x or printf("should not print\n");
print "ok\n";
