/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */
#include "nsContainerFrame.h"
#include "nsIContent.h"
#include "nsIPresContext.h"
#include "nsIRenderingContext.h"
#include "nsISpaceManager.h"
#include "nsIStyleContext.h"
#include "nsRect.h"
#include "nsPoint.h"
#include "nsGUIEvent.h"
#include "nsStyleConsts.h"
#include "nsIView.h"
#include "nsVoidArray.h"
#include "nsISizeOfHandler.h"
#include "nsHTMLIIDs.h"

#ifdef NS_DEBUG
#undef NOISY
#else
#undef NOISY
#endif

nsContainerFrame::nsContainerFrame(nsIContent* aContent, nsIFrame* aParent)
  : nsSplittableFrame(aContent, aParent)
{
}

nsContainerFrame::~nsContainerFrame()
{
}

NS_IMETHODIMP
nsContainerFrame::SizeOf(nsISizeOfHandler* aHandler) const
{
  aHandler->Add(sizeof(*this));
  nsContainerFrame::SizeOfWithoutThis(aHandler);
  return NS_OK;
}

NS_IMETHODIMP
nsContainerFrame::Init(nsIPresContext& aPresContext, nsIFrame* aChildList)
{
  NS_PRECONDITION(nsnull == mFirstChild, "already initialized");

  mFirstChild = aChildList;
  return NS_OK;
}

NS_IMETHODIMP
nsContainerFrame::DeleteFrame(nsIPresContext& aPresContext)
{
  // Delete our child frames before doing anything else. In particular
  // we do all of this before our base class releases it's hold on the
  // view.
  for (nsIFrame* child = mFirstChild; child; ) {
    mFirstChild = nsnull;  // XXX hack until HandleEvent is not called until after destruction
    nsIFrame* nextChild;
     
    child->GetNextSibling(nextChild);
    child->DeleteFrame(aPresContext);
    child = nextChild;
  }

  nsFrame::DeleteFrame(aPresContext);

  return NS_OK;
}

void
nsContainerFrame::SizeOfWithoutThis(nsISizeOfHandler* aHandler) const
{
  nsSplittableFrame::SizeOfWithoutThis(aHandler);
  for (nsIFrame* child = mFirstChild; child; ) {
    child->SizeOf(aHandler);
    child->GetNextSibling(child);
  }
}

void
nsContainerFrame::PrepareContinuingFrame(nsIPresContext&   aPresContext,
                                         nsIFrame*         aParent,
                                         nsIStyleContext*  aStyleContext,
                                         nsContainerFrame* aContFrame)
{
  // Append the continuing frame to the flow
  aContFrame->AppendToFlow(this);
  aContFrame->SetStyleContext(&aPresContext, aStyleContext);
}

NS_METHOD
nsContainerFrame::DidReflow(nsIPresContext& aPresContext,
                            nsDidReflowStatus aStatus)
{
  NS_FRAME_TRACE_MSG(NS_FRAME_TRACE_CALLS,
                     ("enter nsContainerFrame::DidReflow: status=%d",
                      aStatus));
  if (NS_FRAME_REFLOW_FINISHED == aStatus) {
    nsIFrame* kid;
    FirstChild(kid);
    while (nsnull != kid) {
      nsIHTMLReflow*  htmlReflow;

      if (NS_OK == kid->QueryInterface(kIHTMLReflowIID, (void**)&htmlReflow)) {
        htmlReflow->DidReflow(aPresContext, aStatus);
      }
      kid->GetNextSibling(kid);
    }
  }

  NS_FRAME_TRACE_OUT("nsContainerFrame::DidReflow");

  // Let nsFrame position and size our view (if we have one), and clear
  // the NS_FRAME_IN_REFLOW bit
  return nsFrame::DidReflow(aPresContext, aStatus);
}

/////////////////////////////////////////////////////////////////////////////
// Child frame enumeration

NS_METHOD nsContainerFrame::FirstChild(nsIFrame*& aFirstChild) const
{
  aFirstChild = mFirstChild;
  return NS_OK;
}

/////////////////////////////////////////////////////////////////////////////
// Painting

NS_METHOD nsContainerFrame::Paint(nsIPresContext&      aPresContext,
                                  nsIRenderingContext& aRenderingContext,
                                  const nsRect&        aDirtyRect)
{
  PaintChildren(aPresContext, aRenderingContext, aDirtyRect);
  return NS_OK;
}

// aDirtyRect is in our coordinate system
// child rect's are also in our coordinate system
void nsContainerFrame::PaintChildren(nsIPresContext&      aPresContext,
                                     nsIRenderingContext& aRenderingContext,
                                     const nsRect&        aDirtyRect)
{
  // Set clip rect so that children don't leak out of us
  const nsStyleDisplay* disp =
    (const nsStyleDisplay*)mStyleContext->GetStyleData(eStyleStruct_Display);
  PRBool hidden = PR_FALSE;
  PRBool clipState;
  if (NS_STYLE_OVERFLOW_HIDDEN == disp->mOverflow) {
    aRenderingContext.PushState();
    aRenderingContext.SetClipRect(nsRect(0, 0, mRect.width, mRect.height),
                                  nsClipCombine_kIntersect, clipState);
    hidden = PR_TRUE;
  }

  // See if we should render everything, or just what can be seen
  PRBool renderEverything = PR_TRUE;
  if (NS_STYLE_OVERFLOW_VISIBLE != disp->mOverflow) {
    renderEverything = PR_FALSE;
  }
  // XXX for now, disable this...
  renderEverything = PR_FALSE;

  // XXX reminder: use the coordinates in the dirty rect to figure out
  // which set of children are impacted and only do the intersection
  // work for them. In addition, stop when we no longer overlap.

  nsIFrame* kid = mFirstChild;
  while (nsnull != kid) {
    nsIView *pView;
    kid->GetView(pView);
    if (nsnull == pView) {
      nsRect kidRect;
      kid->GetRect(kidRect);
      nsRect damageArea;
      PRBool overlap = damageArea.IntersectRect(aDirtyRect, kidRect);
#ifdef NS_DEBUG
      if (!overlap && (0 == kidRect.width) && (0 == kidRect.height)) {
        overlap = PR_TRUE;
      }
#endif
//XXX ListTag(stdout); printf(": re=%c overlap=%c dirtyRect={%d,%d,%d,%d} damageArea={%d,%d,%d,%d}\n", renderEverything?'T':'F', overlap?'T':'F', aDirtyRect, damageArea);
      if (renderEverything || overlap) {
        // Translate damage area into kid's coordinate system
        nsRect kidDamageArea(damageArea.x - kidRect.x,
                             damageArea.y - kidRect.y,
                             damageArea.width, damageArea.height);
        aRenderingContext.PushState();
        aRenderingContext.Translate(kidRect.x, kidRect.y);
        kid->Paint(aPresContext, aRenderingContext, kidDamageArea);
#ifdef NS_DEBUG
        if (nsIFrame::GetShowFrameBorders() &&
            (0 != kidRect.width) && (0 != kidRect.height)) {
          nsIView* view;
          GetView(view);
          if (nsnull != view) {
            aRenderingContext.SetColor(NS_RGB(0,0,255));
          }
          else {
            aRenderingContext.SetColor(NS_RGB(255,0,0));
          }
          aRenderingContext.DrawRect(0, 0, kidRect.width, kidRect.height);
        }
#endif
        aRenderingContext.PopState(clipState);
      }
    }
    kid->GetNextSibling(kid);
  }

  if (hidden) {
    aRenderingContext.PopState(clipState);
  }
}

/////////////////////////////////////////////////////////////////////////////
// Events

NS_METHOD nsContainerFrame::HandleEvent(nsIPresContext& aPresContext, 
                                        nsGUIEvent*     aEvent,
                                        nsEventStatus&  aEventStatus)
{
  aEventStatus = nsEventStatus_eIgnore;

  nsIFrame* kid;
  FirstChild(kid);
  while (nsnull != kid) {
    nsRect kidRect;
    kid->GetRect(kidRect);
    if (kidRect.Contains(aEvent->point)) {
      aEvent->point.MoveBy(-kidRect.x, -kidRect.y);
      kid->HandleEvent(aPresContext, aEvent, aEventStatus);
      aEvent->point.MoveBy(kidRect.x, kidRect.y);
      break;
    }
    kid->GetNextSibling(kid);
  }

  return NS_OK;
}

NS_METHOD nsContainerFrame::GetCursorAndContentAt(nsIPresContext& aPresContext,
                                        const nsPoint&  aPoint,
                                        nsIFrame**      aFrame,
                                        nsIContent**    aContent,
                                        PRInt32&        aCursor)
{
  aCursor = NS_STYLE_CURSOR_INHERIT;
  *aContent = mContent;

  nsIFrame* kid;
  FirstChild(kid);
  nsPoint tmp;
  while (nsnull != kid) {
    nsRect kidRect;
    kid->GetRect(kidRect);
    if (kidRect.Contains(aPoint)) {
      tmp.MoveTo(aPoint.x - kidRect.x, aPoint.y - kidRect.y);
      kid->GetCursorAndContentAt(aPresContext, tmp, aFrame, aContent, aCursor);
      break;
    }
    kid->GetNextSibling(kid);
  }
  return NS_OK;
}

/////////////////////////////////////////////////////////////////////////////
// Helper member functions

/**
 * Queries the child frame for the nsIHTMLReflow interface and if it's
 * supported invokes the WillReflow() and Reflow() member functions. If
 * the reflow succeeds and the child frame is complete, deletes any
 * next-in-flows using DeleteChildsNextInFlow()
 */
nsresult
nsContainerFrame::ReflowChild(nsIFrame*                aKidFrame,
                              nsIPresContext&          aPresContext,
                              nsHTMLReflowMetrics&     aDesiredSize,
                              const nsHTMLReflowState& aReflowState,
                              nsReflowStatus&          aStatus)
{
  NS_PRECONDITION(aReflowState.frame == aKidFrame, "bad reflow state");

  // Query for the nsIHTMLReflow interface
  nsIHTMLReflow*  htmlReflow;
  nsresult        result;

  result = aKidFrame->QueryInterface(kIHTMLReflowIID, (void**)&htmlReflow);
  if (NS_FAILED(result)) {
    return result;
  }

  // Send the WillReflow notification, and reflow the child frame
  htmlReflow->WillReflow(aPresContext);
  result = htmlReflow->Reflow(aPresContext, aDesiredSize, aReflowState, aStatus);

  // If the reflow was successful and the child frame is complete, delete any
  // next-in-flows
  if (NS_SUCCEEDED(result) && NS_FRAME_IS_COMPLETE(aStatus)) {
    nsIFrame* kidNextInFlow;
     
    aKidFrame->GetNextInFlow(kidNextInFlow);
    if (nsnull != kidNextInFlow) {
      // Remove all of the childs next-in-flows. Make sure that we ask
      // the right parent to do the removal (it's possible that the
      // parent is not this because we are executing pullup code)
      nsIFrame* parent;
      aKidFrame->GetGeometricParent(parent);
      ((nsContainerFrame*)parent)->DeleteChildsNextInFlow(aPresContext, aKidFrame);
    }
  }

  return result;
}

/**
 * Remove and delete aChild's next-in-flow(s). Updates the sibling and flow
 * pointers
 *
 * Updates the child count and content offsets of all containers that are
 * affected
 *
 * @param   aChild child this child's next-in-flow
 * @return  PR_TRUE if successful and PR_FALSE otherwise
 */
PRBool
nsContainerFrame::DeleteChildsNextInFlow(nsIPresContext& aPresContext, nsIFrame* aChild)
{
  NS_PRECONDITION(IsChild(aChild), "bad geometric parent");

  nsIFrame*         nextInFlow;
  nsContainerFrame* parent;
   
  aChild->GetNextInFlow(nextInFlow);
  NS_PRECONDITION(nsnull != nextInFlow, "null next-in-flow");
  nextInFlow->GetGeometricParent((nsIFrame*&)parent);

  // If the next-in-flow has a next-in-flow then delete it, too (and
  // delete it first).
  nsIFrame* nextNextInFlow;

  nextInFlow->GetNextInFlow(nextNextInFlow);
  if (nsnull != nextNextInFlow) {
    parent->DeleteChildsNextInFlow(aPresContext, nextInFlow);
  }

#ifdef NS_DEBUG
  PRInt32   childCount;
  nsIFrame* firstChild;

  nextInFlow->FirstChild(firstChild);
  childCount = LengthOf(firstChild);

  if ((0 != childCount) || (nsnull != firstChild)) {
    nsIFrame* top = nextInFlow;
    for (;;) {
      nsIFrame* parent;
      top->GetGeometricParent(parent);
      if (nsnull == parent) {
        break;
      }
      top = parent;
    }
    top->List();
  }
  NS_ASSERTION((0 == childCount) && (nsnull == firstChild),
               "deleting !empty next-in-flow");
#endif

  // Disconnect the next-in-flow from the flow list
  nextInFlow->BreakFromPrevFlow();

  // Take the next-in-flow out of the parent's child list
  if (parent->mFirstChild == nextInFlow) {
    nextInFlow->GetNextSibling(parent->mFirstChild);

  } else {
    nsIFrame* nextSibling;

    // Because the next-in-flow is not the first child of the parent
    // we know that it shares a parent with aChild. Therefore, we need
    // to capture the next-in-flow's next sibling (in case the
    // next-in-flow is the last next-in-flow for aChild AND the
    // next-in-flow is not the last child in parent)
    NS_ASSERTION(((nsContainerFrame*)parent)->IsChild(aChild), "screwy flow");
    aChild->GetNextSibling(nextSibling);
    NS_ASSERTION(nextSibling == nextInFlow, "unexpected sibling");

    nextInFlow->GetNextSibling(nextSibling);
    aChild->SetNextSibling(nextSibling);
  }

  // Delete the next-in-flow frame
  WillDeleteNextInFlowFrame(nextInFlow);
  nextInFlow->DeleteFrame(aPresContext);

#ifdef NS_DEBUG
  aChild->GetNextInFlow(nextInFlow);
  NS_POSTCONDITION(nsnull == nextInFlow, "non null next-in-flow");
#endif
  return PR_TRUE;
}

void nsContainerFrame::WillDeleteNextInFlowFrame(nsIFrame* aNextInFlow)
{
}

/**
 * Push aFromChild and its next siblings to the next-in-flow. Change the
 * geometric parent of each frame that's pushed. If there is no next-in-flow
 * the frames are placed on the overflow list (and the geometric parent is
 * left unchanged).
 *
 * Updates the next-in-flow's child count. Does <b>not</b> update the
 * pusher's child count.
 *
 * @param   aFromChild the first child frame to push. It is disconnected from
 *            aPrevSibling
 * @param   aPrevSibling aFromChild's previous sibling. Must not be null. It's
 *            an error to push a parent's first child frame
 */
void nsContainerFrame::PushChildren(nsIFrame* aFromChild, nsIFrame* aPrevSibling)
{
  NS_PRECONDITION(nsnull != aFromChild, "null pointer");
  NS_PRECONDITION(nsnull != aPrevSibling, "pushing first child");
#ifdef NS_DEBUG
  nsIFrame* prevNextSibling;

  aPrevSibling->GetNextSibling(prevNextSibling);
  NS_PRECONDITION(prevNextSibling == aFromChild, "bad prev sibling");
#endif

  // Disconnect aFromChild from its previous sibling
  aPrevSibling->SetNextSibling(nsnull);

  // Do we have a next-in-flow?
  nsContainerFrame* nextInFlow = (nsContainerFrame*)mNextInFlow;
  if (nsnull != nextInFlow) {
    PRInt32   numChildren = 0;
    nsIFrame* lastChild = nsnull;

    // Compute the number of children being pushed, and for each child change
    // its geometric parent. Remember the last child
    for (nsIFrame* f = aFromChild; nsnull != f; f->GetNextSibling(f)) {
      numChildren++;
#ifdef NOISY
      printf("  ");
      ((nsFrame*)f)->ListTag(stdout);
      printf("\n");
#endif
      lastChild = f;
      f->SetGeometricParent(nextInFlow);

      nsIFrame* contentParent;

      f->GetContentParent(contentParent);
      if (this == contentParent) {
        f->SetContentParent(nextInFlow);
      }
    }
    NS_ASSERTION(numChildren > 0, "no children to push");

    // Prepend the frames to our next-in-flow's child list
    lastChild->SetNextSibling(nextInFlow->mFirstChild);
    nextInFlow->mFirstChild = aFromChild;

  } else {
    // Add the frames to our overflow list
    NS_ASSERTION(nsnull == mOverflowList, "bad overflow list");
#ifdef NOISY
    ListTag(stdout);
    printf(": pushing kids to my overflow list\n");
#endif
    mOverflowList = aFromChild;
  }
}

/**
 * Moves any frames on the overflwo lists (the prev-in-flow's overflow list and
 * the receiver's overflow list) to the child list.
 *
 * Updates this frame's child count and content mapping.
 *
 * @return  PR_TRUE if any frames were moved and PR_FALSE otherwise
 */
PRBool nsContainerFrame::MoveOverflowToChildList()
{
  PRBool  result = PR_FALSE;

  // Check for an overflow list with our prev-in-flow
  nsContainerFrame* prevInFlow = (nsContainerFrame*)mPrevInFlow;
  if (nsnull != prevInFlow) {
    if (nsnull != prevInFlow->mOverflowList) {
      NS_ASSERTION(nsnull == mFirstChild, "bad overflow list");
      AppendChildren(prevInFlow->mOverflowList);
      prevInFlow->mOverflowList = nsnull;
      result = PR_TRUE;
    }
  }

  // It's also possible that we have an overflow list for ourselves
  if (nsnull != mOverflowList) {
    NS_ASSERTION(nsnull != mFirstChild, "overflow list but no mapped children");
    AppendChildren(mOverflowList, PR_FALSE);
    mOverflowList = nsnull;
    result = PR_TRUE;
  }

  return result;
}

/**
 * Append child list starting at aChild to this frame's child list. Used for
 * processing of the overflow list.
 *
 * Updates this frame's child count and content mapping.
 *
 * @param   aChild the beginning of the child list
 * @param   aSetParent if true each child's geometric (and content parent if
 *            they're the same) parent is set to this frame.
 */
void nsContainerFrame::AppendChildren(nsIFrame* aChild, PRBool aSetParent)
{
  // Link the frames into our child frame list
  if (nsnull == mFirstChild) {
    // We have no children so aChild becomes the first child
    mFirstChild = aChild;
  } else {
    nsIFrame* lastChild = LastFrame(mFirstChild);
    lastChild->SetNextSibling(aChild);
  }

  // Update our child count and last content offset
  nsIFrame* lastChild;
  for (nsIFrame* f = aChild; nsnull != f; f->GetNextSibling(f)) {
    lastChild = f;

    // Reset the geometric parent if requested
    if (aSetParent) {
      nsIFrame* geometricParent;
      nsIFrame* contentParent;

      f->GetGeometricParent(geometricParent);
      f->GetContentParent(contentParent);
      if (contentParent == geometricParent) {
        f->SetContentParent(this);
      }
      f->SetGeometricParent(this);
    }
  }
}

/////////////////////////////////////////////////////////////////////////////
// Debugging

NS_METHOD nsContainerFrame::List(FILE* out, PRInt32 aIndent, nsIListFilter *aFilter) const
{
  // if a filter is present, only output this frame if the filter says we should
  nsIAtom* tag;
  nsAutoString tagString;
  mContent->GetTag(tag);
  if (tag != nsnull) 
    tag->ToString(tagString);
  PRBool outputMe = (PRBool)((nsnull==aFilter) || (PR_TRUE==aFilter->OutputTag(&tagString)));
  if (PR_TRUE==outputMe)
  {
    // Indent
    for (PRInt32 i = aIndent; --i >= 0; ) fputs("  ", out);

    // Output the tag
    ListTag(out);
    nsIView* view;
    GetView(view);
    if (nsnull != view) {
      fprintf(out, " [view=%p]", view);
    }

    if (nsnull != mPrevInFlow) {
      fprintf(out, "prev-in-flow=%p ", mPrevInFlow);
    }
    if (nsnull != mNextInFlow) {
      fprintf(out, "next-in-flow=%p ", mNextInFlow);
    }

    // Output the rect
    out << mRect;
  }

  // Output the children
  if (nsnull != mFirstChild) {
    if (PR_TRUE==outputMe)
    {
      if (0 != mState) {
        fprintf(out, " [state=%08x]", mState);
      }
      fputs("<\n", out);
    }
    for (nsIFrame* child = mFirstChild; child; child->GetNextSibling(child)) {
      child->List(out, aIndent + 1, aFilter);
    }
    if (PR_TRUE==outputMe)
    {
      for (PRInt32 i = aIndent; --i >= 0; ) fputs("  ", out);
      fputs(">\n", out);
    }
  } else {
    if (PR_TRUE==outputMe)
    {
      if (0 != mState) {
        fprintf(out, " [state=%08x]", mState);
      }
      fputs("<>\n", out);
    }
  }

  return NS_OK;
}

#define VERIFY_ASSERT(_expr, _msg)                \
  if (!(_expr)) {                                 \
    DumpTree();                       \
  }                                               \
  NS_ASSERTION(_expr, _msg)

NS_METHOD nsContainerFrame::VerifyTree() const
{
#ifdef NS_DEBUG
  NS_ASSERTION(0 == (mState & NS_FRAME_IN_REFLOW), "frame is in reflow");
  VERIFY_ASSERT(nsnull == mOverflowList, "bad overflow list");
#endif
  return NS_OK;
}

PRInt32 nsContainerFrame::LengthOf(nsIFrame* aFrame)
{
  PRInt32 result = 0;

  while (nsnull != aFrame) {
    result++;
    aFrame->GetNextSibling(aFrame);
  }

  return result;
}

nsIFrame* nsContainerFrame::LastFrame(nsIFrame* aFrame)
{
  nsIFrame* lastChild = nsnull;

  while (nsnull != aFrame) {
    lastChild = aFrame;
    aFrame->GetNextSibling(aFrame);
  }

  return lastChild;
}

nsIFrame* nsContainerFrame::FrameAt(nsIFrame* aFrame, PRInt32 aIndex)
{
  while ((aIndex-- > 0) && (aFrame != nsnull)) {
    aFrame->GetNextSibling(aFrame);
  }
  return aFrame;
}

/////////////////////////////////////////////////////////////////////////////

#ifdef NS_DEBUG

PRBool nsContainerFrame::IsChild(const nsIFrame* aChild) const
{
  // Check the geometric parent
  nsIFrame* parent;

  aChild->GetGeometricParent(parent);
  if (parent != (nsIFrame*)this) {
    return PR_FALSE;
  }

  return PR_TRUE;
}

void nsContainerFrame::DumpTree() const
{
  nsIFrame* root = (nsIFrame*)this;
  nsIFrame* parent = mGeometricParent;

  while (nsnull != parent) {
    root = parent;
    parent->GetGeometricParent(parent);
  }

  root->List();
}

#endif
