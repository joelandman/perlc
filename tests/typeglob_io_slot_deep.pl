# Deep test for D177: *NAME{IO} (the IO slot of a typeglob) was a
# hard "expected ) but got '{'" parse error -- most commonly seen
# passing a bareword filehandle like STDERR/STDOUT to a sub expecting
# a real filehandle value: `usage(*STDERR{IO});`. Found verbatim, the
# identical idiom, in THREE separate real scripts across two surveys:
# /usr/bin/linux-version, /usr/lib/emacsen-common/emacs-package-remove,
# /usr/bin/linux-run-hooks.
sub write_to {
    my $fh = shift;
    my $msg = shift;
    print $fh $msg;
}

write_to(*STDOUT{IO}, "to stdout via IO slot\n");

# *NAME{IO} used directly as an argument in a function call with
# other arguments around it (the exact real-world shape)
sub usage {
    my ($fh, $prefix) = @_;
    print $fh "${prefix}usage text\n";
}
usage(*STDOUT{IO}, "[usage] ");

# *NAME{IO} assigned to a variable first, then used
my $out = *STDOUT{IO};
print $out "via variable\n";

print "done\n";
