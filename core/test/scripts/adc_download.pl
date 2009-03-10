#!/usr/bin/perl -w

use strict;
use config;
use strict;
use config;
use adc;
use adcp2p;

my $config = new config();

my $request_type = "file"; # "file", "tthl" or "list".
my $request_file = "TTH/YZHX3TOMUGBJ7TXDHR4IJMXIZOWTK624DO3AYWQ";

my $host = $config->getLocalAddress();
my $port = 6534; # $config->getLocalPort();

my $adc = new adcp2p($config->getLocalAddress(), $config->getLocalPort());
$adc->set_cid("RX2KLRB4522XGLBNJHRPUFTIQOJTE6EWWL3EBCQ");
# $adc->set_token("autotest");
$adc->connect();

$adc->send_sup();

my $data_count = 0;
my $state = "CSUP";
my $line;
while ($state ne "exit" && defined($line = $adc->recv())) {
	if (!&adc::is_valid_adc_command($line)) 
	{
		die "Invalid command: '$line'\n";
	}
	
	print "recv: $line \n";
	
	if ($line =~ /^CSUP.*/ && $state eq "CSUP")
	{
		# print "CSUP as expected\n";
		$state = "CINF";
		next;
	}
	
	if ($line =~ /^CINF.*/ && $state eq "CINF")
	{
		# print "CINF as expected\n";
		
		$adc->send_inf();
		
		# $adc->send_gfi($request_type, $request_file);
		# $state = "CRES";
		
		$adc->send_get($request_type, $request_file, 0, -1);
		$state = "CSND";
		next;
	}
	
	if ($line =~ /^CSND (list|file|tthl) (TTH\/.*|\/) (\d+) (-?\d+)( (ZL1))?/ && $state eq "CSND")
	{
		my $data_count = $4;
		print "CSND as expected ($1, $2, $data_count)\n";
		$state = "data";
		my $buffer = 0;
		
		my $OUT;
		open(OUT, ">output.$1") || die "Unable to create file: $!\n";
		
		while ($data_count > 0) {
			my $chunk_len = $data_count;
			if ($chunk_len > 32767) { my $chunk_len = 32767; }
			my $cnt = read(SERVER, $buffer, $chunk_len);
			die "unable to read more" unless $cnt;
			print OUT $buffer;
			$data_count -= $cnt;
			print "Read $cnt bytes, remaining $data_count\n";
		}
		close(OUT);
		print "Successfully read all data\n";
		$state = "exit";
		next;
	}
	print "Unexpected server response\n";
	$state = "exit";
}

if ($state ne "exit") {
	print "Unexpected end of connection.\n";
}

close(SERVER) || die "close: $!";
exit;
