use MIME::QuotedPrint;
print "[", encode_qp("a=b\n"), "]\n";
print "[", decode_qp("a=3Db\n"), "]\n";
print "done\n";
