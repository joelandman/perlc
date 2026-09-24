use Digest;
print "md5=", Digest->new("MD5")->add("abc")->hexdigest, "\n";
print "sha=", Digest->new("SHA-256")->add("abc")->hexdigest, "\n";
print "done\n";
