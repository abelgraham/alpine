#if !defined(lint) && !defined(DOS)
static char rcsid[] = "$Id: alpined.c 272 2006-11-27 22:57:25Z hubert@u.washington.edu $";
#endif

/* ========================================================================
 * Copyright 2006 University of Washington
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ========================================================================
 */

/* ========================================================================
    Implement alpine TCL interfaces.  Execute TCL interfaces
    via interpreter reading commands and writing results over
    UNIX domain socket.
   ======================================================================== */


#include <system.h>
#include <general.h>

#include "../../../c-client/c-client.h"
#include "../../../c-client/imap4r1.h"

#include "../../../pith/osdep/color.h"	/* color support library */
#include "../../../pith/osdep/canaccess.h"
#include "../../../pith/osdep/temp_nam.h"

#include "../../../pith/stream.h"
#include "../../../pith/context.h"
#include "../../../pith/state.h"
#include "../../../pith/msgno.h"
#include "../../../pith/debug.h"
#include "../../../pith/init.h"
#include "../../../pith/conf.h"
#include "../../../pith/conftype.h"
#include "../../../pith/detoken.h"
#include "../../../pith/flag.h"
#include "../../../pith/help.h"
#include "../../../pith/remote.h"
#include "../../../pith/status.h"
#include "../../../pith/mailcmd.h"
#include "../../../pith/savetype.h"
#include "../../../pith/save.h"
#include "../../../pith/reply.h"
#include "../../../pith/sort.h"
#include "../../../pith/ldap.h"
#include "../../../pith/addrbook.h"
#include "../../../pith/takeaddr.h"
#include "../../../pith/bldaddr.h"
#include "../../../pith/copyaddr.h"
#include "../../../pith/thread.h"
#include "../../../pith/folder.h"
#include "../../../pith/mailview.h"
#include "../../../pith/indxtype.h"
#include "../../../pith/icache.h"
#include "../../../pith/mailindx.h"
#include "../../../pith/mailpart.h"
#include "../../../pith/mimedesc.h"
#include "../../../pith/detach.h"
#include "../../../pith/newmail.h"
#include "../../../pith/charset.h"
#include "../../../pith/util.h"
#include "../../../pith/rfc2231.h"
#include "../../../pith/string.h"
#include "../../../pith/send.h"
#include "../../../pith/options.h"

#include "alpined.h"
#include "color.h"
#include "imap.h"
#include "ldap.h"
#include "debug.h"
#include "stubs.h"

#include <tcl.h>


/*
 * Fake screen dimension for word wrap and such
 */
#define	FAKE_SCREEN_WIDTH	80
#define	FAKE_SCREEN_LENGTH	24

/*
 * Aribtrary minimum display width (in characters)
 */
#define	MIN_SCREEN_COLS		20


/*
 * Maximum number of lines allowed in signatures
 */
#define	SIG_MAX_LINES		24
#define	SIG_MAX_COLS		132


/*
 * Number of seconds we'll wait before we assume the client has wondered
 * on to more interesting content
 */
#define	PE_INPUT_TIMEOUT	1800


/*
 * Posting error lenght max
 */
#define	WP_MAX_POST_ERROR	128


/*
 *
 */
#define	AUTH_EMPTY_STRING	"NOPASSWD"
#define	AUTH_FAILURE_STRING	"BADPASSWD"


/*
 * Globals referenced throughout pine...
 */
struct pine *ps_global;				/* THE global variable! */


/*
 * More global state
 */
long	   peITop, peICount;

long	   peInputTimeout = PE_INPUT_TIMEOUT;
long	   peAbandonTimeout = 0;

int	   peNoPassword, peCredentialError;
char	   peCredentialRequestor[WP_MAXAUTHREQ];

char	  *peSockName;

char	  **peTSig;

CONTEXT_S *config_context_list;

bitmap_t changed_feature_list;
#define F_CH_ON(feature)	(bitnset((feature),changed_feature_list))
#define F_CH_OFF(feature)	(!F_CH_ON(feature))
#define F_CH_TURN_ON(feature)   (setbitn((feature),changed_feature_list))
#define F_CH_TURN_OFF(feature)  (clrbitn((feature),changed_feature_list))
#define F_CH_SET(feature,value) ((value) ? F_CH_TURN_ON((feature))       \
					 : F_CH_TURN_OFF((feature)))


typedef struct _status_msg {
    time_t		posted;
    unsigned	        type:3;
    unsigned	        seen:1;
    long	        id;
    char	       *text;
    struct _status_msg *next;
} STATMSG_S;

static STATMSG_S *peStatList;

typedef struct _composer_attachment {
    unsigned		 file:1;
    unsigned		 body:1;
    char		*id;
    union {
	struct {
	    char        *local;
	    char	*remote;
	    char	*type;
	    char	*subtype;
	    char	*description;
	    long	 size;
	} f;
	struct {
	    BODY	*body;
	} b;
	struct {
	    long  msgno;
	    char *part;
	} msg;
    } l;
    struct _composer_attachment *next;
} COMPATT_S;

static COMPATT_S *peCompAttach;

/*
 * Holds data passed 
 */
typedef struct _msg_data {
	ENVELOPE  *outgoing;
	METAENV	  *metaenv;
	PINEFIELD *custom;
	STORE_S   *msgtext;
	STRLIST_S *attach;
	char	  *fcc;
	int	   fcc_colid;
	int	   postop_fcc_no_attach;
	int	   flowed;
	char	  *charset;
	int	 (*postfunc)(METAENV *, BODY *, char *, CONTEXT_S **, char *);
} MSG_COL_S;


#ifdef ENABLE_LDAP
WPLDAP_S *wpldap_global;
#endif

/*
 * random string generator flags
 */
#define	PRS_NONE	0x0000
#define	PRS_LOWER_CASE	0x0001
#define	PRS_UPPER_CASE	0x0002
#define	PRS_MIXED_CASE	0x0004

/*
 * length of thread info string
 */
#define	WP_MAX_THRD_S	64



/*----------------------------------------------------------------------
  General use big buffer. It is used in the following places:
    compose_mail:    while parsing header of postponed message
    append_message2: while writing header into folder
    q_status_messageX: while doing printf formatting
    addr_book: Used to return expanded address in. (Can only use here 
               because mm_log doesn't q_status on PARSE errors !)
    alpine.c: When address specified on command line
    init.c: When expanding variable values
    and many many more...

 ----*/
char         tmp_20k_buf[20480];




/* Internal prototypes */
void	     peReturn(int, char *, char *);
int	     peWrite(int, char *);
char	    *peCreateUserContext(Tcl_Interp *, char *, char *, char *);
void	     peDestroyUserContext(struct pine **);
char	    *peLoadConfig(struct pine *);
int	     peCreateStream(Tcl_Interp *, CONTEXT_S *, char *, int);
void	     peDestroyStream(struct pine *);
int	     peSelect(Tcl_Interp *, int, Tcl_Obj **);
char	    *peSelValTense(Tcl_Obj *);
char	    *peSelValYear(Tcl_Obj *);
char	    *peSelValMonth(Tcl_Obj *);
char	    *peSelValDay(Tcl_Obj *);
int	     peSelValCase(Tcl_Obj *);
int	     peSelValField(Tcl_Obj *);
int	     peSelValFlag(Tcl_Obj *);
int	     peSelectError(Tcl_Interp *, char *);
int	     peApply(Tcl_Interp *, int, Tcl_Obj **);
int	     peApplyError(Tcl_Interp *, char *);
int	     peIndexFormat(Tcl_Interp *);
int	     peAppendIndexParts(Tcl_Interp *, long, Tcl_Obj *);
int	     peMessageStatusBits(Tcl_Interp *, long, int, Tcl_Obj **);
char	    *peMsgStatBitString(struct pine *, MAILSTREAM *, MSGNO_S *, long, long, long);
int	     peNewMailResult(Tcl_Interp *);
void	     peMarkInputTime(void);
int	     peMessageSize(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageDate(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageSubject(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageFromAddr(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageToAddr(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageCcAddr(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageField(Tcl_Interp *, long, char *);
int	     peMessageStatus(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageCharset(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageBounce(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageSpam(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMsgnoFromUID(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessageText(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMessagePartFromCID(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peLocateBodyByCID(char *, char *, BODY *);
char	    *peColorStr(char *, char *);
int	     peInterpWritec(int);
int	     peInterpFlush(void);
int	     peNullWritec(int);
void	     peGetMimeTyping(BODY *, Tcl_Obj **, Tcl_Obj **, Tcl_Obj **, Tcl_Obj **);
int	     peGetFlag(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peSetFlag(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peMsgSelect(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peReplyHeaders(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peAppListStrStr(Tcl_Interp *, Tcl_Obj *, char *, char *);
int	     peAppListStrPat(Tcl_Interp *, Tcl_Obj *, char *, PATTERN_S *);
int	     peAppListStrPatstat(Tcl_Interp *, Tcl_Obj *, char *, int);
int	     peAppListStrInt(Tcl_Interp *, Tcl_Obj *, char *, int);
int	     peAppListIntStr(Tcl_Interp *, Tcl_Obj *, int, char *);
int	     peAppListLongLong(Tcl_Interp *, Tcl_Obj *, long, long);
int	     peAppListStrAddr(Tcl_Interp *, Tcl_Obj *, char *, ADDRESS *);
int	     peReplyText(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peSoStrToList(Tcl_Interp *, Tcl_Obj *, STORE_S *);
int	     peForwardHeaders(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peForwardText(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peDetach(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peAttachInfo(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peSaveDefault(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peSave(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peGotoDefault(Tcl_Interp *, long, Tcl_Obj **);
int	     peTakeaddr(Tcl_Interp *, long, int, Tcl_Obj **);
int	     peReplyQuote(Tcl_Interp *, long, int, Tcl_Obj **);
long	     peMessageNumber(long);
long	     peSequenceNumber(long);
int	     peMsgCollector(Tcl_Interp *, int, Tcl_Obj **,
			    int (*)(METAENV *, BODY *, char *, CONTEXT_S **, char *));
int	     peMsgCollected(Tcl_Interp  *, MSG_COL_S *, char *);
void	     peMsgSetParm(PARAMETER **, char *, char *);
Tcl_Obj	    *peMsgAttachCollector(Tcl_Interp *, BODY *);
int	     peFccAppend(Tcl_Interp *, Tcl_Obj *, char *, int);
int	     peDoPost(METAENV *, BODY *, char *, CONTEXT_S **, char *);
int	     peDoPostpone(METAENV *, BODY *, char *, CONTEXT_S **, char *);
int	     peWriteSig (Tcl_Interp *, char *, Tcl_Obj **);
int	     peInitAddrbooks(Tcl_Interp *, int);
int	     peRuleStatVal(char *, int *);
int	     peRuleSet(Tcl_Interp *, Tcl_Obj **);
#if	0
ADDRESS	    *peAEToAddress(AdrBk_Entry *);
char	    *peAEFcc(AdrBk_Entry *);
#endif
NAMEVAL_S   *sort_key_rules(int);
NAMEVAL_S   *wp_indexheight_rules(int);
PINEFIELD   *peCustomHdrs(void);
STATMSG_S   *sml_newmsg(int, char *);
char	    *sml_getmsg(void);
char	   **sml_getmsgs(void);
void	     sml_seen(void);
#ifdef ENABLE_LDAP
int	     peLdapQueryResults(Tcl_Interp *);
int          peLdapStrlist(Tcl_Interp *, Tcl_Obj *, char **);
int          init_ldap_pname(struct pine *);
#endif /* ENABLE_LDAP */
char	    *strqchr(char *, int, int *, int);
Tcl_Obj     *wp_prune_folders(CONTEXT_S *, char *, int, char *, 
			      unsigned, int *, int, Tcl_Interp *);
int          hex_colorstr(char *, char *);
int          hexval(char);
int          ascii_colorstr(char *, char *);
COMPATT_S   *peNewAttach(void);
void	     peFreeAttach(COMPATT_S **);
COMPATT_S   *peGetAttachID(char *);
char	    *peFileAttachID(char *, char *, char *, char *, char *, int);
char	    *peBodyAttachID(BODY *);
void	     peBodyMoveContents(BODY *, BODY *);
int	     peClearAttachID(char *);
char	    *peRandomString(char *, int, int);
void	     ms_init(STRING *, void *, unsigned long);
char	     ms_next(STRING *);
void	     ms_setpos(STRING *, unsigned long);
long	     peAppendMsg(MAILSTREAM *, void *, char **, char **, STRING **);


/* Prototypes for Tcl-exported methods */
int	PEInit(Tcl_Interp *interp, char *);
void    PEExitCleanup(ClientData);
int	PEInfoCmd(ClientData clientData, Tcl_Interp *interp,
		  int objc, Tcl_Obj *CONST objv[]);
int	PEConfigCmd(ClientData clientData, Tcl_Interp *interp,
		    int objc, Tcl_Obj *CONST objv[]);
int	PEDebugCmd(ClientData clientData, Tcl_Interp *interp,
		   int objc, Tcl_Obj *CONST objv[]);
int	PESessionCmd(ClientData clientData, Tcl_Interp *interp,
		     int objc, Tcl_Obj *CONST objv[]);
int	PEMailboxCmd(ClientData clientData, Tcl_Interp *interp,
		     int objc, Tcl_Obj *CONST objv[]);
int	PEThreadCmd(ClientData clientData, Tcl_Interp *interp,
		    int objc, Tcl_Obj *CONST objv[]);
int	PEMessageCmd(ClientData clientData, Tcl_Interp *interp,
		     int objc, Tcl_Obj *CONST objv[]);
int	PEFolderCmd(ClientData clientData, Tcl_Interp *interp,
		    int objc, Tcl_Obj *CONST objv[]);
int	PEComposeCmd(ClientData clientData, Tcl_Interp *interp,
		     int objc, Tcl_Obj *CONST objv[]);
int	PEPostponeCmd(ClientData clientData, Tcl_Interp *interp,
		      int objc, Tcl_Obj *CONST objv[]);
int	PEAddressCmd(ClientData clientData, Tcl_Interp *interp,
		     int objc, Tcl_Obj *CONST objv[]);
int	PEClistCmd(ClientData clientData, Tcl_Interp *interp,
		   int objc, Tcl_Obj *CONST objv[]);
int	PELdapCmd(ClientData clientData, Tcl_Interp *interp,
		  int objc, Tcl_Obj *CONST objv[]);


/* Append package */
typedef struct append_pkg {
  MAILSTREAM *stream;		/* source stream */
  unsigned long msgno;		/* current message number */
  unsigned long msgmax;		/* maximum message number */
  char *flags;			/* current flags */
  char *date;			/* message internal date */
  STRING *message;		/* stringstruct of message */
} APPEND_PKG;

STRINGDRIVER mstring = {
  ms_init,			/* initialize string structure */
  ms_next,			/* get next byte in string structure */
  ms_setpos			/* set position in string structure */
};


/*----------------------------------------------------------------------
     main routine -- entry point

  Args: argv, argc -- The command line arguments


  Setup c-client drivers and dive into TCL interpreter engine

  ----*/

main(int argc, char *argv[])
{
    int	   ev = 1, s, cs, n, co, o, l, bl = 256, argerr;
    char   *buf, sname[256];
    struct sockaddr_un name;
    Tcl_Interp *interp;
#if	PUBCOOKIE
    extern AUTHENTICATOR auth_gss_proxy;
#endif
    srandom(getpid() + time(0));

    /*----------------------------------------------------------------------
           Initialize c-client
      ----------------------------------------------------------------------*/

    /*
     * NO LOCAL DRIVERS ALLOWED
     * For this to change pintecld *MUST* be running under the user's UID and
     * and signal.[ch] need to get fixed to handle KOD rather than change
     * the debug level
     */
    mail_link (&imapdriver);		/* link in the imap driver */
    mail_link (&unixdriver);		/* link in the unix driver */
    mail_link (&dummydriver);		/* link in the dummy driver */

    /* link authentication drivers */
#if	PUBCOOKIE
    auth_link (&auth_gss_proxy);	/* pubcoookie proxy authenticator */
#endif
    auth_link (&auth_md5);		/* link in the md5 authenticator */
    auth_link (&auth_pla);
    auth_link (&auth_log);		/* link in the log authenticator */
    ssl_onceonlyinit ();

#if	PUBCOOKIE
    /* if REMOTE_USER set, use it as username */
    if(buf = getenv("REMOTE_USER"))
      env_init(buf, "/tmp");
#endif

    if(!mail_parameters(NULL, DISABLE_DRIVER, "unix")){
	fprintf(stderr, "Can't disable unix driver");
	exit(1);
    }


    /*----------------------------------------------------------------------
           Initialize pith library
      ----------------------------------------------------------------------*/
    setup_for_index_index_screen();


    /*----------------------------------------------------------------------
           Parse arguments
      ----------------------------------------------------------------------*/
    debug = 0;
    for(argerr = 0; !argerr && ((n = getopt(argc,argv,"d")) != -1); ) {
	switch(n) {
	  case 'd' : debug++; break;
	  case '?' : argerr = 1; break;
	}
    }

    if(argerr || optind != argc){
	char *p = strrchr(argv[0],'/');
	fprintf(stderr, "Usage: %s [-d]\n", p ? p + 1 : argv[0]);
	exit(1);
    }
sleep(10);

    /*----------------------------------------------------------------------
           Hop into the Tcl processing loop
      ----------------------------------------------------------------------*/

    buf = (char *) fs_get(bl * sizeof(char));

    if(fgets(sname, 255, stdin) && *sname){
	if(sname[l = strlen(sname) - 1] == '\n')
	  sname[l] = '\0';

	if((s = socket(AF_UNIX, SOCK_STREAM, 0)) != -1){

	    name.sun_family = AF_UNIX;
	    strcpy(name.sun_path, peSockName = sname);
	    l = sizeof(name);

	    if(bind(s, (struct sockaddr *) &name, l) == 0){
		if(listen(s, 5) == 0){
		    /*
		     * after the groundwork's done, go into the background.
		     * the fork saves the caller from invoking us in the background
		     * which introduces a timing race between the first client
		     * request arrival and our being prepared to accept it.
		     */
		    if(debug < 2){
			switch(fork()){
			  case -1 :		/* error */
			    perror("fork");
			    exit(1);

			  case 0 :		/* child */
			    close(0);		/* disassociate */
			    close(1);
			    close(2);
			    setpgrp(0, 0);
			    break;

			  default :		/* parent */
			    exit(0);
			}
		    }

		    debug_init();
		    dprint((SYSDBG_INFO, "started"));

		    interp = Tcl_CreateInterp();

		    PEInit(interp, sname);

		    while(1){
			struct timeval tv;
			fd_set	       rfd;

			FD_ZERO(&rfd);
			FD_SET(s, &rfd);
			tv.tv_sec = (peAbandonTimeout) ? peAbandonTimeout : peInputTimeout;
			tv.tv_usec = 0;
			if((n = select(s+1, &rfd, 0, 0, &tv)) > 0){

			    peAbandonTimeout = 0;

			    if((cs = accept(s, (struct sockaddr *) &name, &l)) == -1){
				dprint((SYSDBG_ERR, "accept failure: %s",
					error_description(errno)));
				break;
			    }

			    dprint((5, "accept success: %d", cs));

			    /*
			     * tcl commands are prefixed with a number representing
			     * the length of the command string and a newline character.
			     * the characters representing the length and the newline
			     * are not included in the command line length calculation.
			     */
			    o = co = 0;
			    while((n = read(cs, buf + o, bl - o - 1)) > 0){
				o += n;
				if(!co){
				    int i, x = 0;

				    for(i = 0; i < o; i++)
				      if(buf[i] == '\n'){
					  co = ++i;
					  l = x + co;
					  if(bl < l + 1){
					      bl = l + 1;
					      fs_resize((void **) &buf, bl * sizeof(char));
					  }

					  break;
				      }
				      else
					x = (x * 10) + (buf[i] - '0');
				}

				if(o && o == l)
				  break;
			    }

			    if(n == 0){
				dprint((SYSDBG_ERR, "read EOF"));
			    }
			    else if(n < 0){
				dprint((SYSDBG_ERR, "read failure: %s", error_description(errno)));
			    }
			    else{
				buf[o] = '\0';
#ifdef	DEBUG
				/* Log every Eval if somebody *really* wants to see it. */
				if(debug > 6){
				    char dbuf[5120];
				    int  dlim = (debug >= 9) ? 256 : 5120 - 32;

				    snprintf(dbuf, sizeof(dbuf), "Tcl_Eval(%.*s)", dlim, &buf[co]);

				    /* But DON'T log any clear-text credentials */
				    if(dbuf[9] == 'P'
				       && dbuf[10] == 'E'
				       && dbuf[11] == 'S'
				       && !strncmp(dbuf + 12, "ession creds ", 13)){
					char *p;

					for(p = &dbuf[25]; *p; p++)
					  *p = 'X';
				    }

				    dprint((1, dbuf));
				}
#endif

				switch(Tcl_Eval(interp, &buf[co])){
				  case TCL_OK	  : peReturn(cs, "OK", interp->result); break;
				  case TCL_ERROR  : peReturn(cs, "ERROR", interp->result); break;
				  case TCL_BREAK  : peReturn(cs, "BREAK", interp->result); break;
				  case TCL_RETURN : peReturn(cs, "RETURN", interp->result); break;
				  default	  : peReturn(cs, "BOGUS", "eval returned unexpected value"); break;
				}
			    }

			    close(cs);
			}
			else{
			    if(n < 0){
				dprint((SYSDBG_ALERT, "select failure: %s", error_description(errno)));
			    }
			    else{
				dprint((SYSDBG_INFO, "timeout after %d seconds", tv.tv_sec));
			    }

			    Tcl_Exit(0);

			    /* Tcl_Exit should never return. Getting here is an error. */
			    dprint((SYSDBG_ERR, "Tcl_Exit failure"));
			}
		    }
		}
		else
		  perror("listen");
	    }
	    else
	      perror("bind");

	    close(s);
	    unlink(sname);
	}
	else
	  perror("socket");
    }
    else
      fprintf(stderr, "Can't read socket name\n");

    exit(ev);
}


/*
 * peReturn - common routine to return TCL result
 */
void
peReturn(int sock, char *status, char *result)
{
    if(peWrite(sock, status))
      if(peWrite(sock, "\n"))
	peWrite(sock, result);
}

/*
 * peWrite - write all the given string on the given socket
 */
int
peWrite(int sock, char *s)
{
    int i, n;

    for(i = 0, n = strlen(s); n; n = n - i)
      if((i = write(sock, s + i, n)) < 0){
	  dprint((SYSDBG_ERR, "write: %s", error_description(errno)));
	  return(0);
      }

    return(1);  
}

/*
 * PEInit - Initialize exported TCL functions
 */
int
PEInit(Tcl_Interp *interp, char *sname)
{
    dprint((2, "PEInit: %s", sname));

    if(Tcl_Init(interp) == TCL_ERROR) {
	return(TCL_ERROR);
    }

    Tcl_CreateObjCommand(interp, "PEInfo", PEInfoCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEConfig", PEConfigCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEDebug", PEDebugCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PESession", PESessionCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEFolder", PEFolderCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEMailbox", PEMailboxCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEThread", PEThreadCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEMessage", PEMessageCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PECompose", PEComposeCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEPostpone", PEPostponeCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEAddress", PEAddressCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PEClist", PEClistCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand(interp, "PELdap", PELdapCmd,
			(ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateExitHandler(PEExitCleanup, sname);

#ifdef ENABLE_LDAP
    wpldap_global = (WPLDAP_S *)fs_get(sizeof(WPLDAP_S));
    wpldap_global->query_no = 0;
    wpldap_global->ldap_search_list = NULL;
#endif /* ENABLE_LDAP */

    return(TCL_OK);
}


void
PEExitCleanup(ClientData clientData)
{
    dprint((4, "PEExitCleanup"));

    if(ps_global){
	/* destroy any open stream */
	peDestroyStream(ps_global);
	
	/* destroy user context */
	peDestroyUserContext(&ps_global);
    }

#ifdef ENABLE_LDAP
    if(wpldap_global){
        if(wpldap_global->ldap_search_list)
	  free_wpldapres(wpldap_global->ldap_search_list);
	fs_give((void **)&wpldap_global);
    }
#endif /* ENABLE_LDAP */

    if((char *) clientData)
      unlink((char *) clientData);

    peFreeAttach(&peCompAttach);

    dprint((SYSDBG_INFO, "finished"));
}


/*
 * PEInfoCmd - export various bits of alpine state
 */
int
PEInfoCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err = "Unknown PEInfo request";

    dprint((2, "PEInfoCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else{
	char *s1 = Tcl_GetStringFromObj(objv[1], NULL);

	if(s1){
	    if(!strcmp(s1, "colorset")){
		char *varname, *fghex, *bghex;
		char  tvname[256], asciicolor[256];
		struct  variable *vtmp;
		Tcl_Obj **cObj;
		int       cObjc;
		SPEC_COLOR_S *hcolors, *thc;
		
		if(!(varname = Tcl_GetStringFromObj(objv[2], NULL))){
		    Tcl_SetResult(interp, "colorset: can't read variable name", TCL_STATIC);
		    return(TCL_ERROR);
		}

		if(!strcmp(varname, "viewer-hdr-colors")){
		    char *newhdr = NULL, *newpat = NULL, *utype;
		    int   hindex, i;

		    if(objc < 5){
			Tcl_SetResult(interp, "colorset: too few view-hdr args", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    hcolors = spec_colors_from_varlist(ps_global->VAR_VIEW_HDR_COLORS, 0);
		    if(!(utype = Tcl_GetStringFromObj(objv[3], NULL))){
			Tcl_SetResult(interp, "colorset: can't read operation", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(!strcmp(utype, "delete")){
			if(!hcolors){
			    Tcl_SetResult(interp, "colorset: no viewer-hdrs to delete", TCL_STATIC);
			    return(TCL_ERROR);
			}

			if(Tcl_GetIntFromObj(interp, objv[4], &hindex) == TCL_ERROR){
			    Tcl_SetResult(interp, "colorset: can't read index", TCL_STATIC);
			    return(TCL_ERROR);
			}

			if(hindex == 0){
			    thc = hcolors;
			    hcolors = hcolors->next;
			    thc->next = NULL;
			    free_spec_colors(&thc);
			}
			else{
			    /* zero based */
			    for(thc = hcolors, i = 1; thc && i < hindex; thc = thc->next, i++)
			      ;

			    if(thc && thc->next){
				SPEC_COLOR_S *thc2 = thc->next;

				thc->next = thc2->next;
				thc2->next = NULL;
				free_spec_colors(&thc2);
			    }
			    else{
				Tcl_SetResult(interp, "colorset: invalid index", TCL_STATIC);
				return(TCL_ERROR);
			    }
			}
		    }
		    else if(!strcmp(utype, "add")){
			if(objc != 6){
			    Tcl_SetResult(interp, "colorset: wrong number of view-hdr add args", TCL_STATIC);
			    return(TCL_ERROR);
			}

			if(Tcl_ListObjGetElements(interp, objv[4], &cObjc, &cObj) != TCL_OK)
			  return (TCL_ERROR);

			if(cObjc != 2){
			    Tcl_SetResult(interp, "colorset: wrong number of hdrs for view-hdr add", TCL_STATIC);
			    return(TCL_ERROR);
			}

			newhdr = Tcl_GetStringFromObj(cObj[0], NULL);
			newpat = Tcl_GetStringFromObj(cObj[1], NULL);
			if(Tcl_ListObjGetElements(interp, objv[5], &cObjc, &cObj) != TCL_OK)
			  return (TCL_ERROR);

			if(cObjc != 2){
			    Tcl_SetResult(interp, "colorset: wrong number of colors for view-hdr add", TCL_STATIC);
			    return(TCL_ERROR);
			}

			fghex = Tcl_GetStringFromObj(cObj[0], NULL);
			bghex = Tcl_GetStringFromObj(cObj[1], NULL);
			if(newhdr && newpat && fghex && bghex){
			    SPEC_COLOR_S **hcp;

			    for(hcp = &hcolors; *hcp != NULL; hcp = &(*hcp)->next)
			      ;

			    *hcp = (SPEC_COLOR_S *)fs_get(sizeof(SPEC_COLOR_S));
			    (*hcp)->inherit = 0;
			    (*hcp)->spec = cpystr(newhdr);
			    (*hcp)->fg = cpystr((ascii_colorstr(asciicolor, fghex) == 0) ? asciicolor : "black");
			    (*hcp)->bg = cpystr((ascii_colorstr(asciicolor, bghex) == 0) ? asciicolor : "white");

			    if(newpat && *newpat)
			      (*hcp)->val = string_to_pattern(newpat);
			    else
			      (*hcp)->val = NULL;

			    (*hcp)->next = NULL;
			}
			else{
			    Tcl_SetResult(interp, "colorset: invalid args for view-hdr add", TCL_STATIC);
			    return(TCL_ERROR);
			}
		    }
		    else if(!strcmp(utype, "update")){
			if(objc != 6){
			    Tcl_SetResult(interp, "colorset: wrong number of view-hdr update args", TCL_STATIC);
			    return(TCL_ERROR);
			}

			if(!(Tcl_ListObjGetElements(interp, objv[4], &cObjc, &cObj) == TCL_OK
			     && cObjc == 3
			     && Tcl_GetIntFromObj(interp, cObj[0], &hindex) == TCL_OK
			     && (newhdr = Tcl_GetStringFromObj(cObj[1], NULL))
			     && (newpat = Tcl_GetStringFromObj(cObj[2], NULL)))){
			    Tcl_SetResult(interp, "colorset: view-hdr update can't read index or header", TCL_STATIC);
			    return (TCL_ERROR);
			}

			if(!(Tcl_ListObjGetElements(interp, objv[5], &cObjc, &cObj) == TCL_OK
			     && cObjc == 2
			     && (fghex = Tcl_GetStringFromObj(cObj[0], NULL))
			     && (bghex = Tcl_GetStringFromObj(cObj[1], NULL)))){
			    Tcl_SetResult(interp, "colorset: view-hdr update can't read colors", TCL_STATIC);
			    return (TCL_ERROR);
			}

			for(thc = hcolors, i = 0; thc && i < hindex; thc = thc->next, i++)
			  ;

			if(!thc){
			    Tcl_SetResult(interp, "colorset: view-hdr update invalid index", TCL_STATIC);
			    return (TCL_ERROR);
			}

			if(thc->spec)
			  fs_give((void **)&thc->spec);

			thc->spec = cpystr(newhdr);
			if(ascii_colorstr(asciicolor, fghex) == 0) {
			    if(thc->fg)
			      fs_give((void **)&thc->fg);

			    thc->fg = cpystr(asciicolor);
			}
			else{
			    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid foreground color value %.100s", fghex);
			    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			    return(TCL_ERROR);
			}

			if(ascii_colorstr(asciicolor, bghex) == 0) {
			    if(thc->bg)
			      fs_give((void **)&thc->bg);

			    thc->bg = cpystr(asciicolor);
			}
			else{
			    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background color value %.100s", bghex);
			    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			    return(TCL_ERROR);
			}

			if(thc->val)
			  fs_give((void **)&thc->val);

			if(newpat && *newpat){
			    thc->val = string_to_pattern(newpat);
			}
		    }
		    else{
			Tcl_SetResult(interp, "colorset: unknown operation", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    vtmp = &ps_global->vars[V_VIEW_HDR_COLORS];
		    for(i = 0; vtmp->main_user_val.l && vtmp->main_user_val.l[i]; i++)
		      fs_give((void **)&vtmp->main_user_val.l[i]);

		    if(vtmp->main_user_val.l)
		      fs_give((void **)&vtmp->main_user_val.l);

		    vtmp->main_user_val.l = varlist_from_spec_colors(hcolors);
		    set_current_val(vtmp, FALSE, FALSE);
		    free_spec_colors(&hcolors);
		    return(TCL_OK);
		}
		else {
		    if(objc != 4){
			Tcl_SetResult(interp, "colorset: Wrong number of args", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(!(Tcl_ListObjGetElements(interp, objv[3], &cObjc, &cObj) == TCL_OK
			 && cObjc == 2
			 && (fghex = Tcl_GetStringFromObj(cObj[0], NULL))
			 && (bghex = Tcl_GetStringFromObj(cObj[1], NULL)))){
			Tcl_SetResult(interp, "colorset: Problem reading fore/back ground colors", TCL_STATIC);
			return (TCL_ERROR);
		    }

		    snprintf(tvname, sizeof(tvname), "%.200s-foreground-color", varname);
		    for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR];
			vtmp->name && strcmp(vtmp->name, tvname);
			vtmp++)
		      ;

		    if(!vtmp->name || vtmp->is_list){
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background var %.100s", varname);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    if(ascii_colorstr(asciicolor, fghex) == 0) {
			if(vtmp->main_user_val.p)
			  fs_give((void **)&vtmp->main_user_val.p);

			vtmp->main_user_val.p = cpystr(asciicolor);
			set_current_val(vtmp, FALSE, FALSE);
			if(!strucmp(varname, "normal"))
			  pico_set_fg_color(asciicolor);
		    }
		    else{
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid color value %.100s", fghex);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-background-color");
		    vtmp++;
		    if((vtmp->name && strcmp(vtmp->name, tvname)) || !vtmp->name)
		      for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR];
			  vtmp->name && strcmp(vtmp->name, tvname);
			  vtmp++)
			;

		    if(!vtmp->name || vtmp->is_list){
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background var %.100s", varname);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    if(ascii_colorstr(asciicolor, bghex) == 0) {
			if(vtmp->main_user_val.p)
			  fs_give((void **)&vtmp->main_user_val.p);

			vtmp->main_user_val.p = cpystr(asciicolor);
			set_current_val(vtmp, FALSE, FALSE);
			if(!strucmp(varname, "normal"))
			  pico_set_bg_color(asciicolor);
		    }
		    else{
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background color value %.100s", bghex);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    Tcl_SetResult(interp, "1", TCL_STATIC);
		    return(TCL_OK);
		}
	    }
	    else if(!strcmp(s1, "lappend")){
		if(objc >= 4){
		    Tcl_Obj *dObj;
		    int	     i;

		    if((dObj = Tcl_ObjGetVar2(interp, objv[2], NULL, TCL_LEAVE_ERR_MSG)) != NULL){
			for(i = 3; i < objc; i++)
			  if(Tcl_ListObjAppendElement(interp, dObj, objv[i]) != TCL_OK)
			    return(TCL_ERROR);

			if(i == objc){
			    return(TCL_OK);
			}
		    }
		    else
		      err = "PEInfo lappend: Unknown list name";
		}
		else
		  err = "PEInfo lappend: Too few args";
	    }
	    else if(objc == 2){
		if(!strcmp(s1, "version")){
		    char buf[256];
		    extern char compstamp[];

		    /*
		     * CMD: version
		     *
		     * Returns: string representing Pine version
		     * engine built on
		     */
		    snprintf(buf, sizeof(buf), "%s.%s", ALPINE_VERSION, compstamp);
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "key")){
		    static char key[64];

		    if(!key[0])
		      peRandomString(key,32,PRS_UPPER_CASE);

		    Tcl_SetResult(interp, key, TCL_STATIC);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "indexheight")){
		    Tcl_SetResult(interp, ps_global->VAR_WP_INDEXHEIGHT ?
				  ps_global->VAR_WP_INDEXHEIGHT : "", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "indexlines")){
		    Tcl_SetResult(interp, ps_global->VAR_WP_INDEXLINES ?
				  ps_global->VAR_WP_INDEXLINES : "0", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "aggtabstate")){
		    Tcl_SetResult(interp, ps_global->VAR_WP_AGGSTATE ?
				  ps_global->VAR_WP_AGGSTATE : "0", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "alpinestate")){
		    Tcl_SetResult(interp, ps_global->VAR_WP_STATE ?
				  ps_global->VAR_WP_STATE : "", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "foreground")){
		    char *color;

		    if(!((color = pico_get_last_fg_color())
			 && (color = color_to_asciirgb(color))
			 && (color = peColorStr(color,tmp_20k_buf))))
		      color = "000000";

		    Tcl_SetResult(interp, color, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "background")){
		    char *color;

		    if(!((color = pico_get_last_bg_color())
			 && (color = color_to_asciirgb(color))
			 && (color = peColorStr(color,tmp_20k_buf))))
		      color = "FFFFFF";

		    Tcl_SetResult(interp, color, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "flaglist")){
		    int	       i;
		    char      *p;
		    Tcl_Obj   *itemObj;

		    /*
		     * BUG: This list should get merged with the static list in "cmd_flag"
		     * and exported via some function similar to "feature_list()"
		     */
		    static char *flag_list[] = {
			"Important", "New", "Answered", "Deleted", NULL
		    };

		    /*
		     * CMD: flaglist
		     *
		     * Returns: list of FLAGS available for setting
		     */
		    for(i = 0; p = flag_list[i]; i++)
		      if(itemObj = Tcl_NewStringObj(p, -1)){
			  if(Tcl_ListObjAppendElement(interp,
						      Tcl_GetObjResult(interp),
						      itemObj) != TCL_OK)
			    ;
		      }
			
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "featurelist")){
		    int	       i;
		    char      *curfeature, *s;
		    FEATURE_S *feature;
		    Tcl_Obj   *itemObj, *secObj = NULL, *resObj = NULL;

		    /*
		     * CMD: featurelist
		     *
		     * Returns: list of FEATURES available for setting
		     */
		    for(i = 0, curfeature = NULL; feature = feature_list(i); i++)
		      if(s = feature_list_section(feature)){
			  if(!curfeature || strcmp(s, curfeature)){
			      if(resObj) {
				  Tcl_ListObjAppendElement(interp,
							   secObj,
							   resObj);
				  Tcl_ListObjAppendElement(interp,
							   Tcl_GetObjResult(interp),
							   secObj);
			      }

			      secObj = Tcl_NewListObj(0, NULL);
			      resObj = Tcl_NewListObj(0, NULL);
			      if(Tcl_ListObjAppendElement(interp,
							  secObj,
							  Tcl_NewStringObj(s,-1)) != TCL_OK)
				;

			      curfeature = s;
			  }

			  if(itemObj = Tcl_NewStringObj(feature->name, -1)){
			      if(Tcl_ListObjAppendElement(interp,
							  resObj,
							  itemObj) != TCL_OK)
				;
			  }
		      }

		    if(resObj){
			Tcl_ListObjAppendElement(interp, secObj, resObj);
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), secObj);
		    }

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "featuresettings")){
		    int	       i;
		    FEATURE_S *feature;
		    Tcl_Obj   *itemObj;

		    /*
		     * CMD: featuresettings
		     *
		     * Returns: list of FEATURES currently SET
		     */
		    for(i = 0; feature = feature_list(i); i++)
		      if(feature_list_section(feature)){
			  if(F_ON(feature->id, ps_global)){
			      if(itemObj = Tcl_NewStringObj(feature->name, -1)){
				  if(Tcl_ListObjAppendElement(interp,
							      Tcl_GetObjResult(interp),
							      itemObj) != TCL_OK)
				    ;
			      }
			  }
		      }

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "signature")){
		    char *sig;

		    if((ps_global->VAR_LITERAL_SIG
		       || (ps_global->VAR_SIGNATURE_FILE
			   && IS_REMOTE(ps_global->VAR_SIGNATURE_FILE)))
		       && (sig = detoken(NULL, NULL, 2, 0, 1, NULL, NULL))){
			char *p, *q;

			for(p = sig; q = strindex(p, '\n'); p = q + 1)
			  Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						   Tcl_NewStringObj(p, q - p));

			if(*p)
			  Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						   Tcl_NewStringObj(p, -1));

			fs_give((void **) &sig);
		    }
		    else
		      Tcl_SetResult(interp, "", TCL_STATIC);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "rawsig")){
		    char *err = NULL, *sig = NULL, *p, *q;

		    if(ps_global->VAR_LITERAL_SIG){
			char     *err = NULL;
			char    **apval;

			if(ps_global->restricted){
			    err = "Pine demo can't change config file";
			}
			else{
			    /* BUG: no "exceptions file" support */
			    if(apval = APVAL(&ps_global->vars[V_LITERAL_SIG], Main)){
				sig = (char *) fs_get((strlen(*apval ? *apval : "") + 1) * sizeof(char));
				sig[0] = '\0';
				cstring_to_string(*apval, sig);
			    }
			    else
			      err = "Problem accessing configuration";
			}
		    }
		    else if(!IS_REMOTE(ps_global->VAR_SIGNATURE_FILE))
		      snprintf(err = tmp_20k_buf, SIZEOF_20KBUF, "Non-Remote signature file: %s",
			      ps_global->VAR_SIGNATURE_FILE ? ps_global->VAR_SIGNATURE_FILE : "<null>");
		    else if(!(sig = read_remote_sigfile(ps_global->VAR_SIGNATURE_FILE)))
		      err = "Can't read remote pinerc";

		    if(err){
			Tcl_SetResult(interp, err, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    for(p = sig; q = strindex(p, '\n'); p = q + 1)
		      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					       Tcl_NewStringObj(p, q - p));

		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(p, -1));
		    fs_give((void **) &sig);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "statmsg")){
		    char *s = sml_getmsg();
		    /* BUG: can this be removed? */

		    Tcl_SetResult(interp, s ? s : "", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "statmsgs")){
		    char **s = sml_getmsgs();
		    char **tmps;

		    for(tmps = s; tmps && *tmps; tmps++)
		      Tcl_ListObjAppendElement(interp,
					       Tcl_GetObjResult(interp),
					       Tcl_NewStringObj(*tmps, -1));
		    fs_give((void **)&s);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "saveconf")){
		    write_pinerc(ps_global, Main, WRP_NOUSER);
		    return(TCL_OK);
		}
		else if(!strucmp(s1, "sort")){
		    return(peAppendDefaultSort(interp));
		}
		else if(!strcmp(s1, "ldapenabled")){
		    /*
		     * CMD: ldapenabled
		     *
		     * Returns: 1 if enabled 0 if not
		     */
#ifdef ENABLE_LDAP
		    Tcl_SetResult(interp, "1", TCL_VOLATILE);
#else
		    Tcl_SetResult(interp, "0", TCL_VOLATILE);
#endif

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "prunecheck")){
		    time_t  now;
		    struct tm *tm_now;
		    char    tmp[50];

		    if(!check_prune_time(&now, &tm_now)){
		        Tcl_SetResult(interp, "0", TCL_VOLATILE);
			return(TCL_OK);
		    } else {
		      /*
		       * We're going to reset the last-time-pruned variable
		       * so that it asks a maximum of 1 time per month.
		       * PROs: Annoying-factor is at its lowest
		       *       Can go ahead and move folders right away if
		       *         pruning-rule is automatically set to do so
		       * CONs: Annoying-factor is at its lowest, if it's set
		       *         later then we can ensure that the questions
		       *         actually get answered or it will keep asking
		       */
		      ps_global->last_expire_year = tm_now->tm_year;
		      ps_global->last_expire_month = tm_now->tm_mon;
		      snprintf(tmp, sizeof(tmp), "%d.%d", ps_global->last_expire_year,
			      ps_global->last_expire_month + 1);
		      set_variable(V_LAST_TIME_PRUNE_QUESTION, tmp, 0, 1, Main);

		      Tcl_SetResult(interp, "1", TCL_VOLATILE);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "prunetime")){
		    time_t	 now;
		    struct tm	*tm_now;
		    CONTEXT_S   *prune_cntxt;
		    Tcl_Obj     *retObj = NULL;
		    int          cur_month, ok = 1;
		    char       **p;
		    static int   moved_fldrs = 0;

		    now = time((time_t *)0);
		    tm_now = localtime(&now);
		    cur_month = (1900 + tm_now->tm_year) * 12 + tm_now->tm_mon;

		    if(!(prune_cntxt = default_save_context(ps_global->context_list)))
		      prune_cntxt = ps_global->context_list;

		    if(prune_cntxt){
		        if(ps_global->VAR_DEFAULT_FCC && *ps_global->VAR_DEFAULT_FCC
			   && context_isambig(ps_global->VAR_DEFAULT_FCC))
			    if(retObj = wp_prune_folders(prune_cntxt,
							 ps_global->VAR_DEFAULT_FCC,
							 cur_month, "sent",
							 ps_global->pruning_rule, &ok,
							 moved_fldrs, interp))
			        Tcl_ListObjAppendElement(interp,
							 Tcl_GetObjResult(interp),
							 retObj);

			if(ok && ps_global->VAR_READ_MESSAGE_FOLDER 
			   && *ps_global->VAR_READ_MESSAGE_FOLDER
			   && context_isambig(ps_global->VAR_READ_MESSAGE_FOLDER))
			    if(retObj = wp_prune_folders(prune_cntxt,
							 ps_global->VAR_READ_MESSAGE_FOLDER,
							 cur_month, "read",
							 ps_global->pruning_rule, &ok,
							 moved_fldrs, interp))
			        Tcl_ListObjAppendElement(interp,
							 Tcl_GetObjResult(interp),
							 retObj);
			if(ok && (p = ps_global->VAR_PRUNED_FOLDERS)){
			    for(; ok && *p; p++)
			      if(**p && context_isambig(*p))
				    if(retObj = wp_prune_folders(prune_cntxt,
							   *p, cur_month, "", 
							    ps_global->pruning_rule, &ok,
							    moved_fldrs, interp))
				        Tcl_ListObjAppendElement(interp,
								 Tcl_GetObjResult(interp),
								 retObj);
			}
		    }
		    moved_fldrs = 1;
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "authrequestor")){
		    Tcl_SetResult(interp, peCredentialRequestor, TCL_STATIC);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "noop")){
		    /* tickle the imap server too */
		    if(ps_global->mail_stream)
		      pine_mail_ping(ps_global->mail_stream);

		    Tcl_SetResult(interp, "NOOP", TCL_STATIC);
		    return(TCL_OK);
		}
	    }
	    else if(objc == 3){
		if(!strcmp(s1, "feature")){
		    char      *featurename;
		    int	       i, isset = 0;
		    FEATURE_S *feature;

		    /*
		     * CMD: feature
		     *
		     * ARGS: featurename - 
		     *
		     * Returns: 1 if named feature set, 0 otherwise
		     *          
		     */
		    if(featurename = Tcl_GetStringFromObj(objv[2], NULL))
		      for(i = 0; feature = feature_list(i); i++)
			if(!strcmp(featurename, feature->name)){
			    isset = F_ON(feature->id, ps_global);
			    break;
			}

		    Tcl_SetResult(interp, int2string(isset), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "colorget")){
		    char             *varname;
		    char              tvname[256], hexcolor[256];
		    struct  variable *vtmp;
		    if(!(varname = Tcl_GetStringFromObj(objv[2], NULL))){
		      return(TCL_ERROR);
		    }
		    if(strcmp("viewer-hdr-colors", varname) == 0){
			SPEC_COLOR_S *hcolors, *thc;
			Tcl_Obj     *resObj;
			char         hexcolor[256], *tstr = NULL;

			hcolors = spec_colors_from_varlist(ps_global->VAR_VIEW_HDR_COLORS, 0);
			for(thc = hcolors; thc; thc = thc->next){
			    resObj = Tcl_NewListObj(0,NULL);
			    Tcl_ListObjAppendElement(interp, resObj,
						     Tcl_NewStringObj(thc->spec, -1));
			    hex_colorstr(hexcolor, thc->fg);
			    Tcl_ListObjAppendElement(interp, resObj,
						     Tcl_NewStringObj(hexcolor, -1));
			    hex_colorstr(hexcolor, thc->bg);
			    Tcl_ListObjAppendElement(interp, resObj,
						     Tcl_NewStringObj(hexcolor, -1));
			    Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(thc->val 
					? tstr = pattern_to_string(thc->val) 
							 : "", -1));
			    if(tstr) fs_give((void **)&tstr);
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     resObj);
			}
			fs_give((void **)&hcolors);
			return(TCL_OK);
		    }
		    else {
			snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-foreground-color");
			for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR]; vtmp->name
			      && strcmp(vtmp->name, tvname); vtmp++);
			if(!vtmp->name) return(TCL_ERROR);
			if(vtmp->is_list) return(TCL_ERROR);
			if(!vtmp->current_val.p)
			  Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						   Tcl_NewStringObj("", -1));
			else{
			    hex_colorstr(hexcolor, vtmp->current_val.p);
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(hexcolor, -1));
			}
			snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-background-color");
			vtmp++;
			if((vtmp->name && strcmp(vtmp->name, tvname)) || !vtmp->name)
			  for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR]; vtmp->name
				&& strcmp(vtmp->name, tvname); vtmp++);
			if(!vtmp->name) return(TCL_ERROR);
			if(vtmp->is_list) return(TCL_ERROR);
			if(!vtmp->current_val.p)
			  Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						   Tcl_NewStringObj("", -1));
			else{
			    hex_colorstr(hexcolor, vtmp->current_val.p);
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(hexcolor, -1));
			}
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "varget")){
		    struct     variable *vtmp;
		    Tcl_Obj   *itemObj, *resObj, *secObj;
		    char      *vallist, *varname, tmperrmsg[256];
		    int        i;
		    NAMEVAL_S *tmpnv;

		    /*
		     * CMD: varget
		     *
		     * Returns: get the values for the requested variable
		     *
		     * The list returned follows this general form:
		     *
		     * char *;  variable name
		     * char **;  list of set values
		     * char *;  display type (listbox, text, textarea, ...)
		     * char **; list of possible values
		     *            (so far this is only useful for listboxes)
		     */
		    if(!(varname = Tcl_GetStringFromObj(objv[2], NULL))){
		      Tcl_SetResult(interp, "Can't Tcl_GetStringFromObj", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    for(vtmp = ps_global->vars; vtmp->name && 
			  strcmp(vtmp->name, varname); vtmp++);
		    if(!vtmp->name){
		      snprintf(tmperrmsg, sizeof(tmperrmsg), "Can't find variable named %s", 
			      strlen(varname) < 200 ? varname : "");
		      Tcl_SetResult(interp, tmperrmsg, TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    if(itemObj = Tcl_NewStringObj(vtmp->name, -1)){
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp),
						 itemObj);
			resObj = Tcl_NewListObj(0, NULL);
			if(vtmp->is_list){
			    for(i = 0 ; vtmp->current_val.l && vtmp->current_val.l[i]; i++){
			        vallist = vtmp->current_val.l[i];
				if(*(vallist))
				  itemObj = Tcl_NewStringObj(vallist, -1);
				else
				  itemObj = Tcl_NewStringObj("", -1);
				Tcl_ListObjAppendElement(interp, resObj, itemObj);
			    }
			}
			else{
			  itemObj = Tcl_NewStringObj(vtmp->current_val.p ? 
						     vtmp->current_val.p : "", -1);
			  Tcl_ListObjAppendElement(interp, resObj, itemObj);
			}
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp),
						 resObj);
			secObj = Tcl_NewListObj(0, NULL);
			if(vtmp->is_list)
			  itemObj = Tcl_NewStringObj("textarea", -1);
			else{
			  NAMEVAL_S *(*tmpf)(int);
			  switch(vtmp - ps_global->vars){
			    case V_SAVED_MSG_NAME_RULE:
			      tmpf = save_msg_rules;
			      break;
			    case V_FCC_RULE:
			      tmpf = fcc_rules;
			      break;
			    case V_SORT_KEY:
			      tmpf = sort_key_rules;
			      break;
			    case V_AB_SORT_RULE:
			      tmpf = ab_sort_rules;
			      break;
			    case V_FLD_SORT_RULE:
			      tmpf = fld_sort_rules;
			      break;
			    case V_GOTO_DEFAULT_RULE:
			      tmpf = goto_rules;
			      break;
			    case V_INCOMING_STARTUP:
			      tmpf = incoming_startup_rules;
			      break;
			    case V_PRUNING_RULE:
			      tmpf = pruning_rules;
			      break;
			    case V_WP_INDEXHEIGHT:
			      tmpf = wp_indexheight_rules;
			      break;
			    default:
			      tmpf = NULL;
			      break;
			  }
			  if(tmpf){
			    for(i = 0; tmpnv = (tmpf)(i); i++){
			      itemObj = Tcl_NewListObj(0, NULL);
			      Tcl_ListObjAppendElement(interp, itemObj,
						       Tcl_NewStringObj(tmpnv->name, -1));
			      if(tmpnv->shortname)
				Tcl_ListObjAppendElement(interp, itemObj,
					     Tcl_NewStringObj(tmpnv->shortname, -1));
			      Tcl_ListObjAppendElement(interp, secObj, itemObj);
			    }
			    itemObj = Tcl_NewStringObj("listbox", -1);
			  }
			  else
			    itemObj = Tcl_NewStringObj("text", -1);
			}
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp), 
						 itemObj);
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp), 
						 secObj);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "expungecheck")) {
		  /* 
		   * Return open folders and how many deleted messages they have
		   *
		   * return looks something like a list of these:
		   * {folder-name number-deleted isinbox isincoming}
		   */
		    char *type;
		    long delete_count;
		    Tcl_Obj *resObj;

		    type = Tcl_GetStringFromObj(objv[2], NULL);
		    if(!type) return(TCL_ERROR);
		    if(strcmp(type, "current") != 0 && strcmp(type, "quit") != 0)
		      return(TCL_ERROR);
		    if(ps_global->mail_stream != sp_inbox_stream()
		       || strcmp(type, "current") == 0){
		        delete_count = count_flagged(ps_global->mail_stream, F_DEL);
			resObj = Tcl_NewListObj(0, NULL);
			Tcl_ListObjAppendElement(interp, resObj, 
				 Tcl_NewStringObj(pretty_fn(ps_global->cur_folder), -1));
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewIntObj(delete_count));
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewIntObj((ps_global->mail_stream 
							    == sp_inbox_stream())
							       ? 1 : 0));
			Tcl_ListObjAppendElement(interp, resObj, 
			    Tcl_NewIntObj((ps_global->context_current->use & CNTXT_INCMNG)
					  ? 1 : 0));
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 resObj);
		    }
		    if(strcmp(type, "quit") == 0){
		        delete_count = count_flagged(sp_inbox_stream(), F_DEL);
			resObj = Tcl_NewListObj(0, NULL);
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewStringObj("INBOX", -1));
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewIntObj(delete_count));
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewIntObj(1));
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewIntObj(1));
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp), resObj);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "rawsig")){

		    if(ps_global->VAR_LITERAL_SIG){
			char	 *cstring_version, *sig, *line;
			int	  i, nSig;
			Tcl_Obj **objSig;

			tmp_20k_buf[0] = '\0';
			Tcl_ListObjGetElements(interp, objv[2], &nSig, &objSig);
			for(i = 0; i < nSig && i < SIG_MAX_LINES; i++)
			  if(line = Tcl_GetStringFromObj(objSig[i], NULL))
			    snprintf(tmp_20k_buf + strlen(tmp_20k_buf), SIZEOF_20KBUF - strlen(tmp_20k_buf), "%.*s\n", SIG_MAX_COLS, line);

			sig = cpystr(tmp_20k_buf);

			if(cstring_version = string_to_cstring(sig)){
			    set_variable(V_LITERAL_SIG, cstring_version, 0, 0, Main);
			    fs_give((void **)&cstring_version);
			}

			fs_give((void **) &sig);
			return(TCL_OK);
		    }
		    else
		      return(peWriteSig(interp, ps_global->VAR_SIGNATURE_FILE,
					&((Tcl_Obj **)objv)[2]));
		}
		else if(!strcmp(s1, "statmsg")){
		    char *msg;

		    /*
		     * CMD: statmsg
		     *
		     * ARGS: msg - text to set
		     *
		     * Returns: nothing, but with global status message
		     *		buf set to given msg
		     *          
		     */
		    if(msg = Tcl_GetStringFromObj(objv[2], NULL))
		      sml_addmsg(0, msg);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "mode")){
		    char *mode;
		    int	  rv = 0;

		    /*
		     * CMD: mode
		     *
		     * ARGS: <mode>
		     *
		     * Returns: return value of given binary mode
		     *          
		     */
		    if(mode = Tcl_GetStringFromObj(objv[2], NULL)){
			if(!strcmp(mode, "full-header-mode"))
			  rv = ps_global->full_header;
		    }

		    Tcl_SetResult(interp, int2string(rv), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "indexlines")){
		    int   n;
		    char *p;

		    if(Tcl_GetIntFromObj(interp, objv[2], &n) == TCL_OK){
			set_variable(V_WP_INDEXLINES, p = int2string(n), 0, 0, Main);
			Tcl_SetResult(interp, p, TCL_VOLATILE);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "aggtabstate")){
		    int   n;
		    char *p;

		    if(Tcl_GetIntFromObj(interp, objv[2], &n) == TCL_OK){
			set_variable(V_WP_AGGSTATE, p = int2string(n), 0, 0, Main);
			Tcl_SetResult(interp, p, TCL_VOLATILE);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "alpinestate")){
		    int   n;
		    char *p;

		    if((p = Tcl_GetStringFromObj(objv[2], NULL)) != NULL){
			set_variable(V_WP_STATE, p, 0, 1, Main);
			Tcl_SetResult(interp, p, TCL_VOLATILE);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "set")){
		    Tcl_Obj *rObj;

		    if((rObj = Tcl_ObjGetVar2(interp, objv[2], NULL, TCL_LEAVE_ERR_MSG)) != NULL){
			Tcl_SetObjResult(interp, rObj);
			return(TCL_OK);
		    }
		    else
		      return(TCL_ERROR);
		}
		else if(!strcmp(s1, "unset")){
		    char *varname;

		    return((varname = Tcl_GetStringFromObj(objv[2], NULL)) ? Tcl_UnsetVar2(interp, varname, NULL, TCL_LEAVE_ERR_MSG) : TCL_ERROR);
		}
	    }
	    else if(objc == 4){
		if(!strcmp(s1, "feature")){
		    char      *featurename;
		    int	       i, set, wasset = 0;
		    FEATURE_S *feature;

		    /*
		     * CMD: feature
		     *
		     * ARGS: featurename -
		     *	     value - new value to assign flag
		     *
		     * Returns: 1 if named feature set, 0 otherwise
		     *          
		     */
		    if((featurename = Tcl_GetStringFromObj(objv[2], NULL))
		       && Tcl_GetIntFromObj(interp, objv[3], &set) != TCL_ERROR)
		      for(i = 0; feature = feature_list(i); i++)
			if(!strcmp(featurename, feature->name)){
			    if(set != F_ON(feature->id, ps_global)){
				toggle_feature(ps_global,
					       &ps_global->vars[V_FEATURE_LIST],
					       feature, TRUE, Main);

				if(ps_global->prc)
				  ps_global->prc->outstanding_pinerc_changes = 1;
			    }

			    break;
			}

		    Tcl_SetResult(interp, int2string(wasset), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strucmp(s1, "help")){
		    HelpType   text;
		    int        i;
		    char     **help_text, **ptext, *helpname, tmperrmsg[256],
		              *function;
		    Tcl_Obj   *itemObj;
		    struct     variable *vtmp;
		    FEATURE_S *ftmp;

		    if(!(helpname = Tcl_GetStringFromObj(objv[2], NULL))){
		      Tcl_SetResult(interp,
				    "Can't Tcl_GetStringFromObj for helpname", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    if(!(function = Tcl_GetStringFromObj(objv[3], NULL))){
		      Tcl_SetResult(interp,
				    "Can't Tcl_GetStringFromObj for function", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    if(strucmp(function, "plain") == 0){
		      if((text = help_name2section(helpname, strlen(helpname)))
			 == NO_HELP)
			return(TCL_OK);
		    }
		    else if(strucmp(function, "variable") == 0){
		        for(vtmp = ps_global->vars; vtmp->name && 
			      strcmp(vtmp->name, helpname); vtmp++);
			if(!vtmp->name) {
			  snprintf(tmperrmsg, sizeof(tmperrmsg), "Can't find variable named %s", 
				  strlen(helpname) < 200 ? helpname : "");
			  Tcl_SetResult(interp, tmperrmsg, TCL_VOLATILE);
			  return(TCL_ERROR);
			}
			text = config_help(vtmp - ps_global->vars, 0);
			if(text == NO_HELP)
			  return(TCL_OK);		      
		    }
		    else if(strucmp(function, "feature") == 0){
		        for(i = 0; ftmp = feature_list(i); i++){
			    if(!strcmp(helpname, ftmp->name)){
			      text = ftmp->help;
			      break;
			    }
			}
			if(!ftmp || text == NO_HELP){
			  return(TCL_OK);
			}
		    }
		    else {
		      snprintf(tmperrmsg, sizeof(tmperrmsg), "Invalid function: %s", 
			      strlen(helpname) < 200 ? function : "");
		      Tcl_SetResult(interp, tmperrmsg, TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
#ifdef HELPFILE
		    if((help_text = get_help_text(text)) == NULL)
		      return(TCL_OK);
#else
		    help_text = text;
#endif
		    for(ptext = help_text; *ptext; ptext++){
		      itemObj = Tcl_NewStringObj(*ptext, -1);
		      Tcl_ListObjAppendElement(interp, 
					       Tcl_GetObjResult(interp),
					       itemObj);
		    }
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "varset")){
		    char     *varname, **tmpstrlist, *line;
		    struct    variable *vtmp;
		    Tcl_Obj **objVal;
		    int       i, numlistvals = 0, strlistpos;

		    if((varname = Tcl_GetStringFromObj(objv[2], NULL))
		       && (Tcl_ListObjGetElements(interp, objv[3], &numlistvals, 
							   &objVal) == TCL_OK)){
		        for(vtmp = ps_global->vars; vtmp->name && strcmp(vtmp->name, varname); 
			    vtmp++);
			if(!vtmp->name){
			  return(TCL_ERROR);
			}
			else{
			    /* found the variable */
			    if(vtmp->is_list){
				for(i = 0; vtmp->main_user_val.l && vtmp->main_user_val.l[i]; i++)
				  fs_give((void **)&vtmp->main_user_val.l[i]);
				if(vtmp->main_user_val.l)
				  fs_give((void **)&vtmp->main_user_val.l);
				if(numlistvals > 0){
				    tmpstrlist = (char **)fs_get((numlistvals + 1) * sizeof(char *));
				    for(i = 0, strlistpos = 0; i < numlistvals; i++){
				        if(line = Tcl_GetStringFromObj(objVal[i], 0)){
					  removing_leading_and_trailing_white_space(line);
					  if(*line)
					    tmpstrlist[strlistpos++] = cpystr(line);
					}
				    }
				    tmpstrlist[strlistpos] = NULL;
				    vtmp->main_user_val.l = (char **)fs_get((strlistpos+1) *
									    sizeof(char *));
				    for(i = 0; i <= strlistpos; i++)
				      vtmp->main_user_val.l[i] = tmpstrlist[i];
				    fs_give((void **)&tmpstrlist);
				}
				set_current_val(vtmp, FALSE, FALSE);
				return(TCL_OK);
			    }
			    else{
			        if(line = Tcl_GetStringFromObj(objVal[0], NULL)){
				    if(strcmp(vtmp->name, "reply-indent-string"))
				      removing_leading_and_trailing_white_space(line);
				    if(vtmp->main_user_val.p)
				      fs_give((void **)&vtmp->main_user_val.p);
				    if(*line)
				      vtmp->main_user_val.p = cpystr(line);
				    set_current_val(vtmp, FALSE, FALSE);
				    return(TCL_OK);
				}
			    }
			}
		    }
		    return(TCL_ERROR);
		}
		else if(!strcmp(s1, "mode")){
		    char *mode;
		    int	  value, rv = 0;

		    /*
		     * CMD: mode
		     *
		     * ARGS:  <mode> <value>
		     *
		     * Returns: old value of binary mode we were told to set
		     *          
		     */
		    if((mode = Tcl_GetStringFromObj(objv[2], NULL))
		       && Tcl_GetIntFromObj(interp, objv[3], &value) != TCL_ERROR){
			if(!strcmp(mode, "full-header-mode")){
			    rv = ps_global->full_header;
			    ps_global->full_header = value;
			}
		    }

		    Tcl_SetResult(interp, int2string(rv), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "set")){
		    Tcl_Obj *rObj;

		    if((rObj = Tcl_ObjSetVar2(interp, objv[2], NULL, objv[3], TCL_LEAVE_ERR_MSG)) != NULL){
			Tcl_SetObjResult(interp, rObj);
			return(TCL_OK);
		    }
		    else
		      return(TCL_ERROR);
		}
	    }
	    else
	      err = "PEInfo: Too many arguments";
	}
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


/*
 * PEConfigCmd - edit various alpine config variables
 *
 * The goal here is to remember what's changed, but not write to pinerc
 * until the user's actually chosen to save.
 */
int
PEConfigCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err = "Unknown PEConfig request";
    char *s1;

    dprint((2, "PEConfigCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
	Tcl_SetResult(interp, err, TCL_STATIC);
	return(TCL_ERROR);
    }
    s1 = Tcl_GetStringFromObj(objv[1], NULL);

    if(s1){
	if(!strcmp(s1, "colorset")){
	    char *varname, *fghex, *bghex;
	    char  tvname[256], asciicolor[256];
	    struct  variable *vtmp;
	    Tcl_Obj **cObj;
	    int       cObjc;
	    SPEC_COLOR_S *hcolors, *thc;
		
	    if(!(varname = Tcl_GetStringFromObj(objv[2], NULL))){
		Tcl_SetResult(interp, "colorset: can't read variable name", TCL_STATIC);
		return(TCL_ERROR);
	    }

	    if(!strcmp(varname, "viewer-hdr-colors")){
		char *newhdr = NULL, *newpat = NULL, *utype;
		int   hindex, i;

		if(objc < 5){
		    Tcl_SetResult(interp, "colorset: too few view-hdr args", TCL_STATIC);
		    return(TCL_ERROR);
		}

		if(ps_global->vars[V_VIEW_HDR_COLORS].is_changed_val)
		  hcolors = spec_colors_from_varlist(ps_global->vars[V_VIEW_HDR_COLORS].changed_val.l, 0);
		else
		  hcolors = spec_colors_from_varlist(ps_global->VAR_VIEW_HDR_COLORS, 0);
		if(!(utype = Tcl_GetStringFromObj(objv[3], NULL))){
		    Tcl_SetResult(interp, "colorset: can't read operation", TCL_STATIC);
		    return(TCL_ERROR);
		}

		if(!strcmp(utype, "delete")){
		    if(!hcolors){
			Tcl_SetResult(interp, "colorset: no viewer-hdrs to delete", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(Tcl_GetIntFromObj(interp, objv[4], &hindex) == TCL_ERROR){
			Tcl_SetResult(interp, "colorset: can't read index", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(hindex == 0){
			thc = hcolors;
			hcolors = hcolors->next;
			thc->next = NULL;
			free_spec_colors(&thc);
		    }
		    else{
			/* zero based */
			for(thc = hcolors, i = 1; thc && i < hindex; thc = thc->next, i++)
			  ;

			if(thc && thc->next){
			    SPEC_COLOR_S *thc2 = thc->next;

			    thc->next = thc2->next;
			    thc2->next = NULL;
			    free_spec_colors(&thc2);
			}
			else{
			    Tcl_SetResult(interp, "colorset: invalid index", TCL_STATIC);
			    return(TCL_ERROR);
			}
		    }
		}
		else if(!strcmp(utype, "add")){
		    if(objc != 6){
			Tcl_SetResult(interp, "colorset: wrong number of view-hdr add args", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(Tcl_ListObjGetElements(interp, objv[4], &cObjc, &cObj) != TCL_OK)
		      return (TCL_ERROR);

		    if(cObjc != 2){
			Tcl_SetResult(interp, "colorset: wrong number of hdrs for view-hdr add", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    newhdr = Tcl_GetStringFromObj(cObj[0], NULL);
		    newpat = Tcl_GetStringFromObj(cObj[1], NULL);
		    if(Tcl_ListObjGetElements(interp, objv[5], &cObjc, &cObj) != TCL_OK)
		      return (TCL_ERROR);

		    if(cObjc != 2){
			Tcl_SetResult(interp, "colorset: wrong number of colors for view-hdr add", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    fghex = Tcl_GetStringFromObj(cObj[0], NULL);
		    bghex = Tcl_GetStringFromObj(cObj[1], NULL);
		    if(newhdr && newpat && fghex && bghex){
			SPEC_COLOR_S **hcp;

			for(hcp = &hcolors; *hcp != NULL; hcp = &(*hcp)->next)
			  ;

			*hcp = (SPEC_COLOR_S *)fs_get(sizeof(SPEC_COLOR_S));
			(*hcp)->inherit = 0;
			(*hcp)->spec = cpystr(newhdr);
			(*hcp)->fg = cpystr((ascii_colorstr(asciicolor, fghex) == 0) ? asciicolor : "black");
			(*hcp)->bg = cpystr((ascii_colorstr(asciicolor, bghex) == 0) ? asciicolor : "white");

			if(newpat && *newpat)
			  (*hcp)->val = string_to_pattern(newpat);
			else
			  (*hcp)->val = NULL;

			(*hcp)->next = NULL;
		    }
		    else{
			Tcl_SetResult(interp, "colorset: invalid args for view-hdr add", TCL_STATIC);
			return(TCL_ERROR);
		    }
		}
		else if(!strcmp(utype, "update")){
		    if(objc != 6){
			Tcl_SetResult(interp, "colorset: wrong number of view-hdr update args", TCL_STATIC);
			return(TCL_ERROR);
		    }

		    if(!(Tcl_ListObjGetElements(interp, objv[4], &cObjc, &cObj) == TCL_OK
			 && cObjc == 3
			 && Tcl_GetIntFromObj(interp, cObj[0], &hindex) == TCL_OK
			 && (newhdr = Tcl_GetStringFromObj(cObj[1], NULL))
			 && (newpat = Tcl_GetStringFromObj(cObj[2], NULL)))){
			Tcl_SetResult(interp, "colorset: view-hdr update can't read index or header", TCL_STATIC);
			return (TCL_ERROR);
		    }

		    if(!(Tcl_ListObjGetElements(interp, objv[5], &cObjc, &cObj) == TCL_OK
			 && cObjc == 2
			 && (fghex = Tcl_GetStringFromObj(cObj[0], NULL))
			 && (bghex = Tcl_GetStringFromObj(cObj[1], NULL)))){
			Tcl_SetResult(interp, "colorset: view-hdr update can't read colors", TCL_STATIC);
			return (TCL_ERROR);
		    }

		    for(thc = hcolors, i = 0; thc && i < hindex; thc = thc->next, i++)
		      ;

		    if(!thc){
			Tcl_SetResult(interp, "colorset: view-hdr update invalid index", TCL_STATIC);
			return (TCL_ERROR);
		    }

		    if(thc->spec)
		      fs_give((void **)&thc->spec);

		    thc->spec = cpystr(newhdr);
		    if(ascii_colorstr(asciicolor, fghex) == 0) {
			if(thc->fg)
			  fs_give((void **)&thc->fg);

			thc->fg = cpystr(asciicolor);
		    }
		    else{
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid foreground color value %.100s", fghex);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    if(ascii_colorstr(asciicolor, bghex) == 0) {
			if(thc->bg)
			  fs_give((void **)&thc->bg);

			thc->bg = cpystr(asciicolor);
		    }
		    else{
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background color value %.100s", bghex);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_ERROR);
		    }

		    if(thc->val)
		      fs_give((void **)&thc->val);

		    if(newpat && *newpat){
			thc->val = string_to_pattern(newpat);
		    }
		}
		else{
		    Tcl_SetResult(interp, "colorset: unknown operation", TCL_STATIC);
		    return(TCL_ERROR);
		}

		vtmp = &ps_global->vars[V_VIEW_HDR_COLORS];
		for(i = 0; vtmp->changed_val.l && vtmp->changed_val.l[i]; i++)
		  fs_give((void **)&vtmp->changed_val.l[i]);

		if(vtmp->changed_val.l)
		  fs_give((void **)&vtmp->changed_val.l);

		vtmp->changed_val.l = varlist_from_spec_colors(hcolors);
		vtmp->is_changed_val = 1;
		free_spec_colors(&hcolors);
		return(TCL_OK);
	    }
	    else {
		if(objc != 4){
		    Tcl_SetResult(interp, "colorset: Wrong number of args", TCL_STATIC);
		    return(TCL_ERROR);
		}

		if(!(Tcl_ListObjGetElements(interp, objv[3], &cObjc, &cObj) == TCL_OK
		     && cObjc == 2
		     && (fghex = Tcl_GetStringFromObj(cObj[0], NULL))
		     && (bghex = Tcl_GetStringFromObj(cObj[1], NULL)))){
		    Tcl_SetResult(interp, "colorset: Problem reading fore/back ground colors", TCL_STATIC);
		    return (TCL_ERROR);
		}

		snprintf(tvname, sizeof(tvname), "%.200s-foreground-color", varname);
		for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR];
		    vtmp->name && strcmp(vtmp->name, tvname);
		    vtmp++)
		  ;

		if(!vtmp->name || vtmp->is_list){
		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background var %.100s", varname);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}

		if(ascii_colorstr(asciicolor, fghex) == 0) {
		    if(vtmp->changed_val.p)
		      fs_give((void **)&vtmp->changed_val.p);

		    vtmp->changed_val.p = cpystr(asciicolor);
		    vtmp->is_changed_val = 1;

		    /* We need to handle this in the actual config setting
		     *  if(!strucmp(varname, "normal"))
		     *  pico_set_fg_color(asciicolor);
		     */
		}
		else{
		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid color value %.100s", fghex);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}

		snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-background-color");
		vtmp++;
		if((vtmp->name && strcmp(vtmp->name, tvname)) || !vtmp->name)
		  for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR];
		      vtmp->name && strcmp(vtmp->name, tvname);
		      vtmp++)
		    ;

		if(!vtmp->name || vtmp->is_list){
		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background var %.100s", varname);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}

		if(ascii_colorstr(asciicolor, bghex) == 0) {
		    if(vtmp->changed_val.p)
		      fs_give((void **)&vtmp->changed_val.p);

		    vtmp->changed_val.p = cpystr(asciicolor);
		    vtmp->is_changed_val = 1;
		    /* again, we need to handle this when we actually set the variable
		     * if(!strucmp(varname, "normal"))
		     *  pico_set_bg_color(asciicolor);
		     */
		}
		else{
		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "colorset: invalid background color value %.100s", bghex);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}

		Tcl_SetResult(interp, "1", TCL_STATIC);
		return(TCL_OK);
	    }
	}
	else if(!strcmp(s1, "ruleset")){
	    return(peRuleSet(interp, &((Tcl_Obj **)objv)[2]));
	}
	else if(objc == 2){
	    if(!strcmp(s1, "featuresettings")){
		struct variable *vtmp;
		int i;
		FEATURE_S *feature;

		vtmp = &ps_global->vars[V_FEATURE_LIST];
		for(i = 0; feature = feature_list(i); i++)
		  if(feature_list_section(feature)){
		      if(vtmp->is_changed_val ? F_CH_ON(feature->id)
			 : F_ON(feature->id, ps_global)){
			  Tcl_ListObjAppendElement(interp,
						   Tcl_GetObjResult(interp),
						   Tcl_NewStringObj(feature->name, -1));
		      }
		  }
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "rawsig")){
		char *err = NULL, *sig = NULL, *p, *q;
		int i;
		struct variable *vtmp;

		vtmp = &ps_global->vars[V_LITERAL_SIG];
		if(vtmp->is_changed_val ? vtmp->changed_val.p
		   : ps_global->VAR_LITERAL_SIG){
		    char     *err = NULL;
		    char    **apval;

		    if(ps_global->restricted){
			err = "Pine demo can't change config file";
		    }
		    else{
			/* BUG: no "exceptions file" support */
			apval = (vtmp->is_changed_val ? &vtmp->changed_val.p
				 : APVAL(&ps_global->vars[V_LITERAL_SIG], Main));
			if(apval){
			    sig = (char *) fs_get((strlen(*apval ? *apval : "") + 1) * sizeof(char));
			    sig[0] = '\0';
			    cstring_to_string(*apval, sig);
			}
			else
			  err = "Problem accessing configuration";
		    }
		}
		else if((vtmp = &ps_global->vars[V_SIGNATURE_FILE])
			&& !IS_REMOTE(vtmp->is_changed_val ? vtmp->changed_val.p
				      : ps_global->VAR_SIGNATURE_FILE))
		  snprintf(err = tmp_20k_buf, SIZEOF_20KBUF, "Non-Remote signature file: %s",
			  vtmp->is_changed_val ? (vtmp->changed_val.p
						  ? vtmp->changed_val.p : "<null>")
			  : (ps_global->VAR_SIGNATURE_FILE
			     ? ps_global->VAR_SIGNATURE_FILE : "<null>"));
		else if(!(peTSig || (sig = read_remote_sigfile(vtmp->is_changed_val
						    ? vtmp->changed_val.p
						    : ps_global->VAR_SIGNATURE_FILE))))
		  err = "Can't read remote pinerc";

		if(err){
		    Tcl_SetResult(interp, err, TCL_VOLATILE);
		    return(TCL_ERROR);
		}

		if(peTSig){
		    for(i = 0; peTSig[i]; i++)
		      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					       Tcl_NewStringObj(peTSig[i],-1));
		}
		else {
		    for(p = sig; q = strindex(p, '\n'); p = q + 1)
		      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					       Tcl_NewStringObj(p, q - p));

		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(p, -1));
		    fs_give((void **) &sig);
		}
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "filters")){
		long      rflags = ROLE_DO_FILTER | PAT_USE_CHANGED;
		PAT_STATE pstate;
		PAT_S    *pat;

		if(any_patterns(rflags, &pstate)){
		    for(pat = first_pattern(&pstate);
			pat;
			pat = next_pattern(&pstate)){
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 Tcl_NewStringObj(pat->patgrp->nick, -1));
		    }
		}
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "collections")){
		struct variable  *vtmp;
		int               i;
		CONTEXT_S        *new_ctxt;

		vtmp = &ps_global->vars[V_FOLDER_SPEC];
		for(i = 0; (vtmp->is_changed_val
			    ? vtmp->changed_val.l && vtmp->changed_val.l[i]
			    : vtmp->current_val.l && vtmp->current_val.l[i]);
		    i++){
		    new_ctxt = new_context(vtmp->is_changed_val
					   ? vtmp->changed_val.l[i]
					   : vtmp->current_val.l[i], NULL);
		    peAppListStrStr(interp, Tcl_GetObjResult(interp),
				    new_ctxt->nickname ? new_ctxt->nickname
				    : (new_ctxt->server ? new_ctxt->server
				       : (new_ctxt->label ? new_ctxt->label
					  : "Some Collection")),
				    new_ctxt->label ? new_ctxt->label : "");
		    free_context(&new_ctxt);
		}
		vtmp = &ps_global->vars[V_NEWS_SPEC];
		for(i = 0; (vtmp->is_changed_val
			    ? vtmp->changed_val.l && vtmp->changed_val.l[i]
			    : vtmp->current_val.l && vtmp->current_val.l[i]);
		    i++){
		    new_ctxt = new_context(vtmp->is_changed_val
					   ? vtmp->changed_val.l[i]
					   : vtmp->current_val.l[i], NULL);
		    peAppListStrStr(interp, Tcl_GetObjResult(interp),
				    new_ctxt->nickname ? new_ctxt->nickname
				    : (new_ctxt->server ? new_ctxt->server
				       : (new_ctxt->label ? new_ctxt->label
					  : "Some Collection")),
				    new_ctxt->label ? new_ctxt->label : "");
		    free_context(&new_ctxt);
		}

		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "newconf")){
		struct variable *vtmp;
		int i;
		FEATURE_S *feature;

		for(vtmp = ps_global->vars; vtmp->name; vtmp++)
		  vtmp->is_changed_val = 0;

		for(i = 0; feature = feature_list(i); i++)
		  F_CH_SET(feature->id, F_ON(feature->id, ps_global));

		if(peTSig){
		    for(i = 0; peTSig[i]; i++)
		      fs_give((void **)&peTSig[i]);
		    fs_give((void **)&peTSig);
		}

		close_patterns(ROLE_DO_FILTER |PAT_USE_CHANGED);
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "saveconf")){
		struct variable *vtmp;
		int              i, did_change = 0, def_sort_rev;
		FEATURE_S       *feature;

		if(ps_global->vars[V_FEATURE_LIST].is_changed_val){
		    ps_global->vars[V_FEATURE_LIST].is_changed_val = 0;
		    for(i = 0; feature = feature_list(i); i++)
		      if(feature_list_section(feature)){
			  if(F_CH_ON(feature->id) != F_ON(feature->id, ps_global)){
			      did_change = 1;
			      toggle_feature(ps_global,
					     &ps_global->vars[V_FEATURE_LIST],
					     feature, TRUE, Main);
			  }
		      }
		}

		for(vtmp = ps_global->vars; vtmp->name; vtmp++){
		    if(vtmp->is_changed_val
			&& (vtmp - ps_global->vars != V_FEATURE_LIST)){
			if(vtmp->is_list){
			    for(i = 0; vtmp->main_user_val.l
				  && vtmp->main_user_val.l[i]; i++)
			      fs_give((void **)&vtmp->main_user_val.l[i]);
			    if(vtmp->main_user_val.l)
			      fs_give((void **)&vtmp->main_user_val.l);
			    vtmp->main_user_val.l = vtmp->changed_val.l;
			    vtmp->changed_val.l = NULL;
			}
			else {
			    if(vtmp->main_user_val.p)
			      fs_give((void **)&vtmp->main_user_val.p);
			    vtmp->main_user_val.p = vtmp->changed_val.p;
			    vtmp->changed_val.p = NULL;
			}
			set_current_val(vtmp, FALSE, FALSE);
			vtmp->is_changed_val = 0;
			did_change = 1;
			switch (vtmp - ps_global->vars) {
			    case V_USER_DOMAIN:
			      init_hostname(ps_global);
			    case V_FOLDER_SPEC:
			    case V_NEWS_SPEC:
			      free_contexts(&ps_global->context_list);
			      init_folders(ps_global);
			      break;
			    case V_NORM_FORE_COLOR:
			      pico_set_fg_color(vtmp->current_val.p);
			      break;
			    case V_NORM_BACK_COLOR:
			      pico_set_bg_color(vtmp->current_val.p);
			      break;
			    case V_ADDRESSBOOK:
			    case V_GLOB_ADDRBOOK:
#ifdef	ENABLE_LDAP
			    case V_LDAP_SERVERS:
#endif
			    case V_ABOOK_FORMATS:
			      addrbook_reset();
			    case V_INDEX_FORMAT:
			      init_index_format(ps_global->VAR_INDEX_FORMAT,
						&ps_global->index_disp_format);
			      clear_index_cache(sp_inbox_stream(), 0);
			      break;
			    case V_PAT_FILTS:
			      close_patterns(ROLE_DO_FILTER | PAT_USE_CURRENT);
			      role_process_filters();
			      break;
			    case V_DEFAULT_FCC:
			    case V_DEFAULT_SAVE_FOLDER:
			      init_save_defaults();
			      break;
			    case V_SORT_KEY:
			      decode_sort(ps_global->VAR_SORT_KEY, &ps_global->def_sort, &def_sort_rev);
			      break;
			    case V_VIEW_HDR_COLORS :
			      set_custom_spec_colors(ps_global);
			      break;
			    case V_POST_CHAR_SET :
			      update_posting_charset(ps_global, 1);
			      break;
			    default:
			      break;
			}
		    }
		}
		if(peTSig){
		    peWriteSig(interp, ps_global->VAR_SIGNATURE_FILE, NULL);
		}
		if(did_change){
		    write_pinerc(ps_global, Main, WRP_NOUSER);
		    q_status_message(SM_ORDER, 0, 3, "Configuration changes saved!");
		}
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "columns")){
		Tcl_SetResult(interp, int2string(ps_global->ttyo->screen_cols), TCL_VOLATILE);
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "indextokens")){
		INDEX_PARSE_T *tok;
		Tcl_Obj	      *rObj;
		int	       i;

		for(i = 0; (tok = itoken(i)) != NULL; i++)
		  if(tok->what_for & FOR_INDEX)
		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(tok->name, -1));

		return(TCL_OK);
	    }
	}
	else if(objc == 3){
	    if(!strcmp(s1, "varget")){
		char *varname = Tcl_GetStringFromObj(objv[2], NULL);
		struct variable *vtmp;
		Tcl_Obj         *resObj, *secObj;
		char            *input_type;
		int              is_default, i;
		NAMEVAL_S       *tmpnv;

		if(varname == NULL) return(TCL_ERROR);
		for(vtmp = ps_global->vars; vtmp->name
		      && strcmp(vtmp->name, varname); vtmp++);
		if(!vtmp->name){
		    Tcl_SetResult(interp, err, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		resObj = Tcl_NewListObj(0, NULL);
		if(vtmp->is_list){
		    if(vtmp->is_changed_val){
			for(i = 0; vtmp->changed_val.l && vtmp->changed_val.l[i]; i++){
			    Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(vtmp->changed_val.l[i], -1));
			}
		    }
		    else {
			for(i = 0; vtmp->current_val.l && vtmp->current_val.l[i]; i++){
			    Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(vtmp->current_val.l[i], -1));
			}
		    }
		}
		else {
		    if(vtmp->is_changed_val){
			if(vtmp->changed_val.p)
			  Tcl_ListObjAppendElement(interp, resObj,
						   Tcl_NewStringObj(vtmp->changed_val.p[0]
								    ? vtmp->changed_val.p
								    : "\"\"", -1));
		    }
		    else {
			if(vtmp->current_val.p)
			  Tcl_ListObjAppendElement(interp, resObj,
						   Tcl_NewStringObj(vtmp->current_val.p[0]
								    ? vtmp->current_val.p
								    : "\"\"", -1));
		    }
		}
		Tcl_ListObjAppendElement(interp, 
					 Tcl_GetObjResult(interp),
					 resObj);
		secObj = Tcl_NewListObj(0, NULL);
		if(vtmp->is_list)
		  input_type = cpystr("textarea");
		else{
		    NAMEVAL_S *(*tmpf)(int);
		    switch(vtmp - ps_global->vars){
		      case V_SAVED_MSG_NAME_RULE:
			tmpf = save_msg_rules;
			break;
		      case V_FCC_RULE:
			tmpf = fcc_rules;
			break;
		      case V_SORT_KEY:
			tmpf = sort_key_rules;
			break;
		      case V_AB_SORT_RULE:
			tmpf = ab_sort_rules;
			break;
		      case V_FLD_SORT_RULE:
			tmpf = fld_sort_rules;
			break;
		      case V_GOTO_DEFAULT_RULE:
			tmpf = goto_rules;
			break;
		      case V_INCOMING_STARTUP:
			tmpf = incoming_startup_rules;
			break;
		      case V_PRUNING_RULE:
			tmpf = pruning_rules;
			break;
		      case V_WP_INDEXHEIGHT:
			tmpf = wp_indexheight_rules;
			break;
		      default:
			tmpf = NULL;
			break;
		    }
		    if(tmpf){
			for(i = 0; tmpnv = (tmpf)(i); i++){
			  if(tmpnv->shortname)
			    peAppListStrStr(interp, secObj, tmpnv->name, tmpnv->shortname);
			  else
			    Tcl_ListObjAppendElement(interp, secObj, 
						     Tcl_NewStringObj(tmpnv->name, -1));
			}
			input_type = cpystr("listbox");
		    }
		    else
		      input_type = cpystr("text");
		}
		Tcl_ListObjAppendElement(interp, 
					 Tcl_GetObjResult(interp), 
					 Tcl_NewStringObj(input_type, -1));
		Tcl_ListObjAppendElement(interp, 
					 Tcl_GetObjResult(interp),
					 secObj);
		if(vtmp->is_list)
		  is_default = !vtmp->is_changed_val && !vtmp->main_user_val.l;
		else
		  is_default = !vtmp->is_changed_val && !vtmp->main_user_val.p;
		Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					 Tcl_NewIntObj(is_default));
		Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					 Tcl_NewIntObj(vtmp->is_fixed));
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "filtextended")){
		int fl, i;
		long      rflags = ROLE_DO_FILTER | PAT_USE_CHANGED;
		PAT_STATE pstate;
		PAT_S    *pat;
		Tcl_Obj  *resObj = NULL, *tObj = NULL;

		if(Tcl_GetIntFromObj(interp, objv[2], &fl) == TCL_ERROR)
		  return(TCL_ERROR);
		if(any_patterns(rflags, &pstate)){
		    for(pat = first_pattern(&pstate), i = 0;
			pat && i != fl;
			pat = next_pattern(&pstate), i++);
		    if(i != fl) return(TCL_ERROR);

		    /* append the pattern */
		    resObj = Tcl_NewListObj(0, NULL);
		    peAppListStrStr(interp, resObj, "nickname", pat->patgrp->nick);
		    peAppListStrPat(interp, resObj, "to", pat->patgrp->to);
		    peAppListStrPat(interp, resObj, "from", pat->patgrp->from);
		    peAppListStrPat(interp, resObj, "sender", pat->patgrp->sender);
		    peAppListStrPat(interp, resObj, "cc", pat->patgrp->cc);
		    peAppListStrPat(interp, resObj, "recip", pat->patgrp->recip);
		    peAppListStrPat(interp, resObj, "partic", pat->patgrp->partic);
		    peAppListStrPat(interp, resObj, "news", pat->patgrp->news);
		    peAppListStrPat(interp, resObj, "subj", pat->patgrp->subj);
		    peAppListStrPat(interp, resObj, "alltext", pat->patgrp->alltext);
		    switch(pat->patgrp->fldr_type){
		      case FLDR_ANY:
			peAppListStrStr(interp, resObj, "ftype", "any");
			break;
		      case FLDR_NEWS:
			peAppListStrStr(interp, resObj, "ftype", "news");
			break;
		      case FLDR_EMAIL:
			peAppListStrStr(interp, resObj, "ftype", "email");
			break;
		      case FLDR_SPECIFIC:
			peAppListStrStr(interp, resObj, "ftype", "specific");
			break;
		    }
		    peAppListStrPat(interp, resObj, "folder", pat->patgrp->folder);
		    peAppListStrPatstat(interp, resObj, "stat_new", pat->patgrp->stat_new);
		    peAppListStrPatstat(interp, resObj, "stat_del", pat->patgrp->stat_del);
		    peAppListStrPatstat(interp, resObj, "stat_imp", pat->patgrp->stat_imp);
		    peAppListStrPatstat(interp, resObj, "stat_ans", pat->patgrp->stat_ans);
		    tObj = Tcl_NewListObj(0, NULL);
		    Tcl_ListObjAppendElement(interp, tObj, Tcl_NewStringObj("pattern", -1));
		    Tcl_ListObjAppendElement(interp, tObj, resObj);
		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), tObj);
		    
		    /* now append the filter action */
		    resObj = Tcl_NewListObj(0, NULL);
		    peAppListStrInt(interp, resObj, "kill", pat->action->folder ? 0 : 1);
		    peAppListStrPat(interp, resObj, "folder", pat->action->folder);
		    peAppListStrInt(interp, resObj, "move_only_if_not_deleted",
				    pat->action->move_only_if_not_deleted);
		    tObj = Tcl_NewListObj(0, NULL);
		    Tcl_ListObjAppendElement(interp, tObj, Tcl_NewStringObj("filtaction", -1));
		    Tcl_ListObjAppendElement(interp, tObj, resObj);
		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), tObj);
		}
		else return(TCL_ERROR);

		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "clextended")){
		int cl, i, j = 0, in_folder_spec = 0;
		struct variable *vtmp;
		char tpath[MAILTMPLEN], *p;
		CONTEXT_S *ctxt;

		vtmp = &ps_global->vars[V_FOLDER_SPEC];
		if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR)
		  return(TCL_ERROR);
		for(i = 0; i < cl && (vtmp->is_changed_val
				      ? (vtmp->changed_val.l
					  && vtmp->changed_val.l[i])
				      : (vtmp->current_val.l
					  && vtmp->current_val.l[i])); i++);
		if(i == cl && (vtmp->is_changed_val
			       ? vtmp->changed_val.l && vtmp->changed_val.l[i]
			       : vtmp->current_val.l && vtmp->current_val.l[i]))
		  in_folder_spec = 1;
		else {
		    vtmp = &ps_global->vars[V_NEWS_SPEC];
		    for(j = 0; i + j < cl && (vtmp->is_changed_val
					      ? (vtmp->changed_val.l
						 && vtmp->changed_val.l[j])
					      : (vtmp->current_val.l
						 && vtmp->current_val.l[j])); j++);
		}
		if(in_folder_spec || (i + j == cl && (vtmp->is_changed_val
			       ? vtmp->changed_val.l && vtmp->changed_val.l[j]
			       : vtmp->current_val.l && vtmp->current_val.l[j]))){
		    ctxt = new_context(vtmp->is_changed_val ? vtmp->changed_val.l[in_folder_spec ? i : j]
				       : vtmp->current_val.l[in_folder_spec ? i : j], NULL);
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(ctxt->nickname ? ctxt->nickname : "", -1));
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(ctxt->label ? ctxt->label : "", -1));
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(ctxt->server ? ctxt->server : "", -1));
		    tpath[0] = '\0';
		    if(ctxt->context){
			strncpy(tpath, (ctxt->context[0] == '{'
				       && (p = strchr(ctxt->context, '}')))
			       ? ++p
			       : ctxt->context, sizeof(tpath));
			tpath[sizeof(tpath)-1] = '\0';
			if(p = strstr(tpath, "%s"))
			  *p = '\0';
		    }
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(tpath, -1));
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(ctxt->dir && ctxt->dir->view.user
							      ? ctxt->dir->view.user : "", -1));
		    free_context(&ctxt);

		    return(TCL_OK);
		}
		else 
		  return(TCL_ERROR);
	    }
	    else if(!strcmp(s1, "rawsig")){
		struct variable *vtmp;
		char	 *cstring_version, *sig, *line;
		int	  i, nSig;
		Tcl_Obj **objSig;

		vtmp = &ps_global->vars[V_LITERAL_SIG];
		if(vtmp->is_changed_val ? vtmp->changed_val.p
		   : ps_global->VAR_LITERAL_SIG){

		    tmp_20k_buf[0] = '\0';
		    Tcl_ListObjGetElements(interp, objv[2], &nSig, &objSig);
		    for(i = 0; i < nSig && i < SIG_MAX_LINES; i++)
		      if(line = Tcl_GetStringFromObj(objSig[i], NULL))
			snprintf(tmp_20k_buf + strlen(tmp_20k_buf), SIZEOF_20KBUF - strlen(tmp_20k_buf), "%.*s\n", SIG_MAX_COLS, line);

		    sig = cpystr(tmp_20k_buf);

		    if(cstring_version = string_to_cstring(sig)){
			if(vtmp->changed_val.p)
			  fs_give((void **)&vtmp->changed_val.p);
			vtmp->is_changed_val = 1;
			vtmp->changed_val.p = cstring_version;
		    }

		    fs_give((void **) &sig);
		    return(TCL_OK);
		}
		else {
		    if(peTSig){
			for(i = 0; peTSig[i]; i++)
			  fs_give((void **)&peTSig[i]);
			fs_give((void **)&peTSig);
		    }
		    Tcl_ListObjGetElements(interp, objv[2], &nSig, &objSig);
		    peTSig = (char **)fs_get(sizeof(char)*(nSig + 1));
		    for(i = 0; i < nSig; i++){
			line = Tcl_GetStringFromObj(objSig[i], NULL);
			peTSig[i] = cpystr(line ? line : "");
		    }
		    peTSig[i] = NULL;
		    return(TCL_OK);
		}
	    }
	    else if(!strcmp(s1, "colorget")){
		char             *varname;
		char              tvname[256], hexcolor[256];
		struct  variable *vtmp;
		if(!(varname = Tcl_GetStringFromObj(objv[2], NULL))){
		    return(TCL_ERROR);
		}
		if(strcmp("viewer-hdr-colors", varname) == 0){
		    SPEC_COLOR_S *hcolors, *thc;
		    Tcl_Obj     *resObj;
		    char         hexcolor[256], *tstr = NULL;

		    if(ps_global->vars[V_VIEW_HDR_COLORS].is_changed_val)
		      hcolors = spec_colors_from_varlist(ps_global->vars[V_VIEW_HDR_COLORS].changed_val.l, 0);
		    else
		      hcolors = spec_colors_from_varlist(ps_global->VAR_VIEW_HDR_COLORS, 0);
		    for(thc = hcolors; thc; thc = thc->next){
			resObj = Tcl_NewListObj(0,NULL);
			Tcl_ListObjAppendElement(interp, resObj,
						 Tcl_NewStringObj(thc->spec, -1));
			hex_colorstr(hexcolor, thc->fg);
			Tcl_ListObjAppendElement(interp, resObj,
						 Tcl_NewStringObj(hexcolor, -1));
			hex_colorstr(hexcolor, thc->bg);
			Tcl_ListObjAppendElement(interp, resObj,
						 Tcl_NewStringObj(hexcolor, -1));
			Tcl_ListObjAppendElement(interp, resObj,
						 Tcl_NewStringObj(thc->val 
								  ? tstr = pattern_to_string(thc->val) 
								  : "", -1));
			if(tstr) fs_give((void **)&tstr);
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 resObj);
		    }
		    fs_give((void **)&hcolors);
		    return(TCL_OK);
		}
		else {
		    char *colorp;

		    snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-foreground-color");
		    for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR]; vtmp->name
			  && strcmp(vtmp->name, tvname); vtmp++);
		    if(!vtmp->name) return(TCL_ERROR);
		    if(vtmp->is_list) return(TCL_ERROR);

		    colorp = (vtmp->is_changed_val && vtmp->changed_val.p)
			      ? vtmp->changed_val.p
			      : (vtmp->current_val.p) ? vtmp->current_val.p
				   :  vtmp->global_val.p;

		    if(colorp){
			hex_colorstr(hexcolor, colorp);
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 Tcl_NewStringObj(hexcolor, -1));
		    }
		    else
		      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					       Tcl_NewStringObj("", -1));

		    snprintf(tvname, sizeof(tvname), "%.200s%.50s", varname, "-background-color");
		    vtmp++;
		    if((vtmp->name && strcmp(vtmp->name, tvname)) || !vtmp->name)
		      for(vtmp = &ps_global->vars[V_NORM_FORE_COLOR]; vtmp->name
			    && strcmp(vtmp->name, tvname); vtmp++);
		    if(!vtmp->name) return(TCL_ERROR);
		    if(vtmp->is_list) return(TCL_ERROR);

		    colorp = (vtmp->is_changed_val && vtmp->changed_val.p)
			      ? vtmp->changed_val.p
			      : (vtmp->current_val.p) ? vtmp->current_val.p
				   :  vtmp->global_val.p;

		    if(colorp){
			hex_colorstr(hexcolor, colorp);
			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 Tcl_NewStringObj(hexcolor, -1));
		    }
		    else
		      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					       Tcl_NewStringObj("", -1));
		}
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "cldel")){
		int cl, i, j, n;
		struct variable *vtmp;
		char **newl;

		if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR)
		  return(TCL_ERROR);
		vtmp = &ps_global->vars[V_FOLDER_SPEC];
		for(i = 0; i < cl && (vtmp->is_changed_val
				      ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
				      : (vtmp->current_val.l && vtmp->current_val.l[i])); i++);
		if(!(i == cl && (vtmp->is_changed_val
				 ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
				 : (vtmp->current_val.l && vtmp->current_val.l[i])))){
		    vtmp = &ps_global->vars[V_NEWS_SPEC];
		    for(j = 0; i + j < cl && (vtmp->is_changed_val
					      ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
					      : (vtmp->current_val.l && vtmp->current_val.l[j]));
			j++);
		    if(!(vtmp->is_changed_val
			 ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
			 : (vtmp->current_val.l && vtmp->current_val.l[j])))
		      return(TCL_ERROR);
		    i = j;
		}
		for(n = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[n])
		      : (vtmp->current_val.l && vtmp->current_val.l[n]); n++);
		newl = (char **)fs_get(n*(sizeof(char *)));
		for(n = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[n])
		      : (vtmp->current_val.l && vtmp->current_val.l[n]); n++){
		    if(n < i)
		      newl[n] = cpystr(vtmp->is_changed_val ? vtmp->changed_val.l[n]
				       : vtmp->current_val.l[n]);
		    else if(n > i)
		      newl[n-1] = cpystr(vtmp->is_changed_val ? vtmp->changed_val.l[n]
					 : vtmp->current_val.l[n]);
		}
		newl[n-1] = NULL;
		vtmp->is_changed_val = 1;
		for(n = 0; vtmp->changed_val.l && vtmp->changed_val.l[n]; n++)
		  fs_give((void **) &vtmp->changed_val.l[n]);
		if(vtmp->changed_val.l) fs_give((void **)&vtmp->changed_val.l);
		vtmp->changed_val.l = newl;

		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "columns")){
		int   n;
		char *p;

		if(Tcl_GetIntFromObj(interp, objv[2], &n) != TCL_ERROR
		   && n >= MIN_SCREEN_COLS
		   && n < (MAX_SCREEN_COLS - 1)
		   && ps_global->ttyo->screen_cols != n){
		    clear_index_cache(sp_inbox_stream(), 0);
		    ps_global->ttyo->screen_cols = n;
		    set_variable(V_WP_COLUMNS, p = int2string(n), 0, 0, Main);
		    Tcl_SetResult(interp, p, TCL_VOLATILE);
		}
		else
		  Tcl_SetResult(interp, int2string(ps_global->ttyo->screen_cols), TCL_VOLATILE);

		return(TCL_OK);
	    }
	}
	else if(objc == 4){
	    if(!strcmp(s1, "varset")){
		char *varname = Tcl_GetStringFromObj(objv[2], NULL);
		struct variable *vtmp;
		char  **tstrlist = NULL, *line, *tline;
		Tcl_Obj **objVal;
		int i, strlistpos, numlistvals;

		if(varname == NULL) return(TCL_ERROR);
		for(vtmp = ps_global->vars; vtmp->name
		      && strcmp(vtmp->name, varname); vtmp++);
		if(!vtmp->name){
		    Tcl_SetResult(interp, err, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if(Tcl_ListObjGetElements(interp, objv[3], &numlistvals,
					  &objVal) != TCL_OK)
		  return(TCL_ERROR);
		vtmp->is_changed_val = 1;
		if(vtmp->is_list){
		    if(vtmp->changed_val.l){
			for(i = 0; vtmp->changed_val.l[i]; i++)
			  fs_give((void **)&vtmp->changed_val.l[i]);
			fs_give((void **)&vtmp->changed_val.l);
		    }
		    if(numlistvals)
		      tstrlist = (char **)fs_get((numlistvals + 1) * sizeof(char *));
		    for(i = 0, strlistpos = 0; i < numlistvals; i++){
			if(line = Tcl_GetStringFromObj(objVal[i], 0)){
			    tline = cpystr(line);
			    removing_leading_and_trailing_white_space(tline);
			    if(*tline)
			      tstrlist[strlistpos++] = cpystr(tline);
			    fs_give((void **) &tline);
			}
		    }
		    if(tstrlist)
		      tstrlist[strlistpos] = NULL;
		    vtmp->changed_val.l = tstrlist;
		}
		else {
		    if(vtmp->changed_val.p)
		      fs_give((void **)&vtmp->changed_val.p);
		    if(numlistvals){
			if(line = Tcl_GetStringFromObj(objVal[0], 0)){
			    tline = cpystr(line);
			    if(strcmp(vtmp->name, "reply-indent-string"))
			      removing_leading_and_trailing_white_space(tline);
			    if(!strcmp(tline, "\"\"")){
				tline[0] = '\0';
			    }
			    else if(tline[0] == '\0'){
				fs_give((void **)&tline);
			    }
			    if(tline){
				vtmp->changed_val.p = cpystr(tline);
				fs_give((void **)&tline);
			    }
			}
			else
			  vtmp->changed_val.p = cpystr("");
		    }
		}
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "feature")){
		char      *featurename;
		int	       i, set, wasset = 0;
		FEATURE_S *feature;

		    /*
		     * CMD: feature
		     *
		     * ARGS: featurename -
		     *	     value - new value to assign flag
		     *
		     * Returns: 1 if named feature set, 0 otherwise
		     *          
		     */
		if((featurename = Tcl_GetStringFromObj(objv[2], NULL))
		   && Tcl_GetIntFromObj(interp, objv[3], &set) != TCL_ERROR)
		  for(i = 0; feature = feature_list(i); i++)
		    if(!strcmp(featurename, feature->name)){
			ps_global->vars[V_FEATURE_LIST].is_changed_val = 1;
			wasset = F_CH_ON(feature->id);
			F_CH_SET(feature->id, set);
			break;
		    }

		Tcl_SetResult(interp, int2string(wasset), TCL_VOLATILE);
		return(TCL_OK);
	    }
	    else if(!strcmp(s1, "clshuff")){
		char *dir, *tstr, **newl;
		int cl, up = 0, fvarn, nvarn, icnt, i;
		struct variable *fvar, *nvar, *vtmp;

		if(!(dir = Tcl_GetStringFromObj(objv[2], NULL)))
		  return TCL_ERROR;
		if(Tcl_GetIntFromObj(interp, objv[3], &cl) == TCL_ERROR)
		  return(TCL_ERROR);
		if(!strcmp(dir, "up"))
		  up = 1;
		else if(!strcmp(dir, "down"))
		  up = 0;
		else
		  return(TCL_ERROR);
		fvar = &ps_global->vars[V_FOLDER_SPEC];
		nvar = &ps_global->vars[V_NEWS_SPEC];
		for(fvarn = 0; fvar->is_changed_val ? (fvar->changed_val.l && fvar->changed_val.l[fvarn])
		      : (fvar->current_val.l && fvar->current_val.l[fvarn]); fvarn++);
		for(nvarn = 0; nvar->is_changed_val ? (nvar->changed_val.l && nvar->changed_val.l[nvarn])
		      : (nvar->current_val.l && nvar->current_val.l[nvarn]); nvarn++);
		if(cl < fvarn){
		    vtmp = fvar;
		    icnt = cl;
		}
		else if(cl >= fvarn && cl < nvarn + fvarn){
		    vtmp = nvar;
		    icnt = cl - fvarn;
		}
		else
		  return(TCL_ERROR);
		if(vtmp == nvar && icnt == 0 && up){
		    newl = (char **)fs_get((fvarn + 2)*sizeof(char *));
		    for(i = 0; fvar->is_changed_val ? (fvar->changed_val.l && fvar->changed_val.l[i])
			  : (fvar->current_val.l && fvar->current_val.l[i]); i++)
		      newl[i] = cpystr(fvar->is_changed_val ? fvar->changed_val.l[i]
				       : fvar->current_val.l[i]);
		    newl[i++] = cpystr(nvar->is_changed_val ? nvar->changed_val.l[0]
				       : nvar->current_val.l[0]);
		    newl[i] = NULL;
		    fvar->is_changed_val = 1;
		    for(i = 0; fvar->changed_val.l && fvar->changed_val.l[i]; i++)
		      fs_give((void **)&fvar->changed_val.l[i]);
		    if(fvar->changed_val.l) fs_give((void **)&fvar->changed_val.l);
		    fvar->changed_val.l = newl;
		    newl = (char **)fs_get(nvarn*sizeof(char *));
		    for(i = 1; nvar->is_changed_val ? (nvar->changed_val.l && nvar->changed_val.l[i])
			  : (nvar->current_val.l && nvar->current_val.l[i]); i++)
		      newl[i-1] = cpystr(nvar->is_changed_val ? nvar->changed_val.l[i]
					 : nvar->current_val.l[i]);
		    newl[i-1] = NULL;
		    nvar->is_changed_val = 1;
		    for(i = 0; nvar->changed_val.l && nvar->changed_val.l[i]; i++)
		      fs_give((void **)&nvar->changed_val.l[i]);
		    if(nvar->changed_val.l) fs_give((void **)&nvar->changed_val.l);
		    nvar->changed_val.l = newl;
		    vtmp = fvar;
		    icnt = fvarn;
		}
		else if(vtmp == fvar && icnt == fvarn - 1 && !up){
		    newl = (char **)fs_get(fvarn*sizeof(char *));
		    for(i = 0; fvar->is_changed_val ? (fvar->changed_val.l && fvar->changed_val.l[i+1])
			  : (fvar->current_val.l && fvar->current_val.l[i+1]); i++)
		      newl[i] = cpystr(fvar->is_changed_val ? fvar->changed_val.l[i]
				       : fvar->current_val.l[i]);
		    newl[i] = NULL;
		    tstr = cpystr(fvar->is_changed_val ? fvar->changed_val.l[i]
				  : fvar->current_val.l[i]);
		    fvar->is_changed_val = 1;
		    for(i = 0; fvar->changed_val.l && fvar->changed_val.l[i]; i++)
		      fs_give((void **)&fvar->changed_val.l[i]);
		    if(fvar->changed_val.l) fs_give((void **)&fvar->changed_val.l);
		    fvar->changed_val.l = newl;
		    newl = (char **)fs_get((nvarn+2)*sizeof(char *));
		    newl[0] = tstr;
		    for(i = 0; nvar->is_changed_val ? (nvar->changed_val.l && nvar->changed_val.l[i])
			  : (nvar->current_val.l && nvar->current_val.l[i]); i++)
		      newl[i+1] = cpystr(nvar->is_changed_val ? nvar->changed_val.l[i]
					 : nvar->current_val.l[i]);
		    newl[i+1] = NULL;
		    nvar->is_changed_val = 1;
		    for(i = 0; nvar->changed_val.l && nvar->changed_val.l[i]; i++)
		      fs_give((void **)&nvar->changed_val.l[i]);
		    if(nvar->changed_val.l) fs_give((void **)&nvar->changed_val.l);
		    nvar->changed_val.l = newl;
		    vtmp = nvar;
		    icnt = 0;
		}
		else {
		    newl = (char **)fs_get(((vtmp == fvar ? fvarn : nvarn) + 1)*sizeof(char *));
		    for(i = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
			  : (vtmp->current_val.l && vtmp->current_val.l[i]); i++)
		      newl[i] = cpystr(vtmp->is_changed_val ? vtmp->changed_val.l[i]
				    : vtmp->current_val.l[i]);
		    newl[i] = NULL;
		    vtmp->is_changed_val = 1;
		    for(i = 0; vtmp->changed_val.l && vtmp->changed_val.l[i]; i++)
		      fs_give((void **)&vtmp->changed_val.l[i]);
		    if(vtmp->changed_val.l) fs_give((void **)&vtmp->changed_val.l);
		    vtmp->changed_val.l = newl;
		}
		if(up){
		    tstr = vtmp->changed_val.l[icnt-1];
		    vtmp->changed_val.l[icnt-1] = vtmp->changed_val.l[icnt];
		    vtmp->changed_val.l[icnt] = tstr;
		}
		else {
		    tstr = vtmp->changed_val.l[icnt+1];
		    vtmp->changed_val.l[icnt+1] = vtmp->changed_val.l[icnt];
		    vtmp->changed_val.l[icnt] = tstr;
		}
		return(TCL_OK);
	    }
	}
	else if(objc == 7){
	    if(!strcmp(s1, "cledit") || !strcmp(s1, "cladd")){
		int add = 0, cl, quotes_needed = 0, i, j, newn;
		char *nick, *server, *path, *view, context_buf[MAILTMPLEN*4];
		char **newl;
		struct variable *vtmp;

		if(!strcmp(s1, "cladd")) add = 1;

		if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR)
		  return(TCL_ERROR);
		if(!(nick = Tcl_GetStringFromObj(objv[3], NULL)))
		  return TCL_ERROR;
		if(!(server = Tcl_GetStringFromObj(objv[4], NULL)))
		  return TCL_ERROR;
		if(!(path = Tcl_GetStringFromObj(objv[5], NULL)))
		  return TCL_ERROR;
		if(!(view = Tcl_GetStringFromObj(objv[6], NULL)))
		  return TCL_ERROR;
		removing_leading_and_trailing_white_space(nick);
		removing_leading_and_trailing_white_space(server);
		removing_leading_and_trailing_white_space(path);
		removing_leading_and_trailing_white_space(view);
		if(strchr(nick, ' '))
		  quotes_needed = 1;
		if(strlen(nick)+strlen(server)+strlen(path)+strlen(view) >
		   MAILTMPLEN * 4 - 20) { /* for good measure */
		    Tcl_SetResult(interp, "info too long", TCL_VOLATILE);
		    return TCL_ERROR;
		}
		if(3 + strlen(nick) + strlen(server) + strlen(path) +
		   strlen(view) > MAILTMPLEN + 4){
		    Tcl_SetResult(interp, "collection fields too long", TCL_VOLATILE);
		    return(TCL_OK);
		}
		snprintf(context_buf, sizeof(context_buf), "%s%s%s%s%s%s[%s]", quotes_needed ?
			"\"" : "", nick, quotes_needed ? "\"" : "",
			strlen(nick) ? " " : "",
			server, path, view);
		if(add) {
		    vtmp = &ps_global->vars[V_NEWS_SPEC];
		    if(!(vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[0])
		       : (vtmp->current_val.l && vtmp->current_val.l[0])))
		      vtmp = &ps_global->vars[V_FOLDER_SPEC];
		    for(i = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
			  : (vtmp->current_val.l && vtmp->current_val.l[i]); i++);
		    newn = i + 1;
		    newl = (char **)fs_get((newn + 1)*sizeof(char *));
		    for(i = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
			  : (vtmp->current_val.l && vtmp->current_val.l[i]); i++)
		      newl[i] = cpystr(vtmp->is_changed_val ? vtmp->changed_val.l[i]
				       : vtmp->current_val.l[i]);
		    newl[i++] = cpystr(context_buf);
		    newl[i] = NULL;
		}
		else {
		    vtmp = &ps_global->vars[V_FOLDER_SPEC];
		    for(i = 0; i < cl && (vtmp->is_changed_val
					  ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
					  : (vtmp->current_val.l && vtmp->current_val.l[i])); i++);
		    if(!(i == cl && (vtmp->is_changed_val
				     ? (vtmp->changed_val.l && vtmp->changed_val.l[i])
				     : (vtmp->current_val.l && vtmp->current_val.l[i])))){
			vtmp = &ps_global->vars[V_NEWS_SPEC];
			for(j = 0; i + j < cl && (vtmp->is_changed_val
						  ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
						  : (vtmp->current_val.l && vtmp->current_val.l[j]));
			    j++);
			if(!(vtmp->is_changed_val
			     ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
			     : (vtmp->current_val.l && vtmp->current_val.l[j])))
			  return(TCL_ERROR);
			i = j;
		    }
		    for(j = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
			  : (vtmp->current_val.l && vtmp->current_val.l[j]); j++);
		    newl = (char **)fs_get(j * sizeof(char *));
		    for(j = 0; vtmp->is_changed_val ? (vtmp->changed_val.l && vtmp->changed_val.l[j])
			  : (vtmp->current_val.l && vtmp->current_val.l[j]); j++){
			if(j == i)
			  newl[j] = cpystr(context_buf);
			else
			  newl[j] = cpystr(vtmp->is_changed_val ? vtmp->changed_val.l[j]
					   : vtmp->current_val.l[j]);
		    }
		    newl[j] = NULL;
		}
		vtmp->is_changed_val = 1;
		for(j = 0; vtmp->changed_val.l && vtmp->changed_val.l[j]; j++)
		  fs_give((void **)&vtmp->changed_val.l[j]);
		if(vtmp->changed_val.l) fs_give((void **)&vtmp->changed_val.l);
		vtmp->changed_val.l = newl;
		return TCL_OK;
	    }
	}
	else
	  err = "PEInfo: Too many arguments";
    }
    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


int
peWriteSig(Tcl_Interp *interp, char *file, Tcl_Obj **objv)
{
    int        try_cache, e, i, n, nSig;
    char       datebuf[200], *sig, *line;
    FILE      *fp;
    REMDATA_S *rd;
    Tcl_Obj  **objSig;

    if(!(file && IS_REMOTE(file))){
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Non-Remote signature file: %s",
		file ? file : "<null>");
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }

    /*
     * We could parse the name here to find what type it is. So far we
     * only have type RemImap.
     */
    rd = rd_create_remote(RemImap, file, (void *)REMOTE_SIG_SUBTYPE,
			  NULL, "Error: ", "Can't fetch remote signature.");
    if(!rd){
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Can't create stream for sig file: %s", file);
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }
    
    try_cache = rd_read_metadata(rd);

    if(rd->access == MaybeRorW){
	if(rd->read_status == 'R')
	  rd->access = ReadOnly;
	else
	  rd->access = ReadWrite;
    }

    if(rd->access != NoExists){

	rd_check_remvalid(rd, 1L);

	/*
	 * If the cached info says it is readonly but
	 * it looks like it's been fixed now, change it to readwrite.
	 */
        if(rd->read_status == 'R'){
	    /*
	     * We go to this trouble since readonly sigfiles
	     * are likely a mistake. They are usually supposed to be
	     * readwrite so we open it and check if it's been fixed.
	     */
	    rd_check_readonly_access(rd);
	    if(rd->read_status == 'W'){
		rd->access = ReadWrite;
		rd->flags |= REM_OUTOFDATE;
	    }
	    else{
		rd_close_remdata(&rd);
		snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Readonly sig file: %s", file);
		Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	}

	if(rd->flags & REM_OUTOFDATE){
	    if(rd_update_local(rd) != 0){

		dprint((1, "pinerc_remote_open: rd_update_local failed"));
		/*
		 * Don't give up altogether. We still may be
		 * able to use a cached copy.
		 */
	    }
	    else{
		dprint((7, "%s: copied remote to local (%ld)",
			rd->rn, (long)rd->last_use));
	    }
	}

	if(rd->access == ReadWrite)
	  rd->flags |= DO_REMTRIM;
    }

    /* If we couldn't get to remote folder, try using the cached copy */
    if(rd->access == NoExists || rd->flags & REM_OUTOFDATE){
	rd_close_remdata(&rd);
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Unavailable sig file: %s", file);
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }

    unlink(rd->lf);

    sig = NULL;
    tmp_20k_buf[0] = '\0';
    if(objv){
	Tcl_ListObjGetElements(interp, objv[0], &nSig, &objSig);
	for(i = 0; i < nSig && i < SIG_MAX_LINES; i++){
	    if(line = Tcl_GetStringFromObj(objSig[i], NULL))
	      snprintf(tmp_20k_buf + strlen(tmp_20k_buf), SIZEOF_20KBUF - strlen(tmp_20k_buf), "%.*s\n",
		      SIG_MAX_COLS, line);
	}
    }
    else if(peTSig){
	for(i = 0; peTSig[i] && i < SIG_MAX_LINES; i++) {
	    snprintf(tmp_20k_buf + strlen(tmp_20k_buf), SIZEOF_20KBUF - strlen(tmp_20k_buf), "%.*s\n",
		    SIG_MAX_COLS, peTSig[i]);
	}
	for(i = 0; peTSig[i]; i++)
	  fs_give((void **)&peTSig[i]);
	fs_give((void **)&peTSig);
    }
    else
      return(TCL_ERROR);

    sig = cpystr(tmp_20k_buf);

    if(fp = fopen(rd->lf, "w"))
      n = fwrite(sig, strlen(sig), 1, fp);

    fs_give((void **) &sig);

    if(fp){
	if(n != 1){
	    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Sig copy failure1: %s: %s",
		    rd->lf, error_description(errno));
	    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	    rd_close_remdata(&rd);
	}

	fclose(fp);
	if(n != 1)
	  return(TCL_ERROR);
    }
    else {
	rd_close_remdata(&rd);
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Sig copy open failure2: %s: %s",
		rd->lf, error_description(errno));
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }

    datebuf[0] = '\0';

    if(!rd->t.i.stream){
	long retflags = 0;

	rd->t.i.stream = context_open(NULL, NULL, rd->rn, 0L, &retflags);
    }

    if((e = rd_update_remote(rd, datebuf)) != 0){
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Sig update failure: %s: %s",
		rd->lf, error_description(errno));
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	rd_close_remdata(&rd);
	return(TCL_ERROR);
    }

    rd_update_metadata(rd, datebuf);
    rd->read_status = 'W';
    rd_close_remdata(&rd);
    return(TCL_OK);
}



NAMEVAL_S   *sort_key_rules(index)
     int index;
{
    static NAMEVAL_S is_rules[] = {
        {"Arrival",		0},
        {"Date",		0},
        {"Subject",		0},
        {"Cc",			0},
        {"From",		0},
        {"To",			0},
        {"size",		0},
	{"OrderedSubj",		0},
	{"tHread",		0},
        {"Arrival/Reverse",	0},
        {"Date/Reverse",	0},
        {"Subject/Reverse",	0},
        {"Cc/Reverse",		0},
        {"From/Reverse",	0},
        {"To/Reverse",		0},
        {"size/Reverse",	0},
	{"tHread/Reverse",	0},
	{"OrderedSubj/Reverse",	0}
    };

    return((index >= 0 && index < (sizeof(is_rules)/sizeof(is_rules[0])))
	   ? &is_rules[index] : NULL);
}

NAMEVAL_S   *wp_indexheight_rules(index)
     int index;
{
    static NAMEVAL_S is_rules[] = {
	{"normal font",   "24",   0},
	{"smallest font", "20",   0},
	{"small font",    "22",   0},
	{"large font",    "28",   0},
	{"largest font",  "30",   0}
    };

    return((index >= 0 && index < (sizeof(is_rules)/sizeof(is_rules[0])))
	   ? &is_rules[index] : NULL);
}


/*
 * PEDebugCmd - turn on/off and set various debugging options
 */
int
PEDebugCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *s;

    if(!--objc){		/* only one arg? */
	Tcl_WrongNumArgs(interp, 1, objv, "?args?");
    }
    else if(s = Tcl_GetStringFromObj(objv[1], NULL)){
	if(!strucmp(s, "level")){
	    if(objc == 2){
		int level;

		if(Tcl_GetIntFromObj(interp, objv[2], &level) != TCL_OK)
		  return(TCL_ERROR);

		if(level > 0){
		    if(level > 10)
		      level = 10;

		    debug = level;
		    dprint((1, "Debug level %d", level));
		}
		else{
		    dprint((1, "PEDebug ending"));
		    debug = 0;
		}
	    }

	    Tcl_SetResult(interp, int2string(debug), TCL_VOLATILE);
	    return(TCL_OK);
	}
	else if(!strucmp(s, "write")){
	    if(objc == 2 && (s = Tcl_GetStringFromObj(objv[2], NULL))){
		/*
		 * script debugging has a high priority since
		 * statements can be added/removed on the fly
		 * AND are NOT present by default
		 */
		dprint((SYSDBG_INFO, "SCRIPT: %s", s));
	    }

	    return(TCL_OK);
	}
	else if(!strucmp(s, "imap")){
	    int level;

	    if(Tcl_GetIntFromObj(interp, objv[2], &level) != TCL_OK)
	      return(TCL_ERROR);

	    if(level == 0){
		if(ps_global){
		    ps_global->debug_imap = 0;
		    if(ps_global->mail_stream)
		      mail_nodebug(ps_global->mail_stream);
		}
	    }
	    else if(level > 0 && level < 5){
		if(ps_global){
		    ps_global->debug_imap = level;
		    if(ps_global->mail_stream)
		      mail_debug(ps_global->mail_stream);
		}
	    }

	    return(TCL_OK);
	}
	else
	  Tcl_SetResult(interp, "Unknown PEDebug request", TCL_STATIC);
    }

    return(TCL_ERROR);
}


/*
 * PESessionCmd - Export TCL Session-wide command set
 */
int
PESessionCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *op, *err = "Unknown PESession option";
    char *pe_user, *pe_host;
    int	  pe_alt;

    dprint((2, "PEConfigCmd"));

    if(op = Tcl_GetStringFromObj(objv[1], NULL)){
	if(!strcmp(op, "open")){
	    char *s, *pinerc, *pineconf = NULL;

	    /*
	     * CMD: open user remote-pinerc local-default-config
	     *
	     * Initiate a session
	     *
	     * Returns: error string on error, nothing otherwise
	     */

	    if(objc < 4 || objc > 5){
		Tcl_WrongNumArgs(interp, 1, objv, "user password pinerc");
		return(TCL_ERROR);
	    }

	    if(!(s = Tcl_GetStringFromObj(objv[2], NULL))){
		Tcl_SetResult(interp, "Unknown User", TCL_STATIC);
		return(TCL_ERROR);
	    }
	    else{
		pe_user = cpystr(s);
		if(setenv("WPUSER", pe_user, 1)){
		    Tcl_SetResult(interp, "Insufficient Environment Space", TCL_STATIC);
		    return(TCL_ERROR);
		}
	    }

	    if(pinerc = Tcl_GetStringFromObj(objv[3], NULL)){
		NETMBX  mb;

		if(mail_valid_net_parse(pinerc, &mb)){
		    pe_host = cpystr(mb.host);
		    pe_alt = (mb.sslflag || mb.tlsflag);
		}
		else {
		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Non-Remote Config: %s", pinerc);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
	    }
	    else {
		Tcl_SetResult(interp, "Unknown config location", TCL_STATIC);
		return(TCL_ERROR);
	    }

	    if(objc == 5 && !(pineconf = Tcl_GetStringFromObj(objv[4], NULL))){
		Tcl_SetResult(interp, "Can't determine global config", TCL_STATIC);
		return(TCL_ERROR);
	    }

	    dprint((1, "PESession: user: %s\n\tpinerc: %s\n\tpineconf: %s",
		       pe_user, pinerc, pineconf ? pineconf : "<none>"));

	    /* credential cache MUST already be seeded */

	    /* destroy old user context */
	    if(ps_global){
		/* destroy open stream */
		peDestroyStream(ps_global);

		/* destroy old user context */
		peDestroyUserContext(&ps_global);
	    }

	    /* Establish a user context */
	    if(s = peCreateUserContext(interp, pe_user, pinerc, pineconf)){
		Tcl_SetResult(interp, s, TCL_VOLATILE);
		return(TCL_ERROR);
	    }

	    fs_give((void **) &pe_user);
	    fs_give((void **) &pe_host);

	    return(peCreateStream(interp, ps_global->context_list, "INBOX", TRUE));
	}
	else if(!strcmp(op, "close")){
	    if(ps_global){
		/* destroy any open stream */
		peDestroyStream(ps_global);
		
		/* destroy user context */
		peDestroyUserContext(&ps_global);
	    }
    
	    Tcl_SetResult(interp, "BYE", TCL_STATIC);
	    return(TCL_OK);
	}
	else if(!strcmp(op, "creds")){
	    char *folder;
	    int	  colid;

	    if(objc < 4){
		err = "creds: insufficient args";
	    }
	    else if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR
		    && (folder = Tcl_GetStringFromObj(objv[3], NULL))){
		int	   i;
		CONTEXT_S *cp;

		/*
		 * CMD: creds <collection-index> <folder> [user passwd]
		 *
		 * Test for valid credentials to access given folder
		 *
		 * Returns: 1 if so, 0 otherwise
		 */

		for(i = 0, cp = ps_global ? ps_global->context_list : NULL;
		    i < 1 || cp != NULL ;
		    i++, cp = cp->next)
		  if(i == colid){
		      int	  rv = 0;
		      char	  tmp[MAILTMPLEN], *p;

		      if(cp){
			  if(folder[0] == '\0'){
			      if(cp->use & CNTXT_INCMNG)
				rv = 1;
			      else
				folder = "fake-fake";
			  }
			  else if((cp->use & CNTXT_INCMNG)
				  && (p = folder_is_nick(folder, FOLDERS(cp), FN_NONE)))
			    folder = p;
		      }

		      if(!rv && context_allowed(context_apply(tmp, cp, folder, sizeof(tmp)))){
			  NETMBX  mb;

			  if(mail_valid_net_parse(tmp, &mb)){
			      if(objc == 4){		/* check creds */
				  if(!*mb.user && (p = alpine_get_user(mb.host, (mb.sslflag || mb.tlsflag))))
				    strcpy(mb.user, p);

				  if(alpine_have_passwd(mb.user, mb.host, (mb.sslflag || mb.tlsflag)))
				    rv = 1;
			      }
			      else if(objc == 6){	/* set creds */
				  char *user, *passwd;

				  if((user = Tcl_GetStringFromObj(objv[4], NULL))
				     && (passwd = Tcl_GetStringFromObj(objv[5], NULL))){
				      if(*mb.user && strcmp(mb.user, user)){
					  err = "creds: mismatched user names";
					  break;
				      }

				      alpine_set_passwd(user, passwd, mb.host,
							mb.sslflag
							|| mb.tlsflag
							|| (ps_global ? F_ON(F_PREFER_ALT_AUTH, ps_global) : 0));
				      rv = 1;
				  }
				  else {
				      err = "creds: unable to read credentials";
				      break;
				  }
			      }
			      else{
				  err = "creds: invalid args";
				  break;
			      }
			  }
		      }

		      (void) Tcl_ListObjAppendElement(interp,
						      Tcl_GetObjResult(interp),
						      Tcl_NewIntObj(rv));
		      return(TCL_OK);
		  }

		err = "creds: Unrecognized collection ID";
	    }
	    else
	      err = "creds: failure to acquire folder and collection ID";
	}
	else if(!strcmp(op, "nocred")){
	    char *folder;
	    int	  colid;

	    if(!ps_global){
		err = "No Session active";
	    }
	    else if(objc != 4){
		err = "nocred: wrong number of args";
	    }
	    else if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR
		    && (folder = Tcl_GetStringFromObj(objv[3], NULL))){
		int	   i;
		CONTEXT_S *cp;

		/*
		 * CMD: nocred <collection-index> <folder>
		 *
		 * Test for valid credentials to access given folder
		 *
		 * Returns: 1 if so, 0 otherwise
		 */

		for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
		  if(i == colid){
		      int	  rv = 0;
		      char	  tmp[MAILTMPLEN], *p;

		      if((cp->use & CNTXT_INCMNG)
			 && (p = folder_is_nick(folder, FOLDERS(cp), FN_NONE)))
			folder = p;

		      if(context_allowed(context_apply(tmp, cp, folder, sizeof(tmp)))){
			  NETMBX  mb;

			  if(mail_valid_net_parse(tmp, &mb)){
			      if(!*mb.user && (p = alpine_get_user(mb.host, (mb.sslflag || mb.tlsflag))))
				strcpy(mb.user, p);

			      alpine_clear_passwd(mb.user, mb.host);
			  }
		      }

		      (void) Tcl_ListObjAppendElement(interp,
						      Tcl_GetObjResult(interp),
						      Tcl_NewIntObj(rv));
		      return(TCL_OK);
		  }

		err = "creds: Unrecognized collection ID";
	    }
	    else
	      err = "creds: failure to acquire folder and collection ID";
	}
	else if(!strcmp(op, "random")){
	    if(objc != 3){
		err = "PESession: random <length>";
	    } else {
		char s[1025];
		int  l;

		if(Tcl_GetIntFromObj(interp,objv[2],&l) != TCL_ERROR){
		    if(l <= 1024){
			Tcl_SetResult(interp, peRandomString(s,l,PRS_MIXED_CASE), TCL_STATIC);
			return(TCL_OK);
		    }
		    else
		      err = "PESession: random length too long";
		}
		else
		  err = "PESession: can't get random length";
	    }
	}
	else if(!strcmp(op, "authdriver")){
	    if(objc != 4){
		err = "PESession: authdriver {add | remove} drivername";
	    } else {
		char *cmd, *driver;

		if(cmd = Tcl_GetStringFromObj(objv[2], NULL)){
		    if(driver = Tcl_GetStringFromObj(objv[3], NULL)){
			if(!strcmp(cmd,"enable")){
			    err = "PESession: authdriver enable disabled for the nonce";
			}
			else if(!strcmp(cmd,"disable")){
			    if(mail_parameters(NULL, DISABLE_AUTHENTICATOR, (void *) driver)){
				snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Authentication driver %.30s disabled", driver);
				Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
				return(TCL_OK);
			    }
			    else{
				snprintf(tmp_20k_buf, SIZEOF_20KBUF, "PESession: Can't disable %.30s", driver);
				Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
				return(TCL_ERROR);
			    }
			}
			else
			  err = "PESession: unknown authdriver operation";
		    }
		    else
		      err = "PESession: Can't read driver name";
		}
		else
		  err = "PESesions: Can't read authdriver operation";
	    }
	}
	else if(!strcmp(op, "abandon")){
	    /*
	     * CMD: abandon [timeout]
	     *
	     * Returns: nothing
	     */
	    long timeo = 0;

	    if(objc != 3){
		err = "PESession: abandon [timeout]";
	    } else {
		long t;

		if(Tcl_GetLongFromObj(interp, objv[2], &t) == TCL_OK){
		    /* ten second minimum and max of default */
		    if(t > 0 && t <= PE_INPUT_TIMEOUT){
			peAbandonTimeout = t;
			return(TCL_OK);
		    }
		    else
		      err = "unrecognized timeout";
		}
		else
		  err = "Can't read timeout";
	    }
	}
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}



/*
 * PEFolderChange:
 *    call reset_context_folders(cp) to clean up data structures this creates
 * PEMakeFolderString:
 */
int
PEFolderChange(Tcl_Interp *interp, CONTEXT_S *cp, int objc, Tcl_Obj *CONST objv[])
{
  int i;
  FDIR_S *fp;
  char *folder;

  for(i = 0; i < objc; i++) {
    folder = Tcl_GetStringFromObj(objv[i], NULL);
    if(!folder) {
      Tcl_SetResult(interp, "PEFolderChange: Can't read folder", TCL_VOLATILE);
      reset_context_folders(cp);
      return(TCL_ERROR);
    }

    fp = next_folder_dir(cp, folder, FALSE, NULL); /* BUG: mail_stream? */
    fp->desc    = folder_lister_desc(cp, fp);
    fp->delim   = cp->dir->delim;
    fp->prev    = cp->dir;
    fp->status |= CNTXT_SUBDIR;
    cp->dir  = fp;
  }
  
  return(TCL_OK);
}


int
PEMakeFolderString(Tcl_Interp *interp, CONTEXT_S *cp, int objc, Tcl_Obj *CONST objv[], char **ppath)
{
  int i;
  unsigned long size,len;
  char *portion,*path;
  
  size = 0;
  for(i = 0; i < objc; i++) {
    portion = Tcl_GetStringFromObj(objv[i], NULL);
    if(!portion) {
      Tcl_SetResult(interp, "PEMakeFolderString: Can't read folder", 
		    TCL_VOLATILE);
      return(TCL_ERROR);
    }
    if(i) size++;
    size += strlen(portion);
  }

  path = (char*) fs_get(size + 1);
  size = 0;
  for(i = 0; i < objc; i++) {
    portion = Tcl_GetStringFromObj(objv[i], NULL);
    len = strlen(portion);
    if(i) path[size++] = cp->dir->delim;
    memcpy(path + size, portion, len);
    size += len;
  }
  path[size] = '\0';  
  if(ppath) *ppath = path; else fs_give((void**) &path);
  return(TCL_OK);
}


/*
 * PEFolderCmd - export various bits of folder information
 */
int
PEFolderCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *op, errbuf[256], *err = "Unknown PEFolder request";

    dprint((2, "PEFolderCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else if(op = Tcl_GetStringFromObj(objv[1], NULL)){
	if(ps_global && ps_global->mail_stream){
	    if(objc == 2){
		if(!strcmp(op, "current")){
		    /*
		     * CMD: current
		     *
		     * Returns: string representing the name of the
		     *		current mailbox
		     */
		    Tcl_SetResult(interp, ps_global->cur_folder, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "collections")){
		    CONTEXT_S *cp;
		    int	       i;

		    /*
		     * CMD: collections
		     *
		     * Returns: List of currently configured collections
		     */
		    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next){
			Tcl_Obj *objv[3];

			objv[0] = Tcl_NewIntObj(i);
			objv[1] = Tcl_NewStringObj(cp->nickname ? cp->nickname : "", -1);
			objv[2] = Tcl_NewStringObj(cp->label ? cp->label : "", -1);

			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 Tcl_NewListObj(3, objv));
		    }

		    return(TCL_OK);
		}
		else if(!strcmp(op, "clextended")){
		    CONTEXT_S *cp;
		    int	       i;
		    char       tpath[MAILTMPLEN], *p;

		    /*
		     * CMD: clextended
		     *
		     * Returns: Extended list of current collections
		     *
		     * Format:
		     *    0) Collection Number
		     *    1) Nickname
		     *    2) Label
		     *    3) Basically this is a flag to say if we can edit
		     *    4) Server
		     *    5) Path
		     *    6) View
		     */
		    /*
		     * had to get rid of this cause the args are changed
		     *
		     * if(strcmp("extended", 
		     *       Tcl_GetStringFromObj(objv[2], NULL))){
		     * Tcl_SetResult(interp, "invalid argument", TCL_VOLATILE);
		     * return(TCL_ERROR);
		     * }
		     */
		    for(i = 0, cp = ps_global->context_list; cp ;
			i++, cp = cp->next){
			Tcl_Obj *objv[7];

			objv[0] = Tcl_NewIntObj(i);
			objv[1] = Tcl_NewStringObj(cp->nickname ? 
						   cp->nickname : "", -1);
			objv[2] = Tcl_NewStringObj(cp->label ? 
						   cp->label : "", -1);
			objv[3] = Tcl_NewIntObj(cp->var.v ? 1 : 0);
			objv[4] = Tcl_NewStringObj(cp->server ?
						   cp->server : "", -1);
			tpath[0] = '\0';
			if(cp->context){
			    strncpy(tpath, (cp->context[0] == '{'
					   && (p = strchr(cp->context, '}')))
				   ? ++p
				   : cp->context, sizeof(tpath));
			    tpath[sizeof(tpath)-1] = '\0';
			    if(p = strstr(tpath, "%s"))
			      *p = '\0';
			}
			objv[5] = Tcl_NewStringObj(tpath, -1);
			objv[6] = Tcl_NewStringObj(cp->dir && 
						   cp->dir->view.user ? 
						   cp->dir->view.user : 
						   "", -1);
			Tcl_ListObjAppendElement(interp, 
						 Tcl_GetObjResult(interp),
						 Tcl_NewListObj(7, objv));
		    }

		    return(TCL_OK);
		}
	    }
	    else if(objc == 3 && !strcmp(op, "delimiter")){
		int	       colid, i;
		char       delim[2] = {'\0', '\0'};
		CONTEXT_S *cp;

		if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR){
		    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
		      if(i == colid){
			  if(cp->dir && cp->dir->delim)
			    delim[0] = cp->dir->delim;

			  break;
		      }

		    Tcl_SetResult(interp, delim[0] ? delim : "/", TCL_STATIC);
		    return(TCL_OK);
		}
		else
		  err = "PEFolder: delimiter: Can't read collection ID";
	    }
	    else if(objc == 3 && !strcmp(op, "isincoming")){
		int	   colid, i, isincoming = 0;
		CONTEXT_S *cp;

		if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR){
		    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
		      if(i == colid){
			  Tcl_SetResult(interp, int2string(((cp->use & CNTXT_INCMNG) != 0)), TCL_STATIC);
			  return(TCL_OK);
		      }

		    err = "PEFolder: isincoming: Invalid collection ID";
		}
		else
		  err = "PEFolder: isincoming: Can't read collection ID";
	    }
	    else if(!strcmp(op, "export")){
		/*
		 * CMD: export
		 *
		 * Returns: success or failure after writing given
		 *          folder to given local file.
		 *
		 * Format:
		 *    0) Collection Number
		 *    1) Folder
		 *    2) Destination file
		 */
		if(objc == 5){
		    CONTEXT_S  *cp;
		    MAILSTREAM *src;
		    APPEND_PKG	pkg;
		    STRING	msg;
		    long	colid, i, l;
		    char       *folder, *dfile, seq[64], tmp[MAILTMPLEN];
		    int		our_stream = 0;

		    if(Tcl_GetLongFromObj(interp,objv[2],&colid) != TCL_ERROR){
			for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
			  if(i == colid)
			    break;

			if(cp){
			    if(folder = Tcl_GetStringFromObj(objv[3], NULL)){
				if(dfile = Tcl_GetStringFromObj(objv[4], NULL)){
				    if(mail_parameters(NULL, ENABLE_DRIVER, "unix")){

					snprintf(tmp, sizeof(tmp), "#driver.unix/%s", dfile);

					if(pine_mail_create(NULL, tmp)){

					    err = NULL;		/* reset error condition */

					    /*
					     * if not current folder, open a stream, setup the
					     * stuff to write the raw header/text by hand
					     * with berkeley delimiters since we don't want
					     * a local mailbox driver lunk in.
					     * 
					     * comments:
					     *  - BUG: what about logins?
					     *
					     */
					    if(!(context_allowed(context_apply(tmp, cp, folder, sizeof(tmp)))
						 && (src = same_stream_and_mailbox(tmp, ps_global->mail_stream)))){
						long retflags = 0;

						our_stream = 1;
						src = context_open(cp, NULL, folder,
								   OP_READONLY | OP_SHORTCACHE,
								   &retflags);
					    }

					    if(src && src->nmsgs){
						/* Go to work...*/
						pkg.stream = src;
						pkg.msgno  = 0;
						pkg.msgmax = src->nmsgs;
						pkg.flags = pkg.date = NIL;
						pkg.message = &msg;

						snprintf (seq,sizeof(seq),"1:%lu",src->nmsgs);
						mail_fetchfast (src, seq);

						ps_global->noshow_error = 1;
						if(!mail_append_multiple (NULL, dfile,
									  peAppendMsg, (void *) &pkg)){
						    snprintf(err = errbuf, sizeof(errbuf), "PEFolder: export: %.200s",
							    ps_global->c_client_error);
						}

						ps_global->noshow_error = 0;

						if(our_stream)
						  pine_mail_close(src);
					    }
					    else
					      err = "PEFolder: export: can't open mail folder";

					    if(!err)
					      return(TCL_OK);
					}
					else
					  err = "PEFolder: export: can't create destination";

					if(!mail_parameters(NULL, DISABLE_DRIVER, "unix"))
					  err = "PEFolder: export: can't disable driver";
				    }
				    else
				      err = "PEFolder: export: can't enable driver";
				}
				else
				  err = "PEFolder: export: can't read file name";
			    }
			    else
			      err = "PEFolder: export: can't read folder name";
			}
			else
			  err = "PEFolder: export: Invalid collection ID";
		    }
		    else
		      err = "PEFolder:export: Can't read collection ID";
		}
		else
		  err = "PEFolder: export <colid> <folder> <file>";
	    }
	    else if(!strcmp(op, "import")){
		/*
		 * CMD: import
		 *
		 * Returns: success or failure after writing given
		 *          folder to given local file.
		 *
		 * Format:
		 *    0) source file
		 *    1) destination collection number
		 *    2) destination folder
		 */
		if(objc == 5){
		    CONTEXT_S  *cp;
		    MAILSTREAM *src, *dst;
		    APPEND_PKG	pkg;
		    STRING	msg;
		    long	colid, i, l;
		    char       *folder, *sfile, seq[64];
		    int		our_stream = 0;

		    /* get source file with a little sanity check */
		    if((sfile = Tcl_GetStringFromObj(objv[2], NULL))
		       && *sfile == '/' && !strstr(sfile, "..")){
			if(mail_parameters(NULL, ENABLE_DRIVER, "unix")){

			    ps_global->noshow_error = 1;	/* don't queue error msg */
			    err = NULL;				/* reset error condition */

			    /* make sure sfile contains valid mail */
			    if(src = mail_open(NULL, sfile, 0L)){

				if(Tcl_GetLongFromObj(interp,objv[3],&colid) != TCL_ERROR){
				    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
				      if(i == colid)
					break;

				    if(cp){
					if(folder = Tcl_GetStringFromObj(objv[4], NULL)){
					    long retflags = 0;

					    if(context_create(cp, NULL, folder)
					       && (dst = context_open(cp, NULL, folder, 0L, &retflags))){

						if(src->nmsgs){
						    /* Go to work...*/
						    pkg.stream = src;
						    pkg.msgno = 0;
						    pkg.msgmax = src->nmsgs;
						    pkg.flags = pkg.date = NIL;
						    pkg.message = &msg;

						    snprintf (seq,sizeof(seq),"1:%lu",src->nmsgs);
						    mail_fetchfast (src, seq);

						    if(!context_append_multiple(cp, dst, folder,
										peAppendMsg, (void *) &pkg,
										ps_global->mail_stream)){
							snprintf(err = errbuf, sizeof(errbuf), "PEFolder: import: %.200s",
								ps_global->c_client_error);
						    }

						}

						pine_mail_close(dst);
					    }
					    else
					      snprintf(err = errbuf, sizeof(errbuf), "PEFolder: import: %.200s",
						      ps_global->c_client_error);
					}
					else
					  err = "PEFolder: import: can't read folder name";
				    }
				    else
				      err = "PEFolder:import: invalid collection id";
				}
				else
				  err = "PEFolder: import: can't read collection id";

				mail_close(src);

			    }
			    else
			      snprintf(err = errbuf, sizeof(errbuf), "PEFolder: import: %.200s",
				      ps_global->c_client_error);

			    ps_global->noshow_error = 0;

			    if(!mail_parameters(NULL, DISABLE_DRIVER, "unix") && !err)
			      err = "PEFolder: import: can't disable driver";

			    if(!err)
			      return(TCL_OK);
			}
			else
			  err = "PEFolder: import: can't enable driver";
		    }
		    else
		      err = "PEFolder: import: can't read file name";
		}
		else
		  err = "PEFolder: import <file> <colid> <folder>";
	    }
	    else {
	      int i, colid;
	      char *colstr;
	      CONTEXT_S *cp;
	      
	      /*
	       * 3 or more arguments, 3rd is the collection ID, rest
	       * are a folder name
	       */
	      
	      if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR){
		for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
		  if(i == colid) break;
	      }
	      else if(colstr = Tcl_GetStringFromObj(objv[2], NULL)){
		  if(!strcmp("default", colstr))
		    cp = default_save_context(ps_global->context_list);
		  else
		    cp = NULL;
	      }
	      else
		cp = NULL;
	      
	      if(cp){
		if(!strcmp(op, "list")){
		  int i, fcount;
		  char dir[3];
		  
		  ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
		  if(PEFolderChange(interp, cp, objc - 3, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;
		  
		  peNoPassword = 0;  		  
		  build_folder_list(NULL, cp, "*", NULL, (cp->use & CNTXT_NEWS) ? BFL_LSUB : BFL_NONE);
		  if(peNoPassword){
		    Tcl_SetResult(interp, AUTH_FAILURE_STRING, TCL_VOLATILE);
		    reset_context_folders(cp);
		    return(TCL_ERROR);
		  }
		  
		  if(fcount = folder_total(FOLDERS(cp))){
		    for(i = 0; i < fcount; i++){
		      char type[3], *p;
		      FOLDER_S *f = folder_entry(i, FOLDERS(cp));
		      
		      p = type;
		      if(f->isdir)
			*p++ = 'D';
		      
		      if(f->isfolder
			 || f->nickname
			 || (cp->use & CNTXT_INCMNG))
			*p++ = 'F';
		      
		      *p = '\0';
		      
		      peAppListStrStr(interp, Tcl_GetObjResult(interp), type,
				      f->nickname ? f->nickname : f->name);
		    }
		  }
		  
		  reset_context_folders(cp);
		  return(TCL_OK);
		}
		else if(!strucmp(op, "exists")){
		  char *folder, *errstr = NULL;
		  int   rv;

		  if(objc < 4) {
		    Tcl_SetResult(interp, "PEFolder exists: No folder specified", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  folder = Tcl_GetStringFromObj(objv[objc - 1], NULL);
		  if(!folder) {
		    Tcl_SetResult(interp, "PEFolder exists: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  
		  if(PEFolderChange(interp, cp, objc - 4, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;

		  ps_global->c_client_error[0] = '\0';
		  peNoPassword = peCredentialError = 0;

		  rv = folder_name_exists(cp, folder, NULL);

		  if(rv & FEX_ERROR){
		      if(peNoPassword || peCredentialError)
			errstr = AUTH_FAILURE_STRING;
		      else if(ps_global->c_client_error[0])
			errstr = ps_global->c_client_error;
		      else
			errstr = "Indeterminate Error";
		  }

		  Tcl_SetResult(interp, errstr ? errstr : int2string((int)(rv & FEX_ISFILE)), TCL_VOLATILE);
		  return(errstr ? TCL_ERROR : TCL_OK);
		}
		else if(!strucmp(op, "fullname")){
		  char *folder, *fullname;
		  
		  if(objc < 4) {
		    Tcl_SetResult(interp, "PEFolder fullname: No folder specified", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  folder = Tcl_GetStringFromObj(objv[objc - 1], NULL);
		  if(!folder) {
		    Tcl_SetResult(interp, "PEFolder fullname: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  
		  if(PEFolderChange(interp, cp, objc - 4, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;
		  
#if	0
		  Tcl_Obj *obj = Tcl_NewStringObj((fullname = folder_is_nick(folder, FOLDERS(cp)))
						  ? fullname : folder, -1);
		  (void) Tcl_ListObjAppendElement(interp,
						  Tcl_GetObjResult(interp),
						  obj);
#else
		  Tcl_SetResult(interp,
				(fullname = folder_is_nick(folder, FOLDERS(cp), FN_NONE)) ? fullname : folder,
				TCL_VOLATILE);
#endif		
		  
		  return(TCL_OK);
		}
		else if(!strucmp(op, "create")){
		  char *folder;
		  FDIR_S *old_fp;
		  
		  folder = Tcl_GetStringFromObj(objv[objc - 1], NULL);
		  if(!folder) {
		    Tcl_SetResult(interp, "PEFolder create: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  
		  if(PEFolderChange(interp, cp, objc - 4, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;
		  
		  ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
		  peNoPassword = 0;
		  
		  if(!context_create(cp, NULL, folder)){
		    if(peNoPassword){
		      Tcl_SetResult(interp, AUTH_FAILURE_STRING, TCL_VOLATILE);
		    }
		    else{
		      Tcl_SetResult(interp,
				    (ps_global->last_error[0])
				     ? ps_global->last_error
				     : (ps_global->c_client_error[0])
				        ? ps_global->c_client_error
				        : "Unable to create folder",
				    TCL_VOLATILE);
		    }
		    reset_context_folders(cp);
		    return(TCL_ERROR);
		  }
		  Tcl_SetResult(interp, "OK", TCL_STATIC);
		  reset_context_folders(cp);
		  return(TCL_OK);
		}
		else if(!strucmp(op, "delete")){
		  char *folder;
		  FDIR_S *old_fp;
		  
		  folder = Tcl_GetStringFromObj(objv[objc - 1], NULL);
		  if(!folder) {
		    Tcl_SetResult(interp, "PEFolder delete: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  
		  if(PEFolderChange(interp, cp, objc - 4, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;
		  
		  ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';

		  peNoPassword = 0;
		  if(!context_delete(cp, NULL, folder)){
		    if(peNoPassword){
		      Tcl_SetResult(interp, AUTH_FAILURE_STRING, TCL_VOLATILE);
		    }
		    else{
		      Tcl_SetResult(interp,
				    (ps_global->last_error[0])
				     ? ps_global->last_error
				     : (ps_global->c_client_error[0])
				        ? ps_global->c_client_error
				        : "Unable to delete folder",
				    TCL_VOLATILE);
		    }
		    reset_context_folders(cp);
		    return(TCL_ERROR);
		  }
		  Tcl_SetResult(interp, "OK", TCL_STATIC);
		  reset_context_folders(cp);
		  return(TCL_OK);
		}
		/*
		 * must be at least 5 arguments for the next set of commands
		 */
		else if(objc < 5) {
		  Tcl_SetResult(interp, "PEFolder: not enough arguments", TCL_VOLATILE);
		  return(TCL_ERROR);
		}
		else if(!strucmp(op, "rename")){
		  char *folder,*newfolder;
		  FDIR_S *old_fp;
		  
		  folder = Tcl_GetStringFromObj(objv[objc - 2], NULL);
		  if(!folder) {
		    Tcl_SetResult(interp, "PEFolder rename: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }

		  newfolder = Tcl_GetStringFromObj(objv[objc - 1], NULL);
		  if(!newfolder) {
		    Tcl_SetResult(interp, "PEFolder rename: Can't read folder", TCL_VOLATILE);
		    return(TCL_ERROR);
		  }
		  
		  if(PEFolderChange(interp, cp, objc - 5, objv + 3) == TCL_ERROR)
		    return TCL_ERROR;
		  
		  ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
		  peNoPassword = 0;
		  
		  if(!context_rename(cp, NULL, folder, newfolder)){
		    if(peNoPassword){
		      Tcl_SetResult(interp, AUTH_FAILURE_STRING, TCL_VOLATILE);
		    }
		    else{
		      Tcl_SetResult(interp,
				    (ps_global->last_error[0])
				     ? ps_global->last_error
				     : (ps_global->c_client_error[0])
				        ? ps_global->c_client_error
				        : "Unable to rename folder",
				    TCL_VOLATILE);
		    }
		    reset_context_folders(cp);
		    return(TCL_ERROR);
		  }
		  Tcl_SetResult(interp, "OK", TCL_STATIC);
		  reset_context_folders(cp);
		  return(TCL_OK);
		}
	      }
	      else
		err = "PEFolder: Unrecognized collection ID";
	    }
	}
	else
	  err = "No Mailbox Currently Open";
    }

    Tcl_SetResult(interp, err, TCL_VOLATILE);
    return(TCL_ERROR);
}


/*
 * PEMailboxCmd - export various bits of mailbox information
 */
int
PEMailboxCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *op, errbuf[256], *err = "Unknown PEMailbox request";

    dprint((5, "PEMailboxCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else if(op = Tcl_GetStringFromObj(objv[1], NULL)){
	if(ps_global && ps_global->mail_stream){
	    if(!strcmp(op, "select")){
		return(peSelect(interp, objc - 2, &((Tcl_Obj **) objv)[2]));
	    }
	    else if(!strucmp(op, "apply")){
		return(peApply(interp, objc - 2, &((Tcl_Obj **) objv)[2]));
	    }
	    else if(!strucmp(op, "open")){
		    int	       i, colid;
		    char      *folder;
		    CONTEXT_S *cp;
	      
		    /*
		     * CMD: open <context-index> <folder>
		     *
		     * 
		     */
		    if(objc == 2){
			Tcl_SetResult(interp, (!sp_dead_stream(ps_global->mail_stream)) ? "0" : "1", TCL_VOLATILE);
			return(TCL_OK);
		    }

		    if(Tcl_GetIntFromObj(interp,objv[2],&colid) != TCL_ERROR){
			if(folder = Tcl_GetStringFromObj(objv[objc - 1], NULL)) {
			    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
			      if(i == colid) {
				  if(PEMakeFolderString(interp, cp, objc - 3, objv + 3,
							&folder))
				    return TCL_ERROR;
		      
				  dprint((1, "* PEMailbox open dir=%s folder=%s",cp->dir->ref,folder));
		      
				  return(peCreateStream(interp, cp, folder, FALSE));
			      }

			    err = "open: Unrecognized collection ID";
			}
			else
			  err = "open: Can't read folder";
		    }
		    else
		      err = "open: Can't get collection ID";
	    }
	    else if(!strcmp(op, "expunge")){
		/*
		 * CMD: expunge
		 *
		 * Returns: OK after having removed deleted messages
		 */
		char       *streamstr = NULL;
		MAILSTREAM *stream;
		MSGNO_S    *msgmap;

		if(objc == 3) streamstr = Tcl_GetStringFromObj(objv[2], NULL);
		if(!streamstr
		   || (streamstr && (strcmp(streamstr, "current") == 0))){
		    stream = ps_global->mail_stream;
		    msgmap = sp_msgmap(stream);
		}
		else if(streamstr && (strcmp(streamstr, "inbox") == 0)){
		    stream = sp_inbox_stream();
		    msgmap = sp_msgmap(stream);
		}
		else return(TCL_ERROR);
		ps_global->last_error[0] = '\0';
		if(IS_NEWS(stream)
		   && stream->rdonly){
		    msgno_exclude_deleted(stream, msgmap);
		    clear_index_cache(sp_inbox_stream(), 0);

		    /*
		     * This is kind of surprising at first. For most sort
		     * orders, if the whole set is sorted, then any subset
		     * is also sorted. Not so for OrderedSubject sort.
		     * If you exclude the first message of a subject group
		     * then you change the date that group is to be sorted on.
		     */
		    if(mn_get_sort(msgmap) == SortSubject2)
		      refresh_sort(ps_global->mail_stream, msgmap, FALSE);
		}
		else
		  (void) cmd_expunge_work(stream, msgmap);

		Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
		return(TCL_OK);
	    }
	    else if(!strcmp(op, "nextvector")){
		long	 msgno, count, countdown;
		int	 i, aObjN = 0;
		char	*errstr = NULL, *s, *charset = NULL, csbuf[256];
		Tcl_Obj *rvObj, *vObj, *avObj, **aObj;

		/*
		 * CMD: nextvector
		 *
		 * ARGS: msgno - message number "next" is relative to
		 *       count - how many msgno slots to return
		 *	     attrib  - (optional) attributes to be returned with each message in vector
		 *
		 * Returns: vector containing next <count> messagenumbers (and optional attributes)
		 */
		if(objc == 4 || objc == 5){
		    if(Tcl_GetLongFromObj(interp, objv[2], &msgno) == TCL_OK){
			if(Tcl_GetLongFromObj(interp, objv[3], &count) == TCL_OK){
			    if(objc == 4 || Tcl_ListObjGetElements(interp, objv[4], &aObjN, &aObj) == TCL_OK){

				if(count > 0
				   && !(msgno < 1L || msgno > mn_get_total(sp_msgmap(ps_global->mail_stream)))
				   && (rvObj = Tcl_NewListObj(0, NULL))){

				    mn_set_cur(ps_global->msgmap, msgno);

				    for(countdown = count; countdown > 0; countdown--){
					long  n = mn_get_cur(ps_global->msgmap);
					long  uid = mail_uid(ps_global->mail_stream, mn_m2raw(ps_global->msgmap, n));
					char *cs;

					if(vObj = Tcl_NewListObj(0, NULL)){
					    Tcl_ListObjAppendElement(interp, vObj, Tcl_NewLongObj(n));
					    Tcl_ListObjAppendElement(interp, vObj, Tcl_NewLongObj(uid));

					    if(aObjN){
						if(avObj = Tcl_NewListObj(0, NULL)){
						    for(i = 0; i < aObjN; i++){
							if(s = Tcl_GetStringFromObj(aObj[i], NULL)){
							    if(!strcmp(s, "statusbits")){
								char *s = peMsgStatBitString(ps_global, ps_global->mail_stream,
											     ps_global->msgmap, peMessageNumber(uid),
											     peITop, peICount);
								Tcl_ListObjAppendElement(interp, avObj, Tcl_NewStringObj(s, -1));
							    }
							    else if(!strcmp(s, "status")){
								long	      raw;
								char	      stat[3];
								MESSAGECACHE *mc;

								raw = peSequenceNumber(uid);

								if(!((mc = mail_elt(ps_global->mail_stream, raw)) && mc->valid)){
								    mail_fetch_flags(ps_global->mail_stream,
										     long2string(uid), FT_UID);
								    mc = mail_elt(ps_global->mail_stream, raw);
								}

								stat[0] = mc->deleted ? '1' : '0';
								stat[1] = mc->recent ? '1' : '0';
								stat[2] = mc->seen ? '1' : '0';

								Tcl_ListObjAppendElement(interp, avObj, Tcl_NewStringObj(stat,3));
							    }
							    else if(!strcmp(s, "indexparts")){
								Tcl_Obj *iObj;

								if((iObj = Tcl_NewListObj(0, NULL)) != NULL
								   && peAppendIndexParts(interp, uid, iObj) == TCL_OK
								   && Tcl_ListObjAppendElement(interp, avObj, iObj) == TCL_OK){
								}
								else
								  return(TCL_ERROR);

							    }
							}
							else{
							    errstr = "nextvector: can't read attributes";
							    break;
							}
						    }

						    Tcl_ListObjAppendElement(interp, vObj, avObj);
						}
						else{
						    errstr = "nextvector: can't allocate attribute return vector";
						    break;
						}
					    }
					}
					else{
					    errstr = "nextvector: can't allocate new vector";
					    break;
					}

					Tcl_ListObjAppendElement(interp, rvObj, vObj);

					mn_inc_cur(ps_global->mail_stream, ps_global->msgmap, MH_NONE);

					if(n == mn_get_cur(ps_global->msgmap))
					  break;
				    }
				}

				if(!errstr){
				    /* append result vector */
				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), rvObj);
				    /* Everything is coerced to UTF-8 */
				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							     Tcl_NewStringObj("UTF-8", -1));
				    return(TCL_OK);
				}
			    }
			    else
			      errstr = "nextvector: can't read attribute list";
			}
			else
			  errstr = "nextvector: can't read count";
		    }
		    else
		      errstr = "nextvector: can't read message number";
		}
		else
		  errstr = "nextvector: Incorrect number of arguments";

		if(errstr)
		  Tcl_SetResult(interp, errstr, TCL_STATIC);

		return(TCL_ERROR);
	    }
	    else if(objc == 2){
		if(!strcmp(op, "messagecount")){
		    /*
		     * CMD: messagecount
		     *
		     * Returns: count of messsages in open mailbox
		     */
		    Tcl_SetResult(interp,
				  long2string(mn_get_total(ps_global->msgmap)),
				  TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "firstinteresting")){
		    /*
		     * CMD: firstinteresting
		     *
		     * Returns: message number associated with
		     *		"incoming-startup-rule" which had better
		     *		be the "current" message since it was set
		     *		in do_broach_folder and shouldn't have been
		     *		changed otherwise (expunged screw us?)
		     */
		    Tcl_SetResult(interp,
				  long2string(mn_get_cur(ps_global->msgmap)),
				  TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "selected")){
		    /*
		     * CMD: selected
		     *
		     * Returns: count of selected messsages in open mailbox
		     */

		    Tcl_SetResult(interp,
				  long2string(any_lflagged(ps_global->msgmap, MN_SLCT)),
				  TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "mailboxname")){
		    /*
		     * CMD: name
		     *
		     * Returns: string representing the name of the
		     *		current mailbox
		     */
		    Tcl_SetResult(interp, ps_global->cur_folder, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "indexformat")){
		    /*
		     * CMD: indexformat
		     *
		     * Returns: list of lists where:
		     *		*  the first element is the name of the
		     *		   field which may be "From", "Subject"
		     *		   "Date" or the emtpy string.
		     *		*  the second element which is either
		     *		   the percentage width or empty string
		     */
		    return(peIndexFormat(interp));
		}
		else if(!strcmp(op, "close")){
		    /*
		     * CMD: close
		     *
		     * Returns: with global mail_stream closed
		     */
		    peDestroyStream(ps_global);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "newmailreset")){
		    sml_seen();
		    zero_new_mail_count();
		    sp_set_mail_box_changed(ps_global->mail_stream, 0);
		    sp_set_expunge_count(ps_global->mail_stream, 0L);
		    peMarkInputTime();
		    return(TCL_OK);
		}
		else if(!strcmp(op, "newmailstatmsg")){
		    long newest, count;
		    char subject[500], from[500],
			 intro[500], *s = "";

		    /*
		     * CMD: newmailstatmsg
		     *
		     * ARGS: none
		     *
		     * Returns: text for new mail message
		     *          
		     */

		    if(sp_mail_box_changed(ps_global->mail_stream)
		       && (count = new_mail_count())){

			for(newest = ps_global->mail_stream->nmsgs; newest > 1L; newest--)
			  if(!get_lflag(ps_global->mail_stream, NULL, newest, MN_EXLD))
			    break;

			if(newest){
			    format_new_mail_msg(NULL, count,
						pine_mail_fetchstructure(ps_global->mail_stream,
								    newest, NULL),
						intro, from, subject, NULL, sizeof(subject));

			    snprintf(s = tmp_20k_buf, SIZEOF_20KBUF, "%s %s %s", intro, from, subject);
			}
		    }

		    Tcl_SetResult(interp, s, TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "savedefault")){
		    return(peSaveDefault(interp, 0L, 0, NULL));
		}
		else if(!strcmp(op, "gotodefault")){
		    return(peGotoDefault(interp, 0L, NULL));
		}
		else if(!strcmp(op, "zoom")){
		    Tcl_SetResult(interp,
				  long2string((any_lflagged(ps_global->msgmap, MN_HIDE) > 0L)
					        ? any_lflagged(ps_global->msgmap, MN_SLCT) : 0L),
				  TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "first")){
		    if(any_lflagged(ps_global->msgmap, MN_HIDE)){
			long n;

			for(n = 1L; n <= mn_get_total(ps_global->msgmap); n++)
			  if(!get_lflag(ps_global->mail_stream, ps_global->msgmap, n, MN_HIDE)){
			      Tcl_SetResult(interp, long2string(n), TCL_VOLATILE);
			      return(TCL_OK);
			  }

			unzoom_index(ps_global, ps_global->mail_stream, ps_global->msgmap);
			
		    }

		    Tcl_SetResult(interp, int2string(1), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "last")){
		    if(any_lflagged(ps_global->msgmap, MN_HIDE)){
			long n;

			for(n = mn_get_total(ps_global->msgmap); n > 0L; n--)
			  if(!get_lflag(ps_global->mail_stream, ps_global->msgmap, n, MN_HIDE)){
			      Tcl_SetResult(interp, long2string(n), TCL_VOLATILE);
			      return(TCL_OK);
			  }
		    }
		    else{
			Tcl_SetResult(interp, long2string(mn_get_total(ps_global->msgmap)), TCL_VOLATILE);
			return(TCL_OK);
		    }

		    Tcl_SetResult(interp, "Can't set last message number", TCL_STATIC);
		    return(TCL_ERROR);
		}
		else if(!strucmp(op, "sortstyles")){
		    int	       i;
		    /*
		     * CMD: sortstyles
		     *
		     * Returns: list of supported sort styles
		     */

		    for(i = 0; ps_global->sort_types[i] != EndofList; i++)
		      if(Tcl_ListObjAppendElement(interp,
						  Tcl_GetObjResult(interp),
						  Tcl_NewStringObj(sort_name(ps_global->sort_types[i]), -1)) != TCL_OK)
			return(TCL_ERROR);

		    return(TCL_OK);
		}
		else if(!strucmp(op, "sort")){
		    return(peAppendCurrentSort(interp));
		}
		else if(!strucmp(op, "state")){
		    if(!ps_global->mail_stream || sp_dead_stream(ps_global->mail_stream))
		      Tcl_SetResult(interp, "closed", TCL_STATIC);
		    else if(ps_global->mail_stream->rdonly && !IS_NEWS(ps_global->mail_stream))
		      Tcl_SetResult(interp, "readonly", TCL_STATIC);
		    else
		      Tcl_SetResult(interp, "ok", TCL_STATIC);

		    return(TCL_OK);
		}
	    }
	    else if(objc == 3){
		if(!strcmp(op, "uid")){
		    long msgno, raw;

		    /*
		     * Return uid of given message number
		     * 
		     * CMD: uid <msgnumber>
		     */

		    if(Tcl_GetLongFromObj(interp, objv[2], &msgno) != TCL_OK)
		      return(TCL_ERROR); /* conversion problem? */

		    if((raw = mn_m2raw(ps_global->msgmap, msgno)) > 0L){
			raw = mail_uid(ps_global->mail_stream, raw);
			Tcl_SetResult(interp, long2string(raw), TCL_VOLATILE);
			return(TCL_OK);
		    }

		    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Invalid UID for message %ld", msgno);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		else if(!strcmp(op, "newmail")){
		    int		  reload, force = FALSE, rv;
		    static time_t last_check = 0;
		    time_t	  now = time(0);

		    /*
		     * CMD: newmail
		     *
		     * ARGS: reload -- "1" if we're reloading
		     *       (vs. just checking newmail as a side effect
		     *        of building a new page)
		     *
		     * Returns: count -
		     *          mostrecent - 
		     */
		    if(Tcl_GetIntFromObj(interp, objv[2], &reload) == TCL_ERROR)
		      reload = 0;

		    if(!last_check || now - last_check > 15)
		      force = TRUE;

		    last_check = now;

		    /* check for new mail */
		    new_mail(force, reload ? 0 : 2, NM_STATUS_MSG);

		    rv = peNewMailResult(interp);

		    if(!reload)
		      peMarkInputTime();

		    return(rv);
		}
		else if(!strcmp(op, "flagcount")){
		    char     *flag;
		    long      count = 0L;
		    long      flags = 0L;
		    int	      objlc;
		    Tcl_Obj **objlv;
		    

		    /*
		     * CMD: flagcount
		     *
		     * ARGS: flags - 
		     *
		     * Returns: count - number of message thusly flagged
		     *          mostrecent - 
		     */
		    if(Tcl_ListObjGetElements(interp, objv[2], &objlc, &objlv) == TCL_OK){
			while(objlc--)
			  if(flag = Tcl_GetStringFromObj(*objlv++, NULL)){
			      if(!strucmp(flag, "deleted")){
				  flags |= F_DEL;
			      }
			      if(!strucmp(flag, "undeleted")){
				  flags |= F_UNDEL;
			      }
			      else if(!strucmp(flag, "seen")){
				  flags |= F_SEEN;
			      }
			      else if(!strucmp(flag, "unseen")){
				  flags |= F_UNSEEN;
			      }
			      else if(!strucmp(flag, "flagged")){
				  flags |= F_FLAG;
			      }
			      else if(!strucmp(flag, "unflagged")){
				  flags |= F_UNFLAG;
			      }
			      else if(!strucmp(flag, "answered")){
				  flags |= F_ANS;
			      }
			      else if(!strucmp(flag, "unanswered")){
				  flags |= F_UNANS;
			      }
			      else if(!strucmp(flag, "recent")){
				  flags |= F_RECENT;
			      }
			      else if(!strucmp(flag, "unrecent")){
				  flags |= F_UNRECENT;
			      }
			  }

			if(flags)
			  count = count_flagged(ps_global->mail_stream, flags);
		    }

		    Tcl_SetResult(interp, long2string(count), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "zoom")){
		    int  newstate;
		    long n, zoomed = 0L;

		    /*
		     * CMD: zoom
		     *
		     *    Set/clear HID bits of non SLCT messages as requested.
		     *    PEMailbox [first | last | next] are senstive to these flags.
		     *
		     * ARGS: newstate - 1 or 0
		     *
		     * Returns: count of zoomed messages
		     */

		    if(Tcl_GetIntFromObj(interp, objv[2], &newstate) != TCL_ERROR){
			if(newstate > 0){
			    if(any_lflagged(ps_global->msgmap, MN_HIDE) != (mn_get_total(ps_global->msgmap) - (n = any_lflagged(ps_global->msgmap, MN_SLCT)))){
				zoom_index(ps_global, ps_global->mail_stream, ps_global->msgmap);
				zoomed = n;
			    }
			}
			else{
			    if(any_lflagged(ps_global->msgmap, MN_HIDE))
			      unzoom_index(ps_global, ps_global->mail_stream, ps_global->msgmap);
			}
		    }

		    Tcl_SetResult(interp, long2string(zoomed), TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(op, "next")){
		    long  msgno;

		    /*
		     * CMD: next
		     *
		     * ARGS: msgno - message number "next" is relative to
		     *
		     * Returns: previous state
		     */

		    if(Tcl_GetLongFromObj(interp, objv[2], &msgno) != TCL_ERROR){
			mn_set_cur(ps_global->msgmap, msgno);
			mn_inc_cur(ps_global->mail_stream, ps_global->msgmap, MH_NONE);
			Tcl_SetResult(interp, long2string(mn_get_cur(ps_global->msgmap)), TCL_VOLATILE);
			return(TCL_OK);
		    }

		    Tcl_SetResult(interp, "next can't read message number", TCL_STATIC);
		    return(TCL_ERROR);
		}
	    }
	    else if(objc == 4){
		if(!strucmp(op, "indexrange")){
		    long       msgnum, msgcount;
		    STRLIST_S *sl;

		    /*
		     * Note: this isn't required before referring to
		     *       index data, but it helps alot to eliminate
		     *	     fetch rtt's
		     * 
		     * CMD: indexrange msgnumber msgcount
		     */

		    if(Tcl_GetLongFromObj(interp, objv[2], &msgnum) != TCL_OK)
		      return(TCL_ERROR); /* conversion problem? */

		    if(Tcl_GetLongFromObj(interp, objv[3], &msgcount) != TCL_OK)
		      return(TCL_ERROR); /* conversion problem? */

		    if(msgnum > 0L
		       && msgnum <= mn_get_total(ps_global->msgmap)){
			if(msgnum + msgcount > mn_get_total(ps_global->msgmap))
			  msgcount = mn_get_total(ps_global->msgmap) - msgnum;
			peITop   = msgnum;
			peICount = msgcount;
			/* BUG: shouldn't we be loading the cache here? */
			return(TCL_OK);
		    }
		    else
		      err = "Requested Preload out of range";
		}
		else if(!strucmp(op, "sort")){
		    int	       i, reversed = 0;
		    char      *sort;

		    /*
		     * CMD: sort sortstyle reversed
		     *
		     * Returns: OK with the side-effect of message
		     *          numbers now reflecting the requested
		     *          sort order.
		     */

		    if((sort = Tcl_GetStringFromObj(objv[2], NULL))
		       && Tcl_GetIntFromObj(interp, objv[3], &reversed) != TCL_ERROR){
			/* convert sort string into */
			for(i = 0; ps_global->sort_types[i] != EndofList; i++)
			  if(strucmp(sort_name(ps_global->sort_types[i]), sort) == 0){
			      if(sp_unsorted_newmail(ps_global->mail_stream)
				 || !(ps_global->sort_types[i] == mn_get_sort(ps_global->msgmap)
				      && mn_get_revsort(ps_global->msgmap) == reversed))
				sort_folder(ps_global->mail_stream, ps_global->msgmap,
					    ps_global->sort_types[i], 
					    reversed, 0);

			      break;
			  }
		    }

		    return(peAppendCurrentSort(interp));
		}
		else if(!strucmp(op, "selected")){
		    int   rv = 0;
		    long  i, n;
		    char *range;

		    /*
		     * CMD: selected [before | after] #
		     *
		     * Returns: 1 if criteria is true, 0 otherwise
		     */

		    if((range = Tcl_GetStringFromObj(objv[2], NULL))
		       && Tcl_GetLongFromObj(interp, objv[3], &n) != TCL_ERROR){
			if(!strucmp(range, "before")){
			    for(i = 1L; i < n && i <= mn_get_total(ps_global->msgmap); i++)
			      if(get_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT)){
				  rv = 1;
				  break;
			      }

			    Tcl_SetResult(interp, int2string(rv), TCL_STATIC);
			    return(TCL_OK);
			}
			else if(!strucmp(range, "after")){
			    for(i = n + 1L; i <= mn_get_total(ps_global->msgmap); i++)
			      if(get_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT)){
				  rv = 1;
				  break;
			      }

			    Tcl_SetResult(interp, int2string(rv), TCL_STATIC);
			    return(TCL_OK);
			}
		    }

		    Tcl_SetResult(interp, "selected test failed", TCL_STATIC);
		    return(TCL_ERROR);
		}
		else if(!strcmp(op, "next")){
		    long  msgno, count;

		    /*
		     * CMD: next
		     *
		     * ARGS: msgno - message number "next" is relative to
		     *       count - how many to increment it
		     *
		     * Returns: previous state
		     */

		    if(Tcl_GetLongFromObj(interp, objv[2], &msgno) != TCL_ERROR
		       && Tcl_GetLongFromObj(interp, objv[3], &count) != TCL_ERROR){
			mn_set_cur(ps_global->msgmap, msgno);
			while(count)
			  if(count > 0){
			      mn_inc_cur(ps_global->mail_stream, ps_global->msgmap, MH_NONE);
			      count--;
			  }
			  else{
			      mn_dec_cur(ps_global->mail_stream, ps_global->msgmap, MH_NONE);
			      count++;
			  }

			Tcl_SetResult(interp, long2string(mn_get_cur(ps_global->msgmap)), TCL_VOLATILE);
			return(TCL_OK);
		    }

		    Tcl_SetResult(interp, "next can't read message number", TCL_STATIC);
		    return(TCL_ERROR);
		}
		else if(!strcmp(op, "x-nextvector")){
		    long     msgno, count;

		    /*
		     * CMD: nextvector
		     *
		     * ARGS: msgno - message number "next" is relative to
		     *       count - how many msgno slots to return
		     *
		     * Returns: vector containing next <count> messagenumbers
		     */

		    if(Tcl_GetLongFromObj(interp, objv[2], &msgno) != TCL_ERROR
		       && Tcl_GetLongFromObj(interp, objv[3], &count) != TCL_ERROR){
			if(count > 0 && !(msgno < 1L || msgno > mn_get_total(ps_global->msgmap))){
			    mn_set_cur(ps_global->msgmap, msgno);

			    while(count--){
				long n = mn_get_cur(ps_global->msgmap);

				if(peAppListLongLong(interp, Tcl_GetObjResult(interp),
						     n, mail_uid(ps_global->mail_stream,
								 mn_m2raw(ps_global->msgmap, n))) != TCL_OK)
				  return(TCL_ERROR);

				mn_inc_cur(ps_global->mail_stream, ps_global->msgmap, MH_NONE);

				if(n == mn_get_cur(ps_global->msgmap))
				  break;
			    }
			}

			return(TCL_OK);
		    }

		    Tcl_SetResult(interp, "next can't read message number", TCL_STATIC);
		    return(TCL_ERROR);
		}
		else if(!strcmp(op, "messagecount")){
		    char *relative;
		    long  msgno, n, count = 0L;

		    /*
		     * CMD: messagecount
		     *
		     * ARGS: [before | after] relative to 
		     *       msgno
		     *
		     * Returns: count of messsages before or after given message number
		     */

		    if((relative = Tcl_GetStringFromObj(objv[2], NULL))
		       && Tcl_GetLongFromObj(interp, objv[3], &msgno) != TCL_ERROR){
			if(msgno < 1L || msgno > mn_get_total(ps_global->msgmap)){
			    Tcl_SetResult(interp, "relative msgno out of range", TCL_STATIC);
			    return(TCL_ERROR);
			}

			if(!strucmp(relative, "before")){
			    for(n = msgno - 1; n > 0L; n--)
			      if(!get_lflag(ps_global->mail_stream, ps_global->msgmap, n, MN_HIDE))
				count++;

			    Tcl_SetResult(interp, long2string(count), TCL_VOLATILE);
			    return(TCL_OK);
			}
			else if(!strucmp(relative, "after")){
			    for(n = msgno + 1; n <= mn_get_total(ps_global->msgmap); n++)
			      if(!get_lflag(ps_global->mail_stream, ps_global->msgmap, n, MN_HIDE))
				count++;

			    Tcl_SetResult(interp, long2string(count), TCL_VOLATILE);
			    return(TCL_OK);
			}
		    }

		    Tcl_SetResult(interp, "can't read range for count", TCL_STATIC);
		    return(TCL_ERROR);
		}
	    }
	    else
	      err = "PEMailbox: Too many arguments";
	}
	else if(!strucmp(op, "name") || !strcmp(op, "close")){
	    Tcl_SetResult(interp, "", TCL_STATIC);
	    return(TCL_OK);
	}
	else
	  snprintf(err = errbuf, sizeof(errbuf), "%s: No open mailbox",
		  Tcl_GetStringFromObj(objv[0], NULL));
    }

    Tcl_SetResult(interp, err, TCL_VOLATILE);
    return(TCL_ERROR);
}


int
peAppendCurrentSort(Tcl_Interp *interp)
{
    return((Tcl_ListObjAppendElement(interp,
				     Tcl_GetObjResult(interp),
				     Tcl_NewStringObj(sort_name(mn_get_sort(ps_global->msgmap)), -1)) == TCL_OK
	    && Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewStringObj(mn_get_revsort(ps_global->msgmap) ? "1" : "0", 1)) == TCL_OK)
	     ? TCL_OK : TCL_ERROR);
}


int
peAppendDefaultSort(Tcl_Interp *interp)
{
    return((Tcl_ListObjAppendElement(interp,
				     Tcl_GetObjResult(interp),
				     Tcl_NewStringObj(sort_name(ps_global->def_sort), -1)) == TCL_OK
	    && Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewStringObj(ps_global->def_sort_rev ? "1" : "0", 1)) == TCL_OK)
	     ? TCL_OK : TCL_ERROR);
}


int
peSelect(Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    char *subcmd, *p;
    long  n, i, diff, msgno;
    int   narrow, hidden;
    extern     MAILSTREAM *mm_search_stream;
    extern     long	   mm_search_count;

    hidden           = any_lflagged(ps_global->msgmap, MN_HIDE) > 0L;
    mm_search_stream = ps_global->mail_stream;
    mm_search_count  = 0L;

    for(n = 1L; n <= ps_global->mail_stream->nmsgs; n++)
      mail_elt(ps_global->mail_stream, n)->searched = 0;

    /*
     * CMD: select
     *
     * ARGS: subcmd subcmdargs
     *
     * Returns: flip SLCT private bit on all or none
     *          of the messages in the mailbox
     */
    if(subcmd = Tcl_GetStringFromObj(objv[0], NULL)){
	if(!strucmp(subcmd, "all")){
	    /*
	     * Args: <none>
	     */

	    if(objc != 1)
	      return(peSelectError(interp, subcmd));

	    agg_select_all(ps_global->mail_stream, ps_global->msgmap, NULL, 1);
	    Tcl_SetResult(interp, "All", TCL_VOLATILE);
	}
	else if(!strucmp(subcmd, "none")){
	    /*
	     * Args: <none>
	     */
	    long n = 0L;

	    if(objc != 1)
	      return(peSelectError(interp, subcmd));

	    agg_select_all(ps_global->mail_stream, ps_global->msgmap, &n, 0);
	    Tcl_SetResult(interp, long2string(n), TCL_VOLATILE);
	}
	else{
	    if(!strucmp(subcmd, "narrow"))
	      narrow = 1;
	    else if(!strucmp(subcmd, "broad"))
	      narrow = 0;
	    else
	      return(peSelectError(interp, "invalid scope request"));

	    if(!(subcmd = Tcl_GetStringFromObj(objv[1], NULL)))
	      return(peSelectError(interp, "missing subcommand"));

	    if(!strucmp(subcmd, "num")){
		/*
		 * Args: [broad | narrow] firstnumber lastnumber
		 */

		long first = 0L, last = 0L, n;

		if(objc == 4){
		    if(Tcl_GetLongFromObj(interp, objv[2], &first) == TCL_OK
		       && Tcl_GetLongFromObj(interp, objv[3], &last) == TCL_OK){
			if(last && last < first){
			    n = last;
			    last = first;
			    first = n;
			}

			if(first >= 1L && first <= mn_get_total(ps_global->msgmap)){
			    if(last){
				if(last >= 1L && last <= mn_get_total(ps_global->msgmap)){
				    for(n = first; n <= last; n++)
				      mm_searched(ps_global->mail_stream,
						  mn_m2raw(ps_global->msgmap, n));
				}
				else
				  return(peSelectError(interp, "last out of range"));
			    }
			    else{
				mm_searched(ps_global->mail_stream,
					    mn_m2raw(ps_global->msgmap, first));
			    }
			}
			else
			  return(peSelectError(interp, "first out of range"));
		    }
		    else
		      return(peSelectError(interp, "can't read first/last"));
		}
		else
		  return(peSelectError(interp, "num first last"));
	    }
	    else if(!strucmp(subcmd, "date")){
		/*
		 * Args: [broad | narrow] 
		 *	 tense - "on", "since", "before"
		 *	 year - 4 digit year
		 *	 month - abbreviated month "jan", "feb"...
		 *	 day - day number
		 */

		char *tense, *year, *month, *day, buf[256];

		if(objc == 6){
		    if(tense = peSelValTense(objv[2])){
			if(year = peSelValYear(objv[3])){
			    if(month = peSelValMonth(objv[4])){
				if(day = peSelValDay(objv[5])){
				    snprintf(buf, sizeof(buf), "%s %s-%s-%s",
					    tense, day, month, year);
				    mail_search_full(ps_global->mail_stream, NULL,
						     mail_criteria(buf),
						     SE_NOPREFETCH | SE_FREE);
				}
				else
				  return(peSelectError(interp, "<with valid day>"));
			    }
			    else
			      return(peSelectError(interp, "<with valid month>"));
			}
			else
			  return(peSelectError(interp, "<with valid year>"));
		    }
		    else
		      return(peSelectError(interp, "<with valid tense>"));
		}
		else
		  return(peSelectError(interp, "date tense year monthabbrev daynum"));
	    }
	    else if(!strucmp(subcmd, "text")){
		/*
		 * Args: [broad | narrow] 
		 *	 case - in not
		 *	 field - to from cc recip partic subj any
		 *	 text - free text search string
		 */
		int  not;
		char field, *text;

		if(objc == 5){
		    if((not = peSelValCase(objv[2])) >= 0){
			if(field = peSelValField(objv[3])){
			    if((text = Tcl_GetStringFromObj(objv[4], NULL))
			       && strlen(text) < 1024){
				/* BUG: fix charset not to be NULL below */
				if(agg_text_select(ps_global->mail_stream,
						   ps_global->msgmap,
						   field, not, text, NULL, NULL))
				  /* BUG: plug in "charset" above? */
				  return(peSelectError(interp, "programmer botch"));
			    }
			    else
			      return(peSelectError(interp, "<with search string < 1024>"));
			}
			else
			  return(peSelectError(interp, "<with valid field>"));
		    }
		    else
		      return(peSelectError(interp, "<with valid case>"));
		}
		else
		  return(peSelectError(interp, "text case field text"));
	    }
	    else if(!strucmp(subcmd, "status")){
		/*
		 * Args: [broad | narrow] 
		 *	 case - on not
		 *	 status - imp new ans del
		 */

		int  not;
		char flag;

		if(objc == 4){
		    if((not = peSelValCase(objv[2])) >= 0){
			if(flag = peSelValFlag(objv[3])){
			    if(agg_flag_select(ps_global->mail_stream, not, flag, NULL))
			      return(peSelectError(interp, "programmer botch"));
			}
			else
			  return(peSelectError(interp, "<with valid flag>"));
		    }
		    else
		      return(peSelectError(interp, "<with valid case>"));
		}
		else
		  return(peSelectError(interp, "status focus case flag"));
	    }
	    else
	      return(peSelectError(interp, "cmd cmdargs"));

	    /*
	     * at this point all interesting messages should
	     * have searched bit lit
	     */

	    if(narrow)				/* make sure something was selected */
	      for(i = 1L; i <= mn_get_total(ps_global->msgmap); i++)
		if(mail_elt(ps_global->mail_stream,
			    mn_m2raw(ps_global->msgmap, i))->searched){
		    if(get_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT))
		      break;
		    else
		      mm_search_count--;
		}

	    diff = 0L;
	    if(mm_search_count){
		/*
		 * loop thru all the messages, adjusting local flag bits
		 * based on their "searched" bit...
		 */
		for(i = 1L, msgno = 0L; i <= mn_get_total(ps_global->msgmap); i++)
		  if(narrow){
		      /* turning OFF selectedness if the "searched" bit isn't lit. */
		      if(get_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT)){
			  if(!mail_elt(ps_global->mail_stream,
				       mn_m2raw(ps_global->msgmap, i))->searched){
			      diff--;
			      set_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT, 0);
			      if(hidden)
				set_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_HIDE, 1);
			  }
			  else if(msgno < mn_get_cur(ps_global->msgmap))
			    msgno = i;
		      }
		  }
		  else if(mail_elt(ps_global->mail_stream,mn_m2raw(ps_global->msgmap,i))->searched){
		      /* turn ON selectedness if "searched" bit is lit. */
		      if(!get_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT)){
			  diff++;
			  set_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_SLCT, 1);
			  if(hidden)
			    set_lflag(ps_global->mail_stream, ps_global->msgmap, i, MN_HIDE, 0);
		      }
		  }

		/* if we're zoomed and the current message was unselected */
		if(narrow && msgno
		   && get_lflag(ps_global->mail_stream,ps_global->msgmap,mn_get_cur(ps_global->msgmap),MN_HIDE))
		  mn_reset_cur(ps_global->msgmap, msgno);
	    }

	    Tcl_SetResult(interp, long2string(diff), TCL_VOLATILE);
	}

	return(TCL_OK);
    }

    Tcl_SetResult(interp, "Can't read select option", TCL_STATIC);
    return(TCL_ERROR);
}


char *
peSelValTense(Tcl_Obj *objp)
{
    char *tense, **pp;

    if(tense = Tcl_GetStringFromObj(objp, NULL)){
	static char *tenses[] = {"on", "since", "before", NULL};

	for(pp = tenses; *pp; pp++)
	  if(!strucmp(*pp, tense))
	    return(tense);
    }

    return(NULL);
}


char *
peSelValYear(Tcl_Obj *objp)
{
    char *year;

    return((year = Tcl_GetStringFromObj(objp, NULL))
	   && strlen(year) == 4
	   && isdigit((unsigned char) year[0])
	   && isdigit((unsigned char) year[0])
	   && isdigit((unsigned char) year[0])
	     ? year
	     : NULL);
}


char *
peSelValMonth(Tcl_Obj *objp)
{
    char *month, **pp;
    static char *mons[] = {"jan","feb","mar","apr",
			   "may","jun","jul","aug",
			   "sep","oct","nov","dec", NULL};

    if((month = Tcl_GetStringFromObj(objp, NULL)) && strlen(month) == 3)
      for(pp = mons; *pp; pp++)
	if(!strucmp(month, *pp))
	  return(*pp);

    return(NULL);
}


char *
peSelValDay(Tcl_Obj *objp)
{
    char *day;

    return(((day = Tcl_GetStringFromObj(objp, NULL))
	    && (day[0] == '0' || day[0] == '1'
		|| day[0] == '2' || day[0] == '3')
	    && isdigit((unsigned char) day[1])
	    && day[2] == '\0')
	       ? day
	       : NULL);
}


int
peSelValCase(Tcl_Obj *objp)
{
    char *not;

    if(not = Tcl_GetStringFromObj(objp, NULL)){
	if(!strucmp(not, "ton"))
	  return(0);
	else if(!strucmp(not, "not"))
	  return(1);
    }

    return(-1);
}


int
peSelValField(Tcl_Obj *objp)
{
    char *field;
    int   i;
    static struct {
	char *field;
	int   type;
    } fields[] = {{"from",   'f'},
		  {"to",     't'},
		  {"cc",     'c'},
		  {"subj",   's'},
		  {"any",    'a'},
		  {"recip",  'r'},
		  {"partic", 'p'},
		  {NULL,0}};

    if(field = Tcl_GetStringFromObj(objp, NULL))
      for(i = 0; fields[i].field ; i++)
	if(!strucmp(fields[i].field, field))
	  return(fields[i].type);

    return(0);
}


int
peSelValFlag(Tcl_Obj *objp)
{
    char *flag;
    int   i;
    static struct {
	char *flag;
	int   type;
    } flags[] = {{"imp",   '*'},
		 {"new",   'n'},
		 {"ans",   'a'},
		 {"del",   'd'},
		 {NULL,0}};

    if(flag = Tcl_GetStringFromObj(objp, NULL))
      for(i = 0; flags[i].flag ; i++)
	if(!strucmp(flags[i].flag, flag))
	  return(flags[i].type);

    return(0);
}


int
peSelectError(Tcl_Interp *interp, char *usage)
{
    char buf[256];

    snprintf(buf, sizeof(buf), "should be select %.128s", usage);
    Tcl_SetResult(interp, buf, TCL_VOLATILE);
    return(TCL_ERROR);
}


int
peApply(Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    char *subcmd;
    long  n;

    if(!(n = any_lflagged(ps_global->msgmap, MN_SLCT))){
	Tcl_SetResult(interp, "No messages selected", TCL_STATIC);
	return(TCL_ERROR);
    }
    else if(subcmd = Tcl_GetStringFromObj(objv[0], NULL)){
	if(objc == 1){
	    if(!strucmp(subcmd, "delete")){
		/* BUG: is CmdWhere arg always right? */
		(void) cmd_delete(ps_global, ps_global->msgmap, MCMD_AGG | MCMD_SILENT, NULL);
		Tcl_SetResult(interp, long2string(n), TCL_STATIC);
		return(TCL_OK);
	    }
	    else if(!strucmp(subcmd, "undelete")){
		(void) cmd_undelete(ps_global, ps_global->msgmap, MCMD_AGG | MCMD_SILENT);
		Tcl_SetResult(interp, long2string(n), TCL_STATIC);
		return(TCL_OK);
	    }
	}
	else if(objc == 2){
	}
	else if(objc == 3){
	    if(!strucmp(subcmd, "flag")){
		/*
		 * Args: case - on not
		 *	 flag - imp new ans del
		 */

		int  not;
		char flag, *seq, *flagstr;
		long flags, flagged, flagid;
		extern char *currentf_sequence(MAILSTREAM *, MSGNO_S *, long, long *,int);

		if((not = peSelValCase(objv[1])) >= 0){
		    if(flag = peSelValFlag(objv[2])){
			switch (flag) {
			  case '*' :
			    flagstr = "\\FLAGGED";
			    flags = not ? 0L : ST_SET;
			    flagid = not ? F_FLAG : F_UNFLAG;
			    break;
			  case 'n' :
			    flagstr = "\\SEEN";
			    flags = not ? ST_SET : 0L;
			    flagid = not ? F_UNSEEN : F_SEEN;
			    break;
			  case 'a' :
			    flagstr = "\\ANSWERED";
			    flags = not ? 0L : ST_SET;
			    flagid = not ? F_ANS : F_UNANS;
			    break;
			  case 'd':
			    flagstr = "\\DELETED";
			    flags = not ? 0L : ST_SET;
			    flagid = not ? F_DEL : F_UNDEL;
			    break;
			  default :
			    return(peApplyError(interp, "known flag"));
			    break;
			}

			if(pseudo_selected(ps_global->msgmap)){
			    if(seq = currentf_sequence(ps_global->mail_stream,
						       ps_global->msgmap,
						       flagid, &flagged, 1)){
				mail_flag(ps_global->mail_stream, seq, flagstr, flags);
				fs_give((void **) &seq);
			    }

			    Tcl_SetResult(interp, int2string(flagged), TCL_VOLATILE);
			    return(TCL_OK);
			}
			else
			  return(peApplyError(interp, "can't select"));
		    }
		    else
		      return(peApplyError(interp, "invalid flag"));
		}
		else
		  return(peApplyError(interp, "invalid case"));
	    }
	    else if(!strucmp(subcmd, "save")){
		/*
		 * Args: colid - 
		 *	 folder - imp new ans del
		 */

		int	   colid, del, i;
		char	  *folder, *err;
		CONTEXT_S *cp;

		if(Tcl_GetIntFromObj(interp, objv[1], &colid) != TCL_ERROR){

		    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
		      if(i == colid){
			  if(folder = Tcl_GetStringFromObj(objv[2], NULL)){
			      if(pseudo_selected(ps_global->msgmap)){

				  del = (!READONLY_FOLDER(ps_global->mail_stream)
					 && F_OFF(F_SAVE_WONT_DELETE, ps_global));
				  i = save(ps_global, ps_global->mail_stream,
					   cp, folder, ps_global->msgmap, del);

				  err = (i == mn_total_cur(ps_global->msgmap)) ? NULL : "problem saving";

				  restore_selected(ps_global->msgmap);
				  if(err)
				    return(peApplyError(interp, err));

				  Tcl_SetResult(interp, long2string(i), TCL_VOLATILE);
				  return(TCL_OK);
			      }
			      else
				return(peApplyError(interp, "can't select"));
			  }
			  else
			    return(peApplyError(interp, "no folder name"));
		      }

		      return(peApplyError(interp, "bad colid"));
		}
		else
		  return(peApplyError(interp, "invalid case"));
	    }
	}
    }

    return(peApplyError(interp, "unknown option"));
}


int
peApplyError(Tcl_Interp *interp, char *usage)
{
    char buf[256];

    snprintf(buf, sizeof(buf), "apply error: %.128s", usage);
    Tcl_SetResult(interp, buf, TCL_VOLATILE);
    return(TCL_ERROR);
}


/*
 * peIndexFormat - Return with interp's result object set to
 *		   represent the index line's format as a list of
 *		   index-field-name, percentage-width pairs
 */
int
peIndexFormat(Tcl_Interp *interp)
{
    INDEX_COL_S	    *cdesc = NULL;
    struct variable *vars = ps_global->vars;
    char	    *name, wbuf[4];
    int		     remains = 0;

    for(cdesc = ps_global->index_disp_format;
	cdesc->ctype != iNothing;
	cdesc++) {
	switch(cdesc->ctype){
	  case iStatus:
	  case iFStatus:
	  case iIStatus:
	    name = "Status";
	    break;

	  case iMessNo:
	    name = "Number";
	    break;

	  case iDate:     case iSDate:      case iSDateTime: case iSTime:     case iLDate:
	  case iS1Date:   case iS2Date:     case iS3Date:    case iS4Date:    case iDateIso:
	  case iDateIsoS: 
	    name = "Date";
	    break;

	  case iTime24:     case iTime12:    case iTimezone:  
	    name = "Time";
	    break;

	  case iDay2Digit:  case iDayOfWeekAbb:
	    name = "Day";
	    break;

	  case iMonAbb: case iMon2Digit:
	    name = "Month";
	    break;

	  case iYear: case iYear2Digit:
	    name = "Year";
	    break;

	  case iScore :
	    name = "Score";
	    break;

	  case iFromTo:
	  case iFrom:
	    name = "From";
	    break;

	  case iTo:
	    name = "To";
	    break;

	  case iCc:
	    name = "Cc";
	    break;

	  case iRecips:
	    name = "Recipients";
	    break;

	  case iSender:
	    name = "Sender";
	    break;

	  case iSize:
	  case iDescripSize:
	    name = "Size";
	    break;

	  case iAtt:
	    name = "Attachments";
	    break;

	  case iSubject:
	    name = "Subject";
	    break;

	  case iNews:
	  case iNewsAndTo:
	  case iToAndNews:
	    name = "News";
	    break;

	  case iNewsAndRecips:
	    name = "News/Recip";
	    break;

	  case iRecipsAndNews:
	    name = "Recip/News";
	    break;

	  default :
	    name = "";
	    break;
	}

	if(cdesc->width > 0){
	    int p = ((cdesc->width * 100) / FAKE_SCREEN_WIDTH);

	    snprintf(wbuf, sizeof(wbuf), "%d%%", p);
	}
	else
	  wbuf[0] = '\0';

	if(peAppListStrStr(interp, Tcl_GetObjResult(interp), name, wbuf) != TCL_OK)
	  return(TCL_ERROR);
    }

    return(TCL_OK);
}


int
peNewMailResult(Tcl_Interp *interp)
{
    unsigned long n, uid;

    if(sp_mail_box_changed(ps_global->mail_stream)){
	if(n = new_mail_count()){
	    /* first element is count of new messages */
	    if(Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewLongObj(n)) != TCL_OK)
	      return(TCL_ERROR);

	    /*
	     * second element is UID of most recent message
	     */
	    for(uid = ps_global->mail_stream->nmsgs; uid > 1L; uid--)
	      if(!get_lflag(ps_global->mail_stream, NULL, uid, MN_EXLD))
		break;

	    if(!uid){
		Tcl_ResetResult(interp);
		Tcl_SetResult(interp, "0 0 0", TCL_STATIC);
		return(TCL_ERROR);
	    }

	    uid = mail_uid(ps_global->mail_stream, uid);

	    if(Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewLongObj(uid)) != TCL_OK)
	      return(TCL_ERROR);
	}
	else {
	    if(Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewIntObj(0)) != TCL_OK)
	      return(TCL_ERROR);

	    /* zero is UID of new message */
	    if(Tcl_ListObjAppendElement(interp,
					Tcl_GetObjResult(interp),
					Tcl_NewIntObj(0)) != TCL_OK)
	      return(TCL_ERROR);
	}

	if(Tcl_ListObjAppendElement(interp,
				    Tcl_GetObjResult(interp),
				    Tcl_NewLongObj(sp_expunge_count(ps_global->mail_stream)
						   ? sp_expunge_count(ps_global->mail_stream)
						   : 0L)) != TCL_OK)
	  return(TCL_ERROR);

    }
    else
      Tcl_SetResult(interp, "0 0 0", TCL_STATIC);

    return(TCL_OK);
}


/* * * * * * * *  Start of Per-Thread/SubThread access functions * * * * * * * */


/*
 * PEThreadCmd - access/manipulate various pieces of thread state
 */
int
PEThreadCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err, errbuf[256], *cmd, *op;
    int   i, j;
    long  uid;

    dprint((2, "PEThreadCmd"));

    snprintf(err = errbuf, sizeof(errbuf), "Unknown %s request",
	    Tcl_GetStringFromObj(objv[0], NULL));

    if(!(ps_global && ps_global->mail_stream)){
	snprintf(err = errbuf, sizeof(errbuf), "%s: No open mailbox",
		Tcl_GetStringFromObj(objv[0], NULL));
    }
    else if(objc < 2){
	Tcl_WrongNumArgs(interp, 1, objv, "uid cmd ?args?");
    }
    else if(Tcl_GetLongFromObj(interp, objv[1], &uid) != TCL_OK){
	return(TCL_ERROR); /* conversion problem? */
    }
    else if(!peSequenceNumber(uid)){
	snprintf(err = errbuf, sizeof(errbuf), "%s: UID %ld doesn't exist",
		Tcl_GetStringFromObj(objv[0], NULL), uid);
    }
    else if(cmd = Tcl_GetStringFromObj(objv[2], NULL)){
	if(objc == 3){
	    if(!strucmp(cmd,"info")){
#define	WP_MAX_THRD_PREFIX 256
		long raw;
		PINETHRD_S *pthrd;
		char tstr[WP_MAX_THRD_PREFIX];

		if(raw = peSequenceNumber(uid)){
		    /*
		     * translate PINETHRD_S data into 
		     */
		    if(pthrd = msgno_thread_info(ps_global->mail_stream, raw, NULL, THD_TOP)){

			tstr[0] = '\0';
/* BUG: build tstr form pthrd */


			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						 Tcl_NewStringObj(tstr, -1));
		    }
		}
		else
		  Tcl_SetResult(interp, "0", TCL_STATIC);

		return(TCL_OK);
	    }

	}
	else if(objc == 4){
	    if(!strucmp(cmd,"flag")){
		if(op = Tcl_GetStringFromObj(objv[3], NULL)){
		    if(!strucmp(op,"deleted")){
		    }
		}
	    }
	}
	else if(objc == 5){
	    if(!strucmp(cmd,"flag")){
		if(op = Tcl_GetStringFromObj(objv[3], NULL)){
		    if(!strucmp(op,"deleted")){
			int value;

			if(Tcl_GetIntFromObj(interp, objv[4], &value) != TCL_ERROR){
			    long n;
			    int  spaces = 0;
			    PINETHRD_S *pthrd;
			    char *td, *flag;

			    while(1){
				if(!(n = peSequenceNumber(uid))){
				    Tcl_SetResult(interp, "Unrecognized UID", TCL_STATIC);
				    return(TCL_ERROR);
				}

				flag = cpystr("\\DELETED");
				mail_flag(ps_global->mail_stream, long2string(n), flag, (value ? ST_SET : 0L));
				fs_give((void **) &flag);

				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							 Tcl_NewStringObj(long2string(uid), -1));

				if(++n <= ps_global->mail_stream->nmsgs){
				    uid = mail_uid(ps_global->mail_stream, n);
				}
				else
				  break;

				if(pthrd = msgno_thread_info(ps_global->mail_stream, n, NULL,THD_TOP)){
				}
				else
				  break;
			    }
			}
		    }
		}
	    }
	}
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}



/* * * * * * * *  Start of Per-Message access functions * * * * * * * */



static struct _message_cmds {
    char	*cmd;
    int		 hcount;
    struct {
	int	 argcount;
	int    (*f)(Tcl_Interp *, long, int, Tcl_Obj **);
    } h[2];
} message_cmds[] = {
    {"size",		1,  {3, peMessageSize}},
    {"date",		2,  {{3, peMessageDate}, {4, peMessageDate}}},
    {"subject",		1,  {3, peMessageSubject}},
    {"fromaddr",	1,  {3, peMessageFromAddr}},
    {"toaddr",		1,  {3, peMessageToAddr}},
    {"ccaddr",		1,  {3, peMessageCcAddr}},
    {"status",		1,  {3, peMessageStatus}},
    {"statusbits",	1,  {3, peMessageStatusBits}},
    {"charset",		1,  {3, peMessageCharset}},
    {"number",		1,  {3, peMsgnoFromUID}},
    {"envelope",	0},
    {"rawenvelope",	0},
    {"text",		1,  {3, peMessageText}},
    {"cid",		1,  {4, peMessagePartFromCID}},
    {"flag",		2,  {{4, peGetFlag}, {5, peSetFlag}}},
    {"body",		0},
    {"replyheaders",	2,  {{3, peReplyHeaders},{4, peReplyHeaders}}},
    {"replytext",	2,  {{4, peReplyText}, {5, peReplyText}}},
    {"forwardheaders",	2,  {{3, peForwardHeaders},{4, peForwardHeaders}}},
    {"forwardtext",	2,  {{3, peForwardText}, {4, peForwardText}}},
    {"rawbody",		0},
    {"select",		2,  {{3, peMsgSelect}, {4, peMsgSelect}}},
    {"detach",		1,  {5, peDetach}},
    {"attachinfo",	1,  {4, peAttachInfo}},
    {"savedefault",	1,  {3, peSaveDefault}},
    {"save",		1,  {5, peSave}},
    {"takeaddr",       	1,  {3, peTakeaddr}},
    {"replyquote",	1,  {3, peReplyQuote}},
    {"bounce",		2,  {{4, peMessageBounce},{5, peMessageBounce}}},
    {"spam",		2,  {{4, peMessageSpam}, {5, peMessageSpam}}},
    {NULL, 0}
};




/*
 * PEMessageCmd - export various bits of message information
 *
 *   NOTE: all exported commands are of the form:
 *
 *             PEMessage <uid> <cmd> <args>
 */
int
PEMessageCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err, errbuf[256], *cmd;
    int   i, j;
    long  uid;

    dprint((5, "PEMessageCmd"));

    snprintf(err = errbuf, sizeof(errbuf), "Unknown %s request",
	    Tcl_GetStringFromObj(objv[0], NULL));

    if(!(ps_global && ps_global->mail_stream)){
	snprintf(err = errbuf, sizeof(errbuf), "%s: No open mailbox",
		Tcl_GetStringFromObj(objv[0], NULL));
    }
    else if(objc < 3){
	Tcl_WrongNumArgs(interp, 0, objv, "PEMessage <uid> cmd ?args?");
    }
    else if(Tcl_GetLongFromObj(interp, objv[1], &uid) != TCL_OK){
	return(TCL_ERROR); /* conversion problem? */
    }
    else if(!peMessageNumber(uid)){
	snprintf(err = errbuf, sizeof(errbuf), "%s: UID %ld doesn't exist",
		Tcl_GetStringFromObj(objv[0], NULL), uid);
    }
    else if(cmd = Tcl_GetStringFromObj(objv[2], NULL)){
	for(i = 0; message_cmds[i].cmd; i++)
	  if(!strcmp(cmd, message_cmds[i].cmd)){
	      for(j = 0; j < message_cmds[i].hcount; j++)
		if(message_cmds[i].h[j].argcount == objc)
		  return((*message_cmds[i].h[j].f)(interp, uid, objc - 3,
						   &((Tcl_Obj **)objv)[3]));

	      snprintf(err = errbuf, sizeof(errbuf),
		       "PEMessage: %s: mismatched argument count", cmd);
	      break;
	  }
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


long
peMessageNumber(long uid)
{
    return(mn_raw2m(ps_global->msgmap, peSequenceNumber(uid)));
}


long
peSequenceNumber(long uid)
{
    return(mail_msgno(ps_global->mail_stream, uid));
}


int
peMessageSize(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    long raw;

    if((raw = peSequenceNumber(uid))
       && pine_mail_fetchstructure(ps_global->mail_stream, raw, NULL)){
	Tcl_SetResult(interp,
		      long2string(mail_elt(ps_global->mail_stream,
					   raw)->rfc822_size),
		      TCL_VOLATILE);
    }
    else
      Tcl_SetResult(interp, "0", TCL_STATIC);

    return(TCL_OK);
}


int
peMessageDate(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    char	 *cmd;
    long	 raw;
    ENVELOPE	*env;
    MESSAGECACHE mc;

    if((raw = peSequenceNumber(uid))
       && (env = pine_mail_fetchstructure(ps_global->mail_stream, raw, NULL))){
	if(objc == 1 && objv[0]){
	    if(mail_parse_date(&mc, env->date)){
		if(cmd = Tcl_GetStringFromObj(objv[0], NULL)){
		    if(!strucmp(cmd,"day")){
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%02d", mc.day);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
			return(TCL_OK);
		    }
		    else if(!strucmp(cmd,"month")){
			Tcl_SetResult(interp, month_abbrev(mc.month), TCL_VOLATILE);
			return(TCL_OK);
		    }
		    else if(!strucmp(cmd,"year")){
			Tcl_SetResult(interp, int2string(mc.year + BASEYEAR), TCL_VOLATILE);
			return(TCL_OK);
		    }
		    else{
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "peMessageDate cmd: %.20s", cmd);
			Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    }
		}
		else
		  Tcl_SetResult(interp, "peMessageDate: can't get command", TCL_STATIC);
	    }
	    else
	      Tcl_SetResult(interp, "peMessageDate: can't parse date", TCL_STATIC);
	}
	else{
	    Tcl_SetResult(interp, env->date ? (char *) env->date : "", TCL_VOLATILE);
	    return(TCL_OK);
	}
    }
    else
      Tcl_SetResult(interp, "Can't get message structure", TCL_STATIC);

    return(TCL_ERROR);
}


int
peMessageFromAddr(Tcl_Interp *interp,
		  long uid,
		  int objc,
		  Tcl_Obj **objv)
{
    return(peMessageField(interp, uid, "from"));
}


int
peMessageToAddr(Tcl_Interp *interp,
		long uid,
		int objc,
		Tcl_Obj **objv)
{
    return(peMessageField(interp, uid, "to"));
}


int
peMessageCcAddr(Tcl_Interp *interp,
		long uid,
		int objc,
		Tcl_Obj **objv)
{
    return(peMessageField(interp, uid, "cc"));
}


int
peMessageSubject(Tcl_Interp *interp,
		 long uid,
		 int objc,
		 Tcl_Obj **objv)
{
    return(peMessageField(interp, uid, "subject"));
}


int
peMessageField(interp, uid, field)
    Tcl_Interp	*interp;
    long	 uid;
    char	*field;
{
    long      raw;
    char     *s = "";
    ENVELOPE *env;
    ADDRESS  *addr;

    if((raw = peSequenceNumber(uid))
       && (env = pine_mail_fetchstructure(ps_global->mail_stream, raw, NULL))){
	if(!strucmp(field, "from")){
	    if(env->from && env->from->mailbox)
	      snprintf(s = tmp_20k_buf, SIZEOF_20KBUF, "%.256s%s%.256s", env->from->mailbox,
		      (env->from->host) ? "@" : "", (env->from->host) ? env->from->host : "");
	}
	else if(!strucmp(field, "to")){
	    if(env->to && env->to->mailbox)
	      snprintf(s = tmp_20k_buf, SIZEOF_20KBUF, "%.256s%s%.256s", env->to->mailbox,
		      (env->to->host) ? "@" : "", (env->to->host) ? env->to->host : "");
	}
	else if(!strucmp(field, "cc")){
	    if(env->cc && env->cc->mailbox)
	      snprintf(s = tmp_20k_buf, SIZEOF_20KBUF, "%.256s%s%.256s", env->cc->mailbox,
		      (env->cc->host) ? "@" : "", (env->cc->host) ? env->cc->host : "");
	}
	else if(!strucmp(field, "subject")){
	    if(env->subject)
	      snprintf(s = tmp_20k_buf, SIZEOF_20KBUF, "%.256s", env->subject);
	}
	else{
	    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Unknown message field: %.20s", field);
	    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	    return(TCL_ERROR);
	}

	Tcl_SetResult(interp, s, TCL_VOLATILE);
	return(TCL_OK);
    }

    Tcl_SetResult(interp, "Can't read message envelope", TCL_STATIC);
    return(TCL_ERROR);
}


int
peMessageStatus(Tcl_Interp *interp,
		long uid,
		int objc,
		Tcl_Obj **objv)
{
    long	  raw;
    MESSAGECACHE *mc;

    if(raw = peSequenceNumber(uid)){
	if(!((mc = mail_elt(ps_global->mail_stream, raw))
	     && mc->valid)){
	    mail_fetch_flags(ps_global->mail_stream,
			     long2string(uid), FT_UID);
	    mc = mail_elt(ps_global->mail_stream, raw);
	}

	if (mc->deleted)
	  Tcl_ListObjAppendElement(interp,
				   Tcl_GetObjResult(interp),
				   Tcl_NewStringObj("Deleted", -1));

	if (mc->answered)
	  Tcl_ListObjAppendElement(interp,
				   Tcl_GetObjResult(interp),
				   Tcl_NewStringObj("Answered", -1));

	if (!mc->seen)
	  Tcl_ListObjAppendElement(interp,
				   Tcl_GetObjResult(interp),
				   Tcl_NewStringObj("New", -1));

	if (mc->flagged)
	  Tcl_ListObjAppendElement(interp,
				   Tcl_GetObjResult(interp),
				   Tcl_NewStringObj("Important", -1));
    }

    return(TCL_OK);
}


int
peMessageCharset(Tcl_Interp *interp,
		 long uid,
		 int objc,
		 Tcl_Obj **objv)
{
    /* everthing coming out of pith better be utf-8 */
    Tcl_SetResult(interp, "UTF-8", TCL_STATIC);
    return(TCL_OK);
}


int
peMsgnoFromUID(Tcl_Interp *interp,
	       long uid,
	       int objc,
	       Tcl_Obj **objv)
{
    Tcl_SetResult(interp, long2string(peMessageNumber(uid)), TCL_VOLATILE);
    return(TCL_OK);
}


/*
 * locally global structure to keep track of various bits of state
 * needed to collect filtered output
 */
static struct _embedded_data {
    Tcl_Interp *interp;
    STORE_S    *store;
    long	uid;
    HANDLE_S   *handles;
    char	inhandle;
    struct {
	char	fg[7];
	char	bg[7];
	char	fgdef[7];
	char	bgdef[7];
    } color;
} peED;



/*
 * peInterpWritec - collect filtered output, appending to the
 *		    command's result list on each EOL
 */
int
peInterpWritec(int c)
{
    unsigned char ch = (unsigned char) (0xff & c);

    if(ch == '\n')
      return(peInterpFlush() == TCL_OK);
    else
      so_writec(ch, peED.store);

    return(1);
}


/*
 * peInterpFlush - write accumulated line to result object mapping
 *		   embedded data into exportable tcl list members
 *
 */
int
peInterpFlush(void)
{
    char    *line, *p, *tp, *tp2, col1[32], col2[32];
    Tcl_Obj *lobjp, *objColor, *objPair;

    line = (char *) so_text(peED.store);

    if(lobjp = Tcl_NewListObj(0, NULL)){
	if(p = strindex(line, TAG_EMBED)){
	    do{
		*p = '\0';

		if(p - line)
		  peAppListStrStr(peED.interp, lobjp, "t", line);

		switch(*++p){
		  case TAG_HANDLE :
		    {
			int	  i, n;
			HANDLE_S *h;


			for(n = 0, i = *++p; i > 0; i--)
			  n = (n * 10) + (*++p - '0');

			line = ++p;	/* prepare for next section of line */

			if(!peED.inhandle){
			    peED.inhandle = 1;

			    if(h = get_handle(peED.handles, n))
			      switch(h->type){
				case IMG :
				{
				    Tcl_Obj *llObj, *rObj;

				    llObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, llObj, Tcl_NewStringObj("img", -1));

				    rObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewStringObj(h->h.img.src ? h->h.img.src : "", -1));
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewStringObj(h->h.img.alt ? h->h.img.alt : "", -1));

				    Tcl_ListObjAppendElement(peED.interp, llObj, rObj);

				    Tcl_ListObjAppendElement(peED.interp, lobjp, llObj);
				    peED.inhandle = 0;
				}

				break;

				case URL :
				{
				    Tcl_Obj *llObj, *rObj;

				    llObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, llObj, Tcl_NewStringObj("urlstart", -1));

				    rObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewStringObj(h->h.url.path ? h->h.url.path : "", -1));
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewStringObj(h->h.url.name ? h->h.url.name : "", -1));

				    Tcl_ListObjAppendElement(peED.interp, llObj, rObj);

				    Tcl_ListObjAppendElement(peED.interp, lobjp, llObj);
				}

				break;

				case Attach :
				{
				    Tcl_Obj *alObj, *rObj, *tObj, *stObj, *fnObj, *eObj;

				    alObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, alObj, Tcl_NewStringObj("attach", -1));

				    peGetMimeTyping(mail_body(ps_global->mail_stream,
							      peSequenceNumber(peED.uid),
							      h->h.attach->number),
						    &tObj, &stObj, &fnObj, &eObj);


				    rObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewLongObj(peED.uid));
				    Tcl_ListObjAppendElement(peED.interp, rObj, Tcl_NewStringObj(h->h.attach->number, -1));
				    Tcl_ListObjAppendElement(peED.interp, rObj, tObj);
				    Tcl_ListObjAppendElement(peED.interp, rObj, stObj);
				    Tcl_ListObjAppendElement(peED.interp, rObj, fnObj);
				    Tcl_ListObjAppendElement(peED.interp, rObj, eObj);

				    Tcl_ListObjAppendElement(peED.interp, alObj, rObj);

				    Tcl_ListObjAppendElement(peED.interp, lobjp, alObj);
				}

				break;

				default :
				  break;
			      }
			}
		    }

		    break;

		  case TAG_FGCOLOR :
		    if((tp = peColorStr(++p, col1)) && (strcmp(tp, peED.color.fg) || strcmp(tp, peED.color.fgdef))){
			/* look ahead */
			if(p[11] == TAG_EMBED
			   && p[12] == TAG_BGCOLOR
			   && (tp2 = peColorStr(p + 13, col2))){
			    objColor = Tcl_NewListObj(0, NULL);
			    objPair  = Tcl_NewListObj(0, NULL);
			    Tcl_ListObjAppendElement(peED.interp, objColor, Tcl_NewStringObj("color", -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp, -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp2, -1));
			    Tcl_ListObjAppendElement(peED.interp, objColor, objPair);
			    Tcl_ListObjAppendElement(peED.interp, lobjp, objColor);
			    strcpy(peED.color.bg, tp2);
			    p += 13;
			}
			else if(strcmp(peED.color.bg, peED.color.bgdef)){
			    objColor = Tcl_NewListObj(0, NULL);
			    objPair  = Tcl_NewListObj(0, NULL);
			    Tcl_ListObjAppendElement(peED.interp, objColor, Tcl_NewStringObj("color", -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp, -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(peED.color.bgdef, -1));
			    Tcl_ListObjAppendElement(peED.interp, objColor, objPair);
			    Tcl_ListObjAppendElement(peED.interp, lobjp, objColor);
			    strcpy(peED.color.bg, peED.color.bgdef);
			}
			else
			  peAppListStrStr(peED.interp, lobjp, "fgcolor", tp);

			strcpy(peED.color.fg, tp);
		    }

		    line = p + 11;
		    break;

		  case TAG_BGCOLOR :
		    if((tp = peColorStr(++p, col1)) && (strcmp(tp, peED.color.bg) || strcmp(tp, peED.color.bgdef))){
			/* look ahead */
			if(p[11] == TAG_EMBED
			   && p[12] == TAG_FGCOLOR
			   && (tp2 = peColorStr(p + 13, col2))){
			    objColor = Tcl_NewListObj(0, NULL);
			    objPair  = Tcl_NewListObj(0, NULL);
			    Tcl_ListObjAppendElement(peED.interp, objColor, Tcl_NewStringObj("color", -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp2, -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp, -1));
			    Tcl_ListObjAppendElement(peED.interp, objColor, objPair);
			    Tcl_ListObjAppendElement(peED.interp, lobjp, objColor);
			    strcpy(peED.color.fg, tp2);
			    p += 13;
			}
			else if(strcmp(peED.color.fg, peED.color.fgdef)){
			    objColor = Tcl_NewListObj(0, NULL);
			    objPair  = Tcl_NewListObj(0, NULL);
			    Tcl_ListObjAppendElement(peED.interp, objColor, Tcl_NewStringObj("color", -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(peED.color.fgdef, -1));
			    Tcl_ListObjAppendElement(peED.interp, objPair, Tcl_NewStringObj(tp, -1));
			    Tcl_ListObjAppendElement(peED.interp, objColor, objPair);
			    Tcl_ListObjAppendElement(peED.interp, lobjp, objColor);
			    strcpy(peED.color.fg, peED.color.fgdef);
			}
			else
			  peAppListStrStr(peED.interp, lobjp, "bgcolor", tp);

			strcpy(peED.color.bg, tp);
		    }

		    line = p + 11;
		    break;

		  case TAG_ITALICON :
		    peAppListStrStr(peED.interp, lobjp, "italic", "on");
		    line = p + 1;
		    break;

		  case TAG_ITALICOFF :
		    peAppListStrStr(peED.interp, lobjp, "italic", "off");
		    line = p + 1;
		    break;

		  case TAG_BOLDON :
		    peAppListStrStr(peED.interp, lobjp, "bold", "on");
		    line = p + 1;
		    break;

		  case TAG_BOLDOFF :
		    peAppListStrStr(peED.interp, lobjp, "bold", "off");
		    line = p + 1;
		    break;

		  case TAG_ULINEON :
		    peAppListStrStr(peED.interp, lobjp, "underline", "on");
		    line = p + 1;
		    break;

		  case TAG_ULINEOFF :
		    peAppListStrStr(peED.interp, lobjp, "underline", "off");
		    line = p + 1;
		    break;

		  case TAG_STRIKEON :
		    peAppListStrStr(peED.interp, lobjp, "strikethru", "on");
		    line = p + 1;
		    break;

		  case TAG_STRIKEOFF :
		    peAppListStrStr(peED.interp, lobjp, "strikethru", "off");
		    line = p + 1;
		    break;

		  case TAG_BIGON :
		    peAppListStrStr(peED.interp, lobjp, "bigfont", "on");
		    line = p + 1;
		    break;

		  case TAG_BIGOFF :
		    peAppListStrStr(peED.interp, lobjp, "bigfont", "off");
		    line = p + 1;
		    break;

		  case TAG_SMALLON :
		    peAppListStrStr(peED.interp, lobjp, "smallfont", "on");
		    line = p + 1;
		    break;

		  case TAG_SMALLOFF :
		    peAppListStrStr(peED.interp, lobjp, "smallfont", "off");
		    line = p + 1;
		    break;

		  case TAG_INVOFF :
		  case TAG_HANDLEOFF :
		    if(peED.inhandle){
			peAppListStrStr(peED.interp, lobjp, "urlend", "");
			peED.inhandle = 0;
		    }
		    /* fall thru and advance "line" */

		  default :
		    line = p + 1;
		    break;
		}

	    }
	    while(p = strindex(line, TAG_EMBED));

	    if(*line)
	      peAppListStrStr(peED.interp, lobjp, "t", line);
	}
	else
	  peAppListStrStr(peED.interp, lobjp, "t", line);
    }
    else
      peAppListStrStr(peED.interp, lobjp, "t", "");

    if(Tcl_ListObjAppendElement(peED.interp,
				Tcl_GetObjResult(peED.interp),
				lobjp) == TCL_OK){
	so_truncate(peED.store, 0L);
	return(TCL_OK);
    }

    return(TCL_ERROR);
}



/*
 * peInterpWritec - collect filtered output, appending to the
 *		    command's result list on each EOL
 */
int
peNullWritec(int c)
{
    return(1);
}


char *
peColorStr(char *s, char *b)
{
    int i, j, color;

    i = 0;
    b[0] = '\0';
    while(1){
	color = 0;
	for(j = 0; j < 3; j++, s++)
	  if(isdigit((unsigned char) *s))
	    color = (color * 10) + (*s - '0');

	s++;				/* advance past ',' */
	if(color < 256)
	  sprintf(b + strlen(b), "%2.2x", color);
	else
	  break;

	if(++i == 3)
	  return(b);
    }
	

    return(NULL);
}



int
peMessageText(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    MESSAGECACHE *mc;
    ENVELOPE	 *env;
    BODY	 *body;
    int		  flags;
    long	  raw;
    char	 *color;

    memset(&peED, 0, sizeof(peED));
    peED.interp = interp;
    peED.store = so_get(CharStar, NULL, EDIT_ACCESS);

    peED.color.fg[0] = '\0';
    if((color = pico_get_last_fg_color()) && (color = color_to_asciirgb(color))){
	peInterpWritec(TAG_EMBED);
	peInterpWritec(TAG_FGCOLOR);
	gf_puts(color, peInterpWritec);
	strcpy(peED.color.fgdef, peColorStr(color, tmp_20k_buf));
    }

    peED.color.bg[0] = '\0';
    if((color = pico_get_last_bg_color()) && (color = color_to_asciirgb(color))){
	peInterpWritec(TAG_EMBED);
	peInterpWritec(TAG_BGCOLOR);
	gf_puts(color, peInterpWritec);
	strcpy(peED.color.bgdef, peColorStr(color,tmp_20k_buf));
    }

    raw	     = peSequenceNumber(peED.uid = uid);
    body     = NULL;
    ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
    if(!((env = pine_mail_fetchstructure(ps_global->mail_stream, raw, &body))
	 && (mc = mail_elt(ps_global->mail_stream, raw)))){
	char buf[256];

	snprintf(buf, sizeof(buf), "Error getting message %ld: %s", peMessageNumber(uid),
		ps_global->last_error[0] ? ps_global->last_error : "Indeterminate");

	dprint((1, "ERROR fetching %s of msg %ld: %s",
		   env ? "elt" : "env", mn_get_cur(ps_global->msgmap),
		   ps_global->last_error[0] ? ps_global->last_error : "Indeterminate"));

	Tcl_SetResult(interp, buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }

    flags = FM_DISPLAY | FM_NEW_MESS | FM_NOEDITORIAL | FM_NOHTMLREL | FM_HTMLRELATED;

    init_handles(&peED.handles);

    (void) format_message(raw, env, body, &peED.handles, flags, peInterpWritec);

    peInterpFlush();

    so_give(&peED.store);
    free_handles(&peED.handles);
    return(TCL_OK);
}


/*
 * peMessagePartFromCID - return part number assoc'd with given uid and CID
 */
int
peMessagePartFromCID(Tcl_Interp *interp,
		     long uid,
		     int objc,
		     Tcl_Obj **objv)
{
    char     *cid, sect_buf[256];
    long      raw;
    ENVELOPE *env;
    BODY     *body;

    raw	= peSequenceNumber(peED.uid = uid);
    ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';

    if(objv[0] && (cid = Tcl_GetStringFromObj(objv[0], NULL)) && *cid != '\0'){
	if(env = pine_mail_fetchstructure(ps_global->mail_stream, raw, &body)){
	    sect_buf[0] = '\0';
	    if(peLocateBodyByCID(cid, sect_buf, body)){
		Tcl_SetResult(interp, sect_buf, TCL_VOLATILE);
	    }
	}
	else{
	    Tcl_SetResult(interp, ps_global->last_error[0] ? ps_global->last_error : "Error getting CID", TCL_VOLATILE);
	    return(TCL_ERROR);
	}
    }

    return(TCL_OK);
}


int
peLocateBodyByCID(char *cid, char *section, BODY *body)
{
    if(body->type == TYPEMULTIPART){
	char  subsection[256], *subp;
	int   n;
	PART *part     = body->nested.part;

	if(!(part = body->nested.part))
	  return(0);

	subp = subsection;
	if(section && *section){
	    for(n = 0;
		n < sizeof(subsection)-20 && (*subp = section[n]); n++, subp++)
	      ;

	    *subp++ = '.';
	}

	n = 1;
	do {
	    sprintf(subp, "%d", n++);
	    if(peLocateBodyByCID(cid, subsection, &part->body)){
		strcpy(section, subsection);
		return(1);
	    }
	}
	while(part = part->next);

	return(0);
    }

    return((body && body->id) ? !strcmp(cid, body->id) : 0);
}


/*
 * peGetFlag - Return 1 or 0 based on requested flags current state
 *
 * Params: argv[0] == flagname
 */
int
peGetFlag(interp, uid, objc, objv)
    Tcl_Interp	*interp;
    long	 uid;
    int		 objc;
    Tcl_Obj    **objv;
{
    char	 *flagname;
    long	  raw;
    int		  value = 0;
    MESSAGECACHE *mc;

    if(flagname = Tcl_GetStringFromObj(objv[0], NULL)){
	raw = peSequenceNumber(uid);

	if(!((mc = mail_elt(ps_global->mail_stream, raw))
	     && mc->valid)){
	    mail_fetch_flags(ps_global->mail_stream,
			     long2string(uid), FT_UID);
	    mc = mail_elt(ps_global->mail_stream, raw);
	}

	if(!strucmp(flagname, "deleted")){
	    value = mc->deleted;
	}
	else if(!strucmp(flagname, "new")){
	    value = !mc->seen;
	}
	else if(!strucmp(flagname, "important")){
	    value = mc->flagged;
	}
	else if(!strucmp(flagname, "answered")){
	    value = mc->answered;
	}
	else if(!strucmp(flagname, "recent")){
	    value = mc->recent;
	}
    }

    Tcl_SetResult(interp, int2string(value), TCL_VOLATILE);
    return(TCL_OK);
}



/*
 * peSetFlag - Set requested flags value to 1 or 0
 *
 * Params: abjv[0] == flagname
 *         objv[1] == newvalue
 */
int
peSetFlag(interp, uid, objc, objv)
    Tcl_Interp  *interp;
    long	 uid;
    int		 objc;
    Tcl_Obj    **objv;
{
    char	 *flagname, *flagstr = NULL;
    int		  value;

    if((flagname = Tcl_GetStringFromObj(objv[0], NULL))
       && Tcl_GetIntFromObj(interp, objv[1], &value) != TCL_ERROR){
	if(!strucmp(flagname, "deleted")){
	    flagstr = "\\DELETED";
	}
	else if(!strucmp(flagname, "new")){
	    flagstr = "\\SEEN";
	    value = !value;
	}
	else if(!strucmp(flagname, "important")){
	    flagstr = "\\FLAGGED";
	}
	else if(!strucmp(flagname, "answered")){
	    flagstr = "\\ANSWERED";
	}
	else if(!strucmp(flagname, "recent")){
	    flagstr = "\\RECENT";
	}

	if(flagstr){
	    ps_global->c_client_error[0] = '\0';
	    mail_flag(ps_global->mail_stream,
		      long2string(uid),
		      flagstr, (value ? ST_SET : 0L) | ST_UID);
	    if(ps_global->c_client_error[0] != '\0'){
		snprintf(tmp_20k_buf, SIZEOF_20KBUF, "peFlag: %.40s",
			 ps_global->c_client_error);
		Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	}
    }

    Tcl_SetResult(interp, value ? "1" : "0", TCL_STATIC);
    return(TCL_OK);
}


/*
 * peMsgSelect - Return 1 or 0 based on whether given UID is selected
 *
 * Params: argv[0] == selected
 */
int
peMsgSelect(Tcl_Interp *interp,
	    long uid,
	    int objc,
	    Tcl_Obj **objv)
{
    int value;

    if(objc == 1 && objv[0]){
	if(Tcl_GetIntFromObj(interp, objv[0], &value) != TCL_ERROR){
	    if(value){
		set_lflag(ps_global->mail_stream, ps_global->msgmap,
			  peMessageNumber(uid), MN_SLCT, 1);
		set_lflag(ps_global->mail_stream, ps_global->msgmap,
			  peMessageNumber(uid), MN_HIDE, 0);
	    } else {
		set_lflag(ps_global->mail_stream, ps_global->msgmap,
			  peMessageNumber(uid), MN_SLCT, 0);
		/* if zoomed, lite hidden bit */
		if(any_lflagged(ps_global->msgmap, MN_HIDE))
		  set_lflag(ps_global->mail_stream, ps_global->msgmap,
			    peMessageNumber(uid), MN_HIDE, 1);
	    
	    }
	}
	else{
	    Tcl_SetResult(interp, "peMsgSelect: can't get value", TCL_STATIC);
	    return(TCL_ERROR);
	}
    }

    Tcl_SetResult(interp,
		  (get_lflag(ps_global->mail_stream, NULL,
			     peSequenceNumber(uid),
			     MN_SLCT))
		    ? "1" : "0",
		  TCL_VOLATILE);
    return(TCL_OK);
}


/*
 * peAppendIndexParts - append list of digested index pieces to given object
 *
 * Params: 
 *
 */
int
peAppendIndexParts(Tcl_Interp *interp, long uid, Tcl_Obj *aObj)
{
    int		 rv = TCL_OK;
    Tcl_Obj	*objField, *objElement, *objp;
    int		 i;
    ICE_S	*h;
    IFIELD_S    *f;
    IELEM_S	*ie;


    if(h = build_header_work(ps_global, ps_global->mail_stream,
			     ps_global->msgmap, peMessageNumber(uid),
			     peITop, peICount, NULL)){
	for(f = h->ifield; f; f = f->next){

	    if((objField = Tcl_NewListObj(0, NULL)) == NULL)
	      return(TCL_ERROR);

	    for(ie = f->ielem; ie ; ie = ie->next){

		if((objElement = Tcl_NewListObj(0, NULL)) == NULL)
		  return(TCL_ERROR);

		if(ie->datalen){
		    /* FIRST: DATA */
#if	INTERNAL_INDEX_TRUNCATE
		    char *ep;

		    ep = (char *) fs_get((ie->datalen + 1) * sizeof(char));
		    sprintf(ep, "%.*s", ie->wid, ie->data);

		    /* and other stuff to pack trunc'd element into a new object */
#endif

#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
		    objp = Tcl_NewByteArrayObj(ie->data, ie->datalen);
#else
		    objp = Tcl_NewStringObj(ie->data, -1);
#endif
		}
		else
		  objp = Tcl_NewStringObj("", -1);

		if(Tcl_ListObjAppendElement(interp, objElement, objp) != TCL_OK)
		  return(TCL_ERROR);

		if(ie->color){
		    Tcl_Obj *objColor;

		    if((objp = Tcl_NewListObj(0, NULL)) == NULL)
		      return(TCL_ERROR);

		    objColor = Tcl_NewStringObj(color_to_asciirgb(ie->color->fg), -1);
		    if(Tcl_ListObjAppendElement(interp, objp, objColor) != TCL_OK)
		      return(TCL_ERROR);

		    objColor = Tcl_NewStringObj(color_to_asciirgb(ie->color->bg), -1);
		    if(Tcl_ListObjAppendElement(interp, objp, objColor) != TCL_OK)
		      return(TCL_ERROR);
		}
		else
		  objp = Tcl_NewStringObj("", -1);

		if(Tcl_ListObjAppendElement(interp, objElement, objp) != TCL_OK)
		  return(TCL_ERROR);

		/*
		 * IF we ever want to map the thread characters into nice
		 * graphical symbols or take advantage of features like clicking
		 * on a thread element to collapse and such, we need to have
		 * element tagging. That's what the object creation and append
		 * are placeholders for
		 */
		switch(ie->type){
		  case eThreadInfo :
		    objp = Tcl_NewStringObj("threadinfo", -1);
		    break;
		  case eText :
		    objp = NULL;
		    break;
		  default :
		    objp = Tcl_NewStringObj(int2string(ie->type), -1);
		    break;
		}

		if(objp && Tcl_ListObjAppendElement(interp, objElement, objp) != TCL_OK)
		  return(TCL_ERROR);

		if(Tcl_ListObjAppendElement(interp, objField, objElement) != TCL_OK)
		  return(TCL_ERROR);
	    }

	    if(Tcl_ListObjAppendElement(interp, aObj, objField) != TCL_OK){
		return(TCL_ERROR);
	    }
	}
    }

    return(TCL_OK);
}


/*
 * peMessageStatusBits - return list flags indicating pine status bits
 *
 * Params: 
 *
 * Returns: list of lists where:
 *		*  the first element is the list of
 *		   field elements data
 *		*  the second element is a two element
 *		   list containing the lines foreground
 *		   and background colors
 */
int
peMessageStatusBits(Tcl_Interp *interp,
	     long uid,
	     int objc,
	     Tcl_Obj **objv)
{
    long msgno;

    Tcl_SetResult(interp,
		  peMsgStatBitString(ps_global, ps_global->mail_stream,
				     ps_global->msgmap, peMessageNumber(uid),
				     peITop, peICount),
		  TCL_STATIC);
    return(TCL_OK);
}


char *
peMsgStatBitString(struct pine *state,
		   MAILSTREAM  *stream,
		   MSGNO_S     *msgmap,
		   long		msgno,
		   long		top_msgno,
		   long		msgcount)
{
    static char	  buf[36];
    int		  i;
    MESSAGECACHE *mc;
    ICE_S	 *h;

    if((h = build_header_work(state, stream, msgmap,
			      msgno, top_msgno, msgcount, NULL))
       && (mc = mail_elt(stream, mn_m2raw(msgmap, msgno)))){
	/* return a string representing a bit field where:
	   index     meaning
	   -----     -------
	   0         "New"
	   1         deleted
	   2         answered
	   3         flagged
	   4         to us
	   5         cc us
	*/
	i = 0;
	buf[i++] = (mc->seen) ? '0' : '1';
	buf[i++] = (mc->deleted) ? '1' : '0';
	buf[i++] = (mc->answered) ? '1' : '0';
	buf[i++] = (mc->flagged) ? '1' : '0';
	buf[i++] = (h->to_us) ? '1' : '0';
	buf[i++] = (h->cc_us) ? '1' : '0';
	buf[i++] = '\0';

	return(buf);
    }

    return("100000");
}


/*
 * peReplyHeaders - return subject used in reply to given message
 *
 * Params: 
 *
 * Returns: list of header value pairs where headers are:
 *          In-Reply-To:, Subject:, Cc:
 *
 */
int
peReplyHeaders(Tcl_Interp *interp,
	       long uid,
	       int objc,
	       Tcl_Obj **objv)
{
    long	 raw;
    int		 flags = RSF_FORCE_REPLY_TO | RSF_FORCE_REPLY_ALL, err = FALSE;
    char	*errmsg = NULL, *fcc = NULL, *sect = NULL;
    ENVELOPE	*env, *outgoing;
    BODY	*body = NULL;
    Tcl_Obj	*objp;
    ADDRESS	*saved_from, *saved_to, *saved_cc, *saved_resent;

    saved_from		  = (ADDRESS *) NULL;
    saved_to		  = (ADDRESS *) NULL;
    saved_cc		  = (ADDRESS *) NULL;
    saved_resent	  = (ADDRESS *) NULL;

    raw = peSequenceNumber(uid);

    /* if we're given a valid section number that
     * corresponds to a valid msg/rfc822 body part
     * then set up headers in attached message.
     */
    if(objc == 1 && objv[0]
       && (sect = Tcl_GetStringFromObj(objv[0], NULL)) && *sect != '\0'
       && (body = mail_body(ps_global->mail_stream, raw, sect))
       && body->type == TYPEMESSAGE
       && !strucmp(body->subtype, "rfc822")){
	env = body->nested.msg->env;
    }
    else{
	sect = NULL;
	env = mail_fetchstructure(ps_global->mail_stream, raw, NULL);
    }

    if(env){
	if(!reply_harvest(ps_global, raw, sect, env,
			  &saved_from, &saved_to, &saved_cc,
			  &saved_resent, &flags)){
	    
	    Tcl_SetResult(interp, "", TCL_STATIC);
	    return(TCL_ERROR);
	}

	outgoing = mail_newenvelope();

	reply_seed(ps_global, outgoing, env,
		   saved_from, saved_to, saved_cc, saved_resent,
		   &fcc, flags, &errmsg);
	if(errmsg){
	    if(*errmsg){
		q_status_message1(SM_ORDER, 3, 3, "%.200s", errmsg);
	    }

	    fs_give((void **)&errmsg);
	}

	env = pine_mail_fetchstructure(ps_global->mail_stream, raw, NULL);

	outgoing->subject = reply_subject(env->subject, NULL, 0);
	outgoing->in_reply_to = reply_in_reply_to(env);

	err = !(peAppListStrAddr(interp, Tcl_GetObjResult(interp),
				 "to", outgoing->to) == TCL_OK
		&& peAppListStrAddr(interp, Tcl_GetObjResult(interp),
				    "cc", outgoing->cc) == TCL_OK
		&& peAppListStrStr(interp, Tcl_GetObjResult(interp),
				   "in-reply-to", outgoing->in_reply_to) == TCL_OK
		&& peAppListStrStr(interp, Tcl_GetObjResult(interp),
				   "subject", 
				   rfc1522_decode((unsigned char *) tmp_20k_buf,
						  SIZEOF_20KBUF,
						  outgoing->subject, NULL)) == TCL_OK
		&& (fcc ? peFccAppend(interp, Tcl_GetObjResult(interp), fcc, -1) : TRUE));


	/* Fill in x-reply-uid data and append it */
	if(!err && ps_global->mail_stream->uid_validity){
	    char *prefix = reply_quote_str(env);

	    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "(%d %s)(1 %lu %lu)%s",
		    strlen(prefix), prefix,
		    ps_global->mail_stream->uid_validity, uid,
		    ps_global->mail_stream->mailbox);

	    fs_give((void **) &prefix);

	    err = peAppListStrStr(interp, Tcl_GetObjResult(interp),
				  "x-reply-uid", tmp_20k_buf) != TCL_OK;
	}

	mail_free_envelope(&outgoing);

	if(err)
	  return(TCL_ERROR);
    }
    else
      Tcl_SetResult(interp, "", TCL_VOLATILE);

    return(TCL_OK);
}



/*
 * peReplyText - return subject used in reply to given message
 *
 * Params: 
 *
 * Returns:
 *
 */
int
peReplyText(Tcl_Interp *interp,
	    long uid,
	    int objc,
	    Tcl_Obj **objv)
{
    long	   msgno;
    char	  *prefix, *sect = NULL;
    int		   rv = TCL_OK, flowed = 0;
    ENVELOPE	  *env;
    BODY	  *body = NULL, *orig_body;
    STORE_S	  *msgtext;
    REDRAFT_POS_S *redraft_pos = NULL;
    Tcl_Obj	  *objBody = NULL, *objAttach = NULL;

    msgno = peSequenceNumber(uid);

    if((msgtext = (void *) so_get(CharStar, NULL, EDIT_ACCESS)) == NULL){
	Tcl_SetResult(interp, "Unable to create storage for reply text", TCL_VOLATILE);
	return(TCL_ERROR);
    }

    /*--- Grab current envelope ---*/
    /* if we're given a valid section number that
     * corresponds to a valid msg/rfc822 body part
     * then set up to reply the attached message's
     * text.
     */
    if(objc == 2 && objv[1]
       && (sect = Tcl_GetStringFromObj(objv[1], NULL)) && *sect != '\0'
       && (body = mail_body(ps_global->mail_stream, msgno, sect))
       && body->type == TYPEMESSAGE
       && !strucmp(body->subtype, "rfc822")){
	env       = body->nested.msg->env;
	orig_body = body->nested.msg->body;
    }
    else{
	sect = NULL;
	env = mail_fetchstructure(ps_global->mail_stream, msgno, &orig_body);
	if(!(env && orig_body)){
	    Tcl_SetResult(interp, "Unable to fetch message parts", TCL_VOLATILE);
	    return(TCL_ERROR);
	}
    }

    if(prefix = Tcl_GetStringFromObj(objv[0], NULL))
      prefix = cpystr(prefix);
    else
      prefix = reply_quote_str(env);

    /*
     * BUG? Should there be some way to signal to reply_bddy
     * that we'd like it to produced format=flowed body text?
     * right now it's hardwired to in pine/reply.c
     */

    if(body = reply_body(ps_global->mail_stream, env, orig_body,
			 msgno, sect, msgtext, prefix,
			 TRUE, NULL, TRUE, &redraft_pos)){

	objBody = Tcl_NewListObj(0, NULL);

	peSoStrToList(interp, objBody, msgtext);

	Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objBody);

	/* sniff for attachments */
	objAttach = peMsgAttachCollector(interp, body);

	Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objAttach);


	pine_free_body(&body);
    }
    else{
	Tcl_SetResult(interp, "Can't create body text", TCL_VOLATILE);
	rv = TCL_ERROR;
    }

    fs_give((void **) &prefix);

    return(rv);
}


int
peSoStrToList(Tcl_Interp *interp, Tcl_Obj *obj, STORE_S *so)
{
    char *sp, *ep;
    Tcl_Obj *objp;

    for(ep = (char *) so_text(so); *ep; ep++){
	sp = ep;

	while(*ep && *ep != '\n')
	  ep++;

#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
	objp = Tcl_NewByteArrayObj(sp, ep - sp);
#else
	objp = Tcl_NewStringObj(sp, ep - sp);
#endif

	if(Tcl_ListObjAppendElement(interp, obj, objp) != TCL_OK)
	  return(FALSE);
    }

    return(TRUE);
}


/*
 * peForwardHeaders - return subject used in forward of given message
 *
 * Params: 
 *
 * Returns:
 *
 */
int
peForwardHeaders(Tcl_Interp *interp,
		 long uid,
		 int objc,
		 Tcl_Obj **objv)
{

    int	      result;
    long      raw;
    char     *tmp, *sect = NULL;
    ENVELOPE *env;
    BODY     *body;

    raw = peSequenceNumber(uid);

    /* if we're given a valid section number that
     * corresponds to a valid msg/rfc822 body part
     * then set up headers in attached message.
     */
    if(objc == 1 && objv[0]
       && (sect = Tcl_GetStringFromObj(objv[0], NULL)) && *sect != '\0'
       && (body = mail_body(ps_global->mail_stream, raw, sect))
       && body->type == TYPEMESSAGE
       && !strucmp(body->subtype, "rfc822")){
	env = body->nested.msg->env;
    }
    else{
	sect = NULL;
	env = mail_fetchstructure(ps_global->mail_stream, raw, NULL);
    }

    if(env){
	tmp = forward_subject(env, FS_NONE);
	result = peAppListStrStr(interp, Tcl_GetObjResult(interp),
				 "subject", tmp);

	fs_give((void **) &tmp);

	return(result);
    }

    Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
    return(TCL_ERROR);
}



/*
 * peForwardText - return body of message used in
 *		   forward of given message
 *
 * Params: 
 *
 * Returns:
 *
 */
int
peForwardText(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    long	   msgno;
    char	  *bodtext, *p, *sect = NULL;
    int		   rv = TCL_OK;
    ENVELOPE	  *env;
    BODY	  *body, *orig_body;
    STORE_S	  *msgtext;
    REDRAFT_POS_S *redraft_pos = NULL;
    Tcl_Obj	  *objBody = NULL, *objAttach = NULL;

    msgno = peSequenceNumber(uid);

    if((msgtext = (void *) so_get(CharStar, NULL, EDIT_ACCESS)) == NULL){
	Tcl_SetResult(interp, "Unable to create storage for forward text", TCL_VOLATILE);
	return(TCL_ERROR);
    }

    /*--- Grab current envelope ---*/
    /* if we're given a valid section number that
     * corresponds to a valid msg/rfc822 body part
     * then set up to forward the attached message's
     * text.
     */
    if(objc == 1 && objv[0] &&
       (sect = Tcl_GetStringFromObj(objv[0], NULL)) && *sect != '\0'
       && (body = mail_body(ps_global->mail_stream, msgno, sect))
       && body->type == TYPEMESSAGE
       && !strucmp(body->subtype, "rfc822")){
	env       = body->nested.msg->env;
	orig_body = body->nested.msg->body;
    }
    else{
	sect = NULL;
	env = mail_fetchstructure(ps_global->mail_stream, msgno, &orig_body);
	if(!(env && orig_body)){
	    Tcl_SetResult(interp, "Unable to fetch message parts", TCL_VOLATILE);
	    return(TCL_ERROR);
	}
    }

    if(body = forward_body(ps_global->mail_stream, env, orig_body,
			   msgno, sect, msgtext, FWD_NONE)){

	bodtext = (char *) so_text(msgtext);

	objBody = Tcl_NewListObj(0, NULL);

	for(p = bodtext; *p; p++){
	    Tcl_Obj *objp;

	    bodtext = p;
	    while(*p && *p != '\n')
	      p++;

#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
	    objp = Tcl_NewByteArrayObj(bodtext, p - bodtext);
#else
	    objp = Tcl_NewStringObj(bodtext, p - bodtext);
#endif

	    Tcl_ListObjAppendElement(interp, objBody, objp);
	}

	Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objBody);

	/* sniff for attachments */
	objAttach = peMsgAttachCollector(interp, body);
	Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objAttach);

	pine_free_body(&body);
    }
    else{
	Tcl_SetResult(interp, "Can't create body text", TCL_VOLATILE);
	rv = TCL_ERROR;
    }

    return(rv);
}



/*
 * peDetach - 
 *
 * Params: argv[0] == attachment part number
 *         argv[1] == directory to hold tmp file
 *
 * Returns: list containing:
 *
 *		0) response: OK or ERROR
 *	     if OK
 *		1) attachment's mime type
 *		2) attachment's mime sub-type
 *		3) attachment's size in bytes (decoded)
 *		4) attachment's given file name (if any)
 *		5) tmp file holding raw attachment data
 */
int
peDetach(Tcl_Interp *interp,
	 long uid,
	 int objc,
	 Tcl_Obj **objv)
{
    char	 *part, *err, *tfd, *tfn = NULL, *filename;
    long	  raw;
    int		  value = 0;
    gf_io_t	  pc;
    MESSAGECACHE *mc;
    BODY	 *body;
    STORE_S	 *store;
    Tcl_Obj	 *rvobj, *tObj, *stObj, *fnObj;

    if((part = Tcl_GetStringFromObj(objv[0], NULL))
       && (raw = peSequenceNumber(uid))
       && (body = mail_body(ps_global->mail_stream, raw, part))){

	peGetMimeTyping(body, &tObj, &stObj, &fnObj, NULL);

	err = NULL;
	if(!(tfd = Tcl_GetStringFromObj(objv[1], NULL)) || *tfd == '\0'){
	    tfn = temp_nam(tfd = NULL, "pd", TN_BINARY);
	}
	else if(is_writable_dir(tfd) == 0){
	    tfn = temp_nam(tfd, "pd", TN_BINARY);
	}
	else
	  tfn = tfd;

	filename = Tcl_GetStringFromObj(fnObj, NULL);
	dprint((5, "PEDetach(name: %s, tmpfile: %s)",
		   filename ? filename : "<null>", tfn));

	if(store = so_get(FileStar, tfn, WRITE_ACCESS|OWNER_ONLY)){
	    gf_set_so_writec(&pc, store);
	    err = detach(ps_global->mail_stream, raw, part, 0L, NULL, pc, NULL, 0);
	    gf_clear_so_writec(store);
	    so_give(&store);
	}
	else
	  err = "Can't allocate internal storage";
    }
    else
      err = "Can't get message data";

    if(err){
	if(tfn)
	  unlink(tfn);

	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Detach: %s", err);
	Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
	return(TCL_ERROR);
    }

    /* package up response */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewListObj(1, &tObj));

    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewListObj(1, &stObj));

    rvobj = Tcl_NewLongObj(name_file_size(tfn));
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewListObj(1, &rvobj));
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewListObj(1, &fnObj));
    rvobj = Tcl_NewStringObj(tfn, -1);
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewListObj(1, &rvobj));

    return(TCL_OK);
}


/*
 * peAttachInfo - 
 *
 * Params: argv[0] == attachment part number
 *
 * Returns: list containing:
 *
 *		0) response: OK or ERROR
 *	     if OK
 *		1) attachment's mime type
 *		2) attachment's mime sub-type
 *		3) attachment's size in bytes (decoded)
 *		4) attachment's given file name (if any)
 *		5) tmp file holding raw attachment data
 */
int
peAttachInfo(Tcl_Interp *interp,
	     long uid,
	     int objc,
	     Tcl_Obj **objv)
{
    char	 *part;
    long	  raw;
    MESSAGECACHE *mc;
    BODY	 *body;
    PARMLIST_S	 *plist;
    Tcl_Obj	 *tObj, *stObj, *fnObj;

    if((part = Tcl_GetStringFromObj(objv[0], NULL))
       && (raw = peSequenceNumber(uid))
       && (body = mail_body(ps_global->mail_stream, raw, part))){

	peGetMimeTyping(body, &tObj, &stObj, &fnObj, NULL);
    }
    else{
	Tcl_SetResult(interp, "Can't get message data", TCL_STATIC);
	return(TCL_ERROR);
    }

    /* package up response */

    /* filename */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), fnObj);

    /* type */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), tObj);

    /* subtype */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), stObj);

    /* encoding */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewStringObj((body->encoding < ENCMAX)
						? body_encodings[body->encoding]
						: "Unknown", -1));

    /* parameters */
    if(plist = rfc2231_newparmlist(body->parameter)){
	Tcl_Obj *lObj = Tcl_NewListObj(0, NULL);
	Tcl_Obj *pObj[2];

	while(rfc2231_list_params(plist)){
	    pObj[0] = Tcl_NewStringObj(plist->attrib, -1);
	    pObj[1] = Tcl_NewStringObj(plist->value, -1);
	    Tcl_ListObjAppendElement(interp, lObj,
				     Tcl_NewListObj(2, pObj));
	}

	Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), lObj);
	rfc2231_free_parmlist(&plist);
    }

    /* size guesstimate */
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
			     Tcl_NewStringObj(comatose((body->encoding == ENCBASE64)
					       ? ((body->size.bytes * 3)/4)
					       : body->size.bytes), -1));

    return(TCL_OK);
}


/*
 * peSaveDefault - Default saved file name for the given message
 *	    specified collection/folder
 *
 * Params: 
 *	   
 * Returns: name of saved message folder or empty string
 *
 */
int
peSaveDefault(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    char      *folder;
    CONTEXT_S *cntxt, *cp;
    int	       colid;
    long       rawno;
    ENVELOPE  *env;

    if(uid){
	if(!(env = pine_mail_fetchstructure(ps_global->mail_stream,
				       rawno = peSequenceNumber(uid),
				       NULL))){
	    Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
	    return(TCL_ERROR);
	}
    }
    else
      env = NULL;

    if(!(folder = save_get_default(ps_global, env, rawno, NULL, &cntxt))){
	Tcl_SetResult(interp, "Message expunged!", TCL_VOLATILE);
	return(TCL_ERROR);		/* message expunged! */
    }

    for(colid = 0, cp = ps_global->context_list; cp && cp != cntxt ; colid++, cp = cp->next)
      ;

    if(!cp)
      colid = 0;

    (void) Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				    Tcl_NewIntObj(colid));
    (void) Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				    Tcl_NewStringObj(folder, -1));
    return(TCL_OK);
}


/*
 * peSave - Save message with given UID in current folder to
 *	    specified collection/folder
 *
 * Params: argv[0] == destination context number
 *	   argv[1] == testination foldername
 *
 *
 */
int
peSave(Tcl_Interp *interp,
       long uid,
       int objc,
       Tcl_Obj **objv)
{
    int	       flgs, i, colid;
    char      *folder, *err = NULL;
    CONTEXT_S *cp;

    if(Tcl_GetIntFromObj(interp, objv[0], &colid) != TCL_ERROR){
	if(folder = Tcl_GetStringFromObj(objv[1], NULL)){
	    mn_set_cur(ps_global->msgmap, peMessageNumber(uid));
	    for(i = 0, cp = ps_global->context_list; cp ; i++, cp = cp->next)
	      if(i == colid){
		  if(!READONLY_FOLDER(ps_global->mail_stream)
		     && F_OFF(F_SAVE_WONT_DELETE, ps_global))
		    flgs != SV_DELETE;

		  i = save(ps_global, ps_global->mail_stream,
			   cp, folder, ps_global->msgmap, flgs);

		  if(i == mn_total_cur(ps_global->msgmap)){
		      if(mn_total_cur(ps_global->msgmap) <= 1L){
			  if(ps_global->context_list->next
			     && context_isambig(folder)){
			      char *tag = (cp->nickname && strlen(cp->nickname)) ? cp->nickname : (cp->label && strlen(cp->label)) ? cp->label : "Folders";
			      snprintf(tmp_20k_buf, SIZEOF_20KBUF, 
				      "Message %s copied to \"%.15s%s\" in <%.15s%s>",
				      long2string(mn_get_cur(ps_global->msgmap)), folder,
				      (strlen(folder) > 15) ? "..." : "",
				      tag,
				      (strlen(tag) > 15) ? "..." : "");
			  }
			  else
			    snprintf(tmp_20k_buf, SIZEOF_20KBUF,
				    "Message %s copied to folder \"%.27s%s\"",
				    long2string(mn_get_cur(ps_global->msgmap)), folder,
				    (strlen(folder) > 27) ? "..." : "");
		      }
		      else
			snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%s messages saved",
				comatose(mn_total_cur(ps_global->msgmap)));

		      if(flgs & SV_DELETE){
			  strncat(tmp_20k_buf, " and deleted", SIZEOF_20KBUF);
			  tmp_20k_buf[SIZEOF_20KBUF-1] = '\0';
		      }

		      q_status_message(SM_ORDER, 0, 3, tmp_20k_buf);
		      return(TCL_OK);
		  }

		  err = ps_global->last_error;
	      }

	    if(!err)
	      err = "open: Unrecognized collection ID";
	}
	else
	  err = "open: Can't read folder";
    }
    else
      err = "open: Can't get collection ID";

    Tcl_SetResult(interp, err, TCL_VOLATILE);
    return(TCL_ERROR);
}


/*
 * peGotoDefault - Default Goto command file name for the given message
 *	    specified collection/folder
 *
 * Params: 
 *	   
 * Returns: name of Goto command default folder or empty string
 *
 */
int
peGotoDefault(Tcl_Interp *interp, long uid, Tcl_Obj **objv)
{
    char      *folder = NULL;
    CONTEXT_S *cntxt, *cp;
    int	       colid, inbox;
    long       rawno;
    ENVELOPE  *env;

    cntxt = broach_get_folder(ps_global->context_current, &inbox, &folder);

    for(colid = 0, cp = ps_global->context_list; cp != cntxt ; colid++, cp = cp->next)
      ;

    if(!cp)
      colid = 0;

    (void) Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				    Tcl_NewIntObj(colid));
    (void) Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				    Tcl_NewStringObj(folder ? folder : "", -1));
    return(TCL_OK);
}


/*
 * peReplyQuote - 
 *
 * Params: argv[0] == attachment part number
 *
 * Returns: list containing:
 *
 */
int
peReplyQuote(Tcl_Interp *interp,
	     long uid,
	     int objc,
	     Tcl_Obj **objv)
{
    char     *quote;
    ENVELOPE *env;

    if(uid){
	if(env = pine_mail_fetchstructure(ps_global->mail_stream, peSequenceNumber(uid), NULL)){
	    quote = reply_quote_str(env);
	    Tcl_SetResult(interp, quote, TCL_VOLATILE);
	    fs_give((void **) &quote);
	}
	else{
	    Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
	    return(TCL_ERROR);
	}
    }
    else
      Tcl_SetResult(interp, "> ", TCL_VOLATILE);

    return(TCL_OK);
}


void
peGetMimeTyping(BODY *body, Tcl_Obj **tObjp, Tcl_Obj **stObjp, Tcl_Obj **fnObjp, Tcl_Obj **extObjp)
{
    char *p, *ptype = NULL, *psubtype = NULL, *pfile = NULL, *att_name;

    /*-------  Figure out suggested file name ----*/
    att_name = "filename";

    if(body){
	if((body->disposition.type
	    && (p = rfc2231_get_param(body->disposition.parameter,
				      att_name, NULL, NULL)))
	   || (p = rfc2231_get_param(body->parameter,
				     att_name + 4, NULL, NULL))){
	    pfile = p;

	    /*
	     * if part is generic, see if we can get anything
	     * more from the suggested filename's extension...
	     */
	    if(body->type == TYPEAPPLICATION
	       && (!body->subtype
		   || !strucmp(body->subtype, "octet-stream"))){
		BODY *fakebody = mail_newbody();

		if(set_mime_type_by_extension(fakebody, pfile)){
		    ptype = body_type_names(fakebody->type);
		    psubtype = cpystr(fakebody->subtype);
		}

		mail_free_body(&fakebody);
	    }
	}

	if(!ptype) {
	    ptype = body_type_names(body->type);
	    psubtype = cpystr(body->subtype
			      ? body->subtype
			      : (body->type == TYPETEXT)
			      ? "plain"
			      : (body->type == TYPEAPPLICATION)
			      ? "octet-stream"
			      : "");
	}
    }
    else{
	ptype	 = ptype = body_type_names(TYPETEXT);
	psubtype = cpystr("plain");
    }

    if(extObjp){
	*extObjp = Tcl_NewStringObj("", 0);

	if(ptype && psubtype && pfile){
	    size_t l;
	    char *mtype;
	    char  extbuf[32];	/* mailcap.c limits to three */

	    l = strlen(ptype) + strlen(psubtype) + 1;
	    mtype = (char *) fs_get((l+1) * sizeof(char));
	
	    snprintf(mtype, l+1, "%s/%s", ptype, psubtype);

	    if(!set_mime_extension_by_type(extbuf, mtype)){
		char *dotp, *p;

		for(dotp = NULL, p = pfile; *p; p++)
		  if(*p == '.')
		    dotp = p + 1;

		if(dotp)
		  Tcl_SetStringObj(*extObjp, dotp, -1);
	    }
	    else
	      Tcl_SetStringObj(*extObjp, extbuf, -1);

	    fs_give((void **) &mtype);
	}
    }

    if(tObjp)
      *tObjp = Tcl_NewStringObj(ptype, -1);

    if(psubtype){
	if(stObjp)
	  *stObjp = Tcl_NewStringObj(psubtype, -1);

	fs_give((void **) &psubtype);
    }
    else if(stObjp)
      *stObjp = Tcl_NewStringObj("", 0);

    if(pfile){
	if(fnObjp)
	  *fnObjp = Tcl_NewStringObj(pfile, -1);

	fs_give((void **) &pfile);
    }
    else if(fnObjp)
      *fnObjp = Tcl_NewStringObj("", 0);
}



int
peAppListStrStr(Tcl_Interp *interp, Tcl_Obj *lobjp, char *field, char *value)
{
    Tcl_Obj *objv[2];

    objv[0] = Tcl_NewStringObj(field ? field : "", -1);
#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
    objv[1] = Tcl_NewByteArrayObj(value ? value : "", (value ? strlen(value) : 0));
#else
    objv[1] = Tcl_NewStringObj(value ? value : "", -1);
#endif

    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}



int
peAppListStrPat(Tcl_Interp *interp, Tcl_Obj *lobjp, char *field, PATTERN_S *value)
{
    Tcl_Obj *objv[2];
    char *patstr;

    patstr = pattern_to_string(value);

    objv[0] = Tcl_NewStringObj(field ? field : "", -1);
#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
    objv[1] = Tcl_NewByteArrayObj(patstr ? patstr : "", (patstr ? strlen(patstr) : 0));
#else
    objv[1] = Tcl_NewStringObj(patstr ? patstr : "", -1);
#endif

    fs_give((void **)&patstr);

    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}


int
peAppListStrPatstat(Tcl_Interp *interp, Tcl_Obj *lobjp, char *field, int value)
{
    Tcl_Obj *objv[2];

    objv[0] = Tcl_NewStringObj(field ? field : "", -1);
    switch(value){
      case PAT_STAT_EITHER:
	objv[1] = Tcl_NewStringObj("either", -1);
	break;
      case PAT_STAT_YES:
	objv[1] = Tcl_NewStringObj("yes", -1);
	break;
      case PAT_STAT_NO:
	objv[1] = Tcl_NewStringObj("no", -1);
	break;
    }

    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}


int
peAppListStrInt(Tcl_Interp *interp, Tcl_Obj *lobjp, char *field, int value)
{
    Tcl_Obj *objv[2];

    objv[0] = Tcl_NewStringObj(field, -1);
    objv[1] = Tcl_NewIntObj(value);

    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}


int
peAppListIntStr(interp, lobjp, field, value)
    Tcl_Interp *interp;
    Tcl_Obj    *lobjp;
    int		field;
    char       *value;
{
    Tcl_Obj *objv[2];

    objv[0] = Tcl_NewIntObj(field);
    objv[1] = Tcl_NewStringObj(value, -1);

    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}


int
peAppListLongLong(Tcl_Interp *interp, Tcl_Obj *lobjp, long x, long y)
{
    Tcl_Obj *objv[2];

    objv[0] = Tcl_NewLongObj(x);
    objv[1] = Tcl_NewLongObj(y);
    return(Tcl_ListObjAppendElement(interp, lobjp,
				    Tcl_NewListObj(2, objv)));
}


int
peAppListStrAddr(Tcl_Interp *interp, Tcl_Obj *lobjp, char *field, ADDRESS *addrs)
{
    char	*tmp, *p;
    Tcl_Obj	*objList = NULL, *objp;

    if(field){
	objList = Tcl_NewListObj(0, NULL);
	Tcl_ListObjAppendElement(interp, objList,
				 Tcl_NewStringObj(field, -1));	 

	if(addrs) {
	    RFC822BUFFER rbuf;
	    size_t len;

	    len = est_size(addrs);
	    tmp = (char *) fs_get(len * sizeof(char));
	    tmp[0] = '\0';
	    rbuf.f   = dummy_soutr;
	    rbuf.s   = NULL;
	    rbuf.beg = tmp;
	    rbuf.cur = tmp;
	    rbuf.end = tmp+len-1;
	    rfc822_output_address_list(&rbuf, addrs, 0L, NULL);
	    *rbuf.cur = '\0';
	    p = rfc1522_decode((unsigned char *) tmp_20k_buf, SIZEOF_20KBUF, tmp, NULL);
#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
	    objp = Tcl_NewByteArrayObj(p, strlen(p));
#else
	    objp = Tcl_NewStringObj(p, -1);
#endif
	    Tcl_ListObjAppendElement(interp, objList, objp);
	    fs_give((void **) &tmp);
	}
	else
	  Tcl_ListObjAppendElement(interp, objList,
				   Tcl_NewStringObj("", -1));
    }

    return(Tcl_ListObjAppendElement(interp, lobjp, objList));
}


/*
 * peCreateUserContext - create new ps_global and set it up
 */
char *
peCreateUserContext(Tcl_Interp *interp, char *user, char *config, char *defconf)
{
    Tcl_Obj *wObj;

    if(ps_global)
      peDestroyUserContext(&ps_global);

    set_collation();

    ps_global = new_pine_struct();

    /*----------------------------------------------------------------------
           Place any necessary constraints on pith processing
      ----------------------------------------------------------------------*/

    /* got thru close procedure without expunging */
    ps_global->noexpunge_on_close = 1;

    /* do NOT let user set path to local executable */
    ps_global->vars[V_SENDMAIL_PATH].is_user = 0;


    /*----------------------------------------------------------------------
           Proceed with reading acquiring user settings
      ----------------------------------------------------------------------*/
    if(ps_global->pinerc)
      fs_give((void **) &ps_global->pinerc);

    if(ps_global->prc)
      free_pinerc_s(&ps_global->prc);

    if(!IS_REMOTE(config)){
	snprintf(tmp_20k_buf, SIZEOF_20KBUF, "Non-Remote config: %s", config);
	return(tmp_20k_buf);
    }

    ps_global->prc = new_pinerc_s(config);

    if(defconf){
	if(ps_global->pconf)
	  free_pinerc_s(&ps_global->pconf);

	ps_global->pconf = new_pinerc_s(defconf);
    }

    /*
     * Fake up some user information
     */
    ps_global->ui.login = cpystr(user);

    /* CHECK FOR AND PASS BACK ANY INIT ERRORS */
    return(peLoadConfig(ps_global));
}



void
peDestroyUserContext(struct pine **pps)
{

    completely_done_with_adrbks();

    free_pinerc_strings(pps);
#if	0
    imap_flush_passwd_cache(TRUE);
#endif
    clear_index_cache(sp_inbox_stream(), 0);
    free_newsgrp_cache();
    mailcap_free();
    close_patterns(0L);
    free_extra_hdrs();
    free_contexts(&ps_global->context_list);

    pico_endcolor();

    free_pine_struct(pps);
}


char *
peLoadConfig(struct pine *pine_state)
{
    int   rv;
    char *db = NULL;

    if(!pine_state)
      return("No global state present");

#if	0
/*turned off because we don't care about local user*/
    /* need home directory early */
    get_user_info(&pine_state->ui);

    pine_state->home_dir = cpystr((getenv("HOME") != NULL)
				    ? getenv("HOME")
				    : pine_state->ui.homedir);
#endif

    init_pinerc(pine_state, &db);

    fs_give((void **) &db);

    /*
     * Initial allocation of array of stream pool pointers.
     * We do this before init_vars so that we can re-use streams used for
     * remote config files. These sizes may get changed later.
     */
    ps_global->s_pool.max_remstream  = 2;

    ps_global->c_client_error[0] = '\0';
    peCredentialError = 0;

    init_vars(pine_state, NULL);

    if(peCredentialError)
      return("Invalid Login Name or Password");
    else if(ps_global->c_client_error[0])
      return(ps_global->c_client_error);

    mail_parameters(NULL, SET_SENDCOMMAND, (void *) pine_imap_cmd_happened);
    mail_parameters(NULL, SET_FREESTREAMSPAREP, (void *) sp_free_callback);
    mail_parameters(NULL, SET_FREEELTSPAREP,    (void *) free_pine_elt);

    /*
     * Install callback to handle certificate validation failures,
     * allowing the user to continue if they wish.
     */
    mail_parameters(NULL, SET_SSLCERTIFICATEQUERY, (void *) alpine_sslcertquery);
    mail_parameters(NULL, SET_SSLFAILURE, (void *) alpine_sslfailure);

    /*
     * Set up a c-client read timeout and timeout handler.  In general,
     * it shouldn't happen, but a server crash or dead link can cause
     * pine to appear wedged if we don't set this up...
     */
    mail_parameters(NULL, SET_OPENTIMEOUT,
		    (void *)((pine_state->VAR_TCPOPENTIMEO
			      && (rv = atoi(pine_state->VAR_TCPOPENTIMEO)) > 4)
			       ? (long) rv : 30L));
    mail_parameters(NULL, SET_TIMEOUT, (void *) alpine_tcptimeout);

    if(pine_state->VAR_RSHOPENTIMEO
	&& ((rv = atoi(pine_state->VAR_RSHOPENTIMEO)) == 0 || rv > 4))
      mail_parameters(NULL, SET_RSHTIMEOUT, (void *) rv);

    if(pine_state->VAR_SSHOPENTIMEO
	&& ((rv = atoi(pine_state->VAR_SSHOPENTIMEO)) == 0 || rv > 4))
      mail_parameters(NULL, SET_SSHTIMEOUT, (void *) rv);

    /*
     * Tell c-client not to be so aggressive about uid mappings
     */
    mail_parameters(NULL, SET_UIDLOOKAHEAD, (void *) 20);

    /*
     * Setup referral handling
     */
    mail_parameters(NULL, SET_IMAPREFERRAL, (void *) imap_referral);
    mail_parameters(NULL, SET_MAILPROXYCOPY, (void *) imap_proxycopy);

    /*
     * Install extra headers to fetch along with all the other stuff
     * mail_fetch_structure and mail_fetch_overview requests.
     */
    calc_extra_hdrs();

    if(get_extra_hdrs())
      (void) mail_parameters(NULL, SET_IMAPEXTRAHEADERS, (void *) get_extra_hdrs());

    (void) init_username(pine_state);

    (void) init_hostname(ps_global);

#ifdef ENABLE_LDAP
    (void) init_ldap_pname(ps_global);
#endif /* ENABLE_LDAP */
    
    if(ps_global->prc && ps_global->prc->type == Loc &&
       can_access(ps_global->pinerc, ACCESS_EXISTS) == 0 &&
       can_access(ps_global->pinerc, EDIT_ACCESS) != 0)
      ps_global->readonly_pinerc = 1;
      
    /*
     * c-client needs USR2 and we might as well
     * do something sensible with HUP and TERM
     */
    init_signals();

    strncpy(pine_state->inbox_name, INBOX_NAME, sizeof(pine_state->inbox_name));
    pine_state->inbox_name[sizeof(pine_state->inbox_name)-1] = '\0';

    init_folders(pine_state);		/* digest folder spec's */

    /*
     * Various options we want to make sure are set OUR way
     */
    F_TURN_ON(F_QUELL_IMAP_ENV_CB, pine_state);
    F_TURN_ON(F_PASS_CONTROL_CHARS, pine_state);
    F_TURN_ON(F_SLCTBL_ITEM_NOBOLD, pine_state);

    /*
     * Fake screen dimensions for index formatting and
     * message display wrap...
     */
    ps_global->ttyo = (struct ttyo *) fs_get(sizeof(struct ttyo));
    ps_global->ttyo->screen_rows = FAKE_SCREEN_LENGTH;
    ps_global->ttyo->screen_cols = FAKE_SCREEN_WIDTH;
    if(ps_global->VAR_WP_COLUMNS){
	int w = atoi(ps_global->VAR_WP_COLUMNS);
	if(w >= 20 && w <= 128)
	  ps_global->ttyo->screen_cols = w;
    }

    ps_global->ttyo->header_rows = 0;
    ps_global->ttyo->footer_rows = 0;


    /* init colors */
    if(ps_global->VAR_NORM_FORE_COLOR)
      pico_nfcolor(ps_global->VAR_NORM_FORE_COLOR);

    if(ps_global->VAR_NORM_BACK_COLOR)
      pico_nbcolor(ps_global->VAR_NORM_BACK_COLOR);

    if(ps_global->VAR_REV_FORE_COLOR)
      pico_rfcolor(ps_global->VAR_REV_FORE_COLOR);

    if(ps_global->VAR_REV_BACK_COLOR)
      pico_rbcolor(ps_global->VAR_REV_BACK_COLOR);

    pico_set_normal_color();

    return(NULL);
}


int
peCreateStream(Tcl_Interp *interp, CONTEXT_S *context, char *mailbox, int do_inbox)
{
    int		   rv;
    char	  *s;
    unsigned long  flgs = 0L;

    ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
    peCredentialError = peNoPassword = 0;

    if(do_inbox)
      flgs |= DB_INBOXWOCNTXT;

    if(do_broach_folder(mailbox, context, NULL, flgs) && ps_global->mail_stream){
	dprint((SYSDBG_INFO, "Mailbox open: %s",
		ps_global->mail_stream->mailbox ? ps_global->mail_stream->mailbox : "<UNKNOWN>"));
	return(TCL_OK);
    }

    Tcl_SetResult(interp,
		  (peNoPassword || peCredentialError)
		    ? AUTH_FAILURE_STRING
		    : (*ps_global->last_error)
		       ? ps_global->last_error
		       : "Login Error",
		  TCL_VOLATILE);
    return(TCL_ERROR);
}


void
peDestroyStream(struct pine *ps)
{
    int cur_is_inbox;

    if(ps){
	cur_is_inbox = (sp_inbox_stream() == ps_global->mail_stream);

	/* clean up open streams */
	if(ps->mail_stream){
	    expunge_and_close(ps->mail_stream, NULL, EC_NONE);
	    ps_global->mail_stream = NULL;
	    ps_global->cur_folder[0] = '\0';
	}

	if(ps->msgmap)
	  mn_give(&ps->msgmap);

	if(sp_inbox_stream() && !cur_is_inbox){
	    ps->mail_stream   = sp_inbox_stream();
	    ps->msgmap	      = sp_msgmap(ps->mail_stream);
	    sp_set_expunge_count(ps_global->mail_stream, 0L);
	    expunge_and_close(sp_inbox_stream(), NULL, EC_NONE);
	    mn_give(&ps->msgmap);
	}
    }
}



int
peMessageBounce(Tcl_Interp *interp,
		long uid,
		int objc,
		Tcl_Obj **objv)

{
    char      *errstr = NULL, *to, *subj = NULL, errbuf[WP_MAX_POST_ERROR + 1];
    long       rawno;
    ENVELOPE  *env, *outgoing = NULL;
    METAENV   *metaenv;
    PINEFIELD *custom;
    BODY      *body = NULL;

    if(uid){
	rawno = peSequenceNumber(uid);

	if(objc > 0 && objv[0] && (to = Tcl_GetStringFromObj(objv[0], NULL))){
	    if(objc == 2 && objv[1]){
		subj = Tcl_GetStringFromObj(objv[1], NULL);
	    }
	    else if(env = mail_fetchstructure(ps_global->mail_stream, rawno, NULL)){
		subj = env->subject;
	    }
	    else{
		Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
		return(TCL_ERROR);
	    }

	    if((errstr = bounce_msg_body(ps_global->mail_stream, rawno, NULL,
					 &to, subj, &outgoing, &body, NULL))){
		Tcl_SetResult(interp, errstr, TCL_VOLATILE);
		return(TCL_ERROR);
	    }

	    metaenv = pine_new_env(outgoing, NULL, NULL, custom = peCustomHdrs());

	    if(!outgoing->from)
	      outgoing->from = generate_from();

	    rfc822_date(tmp_20k_buf);
	    outgoing->date = cpystr(tmp_20k_buf);

	    outgoing->return_path = rfc822_cpy_adr(outgoing->from);
	    if(!outgoing->message_id)
	      outgoing->message_id = generate_message_id();

	    /* NO FCC  */

	    if(peDoPost(metaenv, body, NULL, NULL, errbuf) != TCL_OK)
	      errstr = errbuf;

	    pine_free_body(&body);

	    pine_free_env(&metaenv);

	    if(custom)
	      free_customs(custom);

	    mail_free_envelope(&outgoing);
	    pine_free_body(&body);

	}
    }

    Tcl_SetResult(interp, (errstr) ? errstr : "OK", TCL_VOLATILE);
    return(errstr ? TCL_ERROR : TCL_OK);
}


int
peMessageSpam(Tcl_Interp *interp,
	      long uid,
	      int objc,
	      Tcl_Obj **objv)
{
    char	*errstr = NULL, *to, *subj = NULL, errbuf[WP_MAX_POST_ERROR + 1], *tmp_a_string;
    long	 rawno;
    ENVELOPE	*env, *outgoing;
    METAENV	*metaenv;
    PINEFIELD	*custom;
    BODY	*body;
    static char	*fakedomain = "@";
    void	*msgtext;

    if(uid){
	rawno = peSequenceNumber(uid);

	if(objc > 0 && objv[0] && (to = Tcl_GetStringFromObj(objv[0], NULL)) && strlen(to)){
	    if((env = mail_fetchstructure(ps_global->mail_stream, rawno, NULL)) == NULL){
		Tcl_SetResult(interp, ps_global->last_error, TCL_VOLATILE);
		return(TCL_ERROR);
	    }

	    /* empty subject gets "spam" subject */
	    if(!(objc == 2 && objv[1]
		 && (subj = Tcl_GetStringFromObj(objv[1], NULL)) != NULL
		 && *subj))
	      subj = env->subject;

	    /*---- New Body to start with ----*/
	    body       = mail_newbody();
	    body->type = TYPEMULTIPART;

	    /*---- The TEXT part/body ----*/
	    body->nested.part            = mail_newbody_part();
	    body->nested.part->body.type = TYPETEXT;

	    if((msgtext = (void *)so_get(CharStar, NULL, EDIT_ACCESS)) == NULL){
		pine_free_body(&body);
		Tcl_SetResult(interp, "peMessageSpam: Can't allocate text", TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	    else{
		sprintf(tmp_20k_buf,
			"The attached message is being reported to <%s> as Spam\n",
			to);
		so_puts((STORE_S *) msgtext, tmp_20k_buf);
		body->nested.part->body.contents.text.data = msgtext;
	    }

	    /*---- Attach the raw message ----*/
	    if(forward_mime_msg(ps_global->mail_stream, rawno, NULL, env,
				&(body->nested.part->next), msgtext)){
		outgoing = mail_newenvelope();
		metaenv = pine_new_env(outgoing, NULL, NULL, custom = peCustomHdrs());
	    }
	    else{
		pine_free_body(&body);
		Tcl_SetResult(interp, "peMessageSpam: Can't generate forwarded message", TCL_VOLATILE);
		return(TCL_ERROR);
	    }

	    /* rfc822_parse_adrlist feels free to destroy input so copy */
	    tmp_a_string = cpystr(to);
	    rfc822_parse_adrlist(&outgoing->to, tmp_a_string,
				 (F_ON(F_COMPOSE_REJECTS_UNQUAL, ps_global))
				   ? fakedomain : ps_global->maildomain);
	    fs_give((void **) &tmp_a_string);

	    outgoing->from	  = generate_from();
	    outgoing->subject	  = cpystr(subj);
	    outgoing->return_path = rfc822_cpy_adr(outgoing->from);
	    outgoing->message_id  = generate_message_id();

	    rfc822_date(tmp_20k_buf);
	    outgoing->date = cpystr(tmp_20k_buf);

	    /* NO FCC for Spam Reporting  */

	    if(peDoPost(metaenv, body, NULL, NULL, errbuf) != TCL_OK)
	      errstr = errbuf;

	    pine_free_body(&body);

	    pine_free_env(&metaenv);

	    if(custom)
	      free_customs(custom);

	    mail_free_envelope(&outgoing);
	    pine_free_body(&body);

	}
    }

    Tcl_SetResult(interp, (errstr) ? errstr : "OK", TCL_VOLATILE);
    return(errstr ? TCL_ERROR : TCL_OK);
}


/* * * * * * * * * * * * *  Start of Composer Routines * * * * * * * * * * * * /


/*
 * PEComposeCmd - export various bits of alpine state
 */
int
PEComposeCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err = "PECompose: Unknown request";

    dprint((2, "PEComposeCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else if(!ps_global){
	Tcl_SetResult(interp, "peCompose: no config present", TCL_STATIC);
	return(TCL_ERROR);
    }
    else{
	char *s1 = Tcl_GetStringFromObj(objv[1], NULL);

	if(s1){
	    if(!strcmp(s1, "post")){
		return(peMsgCollector(interp, objc - 2, (Tcl_Obj **) &objv[2], peDoPost));
	    }
	    else if(objc == 2){
		if(!strcmp(s1, "userhdrs")){
		    int	       i, j;
		    char      *p;
		    PINEFIELD *custom, *cp;
		    ADDRESS   *from;
		    static char *standard[] = {"To", "Cc", "Bcc", "Fcc", "Attach", "Subject", NULL};

		    custom = peCustomHdrs();

		    for(i = 0; standard[i]; i++){
			p = NULL;
			for(cp = custom; cp; cp = cp->next)
			  if(!strucmp(cp->name, standard[i]))
			    p = cp->textbuf;

			peAppListStrStr(interp, Tcl_GetObjResult(interp), standard[i], p);
		    }

		    for(cp = custom; cp != NULL; cp = cp->next){
			if(!strucmp(cp->name, "from")){
			    if(F_OFF(F_ALLOW_CHANGING_FROM, ps_global))
			      continue;

			    if(cp->textbuf && strlen(cp->textbuf)){
				p = cp->textbuf;
			    }
			    else{
				RFC822BUFFER rbuf;

				tmp_20k_buf[0] = '\0';
				rbuf.f   = dummy_soutr;
				rbuf.s   = NULL;
				rbuf.beg = tmp_20k_buf;
				rbuf.cur = tmp_20k_buf;
				rbuf.end = tmp_20k_buf+SIZEOF_20KBUF-1;
				rfc822_output_address_list(&rbuf, from = generate_from(), 0L, NULL);
				*rbuf.cur = '\0';
				mail_free_address(&from);
				p = tmp_20k_buf;
			    }
			}
			else{
			    p = cp->textbuf;
			    for(i = 0; standard[i]; i++)
			      if(!strucmp(standard[i], cp->name))
				p = NULL;

			    if(!p)
			      continue;
			}

			peAppListStrStr(interp, Tcl_GetObjResult(interp), cp->name, p);
		    }

		    if(custom)
		      free_customs(custom);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "syshdrs")){
		    int	     i;
		    static char *extras[] = {"In-Reply-To", "X-Reply-UID", NULL};

		    for(i = 0; extras[i]; i++)
		      peAppListStrStr(interp, Tcl_GetObjResult(interp), extras[i], NULL);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "composehdrs")){
		    char **p, *q;

		    if((p = ps_global->VAR_COMP_HDRS) && *p){
			for(; *p; p++)
			  Tcl_ListObjAppendElement(interp,
						   Tcl_GetObjResult(interp),
						   Tcl_NewStringObj(*p, (q = strchr(*p, ':'))
									   ? (q - *p) : -1));
		    }
		    else
		      Tcl_SetResult(interp, "", TCL_STATIC);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "fccdefault")){
		    int	       ci = 0;
		    CONTEXT_S *c = default_save_context(ps_global->context_list), *c2;

		    for(c2 = ps_global->context_list; c && c != c2; c2 = c2->next)
		      ci++;

		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewIntObj(ci));
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(ps_global->VAR_DEFAULT_FCC
							        ? ps_global->VAR_DEFAULT_FCC
								: "", -1));
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "noattach")){
		    peFreeAttach(&peCompAttach);
		    Tcl_SetResult(interp, "OK", TCL_VOLATILE);
		    return(TCL_OK);
		}
		else if(!strcmp(s1, "from")){
		    RFC822BUFFER rbuf;
		    ADDRESS *from = generate_from();
		    tmp_20k_buf[0] = '\0';
		    rbuf.f   = dummy_soutr;
		    rbuf.s   = NULL;
		    rbuf.beg = tmp_20k_buf;
		    rbuf.cur = tmp_20k_buf;
		    rbuf.end = tmp_20k_buf+SIZEOF_20KBUF-1;
		    rfc822_output_address_list(&rbuf, from, 0L, NULL);
		    *rbuf.cur = '\0';
		    mail_free_address(&from);
		    Tcl_SetResult(interp, tmp_20k_buf, TCL_VOLATILE);
		    return(TCL_OK);
		}
	    }
	    else if(objc == 3){
		if(!strcmp(s1, "unattach")){
		    char *id;

		    if(id = Tcl_GetStringFromObj(objv[2], NULL)){
			if(peClearAttachID(id)){
			    Tcl_SetResult(interp, "OK", TCL_STATIC);
			    return(TCL_OK);
			}
			else
			  err = "Can't access attachment id";
		    }
		    else
		      err = "Can't read attachment id";
		}
		else if(!strcmp(s1, "attachinfo")){
		    COMPATT_S *a;
		    char      *id, *s;

		    /* return: remote-filename size "type/subtype" */
		    if(id = Tcl_GetStringFromObj(objv[2], NULL)){
			if(a = peGetAttachID(id)){
			    if(a->file){
				/* file name */
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj(a->l.f.remote,-1));

				/* file size */
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewLongObj(a->l.f.size));

				/* type/subtype */
				snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%s/%s", a->l.f.type, a->l.f.subtype);
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj(tmp_20k_buf,-1));

				/* description */
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							 Tcl_NewStringObj((a->l.f.description) ? a->l.f.description : "",-1));
				return(TCL_OK);
			    }
			    else if(a->body){
				char *name;

				/* file name */
				if((name = rfc2231_get_param(a->l.b.body->disposition.parameter, "filename", NULL, NULL))
				   || (name = rfc2231_get_param(a->l.b.body->parameter, "name", NULL, NULL))){
				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj(name, -1));
				    fs_give((void **) &name);
				}
				else
				  Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj("Unknown", -1));

				/* file size */
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							 Tcl_NewLongObj((a->l.b.body->encoding == ENCBASE64)
									? ((a->l.b.body->size.bytes * 3)/4)
									: a->l.b.body->size.bytes));

				/* type/subtype */
				snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%s/%s",
					body_type_names(a->l.b.body->type),
					a->l.b.body->subtype ? a->l.b.body->subtype : "Unknown");

				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj(tmp_20k_buf, -1));

				/* description */
				if(a->l.b.body->description){
				    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%.*s", 256, a->l.b.body->description);
				}
				else if(s = rfc2231_get_param(a->l.b.body->parameter, "description", NULL, NULL)){
				    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%.*s", 256, s);
				    fs_give((void **) &s);
				}
				else
				  tmp_20k_buf[0] = '\0';

				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), Tcl_NewStringObj(tmp_20k_buf, -1));

				return(TCL_OK);
			    }

			    err = "Unknown attachment type";
			}
			else
			  err = "Can't access attachment id";
		    }
		    else
		      err = "Can't read attachment id";
		}
	    }
	    else if(objc == 7){
		if(!strcmp(s1, "attach")){
		    char *file, *remote, *type, *subtype, *desc;

		    if((file = Tcl_GetStringFromObj(objv[2], NULL))
		       && (type = Tcl_GetStringFromObj(objv[3], NULL))
		       && (subtype = Tcl_GetStringFromObj(objv[4], NULL))
		       && (remote = Tcl_GetStringFromObj(objv[5], NULL))){
			int  dl;

#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
			desc = Tcl_GetByteArrayFromObj(objv[6], &dl);
			
#else
			desc = Tcl_GetStringFromObj(objv[6], NULL);
			dl   = strlen(desc);
#endif
			if(desc){
			    Tcl_SetResult(interp, peFileAttachID(file, type, subtype, remote, desc, dl), TCL_VOLATILE);
			    return(TCL_OK);
			}
			else
			  err = "Can't read file description";
		    }
		    else
		      err = "Can't read file name";
		}
	    }
	}
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


COMPATT_S *
peNewAttach(void)
{
    COMPATT_S *p = (COMPATT_S *) fs_get(sizeof(COMPATT_S));
    memset(p, 0, sizeof(COMPATT_S));
    return(p);
}


void
peFreeAttach(COMPATT_S **a)
{
    if(a && *a){
	fs_give((void **) &(*a)->id);

	if((*a)->file){
	    if((*a)->l.f.type)
	      fs_give((void **) &(*a)->l.f.type);

	    if((*a)->l.f.subtype)
	      fs_give((void **) &(*a)->l.f.subtype);

	    if((*a)->l.f.remote)
	      fs_give((void **) &(*a)->l.f.remote);

	    if((*a)->l.f.local){
		(void) unlink((*a)->l.f.local);
		fs_give((void **) &(*a)->l.f.local);
	    }

	    if((*a)->l.f.description)
	      fs_give((void **) &(*a)->l.f.description);
	}
	else if((*a)->body){
	    pine_free_body(&(*a)->l.b.body);
	}

	peFreeAttach(&(*a)->next);
	fs_give((void **) a);
    }
}


char *
peFileAttachID(char *f, char *t, char *st, char *r, char *d, int dl)
{
    COMPATT_S *ap = peNewAttach(), *p;
    long       hval;

    ap->file = TRUE;
    ap->l.f.local = cpystr(f);
    ap->l.f.size = name_file_size(f);

    hval = line_hash(f);
    while(1)			/* collisions? */
      if(peGetAttachID(ap->id = cpystr(long2string(hval)))){
	  fs_give((void **) &ap->id);
	  hval += 1;
      }
      else
	break;

    ap->l.f.remote = cpystr(r ? r : "");
    ap->l.f.type = cpystr(t ? t : "Text");
    ap->l.f.subtype = cpystr(st ? st : "Plain");

    ap->l.f.description = fs_get(dl + 1);
    snprintf(ap->l.f.description, dl + 1, "%s", d);

    if(p = peCompAttach){
	do
	  if(!p->next){
	      p->next = ap;
	      break;
	  }
	while(p = p->next);
    }
    else
      peCompAttach = ap;

    return(ap->id);
}


char *
peBodyAttachID(BODY *b)
{
    COMPATT_S *ap = peNewAttach(), *p;
    long       hval;

    ap->body = TRUE;
    ap->l.b.body = copy_body(NULL, b);

    hval = b->id ? line_hash(b->id) : time(0);
    while(1)			/* collisions? */
      if(peGetAttachID(ap->id = cpystr(long2string(hval)))){
	  fs_give((void **) &ap->id);
	  hval += 1;
      }
      else
	break;

    /* move contents pointer to copy */
    peBodyMoveContents(b, ap->l.b.body);

    if(p = peCompAttach){
	do
	  if(!p->next){
	      p->next = ap;
	      break;
	  }
	while(p = p->next);
    }
    else
      peCompAttach = ap;

    return(ap->id);
}


void
peBodyMoveContents(BODY *bs, BODY *bd)
{
    if(bs && bd){
	if(bs->type == TYPEMULTIPART && bd->type == TYPEMULTIPART){
	    PART *ps = bs->nested.part,
		 *pd = bd->nested.part;
	    do				/* for each part */
	      peBodyMoveContents(&ps->body, &pd->body);
	    while ((ps = ps->next) && (pd = pd->next));	/* until done */
	}
	else if(bs->contents.text.data){
	    bd->contents.text.data = bs->contents.text.data;
	    bs->contents.text.data = NULL;
	}
    }
}



COMPATT_S *
peGetAttachID(char *h)
{
    COMPATT_S *p;

    for(p = peCompAttach; p; p = p->next)
      if(!strcmp(p->id, h))
	return(p);

    return(NULL);
}


int
peClearAttachID(char *h)
{
    COMPATT_S *pp, *pt = NULL;

    for(pp = peCompAttach; pp; pp = pp->next){
	if(!strcmp(pp->id, h)){
	    if(pt)
	      pt->next = pp->next;
	    else
	      peCompAttach = pp->next;

	    pp->next = NULL;
	    peFreeAttach(&pp);
	    return(TRUE);
	}

	pt = pp;
    }

    return(FALSE);
}


/*
 * peDoPost - handle preparing header and body text for posting, prepare
 *	      for any Fcc, then call the mailer to send it out
 */
int
peDoPost(METAENV *metaenv, BODY *body, char *fcc, CONTEXT_S **fcc_cntxtp, char *errp)
{
    int	       rv = TCL_OK, recipients;

    if(commence_fcc(fcc, fcc_cntxtp, TRUE)){

	ps_global->c_client_error[0] = ps_global->last_error[0] = '\0';
	peCredentialError = peNoPassword = 0;
	
	if((recipients = (metaenv->env->to || metaenv->env->cc || metaenv->env->bcc))
	   && call_mailer(metaenv, body, NULL, 0, NULL) < 0){
	    if(peNoPassword){
		strcpy(errp, AUTH_EMPTY_STRING);
	    }
	    else if(peCredentialError){
		strcpy(errp, AUTH_FAILURE_STRING);
	    }
	    else if(ps_global->last_error[0]){
		sprintf(errp, "Send Error: %.*s", 64, ps_global->last_error);
	    }
	    else if(ps_global->c_client_error[0]){
		sprintf(errp, "Send Error: %.*s", 64, ps_global->c_client_error);
	    }
	    else
	      strcpy(errp, "Sending Failure");

	    rv = TCL_ERROR;
	    dprint((1, "call_mailer failed!"));
	}
	else if(!wrapup_fcc(fcc, *fcc_cntxtp, recipients ? NULL : metaenv, body)){
	    strcpy(errp, "Fcc Failed!.  No message saved.");
	    rv = TCL_ERROR;
	    dprint((1, "explicit fcc write failed!"));
	}
	else{
	    PINEFIELD *pf;
	    REPLY_S *reply = NULL;

	    /* success, now look for x-reply-uid to flip answered flag for? */

	    for(pf = metaenv->local; pf && pf->name; pf = pf->next)
	      if(!strucmp(pf->name, "x-reply-uid")){
		  if(pf->textbuf){
		      if(reply = (REPLY_S *) build_reply_uid(pf->textbuf)){

			  update_answered_flags(reply);

			  if(reply->mailbox)
			    fs_give((void **) &reply->mailbox);

			  if(reply->prefix)
			    fs_give((void **) &reply->prefix);

			  if(reply->data.uid.msgs)
			    fs_give((void **) &reply->data.uid.msgs);

			  fs_give((void **) &reply);
		      }
		  }

		  break;
	      }
	}
    }
    else{
	dprint((1,"can't open fcc, cont"));
	
	strcpy(errp, "Can't open Fcc");
	rv = TCL_ERROR;
    }

    return(rv);
}



/*
 * pePostponeCmd - export various bits of alpine state
 */
int
PEPostponeCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err = "PEPostpone: unknown request";
    long  uid;

    dprint((2, "PEPostponeCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else if(!ps_global){
	Tcl_SetResult(interp, "pePostpone: no config present", TCL_STATIC);
	return(TCL_ERROR);
    }
    else{
	char *s1 = Tcl_GetStringFromObj(objv[1], NULL);

	if(s1){
	    if(objc == 2){
		if(!strcmp(s1, "any")){
		    MAILSTREAM *stream;

		    if(postponed_stream(&stream) && stream){
			Tcl_SetResult(interp, "1", TCL_STATIC);

			if(stream != ps_global->mail_stream)
			  pine_mail_close(stream);
		    }
		    else
		      Tcl_SetResult(interp, "0", TCL_STATIC);

		    return(TCL_OK);
		}
		else if(!strcmp(s1, "list")){
		    MAILSTREAM *stream;
		    ENVELOPE   *env;
		    Tcl_Obj    *objEnv = NULL, *objEnvList;
		    long	n;
		    char       *charset = NULL, *cs;

		    if(postponed_stream(&stream) && stream){
			if(!stream->nmsgs){
			    (void) redraft_cleanup(&stream, FALSE, REDRAFT_PPND);
			    Tcl_SetResult(interp, "", TCL_STATIC);
			    return(TCL_OK);
			}

			objEnvList = Tcl_NewListObj(0, NULL);

			for(n = 1; n <= stream->nmsgs; n++){
			    if(env = pine_mail_fetchstructure(stream, n, NULL)){
				objEnv = Tcl_NewListObj(0, NULL);

				peAppListStrStr(interp, objEnv, "uid",
						long2string(mail_uid(stream, n)));

				peAppListStrAddr(interp, objEnv, "to", env->to);

				date_str(env->date, iSDate, 1, tmp_20k_buf, SIZEOF_20KBUF);

				peAppListStrStr(interp, objEnv, "date", tmp_20k_buf);

				peAppListStrStr(interp, objEnv, "subj",
						rfc1522_decode((unsigned char *) tmp_20k_buf,
							       SIZEOF_20KBUF, env->subject, &cs));
				if(cs){
				    if(*cs && strucmp(cs, "us-ascii")){
					if(charset){
					    if(strucmp(charset, cs)){
						fs_give((void **) &charset);
						charset = cpystr("X-UNKNOWN");
					    }

					    fs_give((void **) &cs);
					}
					else
					  charset = cs;
				    }
				    else
				      fs_give((void **) &cs);
				}

				Tcl_ListObjAppendElement(interp, objEnvList, objEnv);
			    }
			}

			Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objEnvList);

			if(charset){
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(charset, -1));
			    fs_give((void **) &charset);
			}

			if(stream != ps_global->mail_stream)
			  pine_mail_close(stream);
		    }

		    return(TCL_OK);
		}
	    }
	    else if(objc == 3){
		if(!strcmp(s1, "extract")){
		    if(Tcl_GetLongFromObj(interp, objv[2], &uid) == TCL_OK){
			Tcl_Obj	     *objHdr = NULL, *objBod = NULL, *objAttach = NULL, *objOpts = NULL;
			MAILSTREAM    *stream;
			BODY	      *b;
			ENVELOPE      *env = NULL;
			PINEFIELD     *custom = NULL, *cp;
			REPLY_S	      *reply = NULL;
			ACTION_S      *role = NULL;
			STORE_S	      *so;
			long	       n;
			int	       j;
			char	      *fcc = NULL,
				      *lcc = NULL;

			if(postponed_stream(&stream) && stream){
			    if(so = so_get(CharStar, NULL, EDIT_ACCESS)){
				if((n = mail_msgno(stream, uid)) > 0L
				   && redraft_work(&stream, n, &env, &b, &fcc, &lcc, &reply,
						   NULL, &custom, &role, /* should role be NULL? */
						   REDRAFT_DEL | REDRAFT_PPND, so)){
				    char *charset = NULL;

				    /* prepare to package up for caller */
				    objHdr = Tcl_NewListObj(0, NULL);

				    /* determine body part's charset */
				    if(charset = rfc2231_get_param(b->parameter,"charset",NULL,NULL)){
					objOpts = Tcl_NewListObj(0, NULL);
					peAppListStrStr(interp, objOpts, "charset", charset);
					fs_give((void **) &charset);
				    }
				      
				    peAppListStrAddr(interp, objHdr, "from",
						     role && role->from ? role->from : env->from);
				    peAppListStrAddr(interp, objHdr, "to", env->to);
				    peAppListStrAddr(interp, objHdr, "cc", env->cc);
				    peAppListStrAddr(interp, objHdr, "bcc", env->bcc);
				    peAppListStrStr(interp, objHdr, "in-reply-to", env->in_reply_to);
				    peAppListStrStr(interp, objHdr, "subject",
						    rfc1522_decode((unsigned char *) tmp_20k_buf,
								   SIZEOF_20KBUF, env->subject, NULL));

				    if(fcc)
				      peFccAppend(interp, objHdr, fcc, -1);

				    for(cp = custom; cp && cp->name; cp = cp->next)
				      switch(cp->type){
					case Address :
					  strncpy(tmp_20k_buf, cp->name, SIZEOF_20KBUF);
					  tmp_20k_buf[SIZEOF_20KBUF-1] = '\0';
					  peAppListStrAddr(interp, objHdr,
							   lcase(tmp_20k_buf),
							   *cp->addr);
					  break;

					case Attachment :
					  break;

					case Fcc :
					case Subject :
					  break; /* ignored */

					default :
					  strncpy(tmp_20k_buf, cp->name, SIZEOF_20KBUF);
					  tmp_20k_buf[SIZEOF_20KBUF-1] = '\0';
					  peAppListStrStr(interp, objHdr,
							  lcase(tmp_20k_buf),
							  cp->textbuf ? cp->textbuf : cp->val);
					  break;
				      }

				    if(reply){
					/* blat x-Reply-UID: for possible use? */
					if(reply->flags == REPLY_UID){
					    char uidbuf[MAILTMPLEN], *p;
					    long i;

					    for(i = 0L, p = tmp_20k_buf; reply->data.uid.msgs[i]; i++){
						if(i)
						  sstrncpy(&p, ",", SIZEOF_20KBUF-(p-tmp_20k_buf));

						sstrncpy(&p,long2string(reply->data.uid.msgs[i]), SIZEOF_20KBUF-(p-tmp_20k_buf));
					    }

					    tmp_20k_buf[SIZEOF_20KBUF-1] = '\0';

					    snprintf(uidbuf, sizeof(uidbuf), "(%d %s)(%d %lu %s)%s",
						    strlen(reply->prefix), reply->prefix,
						    i, reply->data.uid.validity,
						    tmp_20k_buf, reply->mailbox);

					    peAppListStrStr(interp, objHdr, "x-reply-uid", uidbuf);
					}

					fs_give((void **) &reply->mailbox);
					fs_give((void **) &reply->prefix);
					fs_give((void **) &reply->data.uid.msgs);
					fs_give((void **) &reply);
				    }

				    objBod = Tcl_NewListObj(0, NULL);
				    peSoStrToList(interp, objBod, so);

				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objHdr);
				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objBod);

				    objAttach = peMsgAttachCollector(interp, b);

				    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), objAttach);

				    if(objOpts){
					Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),objOpts);
				    }

				    /* clean up */
				    if(fcc)
				      fs_give((void **) &fcc);

				    if(lcc)
				      fs_give((void **) &lcc);

				    mail_free_envelope(&env);
				    pine_free_body(&b);
				    free_action(&role);

				    return(TCL_OK);
				}

				so_give(&so);
			    }
			    else
			      err = "No internal storage";

			    /* redraft_work cleaned up the "stream" */
			}
			else
			  err = "No Postponed stream";
		    }
		    else
		      err = "Malformed extract request";
		}
		else if(!strcmp(s1, "append")){
		    return(peMsgCollector(interp, objc - 2, (Tcl_Obj **) &objv[2], peDoPostpone));
		}
	    }
	}
    }

    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


/*
 * peDoPostpone - handle postponing after message collection
 */
int
peDoPostpone(METAENV *metaenv, BODY *body, char *fcc, CONTEXT_S **fcc_cntxtp, char *errp)
{
    PINEFIELD *pf;
    
    /*
     * resolve fcc and store it in fcc custom header field data
     */
    if(fcc && *fcc && fcc_cntxtp && *fcc_cntxtp)
      for(pf = metaenv->local; pf && pf->name; pf = pf->next)
	if(!strucmp("fcc", pf->name)){
	    char *name, *rs, path_in_context[MAILTMPLEN];

	    if(pf->textbuf)			/* free old value */
	      fs_give((void **) &pf->textbuf);

	    /* replace nickname with full name */
	    if(!(name = folder_is_nick(fcc, FOLDERS(*fcc_cntxtp), FN_NONE)))
	      name = fcc;

	    if(context_isambig(name) && !(((*fcc_cntxtp)->use) & CNTXT_SAVEDFLT)){
		context_apply(path_in_context, *fcc_cntxtp, name, sizeof(path_in_context));
		rs = IS_REMOTE(path_in_context) ? path_in_context : NULL;
	    }
	    else
	      rs = cpystr(name);

	    if(rs){
		pf->textbuf = cpystr(rs);
		pf->text = &pf->textbuf;
	    }

	    break;
	}

    return((write_postponed(metaenv, body) < 0) ? TCL_ERROR : TCL_OK);
}


/*
 * peMsgCollector - Collect message parts and call specified handler
 */
int
peMsgCollector(Tcl_Interp *interp,
	       int objc,
	       Tcl_Obj **objv,
	       int (*postfunc)(METAENV *, BODY *, char *, CONTEXT_S **, char *))
{
    Tcl_Obj    **objMsg, **objField, **objBody;
    int		 i, j, vl, nMsg, nField, nBody, rv = TCL_OK;
    char	*field, *value, *err = NULL;
    MSG_COL_S	 md;
    BODY	*body = NULL, *tbp = NULL;
    PINEFIELD	*pf;
    STRLIST_S	*tp, *lp;
    static char	*fakedomain = "@";

    memset(&md, 0, sizeof(MSG_COL_S));
    md.postop_fcc_no_attach = -1;
    md.postfunc = postfunc;

    if(objc != 1){
	Tcl_SetResult(interp, "pePostpone: malformed message data", TCL_STATIC);
	return(TCL_ERROR);
    }
    else if(!ps_global){
	Tcl_SetResult(interp, "pePostpone: no open folder", TCL_STATIC);
	return(TCL_ERROR);
    }

    md.outgoing = mail_newenvelope();

    md.metaenv = pine_new_env(md.outgoing, NULL, NULL, md.custom = peCustomHdrs());

    Tcl_ListObjGetElements(interp, objv[0], &nMsg, &objMsg);
    for(i = 0; i < nMsg; i++){
	if(Tcl_ListObjGetElements(interp, objMsg[i], &nField, &objField) != TCL_OK){
	    err = "";		/* interp's result object has error message */
	    return(peMsgCollected(interp, &md, err));
	}

	if(nField && (field = Tcl_GetStringFromObj(objField[0], NULL))){
	    if(!strcmp(field, "body")){
		if(md.msgtext){
		    err = "pePostpone: Too many bodies";
		    return(peMsgCollected(interp, &md, err));
		}
		else if(md.msgtext = so_get(CharStar, NULL, EDIT_ACCESS)){
		    /* mark storage object as user edited */
		    (void) so_attr(md.msgtext, "edited", "1");

		    Tcl_ListObjGetElements(interp, objField[1], &nBody, &objBody);
		    for(j = 0; j < nBody; j++){
#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
			value = Tcl_GetByteArrayFromObj(objBody[j], &vl);
#else
			if(value = Tcl_GetStringFromObj(objBody[j], NULL))
			  vl = strlen(value);
#endif
			if(value){
			    so_nputs(md.msgtext, value, vl);
			    so_puts(md.msgtext, "\n");
			}
			else{
			    err = "pePostpone: Value read Failure";
			    return(peMsgCollected(interp, &md, err));
			}
		    }
		}
		else {
		    err = "pePostpone: Can't acquire body storage";
		    return(peMsgCollected(interp, &md, err));
		}
	    }
	    else if(!strucmp(field, "attach")){
		char      *id;
		COMPATT_S *a;

		if(nField == 2
		   && (id = Tcl_GetStringFromObj(objField[1], NULL))
		   && (a = peGetAttachID(id))){
		    tp = new_strlist();
		    tp->name = cpystr(id);
		    if(lp = md.attach){
			do
			  if(!lp->next){
			      lp->next = tp;
			      break;
			  }
			while(lp = lp->next);
		    }
		    else
		      md.attach = tp;
		}
		else{
		    strcpy(err = tmp_20k_buf, "pePostpone: unknown attachment ID");
		    return(peMsgCollected(interp, &md, err));
		}
	    }
	    else if(!strucmp(field, "fcc")){
		Tcl_Obj **objFcc;
		int	  nFcc;

		if(Tcl_ListObjGetElements(interp, objField[1], &nFcc, &objFcc) == TCL_OK
		   && nFcc == 2
		   && Tcl_GetIntFromObj(interp, objFcc[0], &md.fcc_colid) == TCL_OK
		   && (value = Tcl_GetStringFromObj(objFcc[1], NULL))){
		    if(md.fcc)
		      fs_give((void **) &md.fcc);

		    md.fcc = cpystr(value);
		}
		else {
		    strcpy(err = tmp_20k_buf, "Unrecognized Fcc: specification");
		    return(peMsgCollected(interp, &md, err));
		}
	    }
	    else if(!strucmp(field, "postoption")){
		Tcl_Obj **objPO;
		int	  nPO, ival;

		value = NULL;
		if(Tcl_ListObjGetElements(interp, objField[1], &nPO, &objPO) == TCL_OK
		   && nPO == 2
		   && (value = Tcl_GetStringFromObj(objPO[0], NULL))){
		    if(!strucmp(value,"fcc-without-attachments")){
			if(Tcl_GetIntFromObj(interp, objPO[1], &ival) == TCL_OK){
			    md.postop_fcc_no_attach = (ival != 0);
			}
			else{
			    sprintf(err = tmp_20k_buf, "Malformed Post Option: fcc-without-attachments", value);
			    return(peMsgCollected(interp, &md, err));
			}
		    }
		    else if(!strucmp(value, "charset")){
			if(value = Tcl_GetStringFromObj(objPO[1], NULL)){
			    char *p;

			    for(p = value; ; p++){		/* sanity check */
				if(!*p){
				    md.charset = cpystr(value);
				    break;
				}

				if(isspace((unsigned char ) *p)
				   || !isprint((unsigned char) *p))
				  break;

				if(p - value > 255)
				  break;
			    }
			}
			else{
			    err = "Post option read failure";
			    return(peMsgCollected(interp, &md, err));
			}
		    }
		    else if(!strucmp(value, "flowed")){
			if(F_OFF(F_QUELL_FLOWED_TEXT,ps_global)){
			    if(value = Tcl_GetStringFromObj(objPO[1], NULL)){
				if(!strucmp(value, "yes"))
				  md.flowed = 1;
			    }
			    else{
				err = "Post option read failure";
				return(peMsgCollected(interp, &md, err));
			    }
			}
		    }
		    else{
			sprintf(err = tmp_20k_buf, "Unknown Post Option: %s", value);
			return(peMsgCollected(interp, &md, err));
		    }
		}
		else{
		    sprintf(err = tmp_20k_buf, "Malformed Post Option");
		    return(peMsgCollected(interp, &md, err));
		}
	    }
	    else {
		if(nField != 2){
		    sprintf(err = tmp_20k_buf, "pePostpone: malformed header (%s)", field);
		    return(peMsgCollected(interp, &md, err));
		}

#if	(TCL_MAJOR_VERSION >= 8 && TCL_MINOR_VERSION > 0)
		value = Tcl_GetByteArrayFromObj(objField[1], &vl);
#else
		if(value = Tcl_GetStringFromObj(objField[1], NULL))
		  vl = strlen(value);
#endif
		if(value){
		    ADDRESS **addrp = NULL;
		    char    **valp = NULL, *valcpy;

		    if(!strucmp(field, "from")){
			addrp = &md.outgoing->from;
		    }
		    else if(!strucmp(field, "reply-to")){
			addrp = &md.outgoing->reply_to;
		    }
		    else if(!strucmp(field, "to")){
			addrp = &md.outgoing->to;
		    }
		    else if(!strucmp(field, "cc")){
			addrp = &md.outgoing->cc;
		    }
		    else if(!strucmp(field, "bcc")){
			addrp = &md.outgoing->bcc;
		    }
		    else if(!strucmp(field, "subject")){
			valp = &md.outgoing->subject;
		    }
		    else if(!strucmp(field, "in-reply-to")){
			valp = &md.outgoing->in_reply_to;
		    }
		    else if(!strucmp(field, "newsgroups")){
			valp = &md.outgoing->newsgroups;
		    }
		    else if(!strucmp(field, "followup-to")){
			valp = &md.outgoing->followup_to;
		    }
		    else if(!strucmp(field, "references")){
			valp = &md.outgoing->references;
		    }
		    else if(!strucmp(field, "x-reply-uid")){
			for(pf = md.metaenv->local; pf && pf->name; pf = pf->next)
			  if(!strucmp(pf->name, "x-reply-uid")){
			      valp = pf->text = &pf->textbuf;
			      break;
			  }
		    }
		    else if(!strucmp(field, "x-auth-received")){
			for(pf = md.metaenv->local; pf && pf->name; pf = pf->next)
			  if(!strucmp(pf->name, "x-auth-received")){
			      valp = pf->text = &pf->textbuf;
			      break;
			  }
		    }
		    else{
			for(pf = md.metaenv->custom; pf && pf->name; pf = pf->next)
			  if(!strucmp(field, pf->name)){
			      if(pf->type == Address)
				addrp = pf->addr;
			      else if(vl)
				valp = &pf->textbuf;
			      else if(pf->textbuf)
				fs_give((void **) &pf->textbuf);

			      break;
			  }

			if(!pf)
			  dprint((2, "\nPOST: unrecognized field - %s\n", field));
		    }

		    if(valp){
			if(*valp)
			  fs_give((void **) valp);

			sprintf(*valp = fs_get((vl + 1) * sizeof(char)), "%.*s", vl, value);
		    }

		    if(addrp){
			sprintf(valcpy = fs_get((vl + 1) * sizeof(char)), "%.*s", vl, value);

			for(; *addrp; addrp = &(*addrp)->next)
			  ;

			rfc822_parse_adrlist(addrp, valcpy,
					     (F_ON(F_COMPOSE_REJECTS_UNQUAL, ps_global))
					       ? fakedomain : ps_global->maildomain);
			fs_give((void **) &valcpy);
		    }
		}
		else{
		    err = "pePostpone: Value read Failure";
		    return(peMsgCollected(interp, &md, err));
		}
	    }
	}
    }

    return(peMsgCollected(interp, &md, err));
}


/*
 * peMsgCollected - Dispatch collected message data and cleanup
 */
int
peMsgCollected(Tcl_Interp  *interp,
	       MSG_COL_S   *md,
	       char	   *err)
{
    int	       rv = TCL_OK;
    BODY      *body = NULL, *tbp = NULL;
    char       errbuf[WP_MAX_POST_ERROR + 1], *charset;
    STRLIST_S *lp;

    if(err){
	if(md->msgtext)
	  so_give(&md->msgtext);

	rv = TCL_ERROR;
    }
    else if(F_ON(F_COMPOSE_REJECTS_UNQUAL, ps_global) && check_addresses(md->metaenv) == CA_BAD){
	sprintf(err = tmp_20k_buf, "Address must be fully qualified.");
	rv = TCL_ERROR;
    }
    else{
	/* put stuff to post here */

	if(!md->outgoing->from)
	  md->outgoing->from = generate_from();

	rfc822_date(tmp_20k_buf);
	md->outgoing->date = cpystr(tmp_20k_buf);
	md->outgoing->return_path = rfc822_cpy_adr(md->outgoing->from);
	md->outgoing->message_id = generate_message_id();

	body = mail_newbody();

	/* wire any attachments to body */
	if(md->attach){
	    PART	  **np;
	    PARAMETER **pp;
	    COMPATT_S  *a;

	    /* setup slot for message text */
	    body->type	  = TYPEMULTIPART;
	    body->nested.part = mail_newbody_part();
	    tbp		  = &body->nested.part->body;

	    /* link in attachments */
	    for(lp = md->attach, np = &body->nested.part->next; lp; lp = lp->next, np = &(*np)->next){
		if(!(a = peGetAttachID(lp->name))){
		    err = "Unknown Attachment ID";
		    rv = TCL_ERROR;
		    break;
		}

		*np = mail_newbody_part();

		if(a->file){
		    (*np)->body.id = generate_message_id();
		    (*np)->body.description = cpystr(a->l.f.description);

		    /* set name parameter */
		    for(pp = &(*np)->body.parameter; *pp; )
		      if(!struncmp((*pp)->attribute, "name", 4)
			 && (!*((*pp)->attribute + 4)
			     || *((*pp)->attribute + 4) == '*')){
			  PARAMETER *free_me = *pp;
			  *pp = (*pp)->next;
			  free_me->next = NULL;
			  mail_free_body_parameter(&free_me);
		      }
		      else
			pp = &(*pp)->next;

		    *pp = mail_newbody_parameter();
		    (*pp)->attribute = cpystr("name");
		    (*pp)->value = cpystr(a->l.f.remote);

		    /* Then set the Content-Disposition ala RFC1806 */
		    if(!(*np)->body.disposition.type){
			(*np)->body.disposition.type = cpystr("attachment");
			for(pp = &(*np)->body.disposition.parameter; *pp; )
			  if(!struncmp((*pp)->attribute, "filename", 4)
			     && (!*((*pp)->attribute + 4)
				 || *((*pp)->attribute + 4) == '*')){
			      PARAMETER *free_me = *pp;
			      *pp = (*pp)->next;
			      free_me->next = NULL;
			      mail_free_body_parameter(&free_me);
			  }
			  else
			    pp = &(*pp)->next;

			*pp = mail_newbody_parameter();
			(*pp)->attribute = cpystr("filename");
			(*pp)->value = cpystr(a->l.f.remote);
		    }

		    if((*np)->body.contents.text.data = (void *) so_get(FileStar, a->l.f.local, READ_ACCESS)){
			(*np)->body.type     = mt_translate_type(a->l.f.type);
			(*np)->body.subtype  = cpystr(a->l.f.subtype);
			(*np)->body.encoding = ENCBINARY;
			(*np)->body.size.bytes = name_file_size(a->l.f.local);

			if((*np)->body.type == TYPEOTHER
			   && !set_mime_type_by_extension(&(*np)->body, a->l.f.local))
			  set_mime_type_by_grope(&(*np)->body, NULL);

			so_release((STORE_S *)(*np)->body.contents.text.data);
		    }
		    else{
			/* unravel here */
			err = "Can't open uploaded attachment";
			rv = TCL_ERROR;
			break;
		    }
		}
		else if(a->body){
		    BODY *newbody = copy_body(NULL, a->l.b.body);
		    (*np)->body = *newbody;
		    fs_give((void **) &newbody);
		    peBodyMoveContents(a->l.b.body, &(*np)->body);
		}
		else{
		    err = "BOTCH: Unknown attacmnent type";
		    rv = TCL_ERROR;
		    break;
		}
	    }
	}
	else
	  tbp = body;

	/* assign MIME parameters to text body part */
	tbp->type = TYPETEXT;
	tbp->contents.text.data = (void *) md->msgtext;

	/*
	 * set up text body part's charset
	 * if it came from compose form as something other
	 * than UTF-8, toss hands up into air, shrug shoulders.
	 */
	charset = (!md->charset || !strucmp(md->charset, "utf-8")) ? "UTF-8" : "X-UNKNOWN";
	peMsgSetParm(&tbp->parameter, "charset", charset);

	/* possibly up/downgrade charset, figure out c-t-e? */
	tbp->encoding = ENCOTHER;
	set_mime_type_by_grope(tbp, charset);

	/* set any text flowed param */
	if(md->flowed)
	  peMsgSetParm(&tbp->parameter, "format", "flowed");

	if(rv == TCL_OK){
	    int	       recipients;
	    CONTEXT_S *fcc_cntxt = ps_global->context_list;

	    while(md->fcc_colid--)
	      if(fcc_cntxt->next)
		fcc_cntxt = fcc_cntxt->next;

	    if(md->postop_fcc_no_attach >= 0){
		int oldval = F_ON(F_NO_FCC_ATTACH, ps_global);
		F_SET(F_NO_FCC_ATTACH, ps_global, md->postop_fcc_no_attach);
		md->postop_fcc_no_attach = oldval;
	    }

	    pine_encode_body(body);

	    rv = (*md->postfunc)(md->metaenv, body, md->fcc, &fcc_cntxt, errbuf);

	    if(md->postop_fcc_no_attach >= 0){
		F_SET(F_NO_FCC_ATTACH, ps_global, md->postop_fcc_no_attach);
	    }

	    if(rv == TCL_OK)
	      peFreeAttach(&peCompAttach);
	    else
	      err = errbuf;
	}

	pine_free_body(&body);
    }
	
    if(md->charset)
      fs_give((void **) &md->charset);

    free_strlist(&md->attach);

    pine_free_env(&md->metaenv);

    if(md->custom)
      free_customs(md->custom);

    mail_free_envelope(&md->outgoing);

    if(err && *err)
      Tcl_SetResult(interp, err, TCL_VOLATILE);

    return(rv);
}


void
peMsgSetParm(PARAMETER **pp, char *pa, char *pv)
{
    for(; *pp; pp = &(*pp)->next)
      if(!strucmp(pa, (*pp)->attribute)){
	if((*pp)->value)
	  fs_give((void **) &(*pp)->value);

	break;
      }

    if(!*pp){
	*pp = mail_newbody_parameter();
	(*pp)->attribute = cpystr(pa);
    }

    (*pp)->value = cpystr(pv);
}


Tcl_Obj *
peMsgAttachCollector(Tcl_Interp *interp, BODY *b)
{
    char      *id, *name = NULL;
    PART      *part;
    PARAMETER *pp;
    COMPATT_S *a;
    Tcl_Obj   *aListObj = NULL, *aObj = NULL;

    peFreeAttach(&peCompAttach);

    aListObj = Tcl_NewListObj(0, NULL);

    if(b->type == TYPEMULTIPART){
	/*
	 * Walk first level, clipping branches and adding them
	 * to the attachment list...
	 */
	for(part = b->nested.part->next; part; part = part->next) {
	    id	 = peBodyAttachID(&part->body);
	    aObj = Tcl_NewListObj(0, NULL);

	    Tcl_ListObjAppendElement(interp, aObj, Tcl_NewStringObj(id, -1));

	    /* name */
	    if((name = rfc2231_get_param(part->body.disposition.parameter, "filename", NULL, NULL))
	       || (name = rfc2231_get_param(part->body.parameter, "name", NULL, NULL))){
		Tcl_ListObjAppendElement(interp, aObj, Tcl_NewStringObj(name, -1));
		fs_give((void **) &name);
	    }
	    else
	      Tcl_ListObjAppendElement(interp, aObj, Tcl_NewStringObj("Unknown", -1));

	    /* size */
	    Tcl_ListObjAppendElement(interp, aObj,
				     Tcl_NewLongObj((part->body.encoding == ENCBASE64)
						    ? ((part->body.size.bytes * 3)/4)
						    : part->body.size.bytes));

	    /* type */
	    snprintf(tmp_20k_buf, SIZEOF_20KBUF, "%s/%s",
		    body_type_names(part->body.type),
		    part->body.subtype ? part->body.subtype : "Unknown");

	    Tcl_ListObjAppendElement(interp, aObj, Tcl_NewStringObj(tmp_20k_buf, -1));
	    Tcl_ListObjAppendElement(interp, aListObj, aObj);
	}
    }

    return (aListObj);
}


int
peFccAppend(Tcl_Interp *interp, Tcl_Obj *obj, char *fcc, int colid)
{
    Tcl_Obj *objfcc = NULL;

    if(colid < 0)
      colid = (ps_global->context_list && (ps_global->context_list->use & CNTXT_INCMNG)) ? 1 : 0;

    return((objfcc = Tcl_NewListObj(0, NULL))
	   && Tcl_ListObjAppendElement(interp, objfcc, Tcl_NewStringObj("fcc", -1)) == TCL_OK
	   && peAppListIntStr(interp, objfcc, colid, fcc) == TCL_OK
	   && Tcl_ListObjAppendElement(interp, obj, objfcc) == TCL_OK);
}


/* * * * * * * * * * * * *  Start of Address Management Routines * * * * * * * * * * * * /


/*
 * PEAddressCmd - export various bits of address book/directory access
 */
int
PEAddressCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *op;

    dprint((2, "PEAddressCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
	return(TCL_ERROR);
    }
    else if(!ps_global){
	Tcl_SetResult(interp, "PEAddress: no open folder", TCL_STATIC);
	return(TCL_ERROR);
    }
    else if(op = Tcl_GetStringFromObj(objv[1], NULL)){
	if(objc == 2){
	    if(!strcmp(op, "safecheck")){
		if(peInitAddrbooks(interp, 1) != TCL_OK)
		  return(TCL_ERROR);
		return(TCL_OK);
	    }
	    else if(!strcmp(op, "books")){
		int   i;
		char *s;

		/*
		 * return the list of configured address books
		 */

		if(peInitAddrbooks(interp, 0) != TCL_OK)
		  return(TCL_ERROR);

		for(i = 0; i < as.n_addrbk; i++){
		    Tcl_Obj *objmv[4];

		    objmv[0] = Tcl_NewIntObj(i);
		    if(as.adrbks[i].abnick){
			objmv[1] = Tcl_NewStringObj(as.adrbks[i].abnick, -1);
		    }
		    else {
			char buf[256];

			snprintf(buf, sizeof(buf), "Address book number %d", i + 1);
			objmv[1] = Tcl_NewStringObj(buf, -1);
		    }

		    objmv[2] = Tcl_NewStringObj(as.adrbks[i].filename ? as.adrbks[i].filename : "", -1);

		    objmv[3] = Tcl_NewIntObj(as.adrbks[i].access == ReadWrite);

		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					     Tcl_NewListObj(4, objmv));
		}

		return(TCL_OK);
	    }
	}
	else if(objc == 3){
	    if(!strcmp(op, "parselist")){
		char	    *addrstr;
		ADDRESS     *addrlist = NULL, *atmp, *anextp;
		static char *fakedomain = "@";

		if(addrstr = Tcl_GetStringFromObj(objv[2], NULL))
		  addrstr = cpystr(addrstr); /* can't munge tcl copy */

		ps_global->c_client_error[0] = '\0';
		rfc822_parse_adrlist(&addrlist, addrstr,
				     (F_ON(F_COMPOSE_REJECTS_UNQUAL, ps_global))
				       ? fakedomain : ps_global->maildomain);

		fs_give((void **) &addrstr);
		if(ps_global->c_client_error[0]){
		    Tcl_SetResult(interp, ps_global->c_client_error, TCL_STATIC);
		    return(TCL_ERROR);
		}

		for(atmp = addrlist; atmp; ){
		    RFC822BUFFER rbuf;

		    anextp = atmp->next;
		    atmp->next = NULL;
		    tmp_20k_buf[0] = '\0';
		    rbuf.f   = dummy_soutr;
		    rbuf.s   = NULL;
		    rbuf.beg = tmp_20k_buf;
		    rbuf.cur = tmp_20k_buf;
		    rbuf.end = tmp_20k_buf+SIZEOF_20KBUF-1;
		    rfc822_output_address_list(&rbuf, atmp, 0L, NULL);
		    *rbuf.cur = '\0';
		    Tcl_ListObjAppendElement(interp,
					     Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(tmp_20k_buf, -1));
		    atmp = anextp;
		}

		mail_free_address(&addrlist);
		return(TCL_OK);
	    }
	    else if(!strcmp(op, "xlookup")){
		char	    *addrstr;
		ADDRESS     *addrlist = NULL, *atmp, *anextp;
		static char *fakedomain = "@";

		if(addrstr = Tcl_GetStringFromObj(objv[2], NULL))
		  addrstr = cpystr(addrstr); /* can't munge tcl copy */

		ps_global->c_client_error[0] = '\0';
		rfc822_parse_adrlist(&addrlist, addrstr,
				     (F_ON(F_COMPOSE_REJECTS_UNQUAL, ps_global))
				       ? fakedomain : ps_global->maildomain);

		fs_give((void **) &addrstr);
		if(ps_global->c_client_error[0]){
		    Tcl_SetResult(interp, ps_global->c_client_error, TCL_STATIC);
		    return(TCL_ERROR);
		}

		for(atmp = addrlist; atmp; ){
		    anextp = atmp->next;
		    atmp->next = NULL;
		    tmp_20k_buf[0] = '\0';
		    if(atmp->host){
			if(atmp->host[0] == '@'){
			    /* leading ampersand means "missing-hostname" */
			}
			else{
			    RFC822BUFFER rbuf;

			    rbuf.f   = dummy_soutr;
			    rbuf.s   = NULL;
			    rbuf.beg = tmp_20k_buf;
			    rbuf.cur = tmp_20k_buf;
			    rbuf.end = tmp_20k_buf+SIZEOF_20KBUF-1;
			    rfc822_output_address_list(&rbuf, atmp, 0L, NULL);
			    *rbuf.cur = '\0';
			    Tcl_ListObjAppendElement(interp,
						     Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(tmp_20k_buf, -1));
			}
		    } /* else group syntax, move on */

		    atmp = anextp;
		}

		mail_free_address(&addrlist);
		return(TCL_OK);
	    }
	    else if(!strcmp(op, "format")){
		int	     i, n, booknum;
		char	     buf[256], *s;
		AdrBk_Entry *ae;
		Tcl_Obj	    *objev[NFIELDS];

		if(peInitAddrbooks(interp, 0) != TCL_OK)
		  return(TCL_ERROR);

		/*
		 * 
		 */
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK)
		  for(i = 0; i < as.n_addrbk; i++)
		    if(i == booknum){
			addrbook_new_disp_form(&as.adrbks[booknum], ps_global->VAR_ABOOK_FORMATS, booknum, NULL);

			for(i = 0; i < NFIELDS && as.adrbks[booknum].disp_form[i].type != Notused; i++){
			    switch(as.adrbks[booknum].disp_form[i].type){
			      case Nickname :
				s = "nick";
				break;
			      case Fullname :
				s = "full";
				break;
			      case Addr :
				s = "addr";
				break;
			      case Filecopy :
				s = "fcc";
				break;
			      case Comment :
				s = "comment";
				break;
			      default :
				s = NULL;
				break;
			    }

			    if(s){
				Tcl_Obj *objmv[2];

				objmv[0] = Tcl_NewStringObj(s, -1);
				objmv[1] = Tcl_NewIntObj((100 * as.adrbks[booknum].disp_form[i].width) / ps_global->ttyo->screen_cols);
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							 Tcl_NewListObj(2, objmv));
			    }
			}


			return(TCL_OK);
		    }

		snprintf(buf, sizeof(buf), "PEAddress list: unknown address book number \"%d\"", booknum);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	    else if(!strcmp(op, "list")){
		int	     i, j, k, n, booknum;
		char	     buf[256], *s;
		AdrBk_Entry *ae;
		Tcl_Obj	    *objev[NFIELDS + 1], *objhv[2];

		if(peInitAddrbooks(interp, 0) != TCL_OK)
		  return(TCL_ERROR);

		/*
		 * 
		 */
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK)
		  for(i = 0; i < as.n_addrbk; i++)
		    if(i == booknum){
			addrbook_new_disp_form(&as.adrbks[booknum], ps_global->VAR_ABOOK_FORMATS, booknum, NULL);

			for(i = 0;
			    ae = adrbk_get_ae(as.adrbks[booknum].address_book, i);
			    i++){

			    /* first member is type: Single, List or Lookup */
			    switch(ae->tag){
			      case Single :
				s = "single";
				break;
			      case List :
				s = "list";
				break;
			      default :	/* not set!?! */
				continue;
			    }

			    if(!ae->nickname)
			      continue;

			    objhv[0] = Tcl_NewStringObj(ae->nickname, -1);
			    objhv[1] = Tcl_NewStringObj(s, -1);
			    objev[n = 0] = Tcl_NewListObj(2, objhv);

			    /*
			     * set fields based on VAR_ABOOK_FORMATS
			     */

			    for(j = 0; j < NFIELDS && as.adrbks[booknum].disp_form[j].type != Notused; j++){
				switch(as.adrbks[booknum].disp_form[j].type){
				  case Nickname :
				    objev[++n] = Tcl_NewStringObj(ae->nickname, -1);
				    break;
				  case Fullname :
				    objev[++n] = Tcl_NewStringObj(ae->fullname, -1);
				    break;
				  case Addr :
				    if(ae->tag == Single){
					objev[++n] = Tcl_NewStringObj(ae->addr.addr, -1);
				    }
				    else{
					Tcl_Obj **objav;

					for(k = 0; ae->addr.list[k]; k++)
					  ;

					objav = (Tcl_Obj **) fs_get(k * sizeof(Tcl_Obj *));
					for(k = 0; ae->addr.list[k]; k++)
					  objav[k] = Tcl_NewStringObj(ae->addr.list[k], -1);

					objev[++n] = Tcl_NewListObj(k, objav);
					fs_give((void **) &objav);
				    }
				    break;
				  case Filecopy :
				    objev[++n] = Tcl_NewStringObj(ae->fcc ? ae->fcc : "", -1);
				    break;
				  case Comment :
				    objev[++n] = Tcl_NewStringObj(ae->extra ? ae->extra : "", -1);
				    break;
				  default :
				    s = NULL;
				    break;
				}
			    }

			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     Tcl_NewListObj(n + 1, objev));
			}

			return(TCL_OK);
		    }

		snprintf(buf, sizeof(buf), "PEAddress list: unknown address book number \"%d\"", booknum);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	}
	else if(objc == 4){
	    if(!strcmp(op, "verify")){
	      /*
	       * The object here is to check the following list of field values
	       * to see that they are valid address list, expanding if necessary.
	       * The first argument is the list of field values, with "to" being
	       * first.  The second arg is the current fcc value.
	       *
	       * The return value is of the following form:
	       *
	       * { {{errstr {{oldstr newstr {ldap-opts ...}} ...}} ...} newfcc}
	       */
	        Tcl_Obj **objVal;
		char	    *addrstr, *newaddr = NULL, *error = NULL, 
		            *tstr1, *tstr2, *fcc, *newfcc = NULL;
		BuildTo	     toaddr;
		int	     rv, badadrs, i , numlistvals,
		             numldapqueries = 0;
		static char *fakedomain = "@";
		Tcl_Obj     *resObj = NULL, *secObj, *strObj, *adrObj, *res2Obj;
#ifdef ENABLE_LDAP
		WPLDAPRES_S **tsl;

		wpldap_global->query_no++;
		if(wpldap_global->ldap_search_list){
		    wpldap_global->ldap_search_list = 
		      free_wpldapres(wpldap_global->ldap_search_list);
		}

		tsl = &(wpldap_global->ldap_search_list);
#endif /* ENABLE_LDAP */

		if(Tcl_ListObjGetElements(interp, objv[2], &numlistvals, 
					  &objVal) == TCL_OK){
		    if((fcc = Tcl_GetStringFromObj(objv[3], NULL)) == NULL)
		      return TCL_ERROR;
		    res2Obj = Tcl_NewListObj(0, NULL);
		    for(i = 0; i < numlistvals; i++){
			size_t l;

		        if((addrstr = Tcl_GetStringFromObj(objVal[i], NULL)) == NULL)
			  return TCL_ERROR;

			addrstr = cpystr(addrstr); /* can't munge tcl copy */
			toaddr.type    = Str;
			toaddr.arg.str = cpystr(addrstr);
			l = strlen(addrstr);
			badadrs = 0;
			resObj = Tcl_NewListObj(0, NULL);
			secObj = Tcl_NewListObj(0, NULL);
			for(tstr1 = addrstr; tstr1; tstr1 = tstr2){
			    tstr2 = strqchr(tstr1, ',', 0, -1);
			    if(tstr2)
			      *tstr2 = '\0';

			    strncpy(toaddr.arg.str, tstr1, l);
			    toaddr.arg.str[l] = '\0';

			    removing_leading_and_trailing_white_space(toaddr.arg.str);
			    if(*toaddr.arg.str){
				if(i == 0 && tstr1 == addrstr)
				  newfcc = cpystr(fcc);

			        rv = our_build_address(toaddr, &newaddr, &error, &newfcc, NULL);

				if(rv == 0){
				    strObj = Tcl_NewListObj(0, NULL);
				    Tcl_ListObjAppendElement(interp, strObj, 
							     Tcl_NewStringObj(toaddr.arg.str, -1));
				    Tcl_ListObjAppendElement(interp, strObj, 
							     Tcl_NewStringObj(newaddr,-1));
				    /* append whether or not ldap stuff 
				     * was returned 
				     */
				    adrObj = Tcl_NewListObj(0,NULL);
#ifdef ENABLE_LDAP
				    if(*tsl) {
				        LDAP_SERV_RES_S *tres, *trl;
					LDAPMessage *e;
					ADDRESS *newadr;
					char    *ret_to;
					
					tres = (LDAP_SERV_RES_S *)fs_get(sizeof(LDAP_SERV_RES_S));
				        for(trl = (*tsl)->reslist; trl; 
					    trl = trl->next){
					    for(e = ldap_first_entry(trl->ld, 
								     trl->res);
						e != NULL; 
						e = ldap_next_entry(trl->ld, e)){
					        tres->ld        = trl->ld;
						tres->res       = e;
						tres->info_used = trl->info_used;
						tres->serv      = trl->serv;
						tres->next      = NULL;
						if(newadr = address_from_ldap(tres)){
						    if(newadr->mailbox && newadr->host){
							RFC822BUFFER rbuf;
							size_t len;

							len = est_size(newadr);
						        ret_to = (char *)fs_get(len * sizeof(char));
							ret_to[0] = '\0';
							rbuf.f   = dummy_soutr;
							rbuf.s   = NULL;
							rbuf.beg = ret_to;
							rbuf.cur = ret_to;
							rbuf.end = ret_to+len-1;
							rfc822_output_address_list(&rbuf, newadr, 0L, NULL);
							*rbuf.cur = '\0';
							Tcl_ListObjAppendElement(interp, 
								    adrObj, Tcl_NewStringObj(ret_to, -1));
							fs_give((void **)&ret_to);
						    }
						    mail_free_address(&newadr);
						}
					    }
					}
					fs_give((void **)&tres);
					numldapqueries++;
					tsl = &((*tsl)->next);
				    }
#endif /* ENABLE_LDAP */
				    Tcl_ListObjAppendElement(interp, strObj, adrObj);
				    Tcl_ListObjAppendElement(interp, secObj, strObj);
				}
				else {
				  badadrs = 1;
				  break;
				}
			    }
			    if(tstr2){
			      *tstr2 = ',';
			      tstr2++;
			    }
			}
			resObj = Tcl_NewListObj(0, NULL);
			Tcl_ListObjAppendElement(interp, resObj, 
						 Tcl_NewStringObj(badadrs
						 ? (error ? error : "Unknown")
						 : "", -1));
			Tcl_ListObjAppendElement(interp, resObj, secObj);
			Tcl_ListObjAppendElement(interp, res2Obj, resObj);
			fs_give((void **) &addrstr);
			fs_give((void **) &toaddr.arg.str);
		    }
		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), res2Obj);
		    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
					     Tcl_NewStringObj(newfcc ? newfcc 
						     : (fcc ? fcc : ""), -1));
		    if(newfcc) fs_give((void **)&newfcc);
		    return(TCL_OK);
		}
		return(TCL_ERROR);
	    }
	    else if(!strcmp(op, "expand")){
		BuildTo	     toaddr;
		char	    *addrstr, *newaddr = NULL, *error = NULL, *fcc, *newfcc = NULL;
		int	     mangle, rv;
		static char *fakedomain = "@";
		Tcl_Obj     *resObj = NULL;

		/*
		 * Return value will be of the form:
		 *  {"addrstr",
		 *   ldap-query-number,
		 *   "fcc"
		 *  }
		 *
		 *  ldap-query-number will be nonzero if
		 *  there is something interesting to display as a result
		 *  of an ldap query.
		 */

		/*
		 * Given what looks like an rfc822 address line, parse the
		 * contents and expand any tokens that look like they ought
		 * to be.
		 */

		if(addrstr = Tcl_GetStringFromObj(objv[2], NULL)){
		    toaddr.type = Str;
		    toaddr.arg.str = cpystr(addrstr); /* can't munge tcl copy */
		    fcc = Tcl_GetStringFromObj(objv[3], NULL);
#ifdef ENABLE_LDAP
		    wpldap_global->query_no++;
		    if(wpldap_global->ldap_search_list){
		      wpldap_global->ldap_search_list = 
			free_wpldapres(wpldap_global->ldap_search_list);
		    }
#endif /* ENABLE_LDAP */
		    newfcc = cpystr(fcc);
		    rv = our_build_address(toaddr, &newaddr, &error, &newfcc, NULL);
		    fs_give((void **) &toaddr.arg.str);
		    if(rv == 0){
#ifdef ENABLE_LDAP
		      /*
		       * c-client quotes results with spaces in them, so we'll go
		       * through and unquote them.
		       */
			if(wpldap_global->ldap_search_list){
			    WPLDAPRES_S *tres;
			    char        *tstr1, *tstr2;
			    char        *qstr1, *newnewaddr;
			    int          qstr1len;

			    for(tres = wpldap_global->ldap_search_list;
				tres; tres = tres->next){
			        if(strqchr(tres->str, ' ', 0, -1)){
				    qstr1len = strlen(tres->str) + 3;
				    qstr1 = (char *)fs_get(qstr1len*sizeof(char));
				    snprintf(qstr1, qstr1len, "\"%.*s\"", qstr1len, tres->str);
				    for(tstr1 = newaddr; tstr1; tstr1 = tstr2){
				        tstr2 = strqchr(tstr1, ',', 0, -1);
					if(strncmp(qstr1, tstr1, tstr2 ? tstr2 - tstr1
						   : strlen(tstr1)) == 0){
					    size_t l;
					    l = strlen(newaddr) + strlen(tres->str) + 2 
						 + (tstr2 ? strlen(tstr2) : 0);
					    newnewaddr = (char *) fs_get(l * sizeof(char));
					    snprintf(newnewaddr, l, "%.*s%s%s", tstr1 - newaddr, 
						    newaddr, tres->str, tstr2 ? tstr2 : "");
					    fs_give((void **)&newaddr);
					    newaddr = newnewaddr;
					    break;
					}
					if(tstr2)
					  tstr2++;
					if(tstr2 && *tstr2 == ' ')
					  tstr2++;
				    }
				    if(qstr1)
				      fs_give((void **) &qstr1);
			        }
			    }
			}
#endif /* ENABLE_LDAP */
		      	if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				    Tcl_NewStringObj(newaddr, -1)) != TCL_OK)
			  return(TCL_ERROR);
#ifdef ENABLE_LDAP
			if(wpldap_global->ldap_search_list){
			  if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				      Tcl_NewIntObj(wpldap_global->query_no)) != TCL_OK)
			      return(TCL_ERROR);
			}
			else
#endif /* ENABLE_LDAP */
		      	if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
				   Tcl_NewIntObj(0)) != TCL_OK)
			  return(TCL_ERROR);
			if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						    Tcl_NewStringObj(newfcc ? newfcc 
						    : (fcc ? fcc : ""), -1)) != TCL_OK)
			  return(TCL_ERROR);
			if(newfcc) fs_give((void **)&newfcc);

			return(TCL_OK);
		    }
		    else{
			Tcl_SetResult(interp, error ? error : "Indeterminate error", TCL_VOLATILE);
			if(newfcc) fs_give((void **)&newfcc);
			return(TCL_ERROR);
		    }
		}
	    }
	}
	else if(objc == 5){
	    if(!strcmp(op, "entry")){
		int	     booknum, i, aindex;
		char	    *nick, *astr = NULL, *errstr = NULL, *fccstr = NULL, buf[128], *extstr = NULL;
		AdrBk_Entry *ae;
		BuildTo      bldto;

		if(peInitAddrbooks(interp, 0) != TCL_OK)
		  return(TCL_ERROR);

		/*
		 * Given an address book handle and nickname, return address
		 */
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK)
		  for(i = 0; i < as.n_addrbk; i++)
		    if(i == booknum){
			if((nick = Tcl_GetStringFromObj(objv[3], NULL)) == NULL){
			    Tcl_SetResult(interp, "PEAddress list: Can't get nickname", TCL_STATIC);
			    return(TCL_ERROR);
			}
			if(Tcl_GetIntFromObj(interp, objv[4], &aindex) != TCL_OK
			    || (*nick == '\0' && aindex < 0)){
			    Tcl_SetResult(interp, "PEAddress list: Can't get aindex", TCL_STATIC);
			    return(TCL_ERROR);
			}
			if((*nick)
			   ? (ae = adrbk_lookup_by_nick(as.adrbks[booknum].address_book, nick, NULL))
			   : (ae = adrbk_get_ae(as.adrbks[booknum].address_book, aindex))){
			    bldto.type    = Abe;
			    bldto.arg.abe = ae;

			    (void) our_build_address(bldto, &astr, &errstr, &fccstr, NULL);

			    if(errstr){
				if(astr)
				  fs_give((void **) &astr);

				Tcl_SetResult(interp, errstr, TCL_VOLATILE);
				return(TCL_ERROR);
			    }

			    if(astr){
				char    *tmp_a_string, *p, *dummy = NULL;
				ADDRESS *a = NULL;
				int	     l;

				l = (4*strlen(astr) + 1) * sizeof(char);
				p = (char *) fs_get(l);
				if(rfc1522_decode((unsigned char *) p, l, astr, &dummy) == (unsigned char *) p){
				    fs_give((void **) &astr);
				    astr = p;
				}
				else
				  fs_give((void **)&p);

				if(dummy)
				  fs_give((void **)&dummy);
			    }
			}

			if(astr){
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(astr, -1));
			    fs_give((void **) &astr);

			    if(fccstr){
				Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
							 Tcl_NewStringObj(*fccstr ? fccstr : "\"\"", -1));
				fs_give((void **) &fccstr);
			    }
			    else
			      Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
						       Tcl_NewStringObj("", -1));
			}
			else
			  Tcl_SetResult(interp, "", TCL_STATIC);

			return(TCL_OK);
		    }

		snprintf(buf, sizeof(buf), "PEAddress list: unknown address book ID %d", booknum);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	    else if(!strcmp(op, "fullentry")){
		int	     booknum, i, j, aindex;
		char	    *nick, *astr = NULL, *errstr = NULL, 
		            *fccstr = NULL, buf[128], *extstr = NULL;
		AdrBk_Entry *ae;
		BuildTo      bldto;
		Tcl_Obj     *resObj;

		if(peInitAddrbooks(interp, 0) != TCL_OK)
		  return(TCL_ERROR);

		/*
		 * Given an address book handle and nickname, return 
		 * nickname, fullname, address(es), fcc, and comments
		 */
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK){
		    if(booknum >= 0 && booknum < as.n_addrbk){
		        if((nick = Tcl_GetStringFromObj(objv[3], NULL)) == NULL)
			  return(TCL_ERROR);
			if(Tcl_GetIntFromObj(interp, objv[4], &aindex) != TCL_OK
			    || (*nick == '\0' && aindex < 0))
			  return(TCL_ERROR);
			if((*nick)
			   ? (ae = adrbk_lookup_by_nick(as.adrbks[booknum].address_book, nick, NULL))
			   : (ae = adrbk_get_ae(as.adrbks[booknum].address_book, aindex))){
			    Tcl_ListObjAppendElement(interp, 
						     Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(ae->nickname ? ae->nickname : "", -1));
			    Tcl_ListObjAppendElement(interp, 
						     Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(ae->fullname ? ae->fullname : "", -1));
			    resObj = Tcl_NewListObj(0,NULL);
			    if(ae->tag == Single)
			      Tcl_ListObjAppendElement(interp, 
						       resObj,
						       Tcl_NewStringObj(ae->addr.addr ? ae->addr.addr : "", -1));
			    else {
				for(j = 0; ae->addr.list[j]; j++)
				  Tcl_ListObjAppendElement(interp, resObj,
							   Tcl_NewStringObj(ae->addr.list[j] ? ae->addr.list[j] : "", -1));
			    }
			    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), resObj);
			    Tcl_ListObjAppendElement(interp, 
						     Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(ae->fcc ? ae->fcc : "", -1));
			    Tcl_ListObjAppendElement(interp, 
						     Tcl_GetObjResult(interp),
						     Tcl_NewStringObj(ae->extra ? ae->extra : "", -1));
			    return(TCL_OK);
			}
		    }
		}
		return(TCL_ERROR);
	    }
	    else if(!strcmp(op, "delete")){
	        char *nick, buf[256];
		int booknum, aindex;
		adrbk_cntr_t old_entry;
		AdrBk *ab;
		if(peInitAddrbooks(interp, 0) != TCL_OK){
		    snprintf(buf, sizeof(buf), "PEAddress delete: couldn't init addressbooks");
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK){
		    nick    = Tcl_GetStringFromObj(objv[3], NULL);
		    removing_leading_and_trailing_white_space(nick);
		}
		else
		  return(TCL_ERROR);
		if(booknum >= 0 && booknum < as.n_addrbk) {
		    if(as.adrbks[booknum].access != ReadWrite) return TCL_ERROR;
		    ab = as.adrbks[booknum].address_book;
		}
		else{
		    snprintf(buf, sizeof(buf), "PEAddress delete: Book number out of range");
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if((Tcl_GetIntFromObj(interp, objv[4], &aindex) != TCL_OK)
		   || (*nick == '\0' && aindex < 0))
		  return(TCL_ERROR);
		adrbk_check_validity(ab, 1L);
		if(ab->flags & FILE_OUTOFDATE ||
		   (ab->rd && ab->rd->flags & REM_OUTOFDATE)){
		    Tcl_SetResult(interp,
				  "Address book out of sync.  Cannot update at this moment",
				  TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if(!nick){
		    snprintf(buf, sizeof(buf), "PEAddress delete: No nickname");
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if((*nick)
		   ? (!adrbk_lookup_by_nick(ab, nick, &old_entry))
		   : ((old_entry = (adrbk_cntr_t)aindex) == -1)){
		    snprintf(buf, sizeof(buf), "PEAddress delete: Nickname \"%.128s\" not found", nick);
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		if(adrbk_delete(ab, old_entry, 0, 0, 1, 1)){
		    snprintf(buf, sizeof(buf), "PEAddress delete: Couldn't delete addressbook entry");
		    Tcl_SetResult(interp, buf, TCL_VOLATILE);
		    return(TCL_ERROR);
		}
		return(TCL_OK);
	    }
	}
	else if((objc == 10 || objc == 11) && !strcmp(op, "edit")){
	    if(!strcmp(op, "edit")){
	        int           booknum, i, adri, add, numlistvals = 0, rv, aindex;
		char         *nick, *fn, *fcc, *comment, *addrfield, 
		              buf[256], **addrs, *orignick = NULL;
		Tcl_Obj     **objVal;
		AdrBk_Entry  *ae;
		AdrBk        *ab;
		adrbk_cntr_t  old_entry, new_entry;
		Tag           tag;
		ADDRESS      *adr = NULL;
		
		
		if(peInitAddrbooks(interp, 0) != TCL_OK)
		    return(TCL_ERROR);
		if(Tcl_GetIntFromObj(interp, objv[2], &booknum) == TCL_OK){
		    if(as.adrbks[booknum].access != ReadWrite) return TCL_ERROR;
		    nick    = Tcl_GetStringFromObj(objv[3], NULL);
		    removing_leading_and_trailing_white_space(nick);
		    if(Tcl_GetIntFromObj(interp, objv[4], &aindex) != TCL_OK
			|| (*nick == '\0' && aindex < 0))
		      return(TCL_ERROR);
		    fn      = Tcl_GetStringFromObj(objv[5], NULL);
		    removing_leading_and_trailing_white_space(fn);
		    if(!*fn) fn = NULL;
		    addrfield = Tcl_GetStringFromObj(objv[6], NULL);
		    removing_leading_and_trailing_white_space(addrfield);
		    if(!*addrfield) addrfield = NULL;
		    /*
		    if(Tcl_ListObjGetElements(interp, objv[7], &numlistvals, &objVal) != TCL_OK)
		      return(TCL_ERROR);
		    */
		    fcc     = Tcl_GetStringFromObj(objv[7], NULL);
		    removing_leading_and_trailing_white_space(fcc);
		    if(!*fcc) fcc = NULL;
		    comment = Tcl_GetStringFromObj(objv[8], NULL);
		    removing_leading_and_trailing_white_space(comment);
		    if(!*comment) comment = NULL;
		    if(Tcl_GetIntFromObj(interp, objv[9], &add) != TCL_OK)
		      return(TCL_ERROR);
		    if(objc == 11) {
		        /*
			 * if objc == 11 then that means that they changed the 
			 * value of nick to something else, and this one is the
			 * original nick
			 */
		        orignick     = Tcl_GetStringFromObj(objv[10], NULL);
			removing_leading_and_trailing_white_space(orignick);
		    }
		    if(addrfield == NULL){
			Tcl_SetResult(interp, "No addresses defined",
				      TCL_VOLATILE);
			return(TCL_OK);
		    }
		    if(addrs = parse_addrlist(addrfield)){
		      int tbuflen = strlen(addrfield);
		      char *tbuf;
		      if(!(tbuf = (char *) fs_get(sizeof(char) * (tbuflen+128)))){
			Tcl_SetResult(interp, "malloc error", TCL_VOLATILE);
			fs_give((void **) &addrs);
			return(TCL_ERROR);
		      }
		      for(adri = 0; addrs[adri]; adri++){
			  if(*(addrs[adri])){
			      ps_global->c_client_error[0] = '\0';
			      strncpy(tbuf, addrs[adri], tbuflen+128);
			      tbuf[tbuflen+128-1] = '\0';
			      rfc822_parse_adrlist(&adr, tbuf, "@");
			      if(adr) mail_free_address(&adr);
			      adr = NULL;
			      if(ps_global->c_client_error[0]){
				snprintf(buf, sizeof(buf),"Problem with address %.10s%s: %s",
					addrs[adri], strlen(addrs[adri]) > 10 ?
					"..." : "", ps_global->c_client_error);
				Tcl_SetResult(interp, buf, TCL_VOLATILE);
				if(tbuf)
				  fs_give((void **) &tbuf);
				fs_give((void **) &addrs);
				return(TCL_OK);
			      }
			  }
		      }
		      if(tbuf) fs_give((void **)&tbuf);
		    }
		    else adri = 0;
		   
		    /* addrs[adri] = NULL; */

		    if(adri > 1) tag = List;
		    else tag = Single;
		    
		    if(booknum >= 0 && booknum < as.n_addrbk) {
		        ab = as.adrbks[booknum].address_book;
		    }
		    else{
		      fs_give((void **) &addrs);
		      return(TCL_ERROR);
		    }
		    adrbk_check_validity(ab, 1L);
		    if(ab->flags & FILE_OUTOFDATE ||
		       (ab->rd && ab->rd->flags & REM_OUTOFDATE)){
			Tcl_SetResult(interp,
				      "Address book out of sync.  Cannot update at this moment",
				      TCL_VOLATILE);
			return(TCL_ERROR);
		    }
		    if(!nick) {
		        snprintf(buf, sizeof(buf), "No nickname defined.");
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			fs_give((void **) &addrs);
			return(TCL_OK);
		    }
		    if((orignick ? *orignick : *nick)
		       ? !(ae = adrbk_lookup_by_nick(ab, orignick ? orignick : nick, &old_entry))
		       : !((ae = adrbk_get_ae(as.adrbks[booknum].address_book, aindex))
			   && ((old_entry = (adrbk_cntr_t)aindex) != -1)))
		      old_entry = NO_NEXT;
		    if((ae && add) || 
		       (orignick && strcmp(orignick, nick) 
			&& adrbk_lookup_by_nick(ab, nick, NULL))) {
		        snprintf(buf, sizeof(buf), "Entry with nickname %.128s already exists.",
				nick);
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			fs_give((void **) &addrs);
			return(TCL_OK);
		    }
		    if(ae &&
		       ((tag == List && ae->tag == Single) ||
			(tag == Single && ae->tag == List))){
		        if(adrbk_delete(ab, old_entry, 0,0,1,0)){
			    snprintf(buf, sizeof(buf), "Problem updating from %s to %s.",
				    ae->tag == Single ? "Single" : "List", 
				    tag == List ? "List" : "Single");
			    Tcl_SetResult(interp, buf, TCL_VOLATILE);
			    fs_give((void **) &addrs);
			    return(TCL_ERROR);
			}
			old_entry = NO_NEXT;
		    }
		    if(rv = adrbk_add(ab, old_entry, 
				      nick ? nick : "", 
				      fn ? fn : "", 
				      tag == List ? (char *)addrs : 
				      *addrs ? *addrs : "", 
				      fcc ? fcc : "", 
				      comment ? comment : "", 
				      tag, &new_entry, NULL, 0, 1, 
				      tag == List ? 0 : 1)){
		        snprintf(buf, sizeof(buf), "Couldn't add entry! rv=%d.", rv);
			Tcl_SetResult(interp, buf, TCL_VOLATILE);
			fs_give((void **) &addrs);
			return(TCL_OK);
		    }
		    if(tag == List) {
		      adrbk_listdel_all(ab, new_entry, 0);
		      adrbk_nlistadd(ab, new_entry, addrs, 0, 1, 1);
		    }
		    return(TCL_OK);
		}
		snprintf(buf, sizeof(buf), "Unknown address book ID %d", booknum);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	}
    }

    Tcl_SetResult(interp, "PEAddress: unrecognized command", TCL_STATIC);
    return(TCL_ERROR);
}


int
peInitAddrbooks(Tcl_Interp *interp, int safe)
{
    if(ps_global->remote_abook_validity > 0)
      (void)adrbk_check_and_fix_all(safe, 0, 0);

    if(!init_addrbooks(NoDisplay, 1, 1, 0)){
	Tcl_SetResult(interp, "No Address Book Configured", TCL_STATIC);
	return(TCL_ERROR);
    }

    return(TCL_OK);
}



int
peRuleStatVal(char *str, int  *n)
{
    if(!strcmp(str, "either"))
      *n = PAT_STAT_EITHER;
    else if(!strcmp(str, "yes"))
      *n = PAT_STAT_YES;
    else if(!strcmp(str, "no"))
      *n = PAT_STAT_NO;
    else
      return 1;
    return 0;
}

#define RS_RULE_EDIT      0x0001
#define RS_RULE_ADD       0x0002
#define RS_RULE_DELETE    0x0004
#define RS_RULE_SHUFFUP   0x0008
#define RS_RULE_SHUFFDOWN 0x0010
#define RS_RULE_GETPAT    0x0100
#define RS_RULE_FINDPAT   0x0200

int
peRuleSet(Tcl_Interp *interp, Tcl_Obj **objv)
{
    char *rule, *patvar, *patval, *actvar, *actval, *tstr, *ruleaction;
    int rno, nPat, nPatEmnt, nAct, nActEmnt, i, statval, get_pat, rv = 0;
    Tcl_Obj **objPat, **objPatEmnt, **objAct, **objActEmnt;
    long rflags = PAT_USE_CHANGED, aflags = 0;
    PAT_STATE pstate;
    PAT_S *pat, *new_pat;

    if(!(rule = Tcl_GetStringFromObj(objv[0], NULL)))
      return(TCL_ERROR);
    if(!(ruleaction = Tcl_GetStringFromObj(objv[1], NULL)))
      return(TCL_ERROR);
    if(Tcl_GetIntFromObj(interp, objv[2], &rno) == TCL_ERROR)
      return(TCL_ERROR);
    if(!(strcmp(rule, "filter")))
      rflags |= ROLE_DO_FILTER;
    else
      return(TCL_ERROR);

    if(!(strcmp(ruleaction, "edit"))){
      aflags |= RS_RULE_EDIT;
      aflags |= RS_RULE_GETPAT;
      aflags |= RS_RULE_FINDPAT;
    }
    else if(!(strcmp(ruleaction, "add"))){
      aflags |= RS_RULE_ADD;
      aflags |= RS_RULE_GETPAT;
    }
    else if(!(strcmp(ruleaction, "delete"))){
      aflags |= RS_RULE_DELETE;
      aflags |= RS_RULE_FINDPAT;
    }
    else if(!(strcmp(ruleaction, "shuffup"))){
      aflags |= RS_RULE_SHUFFUP;
      aflags |= RS_RULE_FINDPAT;
    }
    else if(!(strcmp(ruleaction, "shuffdown"))){
      aflags |= RS_RULE_SHUFFDOWN;
      aflags |= RS_RULE_FINDPAT;
    }
    else return(TCL_ERROR);

    if(aflags & RS_RULE_FINDPAT){
	if(any_patterns(rflags, &pstate)){
	    for(pat = first_pattern(&pstate), i = 0;
		pat && i != rno;
		pat = next_pattern(&pstate), i++);
	    if(i != rno) return(TCL_ERROR);
	}
    }
    if(aflags & RS_RULE_GETPAT){
	Tcl_ListObjGetElements(interp, objv[3], &nPat, &objPat);
	Tcl_ListObjGetElements(interp, objv[4], &nAct, &objAct);

	new_pat = (PAT_S *)fs_get(sizeof(PAT_S));
	memset(new_pat, 0, sizeof(PAT_S));
	new_pat->patgrp = (PATGRP_S *)fs_get(sizeof(PATGRP_S));
	memset(new_pat->patgrp, 0, sizeof(PATGRP_S));
	new_pat->action = (ACTION_S *)fs_get(sizeof(ACTION_S));
	memset(new_pat->action, 0, sizeof(ACTION_S));


	/* Set up the pattern group */
	for(i = 0; i < nPat; i++){
	    Tcl_ListObjGetElements(interp, objPat[i], &nPatEmnt, &objPatEmnt);
	    if(nPatEmnt != 2) return(TCL_ERROR);
	    patvar = Tcl_GetStringFromObj(objPatEmnt[0], NULL);
	    patval = Tcl_GetStringFromObj(objPatEmnt[1], NULL);
	    if(!patvar || !patval) return(TCL_ERROR);

	    tstr = NULL;
	    if(*patval){
		tstr = cpystr(patval);
		removing_leading_and_trailing_white_space(tstr);
		if(!(*tstr))
		  fs_give((void **)&tstr);
	    }
	    if(!(strcmp(patvar, "nickname")))
	      new_pat->patgrp->nick = tstr;
	    else if(!(strcmp(patvar, "to"))){
		new_pat->patgrp->to = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "from"))){
		new_pat->patgrp->from = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "sender"))){
		new_pat->patgrp->sender = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "cc"))){
		new_pat->patgrp->cc = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "recip"))){
		new_pat->patgrp->recip = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "partic"))){
		new_pat->patgrp->partic = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "news"))){
		new_pat->patgrp->news = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "subj"))){
		new_pat->patgrp->subj = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "alltext"))){
		new_pat->patgrp->alltext = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "ftype"))){
		if(!tstr) return(TCL_ERROR);
		if(!(strcmp(patval, "any")))
		  new_pat->patgrp->fldr_type = FLDR_ANY;
		else if(!(strcmp(patval, "news")))
		  new_pat->patgrp->fldr_type = FLDR_NEWS;
		else if(!(strcmp(patval, "email")))
		  new_pat->patgrp->fldr_type = FLDR_EMAIL;
		else if(!(strcmp(patval, "specific")))
		  new_pat->patgrp->fldr_type = FLDR_SPECIFIC;
		else{
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}
	    }
	    else if(!(strcmp(patvar, "folder"))){
		new_pat->patgrp->folder = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(!(strcmp(patvar, "stat_new"))){
		if(peRuleStatVal(patval, &statval)){
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}

		new_pat->patgrp->stat_new = statval;
	    }
	    else if(!(strcmp(patvar, "stat_del"))){
		if(peRuleStatVal(patval, &statval)){
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}

		new_pat->patgrp->stat_del = statval;
	    }
	    else if(!(strcmp(patvar, "stat_imp"))){
		if(peRuleStatVal(patval, &statval)){
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}

		new_pat->patgrp->stat_imp = statval;
	    }
	    else if(!(strcmp(patvar, "stat_ans"))){
		if(peRuleStatVal(patval, &statval)){
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}

		new_pat->patgrp->stat_ans = statval;
	    }
	    else{
		free_pat(&new_pat);
		return(TCL_ERROR);
	    }
	}
	if(new_pat->patgrp->fldr_type != FLDR_SPECIFIC && new_pat->patgrp->folder)
	  fs_give((void **)&new_pat->patgrp->folder);

    /* set up the action */
	if(!(strcmp(rule, "filter")))
	  new_pat->action->is_a_filter = 1;
	else if(!(strcmp(rule, "role")))
	  new_pat->action->is_a_role = 1;
	else if(!(strcmp(rule, "score")))
	  new_pat->action->is_a_score = 1;
	else if(!(strcmp(rule, "incol")))
	  new_pat->action->is_a_incol = 1;
	else{
	    free_pat(&new_pat);
	    return(TCL_ERROR);
	}

	for(i = 0; i < nAct; i++){
	    Tcl_ListObjGetElements(interp, objAct[i], &nActEmnt, &objActEmnt);
	    if(nActEmnt !=2){
		free_pat(&new_pat);
		return(TCL_ERROR);
	    }

	    actvar = Tcl_GetStringFromObj(objActEmnt[0], NULL);
	    actval = Tcl_GetStringFromObj(objActEmnt[1], NULL);
	    if(!actvar || !actval){
		free_pat(&new_pat);
		return(TCL_ERROR);
	    }

	    if(new_pat->action->is_a_filter && !(strcmp(actvar, "action"))){
		if(!strcmp(actval, "delete"))
		  new_pat->action->kill = 1;
		else if(!strcmp(actval, "move"))
		  new_pat->action->kill = 0;
		else{
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}
	    }
	    else if(new_pat->action->is_a_filter && !(strcmp(actvar, "folder"))){
		tstr = cpystr(actval);
		removing_leading_and_trailing_white_space(tstr);
		if(!(*tstr)) fs_give((void **)&tstr);
		new_pat->action->folder = string_to_pattern(tstr);
		if(tstr) fs_give((void **)&tstr);
	    }
	    else if(new_pat->action->is_a_filter && !(strcmp(actvar, "moind"))){
		if(!strcmp(actval, "1"))
		  new_pat->action->move_only_if_not_deleted = 1;
		else if(!strcmp(actval, "0"))
		  new_pat->action->move_only_if_not_deleted = 0;
		else{
		    free_pat(&new_pat);
		    return(TCL_ERROR);
		}
	    }
	    else{
		free_pat(&new_pat);
		return(TCL_ERROR);
	    }
	}

	if(new_pat->action->is_a_filter && new_pat->action->kill && new_pat->action->folder)
	  fs_give((void **)&new_pat->action->folder);
	else if(new_pat->action->is_a_filter && new_pat->action->kill == 0 && new_pat->action->folder == 0){
	    free_pat(&new_pat);
	    Tcl_SetResult(interp, "No folder set for Move", TCL_VOLATILE);
	    return(TCL_OK);
	}
    }

    if(aflags & RS_RULE_EDIT)
      rv = edit_pattern(new_pat, rno, rflags);
    else if(aflags & RS_RULE_ADD)
      rv = add_pattern(new_pat, rflags);
    else if(aflags & RS_RULE_DELETE)
      rv = delete_pattern(rno, rflags);
    else if(aflags & RS_RULE_SHUFFUP)
      rv = shuffle_pattern(rno, 1, rflags);
    else if(aflags & RS_RULE_SHUFFDOWN)
      rv = shuffle_pattern(rno, -1, rflags);
    else
      rv = 1;

    return(rv ? TCL_ERROR : TCL_OK);
}


#if	0
ADDRESS *
peAEToAddress(AdrBk_Entry  *ae)
{
    char    *list, *l1, *l2;
    int	     length;
    BuildTo  bldto;
    ADDRESS *addr = NULL;

    if(ae->tag == List){
	length = 0;
	for(l2 = ae->addr.list; *l2; l2++)
	  length += (strlen(*l2) + 1);

	list = (char *) fs_get(length + 1);
	list[0] = '\0';
	l1 = list;
	for(l2 = ae->addr.list; *l2; l2++){
	    if(l1 != list && l1-list < length+1)
	      *l1++ = ',';

	    strncpy(l1, *l2, length+1-(l1-list));
	    l1 += strlen(l1);
	}

	list[length] = '\0';

	bldto.type    = Str;
	bldto.arg.str = list;
	adr2 = expand_address(bldto, userdomain, localdomain,
			      loop_detected, fcc, did_set,
			      lcc, error, 1, simple_verify,
			      mangled);

	fs_give((void **) &list);
    }
    else if(ae->tag == Single){
	if(strucmp(ae->addr.addr, a->mailbox)){
	    bldto.type    = Str;
	    bldto.arg.str = ae->addr.addr;
	    adr2 = expand_address(bldto, userdomain,
				  localdomain, loop_detected,
				  fcc, did_set, lcc,
				  error, 1, simple_verify,
				  mangled);
	}
	else{
	    /*
	     * A loop within plain single entry is ignored.
	     * Set up so later code thinks we expanded.
	     */
	    adr2          = mail_newaddr();
	    adr2->mailbox = cpystr(ae->addr.addr);
	    adr2->host    = cpystr(userdomain);
	    adr2->adl     = cpystr(a->adl);
	}
    }

    /*
     * Personal names:  If the expanded address has a personal
     * name and the address book entry is a list with a fullname,
     * tack the full name from the address book on in front.
     * This mainly occurs with a distribution list where the
     * list has a full name, and the first person in the list also
     * has a full name.
     *
     * This algorithm doesn't work very well if lists are
     * included within lists, but it's not clear what would
     * be better.
     */
    if(ae->fullname && ae->fullname[0]){
	if(adr2->personal && adr2->personal[0]){
	    if(ae->tag == List){
		/* combine list name and existing name */
		char *name;

		if(!simple_verify){
		    size_t l;
		    l = strlen(adr2->personal) + strlen(ae->fullname) + 4;
		    name = (char *)fs_get((l+1) * sizeof(char));
		    snprintf(name, l+1, "%s -- %s", ae->fullname,
			    adr2->personal);
		    fs_give((void **)&adr2->personal);
		    adr2->personal = name;
		}
	    }
	    else{
		/* replace with nickname fullname */
		fs_give((void **)&adr2->personal);
		adr2->personal = adrbk_formatname(ae->fullname,
						  NULL, NULL);
	    }
	}
	else{
	    if(abe-p>tag != List || !simple_verify){
		if(adr2->personal)
		  fs_give((void **)&adr2->personal);

		adr2->personal = adrbk_formatname(abe->fullname,
						  NULL, NULL);
	    }
	}
    }

    return(addr);
}



char *
peAEFcc(AdrBk_Entry *ae)
{
    char *fcc = NULL;

    if(ae->fcc && ae->fcc[0]){

	if(!strcmp(ae->fcc, "\"\""))
	  fcc = cpystr("");
	else
	  fcc = cpystr(ae->fcc);

    }
    else if(ae->nickname && ae->nickname[0] &&
	    (ps_global->fcc_rule == FCC_RULE_NICK ||
	     ps_global->fcc_rule == FCC_RULE_NICK_RECIP)){
	/*
	 * else if fcc-rule=fcc-by-nickname, use that
	 */

	fcc = cpystr(ae->nickname);
    }

    return(fcc);
}
#endif


PINEFIELD *
peCustomHdrs(void)
{
    extern PINEFIELD *parse_custom_hdrs(char **, CustomType);

    return(parse_custom_hdrs(ps_global->VAR_CUSTOM_HDRS, UseAsDef));
}



/*
 * PEClistCmd - Collection list editing tools
 */
int
PEClistCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char *err = "Unknown PEClist request";

    dprint((2, "PEClistCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
    }
    else{
	char *s1 = Tcl_GetStringFromObj(objv[1], NULL);

	if(s1){
	    if(objc == 3){ /* delete */
	        if(!strcmp(s1, "delete")){
		    int cl, i, n, deln;
		    char **newl;
		    CONTEXT_S *del_ctxt, *tmp_ctxt, *new_ctxt;

		    if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR){
		      Tcl_SetResult(interp,
				    "cledit malformed: first arg must be int", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    for(i = 0, del_ctxt = ps_global->context_list;
			del_ctxt && i < cl; i++, del_ctxt = del_ctxt->next);
		    if(!del_ctxt) return(TCL_ERROR);
		    for(n = 0; del_ctxt->var.v->current_val.l[n]; n++);
		    n--;
		    newl = (char **) fs_get((n + 1) * sizeof(char *));
		    newl[n] = NULL;
		    deln = del_ctxt->var.i;
		    for(i = 0; del_ctxt->var.v->current_val.l[i]; i++){
		      if(i < deln)
			newl[i] = cpystr(del_ctxt->var.v->current_val.l[i]);
		      else if(i > deln)
			newl[i-1] = cpystr(del_ctxt->var.v->current_val.l[i]);
		    }
		    n = set_variable_list(del_ctxt->var.v - ps_global->vars, 
					  *newl ? newl : NULL, TRUE, Main);
		    free_list_array(&newl);
		    set_current_val(del_ctxt->var.v, TRUE, FALSE);
		    if(n){
		      Tcl_SetResult(interp,
				    "Error saving changes", 
				    TCL_VOLATILE);
		      return TCL_OK;
		    }
		    for(tmp_ctxt = del_ctxt->next; tmp_ctxt && tmp_ctxt->var.v == 
			  del_ctxt->var.v; tmp_ctxt = tmp_ctxt->next)
		      tmp_ctxt->var.i--;
		    if(tmp_ctxt = del_ctxt->next)
		      tmp_ctxt->prev = del_ctxt->prev;
		    if(tmp_ctxt = del_ctxt->prev)
		      tmp_ctxt->next= del_ctxt->next;
		    if(!del_ctxt->prev && !del_ctxt->next){
		        new_ctxt = new_context(del_ctxt->var.v->current_val.l[0], NULL);
			ps_global->context_list = new_ctxt;
			if(!new_ctxt->var.v)
			  new_ctxt->var = del_ctxt->var;
		    }
		    else if(ps_global->context_list == del_ctxt){
		        ps_global->context_list = del_ctxt->next;
			if(!ps_global->context_list)
			  return TCL_ERROR;  /* this shouldn't happen */
		    }
		    if(ps_global->context_last == del_ctxt)
		      ps_global->context_last = NULL;
		    if(ps_global->context_current == del_ctxt){
		      strncpy(ps_global->cur_folder, 
			     ps_global->mail_stream->mailbox,
			     sizeof(ps_global->cur_folder));
		      ps_global->cur_folder[sizeof(ps_global->cur_folder)-1] = '\0';
		      ps_global->context_current = ps_global->context_list;
		    }
		    del_ctxt->prev = NULL;
		    del_ctxt->next = NULL;
		    free_context(&del_ctxt);
		    init_inbox_mapping(ps_global->VAR_INBOX_PATH,
				       ps_global->context_list);
		    return TCL_OK;
		}
		else if(!strcmp(s1, "shuffdown")){
		    int cl, i, shn, n;
		    CONTEXT_S *sh_ctxt, *nsh_ctxt, *tctxt;
		    char **newl, *tmpch;

		    if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR){
		      Tcl_SetResult(interp,
				    "cledit malformed: first arg must be int", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    for(sh_ctxt = ps_global->context_list, i = 0;
			sh_ctxt && i < cl ; i++, sh_ctxt = sh_ctxt->next);
		    if(!sh_ctxt || !sh_ctxt->next){
		      Tcl_SetResult(interp,
				    "invalid context list number", 
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    if(sh_ctxt->var.v == sh_ctxt->next->var.v){
		        shn = sh_ctxt->var.i;
			for(n = 0; sh_ctxt->var.v->current_val.l[n]; n++);
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			for(i = 0; sh_ctxt->var.v->current_val.l[i]; i++){
			  if(i == shn)
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i+1]);
			  else if(i == shn + 1)
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i-1]);
			  else
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i]);
			}
			n = set_variable_list(sh_ctxt->var.v - ps_global->vars, 
					      newl, TRUE, Main);
			free_list_array(&newl);
			set_current_val(sh_ctxt->var.v, TRUE, FALSE);
			if(n){
			  Tcl_SetResult(interp,
					"Error saving changes", 
					TCL_VOLATILE);
			  return TCL_OK;
			}
			nsh_ctxt = sh_ctxt->next;
			nsh_ctxt->var.i--;
			sh_ctxt->var.i++;
		    }
		    else{
		        nsh_ctxt = sh_ctxt->next;
			shn = sh_ctxt->var.i;
			tmpch = cpystr(sh_ctxt->var.v->current_val.l[shn]);
			for(n = 0; sh_ctxt->var.v->current_val.l[n]; n++);
			n--;
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			for(i = 0; sh_ctxt->var.v->current_val.l[i+1]; i++)
			  newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i]);
			n = set_variable_list(sh_ctxt->var.v - ps_global->vars, 
					      newl, FALSE, Main);
			free_list_array(&newl);
			set_current_val(sh_ctxt->var.v, TRUE, FALSE);
			for(n = 0; nsh_ctxt->var.v->current_val.l[n]; n++);
			n++;
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			newl[0] = cpystr(nsh_ctxt->var.v->current_val.l[0]);
			newl[1] = tmpch;
			for(i = 2; nsh_ctxt->var.v->current_val.l[i-1]; i++)
			  newl[i] = cpystr(nsh_ctxt->var.v->current_val.l[i-1]);
			n = set_variable_list(nsh_ctxt->var.v - ps_global->vars, 
					      newl, TRUE, Main);
			free_list_array(&newl);
			set_current_val(nsh_ctxt->var.v, TRUE, FALSE);
			sh_ctxt->var.v = nsh_ctxt->var.v;
			sh_ctxt->var.i = 1;
			/* this for loop assumes that there are only two variable lists,
			 * folder-collections and news-collections, a little more will
			 * have to be done if we want to accomodate for the INHERIT
			 * option introduced in 4.30.
			 */
			for(tctxt = nsh_ctxt->next; tctxt; tctxt = tctxt->next)
			  tctxt->var.i++;
		    }
		    if(sh_ctxt->prev) sh_ctxt->prev->next = nsh_ctxt;
		    nsh_ctxt->prev = sh_ctxt->prev;
		    sh_ctxt->next = nsh_ctxt->next;
		    nsh_ctxt->next = sh_ctxt;
		    sh_ctxt->prev = nsh_ctxt;
		    if(sh_ctxt->next) sh_ctxt->next->prev = sh_ctxt;
		    if(ps_global->context_list == sh_ctxt)
		      ps_global->context_list = nsh_ctxt;
		    init_inbox_mapping(ps_global->VAR_INBOX_PATH,
				       ps_global->context_list);
		    return TCL_OK;
		}
		else if(!strcmp(s1, "shuffup")){
		    int cl, i, shn, n;
		    CONTEXT_S *sh_ctxt, *psh_ctxt, *tctxt;
		    char **newl, *tmpch;

		    if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR){
		      Tcl_SetResult(interp,
				    "cledit malformed: first arg must be int", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    for(sh_ctxt = ps_global->context_list, i = 0;
			sh_ctxt && i < cl ; i++, sh_ctxt = sh_ctxt->next);
		    if(!sh_ctxt || !sh_ctxt->prev){
		      Tcl_SetResult(interp,
				    "invalid context list number", 
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    if(sh_ctxt->var.v == sh_ctxt->prev->var.v){
		        shn = sh_ctxt->var.i;
			for(n = 0; sh_ctxt->var.v->current_val.l[n]; n++);
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			for(i = 0; sh_ctxt->var.v->current_val.l[i]; i++){
			  if(i == shn)
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i-1]);
			  else if(i == shn - 1)
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i+1]);
			  else
			    newl[i] = cpystr(sh_ctxt->var.v->current_val.l[i]);
			}
			i = set_variable_list(sh_ctxt->var.v - ps_global->vars, 
					      newl, TRUE, Main);
			free_list_array(&newl);
			set_current_val(sh_ctxt->var.v, TRUE, FALSE);
			if(i){
			  Tcl_SetResult(interp,
					"Error saving changes", 
					TCL_VOLATILE);
			  return TCL_OK;
			}
			psh_ctxt = sh_ctxt->prev;
			psh_ctxt->var.i++;
			sh_ctxt->var.i--;
		    }
		    else{
		        psh_ctxt = sh_ctxt->prev;
			shn = sh_ctxt->var.i;
			tmpch = cpystr(sh_ctxt->var.v->current_val.l[shn]);
			for(n = 0; sh_ctxt->var.v->current_val.l[n]; n++);
			n--;
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			for(i = 1; sh_ctxt->var.v->current_val.l[i]; i++)
			  newl[i-1] = cpystr(sh_ctxt->var.v->current_val.l[i]);
			i = set_variable_list(sh_ctxt->var.v - ps_global->vars, 
					      newl, FALSE, Main);
			free_list_array(&newl);
			if(i){
			  Tcl_SetResult(interp,
					"Error saving changes", 
					TCL_VOLATILE);
			  return TCL_OK;
			}
			set_current_val(sh_ctxt->var.v, TRUE, FALSE);
			for(n = 0; psh_ctxt->var.v->current_val.l[n]; n++);
			n++;
			newl = (char **) fs_get((n + 1) * sizeof(char *));
			newl[n] = NULL;
			for(i = 0; psh_ctxt->var.v->current_val.l[i+1]; i++)
			  newl[i] = cpystr(psh_ctxt->var.v->current_val.l[i]);
			newl[i++] = tmpch;
			newl[i] = cpystr(psh_ctxt->var.v->current_val.l[i-1]);
			i = set_variable_list(psh_ctxt->var.v - ps_global->vars, 
					      newl, TRUE, Main);
			free_list_array(&newl);
			if(i){
			  Tcl_SetResult(interp,
					"Error saving changes", 
					TCL_VOLATILE);
			  return TCL_OK;
			}
			set_current_val(psh_ctxt->var.v, TRUE, FALSE);
			for(tctxt = sh_ctxt->next ; tctxt; tctxt = tctxt->next)
			  tctxt->var.i--;
			sh_ctxt->var.v = psh_ctxt->var.v;
			sh_ctxt->var.i = n - 2;
			/* There MUST be at least 2 collections in the list */
			psh_ctxt->var.i++;
		    }
		    if(sh_ctxt->next) sh_ctxt->next->prev = psh_ctxt;
		    psh_ctxt->next = sh_ctxt->next;
		    sh_ctxt->prev = psh_ctxt->prev;
		    psh_ctxt->prev = sh_ctxt;
		    sh_ctxt->next = psh_ctxt;
		    if(sh_ctxt->prev) sh_ctxt->prev->next = sh_ctxt;
		    if(ps_global->context_list == psh_ctxt)
		      ps_global->context_list = sh_ctxt;
		    init_inbox_mapping(ps_global->VAR_INBOX_PATH,
				       ps_global->context_list);
		    return TCL_OK;
		}
	    }
	    else if(objc == 7){
	        if(!strcmp(s1, "edit") || !strcmp(s1, "add")){
		    int cl, quotes_needed = 0, i, add = 0, n = 0, vnum;
		    char *nick, *server, *path, *view, 
		          context_buf[MAILTMPLEN*4], **newl;
		    CONTEXT_S *new_ctxt, *tmp_ctxt;

		    if(!strcmp(s1, "add")) add = 1;

		    if(Tcl_GetIntFromObj(interp, objv[2], &cl) == TCL_ERROR){
		      Tcl_SetResult(interp,
				    "cledit malformed: first arg must be int", 
				    TCL_VOLATILE);
		      return(TCL_ERROR);
		    }
		    if(!(nick = Tcl_GetStringFromObj(objv[3], NULL))){
		      Tcl_SetResult(interp,
				    "Error1",
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    if(!(server = Tcl_GetStringFromObj(objv[4], NULL))){
		      Tcl_SetResult(interp,
				    "Error2",
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    if(!(path = Tcl_GetStringFromObj(objv[5], NULL))){
		      Tcl_SetResult(interp,
				    "Error3",
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    if(!(view = Tcl_GetStringFromObj(objv[6], NULL))){
		      Tcl_SetResult(interp,
				    "Error4",
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    removing_leading_and_trailing_white_space(nick);
		    removing_leading_and_trailing_white_space(server);
		    removing_leading_and_trailing_white_space(path);
		    removing_leading_and_trailing_white_space(view);
		    if(strchr(nick, ' '))
		      quotes_needed = 1;
		    if(strlen(nick)+strlen(server)+strlen(path)+strlen(view) >
		       MAILTMPLEN * 4 - 20) { /* for good measure */
		      Tcl_SetResult(interp,
				    "info too long", 
				    TCL_VOLATILE);

		      return TCL_ERROR;
		    }
		    if(3 + strlen(nick) + strlen(server) + strlen(path) +
		       strlen(view) > MAILTMPLEN + 4){
		       Tcl_SetResult(interp,
				     "collection fields too long", 
				     TCL_VOLATILE);
		       return(TCL_OK);
		    }
		    snprintf(context_buf, sizeof(context_buf), "%s%s%s%s%s%s[%s]", quotes_needed ?
			    "\"" : "", nick, quotes_needed ? "\"" : "",
			    strlen(nick) ? " " : "",
			    server, path, view);
		    new_ctxt = new_context(context_buf, NULL);
		    if(!add){
		        for(tmp_ctxt = ps_global->context_list, i = 0;
			   tmp_ctxt && i < cl; i++, tmp_ctxt = tmp_ctxt->next);
			if(!tmp_ctxt){
			    Tcl_SetResult(interp,
					  "invalid context list number", 
					  TCL_VOLATILE);
			    return TCL_ERROR;
			}
			new_ctxt->next = tmp_ctxt->next;
			new_ctxt->prev = tmp_ctxt->prev;
			if(tmp_ctxt->prev && tmp_ctxt->prev->next == tmp_ctxt)
			    tmp_ctxt->prev->next = new_ctxt;
			if(tmp_ctxt->next && tmp_ctxt->next->prev == tmp_ctxt)
			    tmp_ctxt->next->prev = new_ctxt;
			if(ps_global->context_list == tmp_ctxt)
			    ps_global->context_list = new_ctxt;
			if(ps_global->context_current == tmp_ctxt){
			    strncpy(ps_global->cur_folder, 
				   ps_global->mail_stream->mailbox,
				   sizeof(ps_global->cur_folder));
			    ps_global->cur_folder[sizeof(ps_global->cur_folder)-1] = '\0';
			    ps_global->context_current = new_ctxt;
			}
			if(ps_global->context_last == tmp_ctxt)
			    ps_global->context_last = new_ctxt;
			new_ctxt->var = tmp_ctxt->var;
			tmp_ctxt->next = tmp_ctxt->prev = NULL;
			free_context(&tmp_ctxt);
		    }
		    else {
		        for(tmp_ctxt = ps_global->context_list;
			    tmp_ctxt->next; tmp_ctxt = tmp_ctxt->next);
			new_ctxt->prev = tmp_ctxt;
			tmp_ctxt->next = new_ctxt;
			new_ctxt->var.v = tmp_ctxt->var.v;
			new_ctxt->var.i = tmp_ctxt->var.i + 1;
		    }
		    if(!new_ctxt->var.v){
		      Tcl_SetResult(interp,
				    "Error5",
				    TCL_VOLATILE);
		      return TCL_ERROR;
		    }
		    for(n = 0; new_ctxt->var.v->current_val.l[n]; n++);
		    if(add) n++;
		    newl = (char **) fs_get((n + 1) * sizeof(char *));
		    newl[n] = NULL;
		    for(n = 0; new_ctxt->var.v->current_val.l[n]; n++)
		        newl[n] = (n == new_ctxt->var.i)
			  ? cpystr(context_buf)
			  : cpystr(new_ctxt->var.v->current_val.l[n]);
		    if(add) newl[n++] = cpystr(context_buf);
		    n = set_variable_list(new_ctxt->var.v - ps_global->vars,
					  newl, TRUE, Main);
		    free_list_array(&newl);
		    set_current_val(new_ctxt->var.v, TRUE, FALSE);
		    init_inbox_mapping(ps_global->VAR_INBOX_PATH,
				       ps_global->context_list);
		    if(n){
		      Tcl_SetResult(interp,
				    "Error saving changes", 
				    TCL_VOLATILE);
		      return TCL_OK;
		    }
		    return TCL_OK;

		}
	    }
	}
    }
    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


/*
 * peTakeaddr - Take Address
 */
int
peTakeaddr(Tcl_Interp *interp,
	   long uid,
	   int objc,
	   Tcl_Obj **objv)
{
    TA_S     *talist = NULL, *current, *head;
    Tcl_Obj  *itemObj, *secObj = NULL, *resObj = NULL;
    int anum = 0, rv;

    mn_set_cur(ps_global->msgmap, peMessageNumber(uid));

    if(set_up_takeaddr('a', ps_global, ps_global->msgmap,
		       &talist, &anum, TA_NOPROMPT, NULL) < 0
                       || (talist == NULL)){
        Tcl_SetResult(interp,
		      "Take address failed to set up",
		      TCL_VOLATILE);
	return(TCL_ERROR);
    }

    for(head = talist ; head->prev; head = head->prev);
    /*
     * Return value will be of the form:
     *  {
     *   { "line to print",
     *     {"personal", "mailbox", "host"}         # addr
     *     {"nick", "fullname", "fcc", "comment"}  # suggested
     *   }
     *   ...
     *  }
     *
     *  The two list items will be empty if that line is
     *  just informational.
     */
    itemObj = Tcl_NewListObj(0, NULL);
    for(current = head; current ; current = current->next){
        if(current->skip_it && !current->print) continue;
	secObj = Tcl_NewListObj(0, NULL);
	if(Tcl_ListObjAppendElement(interp, secObj,
			   Tcl_NewStringObj(current->strvalue,-1)) != TCL_OK)
	    return(TCL_ERROR);
	resObj = Tcl_NewListObj(0, NULL);
	/* append the address information */
	if(current->addr && !current->print){
	    if(Tcl_ListObjAppendElement(interp, resObj,
				  Tcl_NewStringObj(current->addr->personal 
						   ? current->addr->personal 
						   : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj,
				     Tcl_NewStringObj(current->addr->mailbox 
						      ? current->addr->mailbox 
						      : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(current->addr->host
							 ? current->addr->host 
							 : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	}
	if(Tcl_ListObjAppendElement(interp, secObj,
				    resObj) != TCL_OK)
	    return(TCL_ERROR);
	resObj = Tcl_NewListObj(0, NULL);
	/* append the suggested possible entries */
	if(!current->print 
	   && (current->nickname || current->fullname
	       || current->fcc || current->comment)){
	    if(Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(current->nickname 
							 ? current->nickname 
							 : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(current->fullname
							 ? current->fullname 
							 : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(current->fcc 
							 ? current->fcc 
							 : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj,
					Tcl_NewStringObj(current->comment
							 ? current->comment 
							 : "", -1)) != TCL_OK)
	        return(TCL_ERROR);
	}
	if(Tcl_ListObjAppendElement(interp, secObj, resObj) != TCL_OK)
	    return(TCL_ERROR);
	if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), 
				    secObj) != TCL_OK)
	    return(TCL_ERROR);
    }

    free_talines(&talist);
    return(TCL_OK);
}


/* * * * * * * * * Status message ring management * * * * * * * * * * * * */

STATMSG_S *
sml_newmsg(int priority, char *text)
{
    static     long id = 1;
    STATMSG_S *smp;

    smp = (STATMSG_S *) fs_get(sizeof(STATMSG_S));
    memset(smp, 0, sizeof(STATMSG_S));
    smp->id = id++;
    smp->posted = time(0);
    smp->type = priority;
    smp->text = cpystr(text);
    return(smp);
}


void
sml_addmsg(int priority, char *text)
{
    STATMSG_S *smp = sml_newmsg(priority, text);

    if(peStatList){
	smp->next = peStatList;
	peStatList = smp;
    }
    else
      peStatList = smp;
}


char **
sml_getmsgs(void)
{
    int i, n;
    STATMSG_S *smp;
    char **retstrs = NULL, **tmpstrs;

    for(n = 0, smp = peStatList; smp && !smp->seen;
	n++, smp = smp->next);
    if(n == 0) return NULL;
    retstrs = (char **)fs_get((n+1)*sizeof(char *));
    for(tmpstrs = retstrs, smp = peStatList; smp && !smp->seen;
	smp = smp->next){
        *tmpstrs = smp->text;
	tmpstrs++;
    }
    *tmpstrs = NULL;
    return(retstrs);
}


char *
sml_getmsg(void)
{
    return(peStatList ? peStatList->text : "");
}

void
sml_seen(void)
{
    STATMSG_S *smp;

    for(smp = peStatList; smp; smp = smp->next)
      smp->seen = 1;
}



/* * * * * * * * * LDAP Support Routines  * * * * * * * * * * * */


/*
 * PELdapCmd - LDAP TCL interface
 */
int
PELdapCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
#ifndef ENABLE_LDAP
    char *err = "Call to PELdap when LDAP not enabled";
#else
    char *err = "Unknown PELdap request";
    char *s1;

    dprint((2, "PELdapCmd"));

    if(objc == 1){
	Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
	Tcl_SetResult(interp, err, TCL_STATIC);
	return(TCL_ERROR);
    }
    s1 = Tcl_GetStringFromObj(objv[1], NULL);

    if(s1){
        int qn;
	if(!strcmp(s1, "directories")){
	    int          i;
	    LDAP_SERV_S *info;
	    Tcl_Obj     *secObj;

	    if(objc != 2){
	        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
		Tcl_SetResult(interp, err, TCL_STATIC);
		return(TCL_ERROR);
	    }
	    if(ps_global->VAR_LDAP_SERVERS){
		for(i = 0; ps_global->VAR_LDAP_SERVERS[i] &&
		      ps_global->VAR_LDAP_SERVERS[i][0]; i++){
		    info = break_up_ldap_server(ps_global->VAR_LDAP_SERVERS[i]);
		    secObj = Tcl_NewListObj(0, NULL);
		    if(Tcl_ListObjAppendElement(interp, secObj,
						Tcl_NewStringObj(info->nick ? info->nick
								 : "", -1)) != TCL_OK)
		      return(TCL_ERROR);
		    if(Tcl_ListObjAppendElement(interp, secObj,
						Tcl_NewStringObj(info->serv ? info->serv
								 : "", -1)) != TCL_OK)
		      return(TCL_ERROR);

		    if(info)
		      free_ldap_server_info(&info);
		    if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), 
						secObj) != TCL_OK)
		      return(TCL_ERROR);
		}
	    }
	    else
	      Tcl_SetResult(interp, "", TCL_STATIC);

	    return(TCL_OK);
	}
	else if(!strcmp(s1, "query")){
	    int               dir;
	    char             *srchstr, *filtstr;
	    LDAP_SERV_RES_S  *winning_e = NULL, *results = NULL;
	    WP_ERR_S          wp_err;
	    CUSTOM_FILT_S    *filter = NULL;

	    if(objc != 5){
	        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
		Tcl_SetResult(interp, err, TCL_STATIC);
		return(TCL_ERROR);
	    }
	    if(Tcl_GetIntFromObj(interp, objv[2], &dir) == TCL_ERROR){
	        Tcl_SetResult(interp,
			      "PELdap results malformed: first arg must be int", 
			      TCL_VOLATILE);
		return(TCL_ERROR);
	    }
	    wpldap_global->query_no++;
	    if(wpldap_global->ldap_search_list){
	        wpldap_global->ldap_search_list =
		  free_wpldapres(wpldap_global->ldap_search_list);
	    }
	    srchstr = Tcl_GetStringFromObj(objv[3], NULL);
	    filtstr = Tcl_GetStringFromObj(objv[4], NULL);
	    if(!srchstr) return(TCL_ERROR);
	    if(!filtstr) return(TCL_ERROR);
	    if(*filtstr){
	        filter = (CUSTOM_FILT_S *)fs_get(sizeof(CUSTOM_FILT_S));
		filter->filt = cpystr(filtstr);
		filter->combine = 0;
	    }
	    memset(&wp_err, 0, sizeof(wp_err));
	    ldap_lookup_all(srchstr, dir, 0, AlwaysDisplay, filter, &winning_e,
			    &wp_err, &results);
	    if(filter){
	      fs_give((void **)&filter->filt);
	      fs_give((void **)&filter);
	    }
	    Tcl_SetResult(interp, int2string(wpldap_global->ldap_search_list
					     ? wpldap_global->query_no : 0), 
			  TCL_VOLATILE);
	    return(TCL_OK);
	}
	/* 
	 * First argument has always got to be the query number for now.
	 * Might need to rething that when setting up queries.
	 */
	if(objc == 2){
	    Tcl_WrongNumArgs(interp, 1, objv, "cmd ?args?");
	    Tcl_SetResult(interp, err, TCL_STATIC);
	    return(TCL_ERROR);
	}
	if(Tcl_GetIntFromObj(interp, objv[2], &qn) == TCL_ERROR){
	    Tcl_SetResult(interp,
			  "PELdap results malformed: first arg must be int", 
			  TCL_VOLATILE);
	    return(TCL_ERROR);
	}
	if(qn != wpldap_global->query_no){
	    Tcl_SetResult(interp,
			  "Query is no longer valid", TCL_VOLATILE);
	    return(TCL_ERROR);
	}
	if(objc == 3){
	    if(!strcmp(s1, "results")){
	      return(peLdapQueryResults(interp));
	    }
	}
	else if(objc == 4){
	    if(!strcmp(s1, "ldapext")){
	        /*
		 *  Returns a list of the form:
		 *  {"dn" {{attrib {val, ...}}, ...}}
		 */
	        char *whichrec = Tcl_GetStringFromObj(objv[3], NULL);
		char *tmpstr, *tmp, *tmp2, **vals, *a;
		WPLDAPRES_S     *curres;
		LDAP_SERV_RES_S *winning_e = NULL, *trl;
		Tcl_Obj     *secObj = NULL, *resObj = NULL, *itemObj;
		BerElement *ber;
		LDAPMessage *e;
		int i, j, whichi, whichj, curj;
		
		if(whichrec == NULL){
		  Tcl_SetResult(interp, "Ldap ldapext error 1", TCL_VOLATILE);
		  return TCL_ERROR;
		}
		tmpstr = cpystr(whichrec);
		tmp = tmpstr;
		for(tmp2 = tmp; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		if(*tmp2 != '.'){
		  Tcl_SetResult(interp, "Ldap ldapext error 2", TCL_VOLATILE); 
		  return TCL_ERROR;
		}
		*tmp2 = '\0';
		whichi = atoi(tmp);
		*tmp2 = '.';
		tmp2++;
		for(tmp = tmp2; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		if(*tmp2 != '\0'){
		  Tcl_SetResult(interp, "Ldap ldapext error 3", TCL_VOLATILE);
		  return TCL_ERROR;
		}
		whichj = atoi(tmp);
		fs_give((void **)&tmpstr);
		for(curres = wpldap_global->ldap_search_list, i = 0;
		    i < whichi && curres; i++, curres = curres->next);
		if(!curres){
		  Tcl_SetResult(interp, "Ldap ldapext error 4", TCL_VOLATILE);
		  return TCL_ERROR;
		}
		for(trl = curres->reslist, j = 0; trl; trl = trl->next){
		    for(e = ldap_first_entry(trl->ld, trl->res);
			e != NULL && j < whichj;
			e = ldap_next_entry(trl->ld, e), j++);
		    if(e != NULL && j == whichj)
		      break;
		}
		if(e == NULL || trl == NULL){
		  Tcl_SetResult(interp, "Ldap ldapext error 5", TCL_VOLATILE);
		  return TCL_ERROR;
		}
		winning_e = (LDAP_SERV_RES_S *)fs_get(sizeof(LDAP_SERV_RES_S));
		winning_e->ld        = trl->ld;
		winning_e->res       = e;
		winning_e->info_used = trl->info_used;
		winning_e->serv      = trl->serv;
		winning_e->next      = NULL;
		a = ldap_get_dn(winning_e->ld, winning_e->res);
		if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), 
					    Tcl_NewStringObj(a ? a : "", -1)) != TCL_OK)
		  return(TCL_ERROR);
		if(a)
		  our_ldap_dn_memfree(a);
		
		itemObj = Tcl_NewListObj(0, NULL);
		for(a = ldap_first_attribute(winning_e->ld, winning_e->res, &ber);
		    a != NULL;
		    a = ldap_next_attribute(winning_e->ld, winning_e->res, ber)){
		    if(a && *a){
		        secObj = Tcl_NewListObj(0, NULL);
			if(Tcl_ListObjAppendElement(interp, secObj,
						    Tcl_NewStringObj(ldap_translate(a, 
						  winning_e->info_used), -1)) != TCL_OK)
			  return(TCL_ERROR);
			resObj = Tcl_NewListObj(0, NULL);
			vals = ldap_get_values(winning_e->ld, winning_e->res, a);
			if(vals){
			    for(i = 0; vals[i]; i++){
			        if(Tcl_ListObjAppendElement(interp, resObj,
					  Tcl_NewStringObj(vals[i], -1)) != TCL_OK)
				  return(TCL_ERROR);
			    }
			    ldap_value_free(vals);
			    if(Tcl_ListObjAppendElement(interp, secObj, resObj) != TCL_OK)
			      return(TCL_ERROR);
			}
			if(!strcmp(a,"objectclass")){
			    if(Tcl_ListObjAppendElement(interp, secObj,
				       Tcl_NewStringObj("objectclass", -1)) != TCL_OK)
			      return(TCL_ERROR);
			}
			if(Tcl_ListObjAppendElement(interp, itemObj, secObj) != TCL_OK)
			  return(TCL_ERROR);
		    }
		    our_ldap_memfree(a);
		}

		if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), 
					    itemObj) != TCL_OK)
		  return(TCL_ERROR);

		fs_give((void **)&winning_e);
		return(TCL_OK);
	    }
	}
	else if(objc == 6){
	  if(!strcmp(s1, "setaddrs")){
	      char            *listset = Tcl_GetStringFromObj(objv[3], NULL);
	      char            *addrstr = Tcl_GetStringFromObj(objv[4], NULL);
	      char            *tmp, *tmp2, *tmplistset, was_char, *ret_to,
		              *tmpaddrstr;
	      int            **lset, noreplace = 0;
	      ADDRESS         *adr = NULL, *curadr, *prevadr, *newadr,
		              *curnewadr, *newadrs;
	      int              curi, i, j, numsrchs, numset, setit;
	      LDAP_SERV_RES_S *tres, *trl;
	      WPLDAPRES_S     *curres;
	      LDAPMessage     *e;
	      RFC822BUFFER     rbuf;
	      size_t           len;
	      
	      if(Tcl_GetIntFromObj(interp, objv[5], &noreplace) == TCL_ERROR){
		  Tcl_SetResult(interp,
				"PELdap results malformed: first arg must be int", 
				TCL_VOLATILE);
		  return(TCL_ERROR);
	      }
	      if(listset == NULL || addrstr == NULL) return TCL_ERROR;
	      tmpaddrstr = cpystr(addrstr);
	      
	      if(!noreplace){
		  mail_parameters(NIL, SET_PARSEPHRASE, (void *)massage_phrase_addr);
		  rfc822_parse_adrlist(&adr, tmpaddrstr, "@");
		  mail_parameters(NIL, SET_PARSEPHRASE, NULL);
	      }
	      
	      tmplistset = cpystr(listset);
	      for(curres = wpldap_global->ldap_search_list, numsrchs = 0;
		  curres; curres = curres->next, numsrchs++);
	      lset = (int **)fs_get((numsrchs+1)*sizeof(int *));
	      for(i = 0; i < numsrchs; i++){
		  for(tmp = tmplistset, numset = 0; *tmp;){
		      for(tmp2 = tmp; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		      if(*tmp2 != '.'){
			Tcl_SetResult(interp, "Ldap error 1", TCL_VOLATILE);
			return TCL_ERROR;
		      }
		      if(atoi(tmp) ==  i) numset++;
		      tmp2++;
		      for(tmp = tmp2; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		      if(*tmp2 != ',' && *tmp2 != '\0'){
			Tcl_SetResult(interp, "Ldap error 2", TCL_VOLATILE);
			return TCL_ERROR;
		      }
		      if(*tmp2) tmp2++;
		      tmp = tmp2;
		  }
		  lset[i] = (int *)fs_get((numset+1)*sizeof(int));
		  for(tmp = tmplistset, j = 0; *tmp && j < numset;){
		      setit = 0;
		      for(tmp2 = tmp; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		      if(*tmp2 != '.'){
			Tcl_SetResult(interp, "Ldap error 3", TCL_VOLATILE); 
			return TCL_ERROR;
		      }
		      *tmp2 = '\0';
		      if(atoi(tmp) ==  i) setit++;
		      *tmp2 = '.';
		      tmp2++;
		      for(tmp = tmp2; *tmp2 >= '0' && *tmp2 <= '9'; tmp2++);
		      if(*tmp2 != ',' && *tmp2 != '\0'){
			Tcl_SetResult(interp, "Ldap error 4", TCL_VOLATILE);
			return TCL_ERROR;
		      }
		      if(setit){
			  was_char = *tmp2;
			  *tmp2 = '\0';
			  lset[i][j++] = atoi(tmp);				
			  *tmp2 = was_char;
		      }
		      if(*tmp2) tmp2++;
		      tmp = tmp2;
		  }
		  lset[i][j] = -1;
	      }
	      lset[i] = NULL;
	      for(i = 0, curres = wpldap_global->ldap_search_list; 
		  i < numsrchs && curres; i++, curres = curres->next){
		  prevadr = NULL;
		  for(curadr = adr; curadr; curadr = curadr->next){
		      if(strcmp(curadr->mailbox, curres->str) == 0
			 && curadr->host && *curadr->host == '@')
			break;
		      prevadr = curadr;
		  }
		  if(!curadr && !noreplace){
		    Tcl_SetResult(interp, "Ldap error 5", TCL_VOLATILE);
		    return TCL_ERROR;
		  }
		  newadrs = newadr = curnewadr = NULL;
		  for(trl = curres->reslist, j = 0, curi = 0; trl; trl = trl->next){
		  for(e = ldap_first_entry(trl->ld, trl->res);
		      e != NULL && lset[i][curi] != -1;
		      e = ldap_next_entry(trl->ld, e), j++){
		      if(j == lset[i][curi]){
			  tres = (LDAP_SERV_RES_S *)fs_get(sizeof(LDAP_SERV_RES_S));
			  tres->ld        = trl->ld;
			  tres->res       = e;
			  tres->info_used = trl->info_used;
			  tres->serv      = trl->serv;
			  tres->next      = NULL;
			  newadr = address_from_ldap(tres);
			  fs_give((void **)&tres);

			  if(newadrs == NULL){
			    newadrs = curnewadr = newadr;
			  }
			  else {
			    curnewadr->next = newadr;
			    curnewadr = newadr;
			  }
			  curi++;
		      }
		  }
		  }
		  if(newadrs == NULL || curnewadr == NULL){
		      // Tcl_SetResult(interp, "Ldap error 6", TCL_VOLATILE);
		      // return TCL_ERROR;
		      // now we just plug the unchosen handle back into the list
		      snprintf(tmp_20k_buf, SIZEOF_20KBUF, "No Result Selected for \"%s\"", curadr->mailbox ? curadr->mailbox : "noname");
		      q_status_message(SM_ORDER, 0, 3, tmp_20k_buf);
		      newadr = copyaddr(curadr);
		      if(newadrs == NULL){
			  newadrs = curnewadr = newadr;
		      }
		      else {
			  curnewadr->next = newadr;
			  curnewadr = newadr;
		      }
		  }
		  curnewadr->next = curadr ? curadr->next : NULL;
		  if(curadr) curadr->next = NULL;
		  if(curadr == adr)
		    adr = newadrs;
		  else{
		    prevadr->next = newadrs;
		    if(curadr)
		      mail_free_address(&curadr);
		  }
	      }
	      
	      len = est_size(adr);
	      ret_to = (char *)fs_get(len * sizeof(char));
	      ret_to[0] = '\0';
	      strip_personal_quotes(adr);
	      rbuf.f   = dummy_soutr;
	      rbuf.s   = NULL;
	      rbuf.beg = ret_to;
	      rbuf.cur = ret_to;
	      rbuf.end = ret_to+len-1;
	      rfc822_output_address_list(&rbuf, adr, 0L, NULL);
	      *rbuf.cur = '\0';
	      Tcl_SetResult(interp, ret_to, TCL_VOLATILE);
	      fs_give((void **)&ret_to);
	      fs_give((void **)&tmpaddrstr);
	      fs_give((void **)&tmplistset);
	      for(i = 0; lset[i]; i++)
		fs_give((void **)&lset[i]);
	      fs_give((void **)&lset);
	      if(adr)
		mail_free_address(&adr);
	      
	      return(TCL_OK);
	  }
	}
    }
#endif /* ENABLE_LDAP */
    Tcl_SetResult(interp, err, TCL_STATIC);
    return(TCL_ERROR);
}


#ifdef ENABLE_LDAP
int
peLdapQueryResults(Tcl_Interp *interp)
{
    WPLDAPRES_S     *tsl;
    Tcl_Obj         *secObj = NULL, *resObj = NULL, *itemObj;
    LDAPMessage     *e;
    LDAP_SERV_RES_S *trl;
    /* returned list will be of the form:
     *
     * {
     *  {search-string
     *   {name, {title, ...}, {unit, ...}, 
     *     {org, ...}, {email, ...}},
     *    ...
     *  },
     *  ...
     * }
     */

    for(tsl = wpldap_global->ldap_search_list;
	tsl; tsl = tsl->next){
        secObj = Tcl_NewListObj(0, NULL);
	if(Tcl_ListObjAppendElement(interp, secObj,
		  Tcl_NewStringObj(tsl->str ? tsl->str 
				   : "", -1)) != TCL_OK)
	    return(TCL_ERROR);
        resObj = Tcl_NewListObj(0, NULL);
	for(trl = tsl->reslist; trl; trl = trl->next){
	for(e = ldap_first_entry(trl->ld, trl->res);
	    e != NULL;
	    e = ldap_next_entry(trl->ld, e)){
	    char       *dn, *a;
	    char      **cn, **org, **unit, **title, **mail, **sn;
	    BerElement *ber;
	    int         indent, have_mail;
	    
	    dn = NULL;
	    cn = org = title = unit = mail = sn = NULL;
	    
	    itemObj = Tcl_NewListObj(0, NULL);
	    peLdapEntryParse(trl, e, &cn, &org, &unit, &title,
			     &mail, &sn);
	    if(cn){
	        if(Tcl_ListObjAppendElement(interp, itemObj,
			Tcl_NewStringObj(cn[0], -1)) != TCL_OK)
		    return(TCL_ERROR);
		ldap_value_free(cn);
	    }
	    else if(sn){
	        if(Tcl_ListObjAppendElement(interp, itemObj,
		        Tcl_NewStringObj(sn[0], -1)) != TCL_OK)
		    return(TCL_ERROR);
		ldap_value_free(sn);
	    }
	    else{
		dn = ldap_get_dn(trl->ld, e);

		if(dn && !dn[0]){
		    our_ldap_dn_memfree(dn);
		    dn = NULL;
		}

	        if(Tcl_ListObjAppendElement(interp, itemObj,
		        Tcl_NewStringObj(dn ? dn : "", -1)) != TCL_OK)
		    return(TCL_ERROR);

		if(dn)
		  our_ldap_dn_memfree(dn);
	    }
	    if(peLdapStrlist(interp, itemObj, title) == TCL_ERROR)
	      return(TCL_ERROR);
	    if(peLdapStrlist(interp, itemObj, unit) == TCL_ERROR)
	      return(TCL_ERROR);
	    if(peLdapStrlist(interp, itemObj, org) == TCL_ERROR)
	      return(TCL_ERROR);
	    if(peLdapStrlist(interp, itemObj, mail) == TCL_ERROR)
	      return(TCL_ERROR);
	    if(Tcl_ListObjAppendElement(interp, resObj, itemObj) != TCL_OK)
	      return(TCL_ERROR);
	    if(title)
	      ldap_value_free(title);
	    if(unit)
	      ldap_value_free(unit);
	    if(org)
	      ldap_value_free(org);
	    if(mail)
	      ldap_value_free(mail);	    
	}
	}
	if(Tcl_ListObjAppendElement(interp, secObj, resObj) != TCL_OK)
	    return(TCL_ERROR);
	if(Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), 
				    secObj) != TCL_OK)
	    return(TCL_ERROR);
    }
    return(TCL_OK);
}

int
peLdapStrlist(Tcl_Interp *interp, Tcl_Obj *itemObj, char **strl)
{
    Tcl_Obj *strlObj;
    int i;

    strlObj = Tcl_NewListObj(0, NULL);
    if(strl){
        for(i = 0; strl[i] && strl[i][0]; i++){
	    if(Tcl_ListObjAppendElement(interp, strlObj,
		       Tcl_NewStringObj(strl[i], -1)) != TCL_OK)
	        return(TCL_ERROR);		    
	}
    }
    if(Tcl_ListObjAppendElement(interp, itemObj, strlObj) != TCL_OK)
      return(TCL_ERROR);
    return(TCL_OK);
}


int
init_ldap_pname(struct pine *ps)
{
    if(!ps_global->VAR_PERSONAL_NAME 
       || ps_global->VAR_PERSONAL_NAME[0] == '\0'){
        char *pname;
	struct variable  *vtmp;
	
	if(ps->maildomain && *ps->maildomain
	   && ps->VAR_USER_ID && *ps->VAR_USER_ID){
	    pname = peLdapPname(ps->VAR_USER_ID, ps->maildomain);
	    if(pname){
	        vtmp = &ps->vars[V_PERSONAL_NAME];
		if((vtmp->fixed_val.p && vtmp->fixed_val.p[0] == '\0')
		|| (vtmp->is_fixed && !vtmp->fixed_val.p)){
		    if(vtmp->fixed_val.p)
		      fs_give((void **)&vtmp->fixed_val.p);
		    vtmp->fixed_val.p = cpystr(pname);
		}
		else {
		  if(vtmp->global_val.p)
		    fs_give((void **)&vtmp->global_val.p);
		  vtmp->global_val.p = cpystr(pname);
		}
		fs_give((void **)&pname);
		set_current_val(vtmp, FALSE, FALSE);
	    }
	}
    }
    return 0;
}
#endif /* ENABLE_LDAP */

/*
 * Note: this is taken straight out of pico/composer.c
 *
 * strqchr - returns pointer to first non-quote-enclosed occurance of ch in 
 *           the given string.  otherwise NULL.
 *      s -- the string
 *     ch -- the character we're looking for
 *      q -- q tells us if we start out inside quotes on entry and is set
 *           correctly on exit.
 *      m -- max characters we'll check for ch (set to -1 for no check)
 */
char *
strqchr(char *s, int ch, int *q, int m)
{
    int	 quoted = (q) ? *q : 0;

    for(; s && *s && m != 0; s++, m--){
	if(*s == '"'){
	    quoted = !quoted;
	    if(q)
	      *q = quoted;
	}

	if(!quoted && *s == ch)
	  return(s);
    }

    return(NULL);
}


Tcl_Obj *
wp_prune_folders(CONTEXT_S *ctxt, char *fcc, int cur_month, char *type, unsigned pr, int *ok, int moved_fldrs, Tcl_Interp *interp)
{
    Tcl_Obj *resObj = NULL, *secObj = NULL;
    char     path2[MAXPATH+1], tmp[21];
    int      exists, month_to_use;
    struct sm_folder *mail_list, *sm;

    mail_list = get_mail_list(ctxt, fcc);

    for(sm = mail_list; sm != NULL && sm->name && sm->name[0] != '\0'; sm++)
      if(sm->month_num == cur_month - 1)
        break;  /* matched a month */
 
    month_to_use = (sm == NULL || sm->name[0] == '\0') ? cur_month - 1 : 0;

    if(!(month_to_use == 0 || pr == PRUNE_NO_AND_ASK || pr == PRUNE_NO_AND_NO)){
	strncpy(path2, fcc, sizeof(path2)-1);
	path2[sizeof(path2)-1] = '\0';
	strncpy(tmp, month_abbrev((month_to_use % 12)+1), sizeof(tmp)-1);
	tmp[sizeof(tmp)-1] = '\0';
	lcase(tmp);
	snprintf(path2 + strlen(path2), sizeof(path2)-strlen(path2), "-%.20s-%d", tmp, month_to_use/12);

	if((exists = folder_exists(ctxt, fcc)) == FEX_ERROR){
	    (*ok) = 0;
	    return(NULL);
	}
	else if(exists & FEX_ISFILE){
	    if(pr == PRUNE_YES_AND_ASK || pr == PRUNE_YES_AND_NO && !moved_fldrs){
	        prune_move_folder(fcc, path2, ctxt);
	    } else {
	        resObj = Tcl_NewListObj(0, NULL);
		Tcl_ListObjAppendElement(interp, resObj, Tcl_NewStringObj(type, -1));
		secObj = Tcl_NewListObj(0, NULL);
		Tcl_ListObjAppendElement(interp, secObj, Tcl_NewStringObj(fcc, -1));
		Tcl_ListObjAppendElement(interp, secObj, Tcl_NewStringObj(path2, -1));
		Tcl_ListObjAppendElement(interp, resObj, secObj);
	    }
	}
    }
    if(pr == PRUNE_ASK_AND_ASK || pr == PRUNE_YES_AND_ASK
       || pr == PRUNE_NO_AND_ASK){
        sm = mail_list;
        if(!resObj && sm && sm->name && sm->name[0] != '\0'){
	    resObj = Tcl_NewListObj(0, NULL);
	    Tcl_ListObjAppendElement(interp, resObj, Tcl_NewStringObj(type, -1));
	    Tcl_ListObjAppendElement(interp, resObj, Tcl_NewListObj(0, NULL));
	}
	if(resObj)
	    secObj = Tcl_NewListObj(0, NULL);
        for(sm = mail_list; sm != NULL && sm->name && sm->name[0] != '\0'; sm++)
	    Tcl_ListObjAppendElement(interp, secObj, Tcl_NewStringObj(sm->name, -1));
	if(resObj)
	  Tcl_ListObjAppendElement(interp, resObj, secObj);
    } else if(resObj)
      Tcl_ListObjAppendElement(interp, resObj, Tcl_NewListObj(0, NULL));

    free_folder_list(ctxt);

    if(sm = mail_list){
	while(sm->name){
	    fs_give((void **)&(sm->name));
	    sm++;
	}

        fs_give((void **)&mail_list);
    }

    return(resObj);
}


int
hex_colorstr(char *hexcolor, char *str)
{
    char *tstr, *p, *p2, tbuf[256];
    int i;

    strcpy(hexcolor, "000000");
    tstr = color_to_asciirgb(str);
    p = tstr;
    p2 = strindex(p, ',');
    if(p2 == NULL) return 0;
    strncpy(tbuf, p, min(50, p2-p));
    i = atoi(tbuf);
    sprintf(hexcolor, "%2.2x", i);
    p = p2+1;
    p2 = strindex(p, ',');
    if(p2 == NULL) return 0;
    strncpy(tbuf, p, min(50, p2-p));
    i = atoi(tbuf);
    sprintf(hexcolor+2, "%2.2x", i);
    p = p2+1;
    strncpy(tbuf, p, 50);
    i = atoi(tbuf);
    sprintf(hexcolor+4, "%2.2x", i);

    return 0;
}

int
hexval(char ch)
{
    if(ch >= '0' && ch <= '9')
      return (ch - '0');
    else if (ch >= 'A' && ch <= 'F')
      return (10 + (ch - 'A'));
    else if (ch >= 'a' && ch <= 'f')
      return (10 + (ch - 'a'));
    return -1;
}

int
ascii_colorstr(char *acolor, char *hexcolor)
{
    int i, hv;

    if(strlen(hexcolor) > 6) return 1;
    /* red value */
    if((hv = hexval(hexcolor[0])) == -1) return 1;
    i = 16 * hv;
    if((hv = hexval(hexcolor[1])) == -1) return 1;
    i += hv;
    sprintf(acolor, "%3.3d,", i);
    /* green value */
    if((hv = hexval(hexcolor[2])) == -1) return 1;
    i = 16 * hv;
    if((hv = hexval(hexcolor[3])) == -1) return 1;
    i += hv;
    sprintf(acolor+4, "%3.3d,", i);
    /* blue value */
    if((hv = hexval(hexcolor[4])) == -1) return 1;
    i = 16 * hv;
    if((hv = hexval(hexcolor[5])) == -1) return 1;
    i += hv;
    sprintf(acolor+8, "%3.3d", i);

    return 0;
}


char *
peRandomString(char *b, int l, int f)
{
    static unsigned char *kb = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char		 *s = b;
    int			  j;
    long		  n;

    while(1){
	n = random();
	for(j = 0; j < ((sizeof(long) * 8) / 5); j++){
	    if(l-- <= 0){
		*s = '\0';
		return(b);
	    }

	    switch(f){
	      case PRS_LOWER_CASE :
		*s++ = tolower(kb[(n & 0x1F)]);
		break;

	      case PRS_MIXED_CASE :
		if(random() % 2){
		    *s++ = tolower(kb[(n & 0x1F)]);
		    break;
		}

	      default :
		*s++ = kb[(n & 0x1F)];
		break;
	    }

	    n = n >> 5;
	}
    }
}


long
peAppendMsg(MAILSTREAM *stream, void *data, char **flags, char **date, STRING **message)
{
  char *t,*t1,tmp[MAILTMPLEN];
  unsigned long u;
  MESSAGECACHE *elt;
  APPEND_PKG *ap = (APPEND_PKG *) data;
  *flags = *date = NIL;		/* assume no flags or date */
  if (ap->flags) fs_give ((void **) &ap->flags);
  if (ap->date) fs_give ((void **) &ap->date);
  mail_gc (ap->stream,GC_TEXTS);
  if (++ap->msgno <= ap->msgmax) {
				/* initialize flag string */
    memset (t = tmp,0,MAILTMPLEN);
				/* output system flags */
    if ((elt = mail_elt (ap->stream,ap->msgno))->seen) {strncat (t," \\Seen", sizeof(tmp)-(t-tmp)); tmp[sizeof(tmp)-1] = '\0';}
    if (elt->deleted) {strncat (t," \\Deleted", sizeof(tmp)-(t-tmp)); tmp[sizeof(tmp)-1] = '\0';}
    if (elt->flagged) {strncat (t," \\Flagged", sizeof(tmp)-(t-tmp)); tmp[sizeof(tmp)-1] = '\0';}
    if (elt->answered) {strncat (t," \\Answered", sizeof(tmp)-(t-tmp)); tmp[sizeof(tmp)-1] = '\0';}
    if (elt->draft) {strncat (t," \\Draft", sizeof(tmp)-(t-tmp)); tmp[sizeof(tmp)-1] = '\0';}
    if (u = elt->user_flags) do	/* any user flags? */
      if ((MAILTMPLEN - ((t += strlen (t)) - tmp)) > (long)
	  (2 + strlen
	   (t1 = ap->stream->user_flags[find_rightmost_bit (&u)]))) {
	if(t-tmp < sizeof(tmp))
	  *t++ = ' ';		/* space delimiter */
	strncpy (t,t1,sizeof(tmp)-(t-tmp));	/* copy the user flag */
      }
    while (u);			/* until no more user flags */
    tmp[sizeof(tmp)-1] = '\0';
    *flags = ap->flags = cpystr (tmp + 1);
    *date = ap->date = cpystr (mail_date (tmp,elt));
    *message = ap->message;	/* message stringstruct */
    INIT (ap->message,mstring,(void *) ap,elt->rfc822_size);
  }
  else *message = NIL;		/* all done */
  return LONGT;
}


/* Initialize file string structure for file stringstruct
* Accepts: string structure
 *	    pointer to message data structure
 *	    size of string
 */

void
ms_init(STRING *s, void *data, unsigned long size)
{
  APPEND_PKG *md = (APPEND_PKG *) data;
  s->data = data;		/* note stream/msgno and header length */
  mail_fetchheader_full (md->stream,md->msgno,NIL,&s->data1,FT_PREFETCHTEXT);
  mail_fetchtext_full (md->stream,md->msgno,&s->size,NIL);
  s->size += s->data1;		/* header + body size */
  SETPOS (s,0);
}


/* Get next character from file stringstruct
 * Accepts: string structure
 * Returns: character, string structure chunk refreshed
 */
char
ms_next(STRING *s)
{
  char c = *s->curpos++;	/* get next byte */
  SETPOS (s,GETPOS (s));	/* move to next chunk */
  return c;			/* return the byte */
}


/* Set string pointer position for file stringstruct
 * Accepts: string structure
 *	    new position
 */
void
ms_setpos(STRING *s, unsigned long i)
{
  APPEND_PKG *md = (APPEND_PKG *) s->data;
  if (i < s->data1) {		/* want header? */
    s->chunk = mail_fetchheader (md->stream,md->msgno);
    s->chunksize = s->data1;	/* header length */
    s->offset = 0;		/* offset is start of message */
  }
  else if (i < s->size) {	/* want body */
    s->chunk = mail_fetchtext (md->stream,md->msgno);
    s->chunksize = s->size - s->data1;
    s->offset = s->data1;	/* offset is end of header */
  }
  else {			/* off end of message */
    s->chunk = NIL;		/* make sure that we crack on this then */
    s->chunksize = 1;		/* make sure SNX cracks the right way... */
    s->offset = i;
  }
				/* initial position and size */
  s->curpos = s->chunk + (i -= s->offset);
  s->cursize = s->chunksize - i;
}
