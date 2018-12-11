#!/bin/bash
# added 2018-11-07 by Rainer Gerhards
# check error message on cert as key file
# This file is part of the rsyslog project, released under ASL 2.0
. ${srcdir:=.}/diag.sh init
generate_conf
add_conf '
global(	defaultNetstreamDriverCAFile="'$srcdir/tls-certs/ca.pem'"
	defaultNetstreamDriverCertFile="'$srcdir/tls-certs/cert.pem'"
	defaultNetstreamDriverKeyFile="'$srcdir/tls-certs/cert.pem'"
)

module(load="../plugins/imtcp/.libs/imtcp" StreamDriver.Name="ossl" StreamDriver.Mode="1")
input(type="imtcp" port="'$TCPFLOOD_PORT'")

action(type="omfile" file="'$RSYSLOG_OUT_LOG'")
'
# note: we do not need to generate any messages, config error occurs on startup
startup
shutdown_when_empty
wait_shutdown
content_check "Error: Key could not be accessed"
content_check "OpenSSL Error Stack:"
exit_test
