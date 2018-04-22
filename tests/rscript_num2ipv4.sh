#!/bin/bash
# add 2017-02-09 by Jan Gerhards, released under ASL 2.0
. $srcdir/diag.sh init
. $srcdir/diag.sh generate-conf
. $srcdir/diag.sh add-conf '
module(load="../plugins/imtcp/.libs/imtcp")
module(load="../plugins/fmhttp/.libs/fmhttp")
input(type="imtcp" port="13514")

set $!ip!v0 = num2ipv4("");
set $!ip!v1 = num2ipv4("0");
set $!ip!v2 = num2ipv4("1");
set $!ip!v3 = num2ipv4("256");
set $!ip!v4 = num2ipv4("65536");
set $!ip!v5 = num2ipv4("16777216");
set $!ip!v6 = num2ipv4("135");
set $!ip!v7 = num2ipv4("16843009");
set $!ip!v8 = num2ipv4("3777036554");
set $!ip!v9 = num2ipv4("2885681153");
set $!ip!v10 = num2ipv4("4294967295");

set $!ip!e1 = num2ipv4("a");
set $!ip!e2 = num2ipv4("-123");
set $!ip!e3 = num2ipv4("1725464567890");
set $!ip!e4 = num2ipv4("4294967296");
set $!ip!e5 = num2ipv4("2839.");


template(name="outfmt" type="string" string="%!ip%\n")
local4.* action(type="omfile" file="rsyslog.out.log" template="outfmt")
'
. $srcdir/diag.sh startup
. $srcdir/diag.sh tcpflood -m1 -y
. $srcdir/diag.sh shutdown-when-empty
. $srcdir/diag.sh wait-shutdown
echo '{ "v0": "0.0.0.0", "v1": "0.0.0.0", "v2": "0.0.0.1", "v3": "0.0.1.0", "v4": "0.1.0.0", "v5": "1.0.0.0", "v6": "0.0.0.135", "v7": "1.1.1.1", "v8": "225.33.1.10", "v9": "172.0.0.1", "v10": "255.255.255.255", "e1": "-1", "e2": "-1", "e3": "-1", "e4": "-1", "e5": "-1" }' | cmp - rsyslog.out.log
if [ ! $? -eq 0 ]; then
  echo "invalid function output detected, rsyslog.out.log is:"
  cat rsyslog.out.log
  . $srcdir/diag.sh error-exit 1
fi;
. $srcdir/diag.sh exit
