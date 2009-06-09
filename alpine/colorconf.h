/*
 * $Id: colorconf.h 136 2006-09-22 20:06:05Z hubert@u.washington.edu $
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

#ifndef PINE_COLORCONF_INCLUDED
#define PINE_COLORCONF_INCLUDED


#include "conftype.h"
#include "../pith/state.h"
#include "../pith/conf.h"


#define HEADER_WORD "Header "
#define KW_COLORS_HDR "KEYWORD COLORS"
#define ADDHEADER_COMMENT "[ Use the AddHeader command to add colored headers in MESSAGE VIEW ]"
#define EQ_COL 37

#define SPACE_BETWEEN_DOUBLEVARS 3
#define SAMPLE_LEADER "---------------------------"
#define SAMP1 "[Sample ]"
#define SAMP2 "[Default]"
#define SAMPEXC "[Exception]"
#define SBS 1	/* space between samples */
#define COLOR_BLOB "<    >"
#define COLOR_BLOB_LEN 6
#define COLOR_INDENT 3
#define COLORNOSET "  [ Colors below may not be set until color is turned on above ]"


/*
 * The CONF_S's varmem field serves dual purposes.  The low two bytes
 * are reserved for the pre-defined color index (though only 8 are 
 * defined for the nonce, and the high order bits are for the index
 * of the particular SPEC_COLOR_S this CONF_S is associated with.
 * Capiche?
 */
#define	CFC_ICOLOR(V)		((V)->varmem & 0xff)
#define	CFC_ICUST(V)		((V)->varmem >> 16)
#define	CFC_SET_COLOR(I, C)	(((I) << 16) | (C))
#define	CFC_ICUST_INC(V)	CFC_SET_COLOR(CFC_ICUST(V) + 1, CFC_ICOLOR(V))
#define	CFC_ICUST_DEC(V)	CFC_SET_COLOR(CFC_ICUST(V) - 1, CFC_ICOLOR(V))


extern int treat_color_vars_as_text;


/* exported protoypes */
void     color_config_screen(struct pine *, int);
int	 color_setting_tool(struct pine *, int, CONF_S **, unsigned);
char    *sampleexc_text(struct pine *, struct variable *);


#endif /* PINE_COLORCONF_INCLUDED */
