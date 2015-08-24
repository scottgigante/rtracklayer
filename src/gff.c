#include "XVector_interface.h"
#include "S4Vectors_interface.h"

#include <ctype.h>   /* for isdigit() and isspace() */
#include <stdlib.h>  /* for strtod() */
#include <string.h>  /* for memcpy() and memcmp() */

/*
 * Turn string pointed by 'val' into an int. The string has no terminating
 * null byte ('\0') and must have the following format:
 *     ^[[:space:]]*[+-]?[[:digit:]]+[[:space:]]*$
 * Return NA_INTEGER if the string is malformed or if it represents a integer
 * value that cannot be represented by an int (int overflow).
 * TODO: Maybe implement this on top of strtol(). Would be much simpler but
 * would it be equivalent? Also would it be as fast? See how as_double() below
 * is implemented on top of strtod().
 */
#define	LEADING_SPACE 0
#define	NUMBER 1
#define	TRAILING_SPACE 2
static int as_int(const char *val, int val_len)
{
	int n, ndigit, sign, status, i;
	char c;

	n = ndigit = 0;
	sign = 1;
	status = LEADING_SPACE;
	for (i = 0; i < val_len; i++) {
		c = val[i];
		if (isdigit(c)) {
			if (status == TRAILING_SPACE)
				return NA_INTEGER;  /* malformed string */
			status = NUMBER;
			ndigit++;
			n = safe_int_mult(n, 10);
			n = safe_int_add(n, c - '0');
			if (n == NA_INTEGER)
				return NA_INTEGER;  /* int overflow */
			continue;
		}
		if (c == '+' || c == '-') {
			if (status != LEADING_SPACE)
				return NA_INTEGER;  /* malformed string */
			status = NUMBER;
			if (c == '-')
				sign = -1;
			continue;
		}
		if (!isspace(c))
			return NA_INTEGER;  /* malformed string */
		if (status == NUMBER) {
			if (ndigit == 0)
				return NA_INTEGER;  /* malformed string */
			status = TRAILING_SPACE;
		}
	}
	if (ndigit == 0)
		return NA_INTEGER;  /* malformed string */
	if (sign == -1)
		n = -n;
	return n;
}

static double as_double(const char *val, int val_len)
{
	double x;
	char *end_conversion, c;
	int end_offset, i;

	x = strtod(val, &end_conversion);
	end_offset = end_conversion - val;
	if (end_offset == 0)
		return NA_REAL;
	for (i = end_offset; i < val_len; i++) {
		c = val[i];
		if (!isspace(c))
			return NA_REAL;
	}
	return x;
}

/* See http://www.sequenceontology.org/resources/gff3.html for the official
 * GFF3 specs. */

static const char *col_names[] = {
	"seqid",
	"source",
	"type",
	"start",
	"end",
	"score",
	"strand",
	"phase",
	"attributes"
};

static const SEXPTYPE col_types[] = {
	STRSXP,   /* seqid */
	STRSXP,   /* source */
	STRSXP,   /* type */
	INTSXP,   /* start */
	INTSXP,   /* end */
	REALSXP,  /* score */
	STRSXP,   /* strand */
	INTSXP,   /* phase */
	STRSXP    /* attributes */
};

#define	GFF_NCOL ((int) (sizeof(col_names) / sizeof(char *)))

/* --- .Call ENTRY POINT --- */
SEXP gff_colnames()
{
	SEXP ans, ans_elt;
	int col_idx;

	PROTECT(ans = NEW_CHARACTER(GFF_NCOL));
	for (col_idx = 0; col_idx < GFF_NCOL; col_idx++) {
		PROTECT(ans_elt = mkChar(col_names[col_idx]));
		SET_STRING_ELT(ans, col_idx, ans_elt);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return ans;
}

#define	SEQID_IDX 0
#define	SOURCE_IDX 1
#define	TYPE_IDX 2
#define	START_IDX 3
#define	END_IDX 4
#define	SCORE_IDX 5
#define	STRAND_IDX 6
#define	PHASE_IDX 7
#define	ATTRIBUTES_IDX 8

static int prepare_colmap0(int *colmap0, SEXP colmap)
{
	int ans_ncol0, col_idx, j;

	ans_ncol0 = 0;
	for (col_idx = 0; col_idx < GFF_NCOL; col_idx++) {
		j = INTEGER(colmap)[col_idx];
		if (j != NA_INTEGER) {
			if (j > ans_ncol0)
				ans_ncol0 = j;
			j--;
		}
		colmap0[col_idx] = j;
	}
	return ans_ncol0;
}

static SEXP alloc_ans(int ans_nrow, int ans_ncol0,
		const int *colmap0, SEXP tags,
		CharAEAE *tags_buf, CharAEAE *pragmas_buf)
{
	int ans_ntag, ans_ncol, col_idx, j, i;
	SEXP ans, ans_attr, ans_names, ans_col, col_name, tags_elt;
	SEXPTYPE col_type;
	CharAE **ae_p, *ae;

	if (tags == R_NilValue)
		ans_ntag = CharAEAE_get_nelt(tags_buf);
	else
		ans_ntag = LENGTH(tags);
	ans_ncol = ans_ncol0 + ans_ntag;

	PROTECT(ans = NEW_LIST(ans_ncol));
	PROTECT(ans_names = NEW_CHARACTER(ans_ncol));

	/* Alloc main columns. */
	for (col_idx = 0; col_idx < GFF_NCOL; col_idx++) {
		j = colmap0[col_idx];
		if (j == NA_INTEGER)
			continue;
		col_type = col_types[col_idx];
		PROTECT(ans_col = allocVector(col_type, ans_nrow));
		SET_ELEMENT(ans, j, ans_col);
		UNPROTECT(1);
		PROTECT(col_name = mkChar(col_names[col_idx]));
		SET_STRING_ELT(ans_names, j, col_name);
		UNPROTECT(1);
		j++;
	}

	/* Alloc tag columns. */
	for (j = ans_ncol0; j < ans_ncol; j++) {
		PROTECT(ans_col = NEW_CHARACTER(ans_nrow));
		for (i = 0; i < ans_nrow; i++)
			SET_STRING_ELT(ans_col, i, NA_STRING);
		SET_ELEMENT(ans, j, ans_col);
		UNPROTECT(1);
	}
	if (tags == R_NilValue) {
		for (j = ans_ncol0, ae_p = tags_buf->elts;
		     j < ans_ncol;
		     j++, ae_p++)
		{
			ae = *ae_p;
			PROTECT(col_name = mkCharLen(ae->elts,
						     CharAE_get_nelt(ae)));
			SET_STRING_ELT(ans_names, j, col_name);
			UNPROTECT(1);
		}
	} else {
		for (j = ans_ncol0; j < ans_ncol; j++) {
			tags_elt = STRING_ELT(tags, j - ans_ncol0);
			PROTECT(col_name = duplicate(tags_elt));
			SET_STRING_ELT(ans_names, j, col_name);
			UNPROTECT(1);
		}
	}
	SET_NAMES(ans, ans_names);

	/* list_as_data_frame() performs IN-PLACE coercion */
	list_as_data_frame(ans, ans_nrow);

	/* Set additional attributes. */
	PROTECT(ans_attr = ScalarInteger(ans_ncol0));
	SET_ATTR(ans, install("ncol0"), ans_attr);
	UNPROTECT(1);
	PROTECT(ans_attr = ScalarInteger(ans_ntag));
	SET_ATTR(ans, install("ntag"), ans_attr);
	UNPROTECT(1);
	PROTECT(ans_attr = new_CHARACTER_from_CharAEAE(pragmas_buf));
	SET_ATTR(ans, install("pragmas"), ans_attr);
	UNPROTECT(3);
	return ans;
}

static void load_string(SEXP ans_col, int row_idx,
		const char *data, int data_len)
{
	SEXP tmp;

	PROTECT(tmp = mkCharLen(data, data_len));
	SET_STRING_ELT(ans_col, row_idx, tmp);
	UNPROTECT(1);
	return;
}

static void load_int(SEXP ans_col, int row_idx,
		const char *data, int data_len)
{
	INTEGER(ans_col)[row_idx] = as_int(data, data_len);
	return;
}

static void load_double(SEXP ans_col, int row_idx,
		const char *data, int data_len)
{
	REAL(ans_col)[row_idx] = as_double(data, data_len);
	return;
}

static void load_data(SEXP ans, int row_idx, int col_idx, const int *colmap0,
		const char *data, int data_len)
{
	SEXP ans_col;
	SEXPTYPE col_type;

	ans_col = VECTOR_ELT(ans, colmap0[col_idx]);
	col_type = col_types[col_idx];
	switch (col_type) {
	    case STRSXP:
		if (data_len == 1) {
			if (col_idx == STRAND_IDX
			 && (data[0] == '.' || data[0] == '?'))
			{
				data = "*";
				data_len = 1;
			} else if (data[0] == '.') {
				SET_STRING_ELT(ans_col, row_idx, NA_STRING);
				break;
			}
		}
		load_string(ans_col, row_idx, data, data_len);
	    break;
	    case INTSXP:
		load_int(ans_col, row_idx, data, data_len);
	    break;
	    case REALSXP:
		load_double(ans_col, row_idx, data, data_len);
	    break;
	}
	return;
}

static void load_tagval(SEXP ans, int row_idx,
		const char *tagval, int tag_len, int tagval_len)
{
	SEXP ans_names, col_name, ans_col;
	int ans_ncol0, j;

	ans_ncol0 = INTEGER(GET_ATTR(ans, install("ncol0")))[0];
	/* Lookup the current tag in 'names(ans)', starting from the end.
	   Since the number of tags in 'names(ans)' is typically very small
	   (< 25), we do this in a naive way (no hashing), because it's simple
	   and seems to be fast enough. */
	ans_names = GET_NAMES(ans);
	for (j = LENGTH(ans_names) - 1; j >= ans_ncol0; j--) {
		col_name = STRING_ELT(ans_names, j);
		if (LENGTH(col_name) == tag_len
		 && memcmp(CHAR(col_name), tagval, tag_len) == 0)
			break;
	}
	if (j < ans_ncol0)
		return;  /* 'tag' was not found ==> nothing to do */
	ans_col = VECTOR_ELT(ans, j);
	load_string(ans_col, row_idx,
		    tagval + tag_len + 1, tagval_len - tag_len - 1);
	return;
}

#define IOBUF_SIZE 20002
static char errmsg_buf[200];

static void add_tag_to_buf(const char *tag, int tag_len, CharAEAE *tags_buf)
{
	CharAE **ae_p, *ae;
	int ntag, i;

	/* We want to store unique tags in 'tags_buf' so we first check
	   to see if 'tag' is already stored and don't do anything if it is.
	   Since the number of unique tags in a GFF file is typically very
	   small (< 25), we do this in a naive way (no hashing), because it's
	   simple and seems to be fast enough. */
	ntag = CharAEAE_get_nelt(tags_buf);
	for (i = 0, ae_p = tags_buf->elts; i < ntag; i++, ae_p++) {
		ae = *ae_p;
		if (CharAE_get_nelt(ae) == tag_len
                 && memcmp(ae->elts, tag, tag_len) == 0)
			return;  /* 'tag' was found ==> nothing to do */
	}
	/* 'tag' was not found ==> add it */
	ae = new_CharAE(tag_len);
	CharAE_set_nelt(ae, tag_len);
	memcpy(ae->elts, tag, tag_len);
	CharAEAE_insert_at(tags_buf, ntag, ae);
	return;
}

static const char *parse_GFF_tagval(const char *tagval, int tagval_len,
		SEXP ans, int row_idx, CharAEAE *tags_buf)
{
	int tag_len;
	char c;

	for (tag_len = 0; tag_len < tagval_len; tag_len++) {
		c = tagval[tag_len];
		if (c == '=')
			break;
	}
	/* If 'tagval' is not in the tag=value format (i.e. if it has no =)
	   then we simply ignore it. */
	if (tag_len == tagval_len)
		return NULL;
	if (ans != R_NilValue)
		load_tagval(ans, row_idx, tagval, tag_len, tagval_len);
	if (tags_buf != NULL)
		add_tag_to_buf(tagval, tag_len, tags_buf);
	return NULL;
}

static const char *parse_GFF_attributes(const char *data, int data_len,
		SEXP ans, int row_idx, CharAEAE *tags_buf)
{
	const char *tagval;
	int tagval_len, i;
	char c;
	const char *errmsg;

	tagval = data;
	tagval_len = 0;
	for (i = 0; i < data_len; i++) {
		c = data[i];
		if (c != ';') {
			tagval_len++;
			continue;
		}
		errmsg = parse_GFF_tagval(tagval, tagval_len,
					  ans, row_idx, tags_buf);
		if (errmsg != NULL)
			return errmsg;
		tagval = data + i + 1;
		tagval_len = 0;
	}
	errmsg = parse_GFF_tagval(tagval, tagval_len,
				  ans, row_idx, tags_buf);
	if (errmsg != NULL)
		return errmsg;
	return NULL;
}

static const char *parse_GFF_line(const char *line, int lineno,
		SEXP feature_types,
		SEXP ans, int *row_idx, const int *colmap0,
		CharAEAE *tags_buf)
{
	int col_idx, i, data_len;
	const char *data;
	char c;
	const char *errmsg;

	col_idx = i = 0;
	data = line;
	data_len = 0;
	while ((c = line[i++])) {
		if (c != '\t') {
			data_len++;
			continue;
		}
		if (col_idx >= GFF_NCOL - 1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "line %d has more than %d tabs",
				 lineno, GFF_NCOL - 1);
			return errmsg_buf;
		}
		if (ans != R_NilValue && colmap0[col_idx] != NA_INTEGER)
			load_data(ans, *row_idx, col_idx, colmap0,
				  data, data_len);
		if (col_idx == ATTRIBUTES_IDX) {
			errmsg = parse_GFF_attributes(data, data_len,
						      ans, *row_idx, tags_buf);
			if (errmsg != NULL)
				return errmsg;
		}
		col_idx++;
		data = line + i;
		data_len = 0;
	}
	if (col_idx < GFF_NCOL - 2) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "line %d has less than %d tabs",
			 lineno, GFF_NCOL - 2);
		return errmsg_buf;
	}
	data_len = delete_trailing_LF_or_CRLF(data, data_len);
	if (ans != R_NilValue && colmap0[col_idx] != NA_INTEGER)
		load_data(ans, *row_idx, col_idx, colmap0,
			  data, data_len);
	if (col_idx == ATTRIBUTES_IDX) {
		errmsg = parse_GFF_attributes(data, data_len,
					      ans, *row_idx, tags_buf);
		if (errmsg != NULL)
			return errmsg;
	}
	(*row_idx)++;
	return NULL;
}

static const char *parse_GFF_file(SEXP filexp, SEXP feature_types,
		int *ans_nrow, SEXP ans, const int *colmap0,
		CharAEAE *tags_buf, CharAEAE *pragmas_buf)
{
	int row_idx, lineno, ret_code, EOL_in_buf;
	char buf[IOBUF_SIZE], c;
	const char *errmsg;

	row_idx = 0;
	for (lineno = 1;
	     (ret_code = filexp_gets(filexp, buf, IOBUF_SIZE, &EOL_in_buf));
	     lineno += EOL_in_buf)
	{
		if (ret_code == -1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "read error while reading characters "
				 "from line %d", lineno);
			return errmsg_buf;
		}
		if (!EOL_in_buf) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "cannot read line %d, "
				 "line is too long", lineno);
			return errmsg_buf;
		}
		c = buf[0];
		if (c == '\n' || (c == '\r' && buf[1] == '\n'))
			continue;  /* skip empty line */
		if (c == '#') {
			if (buf[1] != '#')
				continue;  /* skip human-readable comment */
			if (pragmas_buf != NULL)
				append_string_to_CharAEAE(pragmas_buf, buf);
			continue;
		}
		if (c == '>')
			break;  /* stop parsing at first FASTA header */
		errmsg = parse_GFF_line(buf, lineno, feature_types,
					ans, &row_idx, colmap0,
					tags_buf);
		if (errmsg != NULL)
			return errmsg;
	}
	*ans_nrow = row_idx;
	return NULL;
}

/* --- .Call ENTRY POINT ---
 * Args:
 *   filexp:        A "file external pointer" (see src/io_utils.c in the
 *                  XVector package).
 *   colmap:        An integer vector of length GFF_NCOL indicating which
 *                  columns to load and in which order.
 *   tags:          NULL or a character vector indicating which tags to load.
 *                  NULL means load all tags.
 *   feature_types: NULL or a character vector of feature types. Only rows
 *                  with that type are loaded. NULL means load all rows.
 */
SEXP gff_read(SEXP filexp, SEXP colmap, SEXP tags, SEXP feature_types)
{
	int colmap0[GFF_NCOL], ans_nrow, ans_ncol0;
	const char *errmsg;
	CharAEAE *pragmas_buf, *tags_buf;
	SEXP ans;

	ans_ncol0 = prepare_colmap0(colmap0, colmap);
	if (!(feature_types == R_NilValue || IS_CHARACTER(feature_types)))
		error("'feature_types' must be NULL or a character vector");

	/* 1st pass */
	if (tags == R_NilValue) {
		tags_buf = new_CharAEAE(0, 0);
	} else {
		tags_buf = NULL;
	}
	pragmas_buf = new_CharAEAE(0, 0);
	filexp_rewind(filexp);
	errmsg = parse_GFF_file(filexp, feature_types,
				&ans_nrow, R_NilValue, colmap0,
				tags_buf, pragmas_buf);
	if (errmsg != NULL)
		error("reading GFF file: %s", errmsg);

	/* 2nd pass */
	PROTECT(ans = alloc_ans(ans_nrow, ans_ncol0, colmap0, tags,
				tags_buf, pragmas_buf));
	filexp_rewind(filexp);
	errmsg = parse_GFF_file(filexp, feature_types,
				&ans_nrow, ans, colmap0,
				NULL, NULL);
	if (errmsg != NULL)
		error("reading GFF file: %s", errmsg);
	UNPROTECT(1);
	return ans;
}
