/* backglue.c - backend glue routines */
/* $OpenLDAP$ */
/*
 * Copyright 2001 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

/*
 * Functions to glue a bunch of other backends into a single tree.
 * All of the glued backends must share a common suffix. E.g., you
 * can glue o=foo and ou=bar,o=foo but you can't glue o=foo and o=bar.
 *
 * The only configuration items that are needed for this backend are
 * the suffixes, and they should be identical to suffixes of other
 * backends that are being configured. The suffixes must be listed
 * in order from longest to shortest, (most-specific to least-specific)
 * in order for the selection to work. Every backend that is being glued
 * must be fully configured as usual.
 *
 * The purpose of this backend is to allow you to split a single database
 * into pieces (for load balancing purposes, whatever) but still be able
 * to treat it as a single database after it's been split. As such, each
 * of the glued backends should have identical rootdn and rootpw.
 *
 * If you need more elaborate configuration, you probably should be using
 * back-meta instead.
 *  -- Howard Chu
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>

#define SLAPD_TOOLS
#include "slap.h"

typedef struct gluenode {
	BackendDB *be;
	char *pdn;
} gluenode;

typedef struct glueinfo {
	int nodes;
	gluenode n[1];
} glueinfo;

/* Just like select_backend, but only for our backends */
static BackendDB *
glue_back_select (
	BackendDB *be,
	const char *dn
)
{
	glueinfo *gi = (glueinfo *) be->be_private;
	struct berval bv;
	int i;

	bv.bv_len = strlen(dn);
	bv.bv_val = (char *) dn;

	for (i = 0; be->be_nsuffix[i]; i++) {
		if (dn_issuffixbv (&bv, be->be_nsuffix[i]))
			return gi->n[i].be;
	}
	return NULL;
}

static int
glue_back_db_open (
	BackendDB *be
)
{
	glueinfo *gi;
	int i, j, k;
	int ok;

	/*
	 * Done already? 
	 */
	if (be->be_private)
		return 0;

	for (i = 0; be->be_nsuffix[i]; i++);

	gi = (struct glueinfo *) ch_calloc (1, sizeof (glueinfo) +
		(i-1) * sizeof (gluenode) );

	be->be_private = gi;

	if (!gi)
		return 1;

	gi->nodes = i;
	be->be_glueflags = SLAP_GLUE_INSTANCE;

	/*
	 * For each of our suffixes, find the real backend that handles this 
	 * suffix. 
	 */
	for (i = 0; be->be_nsuffix[i]; i++) {
		for (j = 0; j < nbackends; j++) {
			if (be == &backends[j])
				continue;
			ok = 0;
			for (k = 0; backends[j].be_nsuffix &&
			     backends[j].be_nsuffix[k]; k++) {
				if (be->be_nsuffix[i]->bv_len !=
				    backends[j].be_nsuffix[k]->bv_len)
					continue;
				if (!strcmp (backends[j].be_nsuffix[k]->bv_val,
					     be->be_nsuffix[i]->bv_val)) {
					ok = 1;
					break;
				}
			}
			if (ok) {
				gi->n[i].be = &backends[j];
				gi->n[i].pdn = dn_parent (NULL,
						 be->be_nsuffix[i]->bv_val);
				if (i < gi->nodes - 1)
					gi->n[i].be->be_glueflags =
						SLAP_GLUE_SUBORDINATE;
				break;
			}
		}
	}

	/* If we were invoked in tool mode, open all the underlying backends */
	if (slapMode & SLAP_TOOL_MODE) {
		for (i = 0; be->be_nsuffix[i]; i++) {
			backend_startup (gi->n[i].be);
		}
	}
	return 0;
}

static int
glue_back_db_close (
	BackendDB *be
)
{
	glueinfo *gi = (glueinfo *) be->be_private;

	if (slapMode & SLAP_TOOL_MODE) {
		int i;
		for (i = 0; be->be_nsuffix[i]; i++) {
			backend_shutdown (gi->n[i].be);
		}
	}
	return 0;
}
int
glue_back_db_destroy (
	BackendDB *be
)
{
	free (be->be_private);
	return 0;
}

int
glue_back_bind (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	int method,
	struct berval *cred,
	char **edn
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_bind) {
		conn->c_authz_backend = be;
		rc = be->be_bind (be, conn, op, dn, ndn, method, cred, edn);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No bind target found",
				  NULL, NULL);
	}
	return rc;
}

typedef struct glue_state {
	int err;
	int nentries;
	int matchlen;
	char *matched;
	int nrefs;
	struct berval **refs;
} glue_state;

void
glue_back_response (
	Connection *conn,
	Operation *op,
	ber_tag_t tag,
	ber_int_t msgid,
	ber_int_t err,
	const char *matched,
	const char *text,
	struct berval **ref,
	const char *resoid,
	struct berval *resdata,
	struct berval *sasldata,
	LDAPControl **ctrls
)
{
	glue_state *gs = op->o_glue;

	if (err == LDAP_SUCCESS || gs->err != LDAP_SUCCESS)
		gs->err = err;
	if (gs->err == LDAP_SUCCESS && gs->matched) {
		free (gs->matched);
		gs->matchlen = 0;
	}
	if (gs->err != LDAP_SUCCESS && matched) {
		int len;
		len = strlen (matched);
		if (len > gs->matchlen) {
			if (gs->matched)
				free (gs->matched);
			gs->matched = ch_strdup (matched);
			gs->matchlen = len;
		}
	}
	if (ref) {
		int i, j, k;
		struct berval **new;

		for (i=0; ref[i]; i++);

		j = gs->nrefs;
		if (!j) {
			new = ch_malloc ((i+1)*sizeof(struct berval *));
		} else {
			new = ch_realloc(gs->refs,
				(j+i+1)*sizeof(struct berval *));
		}
		for (k=0; k<i; j++,k++) {
			new[j] = ber_bvdup(ref[k]);
		}
		new[j] = NULL;
		gs->nrefs = j;
		gs->refs = new;
	}
}

void
glue_back_sresult (
	Connection *c,
	Operation *op,
	ber_int_t err,
	const char *matched,
	const char *text,
	struct berval **refs,
	LDAPControl **ctrls,
	int nentries
)
{
	glue_state *gs = op->o_glue;

	gs->nentries += nentries;
	glue_back_response (c, op, 0, 0, err, matched, text, refs,
			    NULL, NULL, NULL, ctrls);
}

int
glue_back_search (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	int scope,
	int deref,
	int slimit,
	int tlimit,
	Filter *filter,
	const char *filterstr,
	char **attrs,
	int attrsonly
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	BackendDB *be;
	int i, rc, t2limit = 0, s2limit = 0;
	long stoptime = 0;
	glue_state gs = {0};
	struct berval bv;


	if (tlimit)
		stoptime = slap_get_time () + tlimit;

	switch (scope) {
	case LDAP_SCOPE_BASE:
		be = glue_back_select (b0, ndn);

		if (be && be->be_search) {
			rc = be->be_search (be, conn, op, dn, ndn, scope,
				   deref, slimit, tlimit, filter, filterstr,
					    attrs, attrsonly);
		} else {
			rc = LDAP_UNWILLING_TO_PERFORM;
			send_ldap_result (conn, op, rc, NULL,
				      "No search target found", NULL, NULL);
		}
		return rc;

	case LDAP_SCOPE_ONELEVEL:
	case LDAP_SCOPE_SUBTREE:
		op->o_glue = &gs;
		op->o_sresult = glue_back_sresult;
		op->o_response = glue_back_response;
		bv.bv_len = strlen(ndn);
		bv.bv_val = (char *) ndn;

		/*
		 * Execute in reverse order, most general first 
		 */
		for (i = gi->nodes-1; i >= 0; i--) {
			if (!gi->n[i].be || !gi->n[i].be->be_search)
				continue;
			if (tlimit) {
				t2limit = stoptime - slap_get_time ();
				if (t2limit <= 0)
					break;
			}
			if (slimit) {
				s2limit = slimit - gs.nentries;
				if (s2limit <= 0)
					break;
			}
			/*
			 * check for abandon 
			 */
			ldap_pvt_thread_mutex_lock (&op->o_abandonmutex);
			rc = op->o_abandon;
			ldap_pvt_thread_mutex_unlock (&op->o_abandonmutex);
			if (rc) {
				rc = 0;
				goto done;
			}
			be = gi->n[i].be;
			if (scope == LDAP_SCOPE_ONELEVEL && 
				!strcmp (gi->n[i].pdn, ndn)) {
				rc = be->be_search (be, conn, op,
						    b0->be_suffix[i],
						    b0->be_nsuffix[i]->bv_val,
						    LDAP_SCOPE_BASE, deref,
					s2limit, t2limit, filter, filterstr,
						    attrs, attrsonly);
			} else if (scope == LDAP_SCOPE_SUBTREE &&
				dn_issuffixbv (b0->be_nsuffix[i], &bv)) {
				rc = be->be_search (be, conn, op,
						    b0->be_suffix[i],
						    b0->be_nsuffix[i]->bv_val,
						    scope, deref,
					s2limit, t2limit, filter, filterstr,
						    attrs, attrsonly);
			} else if (dn_issuffixbv (&bv, b0->be_nsuffix[i])) {
				rc = be->be_search (be, conn, op,
						    dn, ndn, scope, deref,
					s2limit, t2limit, filter, filterstr,
						    attrs, attrsonly);
			}
		}
		break;
	}
	op->o_sresult = NULL;
	op->o_response = NULL;
	op->o_glue = NULL;

	send_search_result (conn, op, gs.err, gs.matched, NULL, gs.refs,
			    NULL, gs.nentries);
done:
	if (gs.matched)
		free (gs.matched);
	if (gs.refs)
		ber_bvecfree(gs.refs);
	return rc;
}

int
glue_back_compare (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	AttributeAssertion *ava
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_compare) {
		rc = be->be_compare (be, conn, op, dn, ndn, ava);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No compare target found",
				  NULL, NULL);
	}
	return rc;
}

int
glue_back_modify (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	Modifications *mod
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_modify) {
		rc = be->be_modify (be, conn, op, dn, ndn, mod);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No modify target found",
				  NULL, NULL);
	}
	return rc;
}

int
glue_back_modrdn (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	const char *newrdn,
	int del,
	const char *newsup
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_modrdn) {
		rc = be->be_modrdn (be, conn, op, dn, ndn, newrdn, del, newsup);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No modrdn target found",
				  NULL, NULL);
	}
	return rc;
}

int
glue_back_add (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *e
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, e->e_ndn);

	if (be && be->be_add) {
		rc = be->be_add (be, conn, op, e);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No add target found",
				  NULL, NULL);
	}
	return rc;
}

int
glue_back_delete (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_delete) {
		rc = be->be_delete (be, conn, op, dn, ndn);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No delete target found",
				  NULL, NULL);
	}
	return rc;
}

int
glue_back_release_rw (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *e,
	int rw
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, e->e_ndn);

	if (be && be->be_release) {
		rc = be->be_release (be, conn, op, e, rw);
	} else {
		entry_free (e);
		rc = 0;
	}
	return rc;
}

int
glue_back_group (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *target,
	const char *ndn,
	const char *ondn,
	ObjectClass *oc,
	AttributeDescription * ad
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_group) {
		rc = be->be_group (be, conn, op, target, ndn, ondn, oc, ad);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
	}
	return rc;
}

int
glue_back_attribute (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *target,
	const char *ndn,
	AttributeDescription *ad,
	struct berval ***vals
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_attribute) {
		rc = be->be_attribute (be, conn, op, target, ndn, ad, vals);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
	}
	return rc;
}

int
glue_back_referrals (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	const char *dn,
	const char *ndn,
	const char **text
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn);

	if (be && be->be_chk_referrals) {
		rc = be->be_chk_referrals (be, conn, op, dn, ndn, text);
	} else {
		rc = LDAP_SUCCESS;;
	}
	return rc;
}

static int glueMode;
static int glueBack;

int
glue_tool_entry_open (
	BackendDB *b0,
	int mode
)
{
	/* We don't know which backend to talk to yet, so just
	 * remember the mode and move on...
	 */

	glueMode = mode;
	glueBack = -1;

	return 0;
}

int
glue_tool_entry_close (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i, rc = 0;

	i = glueBack;
	if (i >= 0) {
		if (!gi->n[i].be->be_entry_close)
			return 0;
		rc = gi->n[i].be->be_entry_close (gi->n[i].be);
		glueBack = -1;
	}
	return rc;
}

ID
glue_tool_entry_first (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i;

	/* If we're starting from scratch, start at the most general */
	if (glueBack == -1) {
		for (i = gi->nodes-1; i >= 0; i--) {
			if (gi->n[i].be->be_entry_open &&
			    gi->n[i].be->be_entry_first)
				break;
		}
	} else {
		i = glueBack;
	}
	if (gi->n[i].be->be_entry_open (gi->n[i].be, glueMode) != 0)
		return NOID;
	glueBack = i;

	return gi->n[i].be->be_entry_first (gi->n[i].be);
}

ID
glue_tool_entry_next (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i, rc;

	i = glueBack;
	rc = gi->n[i].be->be_entry_next (gi->n[i].be);

	/* If we ran out of entries in one database, move on to the next */
	if (rc == NOID) {
		gi->n[i].be->be_entry_close (gi->n[i].be);
		i--;
		glueBack = i;
		if (i < 0)
			rc = NOID;
		else
			rc = glue_tool_entry_first (b0);
	}
	return rc;
}

Entry *
glue_tool_entry_get (
	BackendDB *b0,
	ID id
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i = glueBack;

	return gi->n[i].be->be_entry_get (gi->n[i].be, id);
}

ID
glue_tool_entry_put (
	BackendDB *b0,
	Entry *e
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	BackendDB *be;
	int i, rc;

	be = glue_back_select (b0, e->e_ndn);
	if (!be->be_entry_put)
		return NOID;

	i = glueBack;
	if (i < 0) {
		rc = be->be_entry_open (be, glueMode);
		if (rc != 0)
			return NOID;
		glueBack = i;
	} else if (be != gi->n[i].be) {
		/* If this entry belongs in a different branch than the
		 * previous one, close the current database and open the
		 * new one.
		 */
		gi->n[i].be->be_entry_close (gi->n[i].be);
		glueBack = -1;
		for (i = 0; b0->be_nsuffix[i]; i++)
			if (gi->n[i].be == be)
				break;
		rc = be->be_entry_open (be, glueMode);
		if (rc != 0)
			return NOID;
		glueBack = i;
	}
	return be->be_entry_put (be, e);
}

int
glue_tool_entry_reindex (
	BackendDB *b0,
	ID id
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i = glueBack;

	if (!gi->n[i].be->be_entry_reindex)
		return -1;

	return gi->n[i].be->be_entry_reindex (gi->n[i].be, id);
}

int
glue_tool_sync (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i;

	/* just sync everyone */
	for (i = 0; b0->be_nsuffix[i]; i++)
		if (gi->n[i].be->be_sync)
			gi->n[i].be->be_sync (gi->n[i].be);
	return 0;
}

int
glue_back_initialize (
	BackendInfo *bi
)
{
	bi->bi_open = 0;
	bi->bi_config = 0;
	bi->bi_close = 0;
	bi->bi_destroy = 0;

	bi->bi_db_init = 0;
	bi->bi_db_config = 0;
	bi->bi_db_open = glue_back_db_open;
	bi->bi_db_close = glue_back_db_close;
	bi->bi_db_destroy = glue_back_db_destroy;

	bi->bi_op_bind = glue_back_bind;
	bi->bi_op_unbind = 0;
	bi->bi_op_search = glue_back_search;
	bi->bi_op_compare = glue_back_compare;
	bi->bi_op_modify = glue_back_modify;
	bi->bi_op_modrdn = glue_back_modrdn;
	bi->bi_op_add = glue_back_add;
	bi->bi_op_delete = glue_back_delete;
	bi->bi_op_abandon = 0;

	bi->bi_extended = 0;

	bi->bi_entry_release_rw = glue_back_release_rw;
	bi->bi_acl_group = glue_back_group;
	bi->bi_acl_attribute = glue_back_attribute;
	bi->bi_chk_referrals = glue_back_referrals;

	/*
	 * hooks for slap tools
	 */
	bi->bi_tool_entry_open = glue_tool_entry_open;
	bi->bi_tool_entry_close = glue_tool_entry_close;
	bi->bi_tool_entry_first = glue_tool_entry_first;
	bi->bi_tool_entry_next = glue_tool_entry_next;
	bi->bi_tool_entry_get = glue_tool_entry_get;
	bi->bi_tool_entry_put = glue_tool_entry_put;
	bi->bi_tool_entry_reindex = glue_tool_entry_reindex;
	bi->bi_tool_sync = glue_tool_sync;

	bi->bi_connection_init = 0;
	bi->bi_connection_destroy = 0;

	return 0;
}
