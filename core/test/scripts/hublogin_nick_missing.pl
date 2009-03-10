#!/usr/bin/perl -w

use strict;
use config;
use adchub;

my $config = new config();
my $adchub = new adchub($config->getHubAddress(), $config->getLocalPort());

$adchub->set_nick("");
$adchub->set_pid($config->getPID());
$adchub->set_cid($config->getCID());

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
		
		$adchub->send("BINF " . $adchub->{sid} . " ID" . $adchub->{cid} . " PD" . $adchub->{pid} . " VE" . &adc::escape($adchub->{ua}) . " SS0 SF0\n");

		$state = "IINF";
		next;
	}
	
	if ($state eq "IINF") {
		$adchub->parse_sta($line);
	
		$adchub->parse_inf($line);
		$state = "idle";
		next;
	}
	
}



