use MIME::QuotedPrint;
print "eq=", (decode_qp(encode_qp("Hello=World")) eq "Hello=World" ? 1 : 0), "\n";
print "nl=", (decode_qp(encode_qp("a=b\n")) =~ /a=b/ ? 1 : 0), "\n";
my $long = "x" x 90;
my $e = encode_qp($long);
print "wrap=", ($e =~ /=\n/ ? 1 : 0), "\n";
print "round=", (decode_qp($e) eq $long ? 1 : 0), "\n";
print "done\n";
