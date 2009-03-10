#!/usr/bin/perl -w

use strict;
use config;
use adchub;

my $config = new config();
my $adchub = new adchub($config->getHubAddress(), $config->getLocalPort());

$adchub->connect();

my $state = "HSUP";
$adchub->send_hsup();

$state = "ISUP";

my $line;

while ($state ne "exit" && defined($line = $adchub->recv()))
{

	if (&adc::is_status_message($line))
	{
		my $status_text = &adc::get_status_message($line);
		print "* Status: " . $status_text . "\n";
		next;
	}
	
	if ($state eq "ISUP") {
		$state = "ISID";
		next;
	}
	
	if ($state eq "ISID") {
		$state = "wait";
		next;
	}
}

print "SUCCESS: Server closed the connection\n";

