/* omgssapi.c
 * This is the implementation of the build-in forwarding output module.
 *
 * NOTE: read comments in module-template.h to understand how this file
 *       works!
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#ifdef USE_GSSAPI
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fnmatch.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#ifdef USE_NETZIP
#include <zlib.h>
#endif
#include <pthread.h>
#include <gssapi/gssapi.h>
#include "syslogd.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "net.h"
#include "omfwd.h"
#include "template.h"
#include "msg.h"
#include "tcpsyslog.h"
#include "cfsysline.h"
#include "module-template.h"
#include "gss-misc.h"

#define INET_SUSPEND_TIME 60		/* equal to 1 minute */
					/* rgerhards, 2005-07-26: This was 3 minutes. As the
					 * same timer is used for tcp based syslog, we have
					 * reduced it. However, it might actually be worth
					 * thinking about a buffered tcp sender, which would be 
					 * a much better alternative. When that happens, this
					 * time here can be re-adjusted to 3 minutes (or,
					 * even better, made configurable).
					 */
#define INET_RETRY_MAX 30		/* maximum of retries for gethostbyname() */
	/* was 10, changed to 30 because we reduced INET_SUSPEND_TIME by one third. So
	 * this "fixes" some of implications of it (see comment on INET_SUSPEND_TIME).
	 * rgerhards, 2005-07-26
	 */

/* internal structures
 */
DEF_OMOD_STATIC_DATA

typedef struct _instanceData {
	char	f_hname[MAXHOSTNAMELEN+1];
	short	sock;			/* file descriptor */
	enum { /* TODO: we shoud revisit these definitions */
		eDestFORW,
		eDestFORW_SUSP,
		eDestFORW_UNKN
	} eDestState;
	int iRtryCnt;
	struct addrinfo *f_addr;
	int compressionLevel; /* 0 - no compression, else level for zlib */
	char *port;
	char *savedMsg;
	int savedMsgLen; /* length of savedMsg in octets */
	TCPFRAMINGMODE tcp_framing;
	enum TCPSendStatus {
		TCP_SEND_NOTCONNECTED = 0,
		TCP_SEND_CONNECTING = 1,
		TCP_SEND_READY = 2
	} status;
	time_t	ttSuspend;	/* time selector was suspended */
	gss_ctx_id_t gss_context;
	OM_uint32 gss_flags;
#	ifdef USE_PTHREADS
	pthread_mutex_t mtxTCPSend;
#	endif
} instanceData;

static char *gss_base_service_name = NULL;
static enum gss_mode_t {
	GSSMODE_MIC,
	GSSMODE_ENC
} gss_mode = GSSMODE_ENC;

/* get the syslog forward port from selector_t. The passed in
 * struct must be one that is setup for forwarding.
 * rgerhards, 2007-06-28
 * We may change the implementation to try to lookup the port
 * if it is unspecified. So far, we use the IANA default auf 514.
 */
char *getFwdSyslogPt(instanceData *pData)
{
	assert(pData != NULL);
	if(pData->port == NULL)
		return("514");
	else
		return(pData->port);
}

/* get send status
 * rgerhards, 2005-10-24
 */
static void TCPSendSetStatus(instanceData *pData, enum TCPSendStatus iNewState)
{
	assert(pData != NULL);
	assert(   (iNewState == TCP_SEND_NOTCONNECTED)
	       || (iNewState == TCP_SEND_CONNECTING)
	       || (iNewState == TCP_SEND_READY));

	/* there can potentially be a race condition, so guard by mutex */
#	ifdef	USE_PTHREADS
		pthread_mutex_lock(&pData->mtxTCPSend);
#	endif
	pData->status = iNewState;
#	ifdef	USE_PTHREADS
		pthread_mutex_unlock(&pData->mtxTCPSend);
#	endif
}


/* get send status
 * rgerhards, 2005-10-24
 */
static enum TCPSendStatus TCPSendGetStatus(instanceData *pData)
{
	enum TCPSendStatus eState;
	assert(pData != NULL);

	/* there can potentially be a race condition, so guard by mutex */
#	ifdef	USE_PTHREADS
		pthread_mutex_lock(&pData->mtxTCPSend);
#	endif
	eState = pData->status;
#	ifdef	USE_PTHREADS
		pthread_mutex_unlock(&pData->mtxTCPSend);
#	endif

	return eState;
}


BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINfreeInstance
OM_uint32 maj_stat, min_stat;
CODESTARTfreeInstance
	switch (pData->eDestState) {
		case eDestFORW:
		case eDestFORW_SUSP:
			freeaddrinfo(pData->f_addr);
			/* fall through */
		case eDestFORW_UNKN:
			if(pData->port != NULL)
				free(pData->port);
			break;
	}

	if (pData->gss_context != GSS_C_NO_CONTEXT) {
		maj_stat = gss_delete_sec_context(&min_stat, &pData->gss_context, GSS_C_NO_BUFFER);
		if (maj_stat != GSS_S_COMPLETE)
			display_status("deleting context", maj_stat, min_stat);
	}
	/* this is meant to be done when module is unloaded,
	   but since this module is static...
	*/
	if (gss_base_service_name != NULL) {
		free(gss_base_service_name);
		gss_base_service_name = NULL;
	}

#	ifdef USE_PTHREADS
	/* delete any mutex objects, if present */
	pthread_mutex_destroy(&pData->mtxTCPSend);
#	endif
	/* final cleanup */
	if(pData->sock >= 0)
		close(pData->sock);
ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	printf("%s", pData->f_hname);
ENDdbgPrintInstInfo


/* CODE FOR SENDING TCP MESSAGES */

/* This function is called immediately before a send retry is attempted.
 * It shall clean up whatever makes sense.
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendGSSPrepRetry(void __attribute__((unused)) *pData)
{
	/* in case of TCP/GSS, there is nothing to do */
	return RS_RET_OK;
}


static rsRetVal TCPSendGSSInit(void *pvData)
{
	DEFiRet;
	int s = -1;
	char *base;
	OM_uint32 maj_stat, min_stat, init_sec_min_stat, *sess_flags, ret_flags;
	gss_buffer_desc out_tok, in_tok;
	gss_buffer_t tok_ptr;
	gss_name_t target_name;
	gss_ctx_id_t *context;
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);

	/* if the socket is already initialized, we are done */
	if(pData->sock > 0)
		ABORT_FINALIZE(RS_RET_OK);

	base = (gss_base_service_name == NULL) ? "host" : gss_base_service_name;
	out_tok.length = strlen(pData->f_hname) + strlen(base) + 2;
	if ((out_tok.value = malloc(out_tok.length)) == NULL) {
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
	}
	strcpy(out_tok.value, base);
	strcat(out_tok.value, "@");
	strcat(out_tok.value, pData->f_hname);
	dbgprintf("GSS-API service name: %s\n", (char*) out_tok.value);

	tok_ptr = GSS_C_NO_BUFFER;
	context = &pData->gss_context;
	*context = GSS_C_NO_CONTEXT;

	maj_stat = gss_import_name(&min_stat, &out_tok, GSS_C_NT_HOSTBASED_SERVICE, &target_name);
	free(out_tok.value);
	out_tok.value = NULL;
	out_tok.length = 0;

	if (maj_stat != GSS_S_COMPLETE) {
		display_status("parsing name", maj_stat, min_stat);
		goto fail;
	}

	sess_flags = &pData->gss_flags;
	*sess_flags = GSS_C_MUTUAL_FLAG;
	if (gss_mode == GSSMODE_MIC) {
		*sess_flags |= GSS_C_INTEG_FLAG;
	}
	if (gss_mode == GSSMODE_ENC) {
		*sess_flags |= GSS_C_CONF_FLAG;
	}
	dbgprintf("GSS-API requested context flags:\n");
	display_ctx_flags(*sess_flags);

	do {
		maj_stat = gss_init_sec_context(&init_sec_min_stat, GSS_C_NO_CREDENTIAL, context,
						target_name, GSS_C_NO_OID, *sess_flags, 0, NULL,
						tok_ptr, NULL, &out_tok, &ret_flags, NULL);
		if (tok_ptr != GSS_C_NO_BUFFER)
			free(in_tok.value);

		if (maj_stat != GSS_S_COMPLETE
		    && maj_stat != GSS_S_CONTINUE_NEEDED) {
			display_status("initializing context", maj_stat, init_sec_min_stat);
			goto fail;
		}

		if (s == -1)
			if ((s = pData->sock = TCPSendCreateSocket(pData->f_addr)) == -1)
				goto fail;

		if (out_tok.length != 0) {
			dbgprintf("GSS-API Sending init_sec_context token (length: %ld)\n", (long) out_tok.length);
			if (send_token(s, &out_tok) < 0) {
				goto fail;
			}
		}
		gss_release_buffer(&min_stat, &out_tok);

		if (maj_stat == GSS_S_CONTINUE_NEEDED) {
			dbgprintf("GSS-API Continue needed...\n");
			if (recv_token(s, &in_tok) <= 0) {
				goto fail;
			}
			tok_ptr = &in_tok;
		}
	} while (maj_stat == GSS_S_CONTINUE_NEEDED);

	dbgprintf("GSS-API Provided context flags:\n");
	*sess_flags = ret_flags;
	display_ctx_flags(*sess_flags);

	dbgprintf("GSS-API Context initialized\n");
	gss_release_name(&min_stat, &target_name);

finalize_it:
	return iRet;

 fail:
	logerror("GSS-API Context initialization failed\n");
	gss_release_name(&min_stat, &target_name);
	gss_release_buffer(&min_stat, &out_tok);
	if (*context != GSS_C_NO_CONTEXT) {
		gss_delete_sec_context(&min_stat, context, GSS_C_NO_BUFFER);
		*context = GSS_C_NO_CONTEXT;
	}
	if (s != -1)
		close(s);
	pData->sock = -1;
	return RS_RET_GSS_SENDINIT_ERROR;
}


static rsRetVal TCPSendGSSSend(void *pvData, char *msg, size_t len)
{
	int s;
	gss_ctx_id_t *context;
	OM_uint32 maj_stat, min_stat;
	gss_buffer_desc in_buf, out_buf;
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);
	assert(msg != NULL);
	assert(len > 0);

	s = pData->sock;
	context = &pData->gss_context;
	in_buf.value = msg;
	in_buf.length = len;
	maj_stat = gss_wrap(&min_stat, *context, (gss_mode == GSSMODE_ENC) ? 1 : 0, GSS_C_QOP_DEFAULT,
			    &in_buf, NULL, &out_buf);
	if (maj_stat != GSS_S_COMPLETE) {
		display_status("wrapping message", maj_stat, min_stat);
		goto fail;
	}
	
	if (send_token(s, &out_buf) < 0) {
		goto fail;
	}
	gss_release_buffer(&min_stat, &out_buf);

	return RS_RET_OK;

 fail:
	close(s);
	pData->sock = -1;
	gss_delete_sec_context(&min_stat, context, GSS_C_NO_BUFFER);
	*context = GSS_C_NO_CONTEXT;
	gss_release_buffer(&min_stat, &out_buf);
	dbgprintf("message not (GSS/tcp)send");
	return RS_RET_GSS_SEND_ERROR;
}


/* try to resume connection if it is not ready
 * rgerhards, 2007-08-02
 */
static rsRetVal doTryResume(instanceData *pData)
{
	DEFiRet;
	struct addrinfo *res;
	struct addrinfo hints;
	unsigned e;

	switch (pData->eDestState) {
	case eDestFORW_SUSP:
		iRet = RS_RET_OK; /* the actual check happens during doAction() only */
		pData->eDestState = eDestFORW;
		break;
		
	case eDestFORW_UNKN:
		/* The remote address is not yet known and needs to be obtained */
		dbgprintf(" %s\n", pData->f_hname);
		memset(&hints, 0, sizeof(hints));
		/* port must be numeric, because config file syntax requests this */
		/* TODO: this code is a duplicate from cfline() - we should later create
		 * a common function.
		 */
		hints.ai_flags = AI_NUMERICSERV;
		hints.ai_family = family;
		hints.ai_socktype = SOCK_STREAM;
		if((e = getaddrinfo(pData->f_hname,
				    getFwdSyslogPt(pData), &hints, &res)) == 0) {
			dbgprintf("%s found, resuming.\n", pData->f_hname);
			pData->f_addr = res;
			pData->iRtryCnt = 0;
			pData->eDestState = eDestFORW;
		} else {
			iRet = RS_RET_SUSPENDED;
		}
		break;
	case eDestFORW:
		/* rgerhards, 2007-09-11: this can not happen, but I've included it to
		 * a) make the compiler happy, b) detect any logic errors */
		assert(0);
		break;
	}

	return iRet;
}


BEGINtryResume
CODESTARTtryResume
	iRet = doTryResume(pData);
ENDtryResume

BEGINdoAction
	char *psz; /* temporary buffering */
	register unsigned l;
CODESTARTdoAction
	switch (pData->eDestState) {
	case eDestFORW_SUSP:
		dbgprintf("internal error in omgssapi.c, eDestFORW_SUSP in doAction()!\n");
		iRet = RS_RET_SUSPENDED;
		break;
		
	case eDestFORW_UNKN:
		dbgprintf("doAction eDestFORW_UNKN\n");
		iRet = doTryResume(pData);
		break;

	case eDestFORW:
		dbgprintf(" %s:%s/%s\n", pData->f_hname, getFwdSyslogPt(pData), "tcp-gssapi");
		pData->ttSuspend = time(NULL);
		psz = (char*) ppString[0];
		l = strlen((char*) psz);
		if (l > MAXLINE)
			l = MAXLINE;

#		ifdef	USE_NETZIP
		/* Check if we should compress and, if so, do it. We also
		 * check if the message is large enough to justify compression.
		 * The smaller the message, the less likely is a gain in compression.
		 * To save CPU cycles, we do not try to compress very small messages.
		 * What "very small" means needs to be configured. Currently, it is
		 * hard-coded but this may be changed to a config parameter.
		 * rgerhards, 2006-11-30
		 */
		if(pData->compressionLevel && (l > MIN_SIZE_FOR_COMPRESS)) {
			Bytef out[MAXLINE+MAXLINE/100+12] = "z";
			uLongf destLen = sizeof(out) / sizeof(Bytef);
			uLong srcLen = l;
			int ret;
			ret = compress2((Bytef*) out+1, &destLen, (Bytef*) psz,
					srcLen, pData->compressionLevel);
			dbgprintf("Compressing message, length was %d now %d, return state  %d.\n",
				l, (int) destLen, ret);
			if(ret != Z_OK) {
				/* if we fail, we complain, but only in debug mode
				 * Otherwise, we are silent. In any case, we ignore the
				 * failed compression and just sent the uncompressed
				 * data, which is still valid. So this is probably the
				 * best course of action.
				 * rgerhards, 2006-11-30
				 */
				dbgprintf("Compression failed, sending uncompressed message\n");
			} else if(destLen+1 < l) {
				/* only use compression if there is a gain in using it! */
				dbgprintf("there is gain in compression, so we do it\n");
				psz = (char*) out;
				l = destLen + 1; /* take care for the "z" at message start! */
			}
			++destLen;
		}
#		endif

		CHKiRet_Hdlr(TCPSend(pData, psz, l, pData->tcp_framing, TCPSendGSSInit, TCPSendGSSSend, TCPSendGSSPrepRetry)) {
			/* error! */
			dbgprintf("error forwarding via tcp, suspending\n");
			pData->eDestState = eDestFORW_SUSP;
			iRet = RS_RET_SUSPENDED;
		}
		break;
	}
ENDdoAction


BEGINparseSelectorAct
	uchar *q;
	int i;
        int error;
	int bErr;
        struct addrinfo hints, *res;
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	/* first check if this config line is actually for us
	 * The first test [*p == '>'] can be skipped if a module shall only
	 * support the newer slection syntax [:modname:]. This is in fact
	 * recommended for new modules. Please note that over time this part
	 * will be handled by rsyslogd itself, but for the time being it is
	 * a good compromise to do it at the module level.
	 * rgerhards, 2007-10-15
	 */

	if(!strncmp((char*) p, ":omgssapi:", sizeof(":omgssapi:") - 1)) {
		p += sizeof(":omgssapi:") - 1; /* eat indicator sequence (-1 because of '\0'!) */
	} else {
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
	}

	/* ok, if we reach this point, we have something for us */
	if((iRet = createInstance(&pData)) != RS_RET_OK)
		goto finalize_it;

#	ifdef USE_PTHREADS
	pthread_mutex_init(&pData->mtxTCPSend, 0);
#	endif

	/* we are now after the protocol indicator. Now check if we should
	 * use compression. We begin to use a new option format for this:
	 * @(option,option)host:port
	 * The first option defined is "z[0..9]" where the digit indicates
	 * the compression level. If it is not given, 9 (best compression) is
	 * assumed. An example action statement might be:
	 * @@(z5,o)127.0.0.1:1400  
	 * Which means send via TCP with medium (5) compresion (z) to the local
	 * host on port 1400. The '0' option means that octet-couting (as in
	 * IETF I-D syslog-transport-tls) is to be used for framing (this option
	 * applies to TCP-based syslog only and is ignored when specified with UDP).
	 * That is not yet implemented.
	 * rgerhards, 2006-12-07
	 */
	if(*p == '(') {
		/* at this position, it *must* be an option indicator */
		do {
			++p; /* eat '(' or ',' (depending on when called) */
			/* check options */
			if(*p == 'z') { /* compression */
#					ifdef USE_NETZIP
				++p; /* eat */
				if(isdigit((int) *p)) {
					int iLevel;
					iLevel = *p - '0';
					++p; /* eat */
					pData->compressionLevel = iLevel;
				} else {
					logerrorInt("Invalid compression level '%c' specified in "
						 "forwardig action - NOT turning on compression.",
						 *p);
				}
#					else
				logerror("Compression requested, but rsyslogd is not compiled "
					 "with compression support - request ignored.");
#					endif /* #ifdef USE_NETZIP */
			} else if(*p == 'o') { /* octet-couting based TCP framing? */
				++p; /* eat */
				/* no further options settable */
				pData->tcp_framing = TCP_FRAMING_OCTET_COUNTING;
			} else { /* invalid option! Just skip it... */
				logerrorInt("Invalid option %c in forwarding action - ignoring.", *p);
				++p; /* eat invalid option */
			}
			/* the option processing is done. We now do a generic skip
			 * to either the next option or the end of the option
			 * block.
			 */
			while(*p && *p != ')' && *p != ',')
				++p;	/* just skip it */
		} while(*p && *p == ','); /* Attention: do.. while() */
		if(*p == ')')
			++p; /* eat terminator, on to next */
		else
			/* we probably have end of string - leave it for the rest
			 * of the code to handle it (but warn the user)
			 */
			logerror("Option block not terminated in gssapi forward action.");
	}
	/* extract the host first (we do a trick - we replace the ';' or ':' with a '\0')
	 * now skip to port and then template name. rgerhards 2005-07-06
	 */
	for(q = p ; *p && *p != ';' && *p != ':' ; ++p)
		/* JUST SKIP */;

	pData->port = NULL;
	if(*p == ':') { /* process port */
		uchar * tmp;

		*p = '\0'; /* trick to obtain hostname (later)! */
		tmp = ++p;
		for(i=0 ; *p && isdigit((int) *p) ; ++p, ++i)
			/* SKIP AND COUNT */;
		pData->port = malloc(i + 1);
		if(pData->port == NULL) {
			logerror("Could not get memory to store syslog forwarding port, "
				 "using default port, results may not be what you intend\n");
			/* we leave f_forw.port set to NULL, this is then handled by
			 * getFwdSyslogPt().
			 */
		} else {
			memcpy(pData->port, tmp, i);
			*(pData->port + i) = '\0';
		}
	}
	
	/* now skip to template */
	bErr = 0;
	while(*p && *p != ';') {
		if(*p && *p != ';' && !isspace((int) *p)) {
			if(bErr == 0) { /* only 1 error msg! */
				bErr = 1;
				errno = 0;
				logerror("invalid selector line (port), probably not doing "
					 "what was intended");
			}
		}
		++p;
	}

	/* TODO: make this if go away! */
	if(*p == ';') {
		*p = '\0'; /* trick to obtain hostname (later)! */
		strcpy(pData->f_hname, (char*) q);
		*p = ';';
	} else
		strcpy(pData->f_hname, (char*) q);

	/* process template */
	if((iRet = cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_NO_RQD_TPL_OPTS, (uchar*) " StdFwdFmt"))
	   != RS_RET_OK)
		goto finalize_it;

	/* first set the pData->eDestState */
	memset(&hints, 0, sizeof(hints));
	/* port must be numeric, because config file syntax requests this */
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	if( (error = getaddrinfo(pData->f_hname, getFwdSyslogPt(pData), &hints, &res)) != 0) {
		pData->eDestState = eDestFORW_UNKN;
		pData->iRtryCnt = INET_RETRY_MAX;
		pData->ttSuspend = time(NULL);
	} else {
		pData->eDestState = eDestFORW;
		pData->f_addr = res;
	}

	/* TODO: do we need to call freeInstance if we failed - this is a general question for
	 * all output modules. I'll address it lates as the interface evolves. rgerhards, 2007-07-25
	 */
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINneedUDPSocket
CODESTARTneedUDPSocket
	iRet = RS_RET_FALSE;
ENDneedUDPSocket


BEGINonSelectReadyWrite
CODESTARTonSelectReadyWrite
	dbgprintf("tcp send socket %d ready for writing.\n", pData->sock);
	TCPSendSetStatus(pData, TCP_SEND_READY);
	/* Send stored message (if any) */
	if(pData->savedMsg != NULL) {
		if(TCPSend(pData, pData->savedMsg, pData->savedMsgLen, pData->tcp_framing,
			   TCPSendGSSInit, TCPSendGSSSend, TCPSendGSSPrepRetry) != RS_RET_OK) {
			/* error! */
			pData->eDestState = eDestFORW_SUSP;
			errno = 0;
			logerror("error forwarding via tcp, suspending...");
		}
		free(pData->savedMsg);
		pData->savedMsg = NULL;
	}
ENDonSelectReadyWrite


BEGINgetWriteFDForSelect
CODESTARTgetWriteFDForSelect
	if(   (pData->eDestState == eDestFORW)
	   && TCPSendGetStatus(pData) == TCP_SEND_CONNECTING) {
		*fd = pData->sock;
		iRet = RS_RET_OK;
	}
ENDgetWriteFDForSelect




BEGINmodExit
CODESTARTmodExit
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
ENDqueryEtryPt


/* set a new GSSMODE based on config directive */
static rsRetVal setGSSMode(void __attribute__((unused)) *pVal, uchar *mode)
{
	if (!strcmp((char *) mode, "integrity")) {
		gss_mode = GSSMODE_MIC;
		free(mode);
		dbgprintf("GSS-API gssmode set to GSSMODE_MIC\n");
	} else if (!strcmp((char *) mode, "encryption")) {
		gss_mode = GSSMODE_ENC;
		free(mode);
		dbgprintf("GSS-API gssmode set to GSSMODE_ENC\n");
	} else {
		logerrorSz("unknown gssmode parameter: %s", (char *) mode);
		free(mode);
		return RS_RET_ERR;
	}

	return RS_RET_OK;
}


static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	gss_mode = GSSMODE_ENC;
	if (gss_base_service_name != NULL) {
		free(gss_base_service_name);
		gss_base_service_name = NULL;
	}
	return RS_RET_OK;
}


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = 1; /* so far, we only support the initial definition */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"gssforwardservicename", 0, eCmdHdlrGetWord, NULL, &gss_base_service_name, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"gssmode", 0, eCmdHdlrGetWord, setGSSMode, &gss_mode, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit

#endif /* #ifdef USE_GSSAPI */
/*
 * vi:set ai:
 */
