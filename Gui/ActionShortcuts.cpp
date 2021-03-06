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

#include "ActionShortcuts.h"

#include <stdexcept>

#include <QWidget>

#include "Gui/GuiApplicationManager.h"

NATRON_NAMESPACE_ENTER;

ActionWithShortcut::ActionWithShortcut(const std::string & group,
                                       const std::string & actionID,
                                       const std::string & actionDescription,
                                       QObject* parent,
                                       bool setShortcutOnAction)
    : QAction(parent)
    , _group( QString::fromUtf8( group.c_str() ) )
    , _shortcuts()
{
    // insert here the output of:
    // fgrep "#define kShortcutDescAction" ActionShortcuts.h | sed -e 's/#define /(void)QT_TR_NOOP(/' -e 's/ .*/);/'
    (void)QT_TR_NOOP(kShortcutDescActionNewProject);
    (void)QT_TR_NOOP(kShortcutDescActionOpenProject);
    (void)QT_TR_NOOP(kShortcutDescActionCloseProject);
    (void)QT_TR_NOOP(kShortcutDescActionReloadProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveAsProject);
    (void)QT_TR_NOOP(kShortcutDescActionSaveAndIncrVersion);
    (void)QT_TR_NOOP(kShortcutDescActionExportProject);
    (void)QT_TR_NOOP(kShortcutDescActionPreferences);
    (void)QT_TR_NOOP(kShortcutDescActionQuit);
    (void)QT_TR_NOOP(kShortcutDescActionProjectSettings);
    (void)QT_TR_NOOP(kShortcutDescActionShowErrorLog);
    (void)QT_TR_NOOP(kShortcutDescActionNewViewer);
    (void)QT_TR_NOOP(kShortcutDescActionFullscreen);
    (void)QT_TR_NOOP(kShortcutDescActionShowWindowsConsole);
    (void)QT_TR_NOOP(kShortcutDescActionClearDiskCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearPlaybackCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearNodeCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearPluginsLoadCache);
    (void)QT_TR_NOOP(kShortcutDescActionClearAllCaches);
    (void)QT_TR_NOOP(kShortcutDescActionShowAbout);
    (void)QT_TR_NOOP(kShortcutDescActionRenderSelected);
    (void)QT_TR_NOOP(kShortcutDescActionEnableRenderStats);
    (void)QT_TR_NOOP(kShortcutDescActionRenderAll);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput1);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput2);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput3);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput4);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput5);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput6);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput7);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput8);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput9);
    (void)QT_TR_NOOP(kShortcutDescActionConnectViewerToInput10);
    (void)QT_TR_NOOP(kShortcutDescActionShowPaneFullScreen);
    (void)QT_TR_NOOP(kShortcutDescActionImportLayout);
    (void)QT_TR_NOOP(kShortcutDescActionExportLayout);
    (void)QT_TR_NOOP(kShortcutDescActionDefaultLayout);
    (void)QT_TR_NOOP(kShortcutDescActionNextTab);
    (void)QT_TR_NOOP(kShortcutDescActionPrevTab);
    (void)QT_TR_NOOP(kShortcutDescActionCloseTab);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRearrangeNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRemoveNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphShowExpressions);
    (void)QT_TR_NOOP(kShortcutDescActionGraphNavigateUpstream);
    (void)QT_TR_NOOP(kShortcutDescActionGraphNavigateDownstram);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectUp);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectDown);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectAll);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSelectAllVisible);
    (void)QT_TR_NOOP(kShortcutDescActionGraphAutoHideInputs);
    (void)QT_TR_NOOP(kShortcutDescActionGraphHideInputs);
    (void)QT_TR_NOOP(kShortcutDescActionGraphSwitchInputs);
    (void)QT_TR_NOOP(kShortcutDescActionGraphCopy);
    (void)QT_TR_NOOP(kShortcutDescActionGraphPaste);
    (void)QT_TR_NOOP(kShortcutDescActionGraphClone);
    (void)QT_TR_NOOP(kShortcutDescActionGraphDeclone);
    (void)QT_TR_NOOP(kShortcutDescActionGraphCut);
    (void)QT_TR_NOOP(kShortcutDescActionGraphDuplicate);
    (void)QT_TR_NOOP(kShortcutDescActionGraphDisableNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphToggleAutoPreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphToggleAutoTurbo);
    (void)QT_TR_NOOP(kShortcutDescActionGraphTogglePreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphForcePreview);
    (void)QT_TR_NOOP(kShortcutDescActionGraphShowCacheSize);
    (void)QT_TR_NOOP(kShortcutDescActionGraphFrameNodes);
    (void)QT_TR_NOOP(kShortcutDescActionGraphFindNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphRenameNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphExtractNode);
    (void)QT_TR_NOOP(kShortcutDescActionGraphMakeGroup);
    (void)QT_TR_NOOP(kShortcutDescActionGraphExpandGroup);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleRemoveKeys);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleConstant);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleLinear);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleSmooth);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleCatmullrom);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleCubic);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleHorizontal);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleBreak);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleSelectAll);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleCenter);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModuleCopy);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModulePasteKeyframes);
    (void)QT_TR_NOOP(kShortcutDescActionAnimationModulePasteKeyframesAbsolute);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorPrevScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorNextScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptEditorClearHistory);
    (void)QT_TR_NOOP(kShortcutDescActionScriptExecScript);
    (void)QT_TR_NOOP(kShortcutDescActionScriptClearOutput);
    (void)QT_TR_NOOP(kShortcutDescActionScriptShowOutput);

    QString actionIDStr = QString::fromUtf8( actionID.c_str() );
    std::list<QKeySequence> seq = getKeybind(_group, actionIDStr);

    if ( seq.empty() ) {
        seq.push_back( QKeySequence() );
    }
    _shortcuts.push_back( std::make_pair( actionIDStr, seq.front() ) );
    assert ( !_group.isEmpty() && !actionIDStr.isEmpty() );
    if (setShortcutOnAction) {
        setShortcut( seq.front() );
    }
    appPTR->addShortcutAction(_group, actionIDStr, this);
    setShortcutContext(Qt::WindowShortcut);
    setText( tr( actionDescription.c_str() ) );
}

ActionWithShortcut::ActionWithShortcut(const std::string & group,
                                       const std::list<std::string> & actionIDs,
                                       const std::string & actionDescription,
                                       QObject* parent,
                                       bool setShortcutOnAction)
    : QAction(parent)
    , _group( QString::fromUtf8( group.c_str() ) )
    , _shortcuts()
{
    QKeySequence seq0;

    for (std::list<std::string>::const_iterator it = actionIDs.begin(); it != actionIDs.end(); ++it) {
        QString actionIDStr = QString::fromUtf8( it->c_str() );
        std::list<QKeySequence> seq = getKeybind(_group, actionIDStr);
        if ( seq.empty() ) {
            seq.push_back( QKeySequence() );
        }
        _shortcuts.push_back( std::make_pair( actionIDStr, seq.front() ) );
        if ( it == actionIDs.begin() ) {
            seq0 = seq.front();
        }
        appPTR->addShortcutAction(_group, actionIDStr, this);
    }
    assert ( !_group.isEmpty() && !actionIDs.empty() );
    if (setShortcutOnAction) {
        setShortcut(seq0);
    }

    setShortcutContext(Qt::WindowShortcut);
    setText( tr( actionDescription.c_str() ) );
}

ActionWithShortcut::~ActionWithShortcut()
{
    assert ( !_group.isEmpty() && !_shortcuts.empty() );
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        appPTR->removeShortcutAction(_group, _shortcuts[i].first, this);
    }
}

void
ActionWithShortcut::setShortcutWrapper(const QString& actionID,
                                       const QKeySequence& shortcut)
{
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        if (_shortcuts[i].first == actionID) {
            _shortcuts[i].second = shortcut;
        }
    }
    setShortcut(shortcut);
}

ToolTipActionShortcut::ToolTipActionShortcut(const std::string & group,
                                             const std::string & actionID,
                                             const std::string & toolip,
                                             QWidget* parent)
    : ActionWithShortcut(group, actionID, std::string(), parent, false)
    , _widget(parent)
    , _originalToolTip( QString::fromUtf8( toolip.c_str() ) )
    , _tooltipSetInternally(false)
{
    assert(parent);
    setToolTipFromOriginalToolTip();
    _widget->installEventFilter(this);
}

ToolTipActionShortcut::ToolTipActionShortcut(const std::string & group,
                                             const std::list<std::string> & actionIDs,
                                             const std::string & toolip,
                                             QWidget* parent)
    : ActionWithShortcut(group, actionIDs, std::string(), parent, false)
    , _widget(parent)
    , _originalToolTip( QString::fromUtf8( toolip.c_str() ) )
    , _tooltipSetInternally(false)
{
    assert(parent);
    setToolTipFromOriginalToolTip();
    _widget->installEventFilter(this);
}

void
ToolTipActionShortcut::setToolTipFromOriginalToolTip()
{
    QString finalToolTip = _originalToolTip;

    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        finalToolTip = finalToolTip.arg( _shortcuts[i].second.toString(QKeySequence::NativeText) );
    }

    _tooltipSetInternally = true;
    _widget->setToolTip(finalToolTip);
    _tooltipSetInternally = false;
}

bool
ToolTipActionShortcut::eventFilter(QObject* watched,
                                   QEvent* event)
{
    if (watched != _widget) {
        return false;
    }
    if (event->type() == QEvent::ToolTipChange) {
        if (_tooltipSetInternally) {
            return false;
        }
        _originalToolTip = _widget->toolTip();
        setToolTipFromOriginalToolTip();
    }

    return false;
}

void
ToolTipActionShortcut::setShortcutWrapper(const QString& actionID,
                                          const QKeySequence& shortcut)
{
    for (std::size_t i = 0; i < _shortcuts.size(); ++i) {
        if (_shortcuts[i].first == actionID) {
            _shortcuts[i].second = shortcut;
        }
    }
    setToolTipFromOriginalToolTip();
}

NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_ActionShortcuts.cpp"
