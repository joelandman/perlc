our $x;
my $name = "x";
${$name} = 5;
print "\$x=$x\n";
${$name} = 8;
print "r=$x\n";
${$name} .= "9";
print "app=$x\n";
my $u = ${$name};
print "u=$u\n";
my $copy = $x;
print "copy=$copy\n";
# ${$ref} block-deref of a REF (same operator as $$ref)
my $v = 5;
my $vr = \$v;
print "refderef=", ${$vr}, "\n";
${$vr} = 11;
print "thru=$v\n";
# \$elem->[idx] / \$h{key}: ref of the element, writable through
my @arr = (1, 2, 3);
my $e = \$arr[1];
print "eref=", ref($e), " val=", $$e, "\n";
$$e = 22;
print "arr=$arr[1]\n";
my %h = (k => 7);
my $hv = \$h{k};
print "href=", ref($hv), " val=", $$hv, "\n";
$$hv = 77;
print "hashv=$h{k}\n";
# computed-name read of a package var from another package
$Foo::q = 12;
my $qn = "Foo::q";
print "fq=", ${$qn}, "\n";
# ${$ref} through a sub arg and hash value
sub rd { return ${$_[0]}; }
my $m = 9; my $mr = \$m;
print "sub=", rd($mr), "\n";
${$mr} += 1;
print "inc=$m\n";
# nested block deref — ref-to-ref: ${$wr2} is the intermediate REF
# (stringified as SCALAR(0x...) — address nondeterministic, so only check
# the ref-ness via ref())
my $w2 = 4; my $wr = \$w2;
my $wr2 = \$wr;
print "nestedref=", ref(${$wr2}), "\n";
print "nestedval=", ${${$wr2}}, "\n";
