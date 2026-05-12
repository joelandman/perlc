#!/usr/bin/env perl


use strict;
use Spreadsheet::XLSX;

my $fn = 'ExpanDrive/OneDrive_HPE/Shasta_installations.xlsx';
my $parser   = Spreadsheet::ParseExcel->new();
my $workbook = $parser->parse($fn);

if ( !defined $workbook ) {
    die $parser->error(), ".\n";
}


my $sheet = $workbook->worksheet("Install Schedule");

sleep 10;