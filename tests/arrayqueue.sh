#!/bin/bash
# Test for fixedArray queue mode
# added 2009-05-20 by rgerhards
# This file is part of the rsyslog project, released  under GPLv3
echo ===============================================================================
echo \[arrayqueue.sh\]: testing queue fixedArray queue mode
. $srcdir/diag.sh init
. $srcdir/diag.sh startup arrayqueue.conf

# 40000 messages should be enough
. $srcdir/diag.sh injectmsg  0 40000

# terminate *now* (don't wait for queue to drain!)
kill `cat rsyslog.pid`

# now wait until rsyslog.pid is gone (and the process finished)
. $srcdir/diag.sh wait-shutdown 
. $srcdir/diag.sh seq-check 0 39999
. $srcdir/diag.sh exit
