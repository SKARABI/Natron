/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ViewerTab.h"
#include "ViewerTabPrivate.h"

#include <cassert>
#include <stdexcept>

#include <QtCore/QDebug>
#include <QtCore/QThread>
#include <QtCore/QCoreApplication>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QtGui/QKeyEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON

#include "Engine/Node.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/KnobTypes.h"
#include "Engine/OSGLFunctions.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Engine/ViewerNode.h"

#include "Gui/NodeSettingsPanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h" // isKeybind
#include "Gui/GuiAppInstance.h"
#include "Gui/NodeGui.h"
#include "Gui/QtEnumConvert.h"
#include "Gui/SpinBox.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewerGL.h"


NATRON_NAMESPACE_ENTER;

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
//OpenGL is column-major for matrixes
static void
transformToOpenGLMatrix(const Transform::Matrix3x3& mat,
                        GLdouble* oglMat)
{
    oglMat[0] = mat.a; oglMat[4] = mat.b; oglMat[8]  = 0; oglMat[12] = mat.c;
    oglMat[1] = mat.d; oglMat[5] = mat.e; oglMat[9]  = 0; oglMat[13] = mat.f;
    oglMat[2] = 0;     oglMat[6] = 0;     oglMat[10] = 1; oglMat[14] = 0;
    oglMat[3] = mat.g; oglMat[7] = mat.h; oglMat[11] = 0; oglMat[15] = mat.i;
}

#endif


void
ViewerTab::getNodesEntitledForOverlays(NodesList & nodes) const
{
    assert(QThread::currentThread() == qApp->thread());

    Gui* gui = getGui();
    if (!gui) {
        return;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return;
    }

    NodesList nodesWithPanelOpened;
    std::list<DockablePanelI*> panels = app->getOpenedSettingsPanels();

    std::set<ViewerNodePtr> viewerNodesWithPanelOpened;
    for (std::list<DockablePanelI*>::const_iterator it = panels.begin();
         it != panels.end(); ++it) {
        NodeSettingsPanel* panel = dynamic_cast<NodeSettingsPanel*>(*it);
        if (!panel) {
            continue;
        }
        NodeGuiPtr node = panel->getNode();
        NodePtr internalNode = node->getNode();
        if (node && internalNode) {
            if ( internalNode->shouldDrawOverlay() ) {
                ViewerNodePtr isViewer = internalNode->isEffectViewerNode();
                if (!isViewer) {
                    // Do not add viewers, add them afterwards
                    nodesWithPanelOpened.push_back( node->getNode() );
                }
            }
        }
    }

    // Now remove from the nodesWithPanelOpened list nodes that are not upstream of this viewer node
    NodePtr thisNode = getInternalNode()->getNode();
    for (NodesList::const_iterator it = nodesWithPanelOpened.begin(); it != nodesWithPanelOpened.end(); ++it) {
        if (thisNode->isNodeUpstream(*it)) {
            nodes.push_back(*it);
        }
    }

    // Also add the viewer itself
    nodes.push_back(thisNode);

}


void
ViewerTab::drawOverlays(double time,
                        const RenderScale & renderScale) const
{
    Gui* gui = getGui();
    if (!gui) {
        return;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return;
    }

    bool isDrawing = app->isDuringPainting();

    if ( !_imp->viewer ||
         app->isClosing() ||
         isFileDialogViewer() ||
         (gui->isGUIFrozen() && !isDrawing) ) {
        return;
    }

    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return;
    }

    ViewIdx view = getInternalNode()->getCurrentView();
    NodesList nodes;
    getNodesEntitledForOverlays(nodes);

    ///Draw overlays in reverse order of appearance so that the first (top) panel is drawn on top of everything else
    for (NodesList::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        NodeGuiPtr nodeUi = boost::dynamic_pointer_cast<NodeGui>( (*it)->getNodeGui() );
        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }
#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
        double transformedTime;
        bool ok = _imp->getTimeTransform(time, view, *it, getInternalNode(), &transformedTime);
        if (!nodeUi) {
            continue;
        }
        bool overlayDeemed = false;
        if (ok) {
            if (time != transformedTime) {
                //when retimed, modify the overlay color so it looks deemed to indicate that the user
                //cannot modify it
                overlayDeemed = true;
            }
            time = transformedTime;
        }
        nodeUi->setOverlayLocked(overlayDeemed);

        Transform::Matrix3x3 mat(1, 0, 0, 0, 1, 0, 0, 0, 1);
        ok = _imp->getOverlayTransform(time, view, *it, getInternalNode(), &mat);
        GLfloat oldMat[16];
        if (ok) {
            //Ok we've got a transform here, apply it to the OpenGL model view matrix

            GLdouble oglMat[16];
            transformToOpenGLMatrix(mat, oglMat);
            GL_GPU::MatrixMode(GL_MODELVIEW);
            GL_GPU::GetFloatv(GL_MODELVIEW_MATRIX, oldMat);
            GL_GPU::MultMatrixd(oglMat);
        }

#endif
        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            EffectInstancePtr effect = (*it)->getEffectInstance();
            assert(effect);
            effect->setCurrentViewportForOverlays_public(_imp->viewer);
            effect->drawOverlay_public(time, renderScale, view);
        }

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
        if (ok) {
            GL_GPU::MatrixMode(GL_MODELVIEW);
            GL_GPU::LoadMatrixf(oldMat);
        }
#endif
    }
} // ViewerTab::drawOverlays

bool
ViewerTab::notifyOverlaysPenDown_internal(const NodePtr& node,
                                          const RenderScale & renderScale,
                                          PenType pen,
                                          const QPointF & viewportPos,
                                          const QPointF & pos,
                                          double pressure,
                                          double timestamp)
{
    ViewerNodePtr isViewerNode = node->isEffectViewerNode();
    if (isViewerNode && isViewerNode != getInternalNode()) {
        return false;
    }
    QPointF transformViewportPos;
    QPointF transformPos;
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();


#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS


    double transformedTime;
    bool ok = _imp->getTimeTransform(time, view, node, getInternalNode(), &transformedTime);
    if (ok) {
        /*
         * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
         * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
         * that interact is not editable when it is retimed.
         */
        if (time != transformedTime) {
            return false;
        }
        time = transformedTime;
    }

    Transform::Matrix3x3 mat(1, 0, 0, 0, 1, 0, 0, 0, 1);
    ok = _imp->getOverlayTransform(time, view, node, getInternalNode(), &mat);
    if (!ok) {
        transformViewportPos = viewportPos;
        transformPos = pos;
    } else {
        mat = Transform::matInverse(mat);
        {
            Transform::Point3D p;
            p.x = viewportPos.x();
            p.y = viewportPos.y();
            p.z = 1;
            p = Transform::matApply(mat, p);
            transformViewportPos.rx() = p.x / p.z;
            transformViewportPos.ry() = p.y / p.z;
        }
        {
            Transform::Point3D p;
            p.x = pos.x();
            p.y = pos.y();
            p.z = 1;
            p = Transform::matApply(mat, p);
            transformPos.rx() = p.x / p.z;
            transformPos.ry() = p.y / p.z;
        }
    }


#else
    transformViewportPos = viewportPos;
    transformPos = pos;
#endif

    bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(node);
    if (!isInActiveViewerUI) {
        EffectInstancePtr effect = node->getEffectInstance();
        assert(effect);
        effect->setCurrentViewportForOverlays_public(_imp->viewer);
        bool didSmthing = effect->onOverlayPenDown_public(time, renderScale, view, transformViewportPos, transformPos, pressure, timestamp, pen);
        if (didSmthing) {
            //http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html
            // if the instance returns kOfxStatOK, the host should not pass the pen motion

            // to any other interactive object it may own that shares the same view.
            _imp->lastOverlayNode = node;

            return true;
        }
    }


    return false;
} // ViewerTab::notifyOverlaysPenDown_internal

bool
ViewerTab::notifyOverlaysPenDown(const RenderScale & renderScale,
                                 PenType pen,
                                 const QPointF & viewportPos,
                                 const QPointF & pos,
                                 double pressure,
                                 double timestamp)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }

    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return false;
    }


    _imp->hasPenDown = true;
    _imp->hasCaughtPenMotionWhileDragging = false;

    NodesList nodes;
    getNodesEntitledForOverlays(nodes);


    NodePtr lastOverlay = _imp->lastOverlayNode.lock();
    if (lastOverlay) {
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it == lastOverlay) {
                if ( notifyOverlaysPenDown_internal(*it, renderScale, pen, viewportPos, pos, pressure, timestamp) ) {
                    return true;
                } else {
                    nodes.erase(it);
                    break;
                }
            }
        }
    }

    for (NodesList::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if ( notifyOverlaysPenDown_internal(*it, renderScale, pen, viewportPos, pos, pressure, timestamp) ) {
            return true;
        }
    }

    if (app->getOverlayRedrawRequestsCount() > 0) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    return false;
}

bool
ViewerTab::notifyOverlaysPenDoubleClick(const RenderScale & renderScale,
                                        const QPointF & viewportPos,
                                        const QPointF & pos)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }

    NodesList nodes;
    getNodesEntitledForOverlays(nodes);

    ViewIdx view = getInternalNode()->getCurrentView();

    for (NodesList::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }

        QPointF transformViewportPos;
        QPointF transformPos;
        double time = app->getTimeLine()->currentFrame();
#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS

        double transformedTime;
        bool ok = _imp->getTimeTransform(time, view, *it, getInternalNode(), &transformedTime);
        if (ok) {
            time = transformedTime;
        }

        Transform::Matrix3x3 mat(1, 0, 0, 0, 1, 0, 0, 0, 1);
        ok = _imp->getOverlayTransform(time, view, *it, getInternalNode(), &mat);
        if (!ok) {
            transformViewportPos = viewportPos;
            transformPos = pos;
        } else {
            mat = Transform::matInverse(mat);
            {
                Transform::Point3D p;
                p.x = viewportPos.x();
                p.y = viewportPos.y();
                p.z = 1;
                p = Transform::matApply(mat, p);
                transformViewportPos.rx() = p.x / p.z;
                transformViewportPos.ry() = p.y / p.z;
            }
            {
                Transform::Point3D p;
                p.x = pos.x();
                p.y = pos.y();
                p.z = 1;
                p = Transform::matApply(mat, p);
                transformPos.rx() = p.x / p.z;
                transformPos.ry() = p.y / p.z;
            }
        }


#else
        transformViewportPos = viewportPos;
        transformPos = pos;
#endif

        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            EffectInstancePtr effect = (*it)->getEffectInstance();
            assert(effect);
            effect->setCurrentViewportForOverlays_public(_imp->viewer);

            bool didSmthing = effect->onOverlayPenDoubleClicked_public(time, renderScale, view, transformViewportPos, transformPos);
            if (didSmthing) {
                //http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html
                // if the instance returns kOfxStatOK, the host should not pass the pen motion

                // to any other interactive object it may own that shares the same view.
                _imp->lastOverlayNode = *it;

                return true;
            }
        }
    }

    return false;
} // ViewerTab::notifyOverlaysPenDoubleClick

bool
ViewerTab::notifyOverlaysPenMotion_internal(const NodePtr& node,
                                            const RenderScale & renderScale,
                                            const QPointF & viewportPos,
                                            const QPointF & pos,
                                            double pressure,
                                            double timestamp)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    ViewerNodePtr isViewerNode = node->isEffectViewerNode();
    if (isViewerNode && isViewerNode != getInternalNode()) {
        return false;
    }

    QPointF transformViewportPos;
    QPointF transformPos;
    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS

    double transformedTime;
    bool ok = _imp->getTimeTransform(time, view, node, getInternalNode(), &transformedTime);
    if (ok) {
        /*
         * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
         * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
         * that interact is not editable when it is retimed.
         */
        if (time != transformedTime) {
            return false;
        }
        time = transformedTime;
    }


    Transform::Matrix3x3 mat(1, 0, 0, 0, 1, 0, 0, 0, 1);
    ok = _imp->getOverlayTransform(time, view, node, getInternalNode(), &mat);
    if (!ok) {
        transformViewportPos = viewportPos;
        transformPos = pos;
    } else {
        mat = Transform::matInverse(mat);
        {
            Transform::Point3D p;
            p.x = viewportPos.x();
            p.y = viewportPos.y();
            p.z = 1;
            p = Transform::matApply(mat, p);
            transformViewportPos.rx() = p.x / p.z;
            transformViewportPos.ry() = p.y / p.z;
        }
        {
            Transform::Point3D p;
            p.x = pos.x();
            p.y = pos.y();
            p.z = 1;
            p = Transform::matApply(mat, p);
            transformPos.rx() = p.x / p.z;
            transformPos.ry() = p.y / p.z;
        }
    }

#else
    transformViewportPos = viewportPos;
    transformPos = pos;
#endif

    bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(node);
    if (!isInActiveViewerUI) {
        ///If we are dragging with mouse, set draft mode (not for roto though)
        if ( _imp->hasPenDown && !getGui()->isDraftRenderEnabled() ) {
            getGui()->setDraftRenderEnabled(true);
        }

        EffectInstancePtr effect = node->getEffectInstance();
        assert(effect);
        effect->setCurrentViewportForOverlays_public(_imp->viewer);
        bool didSmthing = effect->onOverlayPenMotion_public(time, renderScale, view, transformViewportPos, transformPos, pressure, timestamp);
        if (didSmthing) {
            if (_imp->hasPenDown) {
                _imp->hasCaughtPenMotionWhileDragging = true;
            }

            //http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html
            // if the instance returns kOfxStatOK, the host should not pass the pen motion

            // to any other interactive object it may own that shares the same view.
            _imp->lastOverlayNode = node;

            return true;
        }
    }


    return false;
} // ViewerTab::notifyOverlaysPenMotion_internal

bool
ViewerTab::notifyOverlaysPenMotion(const RenderScale & renderScale,
                                   const QPointF & viewportPos,
                                   const QPointF & pos,
                                   double pressure,
                                   double timestamp)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    bool didSomething = false;


    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return false;
    }


    NodesList nodes;
    getNodesEntitledForOverlays(nodes);


    NodePtr lastOverlay = _imp->lastOverlayNode.lock();
    if (lastOverlay) {
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it == lastOverlay) {
                if ( notifyOverlaysPenMotion_internal(*it, renderScale, viewportPos, pos, pressure, timestamp) ) {
                    return true;
                } else {
                    nodes.erase(it);
                    break;
                }
            }
        }
    }


    for (NodesList::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if ( notifyOverlaysPenMotion_internal(*it, renderScale, viewportPos, pos, pressure, timestamp) ) {
            return true;
        }
    }


    if ( !didSomething && (app->getOverlayRedrawRequestsCount() > 0) ) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();


    return didSomething;
}

bool
ViewerTab::notifyOverlaysPenUp(const RenderScale & renderScale,
                               const QPointF & viewportPos,
                               const QPointF & pos,
                               double pressure,
                               double timestamp)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    bool didSomething = false;

    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return false;
    }


    ///Reset draft
    bool mustTriggerRender = false;
    if ( getGui()->isDraftRenderEnabled() ) {
        getGui()->setDraftRenderEnabled(false);
        mustTriggerRender = _imp->hasCaughtPenMotionWhileDragging;
    }

    _imp->hasPenDown = false;
    _imp->hasCaughtPenMotionWhileDragging = false;


    _imp->lastOverlayNode.reset();

    NodesList nodes;
    getNodesEntitledForOverlays(nodes);

    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();

    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {

        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }
        QPointF transformViewportPos;
        QPointF transformPos;
#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS


        double transformedTime;
        bool ok = _imp->getTimeTransform(time, view, *it, getInternalNode(), &transformedTime);
        if (ok) {
            /*
             * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
             * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
             * that interact is not editable when it is retimed.
             */
            if (time != transformedTime) {
                return false;
            }
            time = transformedTime;
        }

        Transform::Matrix3x3 mat(1, 0, 0, 0, 1, 0, 0, 0, 1);
        ok = _imp->getOverlayTransform(time, view, *it, getInternalNode(), &mat);
        if (!ok) {
            transformViewportPos = viewportPos;
            transformPos = pos;
        } else {
            mat = Transform::matInverse(mat);
            {
                Transform::Point3D p;
                p.x = viewportPos.x();
                p.y = viewportPos.y();
                p.z = 1;
                p = Transform::matApply(mat, p);
                transformViewportPos.rx() = p.x / p.z;
                transformViewportPos.ry() = p.y / p.z;
            }
            {
                Transform::Point3D p;
                p.x = pos.x();
                p.y = pos.y();
                p.z = 1;
                p = Transform::matApply(mat, p);
                transformPos.rx() = p.x / p.z;
                transformPos.ry() = p.y / p.z;
            }
        }


#else
        transformViewportPos = viewportPos;
        transformPos = pos;
#endif

        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            EffectInstancePtr effect = (*it)->getEffectInstance();
            assert(effect);
            effect->setCurrentViewportForOverlays_public(_imp->viewer);
            didSomething |= effect->onOverlayPenUp_public(time, renderScale, view, transformViewportPos, transformPos, pressure, timestamp);
        }
    }


    if ( !mustTriggerRender && !didSomething && (app->getOverlayRedrawRequestsCount() > 0) ) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    if (mustTriggerRender) {
        //We had draft enabled but penRelease didn't trigger any render, trigger one to refresh the viewer
        app->renderAllViewers(true);
    }


    return didSomething;
} // ViewerTab::notifyOverlaysPenUp


bool
ViewerTab::checkForTimelinePlayerGlobalShortcut(Qt::Key qKey,
                                                const Qt::KeyboardModifiers& mods)
{
    const char* knobsToCheck[] = {
        kViewerNodeParamSetInPoint,
        kViewerNodeParamSetOutPoint,
        kViewerNodeParamFirstFrame,
        kViewerNodeParamPlayBackward,
        kViewerNodeParamPlayForward,
        kViewerNodeParamLastFrame,
        kViewerNodeParamPreviousFrame,
        kViewerNodeParamNextFrame,
        kViewerNodeParamPreviousKeyFrame,
        kViewerNodeParamNextKeyFrame,
        kViewerNodeParamPreviousIncr,
        kViewerNodeParamNextIncr,
        NULL
    };

    ViewerNodePtr node = getInternalNode();
    const KnobsVec& knobs = node->getKnobs();
    int i = 0;
    std::string pluginShortcutGroup;
    while (knobsToCheck[i]) {
        for (KnobsVec::const_iterator it = knobs.begin(); it!=knobs.end(); ++it) {
            if ( (*it)->getInViewerContextHasShortcut() && !(*it)->getInViewerContextSecret() ) {
                if ( pluginShortcutGroup.empty() ) {
                    pluginShortcutGroup = node->getNode()->getOriginalPlugin()->getPluginShortcutGroup();
                }
                if ( isKeybind(pluginShortcutGroup, (*it)->getName(), mods, qKey) ) {
                    // This only works for groups and buttons, as defined in the spec
                    KnobButtonPtr isButton = toKnobButton(*it);
                    bool ret = false;

                    if (isButton) {
                        ret = isButton->trigger();
                    }
                    return ret;
                }
            }
        }
        ++i;
    }
    return false;
}


bool
ViewerTab::checkNodeViewerContextShortcuts(const NodePtr& node,
                                           Qt::Key qKey,
                                           const Qt::KeyboardModifiers& mods)
{
    // Intercept plug-in defined shortcuts
    NodeViewerContextPtr context;

    for (std::list<ViewerTabPrivate::PluginViewerContext>::const_iterator it = _imp->currentNodeContext.begin(); it != _imp->currentNodeContext.end(); ++it) {
        NodeGuiPtr n = it->currentNode.lock();
        if (!n) {
            continue;
        }
        if (n->getNode() == node) {
            context = it->currentContext;
            break;
        }
    }

    // This is not an active node on the viewer ui, don't trigger any shortcuts
    if (!context) {
        return false;
    }

    EffectInstancePtr effect = node->getEffectInstance();
    assert(effect);
    const KnobsVec& knobs = effect->getKnobs();
    std::string pluginShortcutGroup;
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ( (*it)->getInViewerContextHasShortcut() && !(*it)->getInViewerContextSecret() ) {
            if ( pluginShortcutGroup.empty() ) {
                pluginShortcutGroup = effect->getNode()->getOriginalPlugin()->getPluginShortcutGroup();
            }
            if ( isKeybind(pluginShortcutGroup, (*it)->getName(), mods, qKey) ) {
                // This only works for groups and buttons, as defined in the spec
                KnobButtonPtr isButton = toKnobButton(*it);
                KnobGroupPtr isGrp = toKnobGroup(*it);
                bool ret = false;

                if (isButton) {
                    if ( isButton->getIsCheckable() ) {

                        // Refresh the button state
                        isButton->setValue(!isButton->getValue(), ViewSetSpec::current(), DimIdx(0), eValueChangedReasonNatronGuiEdited, 0 /*keyframe*/, true /*forceHandlerEvenIfNoChange*/);

                        ret = true;
                    } else {
                        ret = isButton->trigger();
                    }
                } else if (isGrp) {
                    // This can only be a toolbutton, notify the NodeViewerContext
                    context->onToolButtonShortcutPressed( QString::fromUtf8( isGrp->getName().c_str() ) );
                    ret = true;
                }

                return ret;
            }
        }
    }

    return false;
} // ViewerTab::checkNodeViewerContextShortcuts

bool
ViewerTab::notifyOverlaysKeyDown_internal(const NodePtr& node,
                                          const RenderScale & renderScale,
                                          Key k,
                                          KeyboardModifiers km,
                                          Qt::Key qKey,
                                          const Qt::KeyboardModifiers& mods)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    ViewerNodePtr isViewerNode = node->isEffectViewerNode();
    if (isViewerNode && isViewerNode != getInternalNode()) {
        return false;
    }

    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
    double transformedTime;
    bool ok = _imp->getTimeTransform(time, ViewIdx(0), node, getInternalNode(), &transformedTime);
    if (ok) {
        /*
         * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
         * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
         * that interact is not editable when it is retimed.
         */
        if (time != transformedTime) {
            return false;
        }
        time = transformedTime;
    }
#endif


    bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(node);
    if (!isInActiveViewerUI) {
        EffectInstancePtr effect = node->getEffectInstance();
        assert(effect);
        effect->setCurrentViewportForOverlays_public(_imp->viewer);

        bool didSmthing = checkNodeViewerContextShortcuts(node, qKey, mods);

        if (!didSmthing) {
            didSmthing = effect->onOverlayKeyDown_public(time, renderScale, view, k, km);
        }
        if (didSmthing) {
            //http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html
            // if the instance returns kOfxStatOK, the host should not pass the pen motion

            // to any other interactive object it may own that shares the same view.
            _imp->lastOverlayNode = node;

            return true;
        }
    }


    return false;
} // ViewerTab::notifyOverlaysKeyDown_internal

bool
ViewerTab::notifyOverlaysKeyDown(const RenderScale & renderScale,
                                 QKeyEvent* e)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    bool didSomething = false;

    /*
       Modifiers key down/up should be passed to all active interacts always so that they can properly figure out
       whether they are up or down
     */
    bool isModifier = e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt ||
                      e->key() == Qt::Key_Meta;
    Qt::Key qKey = (Qt::Key)e->key();
    Qt::KeyboardModifiers qMods = e->modifiers();
    Key natronKey = QtEnumConvert::fromQtKey(qKey );
    KeyboardModifiers natronMod = QtEnumConvert::fromQtModifiers(qMods);
    NodesList nodes;
    getNodesEntitledForOverlays(nodes);

    NodePtr lastOverlay = _imp->lastOverlayNode.lock();
    if (lastOverlay) {
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it == lastOverlay) {
                if ( notifyOverlaysKeyDown_internal(*it, renderScale, natronKey, natronMod, qKey, qMods) ) {
                    if (isModifier) {
                        nodes.erase(it);
                        break;
                    }

                    return true;
                } else {
                    nodes.erase(it);
                    break;
                }
            }
        }
    }


    for (NodesList::reverse_iterator it = nodes.rbegin();
         it != nodes.rend();
         ++it) {
        if ( notifyOverlaysKeyDown_internal(*it, renderScale, natronKey, natronMod, qKey, qMods) ) {
            if (isModifier) {
                continue;
            }

            return true;
        }
    }

    if (isModifier) {
        //Modifiers may not necessarily return true for plug-ins but may require a redraw
        app->queueRedrawForAllViewers();
    }

    if (app->getOverlayRedrawRequestsCount() > 0) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    return didSomething;
} // ViewerTab::notifyOverlaysKeyDown

bool
ViewerTab::notifyOverlaysKeyUp(const RenderScale & renderScale,
                               QKeyEvent* e)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    bool didSomething = false;

    _imp->lastOverlayNode.reset();

    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();
    NodesList nodes;
    getNodesEntitledForOverlays(nodes);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {

        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }
        EffectInstancePtr effect = (*it)->getEffectInstance();
        assert(effect);

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
        double transformedTime;
        bool ok = _imp->getTimeTransform(time, ViewIdx(0), *it, getInternalNode(), &transformedTime);
        if (ok) {
            /*
             * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
             * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
             * that interact is not editable when it is retimed.
             */
            if (time != transformedTime) {
                return false;
            }
            time = transformedTime;
        }
#endif


        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            effect->setCurrentViewportForOverlays_public(_imp->viewer);
            didSomething |= effect->onOverlayKeyUp_public( time, renderScale, view,
                                                           QtEnumConvert::fromQtKey( (Qt::Key)e->key() ), QtEnumConvert::fromQtModifiers( e->modifiers() ) );
        }
    }

    /*
       Do not catch the event if this is a modifier, let it propagate to the Gui
     */
    bool isModifier = e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt ||
                      e->key() == Qt::Key_Meta;

    if (isModifier) {
        //Modifiers may not necessarily return true for plug-ins but may require a redraw
        app->queueRedrawForAllViewers();
    }

    if (app->getOverlayRedrawRequestsCount() > 0) {
        app->redrawAllViewers();
    }

    app->clearOverlayRedrawRequests();


    return didSomething;
} // ViewerTab::notifyOverlaysKeyUp

bool
ViewerTab::notifyOverlaysKeyRepeat_internal(const NodePtr& node,
                                            const RenderScale & renderScale,
                                            Key k,
                                            KeyboardModifiers km,
                                            Qt::Key qKey,
                                            const Qt::KeyboardModifiers& mods)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    ViewerNodePtr isViewerNode = node->isEffectViewerNode();
    if (isViewerNode && isViewerNode != getInternalNode()) {
        return false;
    }

    ViewIdx view = getInternalNode()->getCurrentView();
    double time = app->getTimeLine()->currentFrame();

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
    double transformedTime;
    bool ok = _imp->getTimeTransform(time, ViewIdx(0), node, getInternalNode(), &transformedTime);
    if (ok) {
        /*
         * Do not allow interaction with retimed interacts otherwise the user may end up modifying keyframes at unexpected
         * (or invalid for floating point) frames, which may be confusing. Rather we indicate with the overlay color hint
         * that interact is not editable when it is retimed.
         */
        if (time != transformedTime) {
            return false;
        }
        time = transformedTime;
    }
#endif

    bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(node);
    if (!isInActiveViewerUI) {
        EffectInstancePtr effect = node->getEffectInstance();
        assert(effect);
        effect->setCurrentViewportForOverlays_public(_imp->viewer);

        bool didSmthing = checkNodeViewerContextShortcuts(node, qKey, mods);

        if (!didSmthing) {
            didSmthing = effect->onOverlayKeyRepeat_public(time, renderScale, view, k, km);
        }
        if (didSmthing) {
            //http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html
            // if the instance returns kOfxStatOK, the host should not pass the pen motion

            // to any other interactive object it may own that shares the same view.
            _imp->lastOverlayNode = node;

            return true;
        }
    }


    return false;
}

bool
ViewerTab::notifyOverlaysKeyRepeat(const RenderScale & renderScale,
                                   QKeyEvent* e)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }

    Qt::Key qKey = (Qt::Key)e->key();
    Qt::KeyboardModifiers qMods = e->modifiers();
    Key natronKey = QtEnumConvert::fromQtKey( qKey);
    KeyboardModifiers natronMod = QtEnumConvert::fromQtModifiers( qMods );
    NodesList nodes;
    NodePtr lastOverlay = _imp->lastOverlayNode.lock();
    if (lastOverlay) {
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (*it == lastOverlay) {
                if ( notifyOverlaysKeyRepeat_internal(*it, renderScale, natronKey, natronMod, qKey, qMods) ) {
                    return true;
                } else {
                    nodes.erase(it);
                    break;
                }
            }
        }
    }


    for (NodesList::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if ( notifyOverlaysKeyRepeat_internal(*it, renderScale, natronKey, natronMod, qKey, qMods) ) {
            return true;
        }
    }

    if (app->getOverlayRedrawRequestsCount() > 0) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    return false;
}

bool
ViewerTab::notifyOverlaysFocusGained(const RenderScale & renderScale)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }

    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return false;
    }


    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();
    bool ret = false;
    NodesList nodes;
    getNodesEntitledForOverlays(nodes);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {

        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }

        EffectInstancePtr effect = (*it)->getEffectInstance();
        assert(effect);

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
        double transformedTime;
        bool ok = _imp->getTimeTransform(time, ViewIdx(0), *it, getInternalNode(), &transformedTime);
        if (ok) {
            time = transformedTime;
        }
#endif

        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            effect->setCurrentViewportForOverlays_public(_imp->viewer);
            bool didSmthing = effect->onOverlayFocusGained_public(time, renderScale, view);
            if (didSmthing) {
                ret = true;
            }
        }
    }

    if ( !ret && (app->getOverlayRedrawRequestsCount() > 0) ) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    return ret;
} // ViewerTab::notifyOverlaysFocusGained

bool
ViewerTab::notifyOverlaysFocusLost(const RenderScale & renderScale)
{
    Gui* gui = getGui();
    if (!gui) {
        return false;
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return false;
    }
    if ( app->isClosing() ) {
        return false;
    }
    if ( !getInternalNode() ) {
        return false;
    }

    if ( app->isShowingDialog() ) {
        /*
           We may enter a situation where a plug-in called EffectInstance::message to show a dialog
           and would block the main thread until the user would click OK but Qt would request a paintGL() on the viewer
           because of focus changes. This would end-up in the interact draw action being called whilst the message() function
           did not yet return and may in some plug-ins cause deadlocks (happens in all Genarts Sapphire plug-ins).
         */
        return false;
    }


    double time = app->getTimeLine()->currentFrame();
    ViewIdx view = getInternalNode()->getCurrentView();
    bool ret = false;
    NodesList nodes;
    getNodesEntitledForOverlays(nodes);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {

        ViewerNodePtr isViewerNode = (*it)->isEffectViewerNode();
        if (isViewerNode && isViewerNode != getInternalNode()) {
            continue;
        }

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
        double transformedTime;
        bool ok = _imp->getTimeTransform(time, ViewIdx(0), *it, getInternalNode(), &transformedTime);
        if (ok) {
            time = transformedTime;
        }
#endif
        bool isInActiveViewerUI = _imp->hasInactiveNodeViewerContext(*it);
        if (!isInActiveViewerUI) {
            EffectInstancePtr effect = (*it)->getEffectInstance();
            assert(effect);

            effect->setCurrentViewportForOverlays_public(_imp->viewer);
            bool didSmthing = effect->onOverlayFocusLost_public(time, renderScale, view);
            if (didSmthing) {
                ret = true;
            }
        }
    }


    if ( !ret && (app->getOverlayRedrawRequestsCount() > 0) ) {
        app->redrawAllViewers();
    }
    app->clearOverlayRedrawRequests();

    return ret;
} // ViewerTab::notifyOverlaysFocusLost

NATRON_NAMESPACE_EXIT;
