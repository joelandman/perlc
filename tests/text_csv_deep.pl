use Text::CSV_PP;
use Text::CSV;

# basic parse / fields, including scalar-context count (real Text::CSV_PP
# overrides the generic "list assigned to scalar returns last element"
# convention here — verified against the real, installed module)
my $csv = Text::CSV_PP->new;
$csv->parse("a,b,c");
print "fields=[", join("|", $csv->fields), "]\n";
print "field_count=", scalar($csv->fields), "\n";

# quoted fields, embedded comma, embedded doubled quote, trailing empty
for my $s ("", ",", "a", "a,", ",a", "a,b,c,", 'a,"b,c",d', 'a,"b""q",') {
    $csv->parse($s);
    my @f = $csv->fields;
    print "[$s] => (", join("|", map { defined $_ ? $_ : "UNDEF" } @f), ")\n";
}

# combine: quoting rules (space/comma/quote-char trigger quoting; plain
# fields and undef don't)
$csv->combine("plain", "has space", "has,comma", 'has"quote', undef);
print "combine1=[", $csv->string, "]\n";

my $csv_aq = Text::CSV_PP->new({always_quote => 1});
$csv_aq->combine("a", "b");
print "always_quote=[", $csv_aq->string, "]\n";

my $csv_nq = Text::CSV_PP->new({quote_space => 0});
$csv_nq->combine("has space", "x");
print "no_quote_space=[", $csv_nq->string, "]\n";

# custom sep_char / eol
my $csv_semi = Text::CSV_PP->new({sep_char => ";", eol => "\n"});
$csv_semi->combine("a", "b");
print "semi_eol=[", $csv_semi->string, "]";

# allow_whitespace
my $csv_ws = Text::CSV_PP->new({allow_whitespace => 1});
$csv_ws->parse(" a , b ");
print "allow_ws=[", join("|", $csv_ws->fields), "]\n";

# blank_is_undef
my $csv_bu = Text::CSV_PP->new({blank_is_undef => 1});
$csv_bu->parse("a,,c");
my @fb = $csv_bu->fields;
print "blank_is_undef=", (defined $fb[1] ? "def" : "undef"), "\n";

# getline / getline_all over an in-memory filehandle, multiple records
my $data = "a,b\nc,d\ne,f\n";
open(my $fh, "<", \$data) or die;
my $csv2 = Text::CSV_PP->new;
my $r1 = $csv2->getline($fh);
print "getline1=[", join("|", @$r1), "]\n";
my $r2 = $csv2->getline($fh);
print "getline2=[", join("|", @$r2), "]\n";

open(my $fh2, "<", \$data) or die;
my $all = $csv2->getline_all($fh2);
print "getline_all_count=", scalar(@$all), " row0=[", join("|", @{$all->[0]}), "]\n";

# column_names + getline_hr
my $hdr_data = "name,age\nAlice,30\nBob,25\n";
open(my $fh3, "<", \$hdr_data) or die;
my $csv3 = Text::CSV_PP->new;
$csv3->column_names($csv3->getline($fh3));
my $hr1 = $csv3->getline_hr($fh3);
print "hr1_name=$hr1->{name} hr1_age=$hr1->{age}\n";
my $hr2 = $csv3->getline_hr($fh3);
print "hr2_name=$hr2->{name} hr2_age=$hr2->{age}\n";

# print / say to an in-memory filehandle
my $out = "";
open(my $ofh, ">", \$out) or die;
$csv->print($ofh, ["x", "y,z"]);
print "print_out=[$out]\n";
my $out2 = "";
open(my $ofh2, ">", \$out2) or die;
$csv->say($ofh2, ["p", "q"]);
print "say_out=[$out2]\n";

# status / error_diag: never-used, success, and failure states
my $csv_fresh = Text::CSV_PP->new;
print "fresh_status=[", $csv_fresh->status, "] fresh_diag=", 0 + $csv_fresh->error_diag, "\n";
$csv_fresh->parse("a,b");
print "ok_status=[", $csv_fresh->status, "] ok_diag=", 0 + $csv_fresh->error_diag, "\n";
my $bad_ok = $csv_fresh->parse('a,"unterminated');
print "bad_ok=$bad_ok bad_status=[", $csv_fresh->status, "] bad_diag=", 0 + $csv_fresh->error_diag, "\n";
my $bad2 = $csv_fresh->parse('a,b"c,d');
print "bad2_diag=", 0 + $csv_fresh->error_diag, "\n";

# embedded newline: combine requires binary=>1
my $csv_bin = Text::CSV_PP->new;
my $nl_ok = $csv_bin->combine("a\nb", "c");
print "nl_ok=$nl_ok nl_diag=", 0 + $csv_bin->error_diag, "\n";
my $csv_bin2 = Text::CSV_PP->new({binary => 1});
my $nl_ok2 = $csv_bin2->combine("a\nb", "c");
print "nl_ok2=$nl_ok2 nl_string=[", $csv_bin2->string, "]\n";

# escape_char distinct from quote_char
my $csv_esc = Text::CSV_PP->new({escape_char => "\\"});
$csv_esc->combine('has"quote', "plain");
my $esc_str = $csv_esc->string;
print "esc_combine=[$esc_str]\n";
$csv_esc->parse($esc_str);
print "esc_parse=[", join("|", $csv_esc->fields), "]\n";

# ref() reflects the class ->new was called on
print "ref_pp=", ref(Text::CSV_PP->new), " ref_csv=", ref(Text::CSV->new), "\n";

print "done\n";
