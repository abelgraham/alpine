#if !defined(lint) && !defined(DOS)
static char rcsid[] = "$Id: setup.c 173 2006-10-06 18:40:45Z hubert@u.washington.edu $";
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

#include "headers.h"
#include "setup.h"
#include "keymenu.h"
#include "status.h"
#include "confscroll.h"
#include "colorconf.h"
#include "reply.h"
#include "radio.h"
#include "listsel.h"
#include "../pith/state.h"
#include "../pith/conf.h"
#include "../pith/util.h"
#include "../pith/sort.h"
#include "../pith/folder.h"


/*
 * Internal prototypes
 */
int      inbox_path_text_tool(struct pine *, int, CONF_S **, unsigned);
int      incoming_monitoring_list_tool(struct pine *, int, CONF_S **, unsigned);
char   **adjust_list_of_monitored_incoming(CONTEXT_S *, EditWhich, int);


#define CONFIG_SCREEN_TITLE             _("SETUP CONFIGURATION")
#define CONFIG_SCREEN_TITLE_EXC         _("SETUP CONFIGURATION EXCEPTIONS")



/*----------------------------------------------------------------------
    Present pinerc data for manipulation

    Args: None

  Result: help edit certain pinerc fields.
  ---*/
void
option_screen(struct pine *ps, int edit_exceptions)
{
    char	    tmp[MAXPATH+1], *pval, **lval;
    int		    i, j, ln = 0, readonly_warning = 0;
    struct	    variable  *vtmp;
    CONF_S	   *ctmpa = NULL, *ctmpb, *first_line = NULL;
    FEATURE_S	   *feature;
    SAVED_CONFIG_S *vsave;
    OPT_SCREEN_S    screen;
    int             expose_hidden_config, add_hidden_vars_title = 0;

    dprint((3, "-- option_screen --\n"));

    expose_hidden_config = F_ON(F_EXPOSE_HIDDEN_CONFIG, ps_global);
    treat_color_vars_as_text = expose_hidden_config;

    ew = edit_exceptions ? ps_global->ew_for_except_vars : Main;

    if(ps->restricted)
      readonly_warning = 1;
    else{
	PINERC_S *prc = NULL;

	switch(ew){
	  case Main:
	    prc = ps->prc;
	    break;
	  case Post:
	    prc = ps->post_prc;
	    break;
	}

	readonly_warning = prc ? prc->readonly : 1;
	if(prc && prc->quit_to_edit){
	    quit_to_edit_msg(prc);
	    treat_color_vars_as_text = 0;
	    return;
	}
    }

    ps->next_screen = SCREEN_FUN_NULL;

    mailcap_free(); /* free resources we won't be using for a while */

    if(ps->fix_fixed_warning)
      offer_to_fix_pinerc(ps);

    /*
     * First, find longest variable name
     */
    for(vtmp = ps->vars; vtmp->name; vtmp++){
	if(exclude_config_var(ps, vtmp, expose_hidden_config))
	  continue;

	if((i = utf8_width(vtmp->name)) > ln)
	  ln = i;
    }

    dprint((9, "initialize config list\n"));

    /*
     * Next, allocate and initialize config line list...
     */
    for(vtmp = ps->vars; vtmp->name; vtmp++){
	/*
	 * INCOMING_FOLDERS is currently the first of the normally
	 * hidden variables. Should probably invent a more robust way
	 * to keep this up to date.
	 */
	if(expose_hidden_config && vtmp == &ps->vars[V_INCOMING_FOLDERS])
	  add_hidden_vars_title = 1;

	if(exclude_config_var(ps, vtmp, expose_hidden_config))
	  continue;

	if(add_hidden_vars_title){

	    add_hidden_vars_title = 0;

	    new_confline(&ctmpa);		/* Blank line */
	    ctmpa->flags	       |= CF_NOSELECT | CF_B_LINE;

	    new_confline(&ctmpa)->var	= NULL;
	    ctmpa->help			= NO_HELP;
	    ctmpa->valoffset		= 2;
	    ctmpa->flags	       |= CF_NOSELECT;
	    ctmpa->value = cpystr("--- [ Normally hidden configuration options ] ---");

	    new_confline(&ctmpa);		/* Blank line */
	    ctmpa->flags	       |= CF_NOSELECT | CF_B_LINE;
	}

	if(vtmp->is_list)
	  lval  = LVAL(vtmp, ew);
	else
	  pval  = PVAL(vtmp, ew);

	new_confline(&ctmpa)->var = vtmp;
	if(!first_line)
	  first_line = ctmpa;

	ctmpa->valoffset = ln + 3;
	if(vtmp->is_list)
	  ctmpa->keymenu	 = &config_text_wshuf_keymenu;
	else
	  ctmpa->keymenu	 = &config_text_keymenu;
	  
	ctmpa->help	 = config_help(vtmp - ps->vars, 0);
	ctmpa->tool	 = text_tool;

	utf8_snprintf(tmp, sizeof(tmp), "%-*.100w =", ln, vtmp->name);
	tmp[sizeof(tmp)-1] = '\0';
	ctmpa->varname  = cpystr(tmp);
	ctmpa->varnamep = ctmpb = ctmpa;
	ctmpa->flags   |= CF_STARTITEM;
	if(vtmp == &ps->vars[V_FEATURE_LIST]){	/* special checkbox case */
	    char *this_sect, *new_sect;

	    ctmpa->flags		 |= CF_NOSELECT;
	    ctmpa->keymenu		  = &config_checkbox_keymenu;
	    ctmpa->tool			  = NULL;

	    /* put a nice delimiter before list */
	    new_confline(&ctmpa)->var = NULL;
	    ctmpa->varnamep		  = ctmpb;
	    ctmpa->keymenu		  = &config_checkbox_keymenu;
	    ctmpa->help			  = NO_HELP;
	    ctmpa->tool			  = checkbox_tool;
	    ctmpa->valoffset		  = 12;
	    ctmpa->flags		 |= CF_NOSELECT;
	    ctmpa->value = cpystr("Set    Feature Name");

	    new_confline(&ctmpa)->var = NULL;
	    ctmpa->varnamep		  = ctmpb;
	    ctmpa->keymenu		  = &config_checkbox_keymenu;
	    ctmpa->help			  = NO_HELP;
	    ctmpa->tool			  = checkbox_tool;
	    ctmpa->valoffset		  = 12;
	    ctmpa->flags		 |= CF_NOSELECT;
	    ctmpa->value = cpystr("---  ----------------------");

	    for(i = 0, this_sect = NULL; feature = feature_list(i); i++)
	      if((new_sect = feature_list_section(feature)) &&
		 (strcmp(new_sect, HIDDEN_PREF) != 0)){
		  if(this_sect != new_sect){
		      new_confline(&ctmpa)->var = NULL;
		      ctmpa->varnamep		= ctmpb;
		      ctmpa->keymenu		= &config_checkbox_keymenu;
		      ctmpa->help		= NO_HELP;
		      ctmpa->tool		= checkbox_tool;
		      ctmpa->valoffset		= 2;
		      ctmpa->flags	       |= (CF_NOSELECT | CF_STARTITEM);
		      snprintf(tmp, sizeof(tmp), "[ %s ]", this_sect = new_sect);
		      tmp[sizeof(tmp)-1] = '\0';
		      ctmpa->value = cpystr(tmp);
		  }

		  new_confline(&ctmpa)->var = vtmp;
		  ctmpa->varnamep	    = ctmpb;
		  ctmpa->keymenu	    = &config_checkbox_keymenu;
		  ctmpa->help		    = config_help(vtmp-ps->vars,
							  feature->id);
		  ctmpa->tool		    = checkbox_tool;
		  ctmpa->valoffset	    = 12;
		  ctmpa->varmem		    = i;
		  ctmpa->value		    = pretty_value(ps, ctmpa);
	      }
	}
	else if(standard_radio_var(ps, vtmp)){
	    standard_radio_setup(ps, &ctmpa, vtmp, NULL);
	}
	else if(vtmp == &ps->vars[V_SORT_KEY]){ /* radio case */
	    SortOrder def_sort;
	    int       def_sort_rev;

	    ctmpa->flags       |= CF_NOSELECT;
	    ctmpa->keymenu      = &config_radiobutton_keymenu;
	    ctmpa->tool		= NULL;

	    /* put a nice delimiter before list */
	    new_confline(&ctmpa)->var = NULL;
	    ctmpa->varnamep		  = ctmpb;
	    ctmpa->keymenu		  = &config_radiobutton_keymenu;
	    ctmpa->help			  = NO_HELP;
	    ctmpa->tool			  = radiobutton_tool;
	    ctmpa->valoffset		  = 12;
	    ctmpa->flags		 |= CF_NOSELECT;
	    ctmpa->value = cpystr("Set    Sort Options");

	    new_confline(&ctmpa)->var = NULL;
	    ctmpa->varnamep	      = ctmpb;
	    ctmpa->keymenu	      = &config_radiobutton_keymenu;
	    ctmpa->help		      = NO_HELP;
	    ctmpa->tool		      = radiobutton_tool;
	    ctmpa->valoffset	      = 12;
	    ctmpa->flags             |= CF_NOSELECT;
	    ctmpa->value = cpystr("---  ----------------------");

	    decode_sort(pval, &def_sort, &def_sort_rev);

	    for(j = 0; j < 2; j++){
		for(i = 0; ps->sort_types[i] != EndofList; i++){
		    new_confline(&ctmpa)->var = vtmp;
		    ctmpa->varnamep	      = ctmpb;
		    ctmpa->keymenu	      = &config_radiobutton_keymenu;
		    ctmpa->help		      = config_help(vtmp - ps->vars, 0);
		    ctmpa->tool		      = radiobutton_tool;
		    ctmpa->valoffset	      = 12;
		    ctmpa->varmem	      = i + (j * EndofList);
		    ctmpa->value	      = pretty_value(ps, ctmpa);
		}
	    }
	}
	else if(vtmp == &ps->vars[V_USE_ONLY_DOMAIN_NAME]){ /* yesno case */
	    ctmpa->keymenu = &config_yesno_keymenu;
	    ctmpa->tool	   = yesno_tool;
	    ctmpa->value   = pretty_value(ps, ctmpa);
	}
	else if(vtmp == &ps->vars[V_LITERAL_SIG]){
	    ctmpa->tool    = litsig_text_tool;
	    ctmpa->value   = pretty_value(ps, ctmpa);
	}
	else if(vtmp == &ps->vars[V_INBOX_PATH]){
	    ctmpa->tool    = inbox_path_text_tool;
	    ctmpa->value   = pretty_value(ps, ctmpa);
	}
	else if(vtmp->is_list){
	    int (*t_tool)(struct pine *, int, CONF_S **, unsigned);
	    struct key_menu *km;

	    t_tool = NULL;
	    km = NULL;
	    if(vtmp == &ps->vars[V_INCCHECKLIST]){
		t_tool = incoming_monitoring_list_tool;
		km     = &config_text_keymenu;
	    }

	    if(lval){
		for(i = 0; lval[i]; i++){
		    if(i)
		      (void)new_confline(&ctmpa);

		    ctmpa->var       = vtmp;
		    ctmpa->varmem    = i;
		    ctmpa->valoffset = ln + 3;
		    ctmpa->value     = pretty_value(ps, ctmpa);
		    ctmpa->keymenu   = km ? km : &config_text_wshuf_keymenu;
		    ctmpa->help      = config_help(vtmp - ps->vars, 0);
		    ctmpa->tool      = t_tool ? t_tool : text_tool;
		    ctmpa->varnamep  = ctmpb;
		}
	    }
	    else{
		ctmpa->varmem = 0;
		ctmpa->value  = pretty_value(ps, ctmpa);
		ctmpa->tool   = t_tool ? t_tool : text_tool;
		ctmpa->keymenu = km ? km : &config_text_wshuf_keymenu;
	    }
	}
	else{
	    if(vtmp == &ps->vars[V_FILLCOL]
	       || vtmp == &ps->vars[V_QUOTE_SUPPRESSION]
	       || vtmp == &ps->vars[V_OVERLAP]
	       || vtmp == &ps->vars[V_MAXREMSTREAM]
	       || vtmp == &ps->vars[V_MARGIN]
	       || vtmp == &ps->vars[V_DEADLETS]
	       || vtmp == &ps->vars[V_NMW_WIDTH]
	       || vtmp == &ps->vars[V_STATUS_MSG_DELAY]
	       || vtmp == &ps->vars[V_MAILCHECK]
	       || vtmp == &ps->vars[V_MAILCHECKNONCURR]
	       || vtmp == &ps->vars[V_MAILDROPCHECK]
	       || vtmp == &ps->vars[V_NNTPRANGE]
	       || vtmp == &ps->vars[V_TCPOPENTIMEO]
	       || vtmp == &ps->vars[V_TCPREADWARNTIMEO]
	       || vtmp == &ps->vars[V_TCPWRITEWARNTIMEO]
	       || vtmp == &ps->vars[V_TCPQUERYTIMEO]
	       || vtmp == &ps->vars[V_RSHOPENTIMEO]
	       || vtmp == &ps->vars[V_SSHOPENTIMEO]
	       || vtmp == &ps->vars[V_INCCHECKTIMEO]
	       || vtmp == &ps->vars[V_INCCHECKINTERVAL]
	       || vtmp == &ps->vars[V_USERINPUTTIMEO]
	       || vtmp == &ps->vars[V_REMOTE_ABOOK_VALIDITY]
	       || vtmp == &ps->vars[V_REMOTE_ABOOK_HISTORY])
	      ctmpa->flags |= CF_NUMBER;

	    ctmpa->value = pretty_value(ps, ctmpa);
	}
    }

    dprint((9, "add hidden features\n"));

    /* add the hidden features */
    if(expose_hidden_config){
	char *new_sect;

	new_confline(&ctmpa);		/* Blank line */
	ctmpa->flags	       |= CF_NOSELECT | CF_B_LINE;

	new_confline(&ctmpa)->var	= NULL;
	ctmpa->help			= NO_HELP;
	ctmpa->valoffset		= 2;
	ctmpa->flags	       |= CF_NOSELECT;
	ctmpa->value = cpystr("--- [ Normally hidden configuration features ] ---");

	new_confline(&ctmpa);		/* Blank line */
	ctmpa->flags	       |= CF_NOSELECT | CF_B_LINE;

	vtmp = &ps->vars[V_FEATURE_LIST];

	ctmpa->flags		 |= CF_NOSELECT;
	ctmpa->keymenu		  = &config_checkbox_keymenu;
	ctmpa->tool			  = NULL;

	/* put a nice delimiter before list */
	new_confline(&ctmpa)->var = NULL;
	ctmpa->varnamep		  = ctmpb;
	ctmpa->keymenu		  = &config_checkbox_keymenu;
	ctmpa->help			  = NO_HELP;
	ctmpa->tool			  = checkbox_tool;
	ctmpa->valoffset		  = 12;
	ctmpa->flags		 |= CF_NOSELECT;
	ctmpa->value = cpystr("Set    Feature Name");

	new_confline(&ctmpa)->var = NULL;
	ctmpa->varnamep		  = ctmpb;
	ctmpa->keymenu		  = &config_checkbox_keymenu;
	ctmpa->help			  = NO_HELP;
	ctmpa->tool			  = checkbox_tool;
	ctmpa->valoffset		  = 12;
	ctmpa->flags		 |= CF_NOSELECT;
	ctmpa->value = cpystr("---  ----------------------");

	for(i = 0; feature = feature_list(i); i++)
	  if((new_sect = feature_list_section(feature)) &&
	     (strcmp(new_sect, HIDDEN_PREF) == 0)){

	      new_confline(&ctmpa)->var	= vtmp;
	      ctmpa->varnamep		= ctmpb;
	      ctmpa->keymenu		= &config_checkbox_keymenu;
	      ctmpa->help		= config_help(vtmp-ps->vars,
						      feature->id);
	      ctmpa->tool		= checkbox_tool;
	      ctmpa->valoffset		= 12;
	      ctmpa->varmem		= i;
	      ctmpa->value		= pretty_value(ps, ctmpa);
	  }
    }

    vsave = save_config_vars(ps, expose_hidden_config);
    first_line = first_sel_confline(first_line);

    memset(&screen, 0, sizeof(screen));
    screen.ro_warning = readonly_warning;
    /* TRANSLATORS: Print something1 using something2.
       "configuration" is something1 */
    switch(conf_scroll_screen(ps, &screen, first_line,
			      edit_exceptions ? CONFIG_SCREEN_TITLE_EXC
					      : CONFIG_SCREEN_TITLE,
			      _("configuration"), 0)){
      case 0:
	break;

      case 1:
	write_pinerc(ps, ew, WRP_NONE);
	break;
    
      case 10:
	revert_to_saved_config(ps, vsave, expose_hidden_config);
	break;
      
      default:
	q_status_message(SM_ORDER,7,10,
	    "conf_scroll_screen bad ret, not supposed to happen");
	break;
    }

    pval = PVAL(&ps->vars[V_SORT_KEY], ew);
    if(vsave[V_SORT_KEY].saved_user_val.p && pval
       && strcmp(vsave[V_SORT_KEY].saved_user_val.p, pval)){
	if(!mn_get_mansort(ps_global->msgmap)){
	    clear_index_cache(ps_global->mail_stream, 0);
	    reset_sort_order(SRT_VRB);
	}
    }

    treat_color_vars_as_text = 0;
    free_saved_config(ps, &vsave, expose_hidden_config);
#ifdef _WINDOWS
    mswin_set_quit_confirm (F_OFF(F_QUIT_WO_CONFIRM, ps_global));
#endif
}


int
litsig_text_tool(struct pine *ps, int cmd, CONF_S **cl, unsigned int flags)
{
    char           **apval;
    int		     rv = 0;

    if(cmd != MC_EXIT && fixed_var((*cl)->var, NULL, NULL))
      return(rv);

    apval = APVAL((*cl)->var, ew);

    switch(cmd){
      case MC_ADD:
      case MC_EDIT :
	if(apval){
	    char *input = NULL, *result = NULL, *err = NULL, *cstring_version;
	    char *olddefval = NULL, *start_with;
	    size_t len;

	    if(!*apval && (*cl)->var->current_val.p &&
	       (*cl)->var->current_val.p[0]){
		if(!strncmp((*cl)->var->current_val.p,
			    DSTRING,
			    (len=strlen(DSTRING)))){
		    /* strip DSTRING and trailing paren */
		    olddefval = (char *)fs_get(strlen((*cl)->var->current_val.p)+1);
		    strncpy(olddefval, (*cl)->var->current_val.p+len,
			    strlen((*cl)->var->current_val.p)-len-1);
		    olddefval[strlen((*cl)->var->current_val.p)-len-1] = '\0';
		    start_with = olddefval;
		}
		else{
		    olddefval = cpystr((*cl)->var->current_val.p);
		    start_with = olddefval;
		}
	    }
	    else
	      start_with = (*apval) ? *apval : "";

	    input = (char *)fs_get((strlen(start_with)+1) * sizeof(char));
	    input[0] = '\0';
	    cstring_to_string(start_with, input);
	    err = signature_edit_lit(input, &result,
				     ((*cl)->var == role_comment_ptr)
					 ? "COMMENT EDITOR"
					 : "SIGNATURE EDITOR",
				     ((*cl)->var == role_comment_ptr)
					 ? h_composer_commentedit
					 : h_composer_sigedit);

	    if(!err){
		if(olddefval && !strcmp(input, result) &&
		   want_to(_("Leave unset and use default "), 'y',
			   'y', NO_HELP, WT_FLUSH_IN) == 'y'){
		    rv = 0;
		}
		else{
		    cstring_version = string_to_cstring(result);

		    if(apval && *apval)
		      fs_give((void **)apval);
		    
		    if(apval){
			*apval = cstring_version;
			cstring_version = NULL;
		    }

		    if(cstring_version)
		      fs_give((void **)&cstring_version);

		    rv = 1;
		}
	    }
	    else
	      rv = 0;

	    if(err){
		q_status_message1(SM_ORDER, 3, 5, "%s", err);
		fs_give((void **)&err);
	    }

	    if(result)
	      fs_give((void **)&result);
	    if(olddefval)
	      fs_give((void **)&olddefval);
	    if(input)
	      fs_give((void **)&input);
	}

	ps->mangled_screen = 1;
	break;
	
      default:
	rv = text_tool(ps, cmd, cl, flags);
	break;
    }

    /*
     * At this point, if changes occurred, var->user_val.X is set.
     * So, fix the current_val, and handle special cases...
     *
     * NOTE: we don't worry about the "fixed variable" case here, because
     *       editing such vars should have been prevented above...
     */
    if(rv == 1){
	/*
	 * Now go and set the current_val based on user_val changes
	 * above.  Turn off command line settings...
	 */
	set_current_val((*cl)->var, TRUE, FALSE);

	if((*cl)->value)
	  fs_give((void **)&(*cl)->value);

	(*cl)->value = pretty_value(ps, *cl);

	exception_override_warning((*cl)->var);

	/*
	 * The value of literal sig can affect whether signature file is
	 * used or not. So it affects what we display for sig file variable.
	 */
	if((*cl)->next && (*cl)->next->var == &ps->vars[V_SIGNATURE_FILE]){
	    if((*cl)->next->value)
	      fs_give((void **)&(*cl)->next->value);
	    
	    (*cl)->next->value = pretty_value(ps, (*cl)->next);
	}
    }

    return(rv);
}


int
inbox_path_text_tool(struct pine *ps, int cmd, CONF_S **cl, unsigned int flags)
{
    char           **apval;
    int		     rv = 0;
    char             new_inbox_path[2*MAXFOLDER+1];
    char            *def = NULL;
    CONTEXT_S       *cntxt;

    if(cmd != MC_EXIT && fixed_var((*cl)->var, NULL, NULL))
      return(rv);

    apval = APVAL((*cl)->var, ew);

    switch(cmd){
      case MC_ADD:
      case MC_EDIT:
	cntxt = ps->context_list;
	if(cmd == MC_EDIT && (*cl)->var){
	    if(ew == Post && (*cl)->var->post_user_val.p)
	      def = (*cl)->var->post_user_val.p;
	    else if(ew == Main && (*cl)->var->main_user_val.p)
	      def = (*cl)->var->main_user_val.p;
	    else if((*cl)->var->current_val.p)
	      def = (*cl)->var->current_val.p;
	}

	rv = add_new_folder(cntxt, ew, V_INBOX_PATH, new_inbox_path,
			    sizeof(new_inbox_path), NULL, def);
	rv = rv ? 1 : 0;

	ps->mangled_screen = 1;
        break;

      default:
	rv = text_tool(ps, cmd, cl, flags);
	break;
    }

    /*
     * This is just like the end of text_tool.
     */
    if(rv == 1){
	/*
	 * Now go and set the current_val based on user_val changes
	 * above.  Turn off command line settings...
	 */
	set_current_val((*cl)->var, TRUE, FALSE);
	fix_side_effects(ps, (*cl)->var, 0);

	if((*cl)->value)
	  fs_give((void **) &(*cl)->value);

	(*cl)->value = pretty_value(ps, *cl);

	exception_override_warning((*cl)->var);
    }

    return(rv);
}


int
incoming_monitoring_list_tool(struct pine *ps, int cmd, CONF_S **cl, unsigned int flags)
{
    int	        rv = 0;
    CONTEXT_S  *cntxt;
    char      **the_list;
    CONF_S     *ctmp;
    char      **newval = NULL;
    char     ***alval;

    if(cmd != MC_EXIT && fixed_var((*cl)->var, NULL, NULL))
      return(rv);

    switch(cmd){
      case MC_ADD:
      case MC_EDIT:
	cntxt = ps->context_list;
	if(!(cntxt && cntxt->use & CNTXT_INCMNG)){
	    q_status_message1(SM_ORDER, 3, 3, _("Turn on incoming folders with Config feature \"%s\""), feature_list_name(F_ENABLE_INCOMING));
	    return(rv);
	}

	the_list = adjust_list_of_monitored_incoming(cntxt, ew, V_INCCHECKLIST);

	/* adjust top if it might be necessary */
	for(ctmp = (*cl)->varnamep;
	    ctmp && ctmp->varnamep == (*cl)->varnamep;
	    ctmp = next_confline(ctmp))
	  if(ctmp == opt_screen->top_line)
	    opt_screen->top_line = (*cl)->varnamep;

	if(the_list){
	    alval = ALVAL((*cl)->var, ew);
	    free_list_array(alval);
	    config_add_list(ps, cl, the_list, NULL, 0);

	    /* only have to free the top-level pointer */
	    fs_give((void **) &the_list);
	    rv = 1;
	}
	else{
	    if(LVAL((*cl)->var, ew))
	      q_status_message(SM_ORDER, 0, 3, _("List is unchanged"));
	    else
	      q_status_message(SM_ORDER, 0, 3, _("Using default, monitor all incoming folders"));
	}

	ps->mangled_screen = 1;
        break;

      default:
	rv = text_tool(ps, cmd, cl, flags);
	/* if we deleted last one, reverts to default */
        if(cmd == MC_DELETE && rv == 1 && (*cl)->varmem == 0
	   && (!(*cl)->next || (*cl)->next->varnamep != (*cl)))
	  q_status_message(SM_ORDER, 0, 3, _("Using default, monitor all incoming folders"));

	break;
    }

    /*
     * This is just like the end of text_tool.
     */
    if(rv == 1){
	/*
	 * Now go and set the current_val based on user_val changes
	 * above.  Turn off command line settings...
	 */
	set_current_val((*cl)->var, TRUE, FALSE);
	fix_side_effects(ps, (*cl)->var, 0);

	if((*cl)->value)
	  fs_give((void **) &(*cl)->value);

	(*cl)->value = pretty_value(ps, *cl);

	exception_override_warning((*cl)->var);
    }

    return(rv);
}

char **
adjust_list_of_monitored_incoming(CONTEXT_S *cntxt, EditWhich which, int varnum)
{
    LIST_SEL_S *listhead, *p, *ls;
    int i, cnt, ftotal;
    long width;
    FOLDER_S *f;
    char **the_list = NULL, buf[1000];

    if(!(cntxt && cntxt->use & CNTXT_INCMNG))
      return(the_list);

    p = listhead = NULL;

    /* this width is determined by select_from_list_screen() */
    width = ps_global->ttyo->screen_cols - 4;

    /*
     * Put together a list of folders to select from.
     * We could choose to use the list associated with
     * the config we're editing, and that may be correct.
     * However, we think most users will expect the list
     * to be the list that is in use. In any case, these
     * are almost always the same.
     */

    ftotal = folder_total(FOLDERS(cntxt));

    for(i = 0; i < ftotal; i++){

	f = folder_entry(i, FOLDERS(cntxt));

	ls = (LIST_SEL_S *) fs_get(sizeof(*ls));
	memset(ls, 0, sizeof(*ls));

	if(f && f->nickname){
	    ls->item = cpystr(f->nickname);
	    snprintf(buf, sizeof(buf), "%s  (%s)", f->nickname, f->name ? f->name : "?");
	    ls->display_item = cpystr(buf);
	}
	else
	  ls->item = cpystr((f && f->name) ? f->name : "?");

	if(f && f->last_unseen_update != LUU_NEVERCHK)
	  ls->selected = 1;

	if(p){
	    p->next = ls;
	    p = p->next;
	}
	else{
	    /* add a heading */
	    listhead = (LIST_SEL_S *) fs_get(sizeof(*ls));
	    memset(listhead, 0, sizeof(*listhead));
	    listhead->flags = SFL_NOSELECT;
	    listhead->display_item = cpystr(_("Incoming folders to be monitored"));
	    listhead->next = (LIST_SEL_S *) fs_get(sizeof(*ls));
	    memset(listhead->next, 0, sizeof(*listhead));
	    listhead->next->flags = SFL_NOSELECT;
	    listhead->next->display_item = cpystr(repeat_char(width, '-'));

	    listhead->next->next = ls;
	    p = ls;
	}
    }

    if(!listhead){
	q_status_message1(SM_ORDER, 3, 3, _("Turn on incoming folders with Config feature \"%s\""), feature_list_name(F_ENABLE_INCOMING));
	return(the_list);
    }

    if(!select_from_list_screen(listhead,
			SFL_ALLOW_LISTMODE|SFL_STARTIN_LISTMODE|SFL_ONLY_LISTMODE,
			        _("SELECT FOLDERS TO MONITOR"), _("folders"),
				h_select_incoming_to_monitor,
				_("HELP FOR SELECTING FOLDERS"))){

	for(cnt = 0, p = listhead; p; p = p->next)
	  if(p->selected)
	    cnt++;

	if(cnt > 0 && cnt < ftotal){
	    the_list = (char **) fs_get((cnt+1) * sizeof(*the_list));
	    memset(the_list, 0, (cnt+1) * sizeof(*the_list));
	    for(i = 0, p = listhead; p; p = p->next)
	      if(p->selected)
		the_list[i++] = cpystr(p->item ? p->item : "");
	}
    }

    free_list_sel(&listhead);

    return(the_list);
}
