my $sendmail = "";
for (qw(a b c)) {
    $sendmail = $_, last if $_ eq 'b';
}
print "sendmail=$sendmail\n";
