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

#include "ViewerTabPrivate.h"

#include <cassert>
#include <stdexcept>

#include <QtCore/QDebug>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Engine/ViewerNode.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/ClickableLabel.h"
#include "Gui/Gui.h"
#include "Gui/NodeGui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/ViewerTab.h"


NATRON_NAMESPACE_ENTER;


ViewerTabPrivate::ViewerTabPrivate(ViewerTab* publicInterface,
                                   const NodeGuiPtr& node_ui)
    : publicInterface(publicInterface)
    , viewerNode()
    , viewer(NULL)
    , viewerContainer(NULL)
    , viewerLayout(NULL)
    , viewerSubContainer(NULL)
    , viewerSubContainerLayout(NULL)
    , mainLayout(NULL)
    , infoWidget()
    , timeLineGui(NULL)
    , nodesContext()
    , currentNodeContext()
    , isFileDialogViewer(false)
    , lastOverlayNode()
    , hasPenDown(false)
    , hasCaughtPenMotionWhileDragging(false)
{
    viewerNode = node_ui->getNode()->isEffectViewerNode();
    infoWidget[0] = infoWidget[1] = NULL;
}

#ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS
bool
ViewerTabPrivate::getOverlayTransform(double time,
                                      ViewIdx view,
                                      const NodePtr& target,
                                      const EffectInstancePtr& currentNode,
                                      Transform::Matrix3x3* transform) const
{
    if ( currentNode == target->getEffectInstance() ) {
        return true;
    }
    RenderScale s(1.);
    EffectInstancePtr input;
    StatusEnum stat = eStatusReplyDefault;
    Transform::Matrix3x3 mat;
    // call getTransform even of effects that claim not to support it, because it may still return
    // a transform to apply to the overlays (eg for Reformat).
    // If transform is not implemented, it should return eStatusReplyDefault:
    // http://openfx.sourceforge.net/Documentation/1.4/ofxProgrammingReference.html#mainEntryPoint
    // "the value kOfxStatReplyDefault is returned if the plug-in does not trap the action"
    if ( !currentNode->getNode()->isNodeDisabled() /*&& currentNode->getNode()->getCurrentCanTransform()*/ ) {
        stat = currentNode->getTransform_public(time, s, view, &input, &mat);
    }
    if (stat == eStatusFailed) {
        return false;
    } else if (stat == eStatusReplyDefault) {
        //No transfo matrix found, pass to the input...

        ///Test all inputs recursively, going from last to first, preferring non optional inputs.
        std::list<EffectInstancePtr> nonOptionalInputs;
        std::list<EffectInstancePtr> optionalInputs;
        int maxInp = currentNode->getMaxInputCount();

        ///We cycle in reverse by default. It should be a setting of the application.
        ///In this case it will return input B instead of input A of a merge for example.
        for (int i = maxInp - 1; i >= 0; --i) {
            EffectInstancePtr inp = currentNode->getInput(i);
            bool optional = currentNode->isInputOptional(i);
            if (inp) {
                if (optional) {
                    optionalInputs.push_back(inp);
                } else {
                    nonOptionalInputs.push_back(inp);
                }
            }
        }

        if ( nonOptionalInputs.empty() && optionalInputs.empty() ) {
            return false;
        }

        ///Cycle through all non optional inputs first
        for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
            mat = Transform::Matrix3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
            bool isOk = getOverlayTransform(time, view, target, *it, &mat);
            if (isOk) {
                *transform = Transform::matMul(*transform, mat);

                return true;
            }
        }

        ///Cycle through optional inputs...
        for (std::list<EffectInstancePtr> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
            mat = Transform::Matrix3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
            bool isOk = getOverlayTransform(time, view, target, *it, &mat);
            if (isOk) {
                *transform = Transform::matMul(*transform, mat);

                return true;
            }
        }

        return false;
    } else {
        assert(input);
        double par = input->getAspectRatio(-1);

        //The mat is in pixel coordinates, though
        mat = Transform::matMul(Transform::matPixelToCanonical(par, 1, 1, false), mat);
        mat = Transform::matMul( mat, Transform::matCanonicalToPixel(par, 1, 1, false) );
        *transform = Transform::matMul(*transform, mat);
        bool isOk = getOverlayTransform(time, view, target, input, transform);

        return isOk;
    }

    return false;
} // ViewerTabPrivate::getOverlayTransform

static double
transformTimeForNode(const EffectInstancePtr& currentNode,
                     double inTime)
{
    U64 nodeHash;
    FramesNeededMap framesNeeded = currentNode->getFramesNeeded_public(inTime, ViewIdx(0), &nodeHash);
    FramesNeededMap::iterator foundInput0 = framesNeeded.find(0 /*input*/);

    if ( foundInput0 == framesNeeded.end() ) {
        return inTime;
    }

    FrameRangesMap::iterator foundView0 = foundInput0->second.find( ViewIdx(0) );
    if ( foundView0 == foundInput0->second.end() ) {
        return inTime;
    }

    if ( foundView0->second.empty() ) {
        return inTime;
    } else {
        return (foundView0->second.front().min);
    }
}

bool
ViewerTabPrivate::getTimeTransform(double time,
                                   ViewIdx view,
                                   const NodePtr& target,
                                   const EffectInstancePtr& currentNode,
                                   double *newTime) const
{
    if (!currentNode || !currentNode->getNode()) {
        return false;
    }
    if ( currentNode == target->getEffectInstance() ) {
        *newTime = time;

        return true;
    }

    if ( !currentNode->getNode()->isNodeDisabled() ) {
        *newTime = transformTimeForNode(currentNode, time);
    } else {
        *newTime = time;
    }

    ///Test all inputs recursively, going from last to first, preferring non optional inputs.
    std::list<EffectInstancePtr> nonOptionalInputs;
    std::list<EffectInstancePtr> optionalInputs;
    int maxInp = currentNode->getMaxInputCount();

    ///We cycle in reverse by default. It should be a setting of the application.
    ///In this case it will return input B instead of input A of a merge for example.
    for (int i = maxInp - 1; i >= 0; --i) {
        EffectInstancePtr inp = currentNode->getInput(i);
        bool optional = currentNode->isInputOptional(i);
        if (inp) {
            if (optional) {
                optionalInputs.push_back(inp);
            } else {
                nonOptionalInputs.push_back(inp);
            }
        }
    }

    if ( nonOptionalInputs.empty() && optionalInputs.empty() ) {
        return false;
    }

    ///Cycle through all non optional inputs first
    for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
        double inputTime;
        bool isOk = getTimeTransform(*newTime, view, target, *it, &inputTime);
        if (isOk) {
            *newTime = inputTime;

            return true;
        }
    }

    ///Cycle through optional inputs...
    for (std::list<EffectInstancePtr> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
        double inputTime;
        bool isOk = getTimeTransform(*newTime, view, target, *it, &inputTime);
        if (isOk) {
            *newTime = inputTime;

            return true;
        }
    }

    return false;
} // ViewerTabPrivate::getTimeTransform

#endif // ifdef NATRON_TRANSFORM_AFFECTS_OVERLAYS

std::list<ViewerTabPrivate::PluginViewerContext>::iterator
ViewerTabPrivate::findActiveNodeContextForNode(const NodePtr& node)
{
    // Try once with the pyplug if any and once without, because the plug-in may have changed
    PluginPtr plug = node->getPlugin();
    return findActiveNodeContextForPlugin(plug);
}

std::list<ViewerTabPrivate::PluginViewerContext>::iterator
ViewerTabPrivate::findActiveNodeContextForPlugin(const PluginPtr& plugin)
{
    std::string pluginID = plugin->getPluginID();
    // Roto and RotoPaint are 2 different plug-ins but we don't want them at the same time in the viewer
    bool isRotoOrRotoPaint = pluginID == PLUGINID_NATRON_ROTO || pluginID == PLUGINID_NATRON_ROTOPAINT;
    for (std::list<PluginViewerContext>::iterator it = currentNodeContext.begin(); it != currentNodeContext.end(); ++it) {
        if (isRotoOrRotoPaint) {
            std::string otherID = it->plugin.lock()->getPluginID();
            if (otherID == PLUGINID_NATRON_ROTO || otherID == PLUGINID_NATRON_ROTOPAINT) {
                return it;
            }
        } else {
            if (plugin == it->plugin.lock() || plugin == it->pyPlug.lock()) {
                return it;
            }
        }

    }

    return currentNodeContext.end();
}

bool
ViewerTabPrivate::hasInactiveNodeViewerContext(const NodePtr& node)
{
    if (node->isEffectViewerNode()) {
        return false;
    }
    std::list<ViewerTabPrivate::PluginViewerContext>::iterator found = findActiveNodeContextForNode(node);

    if ( found == currentNodeContext.end() ) {
        return false;
    }
    NodeGuiPtr n = found->currentNode.lock();
    if (!n) {
        return false;
    }

    return n->getNode() != node;
}

NATRON_NAMESPACE_EXIT;
