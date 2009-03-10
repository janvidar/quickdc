#!/usr/bin/perl -w

package adc;

use strict;
# use MIME::Base32 qw( RFC );
1;

sub is_valid_base32
{
	my ($str) = @_;
	if ($str =~ /^([A-Z]|[2-7])+$/) {
		return 1;
	}
	return 0;
}

sub is_valid_adc_command
{
	my ($str) = @_;

	if ($str =~ /^B... [A-Z]|[2-7]{4,4}( .*)?\n$/) {
		return 1;
	}
	
	if ($str =~ /^(C|I|H)...( .*)?\n$/) {
		return 1;
	}

	if ($str =~ /^(D|E|)... ([A-Z]|[2-7]{4,4}) ([A-Z]|[2-7]{4,4})( .*)?\n$/) {
		return 1;
	}
	
	if ($str =~ /^U... ([A-Z]|[2-7]{39,39})( .*)?\n$/) {
		return 1;
	}
	
	if ($str =~ /^F... [A-Z]|[2-7]{4,4} (\+|-...(,\+|-...)*)( .*)?\n$/) {
		return 1;
	}
	
	return 0;
}

sub escape
{
	my($str) = @_;
	$str =~ s/\\/\\\\/g;
	$str =~ s/\ /\\s/g;
	$str =~ s/\n/\\n/g;
	return $str;
}

sub unescape
{
	my($str) = @_;
	$str =~ s/\\s/ /g;
	$str =~ s/\\n/\n/g;
	$str =~ s/\\\\/\\/g;
	return $str;
}

sub is_status_message
{
	my ($line) = @_;
	if ($line =~ /^(B|C|D|E|F|H|I|U)STA ([0-2][0-5][0-9])(.*)?/) {
		return 1;
	}
	return 0;
}

sub get_status_message_text
{
	my ($line) = @_;
	if ($line =~ /^(B|C|D|E|F|H|I|U)STA ([0-2][0-5][0-9]) (.*)/) {
		return unescape($3);
	}
	return "";
}

sub get_status_message_code
{
	my ($line) = @_;
	if ($line =~ /^(B|C|D|E|F|H|I|U)STA ([0-2][0-5][0-9])/) {
		return $2;
	}
	return "";
}

sub get_status_message_severity
{
	my ($line) = @_;
	if ($line =~ /^(B|C|D|E|F|H|I|U)STA ([0-2])([0-5][0-9])/) {
		return $2;
	}
	return -1;
}


sub get_status_message
{
	my($line) = @_;
	my $message = "";
	my $prefix  = "";
	my $use_srv_msg = 1;
	my $code = get_status_message_code($line);
	my $text = get_status_message_text($line);
	my $severity = get_status_message_severity($line);
	
	$prefix .= "UNKNOWN ERR " if ($severity == -1);
	$prefix .= "FATAL ERROR " if ($severity == 2);
	$prefix .= "ERROR       " if ($severity == 1);
	$prefix .= "INFORMATION " if ($severity == 0);
	
	while ($code > 100)
	{
		$code -= 100;
	}
	
	if ($code ==  0) { $message = "generic error"; $use_srv_msg = 1; }
	if ($code == 10) { $message = "generic hub error"; $use_srv_msg = 1; }
	if ($code == 11) { $message = "hub is full"; }
	if ($code == 12) { $message = "hub is disabled"; }
	if ($code == 20) { $message = "generic login/access error"; $use_srv_msg = 1; }
	if ($code == 21) { $message = "nick invalid"; }
	if ($code == 22) { $message = "nick taken"; }
	if ($code == 23) { $message = "invalid password"; }
	if ($code == 24) { $message = "cid taken"; }
	if ($code == 25) { $message = "access denied"; }
	if ($code == 26) { $message = "access denied"; }
	if ($code == 27) { $message = "invalid pid supplied";  }
	if ($code == 30) { $message = "generic kick/ban/disconnect error"; $use_srv_msg = 1; }
	if ($code == 31) { $message = "permanently banned"; }
	if ($code == 32) { $message = "temporarily banned"; }
	if ($code == 40) { $message = "generic protocol error"; $use_srv_msg = 1; }
	if ($code == 41) { $message = "protocol unsupported"; }
	if ($code == 42) { $message = "direct connection failed"; }
	if ($code == 43) { $message = "required missing INF field"; }
	if ($code == 44) { $message = "invalid state flag"; }
	if ($code == 45) { $message = "required missing feature"; }
	if ($code == 46) { $message = "invalid IP in INF"; }
	if ($code == 50) { $message = "generic transfer error"; }
	if ($code == 51) { $message = "file not available"; }
	if ($code == 52) { $message = "file part not available"; }
	if ($code == 53) { $message = "no more slots available"; }
	
	if ($message eq "") { $message = "Unknown status code message"; $use_srv_msg = 1; }
	
	if ($use_srv_msg) {
		$message .= " (server: '" . $text . "')";
	}
	
	return $prefix . "(" . $code . "): " . $message;
	
}
