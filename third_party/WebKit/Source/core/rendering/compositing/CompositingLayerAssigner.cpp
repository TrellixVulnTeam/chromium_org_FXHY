/*
 * Copyright (C) 2009, 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2014 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "core/rendering/compositing/CompositingLayerAssigner.h"

#include "core/inspector/InspectorTraceEvents.h"
#include "core/rendering/compositing/CompositedLayerMapping.h"
#include "platform/TraceEvent.h"

namespace blink {

// We will only allow squashing if the bbox-area:squashed-area doesn't exceed
// the ratio |gSquashingSparsityTolerance|:1.
static uint64_t gSquashingSparsityTolerance = 6;

CompositingLayerAssigner::CompositingLayerAssigner(RenderLayerCompositor* compositor)
    : m_compositor(compositor)
    , m_layerSquashingEnabled(compositor->layerSquashingEnabled())
    , m_layersChanged(false)
{
}

CompositingLayerAssigner::~CompositingLayerAssigner()
{
}

void CompositingLayerAssigner::assign(RenderLayer* updateRoot, Vector<RenderLayer*>& layersNeedingPaintInvalidation)//为Render Layer创建或者删除Composited Layer Mapping
{
    TRACE_EVENT0("blink", "CompositingLayerAssigner::assign");

    SquashingState squashingState;
    assignLayersToBackingsInternal(updateRoot, squashingState, layersNeedingPaintInvalidation);
    if (squashingState.hasMostRecentMapping)
        squashingState.mostRecentMapping->finishAccumulatingSquashingLayers(squashingState.nextSquashedLayerIndex);
}

void CompositingLayerAssigner::SquashingState::updateSquashingStateForNewMapping(CompositedLayerMapping* newCompositedLayerMapping, bool hasNewCompositedLayerMapping)
{
    // The most recent backing is done accumulating any more squashing layers.
    if (hasMostRecentMapping)
        mostRecentMapping->finishAccumulatingSquashingLayers(nextSquashedLayerIndex);

    nextSquashedLayerIndex = 0;
    boundingRect = IntRect();
    mostRecentMapping = newCompositedLayerMapping;
    hasMostRecentMapping = hasNewCompositedLayerMapping;
    haveAssignedBackingsToEntireSquashingLayerSubtree = false;
}

bool CompositingLayerAssigner::squashingWouldExceedSparsityTolerance(const RenderLayer* candidate, const CompositingLayerAssigner::SquashingState& squashingState)
{
    IntRect bounds = candidate->clippedAbsoluteBoundingBox();
    IntRect newBoundingRect = squashingState.boundingRect;
    newBoundingRect.unite(bounds);
    const uint64_t newBoundingRectArea = newBoundingRect.size().area();
    const uint64_t newSquashedArea = squashingState.totalAreaOfSquashedRects + bounds.size().area();
    return newBoundingRectArea > gSquashingSparsityTolerance * newSquashedArea;
}

bool CompositingLayerAssigner::needsOwnBacking(const RenderLayer* layer) const
{
    if (!m_compositor->canBeComposited(layer))//判断layer描述的Render Layer是否需要Compositing
        return false;

    // If squashing is disabled, then layers that would have been squashed should just be separately composited.
    bool needsOwnBackingForDisabledSquashing = !m_layerSquashingEnabled && requiresSquashing(layer->compositingReasons());
//如果参数layer描述的Render Layer满足以下3个条件之一，那么CompositingLayerAssigner类的成员函数needsOwnBacking就会认为它需要进行Compositing：
//1. Render Layer的Compositing Reason表示它需要Compositing，这是通过调用前面提到的函数requiresCompositing判断的。
//2. Render Layer的Compositing Reason表示它需要Squashing，但是浏览器禁用了“Layer Squashing”机制。当浏览器禁用“Layer Squashing”机制时，CompositingLayerAssigner类的成员变量m_layerSquashingEnabled会等于false。调用前面提到的函数requiresSquashing可以判断一个Render Layer是否需要Squashing。
//3. Render Layer是Render Layer Tree的根节点，并且Render Layer Compositor处于Compositing模式中。除非设置了Render Layer Tree的根节点无条件Compositing，否则的话，当在Render Layer Tree根节点的子树中，没有任何Render Layer需要Compositing时， Render Layer Tree根节点也不需要Compositing，这时候Render Layer Compositor就会被设置为非Compositing模式。判断一个Render Layer是否是Render Layer Tree的根节点，调用它的成员函数isRootLayer即可，而判断一个Render Layer Compositor是否处于Compositing模式，调用它的成员函数staleInCompositingMode即可。
    return requiresCompositing(layer->compositingReasons()) || needsOwnBackingForDisabledSquashing || (m_compositor->staleInCompositingMode() && layer->isRootLayer());
}

CompositingStateTransitionType CompositingLayerAssigner::computeCompositedLayerUpdate(RenderLayer* layer)
{
//一个Render Layer的Compositing State Transition分为4种:
//1. 它需要Compositing，但是还没有创建Composited Layer Mapping，这时候Compositing State Transition设置为AllocateOwnCompositedLayerMapping，表示要创建一个新的Composited Layer Mapping。
//2. 它不需要Compositing，但是之前已经创建有Composited Layer Mapping，这时候Compositing State Transition设置为RemoveOwnCompositedLayerMapping，表示要删除之前创建的Composited Layer Mapping。
//3. 它需要Squashing，这时候Compositing State Transition设置为PutInSquashingLayer，表示要将它绘制离其最近的一个Render Layer的Composited Layer Mapping里面的一个Squashing Layer上。
//4. 它不需要Squashing，这时候Compositing State Transition设置为RemoveFromSquashingLayer，表示要将它从原来对应的Squashing Layer上删除。
    CompositingStateTransitionType update = NoCompositingStateChange;
    if (needsOwnBacking(layer)) {//判断是否需要合成
        if (!layer->hasCompositedLayerMapping()) {
            update = AllocateOwnCompositedLayerMapping;
        }
    } else {
        if (layer->hasCompositedLayerMapping())
            update = RemoveOwnCompositedLayerMapping;

        if (m_layerSquashingEnabled) {//默认情况下浏览器是开启Layer Squashing机制的，不过可以通过设置disable-layer-squashing进行关闭，或者通过enable-layer-squashing显示开启
            if (!layer->subtreeIsInvisible() && requiresSquashing(layer->compositingReasons())) {
                // We can't compute at this time whether the squashing layer update is a no-op,
                // since that requires walking the render layer tree.
                update = PutInSquashingLayer;
            } else if (layer->groupedMapping() || layer->lostGroupedMapping()) {
                update = RemoveFromSquashingLayer;
            }
        }
    }
    return update;
}

CompositingReasons CompositingLayerAssigner::getReasonsPreventingSquashing(const RenderLayer* layer, const CompositingLayerAssigner::SquashingState& squashingState)
{
    if (!squashingState.haveAssignedBackingsToEntireSquashingLayerSubtree)
        return CompositingReasonSquashingWouldBreakPaintOrder;

    ASSERT(squashingState.hasMostRecentMapping);
    const RenderLayer& squashingLayer = squashingState.mostRecentMapping->owningLayer();

    // FIXME: this special case for video exists only to deal with corner cases
    // where a RenderVideo does not report that it needs to be directly composited.
    // Video does not currently support sharing a backing, but this could be
    // generalized in the future. The following layout tests fail if we permit the
    // video to share a backing with other layers.
    //
    // compositing/video/video-controls-layer-creation.html
    if (layer->renderer()->isVideo() || squashingLayer.renderer()->isVideo())
        return CompositingReasonSquashingVideoIsDisallowed;

    // Don't squash iframes, frames or plugins.
    // FIXME: this is only necessary because there is frame code that assumes that composited frames are not squashed.
    if (layer->renderer()->isRenderPart() || squashingLayer.renderer()->isRenderPart())
        return CompositingReasonSquashingRenderPartIsDisallowed;

    if (layer->reflectionInfo())
        return CompositingReasonSquashingReflectionIsDisallowed;

    if (squashingWouldExceedSparsityTolerance(layer, squashingState))
        return CompositingReasonSquashingSparsityExceeded;

    if (layer->renderer()->hasBlendMode())
        return CompositingReasonSquashingBlendingIsDisallowed;

    // FIXME: this is not efficient, since it walks up the tree. We should store these values on the CompositingInputsCache.
    if (layer->clippingContainer() != squashingLayer.clippingContainer() && !squashingLayer.compositedLayerMapping()->containingSquashedLayer(layer->clippingContainer(), squashingState.nextSquashedLayerIndex))
        return CompositingReasonSquashingClippingContainerMismatch;

    // Composited descendants need to be clipped by a child containment graphics layer, which would not be available if the layer is
    // squashed (and therefore has no CLM nor a child containment graphics layer).
    if (m_compositor->clipsCompositingDescendants(layer))
        return CompositingReasonSquashedLayerClipsCompositingDescendants;

    if (layer->scrollsWithRespectTo(&squashingLayer))
        return CompositingReasonScrollsWithRespectToSquashingLayer;

    const RenderLayer::AncestorDependentCompositingInputs& compositingInputs = layer->ancestorDependentCompositingInputs();
    const RenderLayer::AncestorDependentCompositingInputs& squashingLayerCompositingInputs = squashingLayer.ancestorDependentCompositingInputs();

    if (compositingInputs.opacityAncestor != squashingLayerCompositingInputs.opacityAncestor)
        return CompositingReasonSquashingOpacityAncestorMismatch;

    if (compositingInputs.transformAncestor != squashingLayerCompositingInputs.transformAncestor)
        return CompositingReasonSquashingTransformAncestorMismatch;

    if (layer->hasFilter() || compositingInputs.filterAncestor != squashingLayerCompositingInputs.filterAncestor)
        return CompositingReasonSquashingFilterMismatch;

    return CompositingReasonNone;
}

void CompositingLayerAssigner::updateSquashingAssignment(RenderLayer* layer, SquashingState& squashingState, const CompositingStateTransitionType compositedLayerUpdate,
    Vector<RenderLayer*>& layersNeedingPaintInvalidation)
{
    // NOTE: In the future as we generalize this, the background of this layer may need to be assigned to a different backing than
    // the squashed RenderLayer's own primary contents. This would happen when we have a composited negative z-index element that needs
    // to paint on top of the background, but below the layer's main contents. For now, because we always composite layers
    // when they have a composited negative z-index child, such layers will never need squashing so it is not yet an issue.
    if (compositedLayerUpdate == PutInSquashingLayer) {//也是根据Render Layer的Compositing State Transition Type决定是否要将它绘制在一个Squashing Graphics Layer中，或者将它从一个Squashing Graphics Layer中删除。
         //对于Compositing State Transition Type等于PutInSquashingLayer的Render Layer，它将会绘制在一个Squashing Graphics Layer中。这个Squashing Graphics Layer保存在一个Composited Layer Mapping中。这个Composited Layer Mapping关联的Render Layer处于要Squashing的Render Layer的下面，并且前者离后者是最近的，记录在参数squashingState描述的一个SquashingState对象的成员变量mostRecentMapping中。通过调用CompositedLayerMapping类的成员函数updateSquashingLayerAssignment可以将一个Render Layer绘制在一个Composited Layer Mapping内部维护的一个quashing Graphics Layer中。
        // A layer that is squashed with other layers cannot have its own CompositedLayerMapping.
        ASSERT(!layer->hasCompositedLayerMapping());
        ASSERT(squashingState.hasMostRecentMapping);

        bool changedSquashingLayer =
            squashingState.mostRecentMapping->updateSquashingLayerAssignment(layer, squashingState.mostRecentMapping->owningLayer(), squashingState.nextSquashedLayerIndex);
        if (!changedSquashingLayer)
            return;

        // If we've modified the collection of squashed layers, we must update
        // the graphics layer geometry.
        squashingState.mostRecentMapping->setNeedsGraphicsLayerUpdate(GraphicsLayerUpdateSubtree);

        layer->clipper().clearClipRectsIncludingDescendants();

        // Issue a paint invalidation, since |layer| may have been added to an already-existing squashing layer.
        TRACE_LAYER_INVALIDATION(layer, InspectorLayerInvalidationTrackingEvent::AddedToSquashingLayer);
        layersNeedingPaintInvalidation.append(layer);
        m_layersChanged = true;
    } else if (compositedLayerUpdate == RemoveFromSquashingLayer) {//对于Compositing State Transition Type等于RemoveFromSquashingLayer的Render Layer，如果它之前已经被设置绘制在一个Squashing Graphics Layer中，那么就需要将它从这个Squashing Graphics Layer中删除。如果一个Render Layer之前被设置绘制在一个Squashing Graphics Layer中，那么调用它的成员函数groupedMapping就可以获得一个Grouped Mapping。这个Grouped Mapping描述的也是一个Composited Layer Mapping，并且Render Layer所绘制在的Squashing Graphics Layer就是由这个Composited Layer Mapping维护的。因此，要将一个Render Layer从一个Squashing Graphics Layer中删除，只要将它的Grouped Mapping设置为0即可。这是通过调用RenderLayer类的成员函数setGroupedMapping实现的。
        if (layer->groupedMapping()) {
            // Before removing |layer| from an already-existing squashing layer that may have other content, issue a paint invalidation.
            m_compositor->paintInvalidationOnCompositingChange(layer);
            layer->groupedMapping()->setNeedsGraphicsLayerUpdate(GraphicsLayerUpdateSubtree);
            layer->setGroupedMapping(0);
        }

        // If we need to issue paint invalidations, do so now that we've removed it from a squashed layer.
        TRACE_LAYER_INVALIDATION(layer, InspectorLayerInvalidationTrackingEvent::RemovedFromSquashingLayer);
        layersNeedingPaintInvalidation.append(layer);
        m_layersChanged = true;

        layer->setLostGroupedMapping(false);
    }
}

void CompositingLayerAssigner::assignLayersToBackingsForReflectionLayer(RenderLayer* reflectionLayer, Vector<RenderLayer*>& layersNeedingPaintInvalidation)
{
    CompositingStateTransitionType compositedLayerUpdate = computeCompositedLayerUpdate(reflectionLayer);
    if (compositedLayerUpdate != NoCompositingStateChange) {
        TRACE_LAYER_INVALIDATION(reflectionLayer, InspectorLayerInvalidationTrackingEvent::ReflectionLayerChanged);
        layersNeedingPaintInvalidation.append(reflectionLayer);
        m_layersChanged = true;
        m_compositor->allocateOrClearCompositedLayerMapping(reflectionLayer, compositedLayerUpdate);
    }
    m_compositor->updateDirectCompositingReasons(reflectionLayer);

    // FIXME: Why do we updateGraphicsLayerConfiguration here instead of in the GraphicsLayerUpdater?
    if (reflectionLayer->hasCompositedLayerMapping())
        reflectionLayer->compositedLayerMapping()->updateGraphicsLayerConfiguration();
}

void CompositingLayerAssigner::assignLayersToBackingsInternal(RenderLayer* layer, SquashingState& squashingState, Vector<RenderLayer*>& layersNeedingPaintInvalidation)//从这个根节点开始，递归是否需要为每一个Render Layer创建或者删除Composited Layer Mapping。
{
    if (m_layerSquashingEnabled && requiresSquashing(layer->compositingReasons())) {
        CompositingReasons reasonsPreventingSquashing = getReasonsPreventingSquashing(layer, squashingState);
        if (reasonsPreventingSquashing)
            layer->setCompositingReasons(layer->compositingReasons() | reasonsPreventingSquashing);
    }

    CompositingStateTransitionType compositedLayerUpdate = computeCompositedLayerUpdate(layer);//计算参数layer描述的Render Layer的Compositing State Transition

    if (m_compositor->allocateOrClearCompositedLayerMapping(layer, compositedLayerUpdate)) {
        TRACE_LAYER_INVALIDATION(layer, InspectorLayerInvalidationTrackingEvent::NewCompositedLayer);
        layersNeedingPaintInvalidation.append(layer);
        m_layersChanged = true;
    }

    // FIXME: special-casing reflection layers here is not right.
    if (layer->reflectionInfo())
        assignLayersToBackingsForReflectionLayer(layer->reflectionInfo()->reflectionLayer(), layersNeedingPaintInvalidation);

    // Add this layer to a squashing backing if needed.
    if (m_layerSquashingEnabled) {//说明开启了Layer-Squashing机制
        updateSquashingAssignment(layer, squashingState, compositedLayerUpdate, layersNeedingPaintInvalidation);//判断是否需要将参数layer描述的Render layer绘制在一个Squashing Graphics Layer中

        const bool layerIsSquashed = compositedLayerUpdate == PutInSquashingLayer || (compositedLayerUpdate == NoCompositingStateChange && layer->groupedMapping());
        if (layerIsSquashed) {
            squashingState.nextSquashedLayerIndex++;
            IntRect layerBounds = layer->clippedAbsoluteBoundingBox();
            squashingState.totalAreaOfSquashedRects += layerBounds.size().area();
            squashingState.boundingRect.unite(layerBounds);
        }
    }

    if (layer->stackingNode()->isStackingContext()) {//判断参数layer描述的Render Layer所关联的Render Object是否是一个Stacking Context。如果是的话，那么就递归调用成员函数assignLayersToBackingsInternal遍历那些z-index为负数的子Render Object对应的Render Layer，确定是否需要为它们创建Composited Layer Mapping。
        RenderLayerStackingNodeIterator iterator(*layer->stackingNode(), NegativeZOrderChildren);
        while (RenderLayerStackingNode* curNode = iterator.next())
            assignLayersToBackingsInternal(curNode->layer(), squashingState, layersNeedingPaintInvalidation);
    }
//到目前为止，CompositingLayerAssigner类的成员函数assignLayersToBackingsInternal就处理完成参数layer描述的Render Layer，以及那些z-index为负数的子Render Layer。这时候，参数layer描述的Render Layer可能会作为那些z-index大于等于0的子Render Layer的Grouped Mapping，因此在继续递归处理z-index大于等于0的子Render Layer之前，CompositingLayerAssigner类的成员函数assignLayersToBackingsInternal需要将参数layer描述的Render Layer对应的Composited Layer Mapping记录下来，前提是这个Render Layer拥有Composited Layer Mapping。这是通过调用参数squashingState描述的一个SquashingState对象的成员函数updateSquashingStateForNewMapping实现的，实际上就是记录在该SquashingState对象的成员变量mostRecentMapping中。这样前面分析的CompositingLayerAssigner类的成员函数updateSquashingAssignment就可以知道将其参数layer描述的Render Layer绘制在哪一个Squashing Graphics Layer中。
    if (m_layerSquashingEnabled) {
        // At this point, if the layer is to be "separately" composited, then its backing becomes the most recent in paint-order.
        if (layer->compositingState() == PaintsIntoOwnBacking || layer->compositingState() == HasOwnBackingButPaintsIntoAncestor) {
            ASSERT(!requiresSquashing(layer->compositingReasons()));
            squashingState.updateSquashingStateForNewMapping(layer->compositedLayerMapping(), layer->hasCompositedLayerMapping());
        }
    }

    if (layer->scrollParent())
        layer->scrollParent()->scrollableArea()->setTopmostScrollChild(layer);

    if (layer->needsCompositedScrolling())
        layer->scrollableArea()->setTopmostScrollChild(0);
//递归调用自己处理那些z-index大于等于0的子Render Layer。递归调用完成之后，整个Render Layer Tree就处理完毕了。这时候哪些Render Layer具有Composited Layer Mapping就可以确定了。
    RenderLayerStackingNodeIterator iterator(*layer->stackingNode(), NormalFlowChildren | PositiveZOrderChildren);
    while (RenderLayerStackingNode* curNode = iterator.next())
        assignLayersToBackingsInternal(curNode->layer(), squashingState, layersNeedingPaintInvalidation);

    if (squashingState.hasMostRecentMapping && &squashingState.mostRecentMapping->owningLayer() == layer)
        squashingState.haveAssignedBackingsToEntireSquashingLayerSubtree = true;
}

}
