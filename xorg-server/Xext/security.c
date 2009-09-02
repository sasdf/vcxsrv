/*

Copyright 1996, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.

*/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#else
#define XACE
#endif

#include "scrnintstr.h"
#include "inputstr.h"
#include "windowstr.h"
#include "propertyst.h"
#include "colormapst.h"
#include "privates.h"
#include "registry.h"
#include "xacestr.h"
#include "securitysrv.h"
#include <X11/extensions/securstr.h>
#include "modinit.h"

/* Extension stuff */
static int SecurityErrorBase;  /* first Security error number */
static int SecurityEventBase;  /* first Security event number */

RESTYPE SecurityAuthorizationResType; /* resource type for authorizations */
static RESTYPE RTEventClient;

static CallbackListPtr SecurityValidateGroupCallback = NULL;

/* Private state record */
static int stateKeyIndex;
static DevPrivateKey stateKey = &stateKeyIndex;

/* This is what we store as client security state */
typedef struct {
    int haveState;
    unsigned int trustLevel;
    XID authId;
} SecurityStateRec;

/* Extensions that untrusted clients shouldn't have access to */
static char *SecurityTrustedExtensions[] = {
    "XC-MISC",
    "BIG-REQUESTS",
    "XpExtension",
    NULL
};

/*
 * Access modes that untrusted clients are allowed on trusted objects.
 */
static const Mask SecurityResourceMask =
    DixGetAttrAccess | DixReceiveAccess | DixListPropAccess |
    DixGetPropAccess | DixListAccess;
static const Mask SecurityWindowExtraMask = DixRemoveAccess;
static const Mask SecurityRootWindowExtraMask =
    DixReceiveAccess | DixSendAccess | DixAddAccess | DixRemoveAccess;
static const Mask SecurityDeviceMask =
    DixGetAttrAccess | DixReceiveAccess | DixGetFocusAccess |
    DixGrabAccess | DixSetAttrAccess | DixUseAccess;
static const Mask SecurityServerMask = DixGetAttrAccess | DixGrabAccess;
static const Mask SecurityClientMask = DixGetAttrAccess;


/* SecurityAudit
 *
 * Arguments:
 *	format is the formatting string to be used to interpret the
 *	  remaining arguments.
 *
 * Returns: nothing.
 *
 * Side Effects:
 *	Writes the message to the log file if security logging is on.
 */

static void
SecurityAudit(char *format, ...)
{
    va_list args;

    if (auditTrailLevel < SECURITY_AUDIT_LEVEL)
	return;
    va_start(args, format);
    VAuditF(format, args);
    va_end(args);
} /* SecurityAudit */

/*
 * Performs a Security permission check.
 */
static int
SecurityDoCheck(SecurityStateRec *subj, SecurityStateRec *obj,
		Mask requested, Mask allowed)
{
    if (!subj->haveState || !obj->haveState)
	return Success;
    if (subj->trustLevel == XSecurityClientTrusted)
	return Success;
    if (obj->trustLevel != XSecurityClientTrusted)
	return Success;
    if ((requested | allowed) == allowed)
	return Success;

    return BadAccess;
}

/*
 * Labels initial server objects.
 */
static void
SecurityLabelInitial(void)
{
    SecurityStateRec *state;

    /* Do the serverClient */
    state = dixLookupPrivate(&serverClient->devPrivates, stateKey);
    state->trustLevel = XSecurityClientTrusted;
    state->haveState = TRUE;
}

/*
 * Looks up a request name
 */
static _X_INLINE const char *
SecurityLookupRequestName(ClientPtr client)
{
    int major = ((xReq *)client->requestBuffer)->reqType;
    int minor = MinorOpcodeOfRequest(client);
    return LookupRequestName(major, minor);
}


#define rClient(obj) (clients[CLIENT_ID((obj)->resource)])

/* SecurityDeleteAuthorization
 *
 * Arguments:
 *	value is the authorization to delete.
 *	id is its resource ID.
 *
 * Returns: Success.
 *
 * Side Effects:
 *	Frees everything associated with the authorization.
 */

static int
SecurityDeleteAuthorization(
    pointer value,
    XID id)
{
    SecurityAuthorizationPtr pAuth = (SecurityAuthorizationPtr)value;
    unsigned short name_len, data_len;
    char *name, *data;
    int status;
    int i;
    OtherClientsPtr pEventClient;

    /* Remove the auth using the os layer auth manager */

    status = AuthorizationFromID(pAuth->id, &name_len, &name,
				 &data_len, &data);
    assert(status);
    status = RemoveAuthorization(name_len, name, data_len, data);
    assert(status);
    (void)status;

    /* free the auth timer if there is one */

    if (pAuth->timer) TimerFree(pAuth->timer);

    /* send revoke events */

    while ((pEventClient = pAuth->eventClients))
    {
	/* send revocation event event */
	ClientPtr client = rClient(pEventClient);

	if (!client->clientGone)
	{
	    xSecurityAuthorizationRevokedEvent are;
	    are.type = SecurityEventBase + XSecurityAuthorizationRevoked;
	    are.sequenceNumber = client->sequence;
	    are.authId = pAuth->id;
	    WriteEventsToClient(client, 1, (xEvent *)&are);
	}
	FreeResource(pEventClient->resource, RT_NONE);
    }

    /* kill all clients using this auth */

    for (i = 1; i<currentMaxClients; i++)
	if (clients[i]) {
	    SecurityStateRec *state;
	    state = dixLookupPrivate(&clients[i]->devPrivates, stateKey);
	    if (state->haveState && state->authId == pAuth->id)
		CloseDownClient(clients[i]);
	}

    SecurityAudit("revoked authorization ID %d\n", pAuth->id);
    xfree(pAuth);
    return Success;

} /* SecurityDeleteAuthorization */


/* resource delete function for RTEventClient */
static int
SecurityDeleteAuthorizationEventClient(
    pointer value,
    XID id)
{
    OtherClientsPtr pEventClient, prev = NULL;
    SecurityAuthorizationPtr pAuth = (SecurityAuthorizationPtr)value;

    for (pEventClient = pAuth->eventClients;
	 pEventClient;
	 pEventClient = pEventClient->next)
    {
	if (pEventClient->resource == id)
	{
	    if (prev)
		prev->next = pEventClient->next;
	    else
		pAuth->eventClients = pEventClient->next;
	    xfree(pEventClient);
	    return(Success);
	}
	prev = pEventClient;
    }
    /*NOTREACHED*/
    return -1; /* make compiler happy */
} /* SecurityDeleteAuthorizationEventClient */


/* SecurityComputeAuthorizationTimeout
 *
 * Arguments:
 *	pAuth is the authorization for which we are computing the timeout
 *	seconds is the number of seconds we want to wait
 *
 * Returns:
 *	the number of milliseconds that the auth timer should be set to
 *
 * Side Effects:
 *	Sets pAuth->secondsRemaining to any "overflow" amount of time
 *	that didn't fit in 32 bits worth of milliseconds
 */

static CARD32
SecurityComputeAuthorizationTimeout(
    SecurityAuthorizationPtr pAuth,
    unsigned int seconds)
{
    /* maxSecs is the number of full seconds that can be expressed in
     * 32 bits worth of milliseconds
     */
    CARD32 maxSecs = (CARD32)(~0) / (CARD32)MILLI_PER_SECOND;

    if (seconds > maxSecs)
    { /* only come here if we want to wait more than 49 days */
	pAuth->secondsRemaining = seconds - maxSecs;
	return maxSecs * MILLI_PER_SECOND;
    }
    else
    { /* by far the common case */
	pAuth->secondsRemaining = 0;
	return seconds * MILLI_PER_SECOND;
    }
} /* SecurityStartAuthorizationTimer */

/* SecurityAuthorizationExpired
 *
 * This function is passed as an argument to TimerSet and gets called from
 * the timer manager in the os layer when its time is up.
 *
 * Arguments:
 *	timer is the timer for this authorization.
 *	time is the current time.
 *	pval is the authorization whose time is up.
 *
 * Returns:
 *	A new time delay in milliseconds if the timer should wait some
 *	more, else zero.
 *
 * Side Effects:
 *	Frees the authorization resource if the timeout period is really
 *	over, otherwise recomputes pAuth->secondsRemaining.
 */

static CARD32
SecurityAuthorizationExpired(
    OsTimerPtr timer,
    CARD32 time,
    pointer pval)
{
    SecurityAuthorizationPtr pAuth = (SecurityAuthorizationPtr)pval;

    assert(pAuth->timer == timer);

    if (pAuth->secondsRemaining)
    {
	return SecurityComputeAuthorizationTimeout(pAuth,
						   pAuth->secondsRemaining);
    }
    else
    {
	FreeResource(pAuth->id, RT_NONE);
	return 0;
    }
} /* SecurityAuthorizationExpired */

/* SecurityStartAuthorizationTimer
 *
 * Arguments:
 *	pAuth is the authorization whose timer should be started.
 *
 * Returns: nothing.
 *
 * Side Effects:
 *	A timer is started, set to expire after the timeout period for
 *	this authorization.  When it expires, the function
 *	SecurityAuthorizationExpired will be called.
 */

static void
SecurityStartAuthorizationTimer(
    SecurityAuthorizationPtr pAuth)
{
    pAuth->timer = TimerSet(pAuth->timer, 0,
	SecurityComputeAuthorizationTimeout(pAuth, pAuth->timeout),
			    SecurityAuthorizationExpired, pAuth);
} /* SecurityStartAuthorizationTimer */


/* Proc functions all take a client argument, execute the request in
 * client->requestBuffer, and return a protocol error status.
 */

static int
ProcSecurityQueryVersion(
    ClientPtr client)
{
    /* REQUEST(xSecurityQueryVersionReq); */
    xSecurityQueryVersionReply 	rep;

    REQUEST_SIZE_MATCH(xSecurityQueryVersionReq);
    rep.type        	= X_Reply;
    rep.sequenceNumber 	= client->sequence;
    rep.length         	= 0;
    rep.majorVersion  	= SECURITY_MAJOR_VERSION;
    rep.minorVersion  	= SECURITY_MINOR_VERSION;
    if(client->swapped)
    {
	char n;
    	swaps(&rep.sequenceNumber, n);
	swaps(&rep.majorVersion, n);
	swaps(&rep.minorVersion, n);
    }
    (void)WriteToClient(client, SIZEOF(xSecurityQueryVersionReply),
			(char *)&rep);
    return (client->noClientException);
} /* ProcSecurityQueryVersion */


static int
SecurityEventSelectForAuthorization(
    SecurityAuthorizationPtr pAuth,
    ClientPtr client,
    Mask mask)
{
    OtherClients *pEventClient;

    for (pEventClient = pAuth->eventClients;
	 pEventClient;
	 pEventClient = pEventClient->next)
    {
	if (SameClient(pEventClient, client))
	{
	    if (mask == 0)
		FreeResource(pEventClient->resource, RT_NONE);
	    else
		pEventClient->mask = mask;
	    return Success;
	}
    }
    
    pEventClient = (OtherClients *) xalloc(sizeof(OtherClients));
    if (!pEventClient)
	return BadAlloc;
    pEventClient->mask = mask;
    pEventClient->resource = FakeClientID(client->index);
    pEventClient->next = pAuth->eventClients;
    if (!AddResource(pEventClient->resource, RTEventClient,
		     (pointer)pAuth))
    {
	xfree(pEventClient);
	return BadAlloc;
    }
    pAuth->eventClients = pEventClient;

    return Success;
} /* SecurityEventSelectForAuthorization */


static int
ProcSecurityGenerateAuthorization(
    ClientPtr client)
{
    REQUEST(xSecurityGenerateAuthorizationReq);
    int len;			/* request length in CARD32s*/
    Bool removeAuth = FALSE;	/* if bailout, call RemoveAuthorization? */
    SecurityAuthorizationPtr pAuth = NULL;  /* auth we are creating */
    int err;			/* error to return from this function */
    XID authId;			/* authorization ID assigned by os layer */
    xSecurityGenerateAuthorizationReply rep; /* reply struct */
    unsigned int trustLevel;    /* trust level of new auth */
    XID group;			/* group of new auth */
    CARD32 timeout;		/* timeout of new auth */
    CARD32 *values;		/* list of supplied attributes */
    char *protoname;		/* auth proto name sent in request */
    char *protodata;		/* auth proto data sent in request */
    unsigned int authdata_len;  /* # bytes of generated auth data */
    char *pAuthdata;		/* generated auth data */
    Mask eventMask;		/* what events on this auth does client want */

    /* check request length */

    REQUEST_AT_LEAST_SIZE(xSecurityGenerateAuthorizationReq);
    len = SIZEOF(xSecurityGenerateAuthorizationReq) >> 2;
    len += (stuff->nbytesAuthProto + (unsigned)3) >> 2;
    len += (stuff->nbytesAuthData  + (unsigned)3) >> 2;
    values = ((CARD32 *)stuff) + len;
    len += Ones(stuff->valueMask);
    if (client->req_len != len)
	return BadLength;

    /* check valuemask */
    if (stuff->valueMask & ~XSecurityAllAuthorizationAttributes)
    {
	client->errorValue = stuff->valueMask;
	return BadValue;
    }

    /* check timeout */
    timeout = 60;
    if (stuff->valueMask & XSecurityTimeout)
    {
	timeout = *values++;
    }

    /* check trustLevel */
    trustLevel = XSecurityClientUntrusted;
    if (stuff->valueMask & XSecurityTrustLevel)
    {
	trustLevel = *values++;
	if (trustLevel != XSecurityClientTrusted &&
	    trustLevel != XSecurityClientUntrusted)
	{
	    client->errorValue = trustLevel;
	    return BadValue;
	}
    }

    /* check group */
    group = None;
    if (stuff->valueMask & XSecurityGroup)
    {
	group = *values++;
	if (SecurityValidateGroupCallback)
	{
	    SecurityValidateGroupInfoRec vgi;
	    vgi.group = group;
	    vgi.valid = FALSE;
	    CallCallbacks(&SecurityValidateGroupCallback, (pointer)&vgi);

	    /* if nobody said they recognized it, it's an error */

	    if (!vgi.valid)
	    {
		client->errorValue = group;
		return BadValue;
	    }
	}
    }

    /* check event mask */
    eventMask = 0;
    if (stuff->valueMask & XSecurityEventMask)
    {
	eventMask = *values++;
	if (eventMask & ~XSecurityAllEventMasks)
	{
	    client->errorValue = eventMask;
	    return BadValue;
	}
    }

    protoname = (char *)&stuff[1];
    protodata = protoname + ((stuff->nbytesAuthProto + (unsigned)3) >> 2);

    /* call os layer to generate the authorization */

    authId = GenerateAuthorization(stuff->nbytesAuthProto, protoname,
				   stuff->nbytesAuthData,  protodata,
				   &authdata_len, &pAuthdata);
    if ((XID) ~0L == authId)
    {
	err = SecurityErrorBase + XSecurityBadAuthorizationProtocol;
	goto bailout;
    }

    /* now that we've added the auth, remember to remove it if we have to
     * abort the request for some reason (like allocation failure)
     */
    removeAuth = TRUE;

    /* associate additional information with this auth ID */

    pAuth = (SecurityAuthorizationPtr)xalloc(sizeof(SecurityAuthorizationRec));
    if (!pAuth)
    {
	err = BadAlloc;
	goto bailout;
    }

    /* fill in the auth fields */

    pAuth->id = authId;
    pAuth->timeout = timeout;
    pAuth->group = group;
    pAuth->trustLevel = trustLevel;
    pAuth->refcnt = 0;	/* the auth was just created; nobody's using it yet */
    pAuth->secondsRemaining = 0;
    pAuth->timer = NULL;
    pAuth->eventClients = NULL;

    /* handle event selection */
    if (eventMask)
    {
	err = SecurityEventSelectForAuthorization(pAuth, client, eventMask);
	if (err != Success)
	    goto bailout;
    }

    if (!AddResource(authId, SecurityAuthorizationResType, pAuth))
    {
	err = BadAlloc;
	goto bailout;
    }

    /* start the timer ticking */

    if (pAuth->timeout != 0)
	SecurityStartAuthorizationTimer(pAuth);

    /* tell client the auth id and data */

    rep.type = X_Reply;
    rep.length = (authdata_len + 3) >> 2;
    rep.sequenceNumber = client->sequence;
    rep.authId = authId;
    rep.dataLength = authdata_len;

    if (client->swapped)
    {
	char n;
    	swapl(&rep.length, n);
    	swaps(&rep.sequenceNumber, n);
    	swapl(&rep.authId, n);
    	swaps(&rep.dataLength, n);
    }

    WriteToClient(client, SIZEOF(xSecurityGenerateAuthorizationReply),
		  (char *)&rep);
    WriteToClient(client, authdata_len, pAuthdata);

    SecurityAudit("client %d generated authorization %d trust %d timeout %d group %d events %d\n",
		  client->index, pAuth->id, pAuth->trustLevel, pAuth->timeout,
		  pAuth->group, eventMask);

    /* the request succeeded; don't call RemoveAuthorization or free pAuth */

    removeAuth = FALSE;
    pAuth = NULL;
    err = client->noClientException;

bailout:
    if (removeAuth)
	RemoveAuthorization(stuff->nbytesAuthProto, protoname,
			    authdata_len, pAuthdata);
    if (pAuth) xfree(pAuth);
    return err;

} /* ProcSecurityGenerateAuthorization */

static int
ProcSecurityRevokeAuthorization(
    ClientPtr client)
{
    REQUEST(xSecurityRevokeAuthorizationReq);
    SecurityAuthorizationPtr pAuth;

    REQUEST_SIZE_MATCH(xSecurityRevokeAuthorizationReq);

    pAuth = (SecurityAuthorizationPtr)SecurityLookupIDByType(client,
	stuff->authId, SecurityAuthorizationResType, DixDestroyAccess);
    if (!pAuth)
	return SecurityErrorBase + XSecurityBadAuthorization;

    FreeResource(stuff->authId, RT_NONE);
    return Success;
} /* ProcSecurityRevokeAuthorization */


static int
ProcSecurityDispatch(
    ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data)
    {
	case X_SecurityQueryVersion:
	    return ProcSecurityQueryVersion(client);
	case X_SecurityGenerateAuthorization:
	    return ProcSecurityGenerateAuthorization(client);
	case X_SecurityRevokeAuthorization:
	    return ProcSecurityRevokeAuthorization(client);
	default:
	    return BadRequest;
    }
} /* ProcSecurityDispatch */

static int
SProcSecurityQueryVersion(
    ClientPtr client)
{
    REQUEST(xSecurityQueryVersionReq);
    char	n;

    swaps(&stuff->length, n);
    REQUEST_SIZE_MATCH(xSecurityQueryVersionReq);
    swaps(&stuff->majorVersion, n);
    swaps(&stuff->minorVersion,n);
    return ProcSecurityQueryVersion(client);
} /* SProcSecurityQueryVersion */


static int
SProcSecurityGenerateAuthorization(
    ClientPtr client)
{
    REQUEST(xSecurityGenerateAuthorizationReq);
    char	n;
    CARD32 *values;
    unsigned long nvalues;
    int values_offset;

    swaps(&stuff->length, n);
    REQUEST_AT_LEAST_SIZE(xSecurityGenerateAuthorizationReq);
    swaps(&stuff->nbytesAuthProto, n);
    swaps(&stuff->nbytesAuthData, n);
    swapl(&stuff->valueMask, n);
    values_offset = ((stuff->nbytesAuthProto + (unsigned)3) >> 2) +
		    ((stuff->nbytesAuthData + (unsigned)3) >> 2);
    if (values_offset > 
	stuff->length - (sz_xSecurityGenerateAuthorizationReq >> 2))
	return BadLength;
    values = (CARD32 *)(&stuff[1]) + values_offset;
    nvalues = (((CARD32 *)stuff) + stuff->length) - values;
    SwapLongs(values, nvalues);
    return ProcSecurityGenerateAuthorization(client);
} /* SProcSecurityGenerateAuthorization */


static int
SProcSecurityRevokeAuthorization(
    ClientPtr client)
{
    REQUEST(xSecurityRevokeAuthorizationReq);
    char	n;

    swaps(&stuff->length, n);
    REQUEST_SIZE_MATCH(xSecurityRevokeAuthorizationReq);
    swapl(&stuff->authId, n);
    return ProcSecurityRevokeAuthorization(client);
} /* SProcSecurityRevokeAuthorization */


static int
SProcSecurityDispatch(
    ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data)
    {
	case X_SecurityQueryVersion:
	    return SProcSecurityQueryVersion(client);
	case X_SecurityGenerateAuthorization:
	    return SProcSecurityGenerateAuthorization(client);
	case X_SecurityRevokeAuthorization:
	    return SProcSecurityRevokeAuthorization(client);
	default:
	    return BadRequest;
    }
} /* SProcSecurityDispatch */

static void 
SwapSecurityAuthorizationRevokedEvent(
    xSecurityAuthorizationRevokedEvent *from,
    xSecurityAuthorizationRevokedEvent *to)
{
    to->type = from->type;
    to->detail = from->detail;
    cpswaps(from->sequenceNumber, to->sequenceNumber);
    cpswapl(from->authId, to->authId);
}

/* SecurityCheckDeviceAccess
 *
 * Arguments:
 *	client is the client attempting to access a device.
 *	dev is the device being accessed.
 *	fromRequest is TRUE if the device access is a direct result of
 *	  the client executing some request and FALSE if it is a
 *	  result of the server trying to send an event (e.g. KeymapNotify)
 *	  to the client.
 * Returns:
 *	TRUE if the device access should be allowed, else FALSE.
 *
 * Side Effects:
 *	An audit message is generated if access is denied.
 */

static void
SecurityDevice(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceDeviceAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;
    Mask requested = rec->access_mode;
    Mask allowed = SecurityDeviceMask;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&serverClient->devPrivates, stateKey);

    if (rec->dev != inputInfo.keyboard)
	/* this extension only supports the core keyboard */
	allowed = requested;

    if (SecurityDoCheck(subj, obj, requested, allowed) != Success) {
	SecurityAudit("Security denied client %d keyboard access on request "
		      "%s\n", rec->client->index,
		      SecurityLookupRequestName(rec->client));
	rec->status = BadAccess;
    }
}

/* SecurityResource
 *
 * This function gets plugged into client->CheckAccess and is called from
 * SecurityLookupIDByType/Class to determine if the client can access the
 * resource.
 *
 * Arguments:
 *	client is the client doing the resource access.
 *	id is the resource id.
 *	rtype is its type or class.
 *	access_mode represents the intended use of the resource; see
 *	  resource.h.
 *	res is a pointer to the resource structure for this resource.
 *
 * Returns:
 *	If access is granted, the value of rval that was passed in, else FALSE.
 *
 * Side Effects:
 *	Disallowed resource accesses are audited.
 */

static void
SecurityResource(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceResourceAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;
    int cid = CLIENT_ID(rec->id);
    Mask requested = rec->access_mode;
    Mask allowed = SecurityResourceMask;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&clients[cid]->devPrivates, stateKey);

    /* disable background None for untrusted windows */
    if ((requested & DixCreateAccess) && (rec->rtype == RT_WINDOW))
	if (subj->haveState && subj->trustLevel != XSecurityClientTrusted)
	    ((WindowPtr)rec->res)->forcedBG = TRUE;

    /* additional permissions for specific resource types */
    if (rec->rtype == RT_WINDOW)
	allowed |= SecurityWindowExtraMask;

    /* special checks for server-owned resources */
    if (cid == 0) {
	if (rec->rtype & RC_DRAWABLE)
	    /* additional operations allowed on root windows */
	    allowed |= SecurityRootWindowExtraMask;

	else if (rec->rtype == RT_COLORMAP)
	    /* allow access to default colormaps */
	    allowed = requested;

	else
	    /* allow read access to other server-owned resources */
	    allowed |= DixReadAccess;
    }

    if (SecurityDoCheck(subj, obj, requested, allowed) == Success)
	return;

    SecurityAudit("Security: denied client %d access %x to resource 0x%x "
		  "of client %d on request %s\n", rec->client->index,
		  requested, rec->id, cid,
		  SecurityLookupRequestName(rec->client));
    rec->status = BadAccess; /* deny access */
}


static void
SecurityExtension(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceExtAccessRec *rec = calldata;
    SecurityStateRec *subj;
    int i = 0;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);

    if (subj->haveState && subj->trustLevel == XSecurityClientTrusted)
	return;

    while (SecurityTrustedExtensions[i])
	if (!strcmp(SecurityTrustedExtensions[i++], rec->ext->name))
	    return;

    SecurityAudit("Security: denied client %d access to extension "
		  "%s on request %s\n",
		  rec->client->index, rec->ext->name,
		  SecurityLookupRequestName(rec->client));
    rec->status = BadAccess;
}

static void
SecurityServer(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceServerAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;
    Mask requested = rec->access_mode;
    Mask allowed = SecurityServerMask;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&serverClient->devPrivates, stateKey);
 
    if (SecurityDoCheck(subj, obj, requested, allowed) != Success) {
	SecurityAudit("Security: denied client %d access to server "
		      "configuration request %s\n", rec->client->index,
		      SecurityLookupRequestName(rec->client));
	rec->status = BadAccess;
    }
}

static void
SecurityClient(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceClientAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;
    Mask requested = rec->access_mode;
    Mask allowed = SecurityClientMask;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&rec->target->devPrivates, stateKey);

    if (SecurityDoCheck(subj, obj, requested, allowed) != Success) {
	SecurityAudit("Security: denied client %d access to client %d on "
		      "request %s\n", rec->client->index, rec->target->index,
		      SecurityLookupRequestName(rec->client));
	rec->status = BadAccess;
    }
}

static void
SecurityProperty(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{    
    XacePropertyAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;
    ATOM name = (*rec->ppProp)->propertyName;
    Mask requested = rec->access_mode;
    Mask allowed = SecurityResourceMask | DixReadAccess;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&wClient(rec->pWin)->devPrivates, stateKey);

    if (SecurityDoCheck(subj, obj, requested, allowed) != Success) {
	SecurityAudit("Security: denied client %d access to property %s "
		      "(atom 0x%x) window 0x%x of client %d on request %s\n",
		      rec->client->index, NameForAtom(name), name,
		      rec->pWin->drawable.id, wClient(rec->pWin)->index,
		      SecurityLookupRequestName(rec->client));
	rec->status = BadAccess;
    }
}

static void
SecuritySend(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceSendAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;

    if (rec->client) {
	int i;

	subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
	obj = dixLookupPrivate(&wClient(rec->pWin)->devPrivates, stateKey);

	if (SecurityDoCheck(subj, obj, DixSendAccess, 0) == Success)
	    return;

	for (i = 0; i < rec->count; i++)
	    if (rec->events[i].u.u.type != UnmapNotify &&
		rec->events[i].u.u.type != ConfigureRequest &&
		rec->events[i].u.u.type != ClientMessage) {

		SecurityAudit("Security: denied client %d from sending event "
			      "of type %s to window 0x%x of client %d\n",
			      rec->client->index,
			      LookupEventName(rec->events[i].u.u.type),
			      rec->pWin->drawable.id,
			      wClient(rec->pWin)->index);
		rec->status = BadAccess;
		return;
	    }
    }
}

static void
SecurityReceive(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    XaceReceiveAccessRec *rec = calldata;
    SecurityStateRec *subj, *obj;

    subj = dixLookupPrivate(&rec->client->devPrivates, stateKey);
    obj = dixLookupPrivate(&wClient(rec->pWin)->devPrivates, stateKey);

    if (SecurityDoCheck(subj, obj, DixReceiveAccess, 0) == Success)
	return;

    SecurityAudit("Security: denied client %d from receiving an event "
		  "sent to window 0x%x of client %d\n",
		  rec->client->index, rec->pWin->drawable.id,
		  wClient(rec->pWin)->index);
    rec->status = BadAccess;
}

/* SecurityClientStateCallback
 *
 * Arguments:
 *	pcbl is &ClientStateCallback.
 *	nullata is NULL.
 *	calldata is a pointer to a NewClientInfoRec (include/dixstruct.h)
 *	which contains information about client state changes.
 *
 * Returns: nothing.
 *
 * Side Effects:
 * 
 * If a new client is connecting, its authorization ID is copied to
 * client->authID.  If this is a generated authorization, its reference
 * count is bumped, its timer is cancelled if it was running, and its
 * trustlevel is copied to TRUSTLEVEL(client).
 * 
 * If a client is disconnecting and the client was using a generated
 * authorization, the authorization's reference count is decremented, and
 * if it is now zero, the timer for this authorization is started.
 */

static void
SecurityClientState(CallbackListPtr *pcbl, pointer unused, pointer calldata)
{
    NewClientInfoRec *pci = calldata;
    SecurityStateRec *state;
    SecurityAuthorizationPtr pAuth;
    int rc;

    state = dixLookupPrivate(&pci->client->devPrivates, stateKey);

    switch (pci->client->clientState) {
    case ClientStateInitial:
	state->trustLevel = XSecurityClientTrusted;
	state->authId = None;
	state->haveState = TRUE;
	break;

    case ClientStateRunning:
	state->authId = AuthorizationIDOfClient(pci->client);
	rc = dixLookupResourceByType((pointer *)&pAuth, state->authId,
			       SecurityAuthorizationResType, serverClient,
			       DixGetAttrAccess);
	if (rc == Success) {
	    /* it is a generated authorization */
	    pAuth->refcnt++;
	    if (pAuth->refcnt == 1 && pAuth->timer)
		TimerCancel(pAuth->timer);

	    state->trustLevel = pAuth->trustLevel;
	}
	break;

    case ClientStateGone:
    case ClientStateRetained:
	rc = dixLookupResourceByType((pointer *)&pAuth, state->authId,
			       SecurityAuthorizationResType, serverClient,
			       DixGetAttrAccess);
	if (rc == Success) {
	    /* it is a generated authorization */
	    pAuth->refcnt--;
	    if (pAuth->refcnt == 0)
		SecurityStartAuthorizationTimer(pAuth);
	}
	break;

    default:
	break;
    }
}

/* SecurityResetProc
 *
 * Arguments:
 *	extEntry is the extension information for the security extension.
 *
 * Returns: nothing.
 *
 * Side Effects:
 *	Performs any cleanup needed by Security at server shutdown time.
 */

static void
SecurityResetProc(
    ExtensionEntry *extEntry)
{
    /* Unregister callbacks */
    DeleteCallback(&ClientStateCallback, SecurityClientState, NULL);

    XaceDeleteCallback(XACE_EXT_DISPATCH, SecurityExtension, NULL);
    XaceDeleteCallback(XACE_RESOURCE_ACCESS, SecurityResource, NULL);
    XaceDeleteCallback(XACE_DEVICE_ACCESS, SecurityDevice, NULL);
    XaceDeleteCallback(XACE_PROPERTY_ACCESS, SecurityProperty, NULL);
    XaceDeleteCallback(XACE_SEND_ACCESS, SecuritySend, NULL);
    XaceDeleteCallback(XACE_RECEIVE_ACCESS, SecurityReceive, NULL);
    XaceDeleteCallback(XACE_CLIENT_ACCESS, SecurityClient, NULL);
    XaceDeleteCallback(XACE_EXT_ACCESS, SecurityExtension, NULL);
    XaceDeleteCallback(XACE_SERVER_ACCESS, SecurityServer, NULL);
}


/* SecurityExtensionInit
 *
 * Arguments: none.
 *
 * Returns: nothing.
 *
 * Side Effects:
 *	Enables the Security extension if possible.
 */

void
SecurityExtensionInit(INITARGS)
{
    ExtensionEntry	*extEntry;
    int ret = TRUE;

    SecurityAuthorizationResType =
	CreateNewResourceType(SecurityDeleteAuthorization);

    RTEventClient = CreateNewResourceType(
				SecurityDeleteAuthorizationEventClient);

    if (!SecurityAuthorizationResType || !RTEventClient)
	return;

    RTEventClient |= RC_NEVERRETAIN;
    RegisterResourceName(SecurityAuthorizationResType, "SecurityAuthorization");
    RegisterResourceName(RTEventClient, "SecurityEventClient");

    /* Allocate the private storage */
    if (!dixRequestPrivate(stateKey, sizeof(SecurityStateRec)))
	FatalError("SecurityExtensionSetup: Can't allocate client private.\n");

    /* Register callbacks */
    ret &= AddCallback(&ClientStateCallback, SecurityClientState, NULL);

    ret &= XaceRegisterCallback(XACE_EXT_DISPATCH, SecurityExtension, NULL);
    ret &= XaceRegisterCallback(XACE_RESOURCE_ACCESS, SecurityResource, NULL);
    ret &= XaceRegisterCallback(XACE_DEVICE_ACCESS, SecurityDevice, NULL);
    ret &= XaceRegisterCallback(XACE_PROPERTY_ACCESS, SecurityProperty, NULL);
    ret &= XaceRegisterCallback(XACE_SEND_ACCESS, SecuritySend, NULL);
    ret &= XaceRegisterCallback(XACE_RECEIVE_ACCESS, SecurityReceive, NULL);
    ret &= XaceRegisterCallback(XACE_CLIENT_ACCESS, SecurityClient, NULL);
    ret &= XaceRegisterCallback(XACE_EXT_ACCESS, SecurityExtension, NULL);
    ret &= XaceRegisterCallback(XACE_SERVER_ACCESS, SecurityServer, NULL);

    if (!ret)
	FatalError("SecurityExtensionSetup: Failed to register callbacks\n");

    /* Add extension to server */
    extEntry = AddExtension(SECURITY_EXTENSION_NAME,
			    XSecurityNumberEvents, XSecurityNumberErrors,
			    ProcSecurityDispatch, SProcSecurityDispatch,
                            SecurityResetProc, StandardMinorOpcode);

    SecurityErrorBase = extEntry->errorBase;
    SecurityEventBase = extEntry->eventBase;

    EventSwapVector[SecurityEventBase + XSecurityAuthorizationRevoked] =
	(EventSwapPtr)SwapSecurityAuthorizationRevokedEvent;

    /* Label objects that were created before we could register ourself */
    SecurityLabelInitial();
}
