#!/usr/bin/perl -w

use strict;

my $memory_peak_level = 0;
my $memory_level  = 0;
my $memory_frees  = 0;
my $memory_allocs = 0;
my $memory_bad_frees = 0;

my $memory_block_peak = 0;
my $memory_block_peak_location;
my $memory_block_peak_location2;

my %allocations;



my $LOGFILE = $ENV{"HOME"} . "/.quickdc/debug/memory.log";
my $FILE;
open(FILE, $LOGFILE) or die("Could not open memory log file. $!: " . $LOGFILE);
my $line;

foreach $line (<FILE>) {
    $line =~ m/^(DELETE|MALLOC|CALLOC): addr=(.+), size=(\d+), stack=(.+), stack=(.+)$/;

    my $type     = $1;
    my $addr     = $2;
    my $size     = int($3);
    my $stack    = $4;
	my $stack2   = $5;

	if ($type eq "MALLOC" || $type eq "CALLOC") {
		$memory_allocs++;
		$memory_level += $size;
		$allocations{$addr} = $size;

		if ($memory_level > $memory_peak_level) {
			$memory_peak_level = $memory_level;
		}
	
		if ($size > $memory_block_peak) {
			$memory_block_peak = $size;
			$memory_block_peak_location = $stack;
			$memory_block_peak_location2 = $stack2;
		}
		
	} else {
		$memory_frees++;
		if ($allocations{$addr}) {
			$size = $allocations{$addr};
			$memory_level -= $size;
			delete $allocations{$addr};
		} else {
			$memory_bad_frees++;
			print STDERR "Invalid free detected. Addr=" . $addr . ", Stack=" . $stack . ", Stack=" . $stack2 . "\n";
		}
	}


    # do line-by-line processing.
}
close(FILE);

print "Memory statistics\n";
print "---------------------------------------------------\n";
print "Total allocations:    " . $memory_allocs . "\n";
print "Total frees:          " . $memory_frees  . "\n";
print "Bad frees:            " . $memory_bad_frees . "\n";
print "Unfreed at exit:      " . ($memory_allocs - $memory_frees) . "\n";
print "Memory used at exit:  " . $memory_level  . "\n";
print "Memory peak usage:    " . $memory_peak_level  . "\n";
print "Biggest block:        " . $memory_block_peak . " (" . $memory_block_peak_location . "->" . $memory_block_peak_location2 . ")\n";



