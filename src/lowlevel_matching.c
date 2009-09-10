/****************************************************************************
 *              Utility functions related to pattern matching               *
 *                           Author: Herve Pages                            *
 ****************************************************************************/
#include "Biostrings.h"
#include "IRanges_interface.h"

static int debug = 0;

SEXP debug_lowlevel_matching()
{
#ifdef DEBUG_BIOSTRINGS
	debug = !debug;
	Rprintf("Debug mode turned %s in file %s\n",
		debug ? "on" : "off", __FILE__);
#else
	Rprintf("Debug mode not available in file %s\n", __FILE__);
#endif
	return R_NilValue;
}


/****************************************************************************
 * nmismatch_at_Pshift_*()
 *
 * The 4 static functions below stop counting mismatches if the number
 * exceeds 'max_nmis'. The caller can disable this by passing 'P->length' to
 * the 'max_nmis' arg.
 *
 * fixedP | fixedS | letters *p and *s match iff...
 * --------------------------------------------------------
 * TRUE   | TRUE   | ...they are equal
 * TRUE   | FALSE  | ...bits at 1 in *p are also at 1 in *s
 * FALSE  | TRUE   | ...bits at 1 in *s are also at 1 in *p
 * FALSE  | FALSE  | ...they share at least one bit at 1
 */

static int nmismatch_at_Pshift_fixedPfixedS(const cachedCharSeq *P,
		const cachedCharSeq *S, int Pshift, int max_nmis)
{
	int nmis, i, j;
	const char *p, *s;

	nmis = 0;
	for (i = 0, j = Pshift, p = P->seq, s = S->seq + Pshift;
	     i < P->length;
	     i++, j++, p++, s++)
	{
		if (j >= 0 && j < S->length && *p == *s)
			continue;
		if (nmis++ >= max_nmis)
			break;
	}
	return nmis;
}

static int nmismatch_at_Pshift_fixedPnonfixedS(const cachedCharSeq *P,
		const cachedCharSeq *S, int Pshift, int max_nmis)
{
	int nmis, i, j;
	const char *p, *s;

	nmis = 0;
	for (i = 0, j = Pshift, p = P->seq, s = S->seq + Pshift;
	     i < P->length;
	     i++, j++, p++, s++)
	{
		if (j >= 0 && j < S->length && ((*p) & ~(*s)) == 0)
			continue;
		if (nmis++ >= max_nmis)
			break;
	}
	return nmis;
}

static int nmismatch_at_Pshift_nonfixedPfixedS(const cachedCharSeq *P,
		const cachedCharSeq *S, int Pshift, int max_nmis)
{
	int nmis, i, j;
	const char *p, *s;

	nmis = 0;
	for (i = 0, j = Pshift, p = P->seq, s = S->seq + Pshift;
	     i < P->length;
	     i++, j++, p++, s++)
	{
		if (j >= 0 && j < S->length && (~(*p) & (*s)) == 0)
			continue;
		if (nmis++ >= max_nmis)
			break;
	}
	return nmis;
}

static int nmismatch_at_Pshift_nonfixedPnonfixedS(const cachedCharSeq *P,
		const cachedCharSeq *S, int Pshift, int max_nmis)
{
	int nmis, i, j;
	const char *p, *s;

	nmis = 0;
	for (i = 0, j = Pshift, p = P->seq, s = S->seq + Pshift;
	     i < P->length;
	     i++, j++, p++, s++)
	{
		if (j >= 0 && j < S->length && ((*p) & (*s)))
			continue;
		if (nmis++ >= max_nmis)
			break;
	}
	return nmis;
}

int (*_selected_nmismatch_at_Pshift_fun)(const cachedCharSeq *P,
		const cachedCharSeq *S, int Pshift, int max_nmis);

void _select_nmismatch_at_Pshift_fun(int fixedP, int fixedS)
{
	if (fixedP) {
		if (fixedS)
			_selected_nmismatch_at_Pshift_fun = &nmismatch_at_Pshift_fixedPfixedS;
		else
			_selected_nmismatch_at_Pshift_fun = &nmismatch_at_Pshift_fixedPnonfixedS;
	} else {
		if (fixedS)
			_selected_nmismatch_at_Pshift_fun = &nmismatch_at_Pshift_nonfixedPfixedS;
		else
			_selected_nmismatch_at_Pshift_fun = &nmismatch_at_Pshift_nonfixedPnonfixedS;
	}
	return;
}


/****************************************************************************
 * An edit distance implementation with early bailout.
 */

/*
 * TODO: (maybe) replace static alloc of buffers by dynamic alloc.
 */
#define MAX_NEDIT 100
#define MAX_ROW_LENGTH (2*MAX_NEDIT+1)

static int row1_buf[MAX_ROW_LENGTH], row2_buf[MAX_ROW_LENGTH];

#define SWAP_NEDIT_BUFS(prev_row, curr_row) \
{ \
	int *tmp; \
	tmp = (prev_row); \
	(prev_row) = (curr_row); \
	(curr_row) = tmp; \
}

#define PROPAGATE_NEDIT(curr_row, B, prev_row, S, j, Pc, row_length) \
{ \
	int nedit, B2, nedit2; \
	nedit = (prev_row)[(B)] + ((j) < 0 || (j) >= (S)->length || (S)->seq[(j)] != (Pc)); \
	if ((B2 = (B) - 1) >= 0 && (nedit2 = (curr_row)[B2] + 1) < nedit) \
		nedit = nedit2; \
	if ((B2 = (B) + 1) < (row_length) && (nedit2 = (prev_row)[B2] + 1) < nedit) \
		nedit = nedit2; \
	(curr_row)[(B)] = nedit; \
}

#ifdef DEBUG_BIOSTRINGS
static void print_curr_row(const char* margin, const int *curr_row, int Bmin, int row_length)
{
	int B;

	Rprintf("[DEBUG]   %s: ", margin);
	for (B = 0; B < row_length; B++) {
		if (B < Bmin)
			Rprintf("%3s", "");
		else
			Rprintf("%3d", curr_row[B]);
	}
	Rprintf("\n");
	return;
}
#endif

/*
 * P left-offset (Ploffset) is the offset of P's first letter in S.
 * P right-offset (Proffset) is the offset of P's last letter in S.
 * The min width (min_width) is the length of the shortest substring S'
 * of S starting at Ploffset (or ending at Proffset) for which nedit(P, S')
 * is minimal.
 * TODO: Implement the 'loose_Ploffset' feature (allowing or not an indel
 * on the first letter of the local alignement).
 */
int _nedit_for_Ploffset(const cachedCharSeq *P, const cachedCharSeq *S,
		int Ploffset, int max_nedit, int loose_Ploffset, int *min_width)
{
	int max_nedit_plus1, *prev_row, *curr_row, row_length,
	    B, b, i, iplus1, jmin, j, min_nedit;
	char Pc;

#ifdef DEBUG_BIOSTRINGS
	if (debug) Rprintf("[DEBUG] _nedit_for_Ploffset():\n");
#endif
	if (P->length == 0)
		return 0;
	if (max_nedit == 0)
		error("Biostrings internal error in _nedit_for_Ploffset(): ",
		      "use _selected_nmismatch_at_Pshift_fun() when 'max_nedit' is 0");
	max_nedit_plus1 = max_nedit + 1;
	if (max_nedit > P->length)
		max_nedit = P->length;
	// from now max_nedit <= P->length
	if (max_nedit > MAX_NEDIT)
		error("'max.nedit' too big");
	prev_row = row1_buf;
	curr_row = row2_buf;
	row_length = 2 * max_nedit + 1;
	jmin = Ploffset;

	// STAGE 0:
	for (B = max_nedit, b = 0; B < row_length; B++, b++)
		curr_row[B] = b;
#ifdef DEBUG_BIOSTRINGS
	if (debug) print_curr_row("STAGE0", curr_row, max_nedit, row_length);
#endif

	// STAGE 1 (1st for() loop): no attempt is made to bailout during
	// this stage because the smallest value in curr_row is guaranteed
	// to be <= iplus1 < max_nedit.
	for (iplus1 = 1, i = 0; iplus1 < max_nedit; iplus1++, i++) {
		Pc = P->seq[i]; // i < iplus1 < max_nedit <= P->length
		SWAP_NEDIT_BUFS(prev_row, curr_row);
		B = max_nedit - iplus1;
		curr_row[B++] = iplus1;
		for (j = jmin; B < row_length; B++, j++)
			PROPAGATE_NEDIT(curr_row, B, prev_row, S, j, Pc, row_length);
#ifdef DEBUG_BIOSTRINGS
		if (debug) print_curr_row("STAGE1", curr_row, max_nedit - iplus1, row_length);
#endif
	}

	// STAGE 2: no attempt is made to bailout during this stage either.
	Pc = P->seq[i];
	SWAP_NEDIT_BUFS(prev_row, curr_row);
	B = 0;
	curr_row[B++] = min_nedit = iplus1;
	*min_width = 0;
	for (j = jmin; B < row_length; B++, j++) {
		PROPAGATE_NEDIT(curr_row, B, prev_row, S, j, Pc, row_length);
		if (curr_row[B] < min_nedit) {
			min_nedit = curr_row[B];
			*min_width = j - Ploffset + 1;
		}
	}
#ifdef DEBUG_BIOSTRINGS
	if (debug) print_curr_row("STAGE2", curr_row, 0, row_length);
#endif
	iplus1++;
	i++;

	// STAGE 3 (2nd for() loop): with attempt to bailout.
	for ( ; i < P->length; i++, iplus1++, jmin++) {
		Pc = P->seq[i];
		SWAP_NEDIT_BUFS(prev_row, curr_row);
		min_nedit = iplus1;
		*min_width = 0;
		for (B = 0, j = jmin; B < row_length; B++, j++) {
			PROPAGATE_NEDIT(curr_row, B, prev_row, S, j, Pc, row_length);
			if (curr_row[B] < min_nedit) {
				min_nedit = curr_row[B];
				*min_width = j - Ploffset + 1;
			}
		}
#ifdef DEBUG_BIOSTRINGS
		if (debug) print_curr_row("STAGE3", curr_row, 0, row_length);
#endif
		if (min_nedit >= max_nedit_plus1) // should never be min_nedit > max_nedit_plus1
			break; // bailout
	}
	return min_nedit;
}

int _nedit_for_Proffset(const cachedCharSeq *P, const cachedCharSeq *S,
		int Proffset, int max_nedit, int loose_Proffset, int *min_width)
{
	int max_nedit_plus1, *prev_row, *curr_row, row_length,
	    B, b, i, iplus1, jmin, j, min_nedit;
	char Pc;

	min_nedit = 0;
	error("_nedit_for_Proffset() is not ready yet, sorry!");
	return min_nedit;
}


/****************************************************************************
 * match_pattern_at()
 */

static void match_pattern_at(const cachedCharSeq *P, const cachedCharSeq *S,
		SEXP at, int at_type0, int max_nmis, int min_nmis, int indels,
		int ans_type0, int *ans_elt)
{
	int at_length, i, *at_elt, offset, nmis, min_width, is_matching;

	if (ans_type0 >= 2)
		*ans_elt = NA_INTEGER;
	at_length = LENGTH(at);
	for (i = 0, at_elt = INTEGER(at); i < at_length; i++, at_elt++)
	{
		if (*at_elt == NA_INTEGER) {
			switch (ans_type0) {
				case 0: *(ans_elt++) = NA_INTEGER; break;
				case 1: *(ans_elt++) = NA_LOGICAL; break;
			}
			continue;
		}
		if (indels) {
			offset = *at_elt - 1;
			if (at_type0 == 0)
				nmis = _nedit_for_Ploffset(P, S, offset, max_nmis, 1, &min_width);
			else
				nmis = _nedit_for_Proffset(P, S, offset, max_nmis, 1, &min_width);
		} else {
			if (at_type0 == 0)
				offset = *at_elt - 1;
			else
				offset = *at_elt - P->length;
			nmis = _selected_nmismatch_at_Pshift_fun(P, S, offset, max_nmis);
		}
		if (ans_type0 == 0) {
			*(ans_elt++) = nmis;
			continue;
		}
		is_matching = nmis <= max_nmis && nmis >= min_nmis;
		if (ans_type0 == 1) {
			*(ans_elt++) = is_matching;
			continue;
		}
		if (is_matching) {
			*ans_elt = ans_type0 == 2 ? i + 1 : *at_elt;
			break;
		}
	}
	return;
}



/****************************************************************************
 *                        --- .Call ENTRY POINTS ---                        *
 ****************************************************************************/

/*
 * XString_match_pattern_at() arguments:
 *   pattern: XString object of same base type as 'subject'.
 *   subject: XString object.
 *   at: the 1-based positions of 'pattern' with respect to 'subject'.
 *   at_type: how to interpret the positions in 'at'. If 0, then they are
 *            those of the first letter of 'pattern'. If 1, then they are
 *            those of its last letter.
 *   max_mismatch: if the number of effective mismatches is <= 'max_mismatch',
 *            then it is reported accurately. Otherwise any number >
 *            'max_mismatch' could be reported. This is to allow the matching
 *            functions used as backends to which XString_match_pattern_at()
 *            delegates to implement early bail out strategies.
 *   with_indels: TRUE or FALSE. If TRUE, then the "number of mismatches" at
 *            a given position means the smallest edit distance between the
 *            'pattern' and all the substrings in 'subject' that start (if
 *            'at_type' is 0) or end (if 'at_type' is 1) at this position.
 *   fixed: a logical of length 2.
 *   ans_type: a single integer specifying the type of answer to return:
 *       0: ans is an integer vector of the same length as 'at';
 *       1: ans is a logical vector of the same length as 'at';
 *       2: ans is the lowest *index* (1-based position) in 'at' for which
 *          a match occurred (or NA if no match occurred);
 *       3: ans is the first *value* in 'at' for which a match occurred
 *          (or NA if no match occurred).
 */
SEXP XString_match_pattern_at(SEXP pattern, SEXP subject, SEXP at, SEXP at_type,
		SEXP max_mismatch, SEXP min_mismatch, SEXP with_indels, SEXP fixed,
		SEXP ans_type)
{
	cachedCharSeq P, S;
	int at_length, at_type0, max_nmis, min_nmis, indels, fixedP, fixedS,
	    ans_type0, *ans_elt;
	SEXP ans;

	P = cache_XRaw(pattern);
	S = cache_XRaw(subject);
	at_length = LENGTH(at);
	at_type0 = INTEGER(at_type)[0];
	max_nmis = INTEGER(max_mismatch)[0];
	min_nmis = INTEGER(min_mismatch)[0];
	indels = LOGICAL(with_indels)[0] && max_nmis != 0;
	fixedP = LOGICAL(fixed)[0];
	fixedS = LOGICAL(fixed)[1];
	if (indels && !(fixedP && fixedS))
		error("when 'with.indels' is TRUE, only 'fixed=TRUE' is supported for now");
	ans_type0 = INTEGER(ans_type)[0];
	switch (ans_type0) {
		case 0:
			PROTECT(ans = NEW_INTEGER(at_length));
			ans_elt = INTEGER(ans);
			break;
		case 1:
			PROTECT(ans = NEW_LOGICAL(at_length));
			ans_elt = LOGICAL(ans);
			break;
		case 2: case 3:
			PROTECT(ans = NEW_INTEGER(1));
			ans_elt = INTEGER(ans);
			break;
		default: error("invalid 'ans_type' value (%d)", ans_type0);
	}
	if (!indels)
		_select_nmismatch_at_Pshift_fun(fixedP, fixedS);

	match_pattern_at(&P, &S, at, at_type0, max_nmis, min_nmis, indels, ans_type0, ans_elt);
	UNPROTECT(1);
	return ans;
}


/*
 * XStringSet_vmatch_pattern_at() arguments are the same as for
 * XString_match_pattern_at() except for:
 *   subject: XStringSet object.
 */
SEXP XStringSet_vmatch_pattern_at(SEXP pattern, SEXP subject, SEXP at, SEXP at_type,
		SEXP max_mismatch, SEXP min_mismatch, SEXP with_indels, SEXP fixed,
		SEXP ans_type)
{
	cachedCharSeq P, S_elt;
	cachedXStringSet S;
	int S_length, at_length, at_type0, max_nmis, min_nmis, indels, fixedP, fixedS,
	    ans_type0, *ans_elt, ans_nrow, i;
	SEXP ans;

	P = cache_XRaw(pattern);
	S = _cache_XStringSet(subject);
	S_length = _get_cachedXStringSet_length(&S);
	at_length = LENGTH(at);
	at_type0 = INTEGER(at_type)[0];
	max_nmis = INTEGER(max_mismatch)[0];
	min_nmis = INTEGER(min_mismatch)[0];
	indels = LOGICAL(with_indels)[0] && max_nmis != 0;
	fixedP = LOGICAL(fixed)[0];
	fixedS = LOGICAL(fixed)[1];
	if (indels && !(fixedP && fixedS))
		error("when 'with.indels' is TRUE, only 'fixed=TRUE' is supported for now");
	ans_type0 = INTEGER(ans_type)[0];
	switch (ans_type0) {
		case 0:
			PROTECT(ans = allocMatrix(INTSXP, at_length, S_length));
			ans_elt = INTEGER(ans);
			ans_nrow = at_length;
			break;
		case 1:
			PROTECT(ans = allocMatrix(LGLSXP, at_length, S_length));
			ans_elt = LOGICAL(ans);
			ans_nrow = at_length;
			break;
		case 2: case 3:
			PROTECT(ans = NEW_INTEGER(S_length));
			ans_elt = INTEGER(ans);
			ans_nrow = 1;
			break;
		default: error("invalid 'ans_type' value (%d)", ans_type0);
	}
	if (!indels)
		_select_nmismatch_at_Pshift_fun(fixedP, fixedS);

	for (i = 0; i < S_length; i++, ans_elt += ans_nrow) {
		S_elt = _get_cachedXStringSet_elt(&S, i);
		match_pattern_at(&P, &S_elt, at, at_type0,
				 max_nmis, min_nmis, indels, ans_type0, ans_elt);
	}
	UNPROTECT(1);
	return ans;
}
