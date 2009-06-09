/*
 * $Id: mailindx.h 136 2006-09-22 20:06:05Z hubert@u.washington.edu $
 *
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

#ifndef PITH_MAILINDX_INCLUDED
#define PITH_MAILINDX_INCLUDED


#include "../pith/indxtype.h"
#include "../pith/msgno.h"
#include "../pith/state.h"
#include "../pith/pattern.h"


extern ICE_S	       *(*format_index_line)(INDEXDATA_S *);
extern void		(*setup_header_widths)(MAILSTREAM *);


/* exported prototypes */
int	       msgline_hidden(MAILSTREAM *, MSGNO_S *, long, int);
void	       adjust_cur_to_visible(MAILSTREAM *, MSGNO_S *);
unsigned long  line_hash(char *);
void	       init_index_format(char *, INDEX_COL_S **);
void	       reset_index_format(void);
INDEX_PARSE_T *itoktype(char *, int);
char	      *prepend_keyword_subject(MAILSTREAM *, long, char *, SubjKW, IELEM_S **, char *);
int	       get_index_line_color(MAILSTREAM *, SEARCHSET *, PAT_STATE **, COLOR_PAIR **);
void	       setup_for_index_index_screen(void);
void	       setup_for_thread_index_screen(void);
INDEX_PARSE_T *itoken(int);
void	       load_overview(MAILSTREAM *, unsigned long, OVERVIEW *, unsigned long);
ICE_S	      *build_header_work(struct pine *, MAILSTREAM  *, MSGNO_S *, long, long, int, int *);
char	      *simple_index_line(char *, size_t, int, ICE_S *, long);
int	       resent_to_us(INDEXDATA_S *);
int	       parsed_resent_to_us(char *);
ADDRESS	      *fetch_cc(INDEXDATA_S *);
void	       index_data_env(INDEXDATA_S *, ENVELOPE *);
void	       date_str(char *, IndexColType, int, char *, size_t);
ADDRESS	      *fetch_to(INDEXDATA_S *);


#endif /* PITH_MAILINDX_INCLUDED */
