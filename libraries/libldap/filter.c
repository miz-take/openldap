/* $OpenLDAP$ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/*  Portions
 *  Copyright (c) 1990 Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  search.c
 */

#include "portable.h"

#include <stdio.h>

#include <ac/stdlib.h>

#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap-int.h"

static char *put_complex_filter LDAP_P((
	BerElement *ber,
	char *str,
	ber_tag_t tag,
	int not ));

static int put_simple_filter LDAP_P((
	BerElement *ber,
	char *str ));

static int put_substring_filter LDAP_P((
	BerElement *ber,
	char *type,
	char *str ));

static int put_filter_list LDAP_P((
	BerElement *ber,
	char *str,
	ber_tag_t tag ));

static int ldap_is_oid ( const char *str )
{
	int i;

	if( LDAP_ALPHA( str[0] )) {
		for( i=1; str[i]; i++ ) {
			if( !LDAP_LDH( str[i] )) {
				return 0;
			}
		}
		return 1;

	} else if LDAP_DIGIT( str[0] ) {
		int dot=0;
		for( i=1; str[i]; i++ ) {
			if( LDAP_DIGIT( str[i] )) {
				dot=0;

			} else if ( str[i] == '.' ) {
				if( dot ) return 0;
				if( ++dot > 1 ) return 0;

			} else {
				return 0;
			}
		}
		return !dot;
	}

	return 0;
}

static int ldap_is_desc ( const char *str )
{
	int i;

	if( LDAP_ALPHA( str[0] )) {
		for( i=1; str[i]; i++ ) {
			if( str[i] == ';' ) {
				str = &str[i+1];
				goto options;
			}

			if( !LDAP_LDH( str[i] )) {
				return 0;
			}
		}
		return 1;

	} else if LDAP_DIGIT( str[0] ) {
		int dot=0;
		for( i=1; str[i]; i++ ) {
			if( str[i] == ';' ) {
				if( dot ) return 0;
				str = &str[i+1];
				goto options;
			}

			if( LDAP_DIGIT( str[i] )) {
				dot=0;

			} else if ( str[i] == '.' ) {
				if( dot ) return 0;
				if( ++dot > 1 ) return 0;

			} else {
				return 0;
			}
		}
		return !dot;
	}

	return 0;

options:
	if( !LDAP_LDH( str[0] )) {
		return 0;
	}
	for( i=1; str[i]; i++ ) {
		if( str[i] == ';' ) {
			str = &str[i+1];
			goto options;
		}
		if( !LDAP_LDH( str[i] )) {
			return 0;
		}
	}
	return 1;
}

static char *
find_right_paren( char *s )
{
	int	balance, escape;

	balance = 1;
	escape = 0;
	while ( *s && balance ) {
		if ( !escape ) {
			if ( *s == '(' ) {
				balance++;
			} else if ( *s == ')' ) {
				balance--;
			}
		}

		escape = ( *s == '\\' && !escape );

		if ( balance ) s++;
	}

	return *s ? s : NULL;
}

static int hex2value( int c )
{
	if( c >= '0' && c <= '9' ) {
		return c - '0';
	}

	if( c >= 'A' && c <= 'F' ) {
		return c + (10 - (int) 'A');
	}

	if( c >= 'a' && c <= 'f' ) {
		return c + (10 - (int) 'a');
	}

	return -1;
}

char *
ldap_pvt_find_wildcard( const char *s )
{
	for( ; *s; s++ ) {
		switch( *s ) {
		case '*':	/* found wildcard */
			return (char *) s;

		case '(':
		case ')':
			return NULL;

		case '\\':
			if( s[1] == '\0' ) return NULL;

			if( LDAP_HEX( s[1] ) && LDAP_HEX( s[2] ) ) {
				s+=2;

			} else switch( s[1] ) {
			default:
				return NULL;

			/* allow RFC 1960 escapes */
			case '*':
			case '(':
			case ')':
			case '\\':
				s++;
			}
		}
	}

	return (char *) s;
}

/* unescape filter value */
/* support both LDAP v2 and v3 escapes */
/* output can include nul characters! */
ber_slen_t
ldap_pvt_filter_value_unescape( char *fval )
{
	ber_slen_t r, v;
	int v1, v2;

	for( r=v=0; fval[v] != '\0'; v++ ) {
		switch( fval[v] ) {
		case '(':
		case ')':
		case '*':
			return -1;

		case '\\':
			/* escape */
			v++;

			if ( fval[v] == '\0' ) {
				/* escape at end of string */
				return -1;
			}

			if (( v1 = hex2value( fval[v] )) >= 0 ) {
				/* LDAPv3 escape */
				if (( v2 = hex2value( fval[v+1] )) < 0 ) {
					/* must be two digit code */
					return -1;
				}

				fval[r++] = v1 * 16 + v2;
				v++;

			} else {
				/* LDAPv2 escape */
				switch( fval[v] ) {
				case '(':
				case ')':
				case '*':
				case '\\':
					fval[r++] = fval[v];
					break;
				default:
					/* illegal escape */
					return -1;
				}
			}
			break;

		default:
			fval[r++] = fval[v];
		}
	}

	fval[r] = '\0';
	return r;
}

static char *
put_complex_filter( BerElement *ber, char *str, ber_tag_t tag, int not )
{
	char	*next;

	/*
	 * We have (x(filter)...) with str sitting on
	 * the x.  We have to find the paren matching
	 * the one before the x and put the intervening
	 * filters by calling put_filter_list().
	 */

	/* put explicit tag */
	if ( ber_printf( ber, "t{" /*"}"*/, tag ) == -1 ) {
		return NULL;
	}

	str++;
	if ( (next = find_right_paren( str )) == NULL ) {
		return NULL;
	}

	*next = '\0';
	if ( put_filter_list( ber, str, tag ) == -1 ) {
		return NULL;
	}

	/* close the '(' */
	*next++ = ')';

	/* flush explicit tagged thang */
	if ( ber_printf( ber, /*"{"*/ "N}" ) == -1 ) {
		return NULL;
	}

	return next;
}

int
ldap_pvt_put_filter( BerElement *ber, const char *str_in )
{
	int rc;
	char	*freeme;
	char	*str;
	char	*next;
	int	parens, balance, escape;

	/*
	 * A Filter looks like this:
	 *      Filter ::= CHOICE {
	 *              and             [0]     SET OF Filter,
	 *              or              [1]     SET OF Filter,
	 *              not             [2]     Filter,
	 *              equalityMatch   [3]     AttributeValueAssertion,
	 *              substrings      [4]     SubstringFilter,
	 *              greaterOrEqual  [5]     AttributeValueAssertion,
	 *              lessOrEqual     [6]     AttributeValueAssertion,
	 *              present         [7]     AttributeType,
	 *              approxMatch     [8]     AttributeValueAssertion,
	 *				extensibleMatch [9]		MatchingRuleAssertion -- LDAPv3
	 *      }
	 *
	 *      SubstringFilter ::= SEQUENCE {
	 *              type               AttributeType,
	 *              SEQUENCE OF CHOICE {
	 *                      initial          [0] IA5String,
	 *                      any              [1] IA5String,
	 *                      final            [2] IA5String
	 *              }
	 *      }
	 *
	 *		MatchingRuleAssertion ::= SEQUENCE {	-- LDAPv3
	 *			matchingRule    [1] MatchingRuleId OPTIONAL,
	 *			type            [2] AttributeDescription OPTIONAL,
	 *			matchValue      [3] AssertionValue,
	 *			dnAttributes    [4] BOOLEAN DEFAULT FALSE }
	 *
	 * Note: tags in a choice are always explicit
	 */

#ifdef NEW_LOGGING
	LDAP_LOG (( "filter", LDAP_LEVEL_ARGS, "ldap_pvt_put_filter: \"%s\"\n",
		str_in ));
#else
	Debug( LDAP_DEBUG_TRACE, "put_filter: \"%s\"\n", str_in, 0, 0 );
#endif

	freeme = LDAP_STRDUP( str_in );
	if( freeme == NULL ) return LDAP_NO_MEMORY;
	str = freeme;

	parens = 0;
	while ( *str ) {
		switch ( *str ) {
		case '(': /*')'*/
			str++;
			parens++;

			/* skip spaces */
			while( LDAP_SPACE( *str ) ) str++;

			switch ( *str ) {
			case '&':
#ifdef NEW_LOGGING
				LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
					"ldap_pvt_put_filter: AND\n" ));
#else
				Debug( LDAP_DEBUG_TRACE, "put_filter: AND\n",
				    0, 0, 0 );
#endif

				str = put_complex_filter( ber, str,
				    LDAP_FILTER_AND, 0 );
				if( str == NULL ) {
					rc = -1;
					goto done;
				}

				parens--;
				break;

			case '|':
#ifdef NEW_LOGGING
				LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
					"ldap_pvt_put_filter: OR\n" ));
#else
				Debug( LDAP_DEBUG_TRACE, "put_filter: OR\n",
				    0, 0, 0 );
#endif

				str = put_complex_filter( ber, str,
				    LDAP_FILTER_OR, 0 );
				if( str == NULL ) {
					rc = -1;
					goto done;
				}

				parens--;
				break;

			case '!':
#ifdef NEW_LOGGING
				LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
					"ldap_pvt_put_filter: NOT\n" ));
#else
				Debug( LDAP_DEBUG_TRACE, "put_filter: NOT\n",
				    0, 0, 0 );
#endif

				str = put_complex_filter( ber, str,
				    LDAP_FILTER_NOT, 0 );
				if( str == NULL ) {
					rc = -1;
					goto done;
				}

				parens--;
				break;

			default:
#ifdef NEW_LOGGING
				LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
					"ldap_pvt_put_filter: simple\n" ));
#else
				Debug( LDAP_DEBUG_TRACE, "put_filter: simple\n",
				    0, 0, 0 );
#endif

				balance = 1;
				escape = 0;
				next = str;

				while ( *next && balance ) {
					if ( escape == 0 ) {
						if ( *next == '(' ) {
							balance++;
						} else if ( *next == ')' ) {
							balance--;
						}
					}

					if ( *next == '\\' && ! escape ) {
						escape = 1;
					} else {
						escape = 0;
					}

					if ( balance ) next++;
				}

				if ( balance != 0 ) {
					rc = -1;
					goto done;
				}

				*next = '\0';

				if ( put_simple_filter( ber, str ) == -1 ) {
					rc = -1;
					goto done;
				}

				*next++ = /*'('*/ ')';

				str = next;
				parens--;
				break;
			}
			break;

		case /*'('*/ ')':
#ifdef NEW_LOGGING
			LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
				"ldap_pvt_put_filter: end\n" ));
#else
			Debug( LDAP_DEBUG_TRACE, "put_filter: end\n",
				0, 0, 0 );
#endif
			if ( ber_printf( ber, /*"["*/ "]" ) == -1 ) {
				rc = -1;
				goto done;
			}
			str++;
			parens--;
			break;

		case ' ':
			str++;
			break;

		default:	/* assume it's a simple type=value filter */
#ifdef NEW_LOGGING
			LDAP_LOG (( "filter", LDAP_LEVEL_DETAIL1, 
				"ldap_pvt_put_filter: default\n" ));
#else
			Debug( LDAP_DEBUG_TRACE, "put_filter: default\n",
				0, 0, 0 );
#endif
			next = strchr( str, '\0' );
			if ( put_simple_filter( ber, str ) == -1 ) {
				rc = -1;
				goto done;
			}
			str = next;
			break;
		}
	}

	rc = parens ? -1 : 0;

done:
	LDAP_FREE( freeme );
	return rc;
}

/*
 * Put a list of filters like this "(filter1)(filter2)..."
 */

static int
put_filter_list( BerElement *ber, char *str, ber_tag_t tag )
{
	char	*next = NULL;
	char	save;

#ifdef NEW_LOGGING
	LDAP_LOG (( "filter", LDAP_LEVEL_ARGS, 
				"put_filter_list \"%s\"\n", str ));
#else
	Debug( LDAP_DEBUG_TRACE, "put_filter_list \"%s\"\n",
		str, 0, 0 );
#endif

	while ( *str ) {
		while ( *str && LDAP_SPACE( (unsigned char) *str ) ) {
			str++;
		}
		if ( *str == '\0' ) break;

		if ( (next = find_right_paren( str + 1 )) == NULL ) {
			return -1;
		}
		save = *++next;

		/* now we have "(filter)" with str pointing to it */
		*next = '\0';
		if ( ldap_pvt_put_filter( ber, str ) == -1 ) return -1;
		*next = save;
		str = next;

		if( tag == LDAP_FILTER_NOT ) break;
	}

	if( tag == LDAP_FILTER_NOT && ( next == NULL || *str )) {
		return -1;
	}

	return 0;
}

static int
put_simple_filter(
	BerElement *ber,
	char *str )
{
	char		*s;
	char		*value;
	ber_tag_t	ftype;
	int		rc = -1;

#ifdef NEW_LOGGING
	LDAP_LOG (( "filter", LDAP_LEVEL_ARGS, 
				"put_simple_filter: \"%s\"\n", str ));
#else
	Debug( LDAP_DEBUG_TRACE, "put_simple_filter: \"%s\"\n",
		str, 0, 0 );
#endif

	str = LDAP_STRDUP( str );
	if( str == NULL ) return -1;

	if ( (s = strchr( str, '=' )) == NULL ) {
		goto done;
	}

	value = s + 1;
	*s-- = '\0';

	switch ( *s ) {
	case '<':
		ftype = LDAP_FILTER_LE;
		*s = '\0';
		break;

	case '>':
		ftype = LDAP_FILTER_GE;
		*s = '\0';
		break;

	case '~':
		ftype = LDAP_FILTER_APPROX;
		*s = '\0';
		break;

	case ':':
		/* RFC2254 extensible filters are off the form:
		 *		type [:dn] [:rule] := value
		 * or	[:dn]:rule := value		
		 */
		ftype = LDAP_FILTER_EXT;
		*s = '\0';

		{
			char *dn = strchr( str, ':' );
			char *rule = NULL;

			if( dn != NULL ) {
				*dn++ = '\0';
				rule = strchr( dn, ':' );

				if( rule == NULL ) {
					/* one colon */
					if ( strcmp(dn, "dn") == 0 ) {
						/* must have attribute */
						if( !ldap_is_desc( str ) ) {
							goto done;
						}

						rule = "";

					} else {
					  rule = dn;
					  dn = NULL;
					}
				
				} else {
					/* two colons */
					*rule++ = '\0';

					if ( strcmp(dn, "dn") != 0 ) {
						/* must have "dn" */
						goto done;
					}
				}

			}

			if ( *str == '\0' && ( !rule || *rule == '\0' ) ) {
				/* must have either type or rule */
				goto done;
			}

			if ( *str != '\0' && !ldap_is_desc( str ) ) {
				goto done;
			}

			if ( rule && *rule != '\0' && !ldap_is_oid( rule ) ) {
				goto done;
			}

			rc = ber_printf( ber, "t{" /*"}"*/, ftype );

			if( rc != -1 && rule && *rule != '\0' ) {
				rc = ber_printf( ber, "ts", LDAP_FILTER_EXT_OID, rule );
			}

			if( rc != -1 && *str != '\0' ) {
				rc = ber_printf( ber, "ts", LDAP_FILTER_EXT_TYPE, str );
			}

			if( rc != -1 ) {
				ber_slen_t len = ldap_pvt_filter_value_unescape( value );

				if( len >= 0 ) {
					rc = ber_printf( ber, "to",
						LDAP_FILTER_EXT_VALUE, value, len );
				} else {
					rc = -1;
				}
			}

			if( rc != -1 && dn ) {
				rc = ber_printf( ber, "tb",
					LDAP_FILTER_EXT_DNATTRS, (ber_int_t) 1 );
			}

			if( rc != -1 ) { 
				rc = ber_printf( ber, /*"{"*/ "N}" );
			}
		}
		goto done;

	default:
		if( !ldap_is_desc( str ) ) {
			goto done;

		} else {
			char *nextstar = ldap_pvt_find_wildcard( value );

			if ( nextstar == NULL ) {
				goto done;

			} else if ( *nextstar == '\0' ) {
				ftype = LDAP_FILTER_EQUALITY;

			} else if ( strcmp( value, "*" ) == 0 ) {
				ftype = LDAP_FILTER_PRESENT;

			} else {
				rc = put_substring_filter( ber, str, value );
				goto done;
			}
		} break;
	}

	if( !ldap_is_desc( str ) ) goto done;

	if ( ftype == LDAP_FILTER_PRESENT ) {
		rc = ber_printf( ber, "ts", ftype, str );

	} else {
		ber_slen_t len = ldap_pvt_filter_value_unescape( value );

		if( len >= 0 ) {
			rc = ber_printf( ber, "t{soN}",
				ftype, str, value, len );
		}
	}

done:
	if( rc != -1 ) rc = 0;
	LDAP_FREE( str );
	return rc;
}

static int
put_substring_filter( BerElement *ber, char *type, char *val )
{
	char *nextstar;
	int gotstar = 0;
	ber_tag_t	ftype = LDAP_FILTER_SUBSTRINGS;

#ifdef NEW_LOGGING
	LDAP_LOG (( "filter", LDAP_LEVEL_ARGS, 
				"put_substring_filter \"%s=%s\"\n", type, val ));
#else
	Debug( LDAP_DEBUG_TRACE, "put_substring_filter \"%s=%s\"\n",
		type, val, 0 );
#endif

	if ( ber_printf( ber, "t{s{" /*"}}"*/, ftype, type ) == -1 ) {
		return -1;
	}

	for( ; *val; val=nextstar ) {
		nextstar = ldap_pvt_find_wildcard( val );

		if ( nextstar == NULL ) {
			return -1;
		}
		
		if ( *nextstar == '\0' ) {
			ftype = LDAP_SUBSTRING_FINAL;
		} else {
			*nextstar++ = '\0';
			if ( gotstar++ == 0 ) {
				ftype = LDAP_SUBSTRING_INITIAL;
			} else {
				ftype = LDAP_SUBSTRING_ANY;
			}
		}

		if ( *val != '\0' || ftype == LDAP_SUBSTRING_ANY ) {
			ber_slen_t len = ldap_pvt_filter_value_unescape( val );

			if ( len < 0  ) {
				return -1;
			}

			if ( ber_printf( ber, "to", ftype, val, len ) == -1 ) {
				return -1;
			}
		}
	}

	if ( ber_printf( ber, /*"{{"*/ "N}N}" ) == -1 ) {
		return -1;
	}

	return 0;
}
