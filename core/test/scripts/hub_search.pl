#!/usr/bin/perl -w

use strict;
use config;
use adchub;

my $config = new config();
my $adchub = new adchub($config->getHubAddress(), $config->getLocalPort());

$adchub->set_nick($config->getNick());
$adchub->set_pid($config->getPID());
$adchub->set_cid($config->getCID());

$adchub->connect();

my $state = "HSUP";
$adchub->send_hsup();

$state = "ISUP";

my $line;

# fixme - read arguments if set.
my $search_term = "mp3";

while ($state ne "exit" && defined($line = $adchub->recv()))
{
	if (&adc::is_status_message($line))
	{
		my $status_text = &adc::get_status_message($line);
		print $status_text . "\n";
		next;
	}
	
	if ($state eq "ISUP") {
		$state = "ISID";
		next;
	}
	
	if ($state eq "ISID") {
		$adchub->parse_sid($line);
		$state = "BINF";
		$adchub->send_binf();
		$state = "IINF";
		next;
	}
	
	if ($state eq "IINF") {
		$adchub->parse_sta($line);
		$adchub->parse_inf($line);
		$state = "idle";
		
		$adchub->send("BSCH " . $adchub->{sid} . " AN" . $search_term . " TOautotest\n");
		next;
	}
	
	if ($state eq "idle") {
		printf "$line";
	}
	
}


