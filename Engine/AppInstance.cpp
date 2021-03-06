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

#include "AppInstance.h"

#include <fstream>
#include <list>
#include <cassert>
#include <stdexcept>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5
#include <QtCore/QUrl>
#include <QtCore/QFileInfo>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtNetwork/QNetworkReply>

#if !defined(SBK_RUN) && !defined(Q_MOC_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// /usr/local/include/boost/bind/arg.hpp:37:9: warning: unused typedef 'boost_static_assert_typedef_37' [-Wunused-local-typedef]
#include <boost/bind.hpp>
#include <boost/algorithm/string/predicate.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
#endif

#include "Global/QtCompat.h" // removeFileExtension

#include "Engine/BlockingBackgroundRender.h"
#include "Engine/CLArgs.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/FileDownloader.h"
#include "Engine/GroupOutput.h"
#include "Engine/DiskCacheNode.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ProcessHandler.h"
#include "Engine/KnobFile.h"
#include "Engine/ReadNode.h"
#include "Engine/SerializableWindow.h"
#include "Engine/Settings.h"
#include "Engine/PyPanelI.h"
#include "Engine/TabWidgetI.h"
#include "Engine/ViewerInstance.h"
#include "Engine/WriteNode.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

#include <SequenceParsing.h> // for SequenceParsing::removePath


NATRON_NAMESPACE_ENTER;

FlagSetter::FlagSetter(bool initialValue,
                       bool* p)
    : p(p)
    , lock(0)
{
    *p = initialValue;
}

FlagSetter::FlagSetter(bool initialValue,
                       bool* p,
                       QMutex* mutex)
    : p(p)
    , lock(mutex)
{
    lock->lock();
    *p = initialValue;
    lock->unlock();
}

FlagSetter::~FlagSetter()
{
    if (lock) {
        lock->lock();
    }
    *p = !*p;
    if (lock) {
        lock->unlock();
    }
}

FlagIncrementer::FlagIncrementer(int* p)
    : p(p)
    , lock(0)
{
    *p = *p + 1;
}

FlagIncrementer::FlagIncrementer(int* p,
                                 QMutex* mutex)
    : p(p)
    , lock(mutex)
{
    lock->lock();
    *p = *p + 1;
    lock->unlock();
}

FlagIncrementer::~FlagIncrementer()
{
    if (lock) {
        lock->lock();
    }
    *p = *p - 1;
    if (lock) {
        lock->unlock();
    }
}

struct RenderQueueItem
{
    AppInstance::RenderWork work;
    QString sequenceName;
    QString savePath;
    ProcessHandlerPtr process;
};

class CreateNodeStackItem;
typedef boost::shared_ptr<CreateNodeStackItem> CreateNodeStackItemPtr;
typedef boost::weak_ptr<CreateNodeStackItem> CreateNodeStackItemWPtr;
typedef std::list<CreateNodeStackItemPtr> CreateNodeStackItemPtrList;

class CreateNodeStackItem
{
public:

    NodePtr node;

    CreateNodeArgsPtr args;

    CreateNodeStackItemWPtr parent;
    CreateNodeStackItemPtrList children;

    CreateNodeStackItem()
    : node()
    , args()
    , parent()
    {

    }
/*
    bool isWithinPyPlugRecursive() const
    {
        if (isPyPlug) {
            return true;
        }
        CreateNodeStackItemPtr p = parent.lock();
        if (p) {
            return p->isWithinPyPlugRecursive();
        }
        return false;
    }
*/
    bool isGuiDisabledRecursive() const
    {
        assert(args);
        if (args->getProperty<bool>(kCreateNodeArgsPropNoNodeGUI)) {
            return true;
        }
        CreateNodeStackItemPtr p = parent.lock();
        if (p) {
            return p->isGuiDisabledRecursive();
        }
        return false;
    }
};

struct CreateNodeStack {
    CreateNodeStackItemPtrList recursionStack;
    CreateNodeStackItemPtr root;
};

struct AppInstancePrivate
{
    Q_DECLARE_TR_FUNCTIONS(AppInstance)

public:

    // ptr to the public object, can not be a smart ptr
    AppInstance* _publicInterface;

    // ptr to the project

    ProjectPtr _currentProject;

    // the unique ID of this instance
    int _appID;

    // Backward compat flag for Natron 1
    bool _projectCreatedWithLowerCaseIDs;

    // Protects creatingGroupMutex and _creatingTree
    mutable QMutex creatingGroupMutex;

    // Stack to recursively keep track of created nodes
    CreateNodeStack createNodeStack;

    // When a node tree is created to avoid trying to compute meta-data on the tree
    // whilst it is undergoing massive changes
    int _creatingTree;

    mutable QMutex renderQueueMutex;

    // Used to block the calling threads while there are active renders
    QWaitCondition activeRendersNotEmptyCond;
    std::list<RenderQueueItem> renderQueue, activeRenders;
    mutable QMutex invalidExprKnobsMutex;
    std::list<KnobIWPtr> invalidExprKnobs;

    SERIALIZATION_NAMESPACE::ProjectBeingLoadedInfo projectBeingLoaded;

    mutable QMutex uiInfoMutex;

    SerializableWindow* mainWindow;
    std::list<SerializableWindow*> floatingWindows;
    std::list<TabWidgetI*> tabWidgets;
    std::list<SplitterI*> splitters;
    std::list<PyPanelI*> pythonPanels;
    std::list<DockablePanelI*> openedSettingsPanels;

    AppInstancePrivate(int appID,
                       AppInstance* app)

        : _publicInterface(app)
        , _currentProject()
        , _appID(appID)
        , _projectCreatedWithLowerCaseIDs(false)
        , creatingGroupMutex()
        , createNodeStack()
        , _creatingTree(0)
        , renderQueueMutex()
        , renderQueue()
        , activeRenders()
        , invalidExprKnobsMutex()
        , invalidExprKnobs()
        , projectBeingLoaded()
        , mainWindow(0)
        , floatingWindows()
        , tabWidgets()
        , pythonPanels()
    {
    }

    void declareCurrentAppVariable_Python();


    void executeCommandLinePythonCommands(const CLArgs& args);

    bool validateRenderOptions(const AppInstance::RenderWork& w,
                               int* firstFrame,
                               int* lastFrame,
                               int* frameStep);

    void getSequenceNameFromWriter(const OutputEffectInstancePtr& writer, QString* sequenceName);

    void renderSequentialInternal(const RenderQueueItem& writerWork);

    void checkNumberOfNonFloatingPanes();

};

AppInstance::AppInstance(int appID)
    : QObject()
    , _imp( new AppInstancePrivate(appID, this) )
{
}

AppInstance::~AppInstance()
{
    _imp->_currentProject->clearNodesBlocking();
}

const SERIALIZATION_NAMESPACE::ProjectBeingLoadedInfo&
AppInstance::getProjectBeingLoadedInfo() const
{
    assert(QThread::currentThread() == qApp->thread());
    return _imp->projectBeingLoaded;
}

void
AppInstance::setProjectBeingLoadedInfo(const SERIALIZATION_NAMESPACE::ProjectBeingLoadedInfo& info)
{
    assert(QThread::currentThread() == qApp->thread());
    _imp->projectBeingLoaded = info;
}


bool
AppInstance::isTopLevelNodeBeingCreated(const NodePtr& node) const
{
    assert( QThread::currentThread() == qApp->thread() );

    if (!_imp->createNodeStack.root) {
        return false;
    }
    return _imp->createNodeStack.root->node == node;
}

bool
AppInstance::isCreatingNodeTree() const
{
    QMutexLocker k(&_imp->creatingGroupMutex);

    return _imp->_creatingTree;
}

void
AppInstance::setIsCreatingNodeTree(bool b)
{
    QMutexLocker k(&_imp->creatingGroupMutex);

    if (b) {
        ++_imp->_creatingTree;
    } else {
        if (_imp->_creatingTree >= 1) {
            --_imp->_creatingTree;
        } else {
            _imp->_creatingTree = 0;
        }
    }
}

void
AppInstance::checkForNewVersion() const
{
    FileDownloader* downloader = new FileDownloader( QUrl( QString::fromUtf8(NATRON_LAST_VERSION_URL) ), false );
    QObject::connect( downloader, SIGNAL(downloaded()), this, SLOT(newVersionCheckDownloaded()) );
    QObject::connect( downloader, SIGNAL(error()), this, SLOT(newVersionCheckError()) );

    ///make the call blocking
    QEventLoop loop;

    connect( downloader->getReply(), SIGNAL(finished()), &loop, SLOT(quit()) );
    loop.exec();
}

//return -1 if a < b, 0 if a == b and 1 if a > b
//Returns -2 if not understood
static
int
compareDevStatus(const QString& a,
                 const QString& b)
{
    if ( ( a == QString::fromUtf8(NATRON_DEVELOPMENT_DEVEL) ) || ( a == QString::fromUtf8(NATRON_DEVELOPMENT_SNAPSHOT) ) ) {
        //Do not try updates when update available is a dev build
        return -1;
    } else if ( ( b == QString::fromUtf8(NATRON_DEVELOPMENT_DEVEL) ) || ( b == QString::fromUtf8(NATRON_DEVELOPMENT_SNAPSHOT) ) ) {
        //This is a dev build, do not try updates
        return -1;
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_STABLE) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_STABLE) ) {
            return 0;
        } else {
            return 1;
        }
    }
    assert(false);

    return -2;
}

void
AppInstance::newVersionCheckDownloaded()
{
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );

    assert(downloader);

    QString extractedFileVersionStr, extractedSoftwareVersionStr, extractedDevStatusStr, extractedBuildNumberStr;
    QString fileVersionTag( QString::fromUtf8("File version: ") );
    QString softwareVersionTag( QString::fromUtf8("Software version: ") );
    QString devStatusTag( QString::fromUtf8("Development status: ") );
    QString buildNumberTag( QString::fromUtf8("Build number: ") );
    QString data( QString::fromUtf8( downloader->downloadedData() ) );
    QTextStream ts(&data);

    while ( !ts.atEnd() ) {
        QString line = ts.readLine();
        if ( line.startsWith( QChar::fromLatin1('#') ) || line.startsWith( QChar::fromLatin1('\n') ) ) {
            continue;
        }

        if ( line.startsWith(fileVersionTag) ) {
            int i = fileVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedFileVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(softwareVersionTag) ) {
            int i = softwareVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedSoftwareVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(devStatusTag) ) {
            int i = devStatusTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedDevStatusStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(buildNumberTag) ) {
            int i = buildNumberTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedBuildNumberStr.push_back( line.at(i) );
                ++i;
            }
        }
    }

    downloader->deleteLater();


    if ( extractedFileVersionStr.isEmpty() || (extractedFileVersionStr.toInt() < NATRON_LAST_VERSION_FILE_VERSION) ) {
        //The file cannot be decoded here
        return;
    }


    QStringList versionDigits = extractedSoftwareVersionStr.split( QChar::fromLatin1('.') );

    ///we only understand 3 digits formed version numbers
    if (versionDigits.size() != 3) {
        return;
    }


    int buildNumber = extractedBuildNumberStr.toInt();
    int major = versionDigits[0].toInt();
    int minor = versionDigits[1].toInt();
    int revision = versionDigits[2].toInt();
    const QString currentDevStatus = QString::fromUtf8(NATRON_DEVELOPMENT_STATUS);
    int devStatCompare = compareDevStatus(extractedDevStatusStr, currentDevStatus);
    int versionEncoded = NATRON_VERSION_ENCODE(major, minor, revision);
    bool hasUpdate = false;

    if ( (versionEncoded > NATRON_VERSION_ENCODED) ||
         ( ( versionEncoded == NATRON_VERSION_ENCODED) &&
           ( ( devStatCompare > 0) || ( ( devStatCompare == 0) && ( buildNumber > NATRON_BUILD_NUMBER) ) ) ) ) {
        if (devStatCompare == 0) {
            if ( ( buildNumber > NATRON_BUILD_NUMBER) && ( versionEncoded == NATRON_VERSION_ENCODED) ) {
                hasUpdate = true;
            } else if (versionEncoded > NATRON_VERSION_ENCODED) {
                hasUpdate = true;
            }
        } else {
            hasUpdate = true;
        }
    }
    if (hasUpdate) {
        const QString popen = QString::fromUtf8("<p>");
        const QString pclose = QString::fromUtf8("</p>");
        QString text =  popen
               + tr("Updates for %1 are now available for download.")
               .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
               + pclose
               + popen
               + tr("You are currently using %1 version %2 - %3.")
               .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
               .arg( QString::fromUtf8(NATRON_VERSION_STRING) )
               .arg( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) )
               + pclose
               + popen
               + tr("The latest version of %1 is version %4 - %5.")
               .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
               .arg(extractedSoftwareVersionStr)
               .arg(extractedDevStatusStr)
               + pclose
               + popen
               + tr("You can download it from %1.")
               .arg( QString::fromUtf8("<a href=\"https://natron.fr/download\">"
                                       "www.natron.fr</a>") )
               + pclose;
        Dialogs::informationDialog( tr("New version").toStdString(), text.toStdString(), true );
    }
} // AppInstance::newVersionCheckDownloaded

void
AppInstance::newVersionCheckError()
{
    ///Nothing to do,
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );

    assert(downloader);
    downloader->deleteLater();
}

void
AppInstance::getWritersWorkForCL(const CLArgs& cl,
                                 std::list<AppInstance::RenderWork>& requests)
{
    const std::list<CLArgs::WriterArg>& writers = cl.getWriterArgs();

    for (std::list<CLArgs::WriterArg>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        NodePtr writerNode;
        if (!it->mustCreate) {
            std::string writerName = it->name.toStdString();
            writerNode = getNodeByFullySpecifiedName(writerName);

            if (!writerNode) {
                QString s = tr("%1 does not belong to the project file. Please enter a valid Write node script-name.").arg(it->name);
                throw std::invalid_argument( s.toStdString() );
            } else {
                if ( !writerNode->isOutputNode() ) {
                    QString s = tr("%1 is not an output node! It cannot render anything.").arg(it->name);
                    throw std::invalid_argument( s.toStdString() );
                }
            }

            if ( !it->filename.isEmpty() ) {
                KnobIPtr fileKnob = writerNode->getKnobByName(kOfxImageEffectFileParamName);
                if (fileKnob) {
                    KnobFilePtr outFile = toKnobFile(fileKnob);
                    if (outFile) {
                        outFile->setValue(it->filename.toStdString());
                    }
                }
            }
        } else {
            CreateNodeArgsPtr args(CreateNodeArgs::create(PLUGINID_NATRON_WRITE, getProject()));
            args->setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
            args->setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
            args->setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            writerNode = createWriter( it->filename.toStdString(), args );
            if (!writerNode) {
                throw std::runtime_error( tr("Failed to create writer for %1.").arg(it->filename).toStdString() );
            }

            //Connect the writer to the corresponding Output node input
            NodePtr output = getProject()->getNodeByFullySpecifiedName( it->name.toStdString() );
            if (!output) {
                throw std::invalid_argument( tr("%1 is not the name of a valid Output node of the script").arg(it->name).toStdString() );
            }
            GroupOutputPtr isGrpOutput = toGroupOutput( output->getEffectInstance() );
            if (!isGrpOutput) {
                throw std::invalid_argument( tr("%1 is not the name of a valid Output node of the script").arg(it->name).toStdString() );
            }
            NodePtr outputInput = output->getRealInput(0);
            if (outputInput) {
                writerNode->connectInput(outputInput, 0);
            }
        }

        assert(writerNode);
        OutputEffectInstancePtr effect = writerNode->isEffectOutput();

        if ( cl.hasFrameRange() ) {
            const std::list<std::pair<int, std::pair<int, int> > >& frameRanges = cl.getFrameRanges();
            for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it2 = frameRanges.begin(); it2 != frameRanges.end(); ++it2) {
                AppInstance::RenderWork r( effect, it2->second.first, it2->second.second, it2->first, cl.areRenderStatsEnabled() );
                requests.push_back(r);
            }
        } else {
            AppInstance::RenderWork r( effect, INT_MIN, INT_MAX, INT_MIN, cl.areRenderStatsEnabled() );
            requests.push_back(r);
        }
    }
} // AppInstance::getWritersWorkForCL

void
AppInstancePrivate::executeCommandLinePythonCommands(const CLArgs& args)
{
    const std::list<std::string>& commands = args.getPythonCommands();

    for (std::list<std::string>::const_iterator it = commands.begin(); it != commands.end(); ++it) {
        std::string err;
        std::string output;
        bool ok  = NATRON_PYTHON_NAMESPACE::interpretPythonScript(*it, &err, &output);
        if (!ok) {
            const QString sp( QString::fromUtf8(" ") );
            QString m = tr("Failed to execute the following Python command:") + sp +
                        QString::fromUtf8( it->c_str() ) + sp +
                        tr("Error:") + sp +
                        QString::fromUtf8( err.c_str() );
            throw std::runtime_error( m.toStdString() );
        } else if ( !output.empty() ) {
            std::cout << output << std::endl;
        }
    }
}

void
AppInstance::load(const CLArgs& cl,
                  bool makeEmptyInstance)
{
    // Initialize the knobs of the project before loading anything else.
    assert(!_imp->_currentProject); // < This function may only be called once per AppInstance
    _imp->_currentProject = Project::create( shared_from_this() );
    _imp->_currentProject->initializeKnobsPublic();

    loadInternal(cl, makeEmptyInstance);
}

void
AppInstance::loadInternal(const CLArgs& cl,
                          bool makeEmptyInstance)
{
    try {
        declareCurrentAppVariable_Python();
    } catch (const std::exception& e) {
        throw std::runtime_error( e.what() );
    }

    if (makeEmptyInstance) {
        return;
    }

    const QString& extraOnProjectCreatedScript = cl.getDefaultOnProjectLoadedScript();

    _imp->executeCommandLinePythonCommands(cl);

    QString exportDocPath = cl.getExportDocsPath();
    if ( !exportDocPath.isEmpty() ) {
        exportDocs(exportDocPath);

        return;
    }

    ///if the app is a background project autorun and the project name is empty just throw an exception.
    if ( ( (appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRun) ||
           ( appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRunLaunchedFromGui) ) ) {
        const QString& scriptFilename =  cl.getScriptFilename();

        if ( scriptFilename.isEmpty() ) {
            // cannot start a background process without a file
            throw std::invalid_argument( tr("Project file name is empty.").toStdString() );
        }


        QFileInfo info(scriptFilename);
        if ( !info.exists() ) {
            throw std::invalid_argument( tr("%1: No such file.").arg(scriptFilename).toStdString() );
        }

        std::list<AppInstance::RenderWork> writersWork;


        if ( info.suffix() == QString::fromUtf8(NATRON_PROJECT_FILE_EXT) ) {
            ///Load the project
            if ( !_imp->_currentProject->loadProject( info.path(), info.fileName() ) ) {
                throw std::invalid_argument( tr("Project file loading failed.").toStdString() );
            }
        } else if ( info.suffix() == QString::fromUtf8("py") ) {
            ///Load the python script
            loadPythonScript(info);
        } else {
            throw std::invalid_argument( tr("%1 only accepts python scripts or .ntp project files.").arg( QString::fromUtf8(NATRON_APPLICATION_NAME) ).toStdString() );
        }


        ///exec the python script specified via --onload
        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }


        getWritersWorkForCL(cl, writersWork);


        ///Set reader parameters if specified from the command-line
        const std::list<CLArgs::ReaderArg>& readerArgs = cl.getReaderArgs();
        for (std::list<CLArgs::ReaderArg>::const_iterator it = readerArgs.begin(); it != readerArgs.end(); ++it) {
            std::string readerName = it->name.toStdString();
            NodePtr readNode = getNodeByFullySpecifiedName(readerName);

            if (!readNode) {
                std::string exc( tr("%1 does not belong to the project file. Please enter a valid Read node script-name.").arg( QString::fromUtf8( readerName.c_str() ) ).toStdString() );
                throw std::invalid_argument(exc);
            } else {
                if ( !readNode->getEffectInstance()->isReader() ) {
                    std::string exc( tr("%1 is not a Read node! It cannot render anything.").arg( QString::fromUtf8( readerName.c_str() ) ).toStdString() );
                    throw std::invalid_argument(exc);
                }
            }

            if ( it->filename.isEmpty() ) {
                std::string exc( tr("%1: Filename specified is empty but [-i] or [--reader] was passed to the command-line.").arg( QString::fromUtf8( readerName.c_str() ) ).toStdString() );
                throw std::invalid_argument(exc);
            }
            KnobIPtr fileKnob = readNode->getKnobByName(kOfxImageEffectFileParamName);
            if (fileKnob) {
                KnobFilePtr outFile = toKnobFile(fileKnob);
                if (outFile) {
                    outFile->setValue(it->filename.toStdString());
                }
            }
        }

        ///launch renders
        if ( !writersWork.empty() ) {
            renderWritersNonBlocking(writersWork);
        } else {
            std::list<std::string> writers;
            startWritersRenderingFromNames( cl.areRenderStatsEnabled(), false, writers, cl.getFrameRanges() );
        }
    } else if (appPTR->getAppType() == AppManager::eAppTypeInterpreter) {
        QFileInfo info( cl.getScriptFilename() );
        if ( info.exists() ) {
            if ( info.suffix() == QString::fromUtf8("py") ) {
                loadPythonScript(info);
            } else if ( info.suffix() == QString::fromUtf8(NATRON_PROJECT_FILE_EXT) ) {
                if ( !_imp->_currentProject->loadProject( info.path(), info.fileName() ) ) {
                    throw std::invalid_argument( tr("Project file loading failed.").toStdString() );
                }
            }
        }

        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }


        appPTR->launchPythonInterpreter();
    } else {
        execOnProjectCreatedCallback();

        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }
    }
} // AppInstance::load

bool
AppInstance::loadPythonScriptAndReportToScriptEditor(const QString& script)
{

    std::string err, output;

    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script.toStdString(), &err, &output);

    if (!ok) {
        QString message( QString::fromUtf8("Failed to load python script: ") );
        message.append( QString::fromUtf8( err.c_str() ) );
        appendToScriptEditor( message.toStdString() );
        return false;
    } else if (!output.empty()) {
        appendToScriptEditor(output);
    }
    return true;
}

bool
AppInstance::loadPythonScript(const QFileInfo& file)
{

    QFile f( file.absoluteFilePath() );
    if ( !f.open(QIODevice::ReadOnly) ) {
        return false;
    }
    QTextStream ts(&f);
    QString content = ts.readAll();
    return loadPythonScriptAndReportToScriptEditor(content);
}

class AddCreateNode_RAII
{
    AppInstancePrivate* _imp;
    CreateNodeStackItemPtr _item;

public:


    AddCreateNode_RAII(AppInstancePrivate* imp,
                       const NodePtr& node,
                       const CreateNodeArgsPtr& args)
        : _imp(imp)
        , _item()
    {
        _item.reset(new CreateNodeStackItem);
        _item->args = args;
        _item->node = node;

        if (!_imp->createNodeStack.recursionStack.empty()) {
            // There is a parent node being created
            const CreateNodeStackItemPtr& parent = _imp->createNodeStack.recursionStack.back();
            parent->children.push_back(_item);
            _item->parent  = parent;
        }

        if (!_imp->createNodeStack.root) {
            _imp->createNodeStack.root = _item;
        }


        // Check recursively if we should create the node UI or not
        bool argsNoNodeGui = args->getProperty<bool>(kCreateNodeArgsPropNoNodeGUI);
        CreateNodeStackItemPtr parent = _item->parent.lock();
        if (!argsNoNodeGui && parent) {
            argsNoNodeGui |= parent->isGuiDisabledRecursive();
            if (argsNoNodeGui) {
                args->setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
            }
        }


        _imp->createNodeStack.recursionStack.push_back(_item);

    }

    virtual ~AddCreateNode_RAII()
    {
        CreateNodeStackItemPtrList::iterator found = std::find(_imp->createNodeStack.recursionStack.begin(), _imp->createNodeStack.recursionStack.end(), _item);

        if ( found != _imp->createNodeStack.recursionStack.end() ) {
            _imp->createNodeStack.recursionStack.erase(found);
        }


        // Now that the group is created and all nodes loaded, autoconnect the group like other nodes.
        // We can only do it now because the group needs to create the internal GroupInput nodes before
        // having its actual inputs.
        
        NodeGroupPtr isGrp = toNodeGroup(_item->node->getEffectInstance());
        if (isGrp) {
            _imp->_publicInterface->onGroupCreationFinished(_item->node, *_item->args);
        }

        if (_item == _imp->createNodeStack.root) {
            _imp->createNodeStack.root.reset();
        }
        
    }
};

NodePtr
AppInstance::createNodeFromPyPlug(const PluginPtr& plugin, const CreateNodeArgsPtr& args)

{
    /*If the plug-in is a toolset, execute the toolset script and don't actually create a node*/
    bool istoolsetScript = plugin->getProperty<bool>(kNatronPluginPropPyPlugIsToolset);
    NodePtr node;

    SERIALIZATION_NAMESPACE::NodeSerializationPtr serialization = args->getProperty<SERIALIZATION_NAMESPACE::NodeSerializationPtr >(kCreateNodeArgsPropNodeSerialization);
    NodeCollectionPtr group = args->getProperty<NodeCollectionPtr >(kCreateNodeArgsPropGroupContainer);

    std::string pyPlugFile = plugin->getProperty<std::string>(kNatronPluginPropPyPlugScriptAbsoluteFilePath);
    std::string pyPlugDirPath;

    std::size_t foundSlash = pyPlugFile.find_last_of("/");
    if (foundSlash != std::string::npos) {
        pyPlugDirPath = pyPlugFile.substr(0, foundSlash);
    }

    std::string pyPlugID = plugin->getPluginID();

    // Backward compat with older PyPlugs using Python scripts
    bool isPyPlugEncodedWithPythonScript = plugin->getProperty<bool>(kNatronPluginPropPyPlugIsPythonScript);
    QString extScriptFile = QString::fromUtf8(plugin->getProperty<std::string>(kNatronPluginPropPyPlugExtScriptFile).c_str());
    if (!isPyPlugEncodedWithPythonScript && !extScriptFile.isEmpty()) {
        // A pyplug might have custom functions defined in a custmo Python script, check if such
        // file exists. If so import it

        QString moduleName = extScriptFile;
        // Remove extension to get module name
        {
            int foundDot = moduleName.lastIndexOf(QLatin1Char('.'));
            if (foundDot != -1) {
                moduleName = moduleName.mid(0, foundDot);
            }
        }


        QString script = QString::fromUtf8("import %1").arg(moduleName);
        std::string error, output;
        if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script.toStdString(), &error, &output)) {
            appendToScriptEditor(error);
        } else if (!output.empty()) {
            appendToScriptEditor(output);
        }
    }

    std::string originalPluginID = plugin->getProperty<std::string>(kNatronPluginPropPyPlugContainerID);
    if (originalPluginID.empty()) {
        originalPluginID = PLUGINID_NATRON_GROUP;
    }
    {
        
        
        CreatingNodeTreeFlag_RAII createNodeTree( shared_from_this() );
        NodePtr containerNode;
        if (!istoolsetScript) {
            CreateNodeArgsPtr groupArgs(new CreateNodeArgs(*args));
            groupArgs->setProperty<bool>(kCreateNodeArgsPropSubGraphOpened, false);
            groupArgs->setProperty<std::string>(kCreateNodeArgsPropPluginID, originalPluginID);
            groupArgs->setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
            groupArgs->setProperty<std::string>(kCreateNodeArgsPropPyPlugID, pyPlugID);
            containerNode = createNode(groupArgs);
            if (!containerNode) {
                return containerNode;
            }

        }
        node = containerNode;

        // For older pyPlugs with Python script, we must create the nodes now.
        // For newer pyPlugs this is taken care of in the Node::load function when creating
        // the container group
        if (isPyPlugEncodedWithPythonScript) {
            boost::scoped_ptr<AddCreateNode_RAII> creatingNode_raii;
            if (containerNode) {
                creatingNode_raii.reset(new AddCreateNode_RAII(_imp.get(), containerNode, args));
            }
            std::string containerFullySpecifiedName;
            if (containerNode) {
                containerFullySpecifiedName = containerNode->getFullyQualifiedName();
            }

            std::string pythonModuleName = pyPlugFile.substr(foundSlash + 1);

            // Remove file exstension
            std::size_t foundDot = pythonModuleName.find_last_of(".");
            if (foundDot != std::string::npos) {
                pythonModuleName = pythonModuleName.substr(0, foundDot);
            }

            int appID = getAppID() + 1;
            std::stringstream ss;
            ss << pythonModuleName;
            ss << ".createInstance(app" << appID;
            if (istoolsetScript) {
                ss << ",\"\"";
            } else {
                ss << ", app" << appID << "." << containerFullySpecifiedName;
            }
            ss << ")\n";
            std::string err;
            std::string output;
            if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(ss.str(), &err, &output) ) {
                Dialogs::errorDialog(tr("Group plugin creation error").toStdString(), err);
                if (containerNode) {
                    containerNode->destroyNode(false, false);
                }

                return node;
            } else {
                if ( !output.empty() ) {
                    appendToScriptEditor(output);
                }
                node = containerNode;
            }
            if (istoolsetScript) {
                return NodePtr();
            }



            // If there's a serialization, restore the serialization of the group node because the Python script probably overriden any state
            if (serialization) {
                containerNode->fromSerialization(*serialization);
            }
            
            // Now that we ran the python script, refresh the default page order so it doesn't get serialized into the project for nothing
            containerNode->refreshDefaultPagesOrder();
        } // isPyPlugEncodedWithPythonScript
        
    } // CreatingNodeTreeFlag_RAII

    return node;
} // AppInstance::createNodeFromPythonModule



NodePtr
AppInstance::createReader(const std::string& filename,
                          const CreateNodeArgsPtr& args)
{
    std::string pluginID;

    args->addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, filename);
    std::string canonicalFilename = filename;
    getProject()->canonicalizePath(canonicalFilename);

    int firstFrame, lastFrame;
    Node::getOriginalFrameRangeForReader(pluginID, canonicalFilename, &firstFrame, &lastFrame);
    std::vector<int> originalRange(2);
    originalRange[0] = firstFrame;
    originalRange[1] = lastFrame;
    args->addParamDefaultValueN<int>(kReaderParamNameOriginalFrameRange, originalRange);

    return createNode(args);
}

NodePtr
AppInstance::createWriter(const std::string& filename,
                          const CreateNodeArgsPtr& args,
                          int firstFrame,
                          int lastFrame)
{

    args->addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, filename);
    if ( (firstFrame != INT_MIN) && (lastFrame != INT_MAX) ) {
        args->addParamDefaultValue<int>("frameRange", 2);
        args->addParamDefaultValue<int>("firstFrame", firstFrame);
        args->addParamDefaultValue<int>("lastFrame", lastFrame);
    }

    return createNode(args);
}

bool
AppInstance::openFileDialogIfNeeded(const CreateNodeArgsPtr& args)
{

    // True if the caller set a value for the kOfxImageEffectFileParamName parameter
    bool hasDefaultFilename = false;
    {
        std::vector<std::string> defaultParamValues = args->getPropertyN<std::string>(kCreateNodeArgsPropNodeInitialParamValues);
        std::vector<std::string>::iterator foundFileName  = std::find(defaultParamValues.begin(), defaultParamValues.end(), std::string(kOfxImageEffectFileParamName));
        if (foundFileName != defaultParamValues.end()) {
            std::string propName(kCreateNodeArgsPropParamValue);
            propName += "_";
            propName += kOfxImageEffectFileParamName;
            hasDefaultFilename = !args->getProperty<std::string>(propName).empty();
        }
    }

    SERIALIZATION_NAMESPACE::NodeSerializationPtr serialization = args->getProperty<SERIALIZATION_NAMESPACE::NodeSerializationPtr >(kCreateNodeArgsPropNodeSerialization);

    bool isSilent = args->getProperty<bool>(kCreateNodeArgsPropSilent);
    bool isPersistent = !args->getProperty<bool>(kCreateNodeArgsPropVolatile);
    bool hasGui = !args->getProperty<bool>(kCreateNodeArgsPropNoNodeGUI);
    bool mustOpenDialog = !isSilent && !serialization && isPersistent && !hasDefaultFilename && hasGui && !isBackground();

    if (mustOpenDialog) {
        std::string pattern = openImageFileDialog();
        if (!pattern.empty()) {
            args->addParamDefaultValue(kOfxImageEffectFileParamName, pattern);
            return true;
        }

        // User canceled operation
        return false;
    } else {
        // We already have a filename
        return true;
    }
}

NodePtr
AppInstance::createNodeInternal(const CreateNodeArgsPtr& args)
{
    NodePtr node;
    PluginPtr plugin;

    SERIALIZATION_NAMESPACE::NodeSerializationPtr serialization = args->getProperty<SERIALIZATION_NAMESPACE::NodeSerializationPtr >(kCreateNodeArgsPropNodeSerialization);

    QString argsPluginID = QString::fromUtf8(args->getProperty<std::string>(kCreateNodeArgsPropPluginID).c_str());
    int versionMajor = args->getProperty<int>(kCreateNodeArgsPropPluginVersion, 0);
    int versionMinor = args->getProperty<int>(kCreateNodeArgsPropPluginVersion, 1);

    bool isSilentCreation = args->getProperty<bool>(kCreateNodeArgsPropSilent);

    QString findId = argsPluginID;

    NodePtr argsIOContainer = args->getProperty<NodePtr>(kCreateNodeArgsPropMetaNodeContainer);
    //If it is a reader or writer, create a ReadNode or WriteNode
    if (!argsIOContainer) {
        if ( ReadNode::isBundledReader( argsPluginID.toStdString(), wasProjectCreatedWithLowerCaseIDs() ) ) {
            args->addParamDefaultValue(kNatronReadNodeParamDecodingPluginID, argsPluginID.toStdString());
            findId = QString::fromUtf8(PLUGINID_NATRON_READ);
        } else if ( WriteNode::isBundledWriter( argsPluginID.toStdString(), wasProjectCreatedWithLowerCaseIDs() ) ) {
            args->addParamDefaultValue(kNatronWriteNodeParamEncodingPluginID, argsPluginID.toStdString());
            findId = QString::fromUtf8(PLUGINID_NATRON_WRITE);
        }
    }

    try {
        plugin = appPTR->getPluginBinary(findId, versionMajor, versionMinor, _imp->_projectCreatedWithLowerCaseIDs && serialization);
    } catch (const std::exception & e1) {
        ///Ok try with the old Ids we had in Natron prior to 1.0
        try {
            plugin = appPTR->getPluginBinaryFromOldID(argsPluginID, _imp->_projectCreatedWithLowerCaseIDs, versionMajor, versionMinor);
        } catch (const std::exception& e2) {
            if (!isSilentCreation) {
                Dialogs::errorDialog(tr("Plugin error").toStdString(),
                                 tr("Cannot load plug-in executable.").toStdString() + ": " + e2.what(), false );
            }
            return node;
        }
    }

    if (!plugin) {
        return node;
    }

    bool allowUserCreatablePlugins = args->getProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins);
    if ( !plugin->getIsUserCreatable() && !allowUserCreatablePlugins ) {
        //The plug-in should not be instantiable by the user
        qDebug() << "Attempt to create" << argsPluginID << "which is not user creatable";

        return node;
    }


    std::string foundPluginID = plugin->getPluginID();

    {
        bool useDialogForWriters = appPTR->getCurrentSettings()->isFileDialogEnabledForNewWriters();

        // For Read/Write, open file dialog if needed
        if (foundPluginID == PLUGINID_NATRON_READ || (useDialogForWriters && foundPluginID == PLUGINID_NATRON_WRITE)) {
            if (!openFileDialogIfNeeded(args)) {
                return node;
            }
        }
    }

    // If the plug-in is a PyPlug create it with createNodeFromPyPlug()
    std::string pyPlugFile = plugin->getProperty<std::string>(kNatronPluginPropPyPlugScriptAbsoluteFilePath);
    if ( !pyPlugFile.empty() ) {
        try {
            return createNodeFromPyPlug(plugin, args);
        } catch (const std::exception& e) {
            if (!isSilentCreation) {
                Dialogs::errorDialog(tr("Plugin error").toStdString(),
                                     tr("Cannot create PyPlug:").toStdString() + e.what(), false );
            }
            return node;
        }
    }


    // Get the group container
    NodeCollectionPtr argsGroup = args->getProperty<NodeCollectionPtr >(kCreateNodeArgsPropGroupContainer);
    if (!argsGroup) {
        argsGroup = getProject();
    }
    assert(argsGroup);

    node = Node::create(shared_from_this(), argsGroup, plugin);


    // Flag that we are creating a node
    AddCreateNode_RAII creatingNode_raii(_imp.get(), node, args);

    {
        // Furnace plug-ins don't handle using the thread pool
        SettingsPtr settings = appPTR->getCurrentSettings();
        if ( !isSilentCreation && boost::starts_with(foundPluginID, "uk.co.thefoundry.furnace") &&
             ( settings->useGlobalThreadPool() || ( settings->getNumberOfParallelRenders() != 1) ) ) {
            StandardButtonEnum reply = Dialogs::questionDialog(tr("Warning").toStdString(),
                                                               tr("The settings of the application are currently set to use "
                                                                  "the global thread-pool for rendering effects.\n"
                                                                  "The Foundry Furnace "
                                                                  "is known not to work well when this setting is checked.\n"
                                                                  "Would you like to turn it off?").toStdString(), false);
            if (reply == eStandardButtonYes) {
                settings->setUseGlobalThreadPool(false);
                settings->setNumberOfParallelRenders(1);
            }
        }

        // If this is a stereo plug-in, check that the project has been set for multi-view
        if (!isSilentCreation) {
            std::vector<std::string> grouping = plugin->getPropertyN<std::string>(kNatronPluginPropGrouping);
            if (!grouping.empty() && grouping[0] == PLUGIN_GROUP_MULTIVIEW) {
                int nbViews = getProject()->getProjectViewsCount();
                if (nbViews < 2) {
                    StandardButtonEnum reply = Dialogs::questionDialog(tr("Multi-View").toStdString(),
                                                                       tr("Using a multi-view node requires the project settings to be setup "
                                                                          "for multi-view.\n"
                                                                          "Would you like to setup the project for stereo?").toStdString(), false);
                    if (reply == eStandardButtonYes) {
                        getProject()->setupProjectForStereo();
                    }
                }
            }
        }
    }

    assert(node);
    // Call load: this will setup the node from the plug-in and its knobs. It also read from the serialization object if any
    try {
        node->load(args);
    } catch (const std::exception & e) {
        if (argsGroup) {
            argsGroup->removeNode(node.get());
        }
        std::string error( e.what() );
        if ( !error.empty() ) {
            if (!isSilentCreation) {
                std::string title("Error while creating node");
                std::string message = title + " " + foundPluginID + ": " + e.what();
                qDebug() << message.c_str();
                errorDialog(title, message, false);
            }
        }
        
        return NodePtr();
    }


    return node;
} // createNodeInternal

NodePtr
AppInstance::createNode(const CreateNodeArgsPtr & args)
{
    return createNodeInternal(args);
}

int
AppInstance::getAppID() const
{
    return _imp->_appID;
}

void
AppInstance::exportDocs(const QString path)
{
    if ( !path.isEmpty() ) {
        QStringList categories;
        QVector<QStringList> plugins;

        // Generate a MD for each plugin
        std::list<std::string> pluginIDs = appPTR->getPluginIDs();
        for (std::list<std::string>::iterator it = pluginIDs.begin(); it != pluginIDs.end(); ++it) {
            QString pluginID = QString::fromUtf8( it->c_str() );
            if (pluginID.isEmpty()) {
                continue;
            }
            PluginPtr plugin = appPTR->getPluginBinary(pluginID, -1, -1, false);
            if (!plugin) {
                continue;
            }
            if (plugin->getProperty<bool>(kNatronPluginPropIsInternalOnly) ) {
                continue;
            }


            std::vector<std::string> groups = plugin->getPropertyN<std::string>(kNatronPluginPropGrouping);

            QString group0 = QString::fromUtf8(groups[0].c_str());
            categories.push_back(group0);

            QStringList plugList;
            plugList << group0  << pluginID << QString::fromUtf8(plugin->getPluginLabel().c_str());
            plugins << plugList;
            CreateNodeArgsPtr args(CreateNodeArgs::create(pluginID.toStdString(), NodeCollectionPtr() ));
            args->setProperty(kCreateNodeArgsPropNoNodeGUI, true);
            args->setProperty(kCreateNodeArgsPropVolatile, true);
            args->setProperty(kCreateNodeArgsPropSilent, true);
            qDebug() << pluginID;
            NodePtr node = createNode(args);
            if (node) {
                QDir mdDir(path);
                if ( !mdDir.exists() ) {
                    mdDir.mkpath(path);
                }

                mdDir.mkdir(QLatin1String("plugins"));
                mdDir.cd(QLatin1String("plugins"));

                QFile imgFile( QString::fromUtf8(plugin->getProperty<std::string>(kNatronPluginPropIconFilePath).c_str()) );
                if ( imgFile.exists() ) {
                    QString dstPath = mdDir.absolutePath() + QString::fromUtf8("/") + pluginID + QString::fromUtf8(".png");
                    if (QFile::exists(dstPath)) {
                        QFile::remove(dstPath);
                    }
                    if ( !imgFile.copy(dstPath) ) {
                        std::cout << "ERROR: failed to copy image: " << imgFile.fileName().toStdString() << std::endl;
                    }
                }

                QString md = node->makeDocumentation(false);
                QFile mdFile( mdDir.absolutePath() + QString::fromUtf8("/") + pluginID + QString::fromUtf8(".md") );
                if ( mdFile.open(QIODevice::Text | QIODevice::WriteOnly) ) {
                    QTextStream out(&mdFile);
                    out << md;
                    mdFile.close();
                } else {
                    std::cout << "ERROR: failed to write to file: " << mdFile.fileName().toStdString() << std::endl;
                }
            }
        }


        // Generate RST for plugin categories
        categories.removeDuplicates();
        QString groupMD;
        groupMD.append( tr("Reference") );
        groupMD.append( QString::fromUtf8("\n") );
        groupMD.append( QString::fromUtf8("=========\n\n") );
        groupMD.append( QString::fromUtf8("Contents:\n\n") );
        groupMD.append( QString::fromUtf8(".. toctree::\n") );
        groupMD.append( QString::fromUtf8("    :maxdepth: 1\n\n") );
        groupMD.append( QString::fromUtf8("    _prefs.rst\n") );

        Q_FOREACH(const QString &category, categories) {
            QString plugMD;

            plugMD.append( category );
            plugMD.append( QString::fromUtf8("\n==========\n\n") );
            plugMD.append( QString::fromUtf8("Contents:\n\n") );
            plugMD.append( QString::fromUtf8(".. toctree::\n") );
            plugMD.append( QString::fromUtf8("    :maxdepth: 1\n\n") );

            Q_FOREACH(const QStringList &currPlugin, plugins) {
                if (currPlugin.size() == 3) {
                    if ( category == currPlugin.at(0) ) {
                        plugMD.append( QString::fromUtf8("    plugins/") + currPlugin.at(1) + QString::fromUtf8(".rst\n") );
                    }
                }
            }
            groupMD.append( QString::fromUtf8("    _group") + category + QString::fromUtf8(".rst\n") );

            QFile plugFile( path + QString::fromUtf8("/_group") + category + QString::fromUtf8(".rst") );
            plugMD.append( QString::fromUtf8("\n") );
            if ( plugFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate) ) {
                QTextStream out(&plugFile);
                out << plugMD;
                plugFile.close();
            }
        }

        // Generate RST for plugins ToC
        QFile groupFile( path + QString::fromUtf8("/_group.rst") );
        groupMD.append( QString::fromUtf8("\n") );
        if ( groupFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate) ) {
            QTextStream out(&groupFile);
            out << groupMD;
            groupFile.close();
        }

        // Generate MD for settings
        SettingsPtr settings = appPTR->getCurrentSettings();
        QString prefsMD = settings->makeHTMLDocumentation(false);
        QFile prefsFile( path + QString::fromUtf8("/_prefs.md") );
        if ( prefsFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate) ) {
            QTextStream out(&prefsFile);
            out << prefsMD;
            prefsFile.close();
        }
    }
} // AppInstance::exportDocs

NodePtr
AppInstance::getNodeByFullySpecifiedName(const std::string & name) const
{
    return _imp->_currentProject->getNodeByFullySpecifiedName(name);
}

ProjectPtr
AppInstance::getProject() const
{
    return _imp->_currentProject;
}

TimeLinePtr
AppInstance::getTimeLine() const
{
    return _imp->_currentProject->getTimeLine();
}

void
AppInstance::errorDialog(const std::string & title,
                         const std::string & message,
                         bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;
}

void
AppInstance::errorDialog(const std::string & title,
                         const std::string & message,
                         bool* stopAsking,
                         bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

void
AppInstance::warningDialog(const std::string & title,
                           const std::string & message,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;
}

void
AppInstance::warningDialog(const std::string & title,
                           const std::string & message,
                           bool* stopAsking,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

void
AppInstance::informationDialog(const std::string & title,
                               const std::string & message,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;
}

void
AppInstance::informationDialog(const std::string & title,
                               const std::string & message,
                               bool* stopAsking,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

StandardButtonEnum
AppInstance::questionDialog(const std::string & title,
                            const std::string & message,
                            bool /*useHtml*/,
                            StandardButtons /*buttons*/,
                            StandardButtonEnum /*defaultButton*/) const
{
    std::cout << "QUESTION: " << title + ": " << message << std::endl;

    return eStandardButtonYes;
}

void
AppInstance::triggerAutoSave()
{
    _imp->_currentProject->triggerAutoSave();
}

void
AppInstance::startWritersRenderingFromNames(bool enableRenderStats,
                                            bool doBlockingRender,
                                            const std::list<std::string>& writers,
                                            const std::list<std::pair<int, std::pair<int, int> > >& frameRanges)
{
    std::list<RenderWork> renderers;

    if ( !writers.empty() ) {
        for (std::list<std::string>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
            const std::string& writerName = *it;
            NodePtr node = getNodeByFullySpecifiedName(writerName);

            if (!node) {
                std::string exc(writerName);
                exc.append( tr(" does not belong to the project file. Please enter a valid Write node script-name.").toStdString() );
                throw std::invalid_argument(exc);
            } else {
                if ( !node->isOutputNode() ) {
                    std::string exc(writerName);
                    exc.append( tr(" is not an output node! It cannot render anything.").toStdString() );
                    throw std::invalid_argument(exc);
                }
                ViewerInstancePtr isViewer = node->isEffectViewerInstance();
                if (isViewer) {
                    throw std::invalid_argument("Internal issue with the project loader...viewers should have been evicted from the project.");
                }

                OutputEffectInstancePtr effect = node->isEffectOutput();
                assert(effect);

                for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it2 = frameRanges.begin(); it2 != frameRanges.end(); ++it2) {
                    RenderWork w(effect, it2->second.first, it2->second.second, it2->first, enableRenderStats);
                    renderers.push_back(w);
                }

                if ( frameRanges.empty() ) {
                    RenderWork r(effect, INT_MIN, INT_MAX, INT_MIN, enableRenderStats);
                    renderers.push_back(r);
                }
            }
        }
    } else {
        //start rendering for all writers found in the project
        std::list<OutputEffectInstancePtr> writers;
        getProject()->getWriters(&writers);

        for (std::list<OutputEffectInstancePtr>::const_iterator it2 = writers.begin(); it2 != writers.end(); ++it2) {
            assert(*it2);
            if (*it2) {
                for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it3 = frameRanges.begin(); it3 != frameRanges.end(); ++it3) {
                    RenderWork w(*it2, it3->second.first, it3->second.second, it3->first, enableRenderStats);
                    renderers.push_back(w);
                }

                if ( frameRanges.empty() ) {
                    RenderWork r(*it2, INT_MIN, INT_MAX, INT_MIN, enableRenderStats);
                    renderers.push_back(r);
                }
            }
        }
    }


    if ( renderers.empty() ) {
        throw std::invalid_argument("Project file is missing a writer node. This project cannot render anything.");
    }
    if (doBlockingRender) {
        renderWritersBlocking(renderers);
    } else {
        renderWritersNonBlocking(renderers);
    }

} // AppInstance::startWritersRenderingFromNames

void
AppInstance::renderWritersInternal(bool doBlockingRender,
                                   const std::list<RenderWork>& writers)
{
    if ( writers.empty() ) {
        return;
    }

    // If queueing is enabled and we have to render multiple writers, render them in order
    bool isQueuingEnabled = appPTR->getCurrentSettings()->isRenderQueuingEnabled();

    // If enabled, we launch the render in a separate process launching NatronRenderer
    bool renderInSeparateProcess = appPTR->getCurrentSettings()->isRenderInSeparatedProcessEnabled();

    // When launching in a separate process, make a temporary save file that we pass to NatronRenderer
    QString savePath;
    if (renderInSeparateProcess) {
        getProject()->saveProject_imp(QString(), QString(), true /*isAutoSave*/, false /*updateprojectProperties*/, &savePath);
    }

    // For all items to render, create the background process if needed or connect signals to enable the queue
    // Also notify the GUI that an item was added to the queue
    std::list<RenderQueueItem> itemsToQueue;
    for (std::list<RenderWork>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        RenderQueueItem item;
        item.work = *it;

        // Check that the render options are OK
        if ( !_imp->validateRenderOptions(item.work, &item.work.firstFrame, &item.work.lastFrame, &item.work.frameStep) ) {
            continue;
        }

        _imp->getSequenceNameFromWriter(it->writer, &item.sequenceName);
        item.savePath = savePath;

        if (renderInSeparateProcess) {
            item.process.reset( new ProcessHandler(savePath, item.work.writer) );
            QObject::connect( item.process.get(), SIGNAL(processFinished(int)), this, SLOT(onBackgroundRenderProcessFinished()) );
        } else {
            QObject::connect(item.work.writer->getRenderEngine().get(), SIGNAL(renderFinished(int)), this, SLOT(onQueuedRenderFinished(int)), Qt::UniqueConnection);
        }

        bool canPause = !item.work.writer->isVideoWriter();

        if (!it->isRestart) {
            notifyRenderStarted(item.sequenceName, item.work.firstFrame, item.work.lastFrame, item.work.frameStep, canPause, item.work.writer, item.process);
        } else {
            notifyRenderRestarted(item.work.writer, item.process);
        }
        itemsToQueue.push_back(item);
    }

    // No valid render, bail out
    if ( itemsToQueue.empty() ) {
        return;
    }

    if (!isQueuingEnabled) {
        // Just launch everything
        for (std::list<RenderQueueItem>::const_iterator it = itemsToQueue.begin(); it != itemsToQueue.end(); ++it) {
            _imp->renderSequentialInternal(*it);
        }
    } else {
        QMutexLocker k(&_imp->renderQueueMutex);
        if ( !_imp->activeRenders.empty() ) {
            _imp->renderQueue.insert( _imp->renderQueue.end(), itemsToQueue.begin(), itemsToQueue.end() );

            return;
        } else {
            std::list<RenderQueueItem>::const_iterator it = itemsToQueue.begin();
            const RenderQueueItem& firstWork = *it;
            ++it;
            for (; it != itemsToQueue.end(); ++it) {
                _imp->renderQueue.push_back(*it);
            }
            k.unlock();
            _imp->renderSequentialInternal(firstWork);
        }
    }
    if (doBlockingRender) {
        QMutexLocker k(&_imp->renderQueueMutex);
        while (!_imp->activeRenders.empty()) {
            // check every 50ms if the queue is not empty
            k.unlock();
            // process events so that the onQueuedRenderFinished slot can be called
            // to clear the activeRenders queue if needed
            QCoreApplication::processEvents();
            k.relock();
            _imp->activeRendersNotEmptyCond.wait(&_imp->renderQueueMutex, 50);
        }
    }


} // AppInstance::startWritersRendering

void
AppInstance::renderWritersBlocking(const std::list<RenderWork>& writers)
{
    renderWritersInternal(true, writers);
}

void
AppInstance::renderWritersNonBlocking(const std::list<RenderWork>& writers)
{
    bool blocking = appPTR->isBackground();
    renderWritersInternal(blocking, writers);
}

void
AppInstancePrivate::getSequenceNameFromWriter(const OutputEffectInstancePtr& writer,
                                              QString* sequenceName)
{
    ///get the output file knob to get the name of the sequence
    DiskCacheNodePtr isDiskCache = boost::dynamic_pointer_cast<DiskCacheNode>(writer);

    if (isDiskCache) {
        *sequenceName = tr("Caching");
    } else {
        *sequenceName = QString();
        KnobIPtr fileKnob = writer->getKnobByName(kOfxImageEffectFileParamName);
        if (fileKnob) {
            KnobStringBasePtr isString = toKnobStringBase(fileKnob);
            assert(isString);
            if (isString) {
                *sequenceName = QString::fromUtf8( isString->getValue().c_str() );
            }
        }
    }
}

bool
AppInstancePrivate::validateRenderOptions(const AppInstance::RenderWork& w,
                                          int* firstFrame,
                                          int* lastFrame,
                                          int* frameStep)
{
    ///validate the frame range to render
    if ( (w.firstFrame == INT_MIN) || (w.lastFrame == INT_MAX) ) {
        double firstFrameD, lastFrameD;
        w.writer->getFrameRange_public(0, &firstFrameD, &lastFrameD);
        if ( (firstFrameD == INT_MIN) || (lastFrameD == INT_MAX) ) {
            _publicInterface->getFrameRange(&firstFrameD, &lastFrameD);
        }

        if (firstFrameD > lastFrameD) {
            Dialogs::errorDialog(w.writer->getNode()->getLabel_mt_safe(),
                                 tr("First frame index in the sequence is greater than the last frame index.").toStdString(), false );

            return false;
        }
        *firstFrame = (int)firstFrameD;
        *lastFrame = (int)lastFrameD;
    } else {
        *firstFrame = w.firstFrame;
        *lastFrame = w.lastFrame;
    }

    if ( (w.frameStep == INT_MAX) || (w.frameStep == INT_MIN) ) {
        ///Get the frame step from the frame step parameter of the Writer
        *frameStep = w.writer->getNode()->getFrameStepKnobValue();
    } else {
        *frameStep = std::max(1, w.frameStep);
    }

    return true;
}
#if 0
void
AppInstancePrivate::renderSequentialBlockingInternal(const RenderQueueItem& w)
{
    BlockingBackgroundRender backgroundRender(w.work.writer);

    // This function doesn't return before rendering is finished
    backgroundRender.blockingRender(w.work.useRenderStats, w.work.firstFrame, w.work.lastFrame, w.work.frameStep);
}
#endif

void
AppInstancePrivate::renderSequentialInternal(const RenderQueueItem& w)
{
    {
        QMutexLocker k(&renderQueueMutex);
        activeRenders.push_back(w);
    }
    if (w.process) {
        w.process->startProcess();
    } else {
        w.work.writer->renderSequential(false, w.work.useRenderStats, NULL, w.work.firstFrame, w.work.lastFrame, w.work.frameStep);
    }
}

/**
 * @brief Called when a render started with renderWritersInternal is finished
 **/
void
AppInstance::onQueuedRenderFinished(int /*retCode*/)
{
    RenderEngine* engine = qobject_cast<RenderEngine*>( sender() );

    if (!engine) {
        return;
    }
    OutputEffectInstancePtr effect = engine->getOutput();
    if (!effect) {
        return;
    }
    startNextQueuedRender(effect);
}

void
AppInstance::removeRenderFromQueue(const OutputEffectInstancePtr& writer)
{
    QMutexLocker k(&_imp->renderQueueMutex);

    for (std::list<RenderQueueItem>::iterator it = _imp->renderQueue.begin(); it != _imp->renderQueue.end(); ++it) {
        if (it->work.writer == writer) {
            _imp->renderQueue.erase(it);
            break;
        }
    }
}

void
AppInstance::startNextQueuedRender(const OutputEffectInstancePtr& finishedWriter)
{
    RenderQueueItem nextWork;

    // Do not make the process die under the mutex otherwise we may deadlock
    ProcessHandlerPtr processDying;
    {
        QMutexLocker k(&_imp->renderQueueMutex);
        for (std::list<RenderQueueItem>::iterator it = _imp->activeRenders.begin(); it != _imp->activeRenders.end(); ++it) {
            if (it->work.writer == finishedWriter) {
                processDying = it->process;
                _imp->activeRenders.erase(it);
                _imp->activeRendersNotEmptyCond.wakeAll();
                break;
            }
        }
        if ( !_imp->renderQueue.empty() ) {
            nextWork = _imp->renderQueue.front();
            _imp->renderQueue.pop_front();
        } else {
            return;
        }
    }
    processDying.reset();

    _imp->renderSequentialInternal(nextWork);
}

void
AppInstance::onBackgroundRenderProcessFinished()
{
    ProcessHandler* proc = qobject_cast<ProcessHandler*>( sender() );
    OutputEffectInstancePtr effect;

    if (proc) {
        effect = proc->getWriter();
    }
    if (effect) {
        startNextQueuedRender(effect);
    }
}

void
AppInstance::getFrameRange(double* first,
                           double* last) const
{
    return _imp->_currentProject->getFrameRange(first, last);
}

void
AppInstance::clearOpenFXPluginsCaches()
{
    NodesList activeNodes;

    _imp->_currentProject->getActiveNodes(&activeNodes);

    for (NodesList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->purgeAllInstancesCaches();
    }
}

void
AppInstance::clearAllLastRenderedImages()
{
    NodesList activeNodes;

    _imp->_currentProject->getNodes_recursive(activeNodes, false);

    for (NodesList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->clearLastRenderedImage();
    }
}

void
AppInstance::aboutToQuit()
{
    ///Clear nodes now, not in the destructor of the project as
    ///deleting nodes might reference the project.
    _imp->_currentProject->reset(true /*aboutToQuit*/, /*blocking*/true);
}

void
AppInstance::quit()
{
    appPTR->quit( shared_from_this() );
}

void
AppInstance::quitNow()
{
    appPTR->quitNow( shared_from_this() );
}

ViewerColorSpaceEnum
AppInstance::getDefaultColorSpaceForBitDepth(ImageBitDepthEnum bitdepth) const
{
    return _imp->_currentProject->getDefaultColorSpaceForBitDepth(bitdepth);
}

void
AppInstance::onOCIOConfigPathChanged(const std::string& path)
{
    _imp->_currentProject->onOCIOConfigPathChanged(path, false);
}

void
AppInstance::declareCurrentAppVariable_Python()
{
#ifdef NATRON_RUN_WITHOUT_PYTHON

    return;
#endif
    /// define the app variable
    std::stringstream ss;

    ss << "app" << _imp->_appID + 1 << " = " << NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.getInstance(" << _imp->_appID << ") \n";
    const KnobsVec& knobs = _imp->_currentProject->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        ss << "app" << _imp->_appID + 1 << "." << (*it)->getName() << " = app" << _imp->_appID + 1 << ".getProjectParam('" <<
        (*it)->getName() << "')\n";
    }
    std::string script = ss.str();
    std::string err;
    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("AppInstance::declareCurrentAppVariable_Python() failed!");
    }

    if ( appPTR->isBackground() ) {
        std::string err;
        ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript("app = app1\n", &err, 0);
        assert(ok);
    }
}

double
AppInstance::getProjectFrameRate() const
{
    return _imp->_currentProject->getProjectFrameRate();
}

void
AppInstance::setProjectWasCreatedWithLowerCaseIDs(bool b)
{
    _imp->_projectCreatedWithLowerCaseIDs = b;
}

bool
AppInstance::wasProjectCreatedWithLowerCaseIDs() const
{
    return _imp->_projectCreatedWithLowerCaseIDs;
}

bool
AppInstance::isCreatingNode() const
{
    return _imp->createNodeStack.root.get() != 0;
}

void
AppInstance::appendToScriptEditor(const std::string& str)
{
    std::cout << str <<  std::endl;
}

void
AppInstance::printAutoDeclaredVariable(const std::string& /*str*/)
{
}

void
AppInstance::execOnProjectCreatedCallback()
{
    std::string cb = appPTR->getCurrentSettings()->getOnProjectCreatedCB();

    if ( cb.empty() ) {
        return;
    }


    std::vector<std::string> args;
    std::string error;
    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        appendToScriptEditor( std::string("Failed to run onProjectCreated callback: ")
                              + e.what() );

        return;
    }

    if ( !error.empty() ) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The on project created callback supports the following signature(s):\n");
    signatureError.append("- callback(app)");
    if (args.size() != 1) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);

        return;
    }
    if (args[0] != "app") {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);

        return;
    }
    std::string appID = getAppIDString();
    std::string script;
    if (appID != "app") {
        script = script + "app = " + appID;
    }
    script = script + "\n" + cb + "(" + appID + ")\n";
    std::string err;
    std::string output;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + err);
    } else {
        if ( !output.empty() ) {
            appendToScriptEditor(output);
        }
    }
} // AppInstance::execOnProjectCreatedCallback

std::string
AppInstance::getAppIDString() const
{
    if ( appPTR->isBackground() ) {
        return "app";
    } else {
        QString appID =  QString( QString::fromUtf8("app%1") ).arg(getAppID() + 1);

        return appID.toStdString();
    }
}

void
AppInstance::onGroupCreationFinished(const NodePtr& node, const CreateNodeArgs& args)
{

    assert(node);
    SERIALIZATION_NAMESPACE::NodeSerializationPtr serialization = args.getProperty<SERIALIZATION_NAMESPACE::NodeSerializationPtr >(kCreateNodeArgsPropNodeSerialization);
    if ( !_imp->_currentProject->isLoadingProject() && !serialization ) {
        NodeGroupPtr isGrp = node->isEffectNodeGroup();
        assert(isGrp);
        if (!isGrp) {
            return;
        }
        isGrp->forceComputeInputDependentDataOnAllTrees();
    }
}

bool
AppInstance::saveTemp(const std::string& filename)
{
    std::string outFile = filename;
    std::string path = SequenceParsing::removePath(outFile);
    ProjectPtr project = getProject();

    return project->saveProject_imp(QString::fromUtf8( path.c_str() ), QString::fromUtf8( outFile.c_str() ), false, false, 0);
}

bool
AppInstance::save(const std::string& filename)
{
    ProjectPtr project = getProject();

    if ( project->hasProjectBeenSavedByUser() ) {
        QString projectFilename = project->getProjectFilename();
        QString projectPath = project->getProjectPath();

        return project->saveProject(projectPath, projectFilename, 0);
    } else {
        return saveAs(filename);
    }
}

bool
AppInstance::saveAs(const std::string& filename)
{
    std::string outFile = filename;
    std::string path = SequenceParsing::removePath(outFile);

    return getProject()->saveProject(QString::fromUtf8( path.c_str() ), QString::fromUtf8( outFile.c_str() ), 0);
}

AppInstancePtr
AppInstance::loadProject(const std::string& filename)
{
    QFileInfo file( QString::fromUtf8( filename.c_str() ) );

    if ( !file.exists() ) {
        return AppInstancePtr();
    }
    QString fileUnPathed = file.fileName();
    QString path = file.path() + QChar::fromLatin1('/');

    //We are in background mode, there can only be 1 instance active, wipe the current project
    ProjectPtr project = getProject();
    project->resetProject();

    bool ok  = project->loadProject( path, fileUnPathed);
    if (ok) {
        return shared_from_this();
    }

    project->resetProject();

    return AppInstancePtr();
}

///Close the current project but keep the window
bool
AppInstance::resetProject()
{
    getProject()->reset(false /*aboutToQuit*/, true /*blocking*/);

    return true;
}

///Reset + close window, quit if last window
bool
AppInstance::closeProject()
{
    getProject()->reset(true/*aboutToQuit*/, true /*blocking*/);
    quit();

    return true;
}

///Opens a new project
AppInstancePtr
AppInstance::newProject()
{
    CLArgs cl;
    AppInstancePtr app = appPTR->newAppInstance(cl, false);

    return app;
}

void
AppInstance::addInvalidExpressionKnob(const KnobIPtr& knob)
{
    QMutexLocker k(&_imp->invalidExprKnobsMutex);

    for (std::list<KnobIWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
        if ( it->lock().get() ) {
            return;
        }
    }
    _imp->invalidExprKnobs.push_back(knob);
}

void
AppInstance::removeInvalidExpressionKnob(const KnobIConstPtr& knob)
{
    QMutexLocker k(&_imp->invalidExprKnobsMutex);

    for (std::list<KnobIWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
        if (it->lock() == knob) {
            _imp->invalidExprKnobs.erase(it);
            break;
        }
    }
}

void
AppInstance::recheckInvalidExpressions()
{
    if (getProject()->isProjectClosing()) {
        return;
    }
    std::list<KnobIPtr> knobs;

    {
        QMutexLocker k(&_imp->invalidExprKnobsMutex);
        for (std::list<KnobIWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
            KnobIPtr k = it->lock();
            if (k) {
                knobs.push_back(k);
            }
        }
    }
    std::list<KnobIWPtr> newInvalidKnobs;

    for (std::list<KnobIPtr>::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ( !(*it)->checkInvalidExpressions() ) {
            newInvalidKnobs.push_back(*it);
        }
    }
    {
        QMutexLocker k(&_imp->invalidExprKnobsMutex);
        _imp->invalidExprKnobs = newInvalidKnobs;
    }
}

void
AppInstance::setMainWindowPointer(SerializableWindow* window)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->mainWindow = window;
}

SerializableWindow*
AppInstance::getMainWindowSerialization() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->mainWindow;
}

std::list<SerializableWindow*>
AppInstance::getFloatingWindowsSerialization() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->floatingWindows;
}

std::list<SplitterI*>
AppInstance::getSplittersSerialization() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->splitters;
}

std::list<TabWidgetI*>
AppInstance::getTabWidgetsSerialization() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->tabWidgets;
}

std::list<PyPanelI*>
AppInstance::getPyPanelsSerialization() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->pythonPanels;
}

void
AppInstance::registerFloatingWindow(SerializableWindow* window)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<SerializableWindow*>::iterator found = std::find(_imp->floatingWindows.begin(), _imp->floatingWindows.end(), window);

    if ( found == _imp->floatingWindows.end() ) {
        _imp->floatingWindows.push_back(window);
    }
}

void
AppInstance::unregisterFloatingWindow(SerializableWindow* window)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<SerializableWindow*>::iterator found = std::find(_imp->floatingWindows.begin(), _imp->floatingWindows.end(), window);

    if ( found != _imp->floatingWindows.end() ) {
        _imp->floatingWindows.erase(found);
    }
}

void
AppInstance::registerSplitter(SplitterI* splitter)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<SplitterI*>::iterator found = std::find(_imp->splitters.begin(), _imp->splitters.end(), splitter);

    if ( found == _imp->splitters.end() ) {
        _imp->splitters.push_back(splitter);
    }
}

void
AppInstance::unregisterSplitter(SplitterI* splitter)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<SplitterI*>::iterator found = std::find(_imp->splitters.begin(), _imp->splitters.end(), splitter);

    if ( found != _imp->splitters.end() ) {
        _imp->splitters.erase(found);
    }
}

void
AppInstance::registerTabWidget(TabWidgetI* tabWidget)
{

    onTabWidgetRegistered(tabWidget);
    {
        QMutexLocker k(&_imp->uiInfoMutex);
        bool hasAnchor = false;

        for (std::list<TabWidgetI*>::iterator it = _imp->tabWidgets.begin(); it != _imp->tabWidgets.end(); ++it) {
            if ( (*it)->isAnchor() ) {
                hasAnchor = true;
                break;
            }
        }
        std::list<TabWidgetI*>::iterator found = std::find(_imp->tabWidgets.begin(), _imp->tabWidgets.end(), tabWidget);

        if ( found == _imp->tabWidgets.end() ) {
            if ( _imp->tabWidgets.empty() ) {
                tabWidget->setClosable(false);
            }
            _imp->tabWidgets.push_back(tabWidget);

            if (!hasAnchor) {
                tabWidget->setAsAnchor(true);
            }
        }
    }

}

void
AppInstance::unregisterTabWidget(TabWidgetI* tabWidget)
{
    {
        QMutexLocker k(&_imp->uiInfoMutex);
        std::list<TabWidgetI*>::iterator found = std::find(_imp->tabWidgets.begin(), _imp->tabWidgets.end(), tabWidget);

        if ( found != _imp->tabWidgets.end() ) {
            _imp->tabWidgets.erase(found);
        }
        if ( ( tabWidget->isAnchor() ) && !_imp->tabWidgets.empty() ) {
            _imp->tabWidgets.front()->setAsAnchor(true);
        }
    }
    onTabWidgetUnregistered(tabWidget);
}

void
AppInstance::clearTabWidgets()
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->tabWidgets.clear();
}

void
AppInstance::clearFloatingWindows()
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->floatingWindows.clear();
}

void
AppInstance::clearSplitters()
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->splitters.clear();
}

void
AppInstance::registerPyPanel(PyPanelI* panel, const std::string& pythonFunction)
{
    panel->setPythonFunction(QString::fromUtf8(pythonFunction.c_str()));
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<PyPanelI*>::iterator found = std::find(_imp->pythonPanels.begin(), _imp->pythonPanels.end(), panel);

    if ( found == _imp->pythonPanels.end() ) {
        _imp->pythonPanels.push_back(panel);
    }
}

void
AppInstance::unregisterPyPanel(PyPanelI* panel)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<PyPanelI*>::iterator found = std::find(_imp->pythonPanels.begin(), _imp->pythonPanels.end(), panel);

    if ( found != _imp->pythonPanels.end() ) {
        _imp->pythonPanels.erase(found);
    }

}

void
AppInstance::registerSettingsPanel(DockablePanelI* panel, int index)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<DockablePanelI*>::iterator found = std::find(_imp->openedSettingsPanels.begin(), _imp->openedSettingsPanels.end(), panel);

    if ( found == _imp->openedSettingsPanels.end() ) {
        if (index == -1 || index >= (int)_imp->openedSettingsPanels.size()) {
            _imp->openedSettingsPanels.push_back(panel);
        } else {
            std::list<DockablePanelI*>::iterator it = _imp->openedSettingsPanels.begin();
            std::advance(it, index);
            _imp->openedSettingsPanels.insert(it, panel);
        }
    }
}

void
AppInstance::unregisterSettingsPanel(DockablePanelI* panel)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    std::list<DockablePanelI*>::iterator found = std::find(_imp->openedSettingsPanels.begin(), _imp->openedSettingsPanels.end(), panel);

    if ( found != _imp->openedSettingsPanels.end() ) {
        _imp->openedSettingsPanels.erase(found);
    }
}

void
AppInstance::clearSettingsPanels()
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->openedSettingsPanels.clear();
}

std::list<DockablePanelI*>
AppInstance::getOpenedSettingsPanels() const
{
    QMutexLocker k(&_imp->uiInfoMutex);
    return _imp->openedSettingsPanels;
}

void
AppInstance::setOpenedSettingsPanelsInternal(const std::list<DockablePanelI*>& panels)
{
    QMutexLocker k(&_imp->uiInfoMutex);
    _imp->openedSettingsPanels = panels;
}

QString
AppInstance::getAvailablePaneName(const QString & baseName) const
{
    std::string name = baseName.toStdString();
    QMutexLocker l(&_imp->uiInfoMutex);
    int baseNumber = _imp->tabWidgets.size();

    if ( name.empty() ) {
        name.append("pane");
        name.append( QString::number(baseNumber).toStdString() );
    }

    for (;;) {
        bool foundName = false;
        for (std::list<TabWidgetI*>::const_iterator it = _imp->tabWidgets.begin(); it != _imp->tabWidgets.end(); ++it) {
            if ( (*it)->getScriptName() == name ) {
                foundName = true;
                break;
            }
        }
        if (foundName) {
            ++baseNumber;
            name = QString::fromUtf8("pane%1").arg(baseNumber).toStdString();
        } else {
            break;
        }
    }

    return QString::fromUtf8(name.c_str());
}

void
AppInstance::saveApplicationWorkspace(SERIALIZATION_NAMESPACE::WorkspaceSerialization* serialization)
{

    // Main window
    SerializableWindow* mainWindow = getMainWindowSerialization();
    if (mainWindow) {
        serialization->_mainWindowSerialization.reset(new SERIALIZATION_NAMESPACE::WindowSerialization);
        mainWindow->toSerialization(serialization->_mainWindowSerialization.get());
    }

    // Floating windows
    std::list<SerializableWindow*> floatingWindows = getFloatingWindowsSerialization();
    for (std::list<SerializableWindow*>::iterator it = floatingWindows.begin(); it!=floatingWindows.end(); ++it) {
        boost::shared_ptr<SERIALIZATION_NAMESPACE::WindowSerialization> s(new SERIALIZATION_NAMESPACE::WindowSerialization);
        (*it)->toSerialization(s.get());
        serialization->_floatingWindowsSerialization.push_back(s);

    }

    // Save active python panels
    std::list<PyPanelI*> pythonPanels = getPyPanelsSerialization();
    for (std::list<PyPanelI*>::iterator it = pythonPanels.begin(); it != pythonPanels.end(); ++it) {
        SERIALIZATION_NAMESPACE::PythonPanelSerialization s;
        (*it)->toSerialization(&s);
        serialization->_pythonPanels.push_back(s);
    }

    // Save opened histograms
    getHistogramScriptNames(&serialization->_histograms);

}

NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_AppInstance.cpp"
