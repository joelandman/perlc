use Digest;
print Digest->new("MD5")->add("abc")->hexdigest, "\n";
print "done\n";
