/*
 * Copyright 1998,1999 The OpenLDAP Foundation, Redwood City, California, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted only
 * as authorized by the OpenLDAP Public License.  A copy of this
 * license is available at http://www.OpenLDAP.org/license.html or
 * in file LICENSE in the top-level directory of the distribution.
 */
/* Portions
 * Copyright (c) 1993 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */
/* avl.h - avl tree definitions */


#ifndef _AVL_H
#define _AVL_H

#include <ldap_cdefs.h>

/*
 * this structure represents a generic avl tree node.
 */

LDAP_BEGIN_DECL

typedef struct avlnode Avlnode;

typedef int		(*AVL_APPLY) LDAP_P((void *, void*));
typedef int		(*AVL_CMP) LDAP_P((const void*, const void*));
typedef int		(*AVL_DUP) LDAP_P((void*, void*));
typedef void	(*AVL_FREE) LDAP_P((void*));

LDAP_F( int )
avl_free LDAP_P(( Avlnode *root, AVL_FREE dfree ));

LDAP_F( int )
avl_insert LDAP_P((Avlnode **, void*, AVL_CMP, AVL_DUP));

LDAP_F( void* )
avl_delete LDAP_P((Avlnode **, void*, AVL_CMP));

LDAP_F( void* )
avl_find LDAP_P((Avlnode *, const void*, AVL_CMP));

LDAP_F( void* )
avl_find_lin LDAP_P((Avlnode *, const void*, AVL_CMP));

#ifdef AVL_NONREENTRANT
LDAP_F( void* )
avl_getfirst LDAP_P((Avlnode *));

LDAP_F( void* )
avl_getnext LDAP_P((void));
#endif

LDAP_F( int )
avl_dup_error LDAP_P((void*, void*));

LDAP_F( int )
avl_dup_ok LDAP_P((void*, void*));

LDAP_F( int )
avl_apply LDAP_P((Avlnode *, AVL_APPLY, void*, int, int));

LDAP_F( int )
avl_prefixapply LDAP_P((Avlnode *, void*, AVL_CMP, void*, AVL_CMP, void*, int));

/* apply traversal types */
#define AVL_PREORDER	1
#define AVL_INORDER	2
#define AVL_POSTORDER	3
/* what apply returns if it ran out of nodes */
#define AVL_NOMORE	(-6)

LDAP_END_DECL

#endif /* _AVL_H */
