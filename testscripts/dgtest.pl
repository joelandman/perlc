#!/usr/bin/env perl

use strict;
use Spreadsheet::ParseExcel::Stream::XLSX;
use Mojo::CSV ;
use Data::Dumper;


my $xls = Spreadsheet::ParseExcel::Stream::XLSX->new('ExpanDrive/OneDrive_HPE/Shasta_installations.xlsx');
my @sheets = @{$xls->{SHEETS}};
my $nsheets = $#sheets+1;
my $ndx = -1;
my $sheet="Install Schedule";
my ($df,$df2,@rawlabels,$max_col,$min_col,@labels,%col,@cols,@out,$row);
my $count = 0;

foreach my $i (0..$nsheets-1) {
	my $s = $sheets[$i];
   if ($s->{Name} =~ /$sheet/) {
	 $ndx = $i;
	 $df=$sheets[$ndx];
	 last;
   }
}
die "FATAL ERROR: sheet $sheet not found\n" if ($ndx == -1);

# row 12 (1 indexed) are the labels
$min_col = $df->{MinCol};
$max_col = $df->{MaxCol};

map { push @rawlabels,$df->get_cell(12-1,$_) } ($min_col .. $max_col);
map {
	  my $s = $_->{Val};
	  $s =~ s/\n.*$//g;  # remove everything after newlines
	  $s =~ s/\*\*.*$//g;  # filter out the **... type comments in the labels
	  $s =~ s/\ /_/g;  # Make spaces underscores
	  $s =~ s/\&\#10\;/_/g;
	  push @labels,$s;
	  $col{$s}=$count;
	  $count++;
    } @rawlabels;

#printf "Labels=%s\n",join(", ",@labels);
# these are the columns we want to extract
@cols= qw(
			Priority_Unfiltered
			Installation_PM_Contact
			Customer
			Customer_System
			Status_
			SW_Refresh_for_Acceptance
			Ship_Date
			NOA_Planned_Qtr
			);

# the actual data starts at row 14.  For some reason, deleted data is
# simple using strike through versus actually deleting, so we need a way to
# detect that.  Also, the typing/formatting of the columns are somewhat
# inconsistent, so we need to parse the values.

# header
push @out,[@cols];
$count = 1;
my @row;
foreach my $i (14-1 .. $max_col) {
	my @row;
	map {
			$ndx = $col{$_};
			my $_ov = $df->get_cell($i,$ndx);
			my $_v  = $_ov->{Val};
			next if ($_ov->{Strikeout});
			push @row,$_v;
	} @cols;
	push @out,[@row];
}

map { printf "%s\n",Mojo::CSV->new->text($_) } @out;

sub get_tab_index {
	my $tab_name = shift;
	
}
