sub usage {
    my $fh = shift;
    print $fh "usage text\n";
}
usage(*STDOUT{IO});
