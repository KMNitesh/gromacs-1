/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 */
/*! \internal \file
 * \brief
 * Implements functions in evaluate.h.
 *
 * \todo
 * One of the major bottlenecks for selection performance is that all the
 * evaluation is carried out for atoms.
 * There are several cases when the evaluation could be done for residues
 * or molecules instead, including keywords that select by residue and
 * cases where residue centers are used as reference positions.
 * Implementing this would require a mechanism for recognizing whether
 * something can be evaluated by residue/molecule instead by atom, and
 * converting selections by residue/molecule into selections by atom
 * when necessary.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \ingroup module_selection
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "maths.h"
#include "smalloc.h"
#include "vec.h"

#include "gromacs/selection/indexutil.h"
#include "gromacs/selection/poscalc.h"
#include "gromacs/selection/selection.h"
#include "gromacs/selection/selmethod.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/gmxassert.h"

#include "evaluate.h"
#include "mempool.h"
#include "selectioncollection-impl.h"
#include "selelem.h"

namespace
{

/*! \internal \brief
 * Reserves memory for a selection element from the evaluation memory pool.
 *
 * This class implements RAII semantics for allocating memory for selection
 * element values from a selection evaluation memory pool.
 *
 * \ingroup module_selection
 */
class MempoolSelelemReserver
{
    public:
        //! Constructs a reserver without initial reservation.
        MempoolSelelemReserver() : sel_(NULL) {}
        /*! \brief
         * Constructs a reserver with initial reservation.
         *
         * \param[in,out] sel    Selection element for which to reserve.
         * \param[in]     count  Number of values to reserve.
         *
         * \see reserve()
         */
        MempoolSelelemReserver(t_selelem *sel, int count)
            : sel_(NULL)
        {
            reserve(sel, count);
        }
        //! Frees any memory allocated using this reserver.
        ~MempoolSelelemReserver()
        {
            if (sel_ != NULL)
            {
                _gmx_selelem_mempool_release(sel_);
            }
        }

        /*! \brief
         * Reserves memory for selection element values using this reserver.
         *
         * \param[in,out] sel    Selection element for which to reserve.
         * \param[in]     count  Number of values to reserve.
         *
         * Allocates space to store \p count output values in \p sel from the
         * memory pool associated with \p sel, or from the heap if there is no
         * memory pool.  Type of values to allocate is automatically determined
         * from \p sel.
         */
        void reserve(t_selelem *sel, int count)
        {
            GMX_RELEASE_ASSERT(sel_ == NULL, "Can only reserve one element with one instance");
            _gmx_selelem_mempool_reserve(sel, count);
            sel_ = sel;
        }

    private:
        t_selelem              *sel_;
};

/*! \internal \brief
 * Reserves memory for an index group from the evaluation memory pool.
 *
 * This class implements RAII semantics for allocating memory for an index
 * group from a selection evaluation memory pool.
 *
 * \ingroup module_selection
 */
class MempoolGroupReserver
{
    public:
        /*! \brief
         * Creates a reserver associated with a given memory pool.
         *
         * \param    mp  Memory pool from which to reserve memory.
         */
        explicit MempoolGroupReserver(gmx_sel_mempool_t *mp)
            : mp_(mp), g_(NULL)
        {
        }
        //! Frees any memory allocated using this reserver.
        ~MempoolGroupReserver()
        {
            if (g_ != NULL)
            {
                _gmx_sel_mempool_free_group(mp_, g_);
            }
        }

        /*! \brief
         * Reserves memory for an index group using this reserver.
         *
         * \param[in,out] g      Index group to reserve.
         * \param[in]     count  Number of atoms to reserve space for.
         *
         * Allocates memory from the memory pool to store \p count atoms in
         * \p g.
         */
        void reserve(gmx_ana_index_t *g, int count)
        {
            GMX_RELEASE_ASSERT(g_ == NULL, "Can only reserve one element with one instance");
            _gmx_sel_mempool_alloc_group(mp_, g, count);
            g_ = g;
        }

    private:
        gmx_sel_mempool_t      *mp_;
        gmx_ana_index_t        *g_;
};

/*! \internal \brief
 * Assigns a temporary value for a selection element.
 *
 * This class implements RAII semantics for temporarily assigning the value
 * pointer of a selection element to point to a different location.
 *
 * \ingroup module_selection
 */
class SelelemTemporaryValueAssigner
{
    public:
        //! Constructs an assigner without an initial assignment.
        SelelemTemporaryValueAssigner()
            : sel_(NULL), old_ptr_(NULL), old_nalloc_(0)
        {
        }
        /*! \brief
         * Constructs an assigner with an initial assignment.
         *
         * \param[in,out] sel     Selection element for which to assign.
         * \param[in]     vsource Element to which \p sel values will point to.
         *
         * \see assign()
         */
        SelelemTemporaryValueAssigner(t_selelem *sel, t_selelem *vsource)
            : sel_(NULL)
        {
            assign(sel, vsource);
        }
        //! Undoes any temporary assignment done using this assigner.
        ~SelelemTemporaryValueAssigner()
        {
            if (sel_ != NULL)
            {
                _gmx_selvalue_setstore_alloc(&sel_->v, old_ptr_, old_nalloc_);
            }
        }

        /*! \brief
         * Assigns a temporary value pointer.
         *
         * \param[in,out] sel     Selection element for which to assign.
         * \param[in]     vsource Element to which \p sel values will point to.
         *
         * Assigns the value pointer in \p sel to point to the values in
         * \p vsource, i.e., any access/modification to values in \p sel
         * actually accesses values in \p vsource.
         */
        void assign(t_selelem *sel, t_selelem *vsource)
        {
            GMX_RELEASE_ASSERT(sel_ == NULL,
                               "Can only assign one element with one instance");
            GMX_RELEASE_ASSERT(sel->v.type == vsource->v.type,
                               "Mismatching selection value types");
            old_ptr_ = sel->v.u.ptr;
            old_nalloc_ = sel->v.nalloc;
            _gmx_selvalue_setstore(&sel->v, vsource->v.u.ptr);
            sel_ = sel;
        }

    private:
        t_selelem              *sel_;
        void                   *old_ptr_;
        int                     old_nalloc_;
};

} // namespace

/*!
 * \param[in] fp       File handle to receive the output.
 * \param[in] evalfunc Function pointer to print.
 */
void
_gmx_sel_print_evalfunc_name(FILE *fp, sel_evalfunc evalfunc)
{
    if (!evalfunc)
        fprintf(fp, "none");
    else if (evalfunc == &_gmx_sel_evaluate_root)
        fprintf(fp, "root");
    else if (evalfunc == &_gmx_sel_evaluate_static)
        fprintf(fp, "static");
    else if (evalfunc == &_gmx_sel_evaluate_subexpr_simple)
        fprintf(fp, "subexpr_simple");
    else if (evalfunc == &_gmx_sel_evaluate_subexpr_staticeval)
        fprintf(fp, "subexpr_staticeval");
    else if (evalfunc == &_gmx_sel_evaluate_subexpr)
        fprintf(fp, "subexpr");
    else if (evalfunc == &_gmx_sel_evaluate_subexprref_simple)
        fprintf(fp, "ref_simple");
    else if (evalfunc == &_gmx_sel_evaluate_subexprref)
        fprintf(fp, "ref");
    else if (evalfunc == &_gmx_sel_evaluate_method)
        fprintf(fp, "method");
    else if (evalfunc == &_gmx_sel_evaluate_modifier)
        fprintf(fp, "mod");
    else if (evalfunc == &_gmx_sel_evaluate_not)
        fprintf(fp, "not");
    else if (evalfunc == &_gmx_sel_evaluate_and)
        fprintf(fp, "and");
    else if (evalfunc == &_gmx_sel_evaluate_or)
        fprintf(fp, "or");
    else if (evalfunc == &_gmx_sel_evaluate_arithmetic)
        fprintf(fp, "arithmetic");
    else
        fprintf(fp, "%p", (void*)(evalfunc));
}

/*!
 * \param[out] data Evaluation data structure to initialize.
 * \param[in]  mp   Memory pool for intermediate evaluation values.
 * \param[in]  gall Index group with all the atoms.
 * \param[in]  top  Topology structure for evaluation.
 * \param[in]  fr   New frame for evaluation.
 * \param[in]  pbc  New PBC information for evaluation.
 */
void
_gmx_sel_evaluate_init(gmx_sel_evaluate_t *data,
                       gmx_sel_mempool_t *mp, gmx_ana_index_t *gall,
                       t_topology *top, t_trxframe *fr, t_pbc *pbc)
{
    data->mp   = mp;
    data->gall = gall;
    data->top  = top;
    data->fr   = fr;
    data->pbc  = pbc;
}

/*! \brief
 * Recursively initializes the flags for evaluation.
 *
 * \param[in,out] sel Selection element to clear.
 *
 * The \ref SEL_INITFRAME flag is set for \ref SEL_EXPRESSION elements whose
 * method defines the \p init_frame callback (see sel_framefunc()), and
 * cleared for other elements.
 *
 * The \ref SEL_EVALFRAME flag is cleared for all elements.
 */
static void
init_frame_eval(t_selelem *sel)
{
    while (sel)
    {
        sel->flags &= ~(SEL_INITFRAME | SEL_EVALFRAME);
        if (sel->type == SEL_EXPRESSION)
        {
            if (sel->u.expr.method && sel->u.expr.method->init_frame)
            {
                sel->flags |= SEL_INITFRAME;
            }
        }
        if (sel->child && sel->type != SEL_SUBEXPRREF)
        {
            init_frame_eval(sel->child);
        }
        sel = sel->next;
    }
}

namespace gmx
{

SelectionEvaluator::SelectionEvaluator()
{
}

/*!
 * \param[in,out] coll  The selection collection to evaluate.
 * \param[in] fr  Frame for which the evaluation should be carried out.
 * \param[in] pbc PBC data, or NULL if no PBC should be used.
 * \returns   0 on successful evaluation, a non-zero error code on error.
 *
 * This functions sets the global variables for topology, frame and PBC,
 * clears some information in the selection to initialize the evaluation
 * for a new frame, and evaluates \p sel and all the selections pointed by
 * the \p next pointers of \p sel.
 *
 * This is the only function that user code should call if they want to
 * evaluate a selection for a new frame.
 */
void
SelectionEvaluator::evaluate(SelectionCollection *coll,
                             t_trxframe *fr, t_pbc *pbc)
{
    gmx_ana_selcollection_t *sc = &coll->impl_->sc_;
    gmx_sel_evaluate_t  data;
    t_selelem          *sel;

    _gmx_sel_evaluate_init(&data, sc->mempool, &sc->gall, sc->top, fr, pbc);
    init_frame_eval(sc->root);
    sel = sc->root;
    while (sel)
    {
        /* Clear the evaluation group of subexpressions */
        if (sel->child && sel->child->type == SEL_SUBEXPR)
        {
            sel->child->u.cgrp.isize = 0;
            /* Not strictly necessary, because the value will be overwritten
             * during first evaluation of the subexpression anyways, but we
             * clear the group for clarity. Note that this is _not_ done during
             * compilation because of some additional complexities involved
             * (see compiler.c), so it should not be relied upon in
             * _gmx_sel_evaluate_subexpr(). */
            if (sel->child->v.type == GROUP_VALUE)
            {
                sel->child->v.u.g->isize = 0;
            }
        }
        if (sel->evaluate)
        {
            sel->evaluate(&data, sel, NULL);
        }
        sel = sel->next;
    }
    /* Update selection information */
    SelectionDataList::const_iterator isel;
    for (isel = sc->sel.begin(); isel != sc->sel.end(); ++isel)
    {
        internal::SelectionData &sel = **isel;
        sel.refreshMassesAndCharges();
        sel.updateCoveredFractionForFrame();
    }
}

/*!
 * \param[in,out] coll  The selection collection to evaluate.
 * \param[in]     nframes Total number of frames.
 */
void
SelectionEvaluator::evaluateFinal(SelectionCollection *coll, int nframes)
{
    gmx_ana_selcollection_t *sc = &coll->impl_->sc_;

    SelectionDataList::const_iterator isel;
    for (isel = sc->sel.begin(); isel != sc->sel.end(); ++isel)
    {
        internal::SelectionData &sel = **isel;
        sel.restoreOriginalPositions();
        sel.computeAverageCoveredFraction(nframes);
    }
}

} // namespace gmx

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel  Selection element being evaluated.
 * \param[in] g    Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates each child of \p sel in \p g.
 */
void
_gmx_sel_evaluate_children(gmx_sel_evaluate_t *data, t_selelem *sel,
                           gmx_ana_index_t *g)
{
    t_selelem  *child;

    child = sel->child;
    while (child)
    {
        if (child->evaluate)
        {
            child->evaluate(data, child, g);
        }
        child = child->next;
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated
 *   (not used, can be NULL).
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates the first child element in the group defined by \p sel->u.cgrp.
 * If \p sel->u.cgrp is empty, nothing is done.
 * The value of \p sel is not touched (root elements do not evaluate to
 * values).
 *
 * This function can be used as \c t_selelem::evaluate for \ref SEL_ROOT
 * elements.
 */
void
_gmx_sel_evaluate_root(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    if (sel->u.cgrp.isize == 0 || !sel->child->evaluate)
    {
        return;
    }

    sel->child->evaluate(data, sel->child,
                         sel->u.cgrp.isize < 0 ? NULL : &sel->u.cgrp);
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 for success.
 *
 * Sets the value of \p sel to the intersection of \p g and \p sel->u.cgrp.
 *
 * This function can be used as \c t_selelem::evaluate for \ref SEL_CONST
 * elements with value type \ref GROUP_VALUE.
 */
void
_gmx_sel_evaluate_static(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    gmx_ana_index_intersection(sel->v.u.g, &sel->u.cgrp, g);
}


/*********************************************************************
 * SUBEXPRESSION EVALUATION
 *********************************************************************/

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel  Selection element being evaluated.
 * \param[in] g    Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates the child element (there should be exactly one) in \p g.
 * The compiler has taken care that the child actually stores the evaluated
 * value in the value pointer of this element.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_SUBEXPR
 * elements that are used only once, and hence do not need full subexpression
 * handling.
 */
void
_gmx_sel_evaluate_subexpr_simple(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    if (sel->child->evaluate)
    {
        sel->child->evaluate(data, sel->child, g);
    }
    sel->v.nr = sel->child->v.nr;
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel  Selection element being evaluated.
 * \param[in] g    Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * If this is the first call for this frame, evaluates the child element
 * there should be exactly one in \p g.
 * The compiler has taken care that the child actually stores the evaluated
 * value in the value pointer of this element.
 * Assumes that \p g is persistent for the duration of the whole evaluation.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_SUBEXPR
 * elements that have a static evaluation group, and hence do not need full
 * subexpression handling.
 */
void
_gmx_sel_evaluate_subexpr_staticeval(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    if (sel->u.cgrp.isize == 0)
    {
        sel->child->evaluate(data, sel->child, g);
        sel->v.nr = sel->child->v.nr;
        gmx_ana_index_set(&sel->u.cgrp, g->isize, g->index, sel->u.cgrp.name, 0);
    }
}

/*!
 * \param[in]  data  Data for the current frame.
 * \param[in]  sel   Selection element being evaluated.
 * \param[in]  g     Group for which \p sel should be evaluated.
 * \returns    0 on success, a non-zero error code on error.
 *
 * Finds the part of \p g for which the subexpression
 * has not yet been evaluated by comparing \p g to \p sel->u.cgrp.
 * If the part is not empty, the child expression is evaluated for this
 * part, and the results merged to the old values of the child.
 * The value of \p sel itself is undefined after the call.
 *
 * \todo
 * The call to gmx_ana_index_difference() can take quite a lot of unnecessary
 * time if the subexpression is evaluated either several times for the same
 * group or for completely distinct groups.
 * However, in the majority of cases, these situations occur when
 * _gmx_sel_evaluate_subexpr_staticeval() can be used, so this should not be a
 * major problem.
 */
void
_gmx_sel_evaluate_subexpr(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    gmx_ana_index_t  gmiss;

    MempoolGroupReserver gmissreserver(data->mp);
    if (sel->u.cgrp.isize == 0)
    {
        {
            SelelemTemporaryValueAssigner assigner(sel->child, sel);
            sel->child->evaluate(data, sel->child, g);
        }
        /* We need to keep the name for the cgrp across the copy to avoid
         * problems if g has a name set. */
        char *name = sel->u.cgrp.name;
        gmx_ana_index_copy(&sel->u.cgrp, g, false);
        sel->u.cgrp.name = name;
        gmiss.isize = 0;
    }
    else
    {
        gmissreserver.reserve(&gmiss, g->isize);
        gmx_ana_index_difference(&gmiss, g, &sel->u.cgrp);
    }
    if (gmiss.isize > 0)
    {
        MempoolSelelemReserver reserver(sel->child, gmiss.isize);
        /* Evaluate the missing values for the child */
        sel->child->evaluate(data, sel->child, &gmiss);
        /* Merge the missing values to the existing ones. */
        if (sel->v.type == GROUP_VALUE)
        {
            gmx_ana_index_merge(sel->v.u.g, sel->child->v.u.g, sel->v.u.g);
        }
        else
        {
            int  i, j, k;

            i = sel->u.cgrp.isize - 1;
            j = gmiss.isize - 1;
            /* TODO: This switch is kind of ugly, but it may be difficult to
             * do this portably without C++ templates. */
            switch (sel->v.type)
            {
                case INT_VALUE:
                    for (k = sel->u.cgrp.isize + gmiss.isize - 1; k >= 0; k--)
                    {
                        if (i < 0 || (j >= 0 && sel->u.cgrp.index[i] < gmiss.index[j]))
                        {
                            sel->v.u.i[k] = sel->v.u.i[j--];
                        }
                        else
                        {
                            sel->v.u.i[k] = sel->child->v.u.i[i--];
                        }
                    }
                    break;

                case REAL_VALUE:
                    for (k = sel->u.cgrp.isize + gmiss.isize - 1; k >= 0; k--)
                    {
                        if (i < 0 || (j >= 0 && sel->u.cgrp.index[i] < gmiss.index[j]))
                        {
                            sel->v.u.r[k] = sel->v.u.r[j--];
                        }
                        else
                        {
                            sel->v.u.r[k] = sel->child->v.u.r[i--];
                        }
                    }
                    break;

                case STR_VALUE:
                    for (k = sel->u.cgrp.isize + gmiss.isize - 1; k >= 0; k--)
                    {
                        if (i < 0 || (j >= 0 && sel->u.cgrp.index[i] < gmiss.index[j]))
                        {
                            sel->v.u.s[k] = sel->v.u.s[j--];
                        }
                        else
                        {
                            sel->v.u.s[k] = sel->child->v.u.s[i--];
                        }
                    }
                    break;

                case POS_VALUE:
                    /* TODO: Implement this */
                    GMX_THROW(gmx::NotImplementedError("position subexpressions not implemented properly"));

                case NO_VALUE:
                case GROUP_VALUE:
                    GMX_THROW(gmx::InternalError("Invalid subexpression type"));
            }
        }
        gmx_ana_index_merge(&sel->u.cgrp, &sel->u.cgrp, &gmiss);
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 for success.
 *
 * Sets the value pointers of the child and its child to point to the same
 * memory as the value pointer of this element to avoid copying, and then
 * evaluates evaluates the child.
 *
 * This function is used as \c t_selelem:evaluate for \ref SEL_SUBEXPRREF
 * elements for which the \ref SEL_SUBEXPR does not have other references.
 */
void
_gmx_sel_evaluate_subexprref_simple(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    if (g)
    {
        _gmx_selvalue_setstore(&sel->child->v, sel->v.u.ptr);
        _gmx_selvalue_setstore_alloc(&sel->child->child->v, sel->v.u.ptr,
                                     sel->child->child->v.nalloc);
        sel->child->evaluate(data, sel->child, g);
    }
    sel->v.nr = sel->child->v.nr;
    if (sel->u.param)
    {
        sel->u.param->val.nr = sel->v.nr;
        if (sel->u.param->nvalptr)
        {
            *sel->u.param->nvalptr = sel->u.param->val.nr;
        }
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * If the value type is \ref POS_VALUE, the value of the child is simply
 * copied to set the value of \p sel (the child subexpression should
 * already have been evaluated by its root).
 * If the value type is something else, the child is evaluated for the
 * group \p g, and the value of the child is then copied.
 * There should be only one child element.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_SUBEXPRREF
 * elements.
 */
void
_gmx_sel_evaluate_subexprref(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    t_selelem *expr;
    int        i, j;

    if (g)
    {
        sel->child->evaluate(data, sel->child, g);
    }
    expr = sel->child;
    switch (sel->v.type)
    {
        case INT_VALUE:
            if (!g)
            {
                sel->v.nr = expr->v.nr;
                memcpy(sel->v.u.i, expr->v.u.i, sel->v.nr*sizeof(*sel->v.u.i));
            }
            else
            {
                sel->v.nr = g->isize;
                /* Extract the values corresponding to g */
                for (i = j = 0; i < g->isize; ++i, ++j)
                {
                    while (sel->child->u.cgrp.index[j] < g->index[i])
                    {
                        ++j;
                    }
                    sel->v.u.i[i] = expr->v.u.i[j];
                }
            }
            break;

        case REAL_VALUE:
            if (!g)
            {
                sel->v.nr = expr->v.nr;
                memcpy(sel->v.u.r, expr->v.u.r, sel->v.nr*sizeof(*sel->v.u.r));
            }
            else
            {
                sel->v.nr = g->isize;
                /* Extract the values corresponding to g */
                for (i = j = 0; i < g->isize; ++i, ++j)
                {
                    while (sel->child->u.cgrp.index[j] < g->index[i])
                    {
                        ++j;
                    }
                    sel->v.u.r[i] = expr->v.u.r[j];
                }
            }
            break;

        case STR_VALUE:
            if (!g)
            {
                sel->v.nr = expr->v.nr;
                memcpy(sel->v.u.s, expr->v.u.s, sel->v.nr*sizeof(*sel->v.u.s));
            }
            else
            {
                sel->v.nr = g->isize;
                /* Extract the values corresponding to g */
                for (i = j = 0; i < g->isize; ++i, ++j)
                {
                    while (sel->child->u.cgrp.index[j] < g->index[i])
                    {
                        ++j;
                    }
                    sel->v.u.s[i] = expr->v.u.s[j];
                }
            }
            break;

        case POS_VALUE:
            /* Currently, there is no need to do anything fancy here,
             * but some future extensions may need a more flexible
             * implementation. */
            gmx_ana_pos_copy(sel->v.u.p, expr->v.u.p, false);
            break;

        case GROUP_VALUE:
            if (!g)
            {
                gmx_ana_index_copy(sel->v.u.g, expr->v.u.g, false);
            }
            else
            {
                gmx_ana_index_intersection(sel->v.u.g, expr->v.u.g, g);
            }
            break;

        default: /* should not be reached */
            GMX_THROW(gmx::InternalError("Invalid subexpression reference type"));
    }
    /* Store the number of values if needed */
    if (sel->u.param)
    {
        sel->u.param->val.nr = sel->v.nr;
        if (sel->u.param->nvalptr)
        {
            *sel->u.param->nvalptr = sel->u.param->val.nr;
        }
    }
}

/********************************************************************
 * METHOD EXPRESSION EVALUATION
 ********************************************************************/

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates each child of a \ref SEL_EXPRESSION element.
 * The value of \p sel is not touched.
 *
 * This function is not used as \c t_selelem::evaluate,
 * but is used internally.
 */
void
_gmx_sel_evaluate_method_params(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    t_selelem *child;

    child = sel->child;
    while (child)
    {
        if (child->evaluate && !(child->flags & SEL_EVALFRAME))
        {
            if (child->flags & SEL_ATOMVAL)
            {
                child->evaluate(data, child, g);
            }
            else
            {
                child->flags |= SEL_EVALFRAME;
                child->evaluate(data, child, NULL);
            }
        }
        child = child->next;
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates all child selections (using _gmx_sel_evaluate_method_params())
 * to evaluate any parameter values.
 * If this is the first time this expression is evaluated for
 * the frame, sel_framefunc() callback is called if one is provided.
 * If a reference position calculation has been initialized for this element,
 * the positions are also updated, and sel_updatefunc_pos() is used to
 * evaluate the value. Otherwise, sel_updatefunc() is used.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_EXPRESSION
 * elements.
 */
void
_gmx_sel_evaluate_method(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    _gmx_sel_evaluate_method_params(data, sel, g);
    if (sel->flags & SEL_INITFRAME)
    {
        sel->flags &= ~SEL_INITFRAME;
        sel->u.expr.method->init_frame(data->top, data->fr, data->pbc,
                                       sel->u.expr.mdata);
    }
    if (sel->u.expr.pc)
    {
        gmx_ana_poscalc_update(sel->u.expr.pc, sel->u.expr.pos, g,
                               data->fr, data->pbc);
        sel->u.expr.method->pupdate(data->top, data->fr, data->pbc,
                                    sel->u.expr.pos, &sel->v,
                                    sel->u.expr.mdata);
    }
    else
    {
        sel->u.expr.method->update(data->top, data->fr, data->pbc, g,
                                   &sel->v, sel->u.expr.mdata);
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates all child selections (using _gmx_sel_evaluate_method_params())
 * to evaluate any parameter values.
 * If this is the first time this expression is evaluated for
 * the frame, sel_framefunc() callback is called if one is provided.
 * The modifier is then evaluated using sel_updatefunc_pos().
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_MODIFIER
 * elements.
 */
void
_gmx_sel_evaluate_modifier(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    _gmx_sel_evaluate_method_params(data, sel, g);
    if (sel->flags & SEL_INITFRAME)
    {
        sel->flags &= ~SEL_INITFRAME;
        sel->u.expr.method->init_frame(data->top, data->fr, data->pbc,
                                            sel->u.expr.mdata);
    }
    GMX_RELEASE_ASSERT(sel->child != NULL,
                       "Modifier element with a value must have a child");
    if (sel->child->v.type != POS_VALUE)
    {
        GMX_THROW(gmx::NotImplementedError("Non-position valued modifiers not implemented"));
    }
    sel->u.expr.method->pupdate(data->top, data->fr, data->pbc,
                                sel->child->v.u.p,
                                &sel->v, sel->u.expr.mdata);
}


/********************************************************************
 * BOOLEAN EXPRESSION EVALUATION
 ********************************************************************/

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Evaluates the child element (there should be only one) in the group
 * \p g, and then sets the value of \p sel to the complement of the 
 * child value.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_BOOLEAN
 * elements with \ref BOOL_NOT.
 */
void
_gmx_sel_evaluate_not(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    MempoolSelelemReserver reserver(sel->child, g->isize);
    sel->child->evaluate(data, sel->child, g);
    gmx_ana_index_difference(sel->v.u.g, g, sel->child->v.u.g);
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Short-circuiting evaluation of logical AND expressions.
 *
 * Starts by evaluating the first child element in the group \p g.
 * The each following child element is evaluated in the intersection
 * of all the previous values until all children have been evaluated
 * or the intersection becomes empty.
 * The value of \p sel is set to the intersection of all the (evaluated)
 * child values.
 *
 * If the first child does not have an evaluation function, it is skipped
 * and the evaluation is started at the second child.
 * This happens if the first child is a constant expression and during
 * compilation it was detected that the evaluation group is always a subset
 * of the constant group
 * (currently, the compiler never detects this).
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_BOOLEAN
 * elements with \ref BOOL_AND.
 */
void
_gmx_sel_evaluate_and(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    t_selelem *child;

    child = sel->child;
    /* Skip the first child if it does not have an evaluation function. */
    if (!child->evaluate)
    {
        child = child->next;
    }
    /* Evaluate the first child */
    {
        MempoolSelelemReserver reserver(child, g->isize);
        child->evaluate(data, child, g);
        gmx_ana_index_copy(sel->v.u.g, child->v.u.g, false);
    }
    child = child->next;
    while (child && sel->v.u.g->isize > 0)
    {
        MempoolSelelemReserver reserver(child, sel->v.u.g->isize);
        child->evaluate(data, child, sel->v.u.g);
        gmx_ana_index_intersection(sel->v.u.g, sel->v.u.g, child->v.u.g);
        child = child->next;
    }
}

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel Selection element being evaluated.
 * \param[in] g   Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 *
 * Short-circuiting evaluation of logical OR expressions.
 *
 * Starts by evaluating the first child element in the group \p g.
 * For each subsequent child, finds the part of \p g that is not
 * included the value of any previous child, and evaluates the child
 * in that group until the last child is evaluated or all of \p g
 * is included in some child value.
 * The value of \p sel is set to the union of all the (evaluated)
 * child values.
 *
 * If the first child does not have an evaluation function, its value is
 * used without evaluation.
 * This happens if the first child is a constant expression, the selection
 * has been compiled, and the evaluation group is the same for each frame.
 * In this case, the compiler has taken care of that the child value is a
 * subset of \p g, making it unnecessary to evaluate it.
 *
 * This function is used as \c t_selelem::evaluate for \ref SEL_BOOLEAN
 * elements with \ref BOOL_OR.
 */
void
_gmx_sel_evaluate_or(gmx_sel_evaluate_t *data, t_selelem *sel, gmx_ana_index_t *g)
{
    t_selelem     *child;
    gmx_ana_index_t  tmp, tmp2;

    child = sel->child;
    if (child->evaluate)
    {
        MempoolSelelemReserver reserver(child, g->isize);
        child->evaluate(data, child, g);
        gmx_ana_index_partition(sel->v.u.g, &tmp, g, child->v.u.g);
    }
    else
    {
        gmx_ana_index_partition(sel->v.u.g, &tmp, g, child->v.u.g);
    }
    child = child->next;
    while (child && tmp.isize > 0)
    {
        tmp.name = NULL;
        {
            MempoolSelelemReserver reserver(child, tmp.isize);
            child->evaluate(data, child, &tmp);
            gmx_ana_index_partition(&tmp, &tmp2, &tmp, child->v.u.g);
        }
        sel->v.u.g->isize += tmp.isize;
        tmp.isize = tmp2.isize;
        tmp.index = tmp2.index;
        child = child->next;
    }
    gmx_ana_index_sort(sel->v.u.g);
}


/********************************************************************
 * ARITHMETIC EVALUATION
 ********************************************************************/

/*!
 * \param[in] data Data for the current frame.
 * \param[in] sel  Selection element being evaluated.
 * \param[in] g    Group for which \p sel should be evaluated.
 * \returns   0 on success, a non-zero error code on error.
 */
void
_gmx_sel_evaluate_arithmetic(gmx_sel_evaluate_t *data, t_selelem *sel,
                             gmx_ana_index_t *g)
{
    int         n, i, i1, i2;
    real        lval, rval=0., val=0.;

    t_selelem  *const left  = sel->child;
    t_selelem  *const right = left->next;

    SelelemTemporaryValueAssigner assigner;
    MempoolSelelemReserver reserver;
    if (left->mempool)
    {
        assigner.assign(left, sel);
        if (right)
        {
            reserver.reserve(right, g->isize);
        }
    }
    else if (right && right->mempool)
    {
        assigner.assign(right, sel);
    }
    _gmx_sel_evaluate_children(data, sel, g);

    n = (sel->flags & SEL_SINGLEVAL) ? 1 : g->isize;
    sel->v.nr = n;

    bool bArithNeg = (sel->u.arith.type == ARITH_NEG);
    GMX_ASSERT(right || bArithNeg,
               "Right operand cannot be null except for negations");
    for (i = i1 = i2 = 0; i < n; ++i)
    {
        lval = left->v.u.r[i1];
        if (!bArithNeg)
        {
            rval = right->v.u.r[i2];
        }
        switch (sel->u.arith.type)
        {
            case ARITH_PLUS:    val = lval + rval;     break;
            case ARITH_MINUS:   val = lval - rval;     break;
            case ARITH_NEG:     val = -lval;           break;
            case ARITH_MULT:    val = lval * rval;     break;
            case ARITH_DIV:     val = lval / rval;     break;
            case ARITH_EXP:     val = pow(lval, rval); break;
        }
        sel->v.u.r[i] = val;
        if (!(left->flags & SEL_SINGLEVAL))
        {
            ++i1;
        }
        if (!bArithNeg && !(right->flags & SEL_SINGLEVAL))
        {
            ++i2;
        }
    }
}
