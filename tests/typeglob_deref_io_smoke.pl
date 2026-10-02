sub usage {
    my $fh = shift;
    print $fh "hello\n";
}
usage(*{\*STDOUT}{IO});
