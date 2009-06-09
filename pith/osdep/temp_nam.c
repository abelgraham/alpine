#if !defined(lint) && !defined(DOS)
static char rcsid[] = "$Id: temp_nam.c 229 2006-11-13 23:14:48Z hubert@u.washington.edu $";
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

#include <system.h>

#if	HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "canaccess.h"
#include "temp_nam.h"
#include "../charconv/utf8.h"
#include "../charconv/filesys.h"


#ifdef	_WINDOWS

#define	ACCESSIBLE	(WRITE_ACCESS)
#define	PATH_SEP	"\\"

#else  /* UNIX */

#define	ACCESSIBLE	(WRITE_ACCESS|EXECUTE_ACCESS)
#define	PATH_SEP	"/"

#endif /* UNIX */


/*
 * Internal Prototypes
 */
char	*was_nonexistent_tmp_name(char *, size_t, int, char *);



/*
 * This routine is derived from BSD4.3 code,
 * Copyright (c) 1987 Regents of the University of California.
 * All rights reserved.
 */
#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)mktemp.c	5.7 (Berkeley) 6/27/88";
#endif /* LIBC_SCCS and not lint */

char *
was_nonexistent_tmp_name(char *as, size_t aslen, int flags, char *ext)
{
    register char  *start, *trv;
    struct stat     sbuf;
    unsigned        pid;
    static unsigned n = 0;
    int             fd, tries = 0;
    int             f;

    pid = ((unsigned)getpid() * 100) + n++;

    /* extra X's get set to 0's */
    for(trv = as; *trv; ++trv)
      ;

    /*
     * We should probably make the name random instead of having it
     * be the pid.
     */
    while(*--trv == 'X'){
	*trv = (pid % 10) + '0';
	pid /= 10;
    }

    /* add the extension, enough room guaranteed by caller */
    if(ext){
	strncat(as, ".", aslen);
	as[aslen-1] = '\0';
	strncat(as, ext, aslen);
	as[aslen-1] = '\0';
    }

    /*
     * Check for write permission on target directory; if you have
     * six X's and you can't write the directory, this will run for
     * a *very* long time.
     */
    for(start = ++trv; trv > as && *trv != PATH_SEP[0]; --trv)
      ;

    if(*trv == PATH_SEP[0]){
#ifdef	_WINDOWS
	char treplace;

        if((trv - as == 2) && isalpha(as[0]) && as[1] == ':')
	  trv++;
	treplace = *trv;
	*trv = '\0';
	if(our_stat(as==trv ? PATH_SEP : as, &sbuf) || !(sbuf.st_mode & S_IFDIR))
	  return((char *)NULL);

	*trv = treplace;

#else  /* UNIX */

	*trv = '\0';

	if(our_stat(as==trv ? PATH_SEP : as, &sbuf) || !(sbuf.st_mode & S_IFDIR))
	  return((char *)NULL);

	*trv = PATH_SEP[0];

#endif
    }
    else if (our_stat(".", &sbuf) == -1)
      return((char *)NULL);

    for(;;){
	/*
	 * Check with lstat to be sure we don't have
	 * a symlink. If lstat fails and no such file, then we
	 * have a winner. Otherwise, lstat shouldn't fail.
	 * If lstat succeeds, then skip it because it exists.
	 */
#ifndef _WINDOWS
	if(our_lstat(as, &sbuf)){		/* lstat failed */
	    if(errno == ENOENT){		/* no such file, success */
#endif /* !_WINDOWS */
		/*
		 * Create the file so that the
		 * evil ones don't have a chance to put something there
		 * that they can read or write before we create it
		 * ourselves.
		 */
		f = O_CREAT|O_EXCL|O_WRONLY;
		if(flags & TN_TEXT)
		  f |= (O_TEXT | _O_U8TEXT);
		else
		  f |= O_BINARY;	/* default */

		if((fd=our_open(as, f, 0600)) >= 0 && close(fd) == 0)
		  return(as);
		else if(++tries > 3)		/* open failed unexpectedly */
		  return((char *)NULL);
#ifndef _WINDOWS
	    }
	    else				/* failed for unknown reason */
	      return((char *)NULL);
	}
#endif /* !_WINDOWS */

	for(trv = start;;){
	    if(!*trv)
	      return((char *)NULL);

	    /*
	     * Change the digits from the initial values into
	     * lower case letters and try again.
	     */
	    if(*trv == 'z')
	      *trv++ = 'a';
	    else{
		if(isdigit((unsigned char)*trv))
		  *trv = 'a';
		else
		  ++*trv;

		break;
	    }
	}
    }
    /*NOTREACHED*/
}


/*
 * This routine is derived from BSD4.3 code,
 * Copyright (c) 1988 Regents of the University of California.
 * All rights reserved.
 */
#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)tmpnam.c	4.5 (Berkeley) 6/27/88";
#endif /* LIBC_SCCS and not lint */
/*----------------------------------------------------------------------
      Return a unique file name in a given directory.  This is not quite
      the same as the usual tempnam() function, though it is similar.
      We want it to use the TMPDIR/TMP/TEMP environment variable only if dir
      is NULL, instead of using it regardless if it is set.
      We also want it to be safer than tempnam().
      If we return a filename, we are saying that the file did not exist
      at the time this function was called (and it wasn't a symlink pointing
      to a file that didn't exist, either).
      If dir is NULL this is a temp file in a public directory. In that
      case we create the file with permission 0600 before returning.

  Args: dir      -- The directory to create the name in
        prefix   -- Prefix of the name
 
 Result: Malloc'd string equal to new name is returned.  It must be free'd
	 by the caller.  Returns the string on success and NULL on failure.
  ----*/
char *
temp_nam(char *dir, char *prefix, int flags)
{
    struct stat buf;
    size_t      l, ll;
    char       *f, *name;

    if(!(name = (char *)malloc((unsigned int) MAXPATH)))
        return((char *)NULL);

    if(!dir && (f = getenv("TMPDIR")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(!dir && (f = getenv("TMP")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(!dir && (f = getenv("TEMP")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(dir){
	strncpy(name, dir, MAXPATH-1);
	name[MAXPATH-1] = '\0';
#ifdef	_WINDOWS

	if(!*dir || (isalpha(*dir) && *(dir+1) == ':' && !*(dir+2))){
	    strncat(name, "\\", MAXPATH);
	    name[MAXPATH-1] = '\0';
	}

#endif /* UNIX */

      if(!our_stat(name, &buf)
	 && (buf.st_mode&S_IFMT) == S_IFDIR
	 && !can_access(name, ACCESSIBLE)){
	  strncpy(name, dir, MAXPATH-1);
	  name[MAXPATH-1] = '\0';
	  goto done;
      }
    }

#ifndef P_tmpdir
#ifdef	_WINDOWS
#define	P_tmpdir	"\\tmp"
#else  /* UNIX */
#define	P_tmpdir	"/usr/tmp"
#endif /* UNIX */
#endif
    if(!our_stat(P_tmpdir, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(P_tmpdir, ACCESSIBLE)){
	strncpy(name, P_tmpdir, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

#ifndef	_WINDOWS

    if(!our_stat("/tmp", &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access("/tmp", ACCESSIBLE)){
	strncpy(name, "/tmp", MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

#endif /* !_WINDOWS */

    free((void *)name);
    return((char *)NULL);

done:
    f = NULL;
    if(name[0] && *((f = &name[l=strlen(name)]) - 1) != PATH_SEP[0] && l+1 < MAXPATH){
	*f++ = PATH_SEP[0];
	*f = '\0';
	l++;
    }

    if(prefix && (ll = strlen(prefix)) && l+ll < MAXPATH){
	strncpy(f, prefix, MAXPATH-(f-name));
	name[MAXPATH-1] = '\0';
	f += ll;
	l += ll;
    }

    if(l+6 < MAXPATH){
	strncpy(f, "XXXXXX", MAXPATH-(f-name));
	name[MAXPATH-1] = '\0';
    }
    else{
	free((void *)name);
	return((char *)NULL);
    }

    return(was_nonexistent_tmp_name(name, MAXPATH, flags, NULL));
}

/*----------------------------------------------------------------------

     Like temp_nam but create a unique name with an extension.
 
 Result: Malloc'd string equal to new name is returned.  It must be free'd
	 by the caller.  Returns the string on success and NULL on failure.
  ----*/
char *
temp_nam_ext(char *dir, char *prefix, int flags, char *ext)
{
    struct stat buf;
    size_t      l, ll;
    char       *f, *name;

    if(ext == NULL || *ext == '\0')
      return(temp_nam(dir, prefix, flags));

    if(!(name = (char *)malloc((unsigned int)MAXPATH)))
        return((char *)NULL);

    if(!dir && (f = getenv("TMPDIR")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(!dir && (f = getenv("TMP")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(!dir && (f = getenv("TEMP")) && !our_stat(f, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(f, ACCESSIBLE)){
	strncpy(name, f, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

    if(dir){
	strncpy(name, dir, MAXPATH-1);
	name[MAXPATH-1] = '\0';

#ifdef	_WINDOWS
	if(!*dir || (isalpha(*dir) && *(dir+1) == ':' && !*(dir+2))){
	    strncat(name, PATH_SEP, MAXPATH);
	    name[MAXPATH-1] = '\0';
	}
#endif

	if(!our_stat(name, &buf)
	   && (buf.st_mode&S_IFMT) == S_IFDIR
	   && !can_access(name, ACCESSIBLE)){
	    strncpy(name, dir, MAXPATH-1);
	    name[MAXPATH-1] = '\0';
	    goto done;
	}
    }

#ifndef P_tmpdir
#ifdef	_WINDOWS
#define	P_tmpdir	"\\tmp"
#else  /* UNIX */
#define	P_tmpdir	"/usr/tmp"
#endif /* UNIX */
#endif
    if(!our_stat(P_tmpdir, &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access(P_tmpdir, ACCESSIBLE)){
	strncpy(name, P_tmpdir, MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }

#ifndef	IS_WINODOWS
    if(!our_stat("/tmp", &buf) &&
                         (buf.st_mode&S_IFMT) == S_IFDIR &&
			 !can_access("/tmp", ACCESSIBLE)){
	strncpy(name, "/tmp", MAXPATH-1);
	name[MAXPATH-1] = '\0';
        goto done;
    }
#endif

    free((void *)name);
    return((char *)NULL);

done:
    f = NULL;
    if(name[0] && *((f = &name[l=strlen(name)]) - 1) != PATH_SEP[0] && l+1 < MAXPATH){
	*f++ = PATH_SEP[0];
	*f = '\0';
	l++;
    }

    if(prefix && (ll = strlen(prefix)) && l+ll < MAXPATH){
	strncpy(f, prefix, MAXPATH-(f-name));
	name[MAXPATH-1] = '\0';
	f += ll;
	l += ll;
    }

    if(l+6+strlen(ext)+1 < MAXPATH){
	strncpy(f, "XXXXXX", MAXPATH-(f-name));
	name[MAXPATH-1] = '\0';
    }
    else{
	free((void *)name);
	return((char *)NULL);
    }

    return(was_nonexistent_tmp_name(name, MAXPATH, flags, ext));
}
