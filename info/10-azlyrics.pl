#!/usr/bin/env perl
# 10-azlyrics.pl: nncmpp info plugin to fetch song lyrics on AZLyrics
#
# Copyright (c) 2022, Přemysl Eric Janouch <p@janouch.name>
# SPDX-License-Identifier: 0BSD
#
# Inspired by a similar ncmpc plugin.

use warnings;
use strict;
use utf8;
use open ':std', ':utf8';
unless (@ARGV) {
	print "Look up on AZLyrics\n";
	exit;
}

use Encode;
my ($title, $artist, $album) = map {decode_utf8($_)} @ARGV;

# TODO: An upgrade would be transliteration with, e.g., Text::Unidecode.
use Unicode::Normalize;
$artist = lc(NFD($artist)) =~ s/^the\s+//ir =~ s/[^a-z0-9]//gr;
$title  = lc(NFD($title))  =~ s/\(.*?\)//gr =~ s/[^a-z0-9]//gr;

# TODO: Consider caching the results in a location like
# $XDG_CACHE_HOME/nncmpp/info/azlyrics/$artist-$title
my $found = 0;
if ($title ne '') {
	open(my $curl, '-|', 'curl', '-sA', 'nncmpp/2.0',
		"https://www.azlyrics.com/lyrics/$artist/$title.html") or die $!;
	while (<$curl>) {
		next unless /^<div>/ .. /^<\/div>/; s/<!--.*?-->//g; s/\s+$//gs;

		$found = 1;
		s/<\/?b>/\x01/g; s/<\/?i>/\x02/g; s/<br>/\n/; s/<.+?>//g;
		s/&lt;/</g; s/&gt;/>/g; s/&quot;/"/g; s/&apos;/'/g; s/&amp;/&/g;
		print;
	}
	close($curl) or die $?;
}

print "No lyrics have been found.\n" unless $found;
