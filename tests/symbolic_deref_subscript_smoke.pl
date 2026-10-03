my $mail = { From => 'a@b.com' };
my $x = ${$mail}{'From'};
print "$x\n";
