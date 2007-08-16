/*
 * $Id$
 *
 * presence module - presence server implementation
 *
 * Copyright (C) 2006 Voice Sistem S.R.L.
 *
 * This file is part of openser, a free SIP server.
 *
 * openser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2006-08-15  initial version (anca)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../db/db.h"
#include "../../dprint.h"
#include "../../mem/shm_mem.h"
#include "../../str.h"
#include "../alias_db/alias_db.h"
#include "../../data_lump_rpl.h"
#include "presentity.h"
#include "presence.h" 
#include "notify.h"
#include "publish.h"
#include "hash.h"
#include "utils_func.h"

extern int use_db;
extern char* presentity_table;
extern db_con_t* pa_db;
extern db_func_t pa_dbf;

static str pu_200_rpl  = str_init("OK");
static str pu_412_rpl  = str_init("Conditional request failed");

char* generate_ETag(int publ_count)
{
	char* etag;
	int size = 0;
	etag = (char*)pkg_malloc(128*sizeof(char));
	if(etag ==NULL)
	{
		LOG(L_ERR, "PRESENCE:generate_ETag:Error while allocating memory \n");
		return NULL ;
	}
	memset(etag, 0, 60*sizeof(char));
	size = sprintf (etag, "%c.%d.%d.%d.%d",prefix, startup_time, pid, counter, publ_count);
	if( size <0 )
	{
		LOG(L_ERR, "PRESENCE: generate_ETag: ERROR unsuccessfull sprintf\n ");
		pkg_free(etag);
		return NULL;
	}
	if(size> 128)
	{
		LOG(L_ERR, "PRESENCE: generate_ETag: ERROR buffer size overflown\n");
		pkg_free(etag);
		return NULL;
	}

	etag[size] = '\0';
	DBG("PRESENCE: generate_ETag: etag= %s / %d\n ",etag, size);
	return etag;
}

int publ_send200ok(struct sip_msg *msg, int lexpire, str etag)
{
	char buf[128];
	int buf_len= 128, size;
	str hdr_append= {0, 0}, hdr_append2= {0, 0} ;

	DBG("PRESENCE:publ_send200ok: send 200OK reply\n");	
	DBG("PRESENCE:publ_send200ok: etag= %s - len= %d\n", etag.s, etag.len);
	
	/* ??? should we use static allocated buffer */
	
	hdr_append.s = buf;
	hdr_append.s[0]='\0';
	hdr_append.len = sprintf(hdr_append.s, "Expires: %d\r\n",lexpire -
			expires_offset);
	if(hdr_append.len < 0)
	{
		LOG(L_ERR, "PRESENCE:publ_send200ok: ERROR unsuccessful sprintf\n");
		goto error;
	}
	if(hdr_append.len > buf_len)
	{
		LOG(L_ERR, "PRESENCE:publ_send200ok: ERROR buffer size overflown\n");
		goto error;
	}
	hdr_append.s[hdr_append.len]= '\0';
		
	if (add_lump_rpl( msg, hdr_append.s, hdr_append.len, LUMP_RPL_HDR)==0 )
	{
		LOG(L_ERR,"PRESENCE: publ_send200ok:ERROR unable to add lump_rl\n");
		goto error;
	}

	size= sizeof(char)*(20+etag.len) ;
	hdr_append2.s = (char *)pkg_malloc(size);
	if(hdr_append2.s == NULL)
	{
		LOG(L_ERR,"PRESENCE:publ_send200ok:ERROR no more memory\n");
		goto error;
	}
	hdr_append2.s[0]='\0';
	hdr_append2.len = sprintf(hdr_append2.s, "SIP-ETag: %s\r\n", etag.s);
	if(hdr_append2.len < 0)
	{
		LOG(L_ERR, "PRESENCE:publ_send200ok:ERROR unsuccessful sprintf\n ");
		goto error;
	}
	if(hdr_append2.len > size)
	{
		LOG(L_ERR, "PRESENCE:publ_send200ok: ERROR buffer size overflown\n");
		goto error;
	}

	hdr_append2.s[hdr_append2.len]= '\0';
	if (add_lump_rpl(msg, hdr_append2.s, hdr_append2.len, LUMP_RPL_HDR)==0 )
	{
		LOG(L_ERR,"PRESENCE:publ_send200ok: unable to add lump_rl\n");
		goto error;
	}

	if( slb.reply( msg, 200, &pu_200_rpl)== -1)
	{
		LOG(L_ERR,"PRESENCE: publ_send200ok: ERORR while sending reply\n");
		goto error;
	}

	pkg_free(hdr_append2.s);
	return 0;

error:

	if(hdr_append2.s)
		pkg_free(hdr_append2.s);

	return 0;

}	
presentity_t* new_presentity( str* domain,str* user,int expires, 
		pres_ev_t* event, str* etag, str* sender)
{
	presentity_t *presentity;
	int size, init_len;
	
	/* allocating memory for presentity */
	size = sizeof(presentity_t)+ (domain->len+ user->len+ etag->len + 50)
		* sizeof(char);
	if(sender)
		size+= sizeof(str)+ sender->len* sizeof(char);
	
	init_len= size;

	presentity = (presentity_t*)pkg_malloc(size);
	if(presentity == NULL)
	{
		LOG(L_ERR, "PRESENCE:new_presentity: No memory left: size=%d\n", size);
		return NULL;
	}
	memset(presentity, 0, size);
	size= sizeof(presentity_t);

	presentity->domain.s = (char*)presentity+ size;
	strncpy(presentity->domain.s, domain->s, domain->len);
	presentity->domain.len = domain->len;
	size+= domain->len;	
	
	presentity->user.s = (char*)presentity+size;
	strncpy(presentity->user.s, user->s, user->len);
	presentity->user.len = user->len;
	size+= user->len;

	presentity->etag.s = (char*)presentity+ size;
	memcpy(presentity->etag.s, etag->s, etag->len);
	presentity->etag.s[etag->len]= '\0';
	presentity->etag.len = etag->len;

	size+= etag->len+1;
	
	if(sender)
	{
		presentity->sender= (str*)((char*)presentity+ size);
		size+= sizeof(str);
		presentity->sender->s= (char*)presentity + size;
		memcpy(presentity->sender->s, sender->s, sender->len);
		presentity->sender->len= sender->len;
		size+= sender->len;
	}

	if(size> init_len)
	{
		LOG(L_ERR, "PRESENCE: new_presentity: ERROR"
			" buffer size overflow init_len= %d, size= %d\n", init_len, size);
		pkg_free(presentity);
		return NULL;
	}
	DBG("PRESENCE: new_presentity:init_len= %d size= %d\n", init_len, size);
	presentity->event= event;
	presentity->expires = expires;
	presentity->received_time= (int)time(NULL);
	return presentity;
    
}

int update_presentity(struct sip_msg* msg, presentity_t* presentity, str* body,
		int new_t)
{
	db_key_t query_cols[11], result_cols[5];
	db_op_t  query_ops[11];
	db_val_t query_vals[11], update_vals[7];
	db_key_t update_keys[7];
	db_res_t *result= NULL;
	int n_query_cols = 0;
	int n_update_cols = 0;
	char* dot= NULL;
	str etag= {0, 0};
	str cur_etag= {0, 0};

	if( !use_db )
		return 0;

	query_cols[n_query_cols] = "domain";
	query_ops[n_query_cols] = OP_EQ;
	query_vals[n_query_cols].type = DB_STR;
	query_vals[n_query_cols].nul = 0;
	query_vals[n_query_cols].val.str_val = presentity->domain;
	n_query_cols++;
	
	query_cols[n_query_cols] = "username";
	query_ops[n_query_cols] = OP_EQ;
	query_vals[n_query_cols].type = DB_STR;
	query_vals[n_query_cols].nul = 0;
	query_vals[n_query_cols].val.str_val = presentity->user;
	n_query_cols++;

	query_cols[n_query_cols] = "event";
	query_ops[n_query_cols] = OP_EQ;
	query_vals[n_query_cols].type = DB_STR;
	query_vals[n_query_cols].nul = 0;
	query_vals[n_query_cols].val.str_val = presentity->event->name;
	n_query_cols++;

	query_cols[n_query_cols] = "etag";
	query_ops[n_query_cols] = OP_EQ;
	query_vals[n_query_cols].type = DB_STR;
	query_vals[n_query_cols].nul = 0;
	query_vals[n_query_cols].val.str_val = presentity->etag;
	n_query_cols++;

	result_cols[0]= "expires"; 
	if(presentity->expires == 0 && !new_t ) 
	{
		if( publ_send200ok(msg, presentity->expires, presentity->etag)< 0)
		{
			LOG(L_ERR, "PRESENCE:update_presentity: ERROR while sending 200OK\n");
			return -1;
		}
		if( publ_notify( presentity, body, &presentity->etag)< 0 )
		{
			LOG(L_ERR,"PRESENCE:update_presentity: ERROR while sending notify\n");
			return -1;
		}
		if (pa_dbf.use_table(pa_db, presentity_table) < 0) 
		{
			LOG(L_ERR, "PRESENCE:update_presentity: Error in use_table\n");
			goto error;
		}
		DBG("PRESENCE:update_presentity: expires =0 -> deleting"
			" from database\n");
		if(pa_dbf.delete(pa_db, query_cols, 0 ,query_vals,n_query_cols)< 0 )
		{
			DBG( "PRESENCE:update_presentity: ERROR cleaning"
					" unsubscribed messages\n");
		}
		DBG("PRESENCE:update_presentity:delete from db %.*s\n",
			presentity->user.len,presentity->user.s );

		/* delete from hash table */
	
		if(delete_phtable(&msg->first_line.u.request.uri, 
					presentity->event->evp->parsed)< 0)
		{
			LOG(L_ERR, "PRESENCE:update_presentity: ERROR deleting record"
					" from hash table\n");
			goto error;
		}

		return 1;
	}

	if(new_t) 
	{
		/* insert new record in hash_table */
		
		if(insert_phtable(&msg->first_line.u.request.uri, 
					presentity->event->evp->parsed)< 0)
		{
			LOG(L_ERR, "PRESENCE:update_presentity: ERROR inserting record"
					" in hash table\n");
			goto error;
		}
		
		/* insert new record into database */	
		query_cols[n_query_cols] = "expires";
		query_vals[n_query_cols].type = DB_INT;
		query_vals[n_query_cols].nul = 0;
		query_vals[n_query_cols].val.int_val = presentity->expires+
				(int)time(NULL);
		n_query_cols++;

		query_cols[n_query_cols] = "body";
		query_vals[n_query_cols].type = DB_BLOB;
		query_vals[n_query_cols].nul = 0;
		query_vals[n_query_cols].val.str_val.s = body->s;
		query_vals[n_query_cols].val.str_val.len = body->len;
		n_query_cols++;
		
		query_cols[n_query_cols] = "received_time";
		query_vals[n_query_cols].type = DB_INT;
		query_vals[n_query_cols].nul = 0;
		query_vals[n_query_cols].val.int_val = presentity->received_time;
		n_query_cols++;

		if (pa_dbf.use_table(pa_db, presentity_table) < 0) 
		{
			LOG(L_ERR, "PRESENCE:update_presentity: Error in use_table\n");
			goto error;
		}

		DBG( "PRESENCE:update_presentity: inserting %d cols into"
				" table\n",
				n_query_cols);
				
		if (pa_dbf.insert(pa_db, query_cols, query_vals, n_query_cols) < 0) 
		{
			LOG(L_ERR, "PRESENCE:update_presentity: Error while"
					" inserting new presentity\n");
			goto error;
		}
		/* send 200ok */
		if(publ_send200ok(msg, presentity->expires, presentity->etag)< 0)
		{
			LOG(L_ERR, "PRESENCE:update_presentity: ERROR while sending 2000k\n");
			goto error;
		}
		
		/* send notify with presence information */
		if (publ_notify(presentity, body,NULL )<0)
		{
			LOG(L_ERR,"PRESENCE:update_presentity: ERROR while sending notify\n");
			return -1;
		}
	}
	else
	{
		if (pa_dbf.use_table(pa_db, presentity_table) < 0) 
		{
			LOG(L_ERR, "PRESENCE:update_presentity: Error in use_table\n");
			goto error;
		}

		DBG("PRESENCE:update_presentity: querying presentity  \n");
		if (pa_dbf.query (pa_db, query_cols, query_ops, query_vals,
			 result_cols, n_query_cols, 1, 0, &result) < 0) 
		{
			LOG(L_ERR, "PRESENCE:update_presentity: Error while querying"
					" presentity\n");
			goto error;
		}
		if(result== NULL)
			goto error;

		if (result->n > 0)
		{
			n_update_cols= 0;
			pa_dbf.free_result(pa_db, result);
			result= NULL;

			if(presentity->event->etag_not_new== 0)
			{	
				/* generate another etag */
				unsigned int publ_nr;
				str str_publ_nr= {0, 0};

				dot= presentity->etag.s+ presentity->etag.len;
				while(*dot!= '.' && str_publ_nr.len< presentity->etag.len)
				{
					str_publ_nr.len++;
					dot--;
				}
				if(str_publ_nr.len== presentity->etag.len)
				{
					LOG(L_ERR, "PRESENCE:update_presentity: ERROR wrong etag\n");
					return -1;			
				}	
				str_publ_nr.s= dot+1;
				str_publ_nr.len--;
	
				if( str2int(&str_publ_nr, &publ_nr)< 0)
				{
					LOG(L_ERR, "PRESENCE: update_presentity: ERROR while converting string to int\n");
					goto error;
				}
				etag.s = generate_ETag(publ_nr+1);
				if(etag.s == NULL)
				{
					LOG(L_ERR,"PRESENCE:update_presentity: ERROR while generating etag\n");
					return -1;
				}
				etag.len=(strlen(etag.s));
				DBG("PRESENCE:update_presentity: new etag  = %.*s \n", etag.len,
					etag.s);
				
				cur_etag= etag;

				update_keys[n_update_cols] = "etag";
				update_vals[n_update_cols].type = DB_STR;
				update_vals[n_update_cols].nul = 0;
				update_vals[n_update_cols].val.str_val = etag;
				n_update_cols++;

			}
			else
				cur_etag= presentity->etag;
			
			update_keys[n_update_cols] = "expires";
			update_vals[n_update_cols].type = DB_INT;
			update_vals[n_update_cols].nul = 0;
			update_vals[n_update_cols].val.int_val = presentity->expires + (int)time(NULL);
			n_update_cols++;

			update_keys[n_update_cols] = "received_time";
			update_vals[n_update_cols].type = DB_INT;
			update_vals[n_update_cols].nul = 0;
			update_vals[n_update_cols].val.int_val = presentity->received_time;
			n_update_cols++;

			if(body==NULL || body->s==NULL) /* if there is no body update expires value
											   and sender if present */
			{
				if( pa_dbf.update( pa_db,query_cols, query_ops,
						query_vals, update_keys, update_vals, n_query_cols,n_update_cols)<0) 
				{
					LOG( L_ERR , "PRESENCE:update_presentity:ERROR while"
							" updating presence information\n");
					goto error;
				}
		
				/* send 200ok */
				if( publ_send200ok(msg, presentity->expires, cur_etag)< 0)
				{
					LOG(L_ERR, "PRESENCE:update_presentity: ERROR while sending 200OK\n");
					if(etag.s)
						pkg_free(etag.s);
					return -1;
				}
				if(etag.s)
					pkg_free(etag.s);
				return 0;
			}

			update_keys[n_update_cols] = "body";
			update_vals[n_update_cols].type = DB_BLOB;
			update_vals[n_update_cols].nul = 0;
			update_vals[n_update_cols].val.str_val.s = body->s;
			update_vals[n_update_cols].val.str_val.len=body->len;
			n_update_cols++;
		
			if( pa_dbf.update( pa_db,query_cols, query_ops, query_vals,
			update_keys,update_vals, n_query_cols, n_update_cols )<0) 
			{
				LOG( L_ERR , "PRESENCE:update_presentity: ERROR while"
						" updating presence information\n");
				goto error;
			}

			
			/* send 200OK */
			if( publ_send200ok(msg, presentity->expires, cur_etag)< 0)
			{
				LOG(L_ERR, "PRESENCE:update_presentity: ERROR while sending 200OK\n");
				if(etag.s)
					pkg_free(etag.s);
				return -1;
			}
			if(etag.s)
				pkg_free(etag.s);
			etag.s= NULL;
			
			/* presentity body is updated so send notify to all watchers */
			if (publ_notify(presentity, body, NULL)<0)
			{
				LOG(L_ERR,"PRESENCE:update_presentity: ERROR while sending notify\n");
				return -1;
			}
		}  
		else  /* if there isn't no registration with those 3 values */
		{
			pa_dbf.free_result(pa_db, result);
			result= NULL;
			LOG(L_DBG, "PRESENCE:update_presentity: No E_Tag match\n");
			if (slb.reply(msg, 412, &pu_412_rpl) == -1)
			{
				LOG(L_ERR, "PRESENCE:PRESENCE:update_presentity: ERROR while sending"
					"reply\n");
				goto error;
			}
		}
	}
	return 0;

error:
	LOG(L_ERR, "PRESENCE:update_presentity: ERROR occured\n");
	if(result)
		pa_dbf.free_result(pa_db, result);
	if(etag.s)
		pkg_free(etag.s);

	return -1;

}

int pres_htable_restore()
{
	/* query all records from presentity table and insert records 
	 * in presentity table */
	db_key_t result_cols[5];
	db_res_t *result= NULL;
	db_row_t *row= NULL ;	
	db_val_t *row_vals;
	int  i;
	str user, domain, ev_str, uri;
	int n_result_cols= 0;
	int user_col, domain_col, event_col, expires_col;
	int event;
	event_t e;

	result_cols[user_col= n_result_cols++]= "username";
	result_cols[domain_col= n_result_cols++]= "domain";
	result_cols[event_col= n_result_cols++]= "event";
	result_cols[expires_col= n_result_cols++]= "expires";

	if (pa_dbf.use_table(pa_db, presentity_table) < 0) 
	{
		LOG(L_ERR, "PRESENCE:update_presentity: Error in use_table\n");
		goto error;
	}

	if (pa_dbf.query (pa_db, 0, 0, 0,result_cols,0, n_result_cols,
				"username", &result) < 0)
	{
		LOG(L_ERR, "PRESENCE:pres_htable_restore: Error while querying"
				" presentity\n");
		goto error;
	}
	if(result== NULL)
		goto error;

	if(result->n<= 0)
	{
		pa_dbf.free_result(pa_db, result);
		return 0;
	}
		
	for(i= 0; i< result->n; i++)
	{
		row = &result->rows[i];
		row_vals = ROW_VALUES(row);

		if(row_vals[expires_col].val.int_val< (int)time(NULL))
			continue;

		user.s= (char*)row_vals[user_col].val.string_val;
		user.len= strlen(user.s);
		domain.s= (char*)row_vals[domain_col].val.string_val;
		domain.len= strlen(domain.s);
		ev_str.s= (char*)row_vals[event_col].val.string_val;
		ev_str.len= strlen(ev_str.s);
	
		if(event_parser(ev_str.s, ev_str.len, &e)< 0)
		{
			LOG(L_ERR, "PRESENCE:pres_htable_restore: ERROR parsing event\n");
			goto error;
		}
		event= e.parsed;

		if(uandd_to_uri(user, domain, &uri)< 0)
		{
			LOG(L_ERR,"PRESENCE:pres_htable_restore:ERROR constructing uri\n");
			goto error;
		}
		/* insert in hash_table*/
		if(insert_phtable(&uri, event)< 0)
		{
			LOG(L_ERR, "PRESENCE:pres_htable_restore: ERROR "
				"inserting record in presentity hash table");
			pkg_free(uri.s);
			goto error;
		}
		pkg_free(uri.s);
	}
	pa_dbf.free_result(pa_db, result);

	return 0;

error:
	if(result)
		pa_dbf.free_result(pa_db, result);
	return -1;	
}
