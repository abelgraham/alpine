#if !defined(lint) && !defined(DOS)
static char rcsid[] = "$Id: keyword.c 203 2006-10-26 17:23:46Z hubert@u.washington.edu $";
#endif

/*
 * ========================================================================
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

#include "../pith/headers.h"
#include "../pith/keyword.h"
#include "../pith/state.h"
#include "../pith/flag.h"
#include "../pith/string.h"
#include "../pith/status.h"
#include "../pith/util.h"


/*
 * Internal prototypes
 */


/*
 * Read the keywords array into a KEYWORD_S structure.
 * Make sure that all of the strings are UTF-8.
 */
KEYWORD_S *
init_keyword_list(char **keywordarray)
{
    char     **t, *nickname, *keyword;
    KEYWORD_S *head = NULL, *new, *kl = NULL;

    for(t = keywordarray; t && *t && **t; t++){
	nickname = keyword = NULL;
	get_pair(*t, &nickname, &keyword, 0, 0);
	new = new_keyword_s(keyword, nickname);
	if(keyword)
	  fs_give((void **) &keyword);

	if(nickname)
	  fs_give((void **) &nickname);

	if(kl)
	  kl->next = new;

	kl = new;

	if(!head)
	  head = kl;
    }

    return(head);
}


KEYWORD_S *
new_keyword_s(char *keyword, char *nickname)
{
    KEYWORD_S *kw = NULL;

    kw = (KEYWORD_S *) fs_get(sizeof(*kw));
    memset(kw, 0, sizeof(*kw));

    if(keyword && *keyword)
      kw->kw = cpystr(keyword);

    if(nickname && *nickname)
      kw->nick = cpystr(nickname);
    
    return(kw);
}


void
free_keyword_list(KEYWORD_S **kl)
{
    if(kl && *kl){
	if((*kl)->next)
	  free_keyword_list(&(*kl)->next);

	if((*kl)->kw)
	  fs_give((void **) &(*kl)->kw);

	if((*kl)->nick)
	  fs_give((void **) &(*kl)->nick);

	fs_give((void **) kl);
    }
}


/*
 * Return a pointer to the keyword associated with a nickname, or the
 * input itself if no match.
 */
char *
nick_to_keyword(char *nick)
{
    KEYWORD_S *kw;
    char      *ret;

    ret = nick;
    for(kw = ps_global->keywords; kw; kw = kw->next)
      if(!strcmp(nick, kw->nick ? kw->nick : kw->kw ? kw->kw : "")){
	  if(kw->nick)
	    ret = kw->kw;

	  break;
      }
    
    return(ret);
}


/*
 * Return a pointer to the nickname associated with a keyword, or the
 * input itself if no match.
 */
char *
keyword_to_nick(char *keyword)
{
    KEYWORD_S *kw;
    char      *ret;

    ret = keyword;
    for(kw = ps_global->keywords; kw; kw = kw->next)
      if(!strcmp(keyword, kw->kw ? kw->kw : "")){
	  if(kw->nick)
	    ret = kw->nick;

	  break;
      }
    
    return(ret);
}


int
user_flag_is_set(MAILSTREAM *stream, long unsigned int rawno, char *keyword)
{
    int           j, is_set = 0;
    MESSAGECACHE *mc;

    if(stream){
	if(rawno > 0L && stream
	   && rawno <= stream->nmsgs
	   && (mc = mail_elt(stream, rawno)) != NULL){
	    j = user_flag_index(stream, keyword);
	    if(j >= 0 && j < NUSERFLAGS && ((1 << j) & mc->user_flags))
	      is_set++;
	}
    }
	
    return(is_set);
}


/*
 * Returns the bit position of the keyword in stream, else -1.
 */
int
user_flag_index(MAILSTREAM *stream, char *keyword)
{
    int i, retval = -1;

    if(stream && keyword){
	for(i = 0; i < NUSERFLAGS; i++)
	  if(stream->user_flags[i] && !strucmp(keyword, stream->user_flags[i])){
	      retval = i;
	      break;
	  }
    }

    return(retval);
}


/*----------------------------------------------------------------------
  Build flags string based on requested flags and what's set in messagecache

   Args: mc -- message cache element to dig the flags out of
	 flags -- flags to test
	 flagbuf -- place to write string representation of bits

 Result: flags represented in bits and mask written in flagbuf
 ----*/
void
flag_string(MESSAGECACHE *mc, long int flags, char *flagbuf, size_t flagbuflen)
{
    char *p;

    if(flagbuflen > 0)
      *(p = flagbuf) = '\0';

    if(!mc)
      return;

    if((flags & F_DEL) && mc->deleted)
      sstrncpy(&p, "\\DELETED ", flagbuflen-(p-flagbuf));

    if((flags & F_ANS) && mc->answered)
      sstrncpy(&p, "\\ANSWERED ", flagbuflen-(p-flagbuf));

    if((flags & F_FLAG) && mc->flagged)
      sstrncpy(&p, "\\FLAGGED ", flagbuflen-(p-flagbuf));

    if((flags & F_SEEN) && mc->seen)
      sstrncpy(&p, "\\SEEN ", flagbuflen-(p-flagbuf));

    if(p != flagbuf && (flagbuflen-(p-flagbuf)>0))
      *--p = '\0';
}


/*
 * Find the last message in dstn_stream's folder that has message_id
 * equal to the argument. Set its keywords equal to the keywords which
 * are set in message mc from stream kw_stream.
 *
 * If you just saved the message you're looking for, it is a good idea
 * to send a ping over before you call this routine.
 *
 * Args:  kw_stream -- stream containing message mc
 *            mcsrc -- mail_elt for the source message
 *      dstn_stream -- where the new message is
 *       message_id -- the message id of the new message
 *            guess -- this is a positive integer, for example, 10. If it
 *                       is set we will first try to find the message_id
 *                       within the last "guess" messages in the folder,
 *                       unless the whole folder isn't much bigger than that
 */
void
set_keywords_in_msgid_msg(MAILSTREAM *kw_stream, MESSAGECACHE *mcsrc,
			  MAILSTREAM *dstn_stream, char *message_id, long int guess)
{
    SEARCHPGM *pgm = NULL;
    long        newmsgno;
    int         iter = 0, k;
    MESSAGECACHE *mc;
    extern MAILSTREAM *mm_search_stream;
    extern long        mm_search_count;

    if(!(kw_stream && dstn_stream))
      return;

    mm_search_count = 0L;
    mm_search_stream = dstn_stream;
    while(mm_search_count == 0L && iter++ < 2
	  && (pgm = mail_newsearchpgm()) != NULL){
	pgm->message_id = mail_newstringlist();
	pgm->message_id->text.data = (unsigned char *) cpystr(message_id);
	pgm->message_id->text.size = strlen(message_id);

	if(iter == 1){
	    /* lots of messages? restrict to last guess message on first try */
	    if(dstn_stream->nmsgs > guess + 40L){
		pgm->msgno = mail_newsearchset();
		pgm->msgno->first = dstn_stream->nmsgs - guess;
		pgm->msgno->first = MIN(MAX(pgm->msgno->first, 1),
					dstn_stream->nmsgs);
		pgm->msgno->last  = dstn_stream->nmsgs;
	    }
	    else
	      iter++;
	}

	pine_mail_search_full(dstn_stream, NULL, pgm, SE_NOPREFETCH | SE_FREE);
	if(mm_search_count){
	    for(newmsgno=dstn_stream->nmsgs; newmsgno > 0L; newmsgno--)
	      if((mc = mail_elt(dstn_stream, newmsgno)) && mc->searched)
		break;
		  
	    if(newmsgno > 0L)
	      for(k = 0; k < NUSERFLAGS; k++)
		if(mcsrc && mcsrc->user_flags & (1 << k)
		   && kw_stream->user_flags[k]
		   && kw_stream->user_flags[k][0]){
		    int i;

		    /*
		     * Check to see if we know it is impossible to set
		     * this keyword before we try.
		     */
		    if(dstn_stream->kwd_create ||
		       (((i = user_flag_index(dstn_stream,
					      kw_stream->user_flags[k])) >= 0)
		        && i < NUSERFLAGS)){
		      mail_flag(dstn_stream, long2string(newmsgno),
			        kw_stream->user_flags[k], ST_SET);
		    }
		    else{
		      int some_defined = 0, w;
		      static time_t last_status_message = 0;
		      time_t now;
		      char b[200], c[200], *p;

		      for(i = 0; !some_defined && i < NUSERFLAGS; i++)
		        if(dstn_stream->user_flags[i])
			  some_defined++;
		    
		      /*
		       * Some unusual status message handling here. We'd
		       * like to print out one status message for every
		       * keyword missed, but if that happens every time
		       * you save to a particular folder, that would get
		       * annoying. So we only print out the first for each
		       * save or aggregate save.
		       */
		      if((now=time((time_t *) 0)) - last_status_message > 3){
		        last_status_message = now;
		        if(some_defined){
			  snprintf(b, sizeof(b), "Losing keyword \"%.30s\". No more keywords allowed in ", keyword_to_nick(kw_stream->user_flags[k]));
			  w = MIN((ps_global->ttyo ? ps_global->ttyo->screen_cols : 80) - strlen(b) - 1 - 2, sizeof(c)-1);
			  p = short_str(STREAMNAME(dstn_stream), c, sizeof(c), w,
					FrontDots);
			  q_status_message2(SM_ORDER, 3, 3, "%s%s!", b, p);
			}
			else{
			  snprintf(b, sizeof(b), "Losing keyword \"%.30s\". Can't add keywords in ", keyword_to_nick(kw_stream->user_flags[k]));
			  w = MIN((ps_global->ttyo ? ps_global->ttyo->screen_cols : 80) - strlen(b) - 1 - 2, sizeof(b)-1);
			  p = short_str(STREAMNAME(dstn_stream), c, sizeof(c), w,
					FrontDots);
			  q_status_message2(SM_ORDER, 3, 3, "%s%s!", b, p);
			}
		      }

		      if(some_defined){
			dprint((1, "Losing keyword \"%s\". No more keywords allowed in %s\n", kw_stream->user_flags[k], dstn_stream->mailbox ? dstn_stream->mailbox : "target folder"));
		      }
		      else{
			dprint((1, "Losing keyword \"%s\". Can't add keywords in %s\n", kw_stream->user_flags[k], dstn_stream->mailbox ? dstn_stream->mailbox : "target folder"));
		      }
		    }
		}
	}
    }
}


long
get_msgno_by_msg_id(MAILSTREAM *stream, char *message_id, MSGNO_S *msgmap)
{
    SEARCHPGM  *pgm = NULL;
    long        hint = mn_m2raw(msgmap, mn_get_cur(msgmap));
    long        newmsgno = -1L;
    int         iter = 0, k;
    MESSAGECACHE *mc;
    extern MAILSTREAM *mm_search_stream;
    extern long        mm_search_count;

    if(!(message_id && message_id[0]))
      return(newmsgno);

    mm_search_count = 0L;
    mm_search_stream = stream;
    while(mm_search_count == 0L && iter++ < 3
	  && (pgm = mail_newsearchpgm()) != NULL){
	pgm->message_id = mail_newstringlist();
	pgm->message_id->text.data = (unsigned char *) cpystr(message_id);
	pgm->message_id->text.size = strlen(message_id);

	if(iter > 1 || hint > stream->nmsgs)
	  iter++;

	if(iter == 1){
	    /* restrict to hint message on first try */
	    pgm->msgno = mail_newsearchset();
	    pgm->msgno->first = pgm->msgno->last = hint;
	}
	else if(iter == 2){
	    /* restrict to last 50 messages on 2nd try */
	    pgm->msgno = mail_newsearchset();
	    if(stream->nmsgs > 100L)
	      pgm->msgno->first = stream->nmsgs-50L;
	    else{
		pgm->msgno->first = 1L;
		iter++;
	    }

	    pgm->msgno->last = stream->nmsgs;
	}

	pine_mail_search_full(stream, NULL, pgm, SE_NOPREFETCH | SE_FREE);

	if(mm_search_count){
	    for(newmsgno=stream->nmsgs; newmsgno > 0L; newmsgno--)
	      if((mc = mail_elt(stream, newmsgno)) && mc->searched)
		break;
	}
    }

    return(mn_raw2m(msgmap, newmsgno));
}


/*
 * These chars are not allowed in keywords.
 *
 * Returns 0 if ok, 1 if not.
 * Returns an allocated error message on error.
 */
int
keyword_check(char *kw, char **error)
{
    register char *t;
    char buf[100];

    if(!kw || !kw[0])
      return 1;

    kw = nick_to_keyword(kw);

    if((t = strindex(kw, SPACE)) ||
       (t = strindex(kw, '{'))   ||
       (t = strindex(kw, '('))   ||
       (t = strindex(kw, ')'))   ||
       (t = strindex(kw, ']'))   ||
       (t = strindex(kw, '%'))   ||
       (t = strindex(kw, '"'))   ||
       (t = strindex(kw, '\\'))  ||
       (t = strindex(kw, '*'))){
	char s[4];
	s[0] = '"';
	s[1] = *t;
	s[2] = '"';
	s[3] = '\0';
	if(error){
	    snprintf(buf, sizeof(buf), "%s not allowed in keywords",
		*t == SPACE ?
		    "Spaces" :
		    *t == '"' ?
			"Quotes" :
			*t == '%' ?
			    "Percents" :
			    s);
	    *error = cpystr(buf);
	}

	return 1;
    }

    return 0;
}
